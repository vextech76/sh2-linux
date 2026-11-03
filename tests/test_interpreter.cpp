#include <doctest/doctest.h>

#include <bit>
#include <vector>

#include "ee/interpreter.h"
#include "mips_asm.h"

using namespace lantern::ee;
using namespace lantern::test::mips;

namespace {
constexpr uint32_t kBase = 0x00100000;
constexpr uint32_t kRet = 0x00FF0000;  // sentinel return address

struct Machine {
  Memory mem;
  Cpu cpu;
  Interpreter interp;
  void load(const std::vector<uint32_t>& code, uint32_t at = kBase) {
    for (size_t i = 0; i < code.size(); ++i) mem.write<uint32_t>(at + static_cast<uint32_t>(i * 4), code[i]);
  }
  // Run code as a function: returns when it jumps to the sentinel return address.
  uint64_t call(uint32_t entry = kBase) {
    cpu.pc = entry;
    cpu.r(ra) = kRet;
    cpu.r(sp) = 0x01000000;
    return interp.run(cpu, mem, kRet, 100000);
  }
  int64_t s(unsigned r) const { return static_cast<int64_t>(cpu.r(r)); }
};

uint32_t fbits(float f) { return std::bit_cast<uint32_t>(f); }
}  // namespace

TEST_CASE("32-bit results sign-extend; $zero is immutable; upper 64 bits preserved") {
  Machine m;
  m.cpu.gpr[t0].u64[1] = 0xDEADBEEF;
  m.load({lui(t1, 0x7FFF), ori(t1, t1, 0xFFFF), addiu(t0, t1, 1), addiu(zero, t1, 5), jr(ra), nop});
  m.call();
  CHECK(m.cpu.r(t0) == 0xFFFFFFFF80000000ull);
  CHECK(m.cpu.gpr[t0].u64[1] == 0xDEADBEEF);
  CHECK(m.cpu.r(zero) == 0);
}

TEST_CASE("64-bit ops do not sign-extend") {
  Machine m;
  m.load({lui(t1, 0x7FFF), ori(t1, t1, 0xFFFF), daddiu(t0, t1, 1), jr(ra), nop});
  m.call();
  CHECK(m.cpu.r(t0) == 0x80000000ull);
}

TEST_CASE("MULT writes rd, HI and LO") {
  Machine m;
  m.load({addiu(a0, zero, -3), addiu(a1, zero, 7), mult(v0, a0, a1), mfhi(v1), jr(ra), nop});
  m.call();
  CHECK(m.s(v0) == -21);
  CHECK(m.s(v1) == -1);
}

TEST_CASE("DIV follows R5900 divide-by-zero and overflow results") {
  Machine m;
  m.load({addiu(a0, zero, 17),
          addiu(a1, zero, -5),
          div(a0, a1),
          mflo(t0),
          mfhi(t1),  //
          div(a0, zero),
          mflo(t2),
          mfhi(t3),  //
          lui(a2, 0x8000),
          addiu(a3, zero, -1),
          div(a2, a3),
          mflo(t4),
          mfhi(t5),  //
          addiu(a0, zero, -4),
          div(a0, zero),
          mflo(t6),
          divu(a0, zero),
          mflo(t7),
          jr(ra),
          nop});
  m.call();
  CHECK(m.s(t0) == -3);
  CHECK(m.s(t1) == 2);
  CHECK(m.s(t2) == -1);  // positive / 0
  CHECK(m.s(t3) == 17);
  CHECK(m.s(t4) == static_cast<int32_t>(0x80000000));
  CHECK(m.s(t5) == 0);
  CHECK(m.s(t6) == 1);   // negative / 0
  CHECK(m.s(t7) == -1);  // unsigned / 0 -> all ones
}

TEST_CASE("LWL/LWR and SWL/SWR implement little-endian unaligned words") {
  Machine m;
  const uint32_t data = 0x00200000;
  for (uint32_t i = 0; i < 16; ++i) m.mem.write<uint8_t>(data + i, static_cast<uint8_t>(0x10 + i));
  m.cpu.r(a0) = data + 1;
  m.load({lwr(t0, 0, a0), lwl(t0, 3, a0), addiu(a1, a0, 8), swr(t0, 0, a1), swl(t0, 3, a1), jr(ra), nop});
  m.call();
  CHECK(m.cpu.r(t0) == 0x14131211ull);
  CHECK(m.mem.read<uint32_t>(data + 9) == 0x14131211u);
  CHECK(m.mem.read<uint8_t>(data + 8) == 0x18);  // neighbours untouched
  CHECK(m.mem.read<uint8_t>(data + 13) == 0x1D);
}

TEST_CASE("LDL/LDR and SDL/SDR implement little-endian unaligned doublewords") {
  Machine m;
  const uint32_t data = 0x00200000;
  for (uint32_t i = 0; i < 32; ++i) m.mem.write<uint8_t>(data + i, static_cast<uint8_t>(i));
  m.cpu.r(a0) = data + 3;
  m.load({ldr(t0, 0, a0), ldl(t0, 7, a0), addiu(a1, a0, 16), sdr(t0, 0, a1), sdl(t0, 7, a1), jr(ra), nop});
  m.call();
  CHECK(m.cpu.r(t0) == 0x0A09080706050403ull);
  CHECK(m.mem.read<uint64_t>(data + 19) == 0x0A09080706050403ull);
  CHECK(m.mem.read<uint8_t>(data + 18) == 18);
  CHECK(m.mem.read<uint8_t>(data + 27) == 27);
}

