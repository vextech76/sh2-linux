#pragma once

#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace lantern::iop {

// Raised for guest conditions the IOP model does not handle; never swallowed.
struct Trap : std::runtime_error {
  uint32_t pc;
  Trap(uint32_t at, const std::string& what) : std::runtime_error(what), pc(at) {}
};

struct IopCpu {
  uint32_t gpr[32]{};
  uint32_t hi = 0, lo = 0;
  uint32_t pc = 0;
  uint32_t cop0[32]{};
  // Load delay: the value a load fetched becomes visible only after the following instruction
  // executes (MIPS I). `pending_reg` is -1 when nothing is in flight.
  int pending_reg = -1;
  uint32_t pending_value = 0;

  void set(int reg, uint32_t value) {
    if (reg) gpr[reg] = value;
  }
  uint32_t at(int reg) const { return gpr[reg]; }
};

// IOP address space: 2 MB of RAM (kseg0/kseg1 mirrors) plus an MMIO hook for everything else.
class IopMemory {
 public:
  static constexpr uint32_t kRamSize = 2u << 20;
  static constexpr uint32_t kScratchpadBase = 0x1F800000;  // 1 KB scratchpad
  static constexpr uint32_t kScratchpadSize = 1u << 10;

  using MmioRead = std::function<void(uint32_t addr, void* out, unsigned size)>;
  using MmioWrite = std::function<void(uint32_t addr, const void* in, unsigned size)>;

  IopMemory() : ram_(new uint8_t[kRamSize]()), scratch_(new uint8_t[kScratchpadSize]()) {}

  void set_mmio(MmioRead r, MmioWrite w) {
    mmio_read_ = std::move(r);
    mmio_write_ = std::move(w);
  }

  uint8_t* ram() { return ram_.get(); }
  const uint8_t* ram() const { return ram_.get(); }

  uint8_t* host(uint32_t addr, unsigned size) {
    if (addr >= kScratchpadBase && addr - kScratchpadBase + size <= kScratchpadSize)
      return scratch_.get() + (addr - kScratchpadBase);
    const uint32_t phys = addr & 0x1FFFFFFFu;  // kuseg/kseg0/kseg1 all map to physical here
    if (phys < kRamSize && phys + size <= kRamSize) return ram_.get() + phys;
    return nullptr;
  }

  template <typename T>
  T read(uint32_t addr) {
    T v{};
    if (uint8_t* p = host(addr, sizeof(T))) {
      __builtin_memcpy(&v, p, sizeof(T));
      return v;
    }
    if (!mmio_read_) throw Trap(0, std::format("unmapped IOP read of {} bytes at 0x{:08x}", sizeof(T), addr));
    mmio_read_(addr, &v, sizeof(T));
    return v;
  }

  template <typename T>
  void write(uint32_t addr, T v) {
    if (uint8_t* p = host(addr, sizeof(T))) {
      __builtin_memcpy(p, &v, sizeof(T));
      return;
    }
    if (!mmio_write_) throw Trap(0, std::format("unmapped IOP write of {} bytes at 0x{:08x}", sizeof(T), addr));
    mmio_write_(addr, &v, sizeof(T));
  }

  void load(uint32_t addr, const void* data, size_t size) {
    for (size_t i = 0; i < size; ++i)
      write<uint8_t>(addr + static_cast<uint32_t>(i), static_cast<const uint8_t*>(data)[i]);
  }

 private:
  std::unique_ptr<uint8_t[]> ram_;
  std::unique_ptr<uint8_t[]> scratch_;
  MmioRead mmio_read_;
  MmioWrite mmio_write_;
};

}  // namespace lantern::iop
