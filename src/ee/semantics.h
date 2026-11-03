#pragma once

// R5900 instruction semantics shared verbatim by the reference interpreter and recompiled code.
// Every non-control-flow operation is `void NAME(Cpu&, Memory&, uint32_t word, uint32_t pc)`;
// recompiled code passes a constant `word`, so field extraction folds away at compile time.
// Branch conditions are `bool NAME(const Cpu&, uint32_t word)`.
//
// Conventions (R5900): non-MMI ops write only the low 64 bits of a GPR; 32-bit results are
// sign-extended to 64 bits; $zero is never written. FPU follows PS2 rules approximately:
// no infinities/NaNs (results clamp to +-FLT_MAX), denormals flush to zero.

#include <algorithm>
#include <array>
#include <bit>
#include <climits>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <type_traits>

#include "ee/cpu.h"
#include "ee/memory.h"

namespace lantern::ee::sem {

// ---- fields --------------------------------------------------------------------------------
constexpr unsigned rs(uint32_t w) { return (w >> 21) & 31; }
constexpr unsigned rt(uint32_t w) { return (w >> 16) & 31; }
constexpr unsigned rd(uint32_t w) { return (w >> 11) & 31; }
constexpr unsigned sa(uint32_t w) { return (w >> 6) & 31; }
constexpr int64_t simm(uint32_t w) { return static_cast<int16_t>(w & 0xFFFF); }
constexpr uint64_t uimm(uint32_t w) { return w & 0xFFFF; }
constexpr uint64_t sx32(uint32_t v) { return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(v))); }
constexpr unsigned fs(uint32_t w) { return rd(w); }
constexpr unsigned ft(uint32_t w) { return rt(w); }
constexpr unsigned fd(uint32_t w) { return sa(w); }

inline uint64_t R(const Cpu& c, unsigned r) { return c.gpr[r].u64[0]; }
inline uint32_t R32(const Cpu& c, unsigned r) { return c.gpr[r].u32[0]; }
inline void set(Cpu& c, unsigned r, uint64_t v) {
  if (r) c.gpr[r].u64[0] = v;
}
inline void set128(Cpu& c, unsigned r, const Reg128& v) {
  if (r) c.gpr[r] = v;
}
inline uint32_t addr(const Cpu& c, uint32_t w) { return static_cast<uint32_t>(R(c, rs(w)) + simm(w)); }

[[noreturn]] inline void trap(uint32_t pc, const char* what, uint32_t w) {
  throw GuestTrap(pc, std::format("{} (insn 0x{:08x} at 0x{:08x})", what, w, pc));
}
inline void align(uint32_t a, uint32_t n, uint32_t pc, uint32_t w) {
  if (a & (n - 1)) trap(pc, "unaligned memory access (address error)", w);
}

#define LANTERN_SEM(name)                                                                            \
  inline void name([[maybe_unused]] Cpu& c, [[maybe_unused]] Memory& m, [[maybe_unused]] uint32_t w, \
                   [[maybe_unused]] uint32_t pc)

// ---- shifts ----------------------------------------------------------------------------------
LANTERN_SEM(SLL) { set(c, rd(w), sx32(R32(c, rt(w)) << sa(w))); }
LANTERN_SEM(SRL) { set(c, rd(w), sx32(R32(c, rt(w)) >> sa(w))); }
LANTERN_SEM(SRA) { set(c, rd(w), sx32(static_cast<uint32_t>(static_cast<int32_t>(R32(c, rt(w))) >> sa(w)))); }
LANTERN_SEM(SLLV) { set(c, rd(w), sx32(R32(c, rt(w)) << (R32(c, rs(w)) & 31))); }
LANTERN_SEM(SRLV) { set(c, rd(w), sx32(R32(c, rt(w)) >> (R32(c, rs(w)) & 31))); }
LANTERN_SEM(SRAV) {
  set(c, rd(w), sx32(static_cast<uint32_t>(static_cast<int32_t>(R32(c, rt(w))) >> (R32(c, rs(w)) & 31))));
}
LANTERN_SEM(DSLLV) { set(c, rd(w), R(c, rt(w)) << (R(c, rs(w)) & 63)); }
LANTERN_SEM(DSRLV) { set(c, rd(w), R(c, rt(w)) >> (R(c, rs(w)) & 63)); }
LANTERN_SEM(DSRAV) { set(c, rd(w), static_cast<uint64_t>(static_cast<int64_t>(R(c, rt(w))) >> (R(c, rs(w)) & 63))); }
LANTERN_SEM(DSLL) { set(c, rd(w), R(c, rt(w)) << sa(w)); }
LANTERN_SEM(DSRL) { set(c, rd(w), R(c, rt(w)) >> sa(w)); }
LANTERN_SEM(DSRA) { set(c, rd(w), static_cast<uint64_t>(static_cast<int64_t>(R(c, rt(w))) >> sa(w))); }
LANTERN_SEM(DSLL32) { set(c, rd(w), R(c, rt(w)) << (sa(w) + 32)); }
LANTERN_SEM(DSRL32) { set(c, rd(w), R(c, rt(w)) >> (sa(w) + 32)); }
LANTERN_SEM(DSRA32) { set(c, rd(w), static_cast<uint64_t>(static_cast<int64_t>(R(c, rt(w))) >> (sa(w) + 32))); }

// ---- ALU -------------------------------------------------------------------------------------
LANTERN_SEM(ADD) {
  int32_t r;
  if (__builtin_add_overflow(static_cast<int32_t>(R32(c, rs(w))), static_cast<int32_t>(R32(c, rt(w))), &r))
    trap(pc, "integer overflow exception (ADD)", w);
  set(c, rd(w), sx32(static_cast<uint32_t>(r)));
}
LANTERN_SEM(ADDU) { set(c, rd(w), sx32(R32(c, rs(w)) + R32(c, rt(w)))); }
LANTERN_SEM(SUB) {
  int32_t r;
  if (__builtin_sub_overflow(static_cast<int32_t>(R32(c, rs(w))), static_cast<int32_t>(R32(c, rt(w))), &r))
    trap(pc, "integer overflow exception (SUB)", w);
  set(c, rd(w), sx32(static_cast<uint32_t>(r)));
}
LANTERN_SEM(SUBU) { set(c, rd(w), sx32(R32(c, rs(w)) - R32(c, rt(w)))); }
LANTERN_SEM(DADD) {
  int64_t r;
  if (__builtin_add_overflow(static_cast<int64_t>(R(c, rs(w))), static_cast<int64_t>(R(c, rt(w))), &r))
    trap(pc, "integer overflow exception (DADD)", w);
  set(c, rd(w), static_cast<uint64_t>(r));
}
LANTERN_SEM(DADDU) { set(c, rd(w), R(c, rs(w)) + R(c, rt(w))); }
LANTERN_SEM(DSUB) {
  int64_t r;
  if (__builtin_sub_overflow(static_cast<int64_t>(R(c, rs(w))), static_cast<int64_t>(R(c, rt(w))), &r))
    trap(pc, "integer overflow exception (DSUB)", w);
  set(c, rd(w), static_cast<uint64_t>(r));
}
LANTERN_SEM(DSUBU) { set(c, rd(w), R(c, rs(w)) - R(c, rt(w))); }
LANTERN_SEM(AND) { set(c, rd(w), R(c, rs(w)) & R(c, rt(w))); }
LANTERN_SEM(OR) { set(c, rd(w), R(c, rs(w)) | R(c, rt(w))); }
LANTERN_SEM(XOR) { set(c, rd(w), R(c, rs(w)) ^ R(c, rt(w))); }
LANTERN_SEM(NOR) { set(c, rd(w), ~(R(c, rs(w)) | R(c, rt(w)))); }
LANTERN_SEM(SLT) { set(c, rd(w), static_cast<int64_t>(R(c, rs(w))) < static_cast<int64_t>(R(c, rt(w))) ? 1 : 0); }
LANTERN_SEM(SLTU) { set(c, rd(w), R(c, rs(w)) < R(c, rt(w)) ? 1 : 0); }
LANTERN_SEM(MOVZ) {
  if (R(c, rt(w)) == 0) set(c, rd(w), R(c, rs(w)));
}
LANTERN_SEM(MOVN) {
  if (R(c, rt(w)) != 0) set(c, rd(w), R(c, rs(w)));
}

