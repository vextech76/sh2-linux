#include "gs/gs_memory.h"

#include <algorithm>
#include <array>

namespace lantern::gs {
// Local-memory layout.
//
// Documentation basis: the GS organises local memory as pages of 8 KB, subdivided into 256-byte
// blocks, and a block into columns. The block tables below (which block sits at which position of
// the page) and the page geometry per format are the documented part. The remaining detail -- how
// a pixel index maps to an offset inside a block -- is expressed here as a *bit order*: offset bit
// i is fed by coordinate bit x[k] or y[k].
//
// CT32 and CT16 orders were derived from the two tables that are demonstrably correct on screen
// (the game's 32-bit font and framebuffer render pixel-exact), and both reproduce those tables
// exactly (`generated CT32/CT16 tables match hand tables`, tests/test_gs.cpp). The CT16 order is
// exactly the CT32 order with the extra x bit placed below x0, which is the structural rule used
// to extend it: finer formats prepend their additional x bits (then y bits) at the low end.
//
// PSMT8/PSMT4 in-block order is *not* published in the freely available public documentation
// (ps2tek documents GS registers, GIF/VIF paths and the IOP, but not these two tables), so the
// order for those two formats is identified empirically against the game's own output
// (`--swizzle-variant`), and the default below is the variant that reproduces the game's
// textures. No emulator source was used or consulted.

namespace {

// Which coordinate bit feeds each in-block offset bit. Encoded: 0..7 = x0..x7, 16..23 = y0..y7.
struct BitOrder {
  std::vector<uint8_t> bits;          // LSB first
  uint32_t block_w = 0, block_h = 0;  // pixels per block
};

constexpr uint8_t X(unsigned i) { return static_cast<uint8_t>(i); }
constexpr uint8_t Y(unsigned i) { return static_cast<uint8_t>(16 + i); }

// Validated (see header comment).
const BitOrder kOrderCt32{{X(0), Y(0), X(1), X(2), Y(1), Y(2)}, 8, 8};
const BitOrder kOrderCt16{{X(3), X(0), Y(0), X(1), X(2), Y(1), Y(2)}, 16, 8};

// Candidate orders for the indexed formats. The base pattern is CT32's; candidates differ only in
// where the additional coordinate bits of the finer format go.
//   T8 block = 16x16 (x0-3, y0-3): base 6 bits + x3 + y3
//   T4 block = 32x16 (x0-4, y0-3): base 6 bits + x3 + x4 + y3
BitOrder order_t8(int variant) {
  switch (variant) {
    case 0: return {{Y(3), X(3), X(0), Y(0), X(1), X(2), Y(1), Y(2)}, 16, 16};  // measured (title screen)
    case 1: return {{X(3), Y(3), X(0), Y(0), X(1), X(2), Y(1), Y(2)}, 16, 16};
    case 2: return {{X(0), Y(0), X(1), X(2), Y(1), Y(2), X(3), Y(3)}, 16, 16};  // extras above y2
    case 3: return {{X(0), Y(0), X(1), X(2), Y(1), Y(2), Y(3), X(3)}, 16, 16};
    case 4: return {{Y(3), Y(2), Y(1), Y(0), X(3), X(2), X(1), X(0)}, 16, 16};  // row-major reference
    default: return order_t8(0);
  }
}
BitOrder order_t4(int variant) {
  switch (variant) {
    case 0: return {{Y(3), X(4), X(3), X(0), Y(0), X(1), X(2), Y(1), Y(2)}, 32, 16};  // same rule as T8
    case 1: return {{Y(3), X(3), X(4), X(0), Y(0), X(1), X(2), Y(1), Y(2)}, 32, 16};
    case 2: return {{X(3), X(4), X(0), Y(0), X(1), X(2), Y(1), Y(2), Y(3)}, 32, 16};
    case 3: return {{X(3), X(0), Y(0), X(1), X(2), Y(1), Y(2), X(4), Y(3)}, 32, 16};
    case 4: return {{Y(3), Y(2), Y(1), Y(0), X(4), X(3), X(2), X(1), X(0)}, 32, 16};  // row-major reference
    default: return order_t4(0);
  }
}

int g_variant_t8 = 0, g_variant_t4 = 0;

const std::array<BitOrder, 5>& variant_orders_t8() {
  static const std::array<BitOrder, 5> k = {order_t8(0), order_t8(1), order_t8(2), order_t8(3), order_t8(4)};
  return k;
}
const std::array<BitOrder, 5>& variant_orders_t4() {
  static const std::array<BitOrder, 5> k = {order_t4(0), order_t4(1), order_t4(2), order_t4(3), order_t4(4)};
  return k;
}

const BitOrder& order_for(uint32_t psm) {
  switch (psm) {
    case kCt32:
    case kZ32:
    case kCt24:
    case kZ24: return kOrderCt32;
    case kCt16:
    case kCt16s:
    case kZ16:
    case kZ16s: return kOrderCt16;
    case kT8:
    case kT8h: return variant_orders_t8()[static_cast<size_t>(std::clamp(g_variant_t8, 0, 4))];
    case kT4:
    case kT4hl:
    case kT4hh: return variant_orders_t4()[static_cast<size_t>(std::clamp(g_variant_t4, 0, 4))];
    default: return kOrderCt32;
  }
}

// Documented block arrangement inside a page: 32 blocks per page, in a 4x8 or 8x4 grid. These
// tables, and the page geometry per format, are the part of the layout that is derivable from the
// public documentation of the GS local-memory organisation; they are unchanged from the version
// that renders the game's 32-bit font pixel-exactly.
constexpr uint8_t kBlock32[4][8] = {{0, 1, 4, 5, 16, 17, 20, 21},
                                    {2, 3, 6, 7, 18, 19, 22, 23},
                                    {8, 9, 12, 13, 24, 25, 28, 29},
                                    {10, 11, 14, 15, 26, 27, 30, 31}};
constexpr uint8_t kBlockZ32[4][8] = {{24, 25, 28, 29, 8, 9, 12, 13},
                                     {26, 27, 30, 31, 10, 11, 14, 15},
                                     {16, 17, 20, 21, 0, 1, 4, 5},
                                     {18, 19, 22, 23, 2, 3, 6, 7}};
constexpr uint8_t kBlock16[8][4] = {{0, 2, 8, 10},    {1, 3, 9, 11},    {4, 6, 12, 14},   {5, 7, 13, 15},
                                    {16, 18, 24, 26}, {17, 19, 25, 27}, {20, 22, 28, 30}, {21, 23, 29, 31}};
constexpr uint8_t kBlock16s[8][4] = {{0, 2, 16, 18}, {1, 3, 17, 19}, {8, 10, 24, 26},  {9, 11, 25, 27},
                                     {4, 6, 20, 22}, {5, 7, 21, 23}, {12, 14, 28, 30}, {13, 15, 29, 31}};
constexpr uint8_t kBlockZ16[8][4] = {{24, 26, 16, 18}, {25, 27, 17, 19}, {28, 30, 20, 22}, {29, 31, 21, 23},
                                     {8, 10, 0, 2},    {9, 11, 1, 3},    {12, 14, 4, 6},   {13, 15, 5, 7}};
constexpr uint8_t kBlockZ16s[8][4] = {{24, 26, 8, 10},  {25, 27, 9, 11},  {16, 18, 0, 2}, {17, 19, 1, 3},
                                      {28, 30, 12, 14}, {29, 31, 13, 15}, {20, 22, 4, 6}, {21, 23, 5, 7}};

uint32_t in_block(const BitOrder& o, uint32_t x, uint32_t y) {
  uint32_t v = 0;
  for (size_t i = 0; i < o.bits.size(); ++i) {
    const uint8_t b = o.bits[i];
    const uint32_t bit = (b & 16) ? (y >> (b & 15)) & 1 : (x >> b) & 1;
    v |= bit << i;
  }
  return v;
}
}  // namespace

void LocalMemory::set_indexed_variant(int t8_variant, int t4_variant) {
  g_variant_t8 = t8_variant;
  g_variant_t4 = t4_variant;
}
int LocalMemory::indexed_variant(uint32_t psm) { return psm == kT8 || psm == kT8h ? g_variant_t8 : g_variant_t4; }

std::string LocalMemory::bit_order_name(uint32_t psm) {
  const BitOrder& o = order_for(psm);
  std::string s;
  for (size_t i = o.bits.size(); i-- > 0;) {
    const uint8_t b = o.bits[i];
    s += (b & 16) ? 'y' : 'x';
    s += static_cast<char>('0' + (b & 15));
    if (i) s += ',';
  }
  return s;
}

uint32_t LocalMemory::in_block_offset(uint32_t psm, uint32_t x, uint32_t y) {
  const BitOrder& o = order_for(psm);
  return in_block(o, x % o.block_w, y % o.block_h);
}

bool LocalMemory::supported(uint32_t psm) {
  switch (psm) {
    case kCt32:
    case kCt24:
    case kCt16:
    case kCt16s:
    case kZ32:
    case kZ24:
    case kZ16:
    case kZ16s:
    case kT8:
    case kT4:
    case kT8h:
    case kT4hl:
    case kT4hh: return true;
    default: return false;
  }
}

uint32_t LocalMemory::word_addr32(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) {
  const uint32_t page = (x / 64) + (y / 32) * (bw ? bw : 1);
  const auto& blk = (psm & 0x30) == 0x30 ? kBlockZ32 : kBlock32;
  const uint32_t block = blk[(y / 8) % 4][(x / 8) % 8];
  return (bp * 64 + page * 2048 + block * 64 + in_block_offset(psm, x, y)) & (kWords - 1);
}

uint32_t LocalMemory::half_addr16(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) {
  const uint32_t page = (x / 64) + (y / 64) * (bw ? bw : 1);
  const auto& blk = psm == kCt16 ? kBlock16 : psm == kCt16s ? kBlock16s : psm == kZ16 ? kBlockZ16 : kBlockZ16s;
  const uint32_t block = blk[(y / 8) % 8][(x / 16) % 4];
  return (bp * 128 + page * 4096 + block * 128 + in_block_offset(psm, x, y)) & (kWords * 2 - 1);
}

namespace {
uint32_t byte_addr8(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) {
  const uint32_t pages_per_row = std::max<uint32_t>(1, bw / 2);
  const uint32_t page = (x / 128) + (y / 64) * pages_per_row;
  const uint32_t block = kBlock32[(y / 16) % 4][(x / 16) % 8];
  return (bp * 256 + page * 8192 + block * 256 + LocalMemory::in_block_offset(kT8, x, y)) &
         (LocalMemory::kWords * 4 - 1);
}
uint32_t nibble_addr4(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) {
  const uint32_t pages_per_row = std::max<uint32_t>(1, bw / 2);
  const uint32_t page = (x / 128) + (y / 128) * pages_per_row;
  const uint32_t block = kBlock16[(y / 16) % 8][(x / 32) % 4];
  return (bp * 512 + page * 16384 + block * 512 + LocalMemory::in_block_offset(kT4, x, y)) &
         (LocalMemory::kWords * 8 - 1);
}
}  // namespace

uint32_t LocalMemory::read_pixel(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) const {
  switch (psm) {
    case kCt32:
    case kZ32: return mem_[word_addr32(psm, bp, bw, x, y)];
    case kCt24:
    case kZ24: return mem_[word_addr32(psm, bp, bw, x, y)] & 0xFFFFFF;
    case kCt16:
    case kCt16s:
    case kZ16:
    case kZ16s: {
      const uint32_t h = half_addr16(psm, bp, bw, x, y);
      return (mem_[h >> 1] >> ((h & 1) * 16)) & 0xFFFF;
    }
    default: return read_index(psm, bp, bw, x, y);
  }
}

void LocalMemory::write_pixel(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t v) {
  switch (psm) {
    case kCt32:
    case kZ32: mem_[word_addr32(psm, bp, bw, x, y)] = v; return;
    case kCt24:
    case kZ24: {
      uint32_t& w = mem_[word_addr32(psm, bp, bw, x, y)];
      w = (w & 0xFF000000u) | (v & 0xFFFFFF);
      return;
    }
    case kCt16:
    case kCt16s:
    case kZ16:
    case kZ16s: {
      const uint32_t h = half_addr16(psm, bp, bw, x, y);
      uint32_t& w = mem_[h >> 1];
      const uint32_t shift = (h & 1) * 16;
      w = (w & ~(0xFFFFu << shift)) | ((v & 0xFFFF) << shift);
      return;
    }
    default: write_index(psm, bp, bw, x, y, v); return;
  }
}

uint32_t LocalMemory::read_index(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) const {
  const uint8_t* b = reinterpret_cast<const uint8_t*>(mem_.data());
  switch (psm) {
    case kT8: return b[byte_addr8(bp, bw, x, y)];
    case kT4: {
      const uint32_t n = nibble_addr4(bp, bw, x, y);
      return (b[n >> 1] >> ((n & 1) * 4)) & 0xF;
    }
    case kT8h: return mem_[word_addr32(kCt32, bp, bw, x, y)] >> 24;
    case kT4hl: return (mem_[word_addr32(kCt32, bp, bw, x, y)] >> 24) & 0xF;
    case kT4hh: return mem_[word_addr32(kCt32, bp, bw, x, y)] >> 28;
    default: return 0;
  }
}

void LocalMemory::write_index(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t v) {
  uint8_t* b = reinterpret_cast<uint8_t*>(mem_.data());
  switch (psm) {
    case kT8: b[byte_addr8(bp, bw, x, y)] = static_cast<uint8_t>(v); return;
    case kT4: {
      const uint32_t n = nibble_addr4(bp, bw, x, y);
      uint8_t& t = b[n >> 1];
      t = (n & 1) ? static_cast<uint8_t>((t & 0x0F) | ((v & 0xF) << 4)) : static_cast<uint8_t>((t & 0xF0) | (v & 0xF));
      return;
    }
    case kT8h: {
      uint32_t& w = mem_[word_addr32(kCt32, bp, bw, x, y)];
      w = (w & 0x00FFFFFFu) | (v << 24);
      return;
    }
    case kT4hl: {
      uint32_t& w = mem_[word_addr32(kCt32, bp, bw, x, y)];
      w = (w & ~0x0F000000u) | ((v & 0xF) << 24);
      return;
    }
    case kT4hh: {
      uint32_t& w = mem_[word_addr32(kCt32, bp, bw, x, y)];
      w = (w & ~0xF0000000u) | ((v & 0xF) << 28);
      return;
    }
    default: return;
  }
}

}  // namespace lantern::gs
