#include "ps2/vu.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <string>

#include "core/log.h"
#include "ps2/vif.h"

namespace lantern::ps2 {
namespace {
constexpr const char* kLog = "vu1";

constexpr uint32_t kUpperI = 0x80000000u, kUpperE = 0x40000000u, kUpperD = 0x10000000u, kUpperT = 0x08000000u;

int32_t sign_extend_11(uint32_t v) {
  return (v & 0x400u) ? static_cast<int32_t>(v | 0xFFFFFC00u) : static_cast<int32_t>(v);
}

// Upper FMAC families (opcode >> 2 for the suffixed forms 0x00..0x1B).
enum Fam { kOpAdd = 0, kOpSub = 1, kOpMadd = 2, kOpMsub = 3, kOpMax = 4, kOpMini = 5, kOpMul = 6 };
}  // namespace

void Vu1::reset() {
  for (auto& r : vf_) r = {0, 0, 0, 0};
  vf_[0] = {0, 0, 0, 0x3F800000u};
  vi_.fill(0);
  acc_.fill(0);
  q_ = p_ = i_ = 0;
  r_ = 0x3F800000u;
  status_ = mac_ = clip_ = 0;
  tpc_ = 0;
  running_ = false;
  faulted_ = false;
  end_pending_ = false;
  branch_target_ = 0;
  branch_in_ = -1;
  stop_in_ = -1;
}

void Vu1::start(uint32_t addr) {
  if (addr != 0xFFFFFFFFu) tpc_ = addr & kMicroMask;
  running_ = true;
  faulted_ = false;
  end_pending_ = false;
  branch_in_ = -1;
  stop_in_ = -1;
  ++stats_.microprograms;
  recent_.fill(0);
  recent_pos_ = 0;

  const uint32_t entry = tpc_;
  uint64_t budget = kStepBudget;
  while (running_) {
    if (budget-- == 0) {
      log::warn(kLog, "microprogram at 0x{:04x} never reached an E bit; stopped at 0x{:04x} after {} instructions",
                entry, tpc_, stats_.instructions);
      if (stats_.faults == 0) dump_recent();
      faulted_ = true;
      ++stats_.faults;
      running_ = false;
      break;
    }
    step();
  }
}

void Vu1::step() {
  const uint8_t* p = mem_.micro.data() + tpc_;
  std::memcpy(&lo_, p, 4);
  std::memcpy(&hi_, p + 4, 4);
  // Diagnostic: try alternative byte orders for the uploaded microcode.
  static const int order = std::getenv("LANTERN_VU1_ORDER") ? std::atoi(std::getenv("LANTERN_VU1_ORDER")) : 0;
  if (order & 1) std::swap(lo_, hi_);
  if (order & 2) {
    lo_ = __builtin_bswap32(lo_);
    hi_ = __builtin_bswap32(hi_);
  }
  tpc_ = (tpc_ + 8) & kMicroMask;
  ++stats_.instructions;
  note_pc();
  decode_fields();

  if (hi_ & (kUpperD | kUpperT)) log::warn(kLog, "VU1 debug break at 0x{:04x} ignored", (tpc_ - 8) & kMicroMask);

  if (hi_ & kUpperI)
    i_ = lo_;  // the I bit loads the lower half into I instead of running it
  else
    exec_lower();
  exec_upper();

  if (hi_ & kUpperE) end_pending_ = true;

  // Both the E bit and a taken branch take effect after one delay-slot instruction.
  if (branch_in_ == 0) {
    tpc_ = branch_target_ & kMicroMask;
    branch_in_ = -1;
  } else if (branch_in_ > 0) {
    --branch_in_;
  }
  if (stop_in_ == 0)
    running_ = false;
  else if (stop_in_ > 0)
    --stop_in_;
  if (end_pending_) {
    end_pending_ = false;
    stop_in_ = 0;  // one more instruction, then stop
  }
}

void Vu1::decode_fields() {
  // Lower half: imm 0..10, is/fs 11..15, it/ft 16..20, id/xyzw 21..24, opcode 25..31.
  // Upper half: opcode 0..5, fd 6..10, fs 11..15, ft 16..20, dest mask/bc 21..24, flags 27..31.
  op_lo_ = (lo_ >> 25) & 0x7F;
  op_hi_ = hi_ & 0x3F;
  fs_ = (lo_ >> 11) & 0x1F;
  ft_ = (lo_ >> 16) & 0x1F;
  fd_ = (hi_ >> 6) & 0x1F;
  is_ = fs_ & 0xF;
  it_ = ft_ & 0xF;
  id_ = fd_ & 0xF;
  mask_ = (hi_ >> 21) & 0xF;   // upper instruction destination mask
  lmask_ = (lo_ >> 21) & 0xF;  // lower instruction destination mask (id field)
  imm_ = lo_ & 0x7FF;
  upper_fs_ = (hi_ >> 11) & 0x1F;
  upper_ft_ = (hi_ >> 16) & 0x1F;
}

float Vu1::f(uint32_t reg, int lane) const {
  if (reg == 0) return lane == 3 ? 1.0f : 0.0f;
  return from_bits(vf_[reg][static_cast<size_t>(lane)]);
}

uint32_t* Vu1::reg_dest(uint32_t reg) {
  if (reg == 0) {
    scratch_.fill(0);
    return scratch_.data();
  }
  return vf_[reg].data();
}

float Vu1::mac_write(int shift, float v) {
  const uint32_t b = bits(v), sign = b & 0x80000000u, exp = (b >> 23) & 0xFF;
  const auto s = static_cast<unsigned>(shift);
  if (sign)
    mac_ |= 0x0010u << s;
  else
    mac_ &= ~(0x0010u << s);
  if (v == 0.0f) {
    mac_ = (mac_ & ~(0x1100u << s)) | (1u << s);
    return v;
  }
  if (exp == 0) {  // denormal: flushed to a signed zero
    mac_ = (mac_ & ~(0x1000u << s)) | (0x0101u << s);
    return from_bits(sign);
  }
  if (exp == 255) {  // overflow/infinity: clamped to the largest finite value
    mac_ = (mac_ & ~(0x0101u << s)) | (0x1000u << s);
    return from_bits(sign | 0x7F7FFFFFu);
  }
  mac_ &= ~(0x1101u << s);
  return v;
}

void Vu1::mac_clear(int shift) { mac_ &= ~(0x1111u << static_cast<unsigned>(shift)); }

void Vu1::stat_update() {
  uint32_t f4 = 0;
  if (mac_ & 0x000Fu) f4 |= 0x1;
  if (mac_ & 0x00F0u) f4 |= 0x2;
  if (mac_ & 0x0F00u) f4 |= 0x4;
  if (mac_ & 0xF000u) f4 |= 0x8;
  status_ = (status_ & 0xFF0u) | f4 | (f4 << 6);  // Z/S/U/O now, sticky copies in bits 6..9
}

// ---------------------------------------------------------------------------------------------
// Upper instructions

void Vu1::fmac(int op, bool to_acc, int broadcast_lane, bool use_q, bool use_i) {
  float* dst = reinterpret_cast<float*>(to_acc ? acc_dest() : reg_dest(fd_));
  float src[4];
  for (int lane = 0; lane < 4; ++lane) src[lane] = f(upper_fs_, lane);
  for (int lane = 0; lane < 4; ++lane) {
    const int shift = 3 - lane;
    if (!(mask_ & (1u << static_cast<unsigned>(shift)))) {
      mac_clear(shift);
      continue;
    }
    const float a = src[lane];
    float b;
    if (use_q)
      b = from_bits(q_);
    else if (use_i)
      b = from_bits(i_);
    else if (broadcast_lane >= 0)
      b = f(upper_ft_, broadcast_lane);
    else
      b = f(upper_ft_, lane);
    float r;
    switch (op) {
      case kOpAdd: r = a + b; break;
      case kOpSub: r = a - b; break;
      case kOpMadd: r = from_bits(acc_[static_cast<size_t>(lane)]) + a * b; break;
      case kOpMsub: r = from_bits(acc_[static_cast<size_t>(lane)]) - a * b; break;
      default: r = a * b; break;
    }
    dst[lane] = mac_write(shift, r);
  }
  stat_update();
}

void Vu1::minmax(bool max_op, int broadcast_lane, bool use_q, bool use_i) {
  float* dst = reinterpret_cast<float*>(reg_dest(fd_));
  float src[4];
  for (int lane = 0; lane < 4; ++lane) src[lane] = f(upper_fs_, lane);
  for (int lane = 0; lane < 4; ++lane) {
    const int shift = 3 - lane;
    if (!(mask_ & (1u << static_cast<unsigned>(shift)))) {
      mac_clear(shift);
      continue;
    }
    float b;
    if (use_q)
      b = from_bits(q_);
    else if (use_i)
      b = from_bits(i_);
    else if (broadcast_lane >= 0)
      b = f(upper_ft_, broadcast_lane);
    else
      b = f(upper_ft_, lane);
    // The VU compares bit patterns as signed integers, ordering negatives reversed.
    const auto ia = static_cast<int32_t>(bits(src[lane])), ib = static_cast<int32_t>(bits(b));
    int32_t pick;
    if (ia < 0 && ib < 0)
      pick = max_op ? std::min(ia, ib) : std::max(ia, ib);
    else
      pick = max_op ? std::max(ia, ib) : std::min(ia, ib);
    dst[lane] = mac_write(shift, from_bits(static_cast<uint32_t>(pick)));
  }
  stat_update();
}

void Vu1::opmula() {
  if (mask_ & 8)
    acc_[0] = bits(mac_write(3, f(upper_fs_, 1) * f(upper_ft_, 2)));
  else
    mac_clear(3);
  if (mask_ & 4)
    acc_[1] = bits(mac_write(2, f(upper_fs_, 2) * f(upper_ft_, 0)));
  else
    mac_clear(2);
  if (mask_ & 2)
    acc_[2] = bits(mac_write(1, f(upper_fs_, 0) * f(upper_ft_, 1)));
  else
    mac_clear(1);
  stat_update();
}

void Vu1::opmsub() {
  float* dst = reinterpret_cast<float*>(reg_dest(fd_));
  if (mask_ & 8)
    dst[0] = mac_write(3, from_bits(acc_[0]) - f(upper_fs_, 1) * f(upper_ft_, 2));
  else
    mac_clear(3);
  if (mask_ & 4)
    dst[1] = mac_write(2, from_bits(acc_[1]) - f(upper_fs_, 2) * f(upper_ft_, 0));
  else
    mac_clear(2);
  if (mask_ & 2)
    dst[2] = mac_write(1, from_bits(acc_[2]) - f(upper_fs_, 0) * f(upper_ft_, 1));
  else
    mac_clear(1);
  if (mask_ & 1) dst[3] = from_bits(acc_[3]);
  stat_update();
}

void Vu1::clip() {
  const uint32_t raw = vf_[upper_ft_][3];
  const int32_t limit = (raw & 0x7F800000u) ? static_cast<int32_t>(raw & 0x7FFFFFFFu) : 0x007FFFFF;
  auto over = [&](int lane) {
    const auto b = static_cast<int32_t>(vf_[upper_fs_][static_cast<size_t>(lane)]);
    return b > limit || static_cast<int32_t>(static_cast<uint32_t>(b) ^ 0x80000000u) > limit;
  };
  clip_ = (clip_ << 6) & 0xFFFFFFu;
  if (over(0)) clip_ |= 0x03;  // -x, +x
  if (over(1)) clip_ |= 0x0C;  // -y, +y
  if (over(2)) clip_ |= 0x30;  // -z, +z
}

void Vu1::int_to_float(int offset) {
  if (upper_ft_ == 0) return;
  float* dst = reinterpret_cast<float*>(reg_dest(upper_ft_));
  for (int lane = 0; lane < 4; ++lane) {
    if (!(mask_ & (8u >> static_cast<unsigned>(lane)))) continue;
    float v = static_cast<float>(static_cast<int32_t>(vf_[upper_fs_][static_cast<size_t>(lane)]));
    if (offset) v *= from_bits(0x3F800000u - (static_cast<uint32_t>(offset) << 23));
    dst[lane] = v;
  }
}

void Vu1::float_to_int(int offset) {
  if (upper_ft_ == 0) return;
  uint32_t* dst = reg_dest(upper_ft_);
  for (int lane = 0; lane < 4; ++lane) {
    if (!(mask_ & (8u >> static_cast<unsigned>(lane)))) continue;
    float v = from_bits(vf_[upper_fs_][static_cast<size_t>(lane)]);
    if (offset) v *= from_bits(0x3F800000u + (static_cast<uint32_t>(offset) << 23));
    const uint32_t b = bits(v);
    if ((b & 0x7F800000u) >= 0x4F000000u)
      dst[lane] = (b & 0x80000000u) ? 0x80000000u : 0x7FFFFFFFu;
    else
      dst[lane] = static_cast<uint32_t>(static_cast<int32_t>(v));
  }
}

void Vu1::exec_upper() {
  const uint32_t op = op_hi_;
  if (op <= 0x1B) {  // ADD/SUB/MADD/MSUB/MAX/MINI/MUL with an x/y/z/w broadcast
    const int fam = static_cast<int>(op >> 2), lane = static_cast<int>(op & 3);
    if (fam == kOpMax || fam == kOpMini)
      minmax(fam == kOpMax, lane, false, false);
    else
      fmac(fam, false, lane, false, false);
    return;
  }
  switch (op) {
    case 0x1C: fmac(kOpMul, false, -1, true, false); return;    // MULq
    case 0x1D: minmax(true, -1, false, true); return;           // MAXi
    case 0x1E: fmac(kOpMul, false, -1, false, true); return;    // MULi
    case 0x1F: minmax(false, -1, false, true); return;          // MINIi
    case 0x20: fmac(kOpAdd, false, -1, true, false); return;    // ADDq
    case 0x21: fmac(kOpMadd, false, -1, true, false); return;   // MADDq
    case 0x22: fmac(kOpAdd, false, -1, false, true); return;    // ADDi
    case 0x23: fmac(kOpMadd, false, -1, false, true); return;   // MADDi
    case 0x24: fmac(kOpSub, false, -1, true, false); return;    // SUBq
    case 0x25: fmac(kOpMsub, false, -1, true, false); return;   // MSUBq
    case 0x26: fmac(kOpSub, false, -1, false, true); return;    // SUBi
    case 0x27: fmac(kOpMsub, false, -1, false, true); return;   // MSUBi
    case 0x28: fmac(kOpAdd, false, -1, false, false); return;   // ADD
    case 0x29: fmac(kOpMadd, false, -1, false, false); return;  // MADD
    case 0x2A: fmac(kOpMul, false, -1, false, false); return;   // MUL
    case 0x2B: minmax(true, -1, false, false); return;          // MAX
    case 0x2C: fmac(kOpSub, false, -1, false, false); return;   // SUB
    case 0x2D: fmac(kOpMsub, false, -1, false, false); return;  // MSUB
    case 0x2E: opmsub(); return;                                // OPMSUB
    case 0x2F: minmax(false, -1, false, false); return;         // MINI
    default: break;
  }
  if (op >= 0x3C) {
    // Function-designator tables: the upper opcode selects the table, the fd field the operation.
    const int table = static_cast<int>(op - 0x3C), fn = static_cast<int>(fd_);
    if (fn == 11 && table == 3) return;  // NOP
    if ((table == 0 && fn == 4) || (table == 1 && fn == 4) || (table == 2 && fn == 4) || (table == 3 && fn == 4)) {
      int_to_float(table == 0 ? 0 : table == 1 ? 4 : table == 2 ? 12 : 15);
      return;
    }
    if ((table == 0 && fn == 5) || (table == 1 && fn == 5) || (table == 2 && fn == 5) || (table == 3 && fn == 5)) {
      float_to_int(table == 0 ? 0 : table == 1 ? 4 : table == 2 ? 12 : 15);
      return;
    }
    if (table == 1 && fn == 7) {  // ABS
      if (upper_ft_ == 0) return;
      uint32_t* dst = reg_dest(upper_ft_);
      for (int lane = 0; lane < 4; ++lane)
        if (mask_ & (8u >> static_cast<unsigned>(lane)))
          dst[lane] = vf_[upper_fs_][static_cast<size_t>(lane)] & 0x7FFFFFFFu;
      return;
    }
    if (table == 3 && fn == 7) {  // CLIP
      clip();
      return;
    }
    if (table == 2 && fn == 11) {  // OPMULA
      opmula();
      return;
    }
    // ACC destinations. Lane broadcasts: x/y/z/w for the suffixed forms; the un-suffixed
    // variants (fn 10/11) take ft component-wise.
    const int bc = (fn <= 3) ? fn : -1;
    bool acc_op = true, use_q = false, use_i = false;
    int fam = kOpAdd;
    switch (table) {
      case 0:
        if (fn <= 3)
          fam = static_cast<int>(fn) == 0 ? kOpAdd : fn == 1 ? kOpSub : fn == 2 ? kOpMadd : kOpMsub;
        else if (fn == 6) {
          fam = kOpMul;
        } else if (fn == 7) {
          fam = kOpMul;
          use_q = true;
        } else if (fn == 8) {
          fam = kOpAdd;
          use_q = true;
        } else if (fn == 9) {
          fam = kOpSub;
          use_q = true;
        } else if (fn == 10) {
          fam = kOpAdd;
        } else if (fn == 11) {
          fam = kOpSub;
        } else
          acc_op = false;
        break;
      case 1:
        if (fn <= 3)
          fam = fn == 0 ? kOpAdd : fn == 1 ? kOpSub : fn == 2 ? kOpMadd : kOpMsub;
        else if (fn == 6) {
          fam = kOpMul;
        } else if (fn == 8) {
          fam = kOpMadd;
          use_q = true;
        } else if (fn == 9) {
          fam = kOpMsub;
          use_q = true;
        } else if (fn == 10) {
          fam = kOpMadd;
        } else if (fn == 11) {
          fam = kOpMsub;
        } else
          acc_op = false;
        break;
      case 2:
        if (fn <= 3)
          fam = fn == 0 ? kOpAdd : fn == 1 ? kOpSub : fn == 2 ? kOpMadd : kOpMsub;
        else if (fn == 6) {
          fam = kOpMul;
        } else if (fn == 7) {
          fam = kOpMul;
          use_i = true;
        } else if (fn == 8) {
          fam = kOpAdd;
          use_i = true;
        } else if (fn == 9) {
          fam = kOpSub;
          use_i = true;
        } else if (fn == 10) {
          fam = kOpMul;
        } else
          acc_op = false;
        break;
      default:
        if (fn <= 3)
          fam = fn == 0 ? kOpAdd : fn == 1 ? kOpSub : fn == 2 ? kOpMadd : fn == 3 ? kOpMsub : kOpAdd;
        else if (fn == 6) {
          fam = kOpMul;
        } else if (fn == 8) {
          fam = kOpMadd;
          use_i = true;
        } else if (fn == 9) {
          fam = kOpMsub;
          use_i = true;
        } else
          acc_op = false;
        break;
    }
    if (acc_op) {
      fmac(fam, true, bc, use_q, use_i);
      return;
    }
  }
  // An unknown encoding stops the program (the VU traps on an illegal instruction). Continuing
  // would execute garbage and spin, which is far worse than a visible stop.
  ++stats_.unknown;
  if (stats_.unknown_ops[(op_hi_ << 8) | op_lo_]++ == 0) {
    dump_recent();
    log::warn(kLog, "illegal upper opcode 0x{:02x} (fn {}) at 0x{:04x}", op_hi_, fd_, (tpc_ - 8) & kMicroMask);
  }
  running_ = false;
  faulted_ = true;
}

// ---------------------------------------------------------------------------------------------
// Lower instructions

void Vu1::exec_lower() {
  const uint32_t op = op_lo_;
  const int32_t imm = sign_extend_11(imm_);
  auto taken = [&](uint32_t target) {
    branch_target_ = target & kMicroMask;
    branch_in_ = 1;  // effective after the delay slot
  };
  auto imm_target = [&]() { return static_cast<uint32_t>(static_cast<int32_t>(tpc_) + imm * 8); };

  switch (op) {
    case 0x00: {  // LQ
      if (ft_ == 0) return;
      const uint32_t addr = (static_cast<uint32_t>(static_cast<uint16_t>(vi_[is_] + imm)) * 16) & kDataMask;
      const uint32_t* q = reinterpret_cast<const uint32_t*>(data_at(addr));
      for (int lane = 0; lane < 4; ++lane)
        if (lmask_ & (8u >> static_cast<unsigned>(lane))) vf_[ft_][static_cast<size_t>(lane)] = q[lane];
      return;
    }
    case 0x01: {  // SQ: data from fs, base in it
      const uint32_t addr = (static_cast<uint32_t>(static_cast<uint16_t>(vi_[it_] + imm)) * 16) & kDataMask;
      uint32_t* q = reinterpret_cast<uint32_t*>(data_at(addr));
      for (int lane = 0; lane < 4; ++lane)
        if (lmask_ & (8u >> static_cast<unsigned>(lane))) q[lane] = vf_[fs_][static_cast<size_t>(lane)];
      return;
    }
    case 0x04:    // ILW
    case 0x05: {  // ISW
      const uint32_t addr = (static_cast<uint32_t>(static_cast<uint16_t>(vi_[is_] + imm)) * 16) & kDataMask;
      uint16_t* q = reinterpret_cast<uint16_t*>(data_at(addr));
      for (int lane = 0; lane < 4; ++lane) {
        if (!(lmask_ & (8u >> static_cast<unsigned>(lane)))) continue;
        if (op == 0x04) {
          if (it_) vi_[it_] = q[lane * 2];
        } else {
          q[lane * 2] = vi_[it_];
          q[lane * 2 + 1] = 0;
        }
      }
      return;
    }
    case 0x08:  // IADDIU
      if (it_) vi_[it_] = static_cast<uint16_t>(vi_[is_] + (((lo_ >> 10) & 0x7800u) | (lo_ & 0x7FFu)));
      return;
    case 0x09:  // ISUBIU
      if (it_) vi_[it_] = static_cast<uint16_t>(vi_[is_] - (((lo_ >> 10) & 0x7800u) | (lo_ & 0x7FFu)));
      return;
    case 0x10: vi_[1] = ((clip_ & 0xFFFFFFu) == (lo_ & 0xFFFFFFu)) ? 1 : 0; return;  // FCEQ
    case 0x11: clip_ = lo_ & 0xFFFFFFu; return;                                      // FCSET
    case 0x12: vi_[1] = ((clip_ & 0xFFFFFFu) & (lo_ & 0xFFFFFFu)) ? 1 : 0; return;   // FCAND
    case 0x13:                                                                       // FCOR
      vi_[1] = (((clip_ & 0xFFFFFFu) | (lo_ & 0xFFFFFFu)) == 0xFFFFFFu) ? 1 : 0;
      return;
    case 0x14: {  // FSEQ
      const uint32_t v = (((lo_ >> 21) & 1u) << 11) | (lo_ & 0x7FFu);
      if (it_) vi_[it_] = ((status_ & 0xFFFu) == v) ? 1 : 0;
      return;
    }
    case 0x15: {  // FSSET
      const uint32_t v = (((lo_ >> 21) & 1u) << 11) | (lo_ & 0x7FFu);
      status_ = (v & 0xFC0u) | (status_ & 0x3Fu);
      return;
    }
    case 0x16: {  // FSAND
      const uint32_t v = (((lo_ >> 21) & 1u) << 11) | (lo_ & 0x7FFu);
      if (it_) vi_[it_] = static_cast<uint16_t>((status_ & 0xFFFu) & v);
      return;
    }
    case 0x17: {  // FSOR
      const uint32_t v = (((lo_ >> 21) & 1u) << 11) | (lo_ & 0x7FFu);
      if (it_) vi_[it_] = static_cast<uint16_t>((status_ & 0xFFFu) | v);
      return;
    }
    case 0x18:  // FMEQ
      if (it_) vi_[it_] = ((mac_ & 0xFFFFu) == vi_[is_]) ? 1 : 0;
      return;
    case 0x1A:  // FMAND
      if (it_) vi_[it_] = static_cast<uint16_t>(vi_[is_] & (mac_ & 0xFFFFu));
      return;
    case 0x1B:  // FMOR
      if (it_) vi_[it_] = static_cast<uint16_t>(vi_[is_] | (mac_ & 0xFFFFu));
      return;
    case 0x1C:  // FCGET
      if (it_) vi_[it_] = static_cast<uint16_t>(clip_ & 0x0FFF);
      return;
    case 0x20: taken(imm_target()); return;  // B
    case 0x21:                               // BAL
      if (it_) vi_[it_] = static_cast<uint16_t>((tpc_ + 8) / 8);
      taken(imm_target());
      return;
    case 0x24: taken(static_cast<uint32_t>(vi_[is_]) * 8); return;  // JR
    case 0x25:                                                      // JALR
      if (it_) vi_[it_] = static_cast<uint16_t>((tpc_ + 8) / 8);
      taken(static_cast<uint32_t>(vi_[is_]) * 8);
      return;
    case 0x28:  // IBEQ
      if (static_cast<int16_t>(vi_[it_]) == static_cast<int16_t>(vi_[is_])) taken(imm_target());
      return;
    case 0x29:  // IBNE
      if (static_cast<int16_t>(vi_[it_]) != static_cast<int16_t>(vi_[is_])) taken(imm_target());
      return;
    case 0x2C:  // IBLTZ
      if (static_cast<int16_t>(vi_[is_]) < 0) taken(imm_target());
      return;
    case 0x2D:  // IBGTZ
      if (static_cast<int16_t>(vi_[is_]) > 0) taken(imm_target());
      return;
    case 0x2E:  // IBLEZ
      if (static_cast<int16_t>(vi_[is_]) <= 0) taken(imm_target());
      return;
    case 0x2F:  // IBGEZ
      if (static_cast<int16_t>(vi_[is_]) >= 0) taken(imm_target());
      return;
    default: break;
  }

  if (op != 0x40) {  // LowerOP special forms live under opcode 0x40
    ++stats_.unknown;
    if (stats_.unknown_ops[(op_hi_ << 8) | op_lo_]++ == 0) {
      dump_recent();
      log::warn(kLog, "illegal lower opcode 0x{:02x} at 0x{:04x}", op_lo_, (tpc_ - 8) & kMicroMask);
    }
    running_ = false;
    faulted_ = true;
    return;
  }

  const uint32_t sub = lo_ & 0x3F, fn = (lo_ >> 6) & 0x1F;
  if (sub >= 0x3C) {
    const int table = static_cast<int>(sub - 0x3C);
    auto qword_load = [&](uint32_t qaddr) {
      if (ft_ == 0) return;
      const uint32_t* q = reinterpret_cast<const uint32_t*>(data_at((qaddr * 16) & kDataMask));
      for (int lane = 0; lane < 4; ++lane)
        if (lmask_ & (8u >> static_cast<unsigned>(lane))) vf_[ft_][static_cast<size_t>(lane)] = q[lane];
    };
    auto qword_store = [&](uint32_t qaddr) {
      uint32_t* q = reinterpret_cast<uint32_t*>(data_at((qaddr * 16) & kDataMask));
      for (int lane = 0; lane < 4; ++lane)
        if (lmask_ & (8u >> static_cast<unsigned>(lane))) q[lane] = vf_[fs_][static_cast<size_t>(lane)];
    };
    if (table == 0 && fn == 12) {  // MOVE
      if (ft_ == 0) return;
      for (int lane = 0; lane < 4; ++lane)
        if (lmask_ & (8u >> static_cast<unsigned>(lane)))
          vf_[ft_][static_cast<size_t>(lane)] = vf_[fs_][static_cast<size_t>(lane)];
      return;
    }
    if (table == 0 && fn == 13) {  // LQI
      qword_load(vi_[is_]);
      if (fs_ != 0) vi_[is_] = static_cast<uint16_t>(vi_[is_] + 1);
      return;
    }
    if (table == 0 && fn == 14) {  // DIV
      const int fsf = static_cast<int>((lo_ >> 21) & 3), ftf = static_cast<int>((lo_ >> 23) & 3);
      const float a = f(fs_, fsf), b = f(ft_, ftf);
      status_ &= ~0x30u;
      if (b == 0.0f) {
        status_ |= (a == 0.0f) ? 0x10u : 0x20u;
        const bool neg = ((vf_[fs_][static_cast<size_t>(fsf)] ^ vf_[ft_][static_cast<size_t>(ftf)]) & 0x80000000u) != 0;
        q_ = neg ? 0xFF7FFFFFu : 0x7F7FFFFFu;
      } else {
        q_ = bits(a / b);
      }
      return;
    }
    if (table == 0 && fn == 15) {  // MTIR
      if (it_) vi_[it_] = static_cast<uint16_t>(vf_[fs_][static_cast<size_t>((lo_ >> 21) & 3)] & 0xFFFFu);
      return;
    }
    if (table == 0 && fn == 16) {  // RNEXT (24-bit LFSR)
      if (ft_ == 0) return;
      r_ = 0x3F800000u | (((r_ << 1) | (((r_ >> 23) ^ (r_ >> 22) ^ (r_ >> 21) ^ (r_ >> 16)) & 1u)) & 0x007FFFFFu);
      for (int lane = 0; lane < 4; ++lane)
        if (lmask_ & (8u >> static_cast<unsigned>(lane))) vf_[ft_][static_cast<size_t>(lane)] = r_;
      return;
    }
    if (table == 0 && fn == 25) {  // MFP
      if (ft_ == 0) return;
      for (int lane = 0; lane < 4; ++lane)
        if (lmask_ & (8u >> static_cast<unsigned>(lane))) vf_[ft_][static_cast<size_t>(lane)] = p_;
      return;
    }
    if (table == 0 && fn == 26) {  // XTOP
      if (it_) vi_[it_] = vif_top_ ? vif_top_() : 0;
      return;
    }
    if (table == 0 && fn == 27) {  // XGKICK: start a GIF PATH1 transfer
      const uint32_t addr = (static_cast<uint32_t>(vi_[is_]) & 0x3FFu) * 16u;
      const uint32_t total = gs::gif_packet_qwords(mem_.data.data(), kDataSize, addr);
      ++stats_.xgkicks;
      uint32_t left = total, at = addr;
      while (left) {
        const uint32_t contig = std::min<uint32_t>(left, (kDataSize - at) / 16u);
        if (kick_) kick_(mem_.data.data() + at, contig);
        at = (at + contig * 16u) & kDataMask;
        left -= contig;
      }
      return;
    }
    if (table == 0 && fn == 28) {  // ESADD
      p_ = bits(f(fs_, 0) * f(fs_, 0) + f(fs_, 1) * f(fs_, 1) + f(fs_, 2) * f(fs_, 2));
      return;
    }
    if (table == 0 && fn == 29) {  // EATANxy
      const float x = f(fs_, 0), y = f(fs_, 1);
      p_ = bits(x == 0.0f ? 0.0f : std::atan(y / x));
      return;
    }
    auto bcfs = [&]() { return f(fs_, static_cast<int>((lo_ >> 21) & 3)); };
    if (table == 0 && fn == 30) {
      p_ = bits(std::sqrt(std::fabs(bcfs())));
      return;
    }  // ESQRT
    if (table == 0 && fn == 31) {
      p_ = bits(std::sin(bcfs()));
      return;
    }  // ESIN
    if (table == 1 && fn == 12) {  // MR32
      if (ft_ == 0) return;
      const uint32_t x = vf_[fs_][0];
      if (lmask_ & 8) vf_[ft_][0] = vf_[fs_][1];
      if (lmask_ & 4) vf_[ft_][1] = vf_[fs_][2];
      if (lmask_ & 2) vf_[ft_][2] = vf_[fs_][3];
      if (lmask_ & 1) vf_[ft_][3] = x;
      return;
    }
    if (table == 1 && fn == 13) {  // SQI
      qword_store(vi_[it_]);
      if (ft_ != 0) vi_[it_] = static_cast<uint16_t>(vi_[it_] + 1);
      return;
    }
    if (table == 1 && fn == 14) {  // SQRT
      const float v = f(ft_, static_cast<int>((lo_ >> 23) & 3));
      status_ &= ~0x30u;
      if (v < 0.0f) status_ |= 0x10u;
      q_ = bits(std::sqrt(std::fabs(v)));
      return;
    }
    if (table == 1 && fn == 15) {  // MFIR
      if (ft_ == 0) return;
      const uint32_t v = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(vi_[is_])));
      for (int lane = 0; lane < 4; ++lane)
        if (lmask_ & (8u >> static_cast<unsigned>(lane))) vf_[ft_][static_cast<size_t>(lane)] = v;
      return;
    }
    if (table == 1 && fn == 16) {  // RGET
      if (ft_ == 0) return;
      for (int lane = 0; lane < 4; ++lane)
        if (lmask_ & (8u >> static_cast<unsigned>(lane))) vf_[ft_][static_cast<size_t>(lane)] = r_;
      return;
    }
    if (table == 1 && fn == 26) {  // XITOP
      if (it_) vi_[it_] = vif_itop_ ? vif_itop_() : 0;
      return;
    }
    if (table == 1 && fn == 28) {  // ERSADD
      const float s = f(fs_, 0) * f(fs_, 0) + f(fs_, 1) * f(fs_, 1) + f(fs_, 2) * f(fs_, 2);
      p_ = bits(s != 0.0f ? 1.0f / s : 0.0f);
      return;
    }
    if (table == 1 && fn == 29) {  // EATANxz
      const float x = f(fs_, 0), z = f(fs_, 2);
      p_ = bits(x == 0.0f ? 0.0f : std::atan(z / x));
      return;
    }
    if (table == 1 && fn == 30) {  // ERSQRT
      const float v = bcfs();
      p_ = bits(v != 0.0f ? 1.0f / std::sqrt(std::fabs(v)) : 0.0f);
      return;
    }
    if (table == 1 && fn == 31) {
      p_ = bits(std::atan(bcfs()));
      return;
    }  // EATAN
    if (table == 2 && fn == 13) {  // LQD
      vi_[is_] = static_cast<uint16_t>(vi_[is_] - 1);
      qword_load(vi_[is_]);
      return;
    }
    if (table == 2 && fn == 14) {  // RSQRT
      const int fsf = static_cast<int>((lo_ >> 21) & 3), ftf = static_cast<int>((lo_ >> 23) & 3);
      const float a = f(fs_, fsf), b = f(ft_, ftf);
      const bool neg = ((vf_[fs_][static_cast<size_t>(fsf)] ^ vf_[ft_][static_cast<size_t>(ftf)]) & 0x80000000u) != 0;
      status_ &= ~0x30u;
      if (b == 0.0f) {
        status_ |= 0x20u;
        if (a != 0.0f) {
          q_ = neg ? 0xFF7FFFFFu : 0x7F7FFFFFu;
        } else {
          q_ = neg ? 0x80000000u : 0u;
          status_ |= 0x10u;
        }
      } else {
        if (b < 0.0f) status_ |= 0x10u;
        q_ = bits(a / std::sqrt(std::fabs(b)));
      }
      return;
    }
    if (table == 2 && fn == 15) {  // ILWR
      if (it_ == 0) return;
      const uint16_t* q =
          reinterpret_cast<const uint16_t*>(data_at((static_cast<uint32_t>(vi_[is_]) * 16) & kDataMask));
      for (int lane = 0; lane < 4; ++lane)
        if (lmask_ & (8u >> static_cast<unsigned>(lane))) vi_[it_] = q[lane * 2];
      return;
    }
    if (table == 2 && fn == 16) {  // RINIT
      r_ = 0x3F800000u | (vf_[fs_][static_cast<size_t>((lo_ >> 21) & 3)] & 0x007FFFFFu);
      return;
    }
    if (table == 2 && fn == 28) {  // ELENG
      const float s = f(fs_, 0) * f(fs_, 0) + f(fs_, 1) * f(fs_, 1) + f(fs_, 2) * f(fs_, 2);
      p_ = bits(std::sqrt(std::fabs(s)));
      return;
    }
    if (table == 2 && fn == 29) {  // ESUM
      p_ = bits(f(fs_, 0) + f(fs_, 1) + f(fs_, 2));
      return;
    }
    if (table == 2 && fn == 30) {  // ERCPR
      const float v = bcfs();
      p_ = bits(v != 0.0f ? 1.0f / v : 0.0f);
      return;
    }
    if (table == 2 && fn == 31) {  // EEXP
      p_ = bits(std::exp2(bcfs()));
      return;
    }
    if (table == 3 && fn == 13) {  // SQD
      vi_[it_] = static_cast<uint16_t>(vi_[it_] - 1);
      qword_store(vi_[it_]);
      return;
    }
    if (table == 3 && fn == 14) return;  // WAITQ
    if (table == 3 && fn == 15) {        // ISWR
      uint16_t* q = reinterpret_cast<uint16_t*>(data_at((static_cast<uint32_t>(vi_[is_]) * 16) & kDataMask));
      for (int lane = 0; lane < 4; ++lane)
        if (lmask_ & (8u >> static_cast<unsigned>(lane))) {
          q[lane * 2] = vi_[it_];
          q[lane * 2 + 1] = 0;
        }
      return;
    }
    if (table == 3 && fn == 16) {  // RXOR
      r_ = 0x3F800000u | ((r_ ^ vf_[fs_][static_cast<size_t>((lo_ >> 21) & 3)]) & 0x007FFFFFu);
      return;
    }
    if (table == 3 && fn == 28) {  // ERLENG
      const float s = f(fs_, 0) * f(fs_, 0) + f(fs_, 1) * f(fs_, 1) + f(fs_, 2) * f(fs_, 2);
      const float l = std::sqrt(std::fabs(s));
      p_ = bits(l != 0.0f ? 1.0f / l : 0.0f);
      return;
    }
    if (table == 3 && fn == 30) return;  // WAITP
  }

  ++stats_.unknown;
  if (stats_.unknown_ops[(op_hi_ << 8) | op_lo_]++ == 0) {
    dump_recent();
    log::warn(kLog, "illegal lower opcode 0x{:02x} (sub 0x{:02x} fn 0x{:02x}) at 0x{:04x}", op_lo_, sub, fn,
              (tpc_ - 8) & kMicroMask);
  }
  running_ = false;
  faulted_ = true;
}

}  // namespace lantern::ps2

namespace lantern::ps2 {
void Vu1::dump_recent() const {
  static bool dumped = false;
  if (!dumped) {
    if (const char* path = std::getenv("LANTERN_VU1_DUMP_LATE")) {
      dumped = true;
      if (FILE* f = std::fopen(path, "wb")) {
        std::fwrite(mem_.micro.data(), 1, mem_.micro.size(), f);
        std::fclose(f);
        log::warn(kLog, "dumped VU1 micro memory at the first unknown opcode");
      }
    }
  }
  std::string s;
  for (size_t i = 0; i < recent_.size(); ++i) {
    const uint32_t pc = recent_[(recent_pos_ + i) % recent_.size()];
    uint32_t lo = 0, hi = 0;
    if (pc) {
      std::memcpy(&lo, mem_.micro.data() + pc, 4);
      std::memcpy(&hi, mem_.micro.data() + pc + 4, 4);
    }
    s += std::format(" {:04x}[{:08x}:{:08x}]", pc, hi, lo);
  }
  log::warn(kLog, "recent PCs (hi:lo):{}", s);
}
}  // namespace lantern::ps2