LANTERN_SEM(ADDI) {
  int32_t r;
  if (__builtin_add_overflow(static_cast<int32_t>(R32(c, rs(w))), static_cast<int32_t>(simm(w)), &r))
    trap(pc, "integer overflow exception (ADDI)", w);
  set(c, rt(w), sx32(static_cast<uint32_t>(r)));
}
LANTERN_SEM(ADDIU) { set(c, rt(w), sx32(R32(c, rs(w)) + static_cast<uint32_t>(simm(w)))); }
LANTERN_SEM(DADDI) {
  int64_t r;
  if (__builtin_add_overflow(static_cast<int64_t>(R(c, rs(w))), simm(w), &r))
    trap(pc, "integer overflow exception (DADDI)", w);
  set(c, rt(w), static_cast<uint64_t>(r));
}
LANTERN_SEM(DADDIU) { set(c, rt(w), R(c, rs(w)) + static_cast<uint64_t>(simm(w))); }
LANTERN_SEM(SLTI) { set(c, rt(w), static_cast<int64_t>(R(c, rs(w))) < simm(w) ? 1 : 0); }
LANTERN_SEM(SLTIU) { set(c, rt(w), R(c, rs(w)) < static_cast<uint64_t>(simm(w)) ? 1 : 0); }
LANTERN_SEM(ANDI) { set(c, rt(w), R(c, rs(w)) & uimm(w)); }
LANTERN_SEM(ORI) { set(c, rt(w), R(c, rs(w)) | uimm(w)); }
LANTERN_SEM(XORI) { set(c, rt(w), R(c, rs(w)) ^ uimm(w)); }
LANTERN_SEM(LUI) { set(c, rt(w), sx32(static_cast<uint32_t>(uimm(w) << 16))); }

// ---- HI/LO, multiply, divide (lane 0 = normal pipeline, lane 1 = MULT1/DIV1) -----------------
inline void put_hilo(Cpu& c, int lane, uint32_t lo, uint32_t hi) {
  c.lo.u64[lane] = sx32(lo);
  c.hi.u64[lane] = sx32(hi);
}
inline void mult_s(Cpu& c, uint32_t w, int lane) {
  int64_t p = static_cast<int64_t>(static_cast<int32_t>(R32(c, rs(w)))) * static_cast<int32_t>(R32(c, rt(w)));
  put_hilo(c, lane, static_cast<uint32_t>(p), static_cast<uint32_t>(static_cast<uint64_t>(p) >> 32));
  set(c, rd(w), c.lo.u64[lane]);
}
inline void mult_u(Cpu& c, uint32_t w, int lane) {
  uint64_t p = static_cast<uint64_t>(R32(c, rs(w))) * R32(c, rt(w));
  put_hilo(c, lane, static_cast<uint32_t>(p), static_cast<uint32_t>(p >> 32));
  set(c, rd(w), c.lo.u64[lane]);
}
// Division by zero / overflow follow R5900 hardware results (as documented by PCSX2):
// signed x/0: LO = (x < 0) ? 1 : -1, HI = x;  INT_MIN/-1: LO = INT_MIN, HI = 0.
inline void div_s(Cpu& c, uint32_t w, int lane) {
  int32_t n = static_cast<int32_t>(R32(c, rs(w))), d = static_cast<int32_t>(R32(c, rt(w)));
  if (d == 0)
    put_hilo(c, lane, n < 0 ? 1u : 0xFFFFFFFFu, static_cast<uint32_t>(n));
  else if (n == std::numeric_limits<int32_t>::min() && d == -1)
    put_hilo(c, lane, 0x80000000u, 0);
  else
    put_hilo(c, lane, static_cast<uint32_t>(n / d), static_cast<uint32_t>(n % d));
}
inline void div_u(Cpu& c, uint32_t w, int lane) {
  uint32_t n = R32(c, rs(w)), d = R32(c, rt(w));
  if (d == 0)
    put_hilo(c, lane, 0xFFFFFFFFu, n);
  else
    put_hilo(c, lane, n / d, n % d);
}
inline void madd(Cpu& c, uint32_t w, int lane, bool is_signed) {
  uint64_t acc = (c.hi.u64[lane] << 32) | (c.lo.u64[lane] & 0xFFFFFFFFu);
  uint64_t p = is_signed ? static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(R32(c, rs(w)))) *
                                                 static_cast<int32_t>(R32(c, rt(w))))
                         : static_cast<uint64_t>(R32(c, rs(w))) * R32(c, rt(w));
  acc += p;
  put_hilo(c, lane, static_cast<uint32_t>(acc), static_cast<uint32_t>(acc >> 32));
  set(c, rd(w), c.lo.u64[lane]);
}
LANTERN_SEM(MULT) { mult_s(c, w, 0); }
LANTERN_SEM(MULTU) { mult_u(c, w, 0); }
LANTERN_SEM(DIV) { div_s(c, w, 0); }
LANTERN_SEM(DIVU) { div_u(c, w, 0); }
LANTERN_SEM(MULT1) { mult_s(c, w, 1); }
LANTERN_SEM(MULTU1) { mult_u(c, w, 1); }
LANTERN_SEM(DIV1) { div_s(c, w, 1); }
LANTERN_SEM(DIVU1) { div_u(c, w, 1); }
LANTERN_SEM(MADD) { madd(c, w, 0, true); }
LANTERN_SEM(MADDU) { madd(c, w, 0, false); }
LANTERN_SEM(MADD1) { madd(c, w, 1, true); }
LANTERN_SEM(MADDU1) { madd(c, w, 1, false); }
LANTERN_SEM(MFHI) { set(c, rd(w), c.hi.u64[0]); }
LANTERN_SEM(MFLO) { set(c, rd(w), c.lo.u64[0]); }
LANTERN_SEM(MTHI) { c.hi.u64[0] = R(c, rs(w)); }
LANTERN_SEM(MTLO) { c.lo.u64[0] = R(c, rs(w)); }
LANTERN_SEM(MFHI1) { set(c, rd(w), c.hi.u64[1]); }
LANTERN_SEM(MFLO1) { set(c, rd(w), c.lo.u64[1]); }
LANTERN_SEM(MTHI1) { c.hi.u64[1] = R(c, rs(w)); }
LANTERN_SEM(MTLO1) { c.lo.u64[1] = R(c, rs(w)); }
LANTERN_SEM(MFSA) { set(c, rd(w), c.sa); }
LANTERN_SEM(MTSA) { c.sa = R32(c, rs(w)) & 0xFF; }
LANTERN_SEM(MTSAB) { c.sa = ((R32(c, rs(w)) ^ static_cast<uint32_t>(uimm(w))) & 15) * 8; }
LANTERN_SEM(MTSAH) { c.sa = ((R32(c, rs(w)) ^ static_cast<uint32_t>(uimm(w))) & 7) * 16; }

