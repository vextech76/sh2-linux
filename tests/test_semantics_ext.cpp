#include <doctest/doctest.h>

#include <bit>
#include <cstring>
#include <memory>

#include "ee/semantics.h"
#include "mips_asm.h"

using namespace lantern::ee;
using namespace lantern::test::mips;

namespace {
uint32_t fb(float f) { return std::bit_cast<uint32_t>(f); }
float bf(uint32_t b) { return std::bit_cast<float>(b); }
constexpr uint32_t kX = 8, kXY = 12, kXYZ = 14, kXYZW = 15;

// VU0 macro encoders: special1 (funct < 60) and special2 (11-bit opcode).
constexpr uint32_t vs1(unsigned funct, unsigned dest, unsigned ft_, unsigned fs_, unsigned fd_) {
  return 18u << 26 | 1u << 25 | dest << 21 | ft_ << 16 | fs_ << 11 | fd_ << 6 | funct;
}
constexpr uint32_t vs2(unsigned idx, unsigned dest, unsigned ft_, unsigned fs_) {
  return 18u << 26 | 1u << 25 | dest << 21 | ft_ << 16 | fs_ << 11 | (idx >> 2) << 6 | 0x3C | (idx & 3);
}
constexpr uint32_t mmi(unsigned f, unsigned sub, unsigned rd_, unsigned rs_, unsigned rt_) {
  return 28u << 26 | R(rs_, rt_, rd_, sub, f);
}

struct M {
  std::unique_ptr<Memory> mem = std::make_unique<Memory>();
  std::unique_ptr<Cpu> cpu = std::make_unique<Cpu>();
  void vec(unsigned r, float x, float y, float z, float w) {
    cpu->vu0.vf[r].u32[0] = fb(x);
    cpu->vu0.vf[r].u32[1] = fb(y);
    cpu->vu0.vf[r].u32[2] = fb(z);
    cpu->vu0.vf[r].u32[3] = fb(w);
  }
  float f(unsigned r, int comp) const { return bf(cpu->vu0.vf[r].u32[comp]); }
  void s1(uint32_t w) { sem::VU0_SPECIAL1(*cpu, *mem, w, 0); }
  void s2(uint32_t w) { sem::VU0_SPECIAL2(*cpu, *mem, w, 0); }
};
}  // namespace

TEST_CASE("VU0: add, broadcast multiply, dest mask, vf0 is constant") {
  M m;
  m.vec(1, 1, 2, 3, 4);
  m.vec(2, 10, 20, 30, 40);
  m.s1(vs1(40, kXYZW, 2, 1, 3));  // vadd.xyzw vf3, vf1, vf2
  CHECK(m.f(3, 0) == 11);
  CHECK(m.f(3, 3) == 44);
  m.s1(vs1(24 + 3, kXY, 2, 1, 4));  // vmulw.xy vf4, vf1, vf2w
  CHECK(m.f(4, 0) == 40);
  CHECK(m.f(4, 1) == 80);
  CHECK(m.f(4, 2) == 0);          // masked out
  m.s1(vs1(40, kXYZW, 2, 1, 0));  // write to vf0 is discarded
  CHECK(m.f(0, 3) == 1.0f);
  CHECK(m.f(0, 0) == 0.0f);
}

TEST_CASE("VU0: cross product via vopmula/vopmsub") {
  M m;
  m.vec(1, 1, 0, 0, 0);          // x axis
  m.vec(2, 0, 1, 0, 0);          // y axis
  m.s2(vs2(46, kXYZ, 2, 1));     // vopmula.xyz ACC, vf1, vf2
  m.s1(vs1(46, kXYZ, 1, 2, 3));  // vopmsub.xyz vf3, vf2, vf1
  CHECK(m.f(3, 0) == 0);
  CHECK(m.f(3, 1) == 0);
  CHECK(m.f(3, 2) == 1);  // x cross y = z
}

TEST_CASE("VU0: Q division, ftoi/itof, mr32, flags, clip") {
  M m;
  m.vec(1, 9, 0, 0, 0);
  m.vec(2, 0, 3, 0, 0);
  m.s2(vs2(56, (1u << 2) | 0, 2, 1));  // vdiv Q, vf1x, vf2y   (ftf = y -> bits 24..23 = 1)
  CHECK(bf(m.cpu->vu0.q) == 3.0f);
  m.s1(vs1(28, kX, 0, 1, 4));  // vmulq.x vf4, vf1, Q
  CHECK(m.f(4, 0) == 27.0f);

  m.vec(5, 1.5f, -2.25f, 0, 0);
  m.s2(vs2(21, kXYZW, 6, 5));  // vftoi4 vf6, vf5
  CHECK(m.cpu->vu0.vf[6].s32[0] == 24);
  CHECK(m.cpu->vu0.vf[6].s32[1] == -36);
  m.s2(vs2(17, kXYZW, 7, 6));  // vitof4 vf7, vf6
  CHECK(m.f(7, 1) == -2.25f);

  m.vec(8, 1, 2, 3, 4);
  m.s2(vs2(49, kXYZW, 9, 8));  // vmr32
  CHECK(m.f(9, 0) == 2);
  CHECK(m.f(9, 3) == 1);

  m.vec(10, -1, 0, 5, 0);
  m.vec(11, 1, 1, 1, 1);
  m.s1(vs1(40, kXYZW, 11, 10, 12));     // vadd: (0, 1, 6, 1)
  CHECK((m.cpu->vu0.mac & 0x8) != 0);   // Zx
  CHECK((m.cpu->vu0.mac & 0xF0) == 0);  // no sign bits
  CHECK((m.cpu->vu0.status & 1) != 0);

  m.vec(13, 2, -2, 0.5f, 0);
  m.vec(14, 0, 0, 0, 1);
  m.s2(vs2(31, kXYZ, 14, 13));         // vclipw.xyz vf13, vf14w
  CHECK(m.cpu->vu0.clip == 0b001001);  // +x > w, -y < -w
}

