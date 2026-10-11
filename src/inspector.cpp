#include <cstdio>
#include <iomanip>
#include <sstream>

#include "minild.h"

namespace minild {

std::string baseName(const std::string& path) {
  size_t p = path.find_last_of("/\\");
  return p == std::string::npos ? path : path.substr(p + 1);
}

std::string pad(const std::string& text, size_t width) {
  return text.size() < width ? text + std::string(width - text.size(), ' ') : text + " ";
}

std::string relocTypeName(uint16_t type) {
  switch (type) {
    case 0x0: return "IMAGE_REL_AMD64_ABSOLUTE";
    case 0x1: return "IMAGE_REL_AMD64_ADDR64";
    case 0x2: return "IMAGE_REL_AMD64_ADDR32";
    case 0x3: return "IMAGE_REL_AMD64_ADDR32NB";
    case 0x4: return "IMAGE_REL_AMD64_REL32";
    case 0x5: case 0x6: case 0x7: case 0x8: case 0x9:
      return "IMAGE_REL_AMD64_REL32_" + std::to_string(type - 4);
    case 0xA: return "IMAGE_REL_AMD64_SECTION";
    case 0xB: return "IMAGE_REL_AMD64_SECREL";
    default: {
      char buf[32];
      snprintf(buf, sizeof buf, "UNKNOWN(0x%x)", type);
      return buf;
    }
  }
}

std::string storageClassName(uint8_t cls) {
  switch (cls) {
    case CLASS_EXTERNAL: return "EXTERNAL";
    case CLASS_STATIC: return "STATIC";
    case CLASS_LABEL: return "LABEL";
    case CLASS_FILE: return "FILE";
    case CLASS_SECTION: return "SECTION";
    case CLASS_WEAK_EXTERNAL: return "WEAK_EXTERNAL";
    default: return "CLASS_" + std::to_string(cls);
  }
}

std::string sectionFlagsString(uint32_t f) {
  std::string s;
  auto add = [&](uint32_t bit, const char* name) {
    if (f & bit) s += (s.empty() ? "" : " ") + std::string(name);
  };
  add(SCN_CNT_CODE, "CODE");
  add(SCN_CNT_INITIALIZED_DATA, "DATA");
  add(SCN_CNT_UNINITIALIZED_DATA, "BSS");
  add(SCN_LNK_INFO, "INFO");
  add(SCN_LNK_REMOVE, "REMOVE");
  add(SCN_LNK_COMDAT, "COMDAT");
  add(SCN_MEM_DISCARDABLE, "DISCARDABLE");
  add(SCN_MEM_EXECUTE, "EXECUTE");
  add(SCN_MEM_READ, "READ");
  add(SCN_MEM_WRITE, "WRITE");
  return s;
}

static std::string hex(uint64_t v) {
  std::ostringstream s;
  s << "0x" << std::hex << v;
  return s.str();
}

static std::string sectionLabel(const ObjectFile& obj, int16_t sec) {
  if (sec == SECTION_UNDEFINED) return "UNDEF";
  if (sec == SECTION_ABSOLUTE) return "ABS";
  if (sec == SECTION_DEBUG) return "DEBUG";
  if (sec > 0 && (size_t)sec <= obj.sections.size()) return obj.sections[sec - 1].name;
  return "?" + std::to_string(sec);
}

void inspect(const ObjectFile& obj, std::ostream& out) {
  size_t realSymbols = 0;
  for (auto& s : obj.symbols) if (!s.isAux) realSymbols++;
  out << "File: " << obj.path << "\n"
      << "  COFF object, machine x86-64 (0x8664), " << obj.sections.size() << " sections, " << realSymbols
      << " symbols (" << obj.symbols.size() << " symbol-table slots including auxiliary records)\n\n";

  out << "Sections\n";
  out << "  #  " << std::left << std::setw(16) << "Name" << std::setw(10) << "Size" << std::setw(7) << "Align"
      << std::setw(8) << "Relocs" << "Flags\n";
  for (size_t i = 0; i < obj.sections.size(); i++) {
    const Section& s = obj.sections[i];
    out << "  " << pad(std::to_string(i + 1), 3) << pad(s.name, 16) << pad(hex(s.size), 10) << pad(std::to_string(s.align), 7)
        << pad(std::to_string(s.relocs.size()), 8) << sectionFlagsString(s.flags) << "\n";
  }

  out << "\nSymbols\n";
  out << "  " << std::setw(7) << "Index" << std::setw(12) << "Value" << std::setw(16) << "Section" << std::setw(15)
      << "Class" << std::setw(10) << "Kind" << std::setw(11) << "Status" << "Name\n";
  std::vector<std::string> definedGlobals, undefined, weak;
  for (size_t i = 0; i < obj.symbols.size(); i++) {
    const Symbol& s = obj.symbols[i];
    if (s.isAux) continue;
    std::string kind = s.storageClass == CLASS_FILE ? "file"
                     : s.isFunction() ? "function"
                     : (s.storageClass == CLASS_STATIC && s.value == 0 && s.section > 0 && s.numAux) ? "section"
                     : "data";
    std::string status = "-";
    if (s.storageClass == CLASS_EXTERNAL) {
      if (s.section == SECTION_UNDEFINED && s.value) { status = "common"; definedGlobals.push_back(s.name + " (common, " + std::to_string(s.value) + " bytes)"); }
      else if (s.section == SECTION_UNDEFINED) { status = "undefined"; undefined.push_back(s.name); }
      else { status = "defined"; definedGlobals.push_back(s.name); }
    } else if (s.storageClass == CLASS_WEAK_EXTERNAL) {
      status = "weak";
      std::string def = s.weakTag < obj.symbols.size() ? obj.symbols[s.weakTag].name : "?";
      weak.push_back(s.name + " (default: " + def + ")");
    } else if (s.storageClass == CLASS_STATIC) {
      status = "local";
    }
    std::ostringstream idx;
    idx << "[" << i << "]";
    out << "  " << pad(idx.str(), 7) << pad(hex(s.value), 12) << pad(sectionLabel(obj, s.section), 16)
        << pad(storageClassName(s.storageClass), 15) << pad(kind, 10) << pad(status, 11) << s.name << "\n";
  }

  for (const Section& s : obj.sections) {
    if (s.relocs.empty()) continue;
    out << "\nRelocations in " << s.name << "\n";
    out << "  " << std::setw(10) << "Offset" << std::setw(28) << "Type" << std::setw(12) << "Addend" << "Symbol\n";
    for (const Reloc& r : s.relocs) {
      // COFF stores the addend inside the bytes being patched ("implicit addend")
      int64_t addend = 0;
      if (r.type == REL_AMD64_ADDR64) {
        for (int k = 7; k >= 0; k--) addend = (addend << 8) | s.data[r.offset + k];
      } else if (r.type != REL_AMD64_ABSOLUTE) {
        uint32_t v = 0;
        for (int k = 3; k >= 0; k--) v = (v << 8) | s.data[r.offset + k];
        addend = (int32_t)v;
      }
      out << "  " << pad(hex(r.offset), 10) << pad(relocTypeName(r.type), 28) << pad(std::to_string(addend), 12)
          << obj.symbols[r.symIndex].name << "\n";
    }
  }

  auto list = [&](const char* title, const std::vector<std::string>& v) {
    out << "  " << title << " (" << v.size() << "): ";
    for (size_t i = 0; i < v.size(); i++) out << (i ? ", " : "") << v[i];
    out << "\n";
  };
  out << "\nSummary\n";
  list("Defined globals", definedGlobals);
  list("Undefined (must come from another file or a DLL)", undefined);
  if (!weak.empty()) list("Weak", weak);
  out << std::right;
}

}  // namespace minild
