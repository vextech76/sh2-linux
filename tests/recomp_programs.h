#pragma once
// Synthetic R5900 test programs for the recompiler differential test (no game code).
// Shared by the build-time generator and the test, so both see identical programs.

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "ee/r5900.h"
#include "mips_asm.h"

namespace lantern::test {

struct TestProgram {
  std::string name;
  uint32_t base;
  uint32_t entry;
  std::vector<uint32_t> code;
};

inline constexpr uint32_t kProgBase = 0x00100000;
inline constexpr uint32_t kDataBase = 0x00400000;  // s7 points here
inline constexpr uint32_t kDataSize = 4096;
inline constexpr uint32_t kStackTop = 0x01000000;

namespace detail {
using namespace mips;

struct Rng {
  uint64_t s;
  uint32_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return static_cast<uint32_t>(s >> 11);
  }
  uint32_t below(uint32_t n) { return next() % n; }
  bool chance(uint32_t pct) { return below(100) < pct; }
};

constexpr unsigned kDest[] = {zero, v0, v1, a0, a1, a2, a3, t0, t1, t2, t3, t4, t5, t6, t7, t8, s0, s1, s2, s3, s4, s5};
constexpr unsigned kSrc[] = {zero, v0, v1, a0, a1, a2, a3, t0, t1, t2, t3, t4, t5, t6, t7, t8, s0, s1, s2, s3, s4, s5};

inline uint32_t special(unsigned f, unsigned rd, unsigned rs, unsigned rt, unsigned sa = 0) {
  return R(rs, rt, rd, sa, f);
}
inline uint32_t mmi(unsigned f, unsigned sub, unsigned rd, unsigned rs, unsigned rt) {
  return 28u << 26 | R(rs, rt, rd, sub, f);
}
inline uint32_t fpu(unsigned funct, unsigned fd_, unsigned fs_, unsigned ft_) {
  return 17u << 26 | 16u << 21 | ft_ << 16 | fs_ << 11 | fd_ << 6 | funct;
}

inline void expect(uint32_t w, ee::Op op) {
  if (ee::decode(w, 0).op != op)
    throw std::logic_error("test encoder produced " + std::string(ee::mnemonic(ee::decode(w, 0).op)) + " expected " +
                           std::string(ee::mnemonic(op)));
}

// One random non-control-flow instruction.
inline uint32_t random_plain(Rng& r) {
  using O = ee::Op;
  const unsigned d = kDest[r.below(std::size(kDest))], s = kSrc[r.below(std::size(kSrc))],
                 t = kSrc[r.below(std::size(kSrc))], sh = r.below(32);
  const int imm = static_cast<int>(r.next() & 0xFFFF) - 0x8000;
  switch (r.below(16)) {
    case 0: {  // R-type ALU
      static constexpr std::pair<unsigned, O> ops[] = {
          {33, O::ADDU}, {35, O::SUBU}, {45, O::DADDU}, {47, O::DSUBU}, {36, O::AND},   {37, O::OR},
          {38, O::XOR},  {39, O::NOR},  {42, O::SLT},   {43, O::SLTU},  {10, O::MOVZ},  {11, O::MOVN},
          {4, O::SLLV},  {6, O::SRLV},  {7, O::SRAV},   {20, O::DSLLV}, {22, O::DSRLV}, {23, O::DSRAV}};
      auto [f, op] = ops[r.below(std::size(ops))];
      uint32_t w = special(f, d, s, t);
      expect(w, op);
      return w;
    }
    case 1: {  // shifts by sa
      static constexpr std::pair<unsigned, O> ops[] = {{0, O::SLL},     {2, O::SRL},     {3, O::SRA},
                                                       {56, O::DSLL},   {58, O::DSRL},   {59, O::DSRA},
                                                       {60, O::DSLL32}, {62, O::DSRL32}, {63, O::DSRA32}};
      auto [f, op] = ops[r.below(std::size(ops))];
      uint32_t w = special(f, d, 0, t, sh);
      if (w == 0) w = special(f, d == zero ? v0 : d, 0, t, sh);  // avoid nop
      expect(w, op);
      return w;
    }
    case 2: {  // immediates
      static constexpr std::pair<unsigned, O> ops[] = {{9, O::ADDIU}, {25, O::DADDIU}, {10, O::SLTI}, {11, O::SLTIU},
                                                       {12, O::ANDI}, {13, O::ORI},    {14, O::XORI}};
      auto [op_, op] = ops[r.below(std::size(ops))];
      uint32_t w = I(op_, s, d, imm);
      expect(w, op);
      return w;
    }
    case 3: return lui(d, r.next() & 0xFFFF);
    case 4: {  // multiply/divide pipelines
      static constexpr std::pair<unsigned, O> ops[] = {{24, O::MULT}, {25, O::MULTU}, {26, O::DIV}, {27, O::DIVU}};
      auto [f, op] = ops[r.below(4)];
      uint32_t w = (op == O::DIV || op == O::DIVU) ? special(f, 0, s, t) : special(f, d, s, t);
      if (r.chance(40)) w |= 28u << 26;  // MULT1/MULTU1/DIV1/DIVU1 share function codes in MMI space
      return w;
    }
    case 5: {
      static constexpr std::pair<unsigned, unsigned> mm[] = {{0, 0}, {1, 0}, {32, 0}, {33, 0}};  // madd(u)(1)
      auto [f, unused] = mm[r.below(4)];
      (void)unused;
      return mmi(f, 0, d, s, t);
    }
    case 6: {  // HI/LO moves
      static constexpr uint32_t ops[] = {16, 17, 18, 19};
      uint32_t f = ops[r.below(4)];
      bool pipe1 = r.chance(50);
      uint32_t w = (f == 16 || f == 18) ? special(f, d, 0, 0) : special(f, 0, s, 0);
      if (pipe1) w = mmi(f, 0, (f == 16 || f == 18) ? d : 0, (f == 17 || f == 19) ? s : 0, 0);
      return w;
    }
    case 7: {  // MMI lane ops
      static constexpr std::pair<unsigned, unsigned> ops[] = {
          {8, 0},  {8, 1},  {8, 4},   {8, 5},  {8, 8},  {8, 9},  {8, 2},   {8, 6},   {8, 10},
          {8, 3},  {8, 7},  {8, 18},  {8, 22}, {8, 26}, {8, 19}, {40, 2},  {40, 6},  {40, 10},
          {40, 3}, {40, 7}, {40, 18}, {9, 18}, {9, 19}, {9, 14}, {41, 18}, {41, 19}, {41, 14}};
      auto [f, sub] = ops[r.below(std::size(ops))];
      return mmi(f, sub, d, s, t);
    }
    case 8: {  // MMI shifts / copies / hi-lo
      switch (r.below(7)) {
        case 0: return mmi(52 + 2 * r.below(2) + (r.below(2) ? 1 : 0) * 0, 0, d, 0, t) | (sh & 15) << 6;  // psllh/psrlh
        case 1: return mmi(60, 0, d, 0, t) | sh << 6;                                                     // psllw
        case 2: return mmi(63, 0, d, 0, t) | sh << 6;                                                     // psraw
        case 3: return mmi(41, 27, d, 0, t);                                                              // pcpyh
        case 4: return mmi(9, 8 + r.below(2), d, 0, 0);                                                   // pmfhi/pmflo
        case 5: return mmi(41, 8 + r.below(2), 0, s, 0);                                                  // pmthi/pmtlo
        default: return mmi(4, 0, d, s, 0);                                                               // plzcw
      }
    }
    case 9: return r.chance(50) ? mtsab(s, static_cast<int>(r.below(16))) : mmi(40, 27, d, s, t);  // qfsrv
    case 10:
    case 11: {  // loads relative to s7 (aligned where required)
      static constexpr std::tuple<unsigned, unsigned, O> ops[] = {
          {32, 1, O::LB}, {36, 1, O::LBU}, {33, 2, O::LH},  {37, 2, O::LHU}, {35, 4, O::LW},  {39, 4, O::LWU},
          {55, 8, O::LD}, {30, 1, O::LQ},  {34, 1, O::LWL}, {38, 1, O::LWR}, {26, 1, O::LDL}, {27, 1, O::LDR}};
      auto [op_, al, op] = ops[r.below(std::size(ops))];
      int off = static_cast<int>(r.below(kDataSize - 16)) & ~static_cast<int>(al - 1);
      uint32_t w = I(op_, s7, d, off);
      expect(w, op);
      return w;
    }
    case 12: {  // stores relative to s7
      static constexpr std::tuple<unsigned, unsigned, O> ops[] = {{40, 1, O::SB},  {41, 2, O::SH},  {43, 4, O::SW},
                                                                  {63, 8, O::SD},  {31, 1, O::SQ},  {42, 1, O::SWL},
                                                                  {46, 1, O::SWR}, {44, 1, O::SDL}, {45, 1, O::SDR}};
      auto [op_, al, op] = ops[r.below(std::size(ops))];
      int off = static_cast<int>(r.below(kDataSize - 16)) & ~static_cast<int>(al - 1);
      uint32_t w = I(op_, s7, t, off);
      expect(w, op);
      return w;
    }
    case 13: {  // GPR <-> FPR
      unsigned f = r.below(32);
      return r.chance(50) ? mtc1(s, f) : mfc1(d, f);
    }
    case 14: {  // FPU arithmetic
      const unsigned a = r.below(32), b = r.below(32), c = r.below(32);
      static constexpr std::pair<unsigned, O> three[] = {{0, O::ADD_S},  {1, O::SUB_S},   {2, O::MUL_S},
                                                         {3, O::DIV_S},  {28, O::MADD_S}, {29, O::MSUB_S},
                                                         {40, O::MAX_S}, {41, O::MIN_S},  {22, O::RSQRT_S}};
      static constexpr std::pair<unsigned, O> acc[] = {
          {24, O::ADDA_S}, {25, O::SUBA_S}, {26, O::MULA_S}, {30, O::MADDA_S}, {31, O::MSUBA_S}};
      switch (r.below(5)) {
        case 0: {
          auto [f, op] = three[r.below(std::size(three))];
          uint32_t w = fpu(f, a, b, c);
          expect(w, op);
          return w;
        }
        case 1: {
          auto [f, op] = acc[r.below(std::size(acc))];
          uint32_t w = fpu(f, 0, b, c);
          expect(w, op);
          return w;
        }
        case 2: return fpu(5 + r.below(3), a, b, 0);  // abs/mov/neg
        case 3: return r.chance(50) ? fpu(36, a, b, 0) : (17u << 26 | 20u << 21 | b << 11 | a << 6 | 32);  // cvt
        default: return fpu(4, a, 0, c);  // sqrt.s fd, ft
      }
    }
    default: {  // FPU compare
      static constexpr unsigned conds[] = {48, 50, 52, 54};
      return fpu(conds[r.below(4)], 0, r.below(32), r.below(32));
    }
  }
}

inline TestProgram random_program(uint64_t seed) {
  Rng r{seed * 0x9E3779B97F4A7C15ull + 1};
  TestProgram p{"random_" + std::to_string(seed), kProgBase, kProgBase, {}};
  auto& w = p.code;
  auto here = [&] { return p.base + static_cast<uint32_t>(w.size() * 4); };
  // prologue: save $ra (calls clobber it)
  w.insert(w.end(), {addiu(sp, sp, -16), sd(ra, 0, sp)});
  std::vector<std::pair<size_t, uint32_t>> fixups;  // index -> helper-call placeholder
  const size_t body = 90 + r.below(60);
  while (w.size() < body) {
    switch (r.below(20)) {
      case 0: {  // forward conditional branch with a plain delay slot
        static constexpr unsigned kinds = 16;
        unsigned k = r.below(kinds);
        unsigned a = kSrc[r.below(std::size(kSrc))], b = kSrc[r.below(std::size(kSrc))];
        int off = 1 + static_cast<int>(r.below(6));
        uint32_t br;
        switch (k) {
          case 0: br = beq(a, b, off); break;
          case 1: br = bne(a, b, off); break;
          case 2: br = blez(a, off); break;
          case 3: br = bgtz(a, off); break;
          case 4: br = bltz(a, off); break;
          case 5: br = bgez(a, off); break;
          case 6: br = beql(a, b, off); break;
          case 7: br = bnel(a, b, off); break;
          case 8: br = I(22, a, 0, off); break;  // blezl
          case 9: br = I(23, a, 0, off); break;  // bgtzl
          case 10: br = I(1, a, 2, off); break;  // bltzl
          case 11: br = I(1, a, 3, off); break;  // bgezl
          case 12: br = bc1t(off); break;
          case 13: br = 17u << 26 | 8u << 21 | (static_cast<uint32_t>(off) & 0xFFFF); break;             // bc1f
          case 14: br = 17u << 26 | 8u << 21 | 3u << 16 | (static_cast<uint32_t>(off) & 0xFFFF); break;  // bc1tl
          default: br = 17u << 26 | 8u << 21 | 2u << 16 | (static_cast<uint32_t>(off) & 0xFFFF); break;  // bc1fl
        }
        w.push_back(br);
        w.push_back(random_plain(r));
        for (int i = 0; i < off - 1 + 2; ++i) w.push_back(random_plain(r));  // target stays in range
        break;
      }
      case 1: {  // direct call to the helper (patched later)
        fixups.emplace_back(w.size(), 0);
        w.push_back(0);
        w.push_back(random_plain(r));
        break;
      }
      case 2: {  // indirect call through t9
        fixups.emplace_back(w.size(), 1);
        w.insert(w.end(), {0, 0, jalr(ra, t9), random_plain(r)});
        break;
      }
      case 3: {  // counted loop
        const unsigned n = 1 + r.below(4);
        w.push_back(addiu(s6, zero, static_cast<int>(n)));
        const uint32_t top = here();
        const unsigned k = 1 + r.below(4);
        for (unsigned i = 0; i < k; ++i) w.push_back(random_plain(r));
        w.push_back(addiu(s6, s6, -1));
        const int back = -static_cast<int>((here() + 4 - top) / 4);
        w.push_back(bne(s6, zero, back));
        w.push_back(random_plain(r));
        break;
      }
      default: w.push_back(random_plain(r)); break;
    }
  }
  // epilogue
  w.insert(w.end(), {ld(ra, 0, sp), jr(ra), addiu(sp, sp, 16)});
  // helper function
  const uint32_t helper = here();
  w.insert(w.end(), {daddu(v0, v0, a0), addiu(sp, sp, -16), sw(a1, 4, sp), lw(v1, 4, sp), jr(ra), addiu(sp, sp, 16)});
  for (auto [idx, kind] : fixups) {
    if (kind == 0) {
      w[idx] = jal(helper);
    } else {
      w[idx] = lui(t9, helper >> 16);
      w[idx + 1] = ori(t9, t9, helper & 0xFFFF);
    }
  }
  return p;
}

}  // namespace detail

