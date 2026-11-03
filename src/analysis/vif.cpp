#include "analysis/vif.h"

#include "ee/elf.h"

namespace lantern::analysis {

std::vector<MpgBlock> scan_vif_mpg(std::span<const uint8_t> image) {
  constexpr uint32_t kMpg = 0x4A;
  constexpr uint32_t kVu1MicroPairs = 2048;  // 16 KB / 8
  constexpr uint32_t kEBit = 1u << 30;       // in the upper (odd) word
  std::vector<MpgBlock> out;
  for (size_t off = 0; off + 4 <= image.size(); off += 4) {
    uint32_t w = ee::load_le32(&image[off]);
    if (((w >> 24) & 0x7F) != kMpg) continue;
    uint32_t count = (w >> 16) & 0xFF;
    if (count == 0) count = 256;
    uint32_t addr = w & 0xFFFF;
    size_t payload = off + 4;
    if (payload % 8 != 0 || addr + count > kVu1MicroPairs) continue;
    if (payload + size_t{count} * 8 > image.size()) continue;
    MpgBlock b{static_cast<uint32_t>(off), addr, count, 0};
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t upper = ee::load_le32(&image[payload + i * 8 + 4]);
      if (upper & kEBit) ++b.e_bits;
    }
    if (b.e_bits == 0) continue;
    out.push_back(b);
    off = payload + size_t{count} * 8 - 4;  // skip the payload
  }
  return out;
}

}  // namespace lantern::analysis
