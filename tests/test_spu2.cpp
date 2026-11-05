#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "iop/spu2.h"

using namespace lantern::iop;

namespace {
void reg16(Spu2& s, uint32_t addr, uint16_t v) { s.write(addr, &v, 2); }
uint16_t get16(const Spu2& s, uint32_t addr) {
  uint16_t v = 0;
  s.read(addr, &v, 2);
  return v;
}
void adpcm_block(Spu2& s, uint32_t unit, uint8_t shift, uint8_t filter, uint8_t flags, const uint8_t* nibbles) {
  uint8_t* p = s.ram() + unit * 8;
  p[0] = static_cast<uint8_t>((filter << 4) | (shift & 0x0F));
  p[1] = flags;
  std::memset(p + 2, 0, 14);
  for (int i = 0; i < 28; ++i) p[2 + i / 2] |= static_cast<uint8_t>((nibbles[i] & 0x0F) << ((i & 1) ? 4 : 0));
}
}  // namespace

TEST_CASE("SPU2: voice registers read back") {
  Spu2 s;
  reg16(s, 0x1F900000, 0x1234);  // voice 0 volume left
  reg16(s, 0x1F900004, 0x0ABC);  // pitch
  CHECK(get16(s, 0x1F900000) == 0x1234);
  CHECK(get16(s, 0x1F900004) == 0x0ABC);
  reg16(s, 0x1F900400, 0x4321);  // core 1 voice 0 left
  CHECK(get16(s, 0x1F900400) == 0x4321);
  CHECK(get16(s, 0x1F900000) == 0x1234);
}

TEST_CASE("SPU2: key-on plays an ADPCM block and key-off sets ENDX and IRQ") {
  Spu2 s;
  int irq = -1;
  s.set_irq_callback([&](int n) { irq = n; });
  uint8_t nibbles[28];
  for (int i = 0; i < 28; ++i) nibbles[i] = 7;
  adpcm_block(s, 0, 0, 0, 0, nibbles);

  reg16(s, 0x1F900000, 0x7FFF);  // volume left
  reg16(s, 0x1F900002, 0x7FFF);
  reg16(s, 0x1F900004, 0x1000);  // pitch
  reg16(s, 0x1F900760, 0x7FFF);  // master left
  reg16(s, 0x1F900762, 0x7FFF);
  // SSA = 0 (0x1C0/0x1C2), LSAX = 0
  reg16(s, 0x1F9001C0, 0);
  reg16(s, 0x1F9001C2, 0);
  reg16(s, 0x1F9001C4, 0);
  reg16(s, 0x1F9001C6, 0);
  reg16(s, 0x1F9001A0, 1);  // key on voice 0

  int16_t out[2 * 8]{};
  s.mix(out, 8);
  CHECK((out[0] != 0 || out[1] != 0));

  reg16(s, 0x1F9001A4, 1);  // key off voice 0
  CHECK((s.endx(0) & 1) != 0);
  CHECK(irq == 9);
  CHECK(s.take_irq(0));
  CHECK_FALSE(s.take_irq(0));
}

TEST_CASE("SPU2: an ADPCM loop-end block stops the voice") {
  Spu2 s;
  uint8_t nibbles[28];
  for (int i = 0; i < 28; ++i) nibbles[i] = 0;
  adpcm_block(s, 0, 0, 0, 0x01, nibbles);  // loop end, no repeat
  reg16(s, 0x1F900000, 0x7FFF);
  reg16(s, 0x1F900002, 0x7FFF);
  reg16(s, 0x1F900004, 0x1000);
  reg16(s, 0x1F900760, 0x7FFF);
  reg16(s, 0x1F900762, 0x7FFF);
  reg16(s, 0x1F9001A0, 1);
  std::vector<int16_t> out(2 * 128);
  s.mix(out.data(), 128);
  CHECK((s.endx(0) & 1) != 0);
}

TEST_CASE("SPU2: transfer data writes SPU RAM at TSA") {
  Spu2 s;
  uint32_t tsa = 0;
  s.write(0x1F9001A8, &tsa, 4);
  reg16(s, 0x1F9001AC, 0xBEEF);
  CHECK(s.ram()[0] == 0xEF);
  CHECK(s.ram()[1] == 0xBE);
}
