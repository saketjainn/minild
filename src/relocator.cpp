#include <cstdio>
#include <iomanip>
#include <sstream>

#include "minild.h"

namespace minild {

namespace {
uint64_t read(const uint8_t* p, int n) {
  uint64_t v = 0;
  for (int i = n - 1; i >= 0; i--) v = (v << 8) | p[i];
  return v;
}
void write(uint8_t* p, uint64_t v, int n) {
  for (int i = 0; i < n; i++) p[i] = (uint8_t)(v >> (8 * i));
}
std::string hex(uint64_t v) {
  std::ostringstream s;
  s << "0x" << std::hex << v;
  return s.str();
}
std::string shex(int64_t v) { return v < 0 ? "-" + hex((uint64_t)(-v)) : hex((uint64_t)v); }
}  

void applyRelocations(std::vector<ObjectFile>& objs, const Resolution& res, Image& img, bool explain, std::ostream& out) {
  for (int o = 0; o < (int)objs.size(); o++) {
    ObjectFile& obj = objs[o];
    const std::string file = baseName(obj.path);
    for (int s = 0; s < (int)obj.sections.size(); s++) {
      Section& sec = obj.sections[s];
      if (sec.discarded || sec.relocs.empty()) continue;
      OutSection& os = img.sections[sec.outSection];
      uint8_t* base = os.data.data() + sec.outOffset;
      const uint64_t secVA = sectionVA(objs, img, o, s);

      for (const Reloc& r : sec.relocs) {
        if (r.type == REL_AMD64_ABSOLUTE) continue;
        const Symbol& sym = obj.symbols[r.symIndex];
        const uint64_t S = symbolVA(objs, res, img, o, (int)r.symIndex);
        const uint64_t P = secVA + r.offset;
        uint8_t* field = base + r.offset;
        const std::string where = file + " " + sec.name + "+" + hex(r.offset);
        auto overflow = [&](const std::string& value) {
          throw LinkError(where + ": relocation overflow: " + relocTypeName(r.type) + " against '" + sym.name +
                          "' needs value " + value + ", which does not fit in 4 bytes");
        };

        int width = 4;
        int64_t A = 0;
        uint64_t value = 0;
        std::string formula, calc;
        switch (r.type) {
          case REL_AMD64_ADDR64:
            width = 8;
            A = (int64_t)read(field, 8);
            value = S + A;
            formula = "S + A";
            calc = hex(S) + " + " + shex(A);
            break;
          case REL_AMD64_ADDR32:
            A = (int32_t)read(field, 4);
            value = S + A;
            if (value > 0xFFFFFFFFull) overflow(hex(value));
            formula = "S + A";
            calc = hex(S) + " + " + shex(A);
            break;
          case REL_AMD64_ADDR32NB:
            A = (int32_t)read(field, 4);
            value = S - img.imageBase + A;
            if (value > 0xFFFFFFFFull) overflow(hex(value));
            formula = "S - ImageBase + A";
            calc = hex(S) + " - " + hex(img.imageBase) + " + " + shex(A);
            break;
          case REL_AMD64_SECREL: {  // offset from the start of the symbol's output section (debug info)
            A = (int32_t)read(field, 4);
            const Section& target = objs[o].sections[sym.section > 0 ? sym.section - 1 : s];
            const uint64_t outStart = img.imageBase + img.sections[target.outSection].rva;
            value = S - outStart + A;
            formula = "S - SectionStart + A";
            calc = hex(S) + " - " + hex(outStart) + " + " + shex(A);
            break;
          }
          case REL_AMD64_SECTION: {  
            width = 2;
            const Section& target = objs[o].sections[sym.section > 0 ? sym.section - 1 : s];
            value = (uint64_t)target.outSection + 1;
            formula = "section index";
            calc = std::to_string(value);
            break;
          }
          default:
            if (r.type >= REL_AMD64_REL32 && r.type <= REL_AMD64_REL32_5) {
              const int n = r.type - REL_AMD64_REL32;
              A = (int32_t)read(field, 4);
              const int64_t v = (int64_t)S - (int64_t)(P + 4 + n) + A;
              if (v < INT32_MIN || v > INT32_MAX) overflow(shex(v));
              value = (uint64_t)v;
              const std::string end = n ? "(P + 4 + " + std::to_string(n) + ")" : "(P + 4)";
              formula = "S - " + end + " + A";
              calc = hex(S) + " - " + hex(P + 4 + n) + " + " + shex(A);
            } else {
              throw LinkError(where + ": unsupported relocation type " + relocTypeName(r.type));
            }
        }
        write(field, value, width);

        if (explain) {
          std::ostringstream bytes;
          for (int b = 0; b < width; b++)
            bytes << (b ? " " : "") << std::hex << std::setw(2) << std::setfill('0') << (int)field[b];
          const std::string value32 = width == 4 ? shex((int32_t)(uint32_t)value) : hex(value);
          out << where << "  " << relocTypeName(r.type) << "  " << sym.name << "\n"
              << "  S (address of " << sym.name << ")" << std::string(sym.name.size() < 18 ? 18 - sym.name.size() : 1, ' ')
              << "= " << hex(S) << "\n"
              << "  A (addend stored in the field)   = " << shex(A) << "\n"
              << "  P (address being patched)        = " << hex(secVA) << " (" << file << " " << sec.name << ") + "
              << hex(r.offset) << " = " << hex(P) << "\n"
              << "  value = " << formula << std::string(formula.size() < 25 ? 25 - formula.size() : 1, ' ') << "= " << calc
              << " = " << value32 << "\n"
              << "  write " << width << " bytes: " << bytes.str() << "\n\n";
        }
      }
    }
  }
}

} 
