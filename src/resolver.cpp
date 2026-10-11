#include <algorithm>
#include <iomanip>

#include "minild.h"

namespace minild {
const char* knownImportDll(const std::string& f) {
  static const std::map<std::string, const char*> known = {
      {"ExitProcess", "kernel32.dll"},     {"GetStdHandle", "kernel32.dll"},
      {"WriteFile", "kernel32.dll"},       {"ReadFile", "kernel32.dll"},
      {"WriteConsoleA", "kernel32.dll"},   {"GetTickCount", "kernel32.dll"},
      {"GetTickCount64", "kernel32.dll"},  {"Sleep", "kernel32.dll"},
      {"GetLastError", "kernel32.dll"},    {"GetProcessHeap", "kernel32.dll"},
      {"HeapAlloc", "kernel32.dll"},       {"HeapFree", "kernel32.dll"},
      {"GetCommandLineA", "kernel32.dll"}, {"GetCurrentProcessId", "kernel32.dll"},
      {"CreateFileA", "kernel32.dll"},     {"CloseHandle", "kernel32.dll"},
      {"GetModuleHandleA", "kernel32.dll"}, {"QueryPerformanceCounter", "kernel32.dll"},
      {"QueryPerformanceFrequency", "kernel32.dll"}, {"MessageBoxA", "user32.dll"},
  };
  auto it = known.find(f);
  return it == known.end() ? nullptr : it->second;
}

Resolution resolveSymbols(std::vector<ObjectFile>& objs, const LinkOptions& opts) {
  Resolution res;
  auto& G = res.globals;
  auto file = [&](int o) { return baseName(objs[o].path); };
  struct WeakRef { int obj, index; };
  std::map<std::string, std::vector<WeakRef>> weakRefs;

  for (int o = 0; o < (int)objs.size(); o++) {
    ObjectFile& obj = objs[o];
    for (int i = 0; i < (int)obj.symbols.size(); i++) {
      const Symbol& s = obj.symbols[i];
      if (s.isAux) continue;
      if (s.storageClass == CLASS_WEAK_EXTERNAL) {
        weakRefs[s.name].push_back({o, i});
        G[s.name].name = s.name;
        continue;
      }
      if (s.storageClass != CLASS_EXTERNAL) continue;
      GlobalSymbol& g = G[s.name];
      g.name = s.name;

      if (s.section > 0 || s.section == SECTION_ABSOLUTE) {  
        const bool absolute = s.section == SECTION_ABSOLUTE;
        if (g.kind == DefKind::Regular || g.kind == DefKind::Absolute) {
          const Section* mine = absolute ? nullptr : &obj.sections[s.section - 1];
          const Symbol& first = objs[g.obj].symbols[g.index];
          const Section* theirs = first.section > 0 ? &objs[g.obj].sections[first.section - 1] : nullptr;
          if (mine && theirs && (mine->flags & SCN_LNK_COMDAT) && (theirs->flags & SCN_LNK_COMDAT)) {
            obj.sections[s.section - 1].discarded = true;  
            res.notes.push_back("COMDAT '" + s.name + "': using the copy from " + file(g.obj) + ", dropping the one in " + file(o));
            continue;
          }
          res.errors.push_back("duplicate symbol '" + s.name + "': defined in " + file(g.obj) + " and " + file(o));
          continue;
        }
        g.kind = absolute ? DefKind::Absolute : DefKind::Regular;
        g.obj = o;
        g.index = i;
      } else if (s.value > 0) { 
        if (g.kind == DefKind::Undefined || g.kind == DefKind::Common) {
          g.kind = DefKind::Common;
          g.commonSize = std::max(g.commonSize, s.value);
          if (g.obj < 0) g.obj = o;
        }
      } else {
        g.referencedBy.insert(o);
      }
    }
  }

  for (int o = 0; o < (int)objs.size(); o++)
    for (const Section& sec : objs[o].sections)
      for (const Reloc& r : sec.relocs) {
        const Symbol& s = objs[o].symbols[r.symIndex];
        if ((s.storageClass == CLASS_EXTERNAL || s.storageClass == CLASS_WEAK_EXTERNAL) && s.section == SECTION_UNDEFINED)
          G[s.name].referencedBy.insert(o);
      }

  for (auto& [name, refs] : weakRefs) {
    GlobalSymbol& g = G[name];
    if (g.kind != DefKind::Undefined) continue;
    const WeakRef& w = refs.front();
    const Symbol& weak = objs[w.obj].symbols[w.index];
    if (weak.weakTag >= objs[w.obj].symbols.size()) {
      res.errors.push_back("weak symbol '" + name + "' in " + file(w.obj) + " has a bad default index");
      continue;
    }
    g.kind = DefKind::WeakDefault;
    g.obj = w.obj;
    g.index = (int)weak.weakTag;
    res.notes.push_back("weak symbol '" + name + "': no strong definition, using the default from " + file(w.obj));
  }

  std::map<std::string, std::string> extra;
  for (auto& [dll, fn] : opts.extraImports) extra[fn] = dll;
  auto dllFor = [&](const std::string& fn) -> std::string {
    auto it = extra.find(fn);
    if (it != extra.end()) return it->second;
    const char* d = knownImportDll(fn);
    return d ? d : "";
  };
  auto addImport = [&](const std::string& dll, const std::string& fn) {
    auto& list = res.imports[dll];
    if (std::find(list.begin(), list.end(), fn) == list.end()) list.push_back(fn);
  };
  for (auto& [name, g] : G) {
    if (g.kind != DefKind::Undefined) continue;
    if (g.referencedBy.empty()) continue;
    const bool viaImp = name.rfind("__imp_", 0) == 0;
    const std::string fn = viaImp ? name.substr(6) : name;
    const std::string dll = dllFor(fn);
    if (!dll.empty()) {
      g.kind = DefKind::Import;
      g.dll = dll;
      g.importName = fn;
      g.viaThunk = !viaImp;
      addImport(dll, fn);
      continue;
    }
    std::string users;
    for (int o : g.referencedBy) users += (users.empty() ? "" : ", ") + file(o);
    std::string msg = "undefined symbol '" + name + "' (referenced in " + users + ")";
    if (viaImp) msg += "; if it is a DLL function, add it with --import DLLNAME:" + fn;
    res.errors.push_back(msg);
  }

  auto e = G.find(opts.entry);
  if (e == G.end() || (e->second.kind != DefKind::Regular && e->second.kind != DefKind::WeakDefault))
    res.errors.push_back("entry symbol '" + opts.entry + "' is not defined (use -e NAME to choose another)");
  return res;
}

void printResolution(const std::vector<ObjectFile>& objs, const Resolution& res, std::ostream& out) {
  auto file = [&](int o) { return baseName(objs[o].path); };
  out << std::left << "  " << std::setw(26) << "Symbol" << std::setw(16) << "Defined in" << std::setw(14) << "Kind"
      << "Used by\n";
  for (auto& [name, g] : res.globals) {
    std::string where = "-", kind = "-";
    switch (g.kind) {
      case DefKind::Regular: where = file(g.obj); kind = "EXTERNAL"; break;
      case DefKind::Absolute: where = file(g.obj); kind = "ABSOLUTE"; break;
      case DefKind::Common: where = "(.bss)"; kind = "COMMON " + std::to_string(g.commonSize); break;
      case DefKind::WeakDefault: where = file(g.obj); kind = "WEAK default"; break;
      case DefKind::Import: where = g.dll; kind = g.viaThunk ? "IMPORT stub" : "IMPORT"; break;
      case DefKind::Undefined: where = "-"; kind = "UNDEFINED"; break;
    }
    std::string users;
    for (int o : g.referencedBy) users += (users.empty() ? "" : ", ") + file(o);
    out << "  " << pad(name, 26) << pad(where, 16) << pad(kind, 14) << (users.empty() ? "-" : users) << "\n";
  }
  out << std::right;
  if (!res.imports.empty()) {
    out << "\n  Imports:\n";
    for (auto& [dll, fns] : res.imports) {
      out << "    " << dll << ":";
      for (auto& f : fns) out << " " << f;
      out << "\n";
    }
  }
  for (auto& n : res.notes) out << "  note: " << n << "\n";
}

} 
