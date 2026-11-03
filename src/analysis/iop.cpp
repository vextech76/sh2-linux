#include "analysis/iop.h"

#include <cstring>
#include <format>

namespace lantern::analysis {
using ee::load_le32;

namespace {
constexpr uint32_t kSectionIopMod = 0x70000080;
constexpr uint32_t kImportMagic = 0x41E00000;
constexpr uint32_t kExportMagic = 0x41C00000;
constexpr uint32_t kJrRa = 0x03E00008;

std::string fixed_name(const uint8_t* p, size_t n) {
  size_t len = 0;
  while (len < n && p[len] >= 0x20 && p[len] < 0x7F) ++len;
  return std::string(reinterpret_cast<const char*>(p), len);
}
}  // namespace

std::optional<IrxInfo> parse_irx(const ee::ElfImage& elf, std::string* error) {
  IrxInfo info;
  const ee::ElfSection* mod = nullptr;
  for (const auto& s : elf.sections())
    if (s.type == kSectionIopMod) mod = &s;
  if (!mod) {
    if (error) *error = "no .iopmod section (not an IRX module?)";
    return std::nullopt;
  }
  auto m = elf.section_data(*mod);
  // struct { u32 moduleinfo, entry, gp, text_size, data_size, bss_size; u16 version; char name[]; }
  if (m.size() >= 26) {
    info.text_size = load_le32(&m[12]);
    info.data_size = load_le32(&m[16]);
    info.bss_size = load_le32(&m[20]);
    info.version = static_cast<uint16_t>(m[24] | m[25] << 8);
    info.name = fixed_name(&m[26], m.size() - 26);
  }
  const ee::ElfSegment* seg = elf.main_segment();
  if (!seg) return info;
  auto code = elf.segment_data(*seg);
  for (size_t off = 0; off + 20 <= code.size(); off += 4) {
    uint32_t magic = load_le32(&code[off]);
    if ((magic != kImportMagic && magic != kExportMagic) || load_le32(&code[off + 4]) != 0) continue;
    IrxLibrary lib;
    lib.version = static_cast<uint16_t>(load_le32(&code[off + 8]) & 0xFFFF);
    lib.name = fixed_name(&code[off + 12], 8);
    if (lib.name.empty()) continue;
    size_t p = off + 20;
    if (magic == kImportMagic) {
      // stubs: jr ra ; addiu zero,zero,N  ... terminated by 0,0
      while (p + 8 <= code.size() && load_le32(&code[p]) == kJrRa) {
        uint32_t li = load_le32(&code[p + 4]);
        if ((li & 0xFFFF0000u) != 0x24000000u) break;
        lib.functions.push_back(static_cast<uint16_t>(li & 0xFFFF));
        p += 8;
      }
      info.imports.push_back(std::move(lib));
    } else {
      while (p + 4 <= code.size() && load_le32(&code[p]) != 0 && lib.functions.size() < 1024) {
        lib.functions.push_back(static_cast<uint16_t>(lib.functions.size()));
        p += 4;
      }
      info.exports.push_back(std::move(lib));
    }
  }
  return info;
}

std::optional<std::vector<RomdirEntry>> parse_romdir(std::span<const uint8_t> image, std::string* error) {
  size_t dir = std::string_view::npos;
  for (size_t off = 0; off + 16 <= image.size() && off < 0x10000; off += 16)
    if (std::memcmp(&image[off], "RESET\0", 6) == 0) {
      dir = off;
      break;
    }
  if (dir == std::string_view::npos) {
    if (error) *error = "no ROMDIR (RESET entry) found";
    return std::nullopt;
  }
  std::vector<RomdirEntry> out;
  uint32_t data_off = 0;
  for (size_t off = dir; off + 16 <= image.size(); off += 16) {
    const uint8_t* e = &image[off];
    if (e[0] == 0) break;
    RomdirEntry r;
    r.name = fixed_name(e, 10);
    r.size = load_le32(e + 12);
    r.offset = data_off;
    data_off += (r.size + 15) & ~15u;
    out.push_back(std::move(r));
    if (out.size() > 512) break;
  }
  return out;
}

}  // namespace lantern::analysis
