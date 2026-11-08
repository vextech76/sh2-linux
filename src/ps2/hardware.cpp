#include "ps2/hardware.h"

#include <cstdlib>
#include <cstring>
#include <format>

#include "core/log.h"
#include "gs/gs.h"
#include "ps2/ipu.h"

namespace lantern::ps2 {
namespace {
constexpr uint32_t kIStat = 0x1000F000, kIMask = 0x1000F010, kDStat = 0x1000E010;
constexpr uint32_t kGsCsr = 0x12001000;
constexpr uint32_t kDmaBase[Hardware::kDmaChannels] = {0x10008000, 0x10009000, 0x1000A000, 0x1000B000, 0x1000B400,
                                                       0x1000C000, 0x1000C400, 0x1000C800, 0x1000D000, 0x1000D400};
constexpr const char* kDmaName[Hardware::kDmaChannels] = {"VIF0", "VIF1", "GIF",  "IPU_FROM", "IPU_TO",
                                                          "SIF0", "SIF1", "SIF2", "SPR_FROM", "SPR_TO"};
constexpr double kBusHz = 147456000.0;
constexpr double kHsyncHz = 15734.0;
}  // namespace

Hardware::Hardware() {
  // The IOP is high-level emulated and always "booted": SMFLG reports SIFINIT | CMDINIT | BOOTEND,
  // and SMCOM holds the IOP-side command buffer address the EE sends SIF commands to.
  sif_[4] = 0x00070000;
  sif_[2] = 0x000A0000;  // SMCOM
  const auto t0 = std::chrono::steady_clock::now();
  clock_ = [t0] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
}

std::string Hardware::region_name(uint32_t a) {
  if (a >= 0x10000000 && a < 0x10002000) return std::format("TIMER{}", (a - 0x10000000) / 0x800);
  if (a >= 0x10002000 && a < 0x10003000) return "IPU";
  if (a >= 0x10003000 && a < 0x10003800) return "GIF";
  if (a >= 0x10003800 && a < 0x10004000) return "VIF0";
  if (a >= 0x10004000 && a < 0x10008000) return "VIF/FIFO";
  for (int c = 0; c < kDmaChannels; ++c)
    if (a >= kDmaBase[c] && a < kDmaBase[c] + 0x400) return std::format("DMA{}({})", c, kDmaName[c]);
  if (a >= 0x1000E000 && a < 0x1000F000) return "DMAC";
  if (a >= 0x1000F000 && a < 0x1000F100) return "INTC";
  if (a >= 0x1000F200 && a < 0x1000F300) return "SIF";
  if (a >= 0x12000000) return "GS-priv";
  return "EE-hw";
}

uint32_t Hardware::timer_count(int t) const {
  const Timer& tm = timers_[t];
  if (!(tm.mode & 0x80)) return tm.base & 0xFFFF;  // CUE: counting enabled
  static constexpr double kDiv[4] = {1, 16, 256, 0};
  const unsigned clks = tm.mode & 3;
  const double rate = clks == 3 ? kHsyncHz : kBusHz / kDiv[clks];
  const double ticks = (clock_() - tm.start) * rate;
  return static_cast<uint32_t>(tm.base + static_cast<uint64_t>(ticks)) & 0xFFFF;
}

// SIF registers: 1 MSCOM, 2 SMCOM, 3 MSFLG (EE->IOP flags), 4 SMFLG (IOP->EE flags).
uint32_t Hardware::sif_reg(uint32_t reg) {
  if (reg == 4 && iop_reboot_pending_) {
    iop_reboot_pending_ = false;
    sif_[4] |= 0x00070000;  // SIFINIT | CMDINIT | BOOTEND
  }
  return reg < 8 ? sif_[reg] : 0;
}
void Hardware::set_sif_reg(uint32_t reg, uint32_t v) {
  if (reg == 4)
    sif_[4] &= ~v;  // EE side: write 1 to clear IOP->EE flags
  else if (reg == 3)
    sif_[3] |= v;  // EE->IOP flags: set
  else if (reg < 8)
    sif_[reg] = v;
}

namespace {
void hw_trace(const char* op, uint32_t a, uint32_t v, Ipu* ipu, uint32_t pc) {
  static const bool on = std::getenv("LANTERN_HW_TRACE") != nullptr;
  static int lines = 0;
  static uint32_t last_a = 0, last_v = 0, repeats = 0;
  static const uint64_t from_mb =
      std::getenv("LANTERN_HW_TRACE") ? std::strtoull(std::getenv("LANTERN_HW_TRACE"), nullptr, 10) : 0;
  if (!on || !ipu || ipu->stats().macroblocks < from_mb || lines > 3000) return;
  if (!((a >= 0x10002000 && a < 0x10002040) || (a >= 0x1000B000 && a < 0x1000B500))) return;
  if (op[0] == 'R' && (a == 0x10002000 || a == 0x10002004 || a == 0x10002010)) return;
  if (a == last_a && v == last_v) {
    ++repeats;
    return;
  }
  if (repeats) log::info("hwtrace", "  (previous access repeated {} times)", repeats);
  repeats = 0;
  last_a = a;
  last_v = v;
  ++lines;
  log::info("hwtrace", "{} 0x{:08x} = 0x{:08x} pc 0x{:08x}", op, a, v, pc);
}
}  // namespace

uint32_t Hardware::read32(uint32_t a) {
  ++all_reads_[a];
  const uint32_t v = read32_impl(a);
  hw_trace("R", a, v, ipu_, pc_source_ ? pc_source_() : 0);
  return v;
}

uint32_t Hardware::read32_impl(uint32_t a) {
  if (a >= 0x10000000 && a < 0x10002000) {
    const int t = static_cast<int>((a - 0x10000000) / 0x800);
    switch (a & 0x7FF) {
      case 0x00: return timer_count(t);
      case 0x10: return timers_[t].mode;
      default: break;
    }
  }
  switch (a) {
    case kIStat: return intc_stat_;
    case kIMask: return intc_mask_;
    case kDStat: return dmac_stat_;
    case kGsCsr:
      if (gs_) return gs_->read_priv32(0x1000);
      return (vblanks_ & 1 ? 1u << 13 : 0) | (0x1Bu << 16) | (0x55u << 24);  // FIELD, REV, ID
    default: break;
  }
  if (ipu_ && Ipu::handles(a)) return ipu_->read32(a);
  for (int c = 0; c < kDmaChannels; ++c)
    if (a == kDmaBase[c]) {
      // CHCR.STR: clear for instant transfers, set while an asynchronous channel is running.
      return (async_active_ & (1u << c)) ? (regs_[a] | 0x100u) : (regs_[a] & ~0x100u);
    }
  if (gs_ && a >= 0x12000000 && a < 0x12002000) return gs_->read_priv32(a - 0x12000000);
  if (a >= 0x1000F200 && a < 0x1000F240) return sif_reg((a - 0x1000F200) / 0x10 + 1);  // MSCOM..SMFLG = 1..4
  auto& acc = unmodelled_[a];
  ++acc.reads;
  return regs_[a];
}

void Hardware::write32(uint32_t a, uint32_t v) {
  hw_trace("W", a, v, ipu_, pc_source_ ? pc_source_() : 0);
  if (a >= 0x10000000 && a < 0x10002000) {
    const int t = static_cast<int>((a - 0x10000000) / 0x800);
    switch (a & 0x7FF) {
      case 0x00:
        timers_[t].base = v & 0xFFFF;
        timers_[t].start = clock_();
        return;
      case 0x10:
        timers_[t].base = timer_count(t);
        timers_[t].start = clock_();
        timers_[t].mode = v & 0x3FF;
        return;
      default: break;
    }
  }
  switch (a) {
    case kIStat: intc_stat_ &= ~v; return;  // write 1 to clear
    case kIMask: intc_mask_ ^= v; return;   // write 1 to toggle
    case kDStat:
      dmac_stat_ &= ~(v & 0xFFFF);      // status: write 1 to clear
      dmac_stat_ ^= (v & 0xFFFF0000u);  // mask: write 1 to toggle
      return;
    default: break;
  }
  if (ipu_ && Ipu::handles(a)) {
    ipu_->write32(a, v);
    return;
  }
  for (int c = 0; c < kDmaChannels; ++c) {
    if (a != kDmaBase[c]) continue;
    regs_[a] = v;
    if ((v & 0x100) && ipu_ && (c == 3 || c == 4)) {  // IPU_FROM / IPU_TO run asynchronously
      ++dma_starts_[c];
      async_active_ |= 1u << c;
      ipu_->dma_started(c);
      return;
    }
    if (!(v & 0x100) && (c == 3 || c == 4)) {  // guest stopped an IPU channel
      async_active_ &= ~(1u << c);
      if (ipu_) ipu_->dma_stopped(c);
    }
    if (v & 0x100) {  // STR: transfer start
      const bool done = dma_hook_ && dma_hook_(c);
      if (dma_starts_[c]++ == 0 && !done)
        log::warn("hw", "PLACEHOLDER: DMA channel {} ({}) started - {} is not emulated yet; transfer discarded", c,
                  kDmaName[c], kDmaName[c]);
      dmac_stat_ |= 1u << c;
      completed_dma_ |= 1u << c;
    }
    return;
  }
  if (gs_ && a >= 0x12000000 && a < 0x12002000) {
    gs_->write_priv32(a - 0x12000000, v);
    return;
  }
  if (a >= 0x1000F200 && a < 0x1000F240) {
    set_sif_reg((a - 0x1000F200) / 0x10 + 1, v);
    return;
  }
  auto& acc = unmodelled_[a];
  ++acc.writes;
  acc.last_value = v;
  regs_[a] = v;
}

void Hardware::read(uint32_t addr, void* out, unsigned size) {
  if (size <= 4) {
    uint32_t v = read32(addr & ~3u) >> (8 * (addr & 3));
    std::memcpy(out, &v, size);
    return;
  }
  // 64/128-bit accesses (FIFOs, GS registers): split into words.
  for (unsigned i = 0; i < size; i += 4) {
    uint32_t v = read32(addr + i);
    std::memcpy(static_cast<uint8_t*>(out) + i, &v, 4);
  }
}

void Hardware::write(uint32_t addr, const void* in, unsigned size) {
  if (size <= 4) {
    uint32_t v = 0;
    std::memcpy(&v, in, size);
    if (size < 4) {
      const uint32_t shift = 8 * (addr & 3), mask = ((1u << (8 * size)) - 1) << shift;
      v = (regs_[addr & ~3u] & ~mask) | ((v << shift) & mask);
    }
    write32(addr & ~3u, v);
    return;
  }
  for (unsigned i = 0; i < size; i += 4) {
    uint32_t v;
    std::memcpy(&v, static_cast<const uint8_t*>(in) + i, 4);
    write32(addr + i, v);
  }
}

}  // namespace lantern::ps2

namespace lantern::ps2 {
void Hardware::dma_done(int channel) {
  if (!(async_active_ & (1u << channel))) return;
  async_active_ &= ~(1u << channel);
  regs_[kDmaBase[channel]] &= ~0x100u;
  dmac_stat_ |= 1u << channel;
  completed_dma_ |= 1u << channel;
}
bool Hardware::take_gs_interrupt() { return gs_ && gs_->take_interrupt(); }
void Hardware::set_gs_imr(uint32_t imr) {
  if (gs_) gs_->set_imr(imr);
}
}  // namespace lantern::ps2
