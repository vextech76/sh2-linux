#pragma once

#include <cstdint>
#include <functional>

#include "ee/cpu.h"
#include "ee/memory.h"
#include "ee/r5900.h"

namespace lantern::ee {

// Execute one non-control-flow instruction via the shared semantics. Traps on unimplemented ops.
void execute_plain(Cpu& c, Memory& m, const Insn& insn);
// True if `op` has shared semantics (i.e. execute_plain/the emitter can handle it).
bool has_semantics(Op op);
// Evaluate a conditional branch's condition. Precondition: insn.has(kBranch).
bool branch_taken(const Cpu& c, const Insn& insn);

// Reference interpreter: the semantic ground truth for recompiled code and the fallback for
// code the recompiler has not seen.
class Interpreter {
 public:
  // Optional: called for direct/indirect calls (after the delay slot). Return true if the call
  // was executed natively (e.g. by recompiled code); execution then resumes at the return address.
  using CallHook = std::function<bool(Cpu&, Memory&, uint32_t target)>;
  void set_call_hook(CallHook hook) { call_hook_ = std::move(hook); }

  // Run from c.pc until control reaches `stop_pc` (typically the caller's return address).
  // Returns the number of instructions executed. Throws GuestTrap after `max_steps`.
  uint64_t run(Cpu& c, Memory& m, uint32_t stop_pc, uint64_t max_steps = 1ull << 32);

 private:
  CallHook call_hook_;
};

}  // namespace lantern::ee