TEST_CASE("VU0: COP2 transfers") {
  M m;
  auto& c = *m.cpu;
  c.gpr[t0].u64[0] = 0x1111;
  c.gpr[t0].u64[1] = 0x2222;
  sem::QMTC2(c, *m.mem, 18u << 26 | 5u << 21 | t0 << 16 | 3u << 11, 0);
  CHECK(c.vu0.vf[3].u64[1] == 0x2222);
  sem::QMFC2(c, *m.mem, 18u << 26 | 1u << 21 | t1 << 16 | 3u << 11, 0);
  CHECK(c.gpr[t1].u64[0] == 0x1111);
  c.r(t2) = 77;
  sem::CTC2(c, *m.mem, 18u << 26 | 6u << 21 | t2 << 16 | 5u << 11, 0);  // vi5 = 77
  CHECK(c.vu0.vi[5] == 77);
  sem::CFC2(c, *m.mem, 18u << 26 | 2u << 21 | t3 << 16 | 5u << 11, 0);
  CHECK(c.r(t3) == 77);
  c.r(a0) = 0x00200000;
  sem::SQC2(c, *m.mem, I(62, a0, 3, 0), 0);
  CHECK(m.mem->read<uint64_t>(0x00200008) == 0x2222);
}

TEST_CASE("MMI: saturation, paddub as 128-bit move, abs, packs") {
  M m;
  auto& c = *m.cpu;
  for (int i = 0; i < 16; ++i) c.gpr[a0].u8[i] = static_cast<uint8_t>(200 + i);
  for (int i = 0; i < 16; ++i) c.gpr[a1].u8[i] = 100;
  sem::PADDUB(c, *m.mem, mmi(40, 24, t0, a0, a1), 0);
  CHECK(c.gpr[t0].u8[0] == 255);
  sem::PADDUB(c, *m.mem, mmi(40, 24, t1, a0, zero), 0);  // move
  CHECK(std::memcmp(&c.gpr[t1], &c.gpr[a0], 16) == 0);

  c.gpr[a2].s32[0] = INT32_MIN;
  c.gpr[a2].s32[1] = -5;
  sem::PABSW(c, *m.mem, mmi(40, 1, t2, 0, a2), 0);
  CHECK(c.gpr[t2].s32[0] == INT32_MAX);
  CHECK(c.gpr[t2].s32[1] == 5);

  c.gpr[a3].s32[0] = INT32_MAX;
  c.gpr[t3].s32[0] = 1;
  sem::PADDSW(c, *m.mem, mmi(8, 16, t4, a3, t3), 0);
  CHECK(c.gpr[t4].s32[0] == INT32_MAX);

  c.gpr[s0].u32[0] = 0x80FF8040;  // a=1, r=0x40, g=0x80, b=0xFF (8888) -> 1555 and back
  sem::PPAC5(c, *m.mem, mmi(8, 31, s1, 0, s0), 0);
  CHECK(c.gpr[s1].u32[0] == 0xFE08u);
  sem::PEXT5(c, *m.mem, mmi(8, 30, s2, 0, s1), 0);
  CHECK(c.gpr[s2].u32[0] == 0x80F88040u);
}

TEST_CASE("MMI: halfword multiply into HI/LO and PMFHL") {
  M m;
  auto& c = *m.cpu;
  for (int i = 0; i < 8; ++i) {
    c.gpr[a0].s16[i] = static_cast<int16_t>(i + 1);
    c.gpr[a1].s16[i] = 10;
  }
  sem::PMULTH(c, *m.mem, mmi(9, 28, t0, a0, a1), 0);
  CHECK(c.lo.s32[0] == 10);  // p0
  CHECK(c.lo.s32[1] == 20);  // p1
  CHECK(c.hi.s32[0] == 30);  // p2
  CHECK(c.gpr[t0].s32[0] == 10);
  CHECK(c.gpr[t0].s32[1] == 30);
  sem::PHMADH(c, *m.mem, mmi(9, 17, t1, a0, a1), 0);
  CHECK(c.lo.s32[0] == 30);   // p1 + p0
  CHECK(c.hi.s32[2] == 150);  // p7 + p6
  sem::PMFHL_LW(c, *m.mem, mmi(48, 0, t2, 0, 0), 0);
  CHECK(c.gpr[t2].s32[0] == c.lo.s32[0]);
  CHECK(c.gpr[t2].s32[1] == c.hi.s32[0]);
}
