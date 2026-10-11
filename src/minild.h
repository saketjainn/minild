#pragma once

#include <cstdint>
#include <map>
#include <ostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace minild {

struct LinkError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

constexpr uint16_t MACHINE_AMD64 = 0x8664;
constexpr uint16_t MACHINE_I386 = 0x014c;

// Symbol storage classes
constexpr uint8_t CLASS_EXTERNAL = 2;
constexpr uint8_t CLASS_STATIC = 3;
constexpr uint8_t CLASS_LABEL = 6;
constexpr uint8_t CLASS_FILE = 103;
constexpr uint8_t CLASS_SECTION = 104;
constexpr uint8_t CLASS_WEAK_EXTERNAL = 105;

// Special section numbers in symbols
constexpr int16_t SECTION_UNDEFINED = 0;
constexpr int16_t SECTION_ABSOLUTE = -1;
constexpr int16_t SECTION_DEBUG = -2;

// Section characteristics
constexpr uint32_t SCN_CNT_CODE = 0x00000020;
constexpr uint32_t SCN_CNT_INITIALIZED_DATA = 0x00000040;
constexpr uint32_t SCN_CNT_UNINITIALIZED_DATA = 0x00000080;
constexpr uint32_t SCN_LNK_INFO = 0x00000200;
constexpr uint32_t SCN_LNK_REMOVE = 0x00000800;
constexpr uint32_t SCN_LNK_COMDAT = 0x00001000;
constexpr uint32_t SCN_ALIGN_MASK = 0x00F00000;
constexpr uint32_t SCN_LNK_NRELOC_OVFL = 0x01000000;
constexpr uint32_t SCN_MEM_DISCARDABLE = 0x02000000;
constexpr uint32_t SCN_MEM_EXECUTE = 0x20000000;
constexpr uint32_t SCN_MEM_READ = 0x40000000;
constexpr uint32_t SCN_MEM_WRITE = 0x80000000;

// x64 relocation types
constexpr uint16_t REL_AMD64_ABSOLUTE = 0x0;
constexpr uint16_t REL_AMD64_ADDR64 = 0x1;
constexpr uint16_t REL_AMD64_ADDR32 = 0x2;
constexpr uint16_t REL_AMD64_ADDR32NB = 0x3;
constexpr uint16_t REL_AMD64_REL32 = 0x4;  // REL32_1..REL32_5 are 0x5..0x9
constexpr uint16_t REL_AMD64_REL32_5 = 0x9;
constexpr uint16_t REL_AMD64_SECTION = 0xA;
constexpr uint16_t REL_AMD64_SECREL = 0xB;

struct Reloc {
  uint32_t offset;    // where to patch, relative to the start of the section
  uint32_t symIndex;  // index into ObjectFile::symbols
  uint16_t type;
};

struct Section {
  std::string name;
  uint32_t flags = 0;
  uint32_t size = 0;           // bytes (for .bss: size in memory)
  std::vector<uint8_t> data;   // empty for uninitialised data
  std::vector<Reloc> relocs;
  uint32_t align = 16;
  bool discarded = false;      // dropped (debug info, duplicate COMDAT, ...)
  // set by layout
  int outSection = -1;
  uint32_t outOffset = 0;      // offset inside the output section

  bool isBss() const { return flags & SCN_CNT_UNINITIALIZED_DATA; }
};

struct Symbol {
  std::string name;
  uint32_t value = 0;
  int16_t section = 0;  // 1-based section number, or 0 / -1 / -2
  uint16_t type = 0;
  uint8_t storageClass = 0;
  uint8_t numAux = 0;
  bool isAux = false;     // this slot holds an auxiliary record, not a symbol
  uint32_t weakTag = 0;   // WEAK_EXTERNAL: index of the default symbol
  uint8_t comdatSelect = 0;

  bool isFunction() const { return (type >> 4) == 2; }
  bool isUndefined() const { return section == SECTION_UNDEFINED; }
};

struct ObjectFile {
  std::string path;
  uint16_t machine = 0;
  std::vector<Section> sections;  // sections[i] is section number i + 1
  std::vector<Symbol> symbols;    // indexed exactly like the COFF symbol table
};

