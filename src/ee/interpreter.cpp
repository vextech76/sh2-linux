#include "ee/interpreter.h"

#include <format>

#include "ee/semantics.h"
#include "ee/semantics_ops.h"

namespace lantern::ee {

void execute_plain(Cpu& c, Memory& m, const Insn& in) {
  switch (in.op) {
#define LANTERN_CASE(name) \
  case Op::name: sem::name(c, m, in.word, in.pc); return;
    LANTERN_PLAIN_OPS(LANTERN_CASE)
#undef LANTERN_CASE
    default:
      throw GuestTrap(in.pc, std::format("unimplemented instruction '{}' (0x{:08x}) at 0x{:08x}",
                                         in.valid() ? disassemble(in) : "(invalid)", in.word, in.pc));
  }
}

bool has_semantics(Op op) {
  switch (op) {
#define LANTERN_CASE(name) case Op::name:
    LANTERN_PLAIN_OPS(LANTERN_CASE)
#undef LANTERN_CASE
    return true;
    default: return false;
  }
}

bool branch_taken(const Cpu& c, const Insn& in) {
  switch (in.op) {
#define LANTERN_CASE(name, cond, likely, link) \
  case Op::name: return sem::cond(c, in.word);
    LANTERN_BRANCH_OPS(LANTERN_CASE)
#undef LANTERN_CASE
    default: throw GuestTrap(in.pc, std::format("unimplemented branch '{}' at 0x{:08x}", disassemble(in), in.pc));
  }
}

uint64_t Interpreter::run(Cpu& c, Memory& m, uint32_t stop_pc, uint64_t max_steps) {
  uint64_t steps = 0;
  while (c.pc != stop_pc) {
    if (++steps > max_steps) throw GuestTrap(c.pc, "interpreter step budget exhausted");
    const uint32_t pc = c.pc;
    const Insn in = decode(m.read<uint32_t>(pc), pc);
    if (!in.valid()) throw GuestTrap(pc, std::format("invalid instruction 0x{:08x} at 0x{:08x}", in.word, pc));
    if (!in.has(kDelay)) {
      if (in.op == Op::ERET) throw GuestTrap(pc, "ERET outside the HLE kernel");
      execute_plain(c, m, in);
      c.pc = pc + 4;
      continue;
    }

    // Control transfer with a delay slot. Resolve target/condition before the slot executes.
    bool taken = true;
    uint32_t target = in.target;
    if (in.has(kBranch)) taken = branch_taken(c, in);
    if (in.op == Op::JR || in.op == Op::JALR) target = static_cast<uint32_t>(c.r(in.rs));
    if (in.has(kLink)) sem::set(c, in.op == Op::JALR ? in.rd : 31, pc + 8);  // visible to the slot

    if (!taken && in.has(kLikely)) {  // annulled delay slot
      c.pc = pc + 8;
      continue;
    }
    const Insn slot = decode(m.read<uint32_t>(pc + 4), pc + 4);
    if (slot.has(kDelay)) throw GuestTrap(pc + 4, "control transfer in a delay slot");
    if (!slot.valid()) throw GuestTrap(pc + 4, std::format("invalid instruction 0x{:08x} in delay slot", slot.word));
    execute_plain(c, m, slot);
    ++steps;

    if (!taken) {
      c.pc = pc + 8;
      continue;
    }
    const bool is_call = in.has(kLink);
    if (is_call && call_hook_ && call_hook_(c, m, target)) {
      c.pc = pc + 8;
      continue;
    }
    c.pc = target;
  }
  return steps;
}

}  // namespace lantern::ee
