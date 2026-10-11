#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

#include "minild.h"

namespace minild {

std::vector<uint8_t> readFileBytes(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw LinkError("cannot open '" + path + "'");
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

namespace {

class ByteReader {
 public:
  ByteReader(const std::vector<uint8_t>& b, const std::string& path) : b_(b), path_(path) {}
  void need(size_t off, size_t n, const char* what) const {
    if (off + n > b_.size() || off + n < off)
      throw LinkError(path_ + ": truncated file (" + what + " runs past the end of the file)");
  }
  uint8_t u8(size_t off) const { need(off, 1, "byte"); return b_[off]; }
  uint16_t u16(size_t off) const { need(off, 2, "field"); return (uint16_t)(b_[off] | b_[off + 1] << 8); }
  uint32_t u32(size_t off) const {
    need(off, 4, "field");
    return (uint32_t)b_[off] | (uint32_t)b_[off + 1] << 8 | (uint32_t)b_[off + 2] << 16 | (uint32_t)b_[off + 3] << 24;
  }
  // A string of at most `max` bytes that may not be NUL-terminated.
  std::string fixedString(size_t off, size_t max) const {
    need(off, max, "name");
    size_t n = 0;
    while (n < max && b_[off + n]) n++;
    return std::string((const char*)&b_[off], n);
  }

 private:
  const std::vector<uint8_t>& b_;
  const std::string& path_;
};

}  // namespace

ObjectFile readObject(const std::string& path) {
  std::vector<uint8_t> b = readFileBytes(path);
  ByteReader r(b, path);

  // Friendly errors for files that are not COFF objects.
  if (b.size() >= 2 && b[0] == 'M' && b[1] == 'Z')
    throw LinkError(path + ": this is a PE executable or DLL, not an object file");
  if (b.size() >= 4 && b[0] == 0x7f && b[1] == 'E' && b[2] == 'L' && b[3] == 'F')
    throw LinkError(path + ": this is a Linux ELF file; minild reads Windows COFF objects");
  if (b.size() >= 8 && std::memcmp(b.data(), "!<arch>\n", 8) == 0)
    throw LinkError(path + ": this is a static library (.a); archives are not supported yet");
  if (b.size() < 20) throw LinkError(path + ": file is too small to be a COFF object");

  ObjectFile obj;
  obj.path = path;
  obj.machine = r.u16(0);
  if (obj.machine == MACHINE_I386)
    throw LinkError(path + ": 32-bit x86 object; minild supports x86-64 only (use 64-bit MinGW)");
  if (obj.machine != MACHINE_AMD64) {
    char buf[64];
    snprintf(buf, sizeof buf, "0x%04x", obj.machine);
    throw LinkError(path + ": not a COFF object for x86-64 (machine field is " + buf + ")");
  }
  const uint16_t numSections = r.u16(2);
  const uint32_t symTableOff = r.u32(8);
  const uint32_t numSymbols = r.u32(12);
  const uint16_t optHeaderSize = r.u16(16);
  if (optHeaderSize != 0) throw LinkError(path + ": has an optional header, so it is an image, not an object file");

  // String table: right after the symbol table; starts with its own 4-byte size.
  const size_t strTableOff = symTableOff + (size_t)numSymbols * 18;
  uint32_t strTableSize = 0;
  if (numSymbols && strTableOff + 4 <= b.size()) strTableSize = r.u32(strTableOff);
  auto stringAt = [&](uint32_t off) -> std::string {
    if (off < 4 || off >= strTableSize) throw LinkError(path + ": name offset " + std::to_string(off) + " is outside the string table");
    size_t start = strTableOff + off, end = start;
    r.need(start, 1, "string table");
    while (end < strTableOff + strTableSize && end < b.size() && b[end]) end++;
    return std::string((const char*)&b[start], end - start);
  };

  // ---- section headers
  for (uint16_t k = 0; k < numSections; k++) {
    const size_t h = 20 + optHeaderSize + (size_t)k * 40;
    r.need(h, 40, "section header");
    Section s;
    s.name = r.fixedString(h, 8);
    if (!s.name.empty() && s.name[0] == '/') s.name = stringAt((uint32_t)std::stoul(s.name.substr(1)));
    s.size = r.u32(h + 16);
    const uint32_t rawPtr = r.u32(h + 20);
    const uint32_t relPtr = r.u32(h + 24);
    uint32_t numRelocs = r.u16(h + 32);
    s.flags = r.u32(h + 36);
    const uint32_t alignCode = (s.flags & SCN_ALIGN_MASK) >> 20;
    s.align = alignCode ? (1u << (alignCode - 1)) : 16;

    if (!s.isBss() && rawPtr != 0 && s.size) {
      r.need(rawPtr, s.size, "section data");
      s.data.assign(b.begin() + rawPtr, b.begin() + rawPtr + s.size);
    } else if (!s.isBss()) {
      s.data.assign(s.size, 0);
    }

    size_t relOff = relPtr;
    if ((s.flags & SCN_LNK_NRELOC_OVFL) && numRelocs == 0xFFFF) {
      numRelocs = r.u32(relPtr);  // real count is stored in the first entry
      relOff += 10;
      numRelocs -= 1;
    }
    for (uint32_t j = 0; j < numRelocs; j++) {
      const size_t o = relOff + (size_t)j * 10;
      s.relocs.push_back(Reloc{r.u32(o), r.u32(o + 4), r.u16(o + 8)});
    }
    obj.sections.push_back(std::move(s));
  }

  // ---- symbol table (auxiliary records occupy their own slots)
  for (uint32_t i = 0; i < numSymbols;) {
    const size_t o = symTableOff + (size_t)i * 18;
    r.need(o, 18, "symbol table");
    Symbol s;
    s.name = r.u32(o) == 0 ? stringAt(r.u32(o + 4)) : r.fixedString(o, 8);
    s.value = r.u32(o + 8);
    s.section = (int16_t)r.u16(o + 12);
    s.type = r.u16(o + 14);
    s.storageClass = r.u8(o + 16);
    s.numAux = r.u8(o + 17);
    if (i + 1 + s.numAux > numSymbols) throw LinkError(path + ": symbol table entry " + std::to_string(i) + " has too many auxiliary records");

    if (s.numAux) {
      const size_t aux = o + 18;
      if (s.storageClass == CLASS_WEAK_EXTERNAL) s.weakTag = r.u32(aux);
      if (s.storageClass == CLASS_STATIC && s.section > 0 && s.value == 0) s.comdatSelect = r.u8(aux + 14);
      if (s.storageClass == CLASS_FILE) s.name = r.fixedString(aux, 18u * s.numAux);
    }
    obj.symbols.push_back(s);
    for (uint8_t a = 0; a < s.numAux; a++) {
      Symbol placeholder;
      placeholder.isAux = true;
      obj.symbols.push_back(placeholder);
    }
    i += 1 + s.numAux;
  }

  // ---- validate relocations
  for (auto& s : obj.sections) {
    for (auto& rel : s.relocs) {
      if (rel.symIndex >= obj.symbols.size() || obj.symbols[rel.symIndex].isAux)
        throw LinkError(path + ": relocation in " + s.name + " refers to invalid symbol index " + std::to_string(rel.symIndex));
      const uint32_t width = rel.type == REL_AMD64_ADDR64 ? 8 : 4;
      if (rel.type != REL_AMD64_ABSOLUTE && (uint64_t)rel.offset + width > s.size)
        throw LinkError(path + ": relocation at " + s.name + "+" + std::to_string(rel.offset) + " is outside the section");
    }
  }
  return obj;
}

}  // namespace minild
