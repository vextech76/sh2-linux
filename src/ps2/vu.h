#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>

#include "ps2/vif.h"

namespace lantern::ps2 {

// VU1 micro-mode interpreter. Instruction fields, opcode numbering and the flag/clamp rules
// follow the documented public hardware behaviour (ps2tek "Vector Unit" plus the per-opcode
// semantics of the 64-bit micro instruction encoding).
//
// VU1 is run to completion when VIF1 starts it (MSCAL/MSCNT): a microprogram stops at its E bit.
// Running it to the end synchronously keeps the VIF1/GIF/GS ordering the game relies on and fits
// this runtime's synchronous DMA model. The exposed pipelines (FMAC/FDIV/EFU stalls) are not
// modelled: results are written back immediately, which is correct for hazard-free microcode and
// avoids inventing stall timing that cannot be verified without the hardware.
class Vu1 {
 public:
  // XGKICK: hand `count` quadwords of VU1 data memory to GIF PATH1.
  using Kick = std::function<void(const uint8_t* qwords, uint32_t count)>;

  explicit Vu1(Vu1Memory& mem) : mem_(mem) { reset(); }

  void set_xgkick(Kick fn) { kick_ = std::move(fn); }
  // XTOP/XITOP read VIF1's double-buffered TOP registers.
  void set_vif_tops(std::function<uint16_t()> top, std::function<uint16_t()> itop) {
    vif_top_ = std::move(top);
    vif_itop_ = std::move(itop);
  }

  void reset();

  // Start a microprogram at a byte address (MSCAL). 0xFFFFFFFF continues at the saved TPC (MSCNT).
  void start(uint32_t addr);

  bool running() const { return running_; }
  uint32_t pc() const { return tpc_; }
  bool faulted() const { return faulted_; }  // stopped by the step guard, not by an E bit

  struct Stats {
    uint64_t microprograms = 0, instructions = 0, xgkicks = 0, unknown = 0, faults = 0;
    std::map<uint32_t, uint64_t> unknown_ops;  // (upper << 8 | lower) -> count
  };
  const Stats& stats() const { return stats_; }

 private:
  static constexpr uint32_t kDataSize = 16384, kMicroSize = 16384, kDataMask = kDataSize - 1,
                            kMicroMask = kMicroSize - 1;
  static constexpr uint64_t kStepBudget = 65536;

  // Fields of the current 64-bit instruction.
  uint32_t lo_ = 0, hi_ = 0, op_lo_ = 0, op_hi_ = 0;
  uint32_t fs_ = 0, ft_ = 0, fd_ = 0, is_ = 0, it_ = 0, id_ = 0, mask_ = 0, lmask_ = 0, imm_ = 0;
  uint32_t upper_fs_ = 0, upper_ft_ = 0;

  void step();
  void decode_fields();
  void exec_upper();
  void exec_lower();

  std::array<std::array<uint32_t, 4>, 32> vf_{};  // bit patterns, so results stay exact
  std::array<uint16_t, 16> vi_{};
  std::array<uint32_t, 4> acc_{};
  uint32_t q_ = 0, p_ = 0, i_ = 0, r_ = 0x3F800000u;
  uint32_t status_ = 0, mac_ = 0, clip_ = 0;
  std::array<uint32_t, 4> scratch_{};  // destination when the register field is 0

  // Recent PCs, for the unknown-opcode diagnostic.
  std::array<uint32_t, 32> recent_{};
  size_t recent_pos_ = 0;
  void note_pc() { recent_[recent_pos_++ % recent_.size()] = tpc_; }
  void dump_recent() const;

  uint32_t tpc_ = 0;
  bool running_ = false, faulted_ = false, end_pending_ = false;
  uint32_t branch_target_ = 0;
  int branch_in_ = -1, stop_in_ = -1;

  static uint32_t bits(float v) {
    uint32_t b;
    __builtin_memcpy(&b, &v, 4);
    return b;
  }
  static float from_bits(uint32_t b) {
    float v;
    __builtin_memcpy(&v, &b, 4);
    return v;
  }
  float f(uint32_t reg, int lane) const;  // lane 0..3 = x/y/z/w, vf00 = (0,0,0,1)
  uint32_t* reg_dest(uint32_t reg);
  uint32_t* acc_dest() { return acc_.data(); }

  float mac_write(int shift, float v);  // write one lane back, updating the MAC flags
  void mac_clear(int shift);
  void stat_update();

  void fmac(int op, bool to_acc, int broadcast_lane, bool use_q, bool use_i);
  void minmax(bool max_op, int broadcast_lane, bool use_q, bool use_i);
  void opmula();
  void opmsub();
  void clip();
  void int_to_float(int offset);
  void float_to_int(int offset);

  uint8_t* data_at(uint32_t byte_addr) { return mem_.data.data() + (byte_addr & kDataMask); }

  Vu1Memory& mem_;
  Kick kick_;
  std::function<uint16_t()> vif_top_, vif_itop_;
  Stats stats_;
};

}  // namespace lantern::ps2