TEST_CASE("LQ/SQ ignore low address bits; unaligned LW traps") {
  Machine m;
  const uint32_t data = 0x00200000;
  m.mem.write<uint64_t>(data, 0x1111);
  m.mem.write<uint64_t>(data + 8, 0x2222);
  m.cpu.r(a0) = data + 5;
  m.load({lq(t0, 0, a0), sq(t0, 16 + 3, a0), jr(ra), nop});
  m.call();
  CHECK(m.cpu.gpr[t0].u64[0] == 0x1111);
  CHECK(m.cpu.gpr[t0].u64[1] == 0x2222);
  CHECK(m.mem.read<uint64_t>(data + 16 + 8) == 0x2222);

  Machine bad;
  bad.cpu.r(a0) = data + 2;
  bad.load({lw(t0, 0, a0), jr(ra), nop});
  CHECK_THROWS_AS(bad.call(), GuestTrap);
}

TEST_CASE("delay slots, likely branches and links") {
  Machine m;
  // sum = 1 + 2 + ... + 10 using a bne loop whose delay slot does the add.
  m.load({addiu(t0, zero, 10), addiu(v0, zero, 0),
          /*loop:*/ addiu(t0, t0, -1), bne(t0, zero, -2), addu(v0, v0, t0),  // adds t0 after decrement
          addiu(v0, v0, 10),                                                 // + the first 10
          // likely not taken: slot annulled
          beql(zero, v0, 2), addiu(v1, zero, 99),
          // bgezal: link visible in the delay slot
          bgezal(zero, 2), or_(a3, ra, zero), nop,
          /*target:*/ jr(s7), nop});
  m.cpu.r(s7) = kRet;
  m.call();
  CHECK(m.s(v0) == 55);
  CHECK(m.s(v1) == 0);
  CHECK(m.cpu.r(a3) == kBase + 8 * 4 + 8);  // bgezal at index 8
}

TEST_CASE("calls, call hook, and jalr link register") {
  Machine m;
  const uint32_t callee = kBase + 0x100;
  m.load(
      {addiu(sp, sp, -16), sw(ra, 0, sp), jal(callee), addiu(a0, zero, 20), lw(ra, 0, sp), jr(ra), addiu(sp, sp, 16)});
  m.load({jr(ra), addiu(v0, a0, 1)}, callee);
  m.call();
  CHECK(m.s(v0) == 21);

  Machine h;
  h.load({addiu(sp, sp, -16), sw(ra, 0, sp), jalr(t9, t1), nop, lw(ra, 0, sp), jr(ra), addiu(sp, sp, 16)});
  h.cpu.r(t1) = 0x00300000;
  uint32_t hooked = 0;
  h.interp.set_call_hook([&](Cpu& c, Memory&, uint32_t target) {
    hooked = target;
    c.r(v0) = 1234;
    return true;
  });
  h.call();
  CHECK(hooked == 0x00300000);
  CHECK(h.s(v0) == 1234);
  CHECK(h.cpu.r(t9) == kBase + 16);
}

TEST_CASE("branch in a delay slot and invalid code trap loudly") {
  Machine m;
  m.load({beq(zero, zero, 2), beq(zero, zero, 2), nop, jr(ra), nop});
  CHECK_THROWS_AS(m.call(), GuestTrap);
  Machine n;
  n.load({0xC0000000u /* ll: not on R5900 */});
  CHECK_THROWS_AS(n.call(), GuestTrap);
}

TEST_CASE("PS2 FPU: clamping, divide by zero, truncation, compare+branch") {
  Machine m;
  m.cpu.r(a0) = fbits(3e38f);
  m.cpu.r(a1) = fbits(-7.9f);
  m.cpu.r(a2) = fbits(1.0f);
  m.load({mtc1(a0, 0), mtc1(a1, 1), mtc1(a2, 2), mtc1(zero, 3), mul_s(4, 0, 0),  // overflow -> +FLT_MAX
          div_s(5, 2, 3),                                                        // 1/0 -> +FLT_MAX
          cvt_w_s(6, 1),                                                         // -7.9 -> -7 (truncate)
          c_lt_s(1, 2), bc1t(2), nop,  // -7.9 < 1 -> taken, skips the next insn
          addiu(v1, zero, 1), mfc1(t0, 4), mfc1(t1, 5), mfc1(t2, 6), jr(ra), nop});
  m.call();
  CHECK(m.cpu.r(t0) == 0x7F7FFFFFull);
  CHECK(m.cpu.r(t1) == 0x7F7FFFFFull);
  CHECK(m.s(t2) == -7);
  CHECK(m.s(v1) == 0);
}

TEST_CASE("MMI lanes and QFSRV funnel shift") {
  Machine m;
  m.cpu.gpr[a0].u32[0] = 1;
  m.cpu.gpr[a0].u32[3] = 0xFFFFFFFF;
  m.cpu.gpr[a1].u32[0] = 2;
  m.cpu.gpr[a1].u32[3] = 1;
  m.cpu.gpr[a2].u64[0] = 0x8877665544332211ull;
  m.cpu.gpr[a2].u64[1] = 0x00FFEEDDCCBBAA99ull;
  m.cpu.gpr[a3].u64[0] = 0x0123456789ABCDEFull;
  m.load({paddw(t0, a0, a1), pextlw(t1, a0, a1), mtsab(zero, 3), qfsrv(t2, a3, a2), jr(ra), nop});
  m.call();
  CHECK(m.cpu.gpr[t0].u32[0] == 3);
  CHECK(m.cpu.gpr[t0].u32[3] == 0);  // wraps
  CHECK(m.cpu.gpr[t1].u32[0] == 2);  // rt lane 0
  CHECK(m.cpu.gpr[t1].u32[1] == 1);  // rs lane 0
  // rs:rt shifted right by 3 bytes: low 8 bytes are rt bytes 3..10.
  CHECK(m.cpu.gpr[t2].u64[0] == 0xBBAA998877665544ull);
}
