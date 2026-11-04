#pragma once

#include <cstdint>
#include <cstring>
#include <format>
#include <functional>
#include <memory>

#include "ee/cpu.h"

namespace lantern::ee {

// EE guest address space: 32 MB RDRAM (+ kseg0/kseg1 and uncached-accelerated mirrors),
// 16 KB scratchpad at 0x70000000, and an MMIO hook for everything else. Little-endian.
class Memory {
 public:
  static constexpr uint32_t kRamSize = 32u << 20;
  static constexpr uint32_t kScratchBase = 0x70000000;
  static constexpr uint32_t kScratchSize = 16u << 10;

  // MMIO: called for addresses outside RAM/scratchpad. `size` in bytes (1..16).
  using MmioRead = std::function<void(uint32_t addr, void* out, unsigned size)>;
  using MmioWrite = std::function<void(uint32_t addr, const void* in, unsigned size)>;

  Memory() : ram_(new uint8_t[kRamSize]()), scratch_(new uint8_t[kScratchSize]()) {}

  // Debug watch: reports every access inside [lo, hi) with the guest PC of the accessing
  // instruction (set by the load/store semantics). Off by default.
  struct Watch {
    uint32_t lo = 0, hi = 0;
    std::function<void(uint32_t addr, uint32_t pc, bool write, unsigned size)> fn;
  };
  void set_watch(uint32_t lo, uint32_t hi, std::function<void(uint32_t, uint32_t, bool, unsigned)> fn) {
    watch_ = {lo, hi, std::move(fn)};
  }
  void clear_watch() { watch_ = {}; }
  // Called by the load/store semantics: records who accessed memory, but only while a watch is
  // installed, so the normal path costs one predictable branch.
  void note_access(uint32_t pc, uint32_t ra, uint32_t sp) {
    if (watch_.hi) {
      guest_pc = pc;
      guest_ra = ra;
      guest_sp = sp;
    }
  }
  uint32_t guest_pc = 0;
  uint32_t guest_ra = 0;  // $ra of the accessing function (call-chain hint for watches)
  uint32_t guest_sp = 0;  // $sp at the access, for a guest stack walk in watch handlers

  void set_mmio(MmioRead r, MmioWrite w) {
    mmio_read_ = std::move(r);
    mmio_write_ = std::move(w);
  }

  uint8_t* ram() { return ram_.get(); }
  const uint8_t* ram() const { return ram_.get(); }

  // Host pointer for [addr, addr+size) if it is plain memory, else nullptr.
  uint8_t* host(uint32_t addr, unsigned size) {
    if (addr >= kScratchBase && addr - kScratchBase + size <= kScratchSize)
      return scratch_.get() + (addr - kScratchBase);
    uint32_t phys = addr;
    if (addr >= 0x80000000u && addr < 0xC0000000u)
      phys = addr & 0x1FFFFFFFu;  // kseg0/kseg1
    else if (addr >= 0x20000000u && addr < 0x40000000u)
      phys = addr & 0x0FFFFFFFu;  // uncached mirrors
    if (watch_.hi && !watch_suspended_ && phys >= watch_.lo && phys < watch_.hi && watch_.fn)
      watch_.fn(addr, guest_pc, false, size);  // physical range, so uncached aliases are seen too
    if (phys < kRamSize && phys + size <= kRamSize) return ram_.get() + phys;
    return nullptr;
  }

  // Suspend the watch for our own writes (host code cannot be distinguised from guest writes in
  // host(), so the writer around the watch says so explicitly).
  void suspend_watch(bool on) { watch_suspended_ = on; }

  template <typename T>
  T read(uint32_t addr) {
    T v;
    if (uint8_t* p = host(addr, sizeof(T))) {
      std::memcpy(&v, p, sizeof(T));
      return v;
    }
    mmio(addr, &v, sizeof(T), false);
    return v;
  }

  template <typename T>
  void write(uint32_t addr, T v) {
    if (uint8_t* p = host(addr, sizeof(T))) {
      std::memcpy(p, &v, sizeof(T));
      return;
    }
    mmio(addr, &v, sizeof(T), true);
  }

  void read128(uint32_t addr, Reg128& out) {
    if (uint8_t* p = host(addr, 16)) {
      std::memcpy(&out, p, 16);
      return;
    }
    mmio(addr, &out, 16, false);
  }
  void write128(uint32_t addr, const Reg128& v) {
    if (uint8_t* p = host(addr, 16)) {
      std::memcpy(p, &v, 16);
      return;
    }
    mmio(addr, const_cast<Reg128*>(&v), 16, true);
  }

  void load(uint32_t addr, const void* data, size_t size) {
    for (size_t i = 0; i < size; ++i)
      write<uint8_t>(addr + static_cast<uint32_t>(i), static_cast<const uint8_t*>(data)[i]);
  }

 private:
  void mmio(uint32_t addr, void* buf, unsigned size, bool is_write) {
    if (is_write && mmio_write_) return mmio_write_(addr, buf, size);
    if (!is_write && mmio_read_) return mmio_read_(addr, buf, size);
    throw GuestTrap(0, std::format("unmapped {}-byte {} at 0x{:08x}", size, is_write ? "write" : "read", addr));
  }

  std::unique_ptr<uint8_t[]> ram_;
  std::unique_ptr<uint8_t[]> scratch_;
  Watch watch_;
  bool watch_suspended_ = false;
  MmioRead mmio_read_;
  MmioWrite mmio_write_;
};

}  // namespace lantern::ee