// ---- traps / system --------------------------------------------------------------------------
#define LANTERN_TRAP_IF(name, cond)                              \
  LANTERN_SEM(name) {                                            \
    if (cond) trap(pc, "trap instruction (" #name ") fired", w); \
  }
LANTERN_TRAP_IF(TGE, static_cast<int64_t>(R(c, rs(w))) >= static_cast<int64_t>(R(c, rt(w))))
LANTERN_TRAP_IF(TGEU, R(c, rs(w)) >= R(c, rt(w)))
LANTERN_TRAP_IF(TLT, static_cast<int64_t>(R(c, rs(w))) < static_cast<int64_t>(R(c, rt(w))))
LANTERN_TRAP_IF(TLTU, R(c, rs(w)) < R(c, rt(w)))
LANTERN_TRAP_IF(TEQ, R(c, rs(w)) == R(c, rt(w)))
LANTERN_TRAP_IF(TNE, R(c, rs(w)) != R(c, rt(w)))
LANTERN_TRAP_IF(TGEI, static_cast<int64_t>(R(c, rs(w))) >= simm(w))
LANTERN_TRAP_IF(TGEIU, R(c, rs(w)) >= static_cast<uint64_t>(simm(w)))
LANTERN_TRAP_IF(TLTI, static_cast<int64_t>(R(c, rs(w))) < simm(w))
LANTERN_TRAP_IF(TLTIU, R(c, rs(w)) < static_cast<uint64_t>(simm(w)))
LANTERN_TRAP_IF(TEQI, static_cast<int64_t>(R(c, rs(w))) == simm(w))
LANTERN_TRAP_IF(TNEI, static_cast<int64_t>(R(c, rs(w))) != simm(w))
#undef LANTERN_TRAP_IF

LANTERN_SEM(SYSCALL) {
  if (!c.hooks) trap(pc, "SYSCALL with no kernel attached", w);
  c.hooks->syscall(c, m, pc);
}
LANTERN_SEM(BREAK) {
  if (!c.hooks) trap(pc, "BREAK", w);
  c.hooks->breakpoint(c, m, pc, (w >> 6) & 0xFFFFF);
}
LANTERN_SEM(SYNC) {}
LANTERN_SEM(CACHE) {}  // data/instruction cache maintenance has no effect on host memory
LANTERN_SEM(PREF) {}

// ---- loads / stores --------------------------------------------------------------------------
LANTERN_SEM(LB) { set(c, rt(w), static_cast<uint64_t>(static_cast<int8_t>(m.read<uint8_t>(addr(c, w))))); }
LANTERN_SEM(LBU) { set(c, rt(w), m.read<uint8_t>(addr(c, w))); }
LANTERN_SEM(LH) {
  uint32_t a = addr(c, w);
  align(a, 2, pc, w);
  set(c, rt(w), static_cast<uint64_t>(static_cast<int16_t>(m.read<uint16_t>(a))));
}
LANTERN_SEM(LHU) {
  uint32_t a = addr(c, w);
  align(a, 2, pc, w);
  set(c, rt(w), m.read<uint16_t>(a));
}
LANTERN_SEM(LW) {
  uint32_t a = addr(c, w);
  align(a, 4, pc, w);
  set(c, rt(w), sx32(m.read<uint32_t>(a)));
}
LANTERN_SEM(LWU) {
  uint32_t a = addr(c, w);
  align(a, 4, pc, w);
  set(c, rt(w), m.read<uint32_t>(a));
}
LANTERN_SEM(LD) {
  uint32_t a = addr(c, w);
  align(a, 8, pc, w);
  set(c, rt(w), m.read<uint64_t>(a));
}
LANTERN_SEM(LQ) {  // R5900 ignores the low 4 address bits
  Reg128 v;
  m.read128(addr(c, w) & ~15u, v);
  set128(c, rt(w), v);
}
LANTERN_SEM(SB) { m.write<uint8_t>(addr(c, w), static_cast<uint8_t>(R(c, rt(w)))); }
LANTERN_SEM(SH) {
  uint32_t a = addr(c, w);
  align(a, 2, pc, w);
  m.write<uint16_t>(a, static_cast<uint16_t>(R(c, rt(w))));
}
LANTERN_SEM(SW) {
  uint32_t a = addr(c, w);
  align(a, 4, pc, w);
  m.write<uint32_t>(a, R32(c, rt(w)));
}
LANTERN_SEM(SD) {
  uint32_t a = addr(c, w);
  align(a, 8, pc, w);
  m.write<uint64_t>(a, R(c, rt(w)));
}
LANTERN_SEM(SQ) { m.write128(addr(c, w) & ~15u, c.gpr[rt(w)]); }

// Unaligned word/doubleword access, little-endian.
LANTERN_SEM(LWL) {
  uint32_t a = addr(c, w), s = a & 3;
  uint32_t mem = m.read<uint32_t>(a & ~3u);
  static constexpr uint32_t kMask[4] = {0x00FFFFFF, 0x0000FFFF, 0x000000FF, 0x00000000};
  set(c, rt(w), sx32((R32(c, rt(w)) & kMask[s]) | (mem << (24 - 8 * s))));
}
LANTERN_SEM(LWR) {
  uint32_t a = addr(c, w), s = a & 3;
  uint32_t mem = m.read<uint32_t>(a & ~3u);
  if (s == 0) {
    set(c, rt(w), sx32(mem));
  } else if (rt(w)) {
    static constexpr uint32_t kMask[4] = {0, 0xFF000000, 0xFFFF0000, 0xFFFFFF00};
    c.gpr[rt(w)].u32[0] = (R32(c, rt(w)) & kMask[s]) | (mem >> (8 * s));  // upper 32 bits unchanged
  }
}
LANTERN_SEM(SWL) {
  uint32_t a = addr(c, w), s = a & 3;
  uint32_t mem = m.read<uint32_t>(a & ~3u);
  static constexpr uint32_t kMask[4] = {0xFFFFFF00, 0xFFFF0000, 0xFF000000, 0x00000000};
  m.write<uint32_t>(a & ~3u, (R32(c, rt(w)) >> (24 - 8 * s)) | (mem & kMask[s]));
}
LANTERN_SEM(SWR) {
  uint32_t a = addr(c, w), s = a & 3;
  uint32_t mem = m.read<uint32_t>(a & ~3u);
  static constexpr uint32_t kMask[4] = {0x00000000, 0x000000FF, 0x0000FFFF, 0x00FFFFFF};
  m.write<uint32_t>(a & ~3u, (R32(c, rt(w)) << (8 * s)) | (mem & kMask[s]));
}
LANTERN_SEM(LDL) {
  uint32_t a = addr(c, w), s = a & 7;
  uint64_t mem = m.read<uint64_t>(a & ~7u);
  uint64_t keep = s == 7 ? 0 : (~0ull >> (8 * (s + 1)));
  set(c, rt(w), (R(c, rt(w)) & keep) | (mem << (56 - 8 * s)));
}
LANTERN_SEM(LDR) {
  uint32_t a = addr(c, w), s = a & 7;
  uint64_t mem = m.read<uint64_t>(a & ~7u);
  uint64_t keep = s == 0 ? 0 : (~0ull << (64 - 8 * s));
  set(c, rt(w), (R(c, rt(w)) & keep) | (mem >> (8 * s)));
}
LANTERN_SEM(SDL) {
  uint32_t a = addr(c, w), s = a & 7;
  uint64_t mem = m.read<uint64_t>(a & ~7u);
  uint64_t keep = s == 7 ? 0 : (~0ull << (8 * (s + 1)));
  m.write<uint64_t>(a & ~7u, (R(c, rt(w)) >> (56 - 8 * s)) | (mem & keep));
}
LANTERN_SEM(SDR) {
  uint32_t a = addr(c, w), s = a & 7;
  uint64_t mem = m.read<uint64_t>(a & ~7u);
  uint64_t keep = s == 0 ? 0 : (~0ull >> (64 - 8 * s));
  m.write<uint64_t>(a & ~7u, (R(c, rt(w)) << (8 * s)) | (mem & keep));
}

// ---- COP0 ------------------------------------------------------------------------------------
LANTERN_SEM(MFC0) { set(c, rt(w), sx32(c.cop0[rd(w)])); }
LANTERN_SEM(MTC0) { c.cop0[rd(w)] = R32(c, rt(w)); }
LANTERN_SEM(EI) { c.cop0[12] |= 1u << 16; }  // Status.EIE
LANTERN_SEM(DI) { c.cop0[12] &= ~(1u << 16); }

// ---- COP1 (PS2 FPU) --------------------------------------------------------------------------
constexpr uint32_t kFltMax = 0x7F7FFFFF;
inline float f_in(uint32_t bits) {
  uint32_t e = (bits >> 23) & 0xFF;
  if (e == 0) return std::bit_cast<float>(bits & 0x80000000u);                 // denormal -> +-0
  if (e == 0xFF) return std::bit_cast<float>((bits & 0x80000000u) | kFltMax);  // no inf/NaN on PS2
  return std::bit_cast<float>(bits);
}
inline uint32_t f_out(float f) {
  uint32_t bits = std::bit_cast<uint32_t>(f);
  uint32_t e = (bits >> 23) & 0xFF;
  if (e == 0xFF) return (bits & 0x80000000u) | kFltMax;  // overflow clamps
  if (e == 0) return bits & 0x80000000u;                 // underflow flushes
  return bits;
}
inline float F(const Cpu& c, unsigned r) { return f_in(c.fpr[r]); }
constexpr uint32_t kFpuCond = 1u << 23;

LANTERN_SEM(MFC1) { set(c, rt(w), sx32(c.fpr[fs(w)])); }
LANTERN_SEM(MTC1) { c.fpr[fs(w)] = R32(c, rt(w)); }
LANTERN_SEM(CFC1) { set(c, rt(w), sx32(fs(w) == 31 ? c.fcr31 : fs(w) == 0 ? 0x2E30u : 0u)); }
LANTERN_SEM(CTC1) {
  if (fs(w) == 31) c.fcr31 = R32(c, rt(w));
}
LANTERN_SEM(LWC1) {
  uint32_t a = addr(c, w);
  align(a, 4, pc, w);
  c.fpr[ft(w)] = m.read<uint32_t>(a);
}
LANTERN_SEM(SWC1) {
  uint32_t a = addr(c, w);
  align(a, 4, pc, w);
  m.write<uint32_t>(a, c.fpr[ft(w)]);
}
LANTERN_SEM(ADD_S) { c.fpr[fd(w)] = f_out(F(c, fs(w)) + F(c, ft(w))); }
LANTERN_SEM(SUB_S) { c.fpr[fd(w)] = f_out(F(c, fs(w)) - F(c, ft(w))); }
LANTERN_SEM(MUL_S) { c.fpr[fd(w)] = f_out(F(c, fs(w)) * F(c, ft(w))); }
LANTERN_SEM(DIV_S) {
  float d = F(c, ft(w));
  if (d == 0.0f)
    c.fpr[fd(w)] = ((c.fpr[fs(w)] ^ c.fpr[ft(w)]) & 0x80000000u) | kFltMax;
  else
    c.fpr[fd(w)] = f_out(F(c, fs(w)) / d);
}
LANTERN_SEM(SQRT_S) { c.fpr[fd(w)] = f_out(std::sqrt(std::fabs(F(c, ft(w))))); }
LANTERN_SEM(RSQRT_S) {
  float d = std::sqrt(std::fabs(F(c, ft(w))));
  if (d == 0.0f)
    c.fpr[fd(w)] = (c.fpr[fs(w)] & 0x80000000u) | kFltMax;
  else
    c.fpr[fd(w)] = f_out(F(c, fs(w)) / d);
}
LANTERN_SEM(ABS_S) { c.fpr[fd(w)] = c.fpr[fs(w)] & 0x7FFFFFFFu; }
LANTERN_SEM(MOV_S) { c.fpr[fd(w)] = c.fpr[fs(w)]; }
LANTERN_SEM(NEG_S) { c.fpr[fd(w)] = c.fpr[fs(w)] ^ 0x80000000u; }
LANTERN_SEM(ADDA_S) { c.acc = f_out(F(c, fs(w)) + F(c, ft(w))); }
LANTERN_SEM(SUBA_S) { c.acc = f_out(F(c, fs(w)) - F(c, ft(w))); }
LANTERN_SEM(MULA_S) { c.acc = f_out(F(c, fs(w)) * F(c, ft(w))); }
LANTERN_SEM(MADD_S) { c.fpr[fd(w)] = f_out(f_in(c.acc) + F(c, fs(w)) * F(c, ft(w))); }
LANTERN_SEM(MSUB_S) { c.fpr[fd(w)] = f_out(f_in(c.acc) - F(c, fs(w)) * F(c, ft(w))); }
LANTERN_SEM(MADDA_S) { c.acc = f_out(f_in(c.acc) + F(c, fs(w)) * F(c, ft(w))); }
LANTERN_SEM(MSUBA_S) { c.acc = f_out(f_in(c.acc) - F(c, fs(w)) * F(c, ft(w))); }
LANTERN_SEM(MAX_S) { c.fpr[fd(w)] = F(c, fs(w)) >= F(c, ft(w)) ? c.fpr[fs(w)] : c.fpr[ft(w)]; }
LANTERN_SEM(MIN_S) { c.fpr[fd(w)] = F(c, fs(w)) <= F(c, ft(w)) ? c.fpr[fs(w)] : c.fpr[ft(w)]; }
LANTERN_SEM(CVT_W_S) {  // R5900: always truncates; saturates out-of-range values
  float f = F(c, fs(w));
  int32_t v;
  if (f >= 2147483648.0f)
    v = std::numeric_limits<int32_t>::max();
  else if (f <= -2147483648.0f)
    v = std::numeric_limits<int32_t>::min();
  else
    v = static_cast<int32_t>(f);
  c.fpr[fd(w)] = static_cast<uint32_t>(v);
}
LANTERN_SEM(CVT_S_W) { c.fpr[fd(w)] = f_out(static_cast<float>(static_cast<int32_t>(c.fpr[fs(w)]))); }
inline void fcond(Cpu& c, bool v) { c.fcr31 = v ? (c.fcr31 | kFpuCond) : (c.fcr31 & ~kFpuCond); }
LANTERN_SEM(C_F_S) { fcond(c, false); }
LANTERN_SEM(C_EQ_S) { fcond(c, F(c, fs(w)) == F(c, ft(w))); }
LANTERN_SEM(C_LT_S) { fcond(c, F(c, fs(w)) < F(c, ft(w))); }
LANTERN_SEM(C_LE_S) { fcond(c, F(c, fs(w)) <= F(c, ft(w))); }

// ---- MMI (128-bit integer SIMD), common subset -----------------------------------------------
template <typename Fn>
inline void lanes(Cpu& c, uint32_t w, Fn fn) {
  Reg128 out;
  fn(c.gpr[rs(w)], c.gpr[rt(w)], out);
  set128(c, rd(w), out);
}
LANTERN_SEM(PAND) {
  lanes(c, w, [](const Reg128& a, const Reg128& b, Reg128& o) {
    o.u64[0] = a.u64[0] & b.u64[0];
    o.u64[1] = a.u64[1] & b.u64[1];
  });
}
LANTERN_SEM(POR) {
  lanes(c, w, [](const Reg128& a, const Reg128& b, Reg128& o) {
    o.u64[0] = a.u64[0] | b.u64[0];
    o.u64[1] = a.u64[1] | b.u64[1];
  });
}
LANTERN_SEM(PXOR) {
  lanes(c, w, [](const Reg128& a, const Reg128& b, Reg128& o) {
    o.u64[0] = a.u64[0] ^ b.u64[0];
    o.u64[1] = a.u64[1] ^ b.u64[1];
  });
}
LANTERN_SEM(PNOR) {
  lanes(c, w, [](const Reg128& a, const Reg128& b, Reg128& o) {
    o.u64[0] = ~(a.u64[0] | b.u64[0]);
    o.u64[1] = ~(a.u64[1] | b.u64[1]);
  });
}
LANTERN_SEM(PCPYLD) {
  lanes(c, w, [](const Reg128& a, const Reg128& b, Reg128& o) {
    o.u64[0] = b.u64[0];
    o.u64[1] = a.u64[0];
  });
}
LANTERN_SEM(PCPYUD) {
  lanes(c, w, [](const Reg128& a, const Reg128& b, Reg128& o) {
    o.u64[0] = a.u64[1];
    o.u64[1] = b.u64[1];
  });
}
LANTERN_SEM(PCPYH) {
  lanes(c, w, [](const Reg128&, const Reg128& b, Reg128& o) {
    for (int i = 0; i < 4; ++i) o.u16[i] = b.u16[0];
    for (int i = 4; i < 8; ++i) o.u16[i] = b.u16[4];
  });
}
LANTERN_SEM(PEXTLW) {
  lanes(c, w, [](const Reg128& a, const Reg128& b, Reg128& o) {
    o.u32[0] = b.u32[0];
    o.u32[1] = a.u32[0];
    o.u32[2] = b.u32[1];
    o.u32[3] = a.u32[1];
  });
}
LANTERN_SEM(PEXTUW) {
  lanes(c, w, [](const Reg128& a, const Reg128& b, Reg128& o) {
    o.u32[0] = b.u32[2];
    o.u32[1] = a.u32[2];
    o.u32[2] = b.u32[3];
    o.u32[3] = a.u32[3];
  });
}
LANTERN_SEM(PEXTLH) {
  lanes(c, w, [](const Reg128& a, const Reg128& b, Reg128& o) {
    for (int i = 0; i < 4; ++i) {
      o.u16[2 * i] = b.u16[i];
      o.u16[2 * i + 1] = a.u16[i];
    }
  });
}
LANTERN_SEM(PEXTLB) {
  lanes(c, w, [](const Reg128& a, const Reg128& b, Reg128& o) {
    for (int i = 0; i < 8; ++i) {
      o.u8[2 * i] = b.u8[i];
      o.u8[2 * i + 1] = a.u8[i];
    }
  });
}
LANTERN_SEM(PPACW) {
  lanes(c, w, [](const Reg128& a, const Reg128& b, Reg128& o) {
    o.u32[0] = b.u32[0];
    o.u32[1] = b.u32[2];
    o.u32[2] = a.u32[0];
    o.u32[3] = a.u32[2];
  });
}
#define LANTERN_PLANE(name, field, n, expr)                                                                      \
  LANTERN_SEM(name) {                                                                                            \
    lanes(c, w, [](const Reg128& a, const Reg128& b, Reg128& o) {                                                \
      for (int i = 0; i < n; ++i) o.field[i] = static_cast<std::remove_reference_t<decltype(o.field[0])>>(expr); \
    });                                                                                                          \
  }
LANTERN_PLANE(PADDW, u32, 4, a.u32[i] + b.u32[i])
LANTERN_PLANE(PSUBW, u32, 4, a.u32[i] - b.u32[i])
LANTERN_PLANE(PADDH, u16, 8, a.u16[i] + b.u16[i])
LANTERN_PLANE(PSUBH, u16, 8, a.u16[i] - b.u16[i])
LANTERN_PLANE(PADDB, u8, 16, a.u8[i] + b.u8[i])
LANTERN_PLANE(PSUBB, u8, 16, a.u8[i] - b.u8[i])
LANTERN_PLANE(PCEQW, u32, 4, a.u32[i] == b.u32[i] ? 0xFFFFFFFFu : 0u)
LANTERN_PLANE(PCEQH, u16, 8, a.u16[i] == b.u16[i] ? 0xFFFFu : 0u)
LANTERN_PLANE(PCEQB, u8, 16, a.u8[i] == b.u8[i] ? 0xFFu : 0u)
LANTERN_PLANE(PCGTW, u32, 4, a.s32[i] > b.s32[i] ? 0xFFFFFFFFu : 0u)
LANTERN_PLANE(PCGTH, u16, 8, a.s16[i] > b.s16[i] ? 0xFFFFu : 0u)
LANTERN_PLANE(PCGTB, u8, 16, a.s8[i] > b.s8[i] ? 0xFFu : 0u)
LANTERN_PLANE(PMAXW, s32, 4, a.s32[i] > b.s32[i] ? a.s32[i] : b.s32[i])
LANTERN_PLANE(PMINW, s32, 4, a.s32[i] < b.s32[i] ? a.s32[i] : b.s32[i])
LANTERN_PLANE(PMAXH, s16, 8, a.s16[i] > b.s16[i] ? a.s16[i] : b.s16[i])
LANTERN_PLANE(PMINH, s16, 8, a.s16[i] < b.s16[i] ? a.s16[i] : b.s16[i])
#undef LANTERN_PLANE

#define LANTERN_PSHIFT(name, field, n, shift_mask, expr)                                                       \
  LANTERN_SEM(name) {                                                                                          \
    Reg128 b = c.gpr[rt(w)], o;                                                                                \
    const unsigned s = sa(w) & (shift_mask);                                                                   \
    for (int i = 0; i < n; ++i) o.field[i] = static_cast<std::remove_reference_t<decltype(o.field[0])>>(expr); \
    set128(c, rd(w), o);                                                                                       \
  }
LANTERN_PSHIFT(PSLLW, u32, 4, 31, b.u32[i] << s)
LANTERN_PSHIFT(PSRLW, u32, 4, 31, b.u32[i] >> s)
LANTERN_PSHIFT(PSRAW, s32, 4, 31, b.s32[i] >> s)
LANTERN_PSHIFT(PSLLH, u16, 8, 15, b.u16[i] << s)
LANTERN_PSHIFT(PSRLH, u16, 8, 15, b.u16[i] >> s)
LANTERN_PSHIFT(PSRAH, s16, 8, 15, b.s16[i] >> s)
#undef LANTERN_PSHIFT

LANTERN_SEM(PMFHI) { set128(c, rd(w), c.hi); }
LANTERN_SEM(PMFLO) { set128(c, rd(w), c.lo); }
LANTERN_SEM(PMTHI) { c.hi = c.gpr[rs(w)]; }
LANTERN_SEM(PMTLO) { c.lo = c.gpr[rs(w)]; }
LANTERN_SEM(PLZCW) {  // count leading bits equal to the sign bit, minus one, per 32-bit lane
  Reg128 o = c.gpr[rd(w)];
  for (int i = 0; i < 2; ++i) {
    uint32_t v = c.gpr[rs(w)].u32[i];
    if (v & 0x80000000u) v = ~v;
    o.u32[i] = static_cast<uint32_t>((v == 0 ? 32 : std::countl_zero(v)) - 1);
  }
  set128(c, rd(w), o);
}
LANTERN_SEM(QFSRV) {  // funnel shift right of rs:rt by SA bits
  const Reg128 hi = c.gpr[rs(w)], lo = c.gpr[rt(w)];
  const unsigned s = c.sa & 127;
  Reg128 o;
  if (s == 0) {
    o = lo;
  } else {
    // 256-bit value hi:lo shifted right by s (1..127) bits, using 64-bit limbs.
    const uint64_t limb[4] = {lo.u64[0], lo.u64[1], hi.u64[0], hi.u64[1]};
    const unsigned q = s / 64, b = s % 64;
    for (unsigned i = 0; i < 2; ++i) {
      uint64_t x = limb[i + q] >> b;
      if (b && i + q + 1 < 4) x |= limb[i + q + 1] << (64 - b);
      o.u64[i] = x;
    }
  }
  set128(c, rd(w), o);
}

// ---- MMI: saturating arithmetic --------------------------------------------------------------
template <typename T, typename W>
constexpr T sat(W v) {
  return static_cast<T>(
      std::clamp<W>(v, static_cast<W>(std::numeric_limits<T>::min()), static_cast<W>(std::numeric_limits<T>::max())));
}
#define LANTERN_PSAT(name, field, n, T, W, op)                                                                   \
  LANTERN_SEM(name) {                                                                                            \
    lanes(c, w, [](const Reg128& a, const Reg128& b, Reg128& o) {                                                \
      for (int i = 0; i < n; ++i) o.field[i] = sat<T>(static_cast<W>(a.field[i]) op static_cast<W>(b.field[i])); \
    });                                                                                                          \
  }
LANTERN_PSAT(PADDSW, s32, 4, int32_t, int64_t, +)
LANTERN_PSAT(PSUBSW, s32, 4, int32_t, int64_t, -)
LANTERN_PSAT(PADDSH, s16, 8, int16_t, int32_t, +)
LANTERN_PSAT(PSUBSH, s16, 8, int16_t, int32_t, -)
LANTERN_PSAT(PADDSB, s8, 16, int8_t, int32_t, +)
LANTERN_PSAT(PSUBSB, s8, 16, int8_t, int32_t, -)
LANTERN_PSAT(PADDUW, u32, 4, uint32_t, int64_t, +)
LANTERN_PSAT(PSUBUW, u32, 4, uint32_t, int64_t, -)
LANTERN_PSAT(PADDUH, u16, 8, uint16_t, int32_t, +)
LANTERN_PSAT(PSUBUH, u16, 8, uint16_t, int32_t, -)
LANTERN_PSAT(PADDUB, u8, 16, uint8_t, int32_t, +)  // `paddub rd,rs,$zero` is GCC's 128-bit move
LANTERN_PSAT(PSUBUB, u8, 16, uint8_t, int32_t, -)
#undef LANTERN_PSAT

LANTERN_SEM(PABSW) {
  Reg128 b = c.gpr[rt(w)], o;
  for (int i = 0; i < 4; ++i) o.s32[i] = b.s32[i] == INT32_MIN ? INT32_MAX : (b.s32[i] < 0 ? -b.s32[i] : b.s32[i]);
  set128(c, rd(w), o);
}
LANTERN_SEM(PABSH) {
  Reg128 b = c.gpr[rt(w)], o;
  for (int i = 0; i < 8; ++i)
    o.s16[i] = b.s16[i] == INT16_MIN ? INT16_MAX : static_cast<int16_t>(b.s16[i] < 0 ? -b.s16[i] : b.s16[i]);
  set128(c, rd(w), o);
}
LANTERN_SEM(PADSBH) {
  lanes(c, w, [](const Reg128& a, const Reg128& b, Reg128& o) {
    for (int i = 0; i < 4; ++i) o.u16[i] = static_cast<uint16_t>(a.u16[i] - b.u16[i]);
    for (int i = 4; i < 8; ++i) o.u16[i] = static_cast<uint16_t>(a.u16[i] + b.u16[i]);
  });
}

// ---- MMI: pack / interleave / permute ---------------------------------------------------------
LANTERN_SEM(PEXTUH) {
  lanes(c, w, [](const Reg128& a, const Reg128& b, Reg128& o) {
    for (int i = 0; i < 4; ++i) {
      o.u16[2 * i] = b.u16[4 + i];
      o.u16[2 * i + 1] = a.u16[4 + i];
    }
  });
}
LANTERN_SEM(PEXTUB) {
  lanes(c, w, [](const Reg128& a, const Reg128& b, Reg128& o) {
    for (int i = 0; i < 8; ++i) {
      o.u8[2 * i] = b.u8[8 + i];
      o.u8[2 * i + 1] = a.u8[8 + i];
    }
  });
}
LANTERN_SEM(PPACH) {
  lanes(c, w, [](const Reg128& a, const Reg128& b, Reg128& o) {
    for (int i = 0; i < 4; ++i) {
      o.u16[i] = b.u16[2 * i];
      o.u16[4 + i] = a.u16[2 * i];
    }
  });
}
LANTERN_SEM(PPACB) {
  lanes(c, w, [](const Reg128& a, const Reg128& b, Reg128& o) {
    for (int i = 0; i < 8; ++i) {
      o.u8[i] = b.u8[2 * i];
      o.u8[8 + i] = a.u8[2 * i];
    }
  });
}
LANTERN_SEM(PINTH) {
  lanes(c, w, [](const Reg128& a, const Reg128& b, Reg128& o) {
    for (int i = 0; i < 4; ++i) {
      o.u16[2 * i] = b.u16[i];
      o.u16[2 * i + 1] = a.u16[4 + i];
    }
  });
}
LANTERN_SEM(PINTEH) {
  lanes(c, w, [](const Reg128& a, const Reg128& b, Reg128& o) {
    for (int i = 0; i < 4; ++i) {
      o.u16[2 * i] = b.u16[2 * i];
      o.u16[2 * i + 1] = a.u16[2 * i];
    }
  });
}
template <size_t N>
inline void permute16(Cpu& c, uint32_t w, const std::array<int, N>& idx) {
  const Reg128 b = c.gpr[rt(w)];
  Reg128 o;
  for (size_t i = 0; i < N; ++i) {
    if constexpr (N == 8)
      o.u16[i] = b.u16[idx[i]];
    else
      o.u32[i] = b.u32[idx[i]];
  }
  set128(c, rd(w), o);
}
LANTERN_SEM(PEXEH) { permute16<8>(c, w, {2, 1, 0, 3, 6, 5, 4, 7}); }
LANTERN_SEM(PREVH) { permute16<8>(c, w, {3, 2, 1, 0, 7, 6, 5, 4}); }
LANTERN_SEM(PEXCH) { permute16<8>(c, w, {0, 2, 1, 3, 4, 6, 5, 7}); }
LANTERN_SEM(PEXEW) { permute16<4>(c, w, {2, 1, 0, 3}); }
LANTERN_SEM(PEXCW) { permute16<4>(c, w, {0, 2, 1, 3}); }
LANTERN_SEM(PROT3W) { permute16<4>(c, w, {1, 2, 0, 3}); }
LANTERN_SEM(PEXT5) {  // 1:5:5:5 -> 8:8:8:8 per word
  const Reg128 b = c.gpr[rt(w)];
  Reg128 o;
  for (int i = 0; i < 4; ++i) {
    const uint32_t x = b.u32[i];
    o.u32[i] = ((x & 0x1F) << 3) | ((x & 0x3E0) << 6) | ((x & 0x7C00) << 9) | ((x & 0x8000) << 16);
  }
  set128(c, rd(w), o);
}
LANTERN_SEM(PPAC5) {  // 8:8:8:8 -> 1:5:5:5 per word
  const Reg128 b = c.gpr[rt(w)];
  Reg128 o;
  for (int i = 0; i < 4; ++i) {
    const uint32_t x = b.u32[i];
    o.u32[i] = ((x >> 3) & 0x1F) | ((x >> 6) & 0x3E0) | ((x >> 9) & 0x7C00) | ((x >> 16) & 0x8000);
  }
  set128(c, rd(w), o);
}

// ---- MMI: variable word shifts on words 0 and 2 (results sign-extended per doubleword) --------
#define LANTERN_PVSHIFT(name, expr)                  \
  LANTERN_SEM(name) {                                \
    const Reg128 a = c.gpr[rs(w)], b = c.gpr[rt(w)]; \
    Reg128 o;                                        \
    for (int i = 0; i < 2; ++i) {                    \
      const uint32_t x = b.u32[2 * i];               \
      const unsigned s = a.u32[2 * i] & 31;          \
      o.u64[i] = sx32(static_cast<uint32_t>(expr));  \
    }                                                \
    set128(c, rd(w), o);                             \
  }
LANTERN_PVSHIFT(PSLLVW, x << s)
LANTERN_PVSHIFT(PSRLVW, x >> s)
LANTERN_PVSHIFT(PSRAVW, static_cast<int32_t>(x) >> s)
#undef LANTERN_PVSHIFT

// ---- MMI: parallel multiply / divide using both HI/LO pipelines -------------------------------
inline void pmul_w(Cpu& c, uint32_t w, bool is_signed, int acc_sign) {
  const Reg128 a = c.gpr[rs(w)], b = c.gpr[rt(w)];
  Reg128 o;
  for (int i = 0; i < 2; ++i) {
    const int k = 2 * i;
    uint64_t p = is_signed ? static_cast<uint64_t>(static_cast<int64_t>(a.s32[k]) * b.s32[k])
                           : static_cast<uint64_t>(a.u32[k]) * b.u32[k];
    if (acc_sign) {
      const uint64_t acc = static_cast<uint64_t>(c.hi.u32[k]) << 32 | c.lo.u32[k];
      p = acc_sign > 0 ? acc + p : acc - p;
    }
    c.lo.u64[i] = sx32(static_cast<uint32_t>(p));
    c.hi.u64[i] = sx32(static_cast<uint32_t>(p >> 32));
    o.u64[i] = p;
  }
  set128(c, rd(w), o);
}
LANTERN_SEM(PMULTW) { pmul_w(c, w, true, 0); }
LANTERN_SEM(PMULTUW) { pmul_w(c, w, false, 0); }
LANTERN_SEM(PMADDW) { pmul_w(c, w, true, 1); }
LANTERN_SEM(PMSUBW) { pmul_w(c, w, true, -1); }
LANTERN_SEM(PMADDUW) { pmul_w(c, w, false, 1); }
LANTERN_SEM(PDIVW) {
  const Reg128 a = c.gpr[rs(w)], b = c.gpr[rt(w)];
  for (int i = 0; i < 2; ++i) {
    const int32_t n = a.s32[2 * i], d = b.s32[2 * i];
    if (d == 0)
      put_hilo(c, i, n < 0 ? 1u : 0xFFFFFFFFu, static_cast<uint32_t>(n));
    else if (n == INT32_MIN && d == -1)
      put_hilo(c, i, 0x80000000u, 0);
    else
      put_hilo(c, i, static_cast<uint32_t>(n / d), static_cast<uint32_t>(n % d));
  }
}
LANTERN_SEM(PDIVUW) {
  const Reg128 a = c.gpr[rs(w)], b = c.gpr[rt(w)];
  for (int i = 0; i < 2; ++i) {
    const uint32_t n = a.u32[2 * i], d = b.u32[2 * i];
    if (d == 0)
      put_hilo(c, i, 0xFFFFFFFFu, n);
    else
      put_hilo(c, i, n / d, n % d);
  }
}
LANTERN_SEM(PDIVBW) {
  const Reg128 a = c.gpr[rs(w)];
  const int32_t d = c.gpr[rt(w)].s16[0];
  for (int i = 0; i < 4; ++i) {
    const int32_t n = a.s32[i];
    if (d == 0) {
      c.lo.s32[i] = n < 0 ? 1 : -1;
      c.hi.s32[i] = n;
    } else if (n == INT32_MIN && d == -1) {
      c.lo.s32[i] = INT32_MIN;
      c.hi.s32[i] = 0;
    } else {
      c.lo.s32[i] = n / d;
      c.hi.s32[i] = n % d;
    }
  }
}
// Halfword multiplies: products p0..p7 (signed 16x16 -> 32).
inline std::array<int32_t, 8> hprod(const Cpu& c, uint32_t w) {
  std::array<int32_t, 8> p{};
  for (int i = 0; i < 8; ++i) p[i] = static_cast<int32_t>(c.gpr[rs(w)].s16[i]) * c.gpr[rt(w)].s16[i];
  return p;
}
inline void hmul(Cpu& c, uint32_t w, int acc_sign) {
  const auto p = hprod(c, w);
  static constexpr int lo_idx[4] = {0, 1, 4, 5}, hi_idx[4] = {2, 3, 6, 7};
  for (int i = 0; i < 4; ++i) {
    const auto pl = static_cast<uint32_t>(p[lo_idx[i]]), ph = static_cast<uint32_t>(p[hi_idx[i]]);
    c.lo.u32[i] = acc_sign == 0 ? pl : acc_sign > 0 ? c.lo.u32[i] + pl : c.lo.u32[i] - pl;
    c.hi.u32[i] = acc_sign == 0 ? ph : acc_sign > 0 ? c.hi.u32[i] + ph : c.hi.u32[i] - ph;
  }
  Reg128 o;
  o.u32[0] = c.lo.u32[0];
  o.u32[1] = c.hi.u32[0];
  o.u32[2] = c.lo.u32[2];
  o.u32[3] = c.hi.u32[2];
  set128(c, rd(w), o);
}
LANTERN_SEM(PMULTH) { hmul(c, w, 0); }
LANTERN_SEM(PMADDH) { hmul(c, w, 1); }
LANTERN_SEM(PMSUBH) { hmul(c, w, -1); }
inline void hpair(Cpu& c, uint32_t w, bool sub) {
  const auto p = hprod(c, w);
  auto f = [&](int hi_i, int lo_i) { return static_cast<uint32_t>(sub ? p[hi_i] - p[lo_i] : p[hi_i] + p[lo_i]); };
  c.lo.u32[0] = f(1, 0);
  c.hi.u32[0] = f(3, 2);
  c.lo.u32[2] = f(5, 4);
  c.hi.u32[2] = f(7, 6);
  Reg128 o;
  o.u32[0] = c.lo.u32[0];
  o.u32[1] = c.hi.u32[0];
  o.u32[2] = c.lo.u32[2];
  o.u32[3] = c.hi.u32[2];
  set128(c, rd(w), o);
}
LANTERN_SEM(PHMADH) { hpair(c, w, false); }
LANTERN_SEM(PHMSBH) { hpair(c, w, true); }

// ---- MMI: HI/LO transfers --------------------------------------------------------------------
LANTERN_SEM(PMFHL_LW) {
  Reg128 o;
  o.u32[0] = c.lo.u32[0];
  o.u32[1] = c.hi.u32[0];
  o.u32[2] = c.lo.u32[2];
  o.u32[3] = c.hi.u32[2];
  set128(c, rd(w), o);
}
LANTERN_SEM(PMFHL_UW) {
  Reg128 o;
  o.u32[0] = c.lo.u32[1];
  o.u32[1] = c.hi.u32[1];
  o.u32[2] = c.lo.u32[3];
  o.u32[3] = c.hi.u32[3];
  set128(c, rd(w), o);
}
LANTERN_SEM(PMFHL_SLW) {
  Reg128 o;
  for (int i = 0; i < 2; ++i) {
    const int64_t v = static_cast<int64_t>(static_cast<uint64_t>(c.hi.u32[2 * i]) << 32 | c.lo.u32[2 * i]);
    o.s64[i] = sat<int32_t>(v);
  }
  set128(c, rd(w), o);
}
LANTERN_SEM(PMFHL_LH) {
  Reg128 o;
  const uint16_t v[8] = {c.lo.u16[0], c.lo.u16[2], c.hi.u16[0], c.hi.u16[2],
                         c.lo.u16[4], c.lo.u16[6], c.hi.u16[4], c.hi.u16[6]};
  for (int i = 0; i < 8; ++i) o.u16[i] = v[i];
  set128(c, rd(w), o);
}
LANTERN_SEM(PMFHL_SH) {
  Reg128 o;
  const int32_t v[8] = {c.lo.s32[0], c.lo.s32[1], c.hi.s32[0], c.hi.s32[1],
                        c.lo.s32[2], c.lo.s32[3], c.hi.s32[2], c.hi.s32[3]};
  for (int i = 0; i < 8; ++i) o.s16[i] = sat<int16_t>(v[i]);
  set128(c, rd(w), o);
}
LANTERN_SEM(PMTHL_LW) {
  const Reg128 a = c.gpr[rs(w)];
  c.lo.u32[0] = a.u32[0];
  c.hi.u32[0] = a.u32[1];
  c.lo.u32[2] = a.u32[2];
  c.hi.u32[2] = a.u32[3];
}

#undef LANTERN_SEM

// ---- branch conditions -----------------------------------------------------------------------
inline bool BEQ(const Cpu& c, uint32_t w) { return R(c, rs(w)) == R(c, rt(w)); }
inline bool BNE(const Cpu& c, uint32_t w) { return R(c, rs(w)) != R(c, rt(w)); }
inline bool BLEZ(const Cpu& c, uint32_t w) { return static_cast<int64_t>(R(c, rs(w))) <= 0; }
inline bool BGTZ(const Cpu& c, uint32_t w) { return static_cast<int64_t>(R(c, rs(w))) > 0; }
inline bool BLTZ(const Cpu& c, uint32_t w) { return static_cast<int64_t>(R(c, rs(w))) < 0; }
inline bool BGEZ(const Cpu& c, uint32_t w) { return static_cast<int64_t>(R(c, rs(w))) >= 0; }
inline bool BC1F(const Cpu& c, uint32_t) { return (c.fcr31 & kFpuCond) == 0; }
inline bool BC1T(const Cpu& c, uint32_t) { return (c.fcr31 & kFpuCond) != 0; }

}  // namespace lantern::ee::sem

#include "ee/semantics_vu0.h"  // IWYU pragma: export
