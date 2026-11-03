#include "recomp/runtime.h"

#include <format>

#include "ee/r5900.h"

namespace lantern::recomp {

GuestFn Runtime::find(uint32_t addr) const {
  if (addr >= ov_base_ && addr < ov_end_) {
    auto ov = overlays_.find(active_overlay_);
    if (ov == overlays_.end()) return nullptr;
    auto it = ov->second.find(addr);
    return it == ov->second.end() ? nullptr : it->second;
  }
  auto it = main_.find(addr);
  return it == main_.end() ? nullptr : it->second;
}

uint32_t Runtime::call(ee::Cpu& c, ee::Memory& m, uint32_t target) {
  if (GuestFn fn = find(target)) {
    ++stats_.native_calls;
    return fn(c, m, *this);
  }
  if (!fallback_) throw ee::GuestTrap(target, std::format("no recompiled function at 0x{:08x}", target));
  if (!interp_hooked_) {
    // Interpreted code calling back into recompiled code goes through the same dispatch.
    interp_.set_call_hook([this](ee::Cpu& cc, ee::Memory& mm, uint32_t t) {
      GuestFn fn = find(t);
      if (!fn) return false;
      ++stats_.native_calls;
      uint32_t expected = static_cast<uint32_t>(cc.r(31));
      uint32_t got = fn(cc, mm, *this);
      if (got != expected) bad_return(t, expected, got);
      return true;
    });
    interp_hooked_ = true;
  }
  ++stats_.interpreted_calls;
  const uint32_t ret = static_cast<uint32_t>(c.r(31));
  c.pc = target;
  stats_.interpreted_steps += interp_.run(c, m, ret);
  return ret;
}

void Runtime::bad_return(uint32_t call_pc, uint32_t expected, uint32_t actual) {
  throw ee::GuestTrap(call_pc, std::format("non-standard return: call at 0x{:08x} expected return to 0x{:08x}, "
                                           "callee jumped to 0x{:08x}",
                                           call_pc, expected, actual));
}

void Runtime::unimplemented(uint32_t word, uint32_t pc) {
  ee::Insn in = ee::decode(word, pc);
  throw ee::GuestTrap(pc, std::format("unimplemented instruction '{}' (0x{:08x}) at 0x{:08x}",
                                      in.valid() ? ee::disassemble(in) : "(invalid)", word, pc));
}

void Runtime::bad_jump_table(uint32_t pc, uint32_t target) {
  throw ee::GuestTrap(pc,
                      std::format("jump table at 0x{:08x}: target 0x{:08x} not in the recovered table", pc, target));
}

}  // namespace lantern::recomp