inline std::vector<TestProgram> test_programs() {
  using namespace mips;
  std::vector<TestProgram> out;
  {  // recursive fibonacci(12) via jal, with stack frames
    TestProgram p{"fib_recursive", kProgBase, kProgBase, {}};
    const uint32_t fib = kProgBase + 0x40;
    p.code = {addiu(sp, sp, -16), sd(ra, 0, sp), addiu(a0, zero, 12), jal(fib), nop,
              ld(ra, 0, sp),      jr(ra),        addiu(sp, sp, 16)};
    p.code.resize(0x40 / 4, 0);
    // fib(a0): if a0 < 2 return a0; else return fib(a0-1) + fib(a0-2)
    p.code.insert(p.code.end(), {slti(t0, a0, 2),
                                 beq(t0, zero, 3),
                                 nop,
                                 jr(ra),
                                 or_(v0, a0, zero),  // base case
                                 addiu(sp, sp, -32),
                                 sd(ra, 0, sp),
                                 sd(s0, 8, sp),
                                 sd(a0, 16, sp),  //
                                 jal(fib),
                                 addiu(a0, a0, -1),
                                 or_(s0, v0, zero),  //
                                 ld(a0, 16, sp),
                                 jal(fib),
                                 addiu(a0, a0, -2),
                                 addu(v0, v0, s0),  //
                                 ld(s0, 8, sp),
                                 ld(ra, 0, sp),
                                 jr(ra),
                                 addiu(sp, sp, 32)});
    out.push_back(std::move(p));
  }
  {  // switch via jump table, with a loop back to the switch function's first instruction
    const uint32_t sw_fn = kProgBase, entry = kProgBase + 0x80, table = kProgBase + 0x100;
    const uint32_t c0 = kProgBase + 0x30, c1 = kProgBase + 0x3C, c2 = kProgBase + 0x48;
    TestProgram p{"switch_table", kProgBase, entry, {}};
    p.code = {sltiu(at, a0, 3),
              beq(at, zero, 18),
              sll(t0, a0, 2),
              lui(t1, table >> 16),  // 0x00
              addu(t1, t1, t0),
              lw(t1, static_cast<int>(table & 0xFFFF), t1),
              jr(t1),
              nop,  // 0x10
              nop,
              nop,
              nop,
              nop,  // 0x20
              addiu(v0, v0, 100),
              beq(zero, zero, 6),
              nop,  // c0 0x30
              addiu(v0, v0, 200),
              beq(zero, zero, 3),
              nop,  // c1 0x3c
              addiu(v0, v0, 300),
              nop,  // c2 0x48
              addiu(a0, a0, 1),
              slti(t2, a0, 3),
              bne(t2, zero, -23),
              nop,  // 0x50
              jr(ra),
              nop};  // 0x60
    p.code.resize(0x80 / 4, 0);
    p.code.insert(p.code.end(), {addiu(sp, sp, -16), sd(ra, 0, sp), addiu(a0, zero, 0), jal(sw_fn), nop, ld(ra, 0, sp),
                                 jr(ra), addiu(sp, sp, 16)});
    p.code.resize(0x100 / 4, 0);
    p.code.insert(p.code.end(), {c0, c1, c2});
    out.push_back(std::move(p));
  }
  for (uint64_t seed = 1; seed <= 64; ++seed) out.push_back(detail::random_program(seed));
  return out;
}

}  // namespace lantern::test
