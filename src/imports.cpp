#include "minild.h"

namespace minild {

static void put16(std::vector<uint8_t>& b, size_t off, uint16_t v) {
  b[off] = v & 0xFF;
  b[off + 1] = v >> 8;
}
static void put32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
  for (int i = 0; i < 4; i++) b[off + i] = (v >> (8 * i)) & 0xFF;
}
static void put64(std::vector<uint8_t>& b, size_t off, uint64_t v) {
  for (int i = 0; i < 8; i++) b[off + i] = (v >> (8 * i)) & 0xFF;
}
static uint32_t alignUp(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

ImportTable buildImportTable(const std::map<std::string, std::vector<std::string>>& imports, uint32_t rva) {
  ImportTable t;
  if (imports.empty()) return t;

  uint32_t entries = 0;  // lookup-table entries, including one terminator per DLL
  for (auto& [dll, fns] : imports) entries += fns.size() + 1;

  t.dirOffset = 0;
  t.dirSize = (uint32_t)(imports.size() + 1) * 20;
  const uint32_t iltOffset = t.dirSize;
  t.iatOffset = iltOffset + entries * 8;
  t.iatSize = entries * 8;
  uint32_t cur = t.iatOffset + t.iatSize;

  std::map<std::string, uint32_t> hintName, dllName;
  for (auto& [dll, fns] : imports)
    for (auto& f : fns) {
      hintName[dll + "!" + f] = cur;
      cur = alignUp(cur + 2 + (uint32_t)f.size() + 1, 2);
    }
  for (auto& [dll, fns] : imports) {
    dllName[dll] = cur;
    cur += (uint32_t)dll.size() + 1;
  }
  t.bytes.assign(alignUp(cur, 8), 0);

  uint32_t entry = 0, d = 0;
  for (auto& [dll, fns] : imports) {
    const size_t dir = t.dirOffset + d * 20;
    put32(t.bytes, dir + 0, rva + iltOffset + entry * 8);   // OriginalFirstThunk (lookup table)
    put32(t.bytes, dir + 12, rva + dllName[dll]);            // Name
    put32(t.bytes, dir + 16, rva + t.iatOffset + entry * 8); // FirstThunk (address table)
    for (auto& f : fns) {
      const uint32_t hn = hintName[dll + "!" + f];
      put64(t.bytes, iltOffset + entry * 8, rva + hn);
      put64(t.bytes, t.iatOffset + entry * 8, rva + hn);
      t.slotOffset[f] = t.iatOffset + entry * 8;
      put16(t.bytes, hn, 0);  // hint: 0 = let the loader search by name
      for (size_t k = 0; k < f.size(); k++) t.bytes[hn + 2 + k] = (uint8_t)f[k];
      entry++;
    }
    entry++;  // terminator
    for (size_t k = 0; k < dll.size(); k++) t.bytes[dllName[dll] + k] = (uint8_t)dll[k];
    d++;
  }
  return t;
}

}  // namespace minild
