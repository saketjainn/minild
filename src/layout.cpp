#include <algorithm>

#include "minild.h"

namespace minild {

namespace {
constexpr uint32_t kSectionAlign = 0x1000;
constexpr uint32_t kFileAlign = 0x200;
uint32_t alignUp(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

enum Out { TEXT, RDATA, DATA, PDATA, IDATA, BSS, NOUT };
const char* kOutName[NOUT] = {".text", ".rdata", ".data", ".pdata", ".idata", ".bss"};
const uint32_t kOutFlags[NOUT] = {
    SCN_CNT_CODE | SCN_MEM_EXECUTE | SCN_MEM_READ,
    SCN_CNT_INITIALIZED_DATA | SCN_MEM_READ,
    SCN_CNT_INITIALIZED_DATA | SCN_MEM_READ | SCN_MEM_WRITE,
    SCN_CNT_INITIALIZED_DATA | SCN_MEM_READ,
    SCN_CNT_INITIALIZED_DATA | SCN_MEM_READ | SCN_MEM_WRITE,
    SCN_CNT_UNINITIALIZED_DATA | SCN_MEM_READ | SCN_MEM_WRITE};
}  // namespace

uint64_t sectionVA(const std::vector<ObjectFile>& objs, const Image& img, int o, int s) {
  const Section& sec = objs[o].sections[s];
  if (sec.discarded || sec.outSection < 0)
    throw LinkError("internal: address of dropped section " + sec.name + " in " + baseName(objs[o].path));
  return img.imageBase + img.sections[sec.outSection].rva + sec.outOffset;
}

uint64_t symbolVA(const std::vector<ObjectFile>& objs, const Resolution& res, const Image& img, int o, int i) {
  const Symbol& s = objs[o].symbols[i];
  if (s.section > 0) {
    const Section& sec = objs[o].sections[s.section - 1];
    if (!sec.discarded && sec.outSection >= 0) return sectionVA(objs, img, o, s.section - 1) + s.value;
    if (s.storageClass != CLASS_EXTERNAL)
      throw LinkError(baseName(objs[o].path) + ": symbol '" + s.name + "' lives in dropped section " + sec.name);
    // a duplicate COMDAT copy: fall through and use the copy that was kept
  } else if (s.section == SECTION_ABSOLUTE) {
    return s.value;
  }
  auto it = res.globals.find(s.name);
  if (it == res.globals.end()) throw LinkError("internal: symbol '" + s.name + "' was never resolved");
  const GlobalSymbol& g = it->second;
  switch (g.kind) {
    case DefKind::Regular:
    case DefKind::WeakDefault:
    case DefKind::Absolute:
      if (g.obj == o && g.index == i) throw LinkError("internal: symbol '" + s.name + "' resolves to itself");
      return symbolVA(objs, res, img, g.obj, g.index);
    case DefKind::Common:
      return img.imageBase + img.commonRva.at(s.name);
    case DefKind::Import:
      return img.imageBase + (g.viaThunk ? img.thunkRva.at(g.importName) : img.iatSlotRva.at(g.importName));
    default:
      throw LinkError("undefined symbol '" + s.name + "'");
  }
}

Image layout(std::vector<ObjectFile>& objs, Resolution& res, const LinkOptions& opts) {
  Image img;
  img.imageBase = opts.imageBase;
  std::vector<OutSection> outs(NOUT);
  for (int k = 0; k < NOUT; k++) {
    outs[k].name = kOutName[k];
    outs[k].flags = kOutFlags[k];
  }

  // ---- classify every input section
  struct Piece {
    int o, s;
    std::string group, suffix;
  };
  std::vector<Piece> pieces[NOUT];
  for (int o = 0; o < (int)objs.size(); o++) {
    for (int s = 0; s < (int)objs[o].sections.size(); s++) {
      Section& sec = objs[o].sections[s];
      const size_t dollar = sec.name.find('$');
      const std::string group = sec.name.substr(0, dollar);
      const std::string suffix = dollar == std::string::npos ? "" : sec.name.substr(dollar + 1);
      std::string why;
      if (sec.discarded) why = "duplicate COMDAT";
      else if (sec.flags & (SCN_LNK_REMOVE | SCN_LNK_INFO)) why = "linker directive / info";
      else if (group.rfind(".debug", 0) == 0) why = "debug info";
      if (!why.empty()) {
        sec.discarded = true;
        img.dropped.push_back(baseName(objs[o].path) + ":" + sec.name + " (" + why + ")");
        continue;
      }
      int k;
      if (group == ".pdata") k = PDATA;
      else if (group == ".xdata") k = RDATA;
      else if ((sec.flags & SCN_CNT_CODE) || group == ".text") k = TEXT;
      else if ((sec.flags & SCN_CNT_UNINITIALIZED_DATA) || group == ".bss") k = BSS;
      else if (sec.flags & SCN_MEM_WRITE) k = DATA;
      else k = RDATA;
      pieces[k].push_back({o, s, group, suffix});
    }
  }

  // ---- place pieces inside each output section
  std::vector<uint32_t> cur(NOUT, 0);
  for (int k = 0; k < NOUT; k++) {
    std::stable_sort(pieces[k].begin(), pieces[k].end(), [](const Piece& a, const Piece& b) {
      return a.group != b.group ? a.group < b.group : a.suffix < b.suffix;
    });
    for (const Piece& p : pieces[k]) {
      Section& sec = objs[p.o].sections[p.s];
      cur[k] = alignUp(cur[k], sec.align);
      sec.outSection = k;
      sec.outOffset = cur[k];
      if (k != BSS) {
        outs[k].data.resize(cur[k], k == TEXT ? 0x90 : 0);  // pad code with NOPs
        outs[k].data.insert(outs[k].data.end(), sec.data.begin(), sec.data.end());
      }
      cur[k] += sec.size;
      outs[k].pieces.push_back({p.o, p.s});
    }
  }

  // ---- jump stubs for DLL functions called directly: jmp qword ptr [rip + IAT slot]
  std::map<std::string, uint32_t> thunkOff;
  for (auto& [name, g] : res.globals) {
    if (g.kind != DefKind::Import || !g.viaThunk || thunkOff.count(g.importName)) continue;
    cur[TEXT] = alignUp(cur[TEXT], 8);
    outs[TEXT].data.resize(cur[TEXT], 0x90);
    thunkOff[g.importName] = cur[TEXT];
    const uint8_t stub[8] = {0xFF, 0x25, 0, 0, 0, 0, 0xCC, 0xCC};
    outs[TEXT].data.insert(outs[TEXT].data.end(), stub, stub + 8);
    cur[TEXT] += 8;
  }

  // ---- COMMON symbols go at the end of .bss
  std::map<std::string, uint32_t> commonOff;
  for (auto& [name, g] : res.globals) {
    if (g.kind != DefKind::Common) continue;
    uint32_t a = 1;
    while (a < g.commonSize && a < 16) a <<= 1;
    cur[BSS] = alignUp(cur[BSS], a);
    commonOff[name] = cur[BSS];
    cur[BSS] += g.commonSize;
  }

  // ---- import table (size first; contents once its RVA is known)
  if (!res.imports.empty()) {
    outs[IDATA].data.assign(buildImportTable(res.imports, 0).bytes.size(), 0);
    cur[IDATA] = (uint32_t)outs[IDATA].data.size();
  }

  // ---- keep non-empty output sections and give them addresses
  std::vector<int> newIndex(NOUT, -1);
  for (int k = 0; k < NOUT; k++) {
    outs[k].virtualSize = k == BSS ? cur[k] : (uint32_t)outs[k].data.size();
    if (outs[k].virtualSize == 0) continue;
    newIndex[k] = (int)img.sections.size();
    img.sections.push_back(std::move(outs[k]));
  }
  const uint32_t headerBytes = 0x80 + 4 + 20 + 240 + 40 * (uint32_t)img.sections.size();
  img.sizeOfHeaders = alignUp(headerBytes, kFileAlign);
  uint32_t rva = alignUp(img.sizeOfHeaders, kSectionAlign), fileOff = img.sizeOfHeaders;
  for (OutSection& s : img.sections) {
    s.rva = rva;
    const bool bss = s.flags & SCN_CNT_UNINITIALIZED_DATA;
    s.rawSize = bss ? 0 : alignUp((uint32_t)s.data.size(), kFileAlign);
    s.fileOffset = bss ? 0 : fileOff;
    if (!bss) s.data.resize(s.rawSize, s.flags & SCN_CNT_CODE ? 0xCC : 0);
    fileOff += s.rawSize;
    rva = alignUp(rva + s.virtualSize, kSectionAlign);
  }
  img.sizeOfImage = rva;

  // remap input sections to the final output-section indices
  for (auto& obj : objs)
    for (auto& sec : obj.sections)
      if (sec.outSection >= 0) sec.outSection = newIndex[sec.outSection];

  auto rvaOf = [&](int k) { return img.sections[newIndex[k]].rva; };
  for (auto& [f, off] : thunkOff) img.thunkRva[f] = rvaOf(TEXT) + off;
  for (auto& [n, off] : commonOff) img.commonRva[n] = rvaOf(BSS) + off;

  if (newIndex[IDATA] >= 0) {
    OutSection& id = img.sections[newIndex[IDATA]];
    ImportTable t = buildImportTable(res.imports, id.rva);
    std::copy(t.bytes.begin(), t.bytes.end(), id.data.begin());
    img.importDirRva = id.rva + t.dirOffset;
    img.importDirSize = t.dirSize;
    img.iatRva = id.rva + t.iatOffset;
    img.iatSize = t.iatSize;
    for (auto& [f, off] : t.slotOffset) img.iatSlotRva[f] = id.rva + off;
  }
  // patch each stub's rel32 so it jumps through its IAT slot
  if (!thunkOff.empty()) {
    OutSection& text = img.sections[newIndex[TEXT]];
    for (auto& [f, off] : thunkOff) {
      const int32_t disp = (int32_t)((int64_t)img.iatSlotRva.at(f) - (int64_t)(text.rva + off + 6));
      for (int b = 0; b < 4; b++) text.data[off + 2 + b] = (uint8_t)(disp >> (8 * b));
    }
  }
  if (newIndex[PDATA] >= 0) {
    img.exceptionRva = rvaOf(PDATA);
    img.exceptionSize = img.sections[newIndex[PDATA]].virtualSize;
  }

  const GlobalSymbol& entry = res.globals.at(opts.entry);
  img.entryRva = (uint32_t)(symbolVA(objs, res, img, entry.obj, entry.index) - img.imageBase);
  return img;
}

}  // namespace minild
