#pragma once

// VU0 macro-mode (COP2) semantics. Floats follow the same PS2 rules as the FPU (no inf/NaN,
// denormals flush) via f_in/f_out. Included by semantics.h; not a standalone header.
//
// Encoding: dest mask = bits 24..21 (x = bit 24 ... w = bit 21), ft = 20..16, fs = 15..11,
// fd = 10..6, broadcast field = bits 1..0, fsf = 22..21, ftf = 24..23.
// MAC flags: Z = bits 0-3, S = 4-7, U = 8-11, O = 12-15, with x as the high bit of each nibble.

namespace lantern::ee::sem {

namespace vu {
constexpr unsigned dest(uint32_t w) { return (w >> 21) & 15; }
constexpr bool on(unsigned dest, int comp) { return dest & (8u >> comp); }
constexpr unsigned bc(uint32_t w) { return w & 3; }
constexpr unsigned fsf(uint32_t w) { return (w >> 21) & 3; }
constexpr unsigned ftf(uint32_t w) { return (w >> 23) & 3; }

inline float vf(const Cpu& c, unsigned r, int comp) { return f_in(c.vu0.vf[r].u32[comp]); }
inline float acc(const Cpu& c, int comp) { return f_in(c.vu0.acc[comp]); }

// Write a result vector to `dst` (vf register index, or -1 for ACC) under the dest mask and
// update the MAC/status flags for written components.
inline void write(Cpu& c, int dst, unsigned d, const float (&v)[4]) {
  uint32_t mac = 0;
  for (int i = 0; i < 4; ++i) {
    if (!on(d, i)) continue;
    const uint32_t bits = f_out(v[i]);
    const int bit = 3 - i;
    if ((bits & 0x7FFFFFFFu) == 0) mac |= 1u << bit;                                                  // Z
    if (bits & 0x80000000u) mac |= 1u << (4 + bit);                                                   // S
    if ((bits & 0x7FFFFFFFu) == kFltMax && std::fabs(v[i]) > 3.4028234e38f) mac |= 1u << (12 + bit);  // O
    if (dst < 0)
      c.vu0.acc[i] = bits;
    else if (dst > 0)
      c.vu0.vf[dst].u32[i] = bits;
  }
  c.vu0.mac = mac;
  uint32_t st = 0;
  if (mac & 0x000F) st |= 1;
  if (mac & 0x00F0) st |= 2;
  if (mac & 0x0F00) st |= 4;
  if (mac & 0xF000) st |= 8;
  c.vu0.status = (c.vu0.status & ~0xFu) | st | (st << 6);  // sticky copies in bits 6..9
}

enum class Rhs { Vec, Bc, Q, I };
inline float rhs(const Cpu& c, uint32_t w, Rhs k, int comp) {
  switch (k) {
    case Rhs::Vec: return vf(c, rt(w), comp);
    case Rhs::Bc: return vf(c, rt(w), static_cast<int>(bc(w)));
    case Rhs::Q: return f_in(c.vu0.q);
    case Rhs::I: return f_in(c.vu0.i);
  }
  return 0.0f;
}

enum class Kind { Add, Sub, Mul, Madd, Msub, Max, Min };
// dst: true = fd, false = ACC
inline void arith(Cpu& c, uint32_t w, Kind k, Rhs r, bool to_fd) {
  float out[4];
  for (int i = 0; i < 4; ++i) {
    const float a = vf(c, rd(w), i), b = rhs(c, w, r, i);
    switch (k) {
      case Kind::Add: out[i] = a + b; break;
      case Kind::Sub: out[i] = a - b; break;
      case Kind::Mul: out[i] = a * b; break;
      case Kind::Madd: out[i] = acc(c, i) + a * b; break;
      case Kind::Msub: out[i] = acc(c, i) - a * b; break;
      case Kind::Max: out[i] = a > b ? a : b; break;
      case Kind::Min: out[i] = a < b ? a : b; break;
    }
  }
  if (k == Kind::Max || k == Kind::Min) {  // MAX/MINI don't update flags
    for (int i = 0; i < 4; ++i)
      if (on(dest(w), i) && fd(w)) c.vu0.vf[fd(w)].u32[i] = f_out(out[i]);
    return;
  }
  write(c, to_fd ? static_cast<int>(fd(w)) : -1, dest(w), out);
}

inline void opm(Cpu& c, uint32_t w, bool to_fd) {  // outer product (cross product helpers)
  static constexpr int y[3] = {1, 2, 0}, z[3] = {2, 0, 1};
  float out[4] = {0, 0, 0, 0};
  for (int i = 0; i < 3; ++i) {
    const float p = vf(c, rd(w), y[i]) * vf(c, rt(w), z[i]);
    out[i] = to_fd ? acc(c, i) - p : p;
  }
  write(c, to_fd ? static_cast<int>(fd(w)) : -1, 0xE, out);
}

inline uint32_t* vumem(Cpu& c, uint32_t qword_index) {
  return reinterpret_cast<uint32_t*>(c.vu0.mem + (qword_index & 0xFF) * 16);
}

inline void rnext(Cpu& c) {
  uint32_t r = c.vu0.r;
  const uint32_t x = (r >> 4) & 1, y = (r >> 22) & 1;
  r = (r << 1) ^ x ^ y;
  c.vu0.r = (r & 0x7FFFFF) | 0x3F800000;
}
}  // namespace vu

#define LANTERN_SEM(name)                                                                            \
  inline void name([[maybe_unused]] Cpu& c, [[maybe_unused]] Memory& m, [[maybe_unused]] uint32_t w, \
                   [[maybe_unused]] uint32_t pc)

LANTERN_SEM(VU0_SPECIAL1) {
  using vu::Kind;
  using vu::Rhs;
  const unsigned f = w & 63;
  if (f < 28) {
    static constexpr Kind kinds[7] = {Kind::Add, Kind::Sub, Kind::Madd, Kind::Msub, Kind::Max, Kind::Min, Kind::Mul};
    return vu::arith(c, w, kinds[f / 4], Rhs::Bc, true);
  }
  switch (f) {
    case 28: return vu::arith(c, w, Kind::Mul, Rhs::Q, true);
    case 29: return vu::arith(c, w, Kind::Max, Rhs::I, true);
    case 30: return vu::arith(c, w, Kind::Mul, Rhs::I, true);
    case 31: return vu::arith(c, w, Kind::Min, Rhs::I, true);
    case 32: return vu::arith(c, w, Kind::Add, Rhs::Q, true);
    case 33: return vu::arith(c, w, Kind::Madd, Rhs::Q, true);
    case 34: return vu::arith(c, w, Kind::Add, Rhs::I, true);
    case 35: return vu::arith(c, w, Kind::Madd, Rhs::I, true);
    case 36: return vu::arith(c, w, Kind::Sub, Rhs::Q, true);
    case 37: return vu::arith(c, w, Kind::Msub, Rhs::Q, true);
    case 38: return vu::arith(c, w, Kind::Sub, Rhs::I, true);
    case 39: return vu::arith(c, w, Kind::Msub, Rhs::I, true);
    case 40: return vu::arith(c, w, Kind::Add, Rhs::Vec, true);
    case 41: return vu::arith(c, w, Kind::Madd, Rhs::Vec, true);
    case 42: return vu::arith(c, w, Kind::Mul, Rhs::Vec, true);
    case 43: return vu::arith(c, w, Kind::Max, Rhs::Vec, true);
    case 44: return vu::arith(c, w, Kind::Sub, Rhs::Vec, true);
    case 45: return vu::arith(c, w, Kind::Msub, Rhs::Vec, true);
    case 46: return vu::opm(c, w, true);
    case 47: return vu::arith(c, w, Kind::Min, Rhs::Vec, true);
    case 48:  // VIADD id = is + it
      if (sa(w) & 15) c.vu0.vi[sa(w) & 15] = static_cast<uint16_t>(c.vu0.vi[rd(w) & 15] + c.vu0.vi[rt(w) & 15]);
      return;
    case 49:
      if (sa(w) & 15) c.vu0.vi[sa(w) & 15] = static_cast<uint16_t>(c.vu0.vi[rd(w) & 15] - c.vu0.vi[rt(w) & 15]);
      return;
    case 50: {  // VIADDI it = is + imm5
      const int imm = static_cast<int>((sa(w) ^ 16u)) - 16;
      if (rt(w) & 15) c.vu0.vi[rt(w) & 15] = static_cast<uint16_t>(c.vu0.vi[rd(w) & 15] + imm);
      return;
    }
    case 52:
      if (sa(w) & 15) c.vu0.vi[sa(w) & 15] = c.vu0.vi[rd(w) & 15] & c.vu0.vi[rt(w) & 15];
      return;
    case 53:
      if (sa(w) & 15) c.vu0.vi[sa(w) & 15] = c.vu0.vi[rd(w) & 15] | c.vu0.vi[rt(w) & 15];
      return;
    default: trap(pc, "VU0 micro mode (VCALLMS/VCALLMSR) is not implemented yet", w);
  }
}

LANTERN_SEM(VU0_SPECIAL2) {
  using vu::Kind;
  using vu::Rhs;
  const unsigned idx = ((w >> 6) & 31) << 2 | (w & 3);
  auto& V = c.vu0;
  const unsigned d = vu::dest(w);
  if (idx < 16) {
    static constexpr Kind kinds[4] = {Kind::Add, Kind::Sub, Kind::Madd, Kind::Msub};
    return vu::arith(c, w, kinds[idx / 4], Rhs::Bc, false);
  }
  if (idx >= 24 && idx < 28) return vu::arith(c, w, Kind::Mul, Rhs::Bc, false);
  switch (idx) {
    case 16:
    case 17:
    case 18:
    case 19: {  // VITOFn ft = (float)fs / 2^n
      static constexpr float scale[4] = {1.0f, 1.0f / 16, 1.0f / 4096, 1.0f / 32768};
      for (int i = 0; i < 4; ++i)
        if (vu::on(d, i) && rt(w)) V.vf[rt(w)].u32[i] = f_out(static_cast<float>(V.vf[rd(w)].s32[i]) * scale[idx - 16]);
      return;
    }
    case 20:
    case 21:
    case 22:
    case 23: {  // VFTOIn ft = (int)(fs * 2^n), truncating, saturating
      static constexpr float scale[4] = {1.0f, 16.0f, 4096.0f, 32768.0f};
      for (int i = 0; i < 4; ++i) {
        if (!vu::on(d, i) || !rt(w)) continue;
        const float f = vu::vf(c, rd(w), i) * scale[idx - 20];
        int32_t v = f >= 2147483648.0f ? INT32_MAX : f <= -2147483648.0f ? INT32_MIN : static_cast<int32_t>(f);
        V.vf[rt(w)].s32[i] = v;
      }
      return;
    }
    case 28: return vu::arith(c, w, Kind::Mul, Rhs::Q, false);
    case 29:  // VABS
      for (int i = 0; i < 4; ++i)
        if (vu::on(d, i) && rt(w)) V.vf[rt(w)].u32[i] = V.vf[rd(w)].u32[i] & 0x7FFFFFFFu;
      return;
    case 30: return vu::arith(c, w, Kind::Mul, Rhs::I, false);
    case 31: {  // VCLIPw.xyz fs, ft: compare fs.xyz against |ft.w|
      const float lim = std::fabs(vu::vf(c, rt(w), 3));
      uint32_t j = 0;
      for (int i = 0; i < 3; ++i) {
        const float x = vu::vf(c, rd(w), i);
        if (x > lim) j |= 1u << (2 * i);
        if (x < -lim) j |= 1u << (2 * i + 1);
      }
      V.clip = ((V.clip << 6) | j) & 0xFFFFFF;
      return;
    }
    case 32: return vu::arith(c, w, Kind::Add, Rhs::Q, false);
    case 33: return vu::arith(c, w, Kind::Madd, Rhs::Q, false);
    case 34: return vu::arith(c, w, Kind::Add, Rhs::I, false);
    case 35: return vu::arith(c, w, Kind::Madd, Rhs::I, false);
    case 36: return vu::arith(c, w, Kind::Sub, Rhs::Q, false);
    case 37: return vu::arith(c, w, Kind::Msub, Rhs::Q, false);
    case 38: return vu::arith(c, w, Kind::Sub, Rhs::I, false);
    case 39: return vu::arith(c, w, Kind::Msub, Rhs::I, false);
    case 40: return vu::arith(c, w, Kind::Add, Rhs::Vec, false);
    case 41: return vu::arith(c, w, Kind::Madd, Rhs::Vec, false);
    case 42: return vu::arith(c, w, Kind::Mul, Rhs::Vec, false);
    case 44: return vu::arith(c, w, Kind::Sub, Rhs::Vec, false);
    case 45: return vu::arith(c, w, Kind::Msub, Rhs::Vec, false);
    case 46: return vu::opm(c, w, false);
    case 47: return;  // VNOP
    case 48:          // VMOVE ft = fs
      for (int i = 0; i < 4; ++i)
        if (vu::on(d, i) && rt(w)) V.vf[rt(w)].u32[i] = V.vf[rd(w)].u32[i];
      return;
    case 49: {  // VMR32: rotate fs right by one component
      const Reg128 s = V.vf[rd(w)];
      for (int i = 0; i < 4; ++i)
        if (vu::on(d, i) && rt(w)) V.vf[rt(w)].u32[i] = s.u32[(i + 1) & 3];
      return;
    }
    case 52:
    case 54: {  // VLQI ft, (is++) / VLQD ft, (--is)
      const unsigned is = rd(w) & 15;
      if (idx == 54 && is) V.vi[is] = static_cast<uint16_t>(V.vi[is] - 1);
      const uint32_t* q = vu::vumem(c, V.vi[is]);
      for (int i = 0; i < 4; ++i)
        if (vu::on(d, i) && rt(w)) V.vf[rt(w)].u32[i] = q[i];
      if (idx == 52 && is) V.vi[is] = static_cast<uint16_t>(V.vi[is] + 1);
      return;
    }
    case 53:
    case 55: {  // VSQI fs, (it++) / VSQD fs, (--it)
      const unsigned it = rt(w) & 15;
      if (idx == 55 && it) V.vi[it] = static_cast<uint16_t>(V.vi[it] - 1);
      uint32_t* q = vu::vumem(c, V.vi[it]);
      for (int i = 0; i < 4; ++i)
        if (vu::on(d, i)) q[i] = V.vf[rd(w)].u32[i];
      if (idx == 53 && it) V.vi[it] = static_cast<uint16_t>(V.vi[it] + 1);
      return;
    }
    case 56: {  // VDIV Q = fs.fsf / ft.ftf
      const float a = vu::vf(c, rd(w), static_cast<int>(vu::fsf(w))),
                  b = vu::vf(c, rt(w), static_cast<int>(vu::ftf(w)));
      V.q = b == 0.0f ? (((std::bit_cast<uint32_t>(a) ^ std::bit_cast<uint32_t>(b)) & 0x80000000u) | kFltMax)
                      : f_out(a / b);
      return;
    }
    case 57: V.q = f_out(std::sqrt(std::fabs(vu::vf(c, rt(w), static_cast<int>(vu::ftf(w)))))); return;
    case 58: {  // VRSQRT Q = fs.fsf / sqrt(ft.ftf)
      const float a = vu::vf(c, rd(w), static_cast<int>(vu::fsf(w)));
      const float b = std::sqrt(std::fabs(vu::vf(c, rt(w), static_cast<int>(vu::ftf(w)))));
      V.q = b == 0.0f ? ((std::bit_cast<uint32_t>(a) & 0x80000000u) | kFltMax) : f_out(a / b);
      return;
    }
    case 59: return;  // VWAITQ: Q is always ready here
    case 60:          // VMTIR it, fs.fsf
      if (rt(w) & 15) V.vi[rt(w) & 15] = static_cast<uint16_t>(V.vf[rd(w)].u32[vu::fsf(w)]);
      return;
    case 61:  // VMFIR ft, is (sign-extended)
      for (int i = 0; i < 4; ++i)
        if (vu::on(d, i) && rt(w)) V.vf[rt(w)].s32[i] = static_cast<int16_t>(V.vi[rd(w) & 15]);
      return;
    case 62:  // VILWR it, (is)
      for (int i = 0; i < 4; ++i)
        if (vu::on(d, i) && (rt(w) & 15)) V.vi[rt(w) & 15] = static_cast<uint16_t>(vu::vumem(c, V.vi[rd(w) & 15])[i]);
      return;
    case 63:  // VISWR it, (is)
      for (int i = 0; i < 4; ++i)
        if (vu::on(d, i)) vu::vumem(c, V.vi[rd(w) & 15])[i] = V.vi[rt(w) & 15];
      return;
    case 64: vu::rnext(c); [[fallthrough]];  // VRNEXT then VRGET
    case 65:
      for (int i = 0; i < 4; ++i)
        if (vu::on(d, i) && rt(w)) V.vf[rt(w)].u32[i] = V.r;
      return;
    case 66: V.r = (V.vf[rd(w)].u32[vu::fsf(w)] & 0x7FFFFF) | 0x3F800000; return;
    case 67: V.r = ((V.r ^ V.vf[rd(w)].u32[vu::fsf(w)]) & 0x7FFFFF) | 0x3F800000; return;
    default: trap(pc, "unassigned VU0 special2 opcode", w);
  }
}

LANTERN_SEM(QMFC2) { set128(c, rt(w), c.vu0.vf[rd(w)]); }
LANTERN_SEM(QMTC2) {
  if (rd(w)) c.vu0.vf[rd(w)] = c.gpr[rt(w)];
}
inline uint32_t vu0_control(const Cpu& c, unsigned reg) {
  if (reg < 16) return c.vu0.vi[reg];
  switch (reg) {
    case 16: return c.vu0.status;
    case 17: return c.vu0.mac;
    case 18: return c.vu0.clip;
    case 20: return c.vu0.r;
    case 21: return c.vu0.i;
    case 22: return c.vu0.q;
    default: return 0;  // TPC/CMSAR/FBRST/VPU-STAT: VU0 is never busy in this model
  }
}
LANTERN_SEM(CFC2) { set(c, rt(w), sx32(vu0_control(c, rd(w)))); }
LANTERN_SEM(CTC2) {
  const unsigned reg = rd(w);
  const uint32_t v = R32(c, rt(w));
  if (reg >= 1 && reg < 16)
    c.vu0.vi[reg] = static_cast<uint16_t>(v);
  else if (reg == 16)
    c.vu0.status = (c.vu0.status & 0x3F) | (v & 0xFC0);  // only sticky bits writable
  else if (reg == 18)
    c.vu0.clip = v & 0xFFFFFF;
  else if (reg == 20)
    c.vu0.r = (v & 0x7FFFFF) | 0x3F800000;
  else if (reg == 21)
    c.vu0.i = v;
  else if (reg == 22)
    c.vu0.q = v;
  // FBRST (28) resets / CMSAR writes have no lasting effect in macro-only mode.
}
LANTERN_SEM(LQC2) {
  if (!rt(w)) return;
  m.read128(addr(c, w) & ~15u, c.vu0.vf[rt(w)]);
}
LANTERN_SEM(SQC2) { m.write128(addr(c, w) & ~15u, c.vu0.vf[rt(w)]); }

inline bool BC2F(const Cpu&, uint32_t) { return true; }  // VU0 never busy (synchronous model)
inline bool BC2T(const Cpu&, uint32_t) { return false; }

#undef LANTERN_SEM

}  // namespace lantern::ee::sem