std::vector<uint8_t> readFileBytes(const std::string& path);
ObjectFile readObject(const std::string& path);

void inspect(const ObjectFile& obj, std::ostream& out);
std::string relocTypeName(uint16_t type);
std::string storageClassName(uint8_t cls);
std::string sectionFlagsString(uint32_t flags);
std::string baseName(const std::string& path);
// Pad to `width` columns, always leaving at least one space after the text.
std::string pad(const std::string& text, size_t width);

struct LinkOptions {
  std::string entry = "start";
  std::string output = "a.exe";
  bool explain = false;
  bool map = false;
  uint64_t imageBase = 0x140000000ull;
  std::vector<std::pair<std::string, std::string>> extraImports;  // dll, function
};

enum class DefKind { Undefined, Regular, Common, Absolute, WeakDefault, Import };

struct GlobalSymbol {
  std::string name;
  DefKind kind = DefKind::Undefined;
  int obj = -1, index = -1;     // where it is defined (Regular / Absolute / WeakDefault)
  uint32_t commonSize = 0;      // Common
  std::string dll, importName;  // Import
  bool viaThunk = false;        // Import called directly (needs a jump stub)
  std::set<int> referencedBy;   // object indices
};

struct Resolution {
  std::map<std::string, GlobalSymbol> globals;
  std::map<std::string, std::vector<std::string>> imports;  // dll -> functions, in first-use order
  std::vector<std::string> errors;
  std::vector<std::string> notes;
};

Resolution resolveSymbols(std::vector<ObjectFile>& objs, const LinkOptions& opts);
void printResolution(const std::vector<ObjectFile>& objs, const Resolution& res, std::ostream& out);

struct OutSection {
  std::string name;
  uint32_t flags = 0;
  std::vector<uint8_t> data;  // initialised contents (empty for .bss)
  uint32_t virtualSize = 0;
  uint32_t rva = 0, fileOffset = 0, rawSize = 0;
  std::vector<std::pair<int, int>> pieces;  // (object index, section index)
};

struct Image {
  uint64_t imageBase = 0;
  std::vector<OutSection> sections;
  uint32_t sizeOfHeaders = 0, sizeOfImage = 0, entryRva = 0;
  uint32_t importDirRva = 0, importDirSize = 0, iatRva = 0, iatSize = 0;
  uint32_t exceptionRva = 0, exceptionSize = 0;
  std::map<std::string, uint32_t> iatSlotRva;  // function -> RVA of its IAT slot
  std::map<std::string, uint32_t> thunkRva;    // function -> RVA of its jump stub
  std::map<std::string, uint32_t> commonRva;   // COMMON symbol -> RVA in .bss
  std::vector<std::string> dropped;            // human-readable list of dropped sections

  int find(const std::string& name) const {
    for (size_t i = 0; i < sections.size(); i++)
      if (sections[i].name == name) return (int)i;
    return -1;
  }
};

Image layout(std::vector<ObjectFile>& objs, Resolution& res, const LinkOptions& opts);

uint64_t sectionVA(const std::vector<ObjectFile>& objs, const Image& img, int obj, int sec);
uint64_t symbolVA(const std::vector<ObjectFile>& objs, const Resolution& res, const Image& img, int obj, int symIndex);

struct ImportTable {
  std::vector<uint8_t> bytes;  // contents of the .idata section
  uint32_t dirOffset = 0, dirSize = 0, iatOffset = 0, iatSize = 0;
  std::map<std::string, uint32_t> slotOffset;  // function -> offset of its IAT slot
};

ImportTable buildImportTable(const std::map<std::string, std::vector<std::string>>& imports, uint32_t rva);
const char* knownImportDll(const std::string& function);

void applyRelocations(std::vector<ObjectFile>& objs, const Resolution& res, Image& img, bool explain, std::ostream& out);
void writePE(const Image& img, const std::string& path);
void printMap(const std::vector<ObjectFile>& objs, const Resolution& res, const Image& img, std::ostream& out);

}
