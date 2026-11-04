#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lantern::gs {

// GS pixel storage formats (PSM).
enum Psm : uint32_t {
  kCt32 = 0x00,
  kCt24 = 0x01,
  kCt16 = 0x02,
  kCt16s = 0x0A,
  kT8 = 0x13,
  kT4 = 0x14,
  kT8h = 0x1B,
  kT4hl = 0x24,
  kT4hh = 0x2C,
  kZ32 = 0x30,
  kZ24 = 0x31,
  kZ16 = 0x32,
  kZ16s = 0x3A,
};

// 4 MB GS local memory with the GS's block/page swizzling. Addresses: bp in 64-word blocks,
// bw in 64-pixel units.
class LocalMemory {
 public:
  static constexpr uint32_t kWords = 1u << 20;
  LocalMemory() : mem_(kWords, 0) {}

  static bool supported(uint32_t psm);
  // Word (32-bit) address of pixel (x, y) for 32/24-bit formats.
  static uint32_t word_addr32(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y);
  // Halfword address for 16-bit formats.
  static uint32_t half_addr16(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y);

  uint32_t read_pixel(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) const;
  void write_pixel(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t value);

  uint32_t* words() { return mem_.data(); }

  uint32_t read_index(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) const;
  void write_index(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t v);

  // In-block offset (in format units) for a pixel inside its block, derived from the per-format
  // bit order below. Exposed for tests.
  static uint32_t in_block_offset(uint32_t psm, uint32_t x, uint32_t y);
  // The bit order in use for a format, e.g. "x3,x0,y0,x1,x2,y1,y2".
  static std::string bit_order_name(uint32_t psm);
  // Variant selection for the indexed formats (empirical, see DESIGN.md): 0 = derived default.
  static void set_indexed_variant(int t8_variant, int t4_variant);
  static int indexed_variant(uint32_t psm);

 private:
  std::vector<uint32_t> mem_;
};

}  // namespace lantern::gs
