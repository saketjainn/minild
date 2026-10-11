#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "minild.h"

namespace minild {

namespace {
class Buf {
 public:
  std::vector<uint8_t> b;
  void u8(uint8_t v) { b.push_back(v); }
  void u16(uint16_t v) { for (int i = 0; i < 2; i++) b.push_back((uint8_t)(v >> (8 * i))); }
  void u32(uint32_t v) { for (int i = 0; i < 4; i++) b.push_back((uint8_t)(v >> (8 * i))); }
  void u64(uint64_t v) { for (int i = 0; i < 8; i++) b.push_back((uint8_t)(v >> (8 * i))); }
  void bytes(const void* p, size_t n) { b.insert(b.end(), (const uint8_t*)p, (const uint8_t*)p + n); }
  void padTo(size_t n) { if (b.size() < n) b.resize(n, 0); }
};
constexpr uint32_t kPeOffset = 0x80;
} 

void writePE(const Image& img, const std::string& path) {
  Buf f;
  f.bytes("MZ", 2);
  f.padTo(0x3C);
  f.u32(kPeOffset); 
  static const uint8_t stub[] = {0x0E, 0x1F, 0xBA, 0x0E, 0x00, 0xB4, 0x09, 0xCD, 0x21, 0xB8, 0x01, 0x4C, 0xCD, 0x21};
  f.bytes(stub, sizeof stub);
  const char* msg = "This program cannot be run in DOS mode.\r\r\n$";
  f.bytes(msg, std::strlen(msg));
  f.padTo(kPeOffset);

  f.bytes("PE\0\0", 4);
  f.u16(MACHINE_AMD64);
  f.u16((uint16_t)img.sections.size());
  f.u32(0);    
  f.u32(0);    
  f.u32(0); 
  f.u16(240);
  f.u16(0x0001 | 0x0002 | 0x0020);

  uint32_t sizeOfCode = 0, sizeOfInit = 0, sizeOfUninit = 0, baseOfCode = 0;
  for (auto& s : img.sections) {
    if (s.flags & SCN_CNT_CODE) { sizeOfCode += s.rawSize; if (!baseOfCode) baseOfCode = s.rva; }
    else if (s.flags & SCN_CNT_UNINITIALIZED_DATA) sizeOfUninit += s.virtualSize;
    else sizeOfInit += s.rawSize;
  }

  // ---- PE32+ optional header
  f.u16(0x20B);
  f.u8(0);
  f.u8(1);
  f.u32(sizeOfCode);
  f.u32(sizeOfInit);
  f.u32(sizeOfUninit);
  f.u32(img.entryRva);
  f.u32(baseOfCode);
  f.u64(img.imageBase);
  f.u32(0x1000);  // SectionAlignment
  f.u32(0x200);   // FileAlignment
  f.u16(6); f.u16(0);  // OperatingSystemVersion 6.0
  f.u16(0); f.u16(0);  // ImageVersion
  f.u16(6); f.u16(0);  // SubsystemVersion 6.0
  f.u32(0);       // Win32VersionValue
  f.u32(img.sizeOfImage);
  f.u32(img.sizeOfHeaders);
  f.u32(0);       // CheckSum (not required for executables)
  f.u16(3);       // Subsystem: Windows console
  f.u16(0x0100 | 0x8000);  // DllCharacteristics: NX_COMPAT | TERMINAL_SERVER_AWARE
  f.u64(0x100000); f.u64(0x1000);  // stack reserve / commit
  f.u64(0x100000); f.u64(0x1000);  // heap reserve / commit
  f.u32(0);       // LoaderFlags
  f.u32(16);      // NumberOfRvaAndSizes
  for (int d = 0; d < 16; d++) {
    uint32_t rva = 0, size = 0;
    if (d == 1) { rva = img.importDirRva; size = img.importDirSize; }   // Import Directory
    if (d == 3) { rva = img.exceptionRva; size = img.exceptionSize; }   // Exception Directory (.pdata)
    if (d == 12) { rva = img.iatRva; size = img.iatSize; }              // Import Address Table
    f.u32(rva);
    f.u32(size);
  }

  // ---- section headers
  for (auto& s : img.sections) {
    char name[8] = {0};
    std::memcpy(name, s.name.data(), std::min<size_t>(8, s.name.size()));
    f.bytes(name, 8);
    f.u32(s.virtualSize);
    f.u32(s.rva);
    f.u32(s.rawSize);
    f.u32(s.fileOffset);
    f.u32(0); f.u32(0);  // relocations / line numbers (none in an image)
    f.u16(0); f.u16(0);
    f.u32(s.flags);
  }
  f.padTo(img.sizeOfHeaders);

  // ---- section contents
  for (auto& s : img.sections) {
    if (!s.rawSize) continue;
    f.padTo(s.fileOffset);
    f.bytes(s.data.data(), s.rawSize);
  }

  std::ofstream out(path, std::ios::binary);
  if (!out) throw LinkError("cannot write '" + path + "'");
  out.write((const char*)f.b.data(), (std::streamsize)f.b.size());
  if (!out) throw LinkError("error while writing '" + path + "'");
}

void printMap(const std::vector<ObjectFile>& objs, const Resolution& res, const Image& img, std::ostream& out) {
  auto hx = [](uint64_t v, int w = 0) {
    std::ostringstream s;
    s << "0x" << std::hex << std::setw(w) << std::setfill('0') << v;
    return s.str();
  };
  out << "Link map\n========\nImage base " << hx(img.imageBase) << ", entry point " << hx(img.imageBase + img.entryRva)
      << ", size of image " << hx(img.sizeOfImage) << "\n\n";
  out << "Output sections\n";
  for (auto& s : img.sections) {
    out << "  " << std::left << std::setw(8) << s.name << std::right << " VA " << hx(img.imageBase + s.rva, 9) << "  size "
        << hx(s.virtualSize) << "  file offset " << hx(s.fileOffset) << "\n";
    for (auto& [o, si] : s.pieces) {
      const Section& sec = objs[o].sections[si];
      out << "      " << hx(sectionVA(objs, img, o, si), 9) << "  " << pad(baseName(objs[o].path), 14) << pad(sec.name, 24)
          << "size " << hx(sec.size) << "\n";
    }
  }

  out << "\nGlobal symbols\n";
  std::vector<std::pair<uint64_t, std::string>> syms;
  for (auto& [name, g] : res.globals) {
    if (g.kind == DefKind::Regular || g.kind == DefKind::WeakDefault)
      syms.push_back({symbolVA(objs, res, img, g.obj, g.index), name + "  (" + baseName(objs[g.obj].path) + ")"});
    else if (g.kind == DefKind::Common)
      syms.push_back({img.imageBase + img.commonRva.at(name), name + "  (COMMON " + std::to_string(g.commonSize) + " bytes)"});
  }
  std::sort(syms.begin(), syms.end());
  for (auto& [va, n] : syms) out << "  " << hx(va, 9) << "  " << n << "\n";

  if (!res.imports.empty()) {
    out << "\nImports (IAT slots filled by the Windows loader)\n";
    for (auto& [dll, fns] : res.imports)
      for (auto& fn : fns) {
        out << "  " << hx(img.imageBase + img.iatSlotRva.at(fn), 9) << "  " << dll << "!" << fn;
        if (img.thunkRva.count(fn)) out << "   stub at " << hx(img.imageBase + img.thunkRva.at(fn));
        out << "\n";
      }
  }
  if (!img.dropped.empty()) {
    out << "\nDropped sections\n";
    for (auto& d : img.dropped) out << "  " << d << "\n";
  }
}

}
