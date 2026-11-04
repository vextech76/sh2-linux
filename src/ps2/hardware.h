#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace lantern::gs {
class Gs;
}

namespace lantern::ps2 {

// EE-side hardware registers (0x10000000-0x1000FFFF) and GS privileged registers
// (0x12000000-0x12001FFF). Models only what boot needs; everything else is stored and
// recorded so the boot report shows exactly which subsystems the game touched.
class Hardware {
 public:
  using Clock = std::function<double()>;  // seconds since start

  static constexpr int kDmaChannels = 10;
  enum Intc : int { kIntcGs = 0, kIntcSbus = 1, kIntcVblankStart = 2, kIntcVblankEnd = 3, kIntcTimer0 = 9 };

  Hardware();
  void set_clock(Clock c) { clock_ = std::move(c); }
  // DMA execution: called when a channel's CHCR.STR is set; return true if the transfer was
  // performed (otherwise it is logged as a PLACEHOLDER and discarded).
  void set_dma_hook(std::function<bool(int channel)> fn) { dma_hook_ = std::move(fn); }
  void set_gs(gs::Gs* gs) { gs_ = gs; }
  uint32_t dma_reg(uint32_t addr) { return regs_[addr]; }
  void set_dma_reg(uint32_t addr, uint32_t v) { regs_[addr] = v; }
  bool take_gs_interrupt();
  void set_gs_imr(uint32_t imr);

  bool handles(uint32_t addr) const {
    return (addr >= 0x10000000 && addr < 0x10010000) || (addr >= 0x12000000 && addr < 0x12002000);
  }
  void read(uint32_t addr, void* out, unsigned size);
  void write(uint32_t addr, const void* in, unsigned size);

  // Raised by the kernel each frame.
  void raise_intc(int cause) { intc_stat_ |= 1u << cause; }
  uint32_t intc_mask() const { return intc_mask_; }
  void on_vblank() { ++vblanks_; }

  // DMA transfers started by the guest since the last call (bit per channel).
  uint32_t take_completed_dma() {
    uint32_t v = completed_dma_;
    completed_dma_ = 0;
    return v;
  }

  // SIF mailbox flags (MSFLG/SMFLG) exposed to the SIF syscalls.
  uint32_t sif_reg(uint32_t reg);
  void set_sif_reg(uint32_t reg, uint32_t value);
  // The HLE IOP received RESET: it "reboots" and re-raises its SMFLG flags when the EE next polls.
  void iop_reset() { iop_reboot_pending_ = true; }

  struct Access {
    uint64_t reads = 0, writes = 0;
    uint32_t last_value = 0;
  };
  const std::map<uint32_t, Access>& unmodelled() const { return unmodelled_; }
  const std::map<int, uint64_t>& dma_starts() const { return dma_starts_; }
  static std::string region_name(uint32_t addr);

 private:
  uint32_t read32(uint32_t addr);
  void write32(uint32_t addr, uint32_t v);
  uint32_t timer_count(int t) const;

  Clock clock_;
  std::function<bool(int)> dma_hook_;
  gs::Gs* gs_ = nullptr;
  std::map<uint32_t, uint32_t> regs_;
  std::map<uint32_t, Access> unmodelled_;
  std::map<int, uint64_t> dma_starts_;
  uint32_t intc_stat_ = 0, intc_mask_ = 0, dmac_stat_ = 0, completed_dma_ = 0;
  uint64_t vblanks_ = 0;
  struct Timer {
    uint32_t mode = 0, base = 0;
    double start = 0;
  } timers_[4];
  uint32_t sif_[8]{};
  bool iop_reboot_pending_ = false;
};

}  // namespace lantern::ps2
