#pragma once

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

#include "ee/cpu.h"
#include "ee/interpreter.h"
#include "ee/memory.h"

namespace lantern::recomp {

class Runtime;
// A recompiled guest function. Runs until the guest executes `jr` back to its caller and
// returns that target address (normally the caller's $ra).
using GuestFn = uint32_t (*)(ee::Cpu&, ee::Memory&, Runtime&);

// Address -> recompiled function dispatch, with optional interpreter fallback. Overlay code is
// registered per overlay id; addresses inside the overlay window resolve against the active one.
class Runtime {
 public:
  struct Stats {
    uint64_t native_calls = 0;
    uint64_t interpreted_calls = 0;
    uint64_t interpreted_steps = 0;
    uint64_t nonstandard_returns = 0;  // guest continuations: the callee returned elsewhere
  };

  void add(uint32_t addr, GuestFn fn) { main_[addr] = fn; }
  void add_overlay(int overlay, uint32_t addr, GuestFn fn) { overlays_[overlay][addr] = fn; }
  void set_overlay_window(uint32_t base, uint32_t end) {
    ov_base_ = base;
    ov_end_ = end;
  }
  void set_active_overlay(int overlay) { active_overlay_ = overlay; }
  void set_interpreter_fallback(bool on) { fallback_ = on; }

  GuestFn find(uint32_t addr) const;

  // Call `target` with $ra already set to the return address. Returns the address the callee
  // returned to.
  uint32_t call(ee::Cpu& c, ee::Memory& m, uint32_t target);
  // Tail jump to `target` (J/JR to code outside the current function).
  uint32_t jump(ee::Cpu& c, ee::Memory& m, uint32_t target) { return call(c, m, target); }

  [[noreturn]] static void bad_return(uint32_t call_pc, uint32_t expected, uint32_t actual);
  [[noreturn]] static void unimplemented(uint32_t word, uint32_t pc);
  [[noreturn]] static void bad_jump_table(uint32_t pc, uint32_t target);

  const Stats& stats() const { return stats_; }
  // Called by recompiled code when a callee returned to a continuation address.
  uint64_t& nonstandard_returns() { return stats_.nonstandard_returns; }

  // Safe point emitted on every loop back-edge. Every 1024 ticks the hook (the kernel) may
  // deliver interrupts or switch threads, so guest busy-wait loops make progress.
  void set_tick_hook(std::function<void()> hook) { tick_hook_ = std::move(hook); }
  void tick() {
    if ((++ticks_ & 1023) == 0 && tick_hook_) tick_hook_();
  }

 private:
  std::unordered_map<uint32_t, GuestFn> main_;
  std::unordered_map<int, std::unordered_map<uint32_t, GuestFn>> overlays_;
  uint32_t ov_base_ = 0, ov_end_ = 0;
  int active_overlay_ = -1;
  bool fallback_ = true;
  ee::Interpreter interp_;
  bool interp_hooked_ = false;
  Stats stats_;
  std::function<void()> tick_hook_;
  uint64_t ticks_ = 0;
};

}  // namespace lantern::recomp
