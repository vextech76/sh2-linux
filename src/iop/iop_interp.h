#pragma once

#include <cstdint>
#include <functional>

#include "iop/iop_cpu.h"
#include "iop/r3000.h"

namespace lantern::iop {

// Interpreter for the R3000A. Models the two things the EE does not have: load delay slots and
// branch delay slots; arithmetic is 32-bit only, division follows the MIPS I results.
class Interpreter {
 public:
  using SyscallHook = std::function<void(IopCpu&, IopMemory&, uint32_t pc)>;

  void set_syscall_hook(SyscallHook hook) { syscall_ = std::move(hook); }

  // Execute one instruction, applying any pending load from the previous instruction.
  void step(IopCpu& c, IopMemory& m);
  // Run until pc == stop_pc. Returns the number of instructions executed.
  uint64_t run(IopCpu& c, IopMemory& m, uint32_t stop_pc, uint64_t max_steps = 1u << 30);

  struct Stats {
    uint64_t steps = 0, loads = 0, branches = 0, syscalls = 0;
  };
  const Stats& stats() const { return stats_; }

 private:
  void execute(IopCpu& c, IopMemory& m, const Insn& in);
  bool branch_taken(const IopCpu& c, const Insn& in) const;
  // Loads record their value here instead of writing the register immediately.
  void begin_load(IopCpu& c, int reg, uint32_t value) {
    c.pending_reg = reg;
    c.pending_value = value;
    ++stats_.loads;
  }

  SyscallHook syscall_;
  Stats stats_;
};

}  // namespace lantern::iop
