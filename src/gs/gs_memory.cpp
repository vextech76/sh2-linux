#include "gs/gs_memory.h"

#include <algorithm>

namespace lantern::gs {
namespace {
// Block arrangement within a page and pixel arrangement within a block (GS User's Manual).
constexpr uint8_t kBlock32[4][8] = {{0, 1, 4, 5, 16, 17, 20, 21},
                                    {2, 3, 6, 7, 18, 19, 22, 23},
                                    {8, 9, 12, 13, 24, 25, 28, 29},
                                    {10, 11, 14, 15, 26, 27, 30, 31}};
constexpr uint8_t kBlockZ32[4][8] = {{24, 25, 28, 29, 8, 9, 12, 13},
                                     {26, 27, 30, 31, 10, 11, 14, 15},
                                     {16, 17, 20, 21, 0, 1, 4, 5},
                                     {18, 19, 22, 23, 2, 3, 6, 7}};
constexpr uint8_t kColumn32[8][8] = {{0, 1, 4, 5, 8, 9, 12, 13},       {2, 3, 6, 7, 10, 11, 14, 15},
                                     {16, 17, 20, 21, 24, 25, 28, 29}, {18, 19, 22, 23, 26, 27, 30, 31},
                                     {32, 33, 36, 37, 40, 41, 44, 45}, {34, 35, 38, 39, 42, 43, 46, 47},
                                     {48, 49, 52, 53, 56, 57, 60, 61}, {50, 51, 54, 55, 58, 59, 62, 63}};
constexpr uint8_t kBlock16[8][4] = {{0, 2, 8, 10},    {1, 3, 9, 11},    {4, 6, 12, 14},   {5, 7, 13, 15},
                                    {16, 18, 24, 26}, {17, 19, 25, 27}, {20, 22, 28, 30}, {21, 23, 29, 31}};
constexpr uint8_t kBlock16s[8][4] = {{0, 2, 16, 18}, {1, 3, 17, 19}, {8, 10, 24, 26},  {9, 11, 25, 27},
                                     {4, 6, 20, 22}, {5, 7, 21, 23}, {12, 14, 28, 30}, {13, 15, 29, 31}};
constexpr uint8_t kBlockZ16[8][4] = {{24, 26, 16, 18}, {25, 27, 17, 19}, {28, 30, 20, 22}, {29, 31, 21, 23},
                                     {8, 10, 0, 2},    {9, 11, 1, 3},    {12, 14, 4, 6},   {13, 15, 5, 7}};
constexpr uint8_t kBlockZ16s[8][4] = {{24, 26, 8, 10},  {25, 27, 9, 11},  {16, 18, 0, 2}, {17, 19, 1, 3},
                                      {28, 30, 12, 14}, {29, 31, 13, 15}, {20, 22, 4, 6}, {21, 23, 5, 7}};
constexpr uint8_t kColumn16[8][16] = {{0, 2, 8, 10, 16, 18, 24, 26, 1, 3, 9, 11, 17, 19, 25, 27},
                                      {4, 6, 12, 14, 20, 22, 28, 30, 5, 7, 13, 15, 21, 23, 29, 31},
                                      {32, 34, 40, 42, 48, 50, 56, 58, 33, 35, 41, 43, 49, 51, 57, 59},
                                      {36, 38, 44, 46, 52, 54, 60, 62, 37, 39, 45, 47, 53, 55, 61, 63},
                                      {64, 66, 72, 74, 80, 82, 88, 90, 65, 67, 73, 75, 81, 83, 89, 91},
                                      {68, 70, 76, 78, 84, 86, 92, 94, 69, 71, 77, 79, 85, 87, 93, 95},
                                      {96, 98, 104, 106, 112, 114, 120, 122, 97, 99, 105, 107, 113, 115, 121, 123},
                                      {100, 102, 108, 110, 116, 118, 124, 126, 101, 103, 109, 111, 117, 119, 125, 127}};
}  // namespace

namespace {
uint32_t byte_addr8(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) {
  const uint32_t pages_per_row = std::max<uint32_t>(1, bw / 2);
  const uint32_t page = (x / 128) + (y / 64) * pages_per_row;
  const uint32_t block = kBlock32[(y / 16) % 4][(x / 16) % 8];
  return (bp * 256 + page * 8192 + block * 256 + (y % 16) * 16 + (x % 16)) & (LocalMemory::kWords * 4 - 1);
}
uint32_t nibble_addr4(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) {
  const uint32_t pages_per_row = std::max<uint32_t>(1, bw / 2);
  const uint32_t page = (x / 128) + (y / 128) * pages_per_row;
  const uint32_t block = kBlock16[(y / 16) % 8][(x / 32) % 4];
  return (bp * 512 + page * 16384 + block * 512 + (y % 16) * 32 + (x % 32)) & (LocalMemory::kWords * 8 - 1);
}
}  // namespace

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

bool LocalMemory::supported(uint32_t psm) {
  switch (psm) {
    case kCt32:
    case kCt24:
    case kCt16:
    case kCt16s:
    case kZ32:
    case kZ24:
    case kZ16:
    case kZ16s: return true;
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
  return (bp * 64 + page * 2048 + block * 64 + kColumn32[y % 8][x % 8]) & (kWords - 1);
}

uint32_t LocalMemory::half_addr16(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) {
  const uint32_t page = (x / 64) + (y / 64) * (bw ? bw : 1);
  const auto& blk = psm == kCt16 ? kBlock16 : psm == kCt16s ? kBlock16s : psm == kZ16 ? kBlockZ16 : kBlockZ16s;
  const uint32_t block = blk[(y / 8) % 8][(x / 16) % 4];
  return (bp * 128 + page * 4096 + block * 128 + kColumn16[y % 8][x % 16]) & (kWords * 2 - 1);
}

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
    default: return 0;
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
    default: return;
  }
}

}  // namespace lantern::gs
