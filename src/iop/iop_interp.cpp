#include "iop/iop_interp.h"

#include <format>

namespace lantern::iop {
namespace {
constexpr uint32_t sx16(uint32_t v) { return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(v))); }
constexpr uint32_t sx8(uint32_t v) { return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(v))); }

void align(uint32_t addr, uint32_t n, uint32_t pc) {
  if (addr & (n - 1)) throw Trap(pc, std::format("unaligned IOP access: address 0x{:08x} at pc 0x{:08x}", addr, pc));
}
}  // namespace

bool Interpreter::branch_taken(const IopCpu& c, const Insn& in) const {
  switch (in.op) {
    case Op::BEQ: return c.at(in.rs) == c.at(in.rt);
    case Op::BNE: return c.at(in.rs) != c.at(in.rt);
    case Op::BLEZ: return static_cast<int32_t>(c.at(in.rs)) <= 0;
    case Op::BGTZ: return static_cast<int32_t>(c.at(in.rs)) > 0;
    case Op::BLTZ:
    case Op::BLTZAL: return static_cast<int32_t>(c.at(in.rs)) < 0;
    case Op::BGEZ:
    case Op::BGEZAL: return static_cast<int32_t>(c.at(in.rs)) >= 0;
    default: throw Trap(in.pc, std::format("not a branch: {}", disassemble(in)));
  }
}

void Interpreter::execute(IopCpu& c, IopMemory& m, const Insn& in) {
  const uint32_t pc = in.pc;
  const uint32_t rs = c.at(in.rs), rt = c.at(in.rt);
  switch (in.op) {
    // ---- shifts ----
    case Op::SLL: c.set(in.rd, rt << in.sa); break;
    case Op::SRL: c.set(in.rd, rt >> in.sa); break;
    case Op::SRA: c.set(in.rd, static_cast<uint32_t>(static_cast<int32_t>(rt) >> in.sa)); break;
    case Op::SLLV: c.set(in.rd, rt << (rs & 31)); break;
    case Op::SRLV: c.set(in.rd, rt >> (rs & 31)); break;
    case Op::SRAV: c.set(in.rd, static_cast<uint32_t>(static_cast<int32_t>(rt) >> (rs & 31))); break;
    // ---- arithmetic / logic ----
    case Op::ADD: {
      int32_t r;
      if (__builtin_add_overflow(static_cast<int32_t>(rs), static_cast<int32_t>(rt), &r))
        throw Trap(pc, "integer overflow (ADD)");
      c.set(in.rd, static_cast<uint32_t>(r));
      break;
    }
    case Op::ADDU: c.set(in.rd, rs + rt); break;
    case Op::SUB: {
      int32_t r;
      if (__builtin_sub_overflow(static_cast<int32_t>(rs), static_cast<int32_t>(rt), &r))
        throw Trap(pc, "integer overflow (SUB)");
      c.set(in.rd, static_cast<uint32_t>(r));
      break;
    }
    case Op::SUBU: c.set(in.rd, rs - rt); break;
    case Op::AND: c.set(in.rd, rs & rt); break;
    case Op::OR: c.set(in.rd, rs | rt); break;
    case Op::XOR: c.set(in.rd, rs ^ rt); break;
    case Op::NOR: c.set(in.rd, ~(rs | rt)); break;
    case Op::SLT: c.set(in.rd, static_cast<int32_t>(rs) < static_cast<int32_t>(rt) ? 1 : 0); break;
    case Op::SLTU: c.set(in.rd, rs < rt ? 1 : 0); break;
    case Op::ADDI: {
      int32_t r;
      if (__builtin_add_overflow(static_cast<int32_t>(rs), static_cast<int32_t>(in.simm), &r))
        throw Trap(pc, "integer overflow (ADDI)");
      c.set(in.rt, static_cast<uint32_t>(r));
      break;
    }
    case Op::ADDIU: c.set(in.rt, rs + static_cast<uint32_t>(in.simm)); break;
    case Op::SLTI: c.set(in.rt, static_cast<int32_t>(rs) < in.simm ? 1 : 0); break;
    case Op::SLTIU: c.set(in.rt, rs < static_cast<uint32_t>(static_cast<int32_t>(in.simm)) ? 1 : 0); break;
    case Op::ANDI: c.set(in.rt, rs & in.uimm); break;
    case Op::ORI: c.set(in.rt, rs | in.uimm); break;
    case Op::XORI: c.set(in.rt, rs ^ in.uimm); break;
    case Op::LUI: c.set(in.rt, static_cast<uint32_t>(in.uimm) << 16); break;
    // ---- multiply / divide (MIPS I: results only in HI/LO) ----
    case Op::MULT: {
      const int64_t p = static_cast<int64_t>(static_cast<int32_t>(rs)) * static_cast<int32_t>(rt);
      c.lo = static_cast<uint32_t>(p);
      c.hi = static_cast<uint32_t>(static_cast<uint64_t>(p) >> 32);
      break;
    }
    case Op::MULTU: {
      const uint64_t p = static_cast<uint64_t>(rs) * rt;
      c.lo = static_cast<uint32_t>(p);
      c.hi = static_cast<uint32_t>(p >> 32);
      break;
    }
    case Op::DIV:
      if (rt == 0) {
        c.lo = static_cast<int32_t>(rs) < 0 ? 1u : 0xFFFFFFFFu;
        c.hi = rs;
      } else if (rs == 0x80000000u && rt == 0xFFFFFFFFu) {
        c.lo = 0x80000000u;
        c.hi = 0;
      } else {
        c.lo = static_cast<uint32_t>(static_cast<int32_t>(rs) / static_cast<int32_t>(rt));
        c.hi = static_cast<uint32_t>(static_cast<int32_t>(rs) % static_cast<int32_t>(rt));
      }
      break;
    case Op::DIVU:
      if (rt == 0) {
        c.lo = 0xFFFFFFFFu;
        c.hi = rs;
      } else {
        c.lo = rs / rt;
        c.hi = rs % rt;
      }
      break;
    case Op::MFHI: c.set(in.rd, c.hi); break;
    case Op::MTHI: c.hi = rs; break;
    case Op::MFLO: c.set(in.rd, c.lo); break;
    case Op::MTLO: c.lo = rs; break;
    // ---- loads / stores (loads go through the delay slot) ----
    case Op::LB: begin_load(c, in.rt, sx8(m.read<uint8_t>(rs + static_cast<uint32_t>(in.simm)))); break;
    case Op::LBU: begin_load(c, in.rt, m.read<uint8_t>(rs + static_cast<uint32_t>(in.simm))); break;
    case Op::LH: {
      const uint32_t a = rs + static_cast<uint32_t>(in.simm);
      align(a, 2, pc);
      begin_load(c, in.rt, sx16(m.read<uint16_t>(a)));
      break;
    }
    case Op::LHU: {
      const uint32_t a = rs + static_cast<uint32_t>(in.simm);
      align(a, 2, pc);
      begin_load(c, in.rt, m.read<uint16_t>(a));
      break;
    }
    case Op::LW: {
      const uint32_t a = rs + static_cast<uint32_t>(in.simm);
      align(a, 4, pc);
      begin_load(c, in.rt, m.read<uint32_t>(a));
      break;
    }
    case Op::LWL: {
      const uint32_t a = rs + static_cast<uint32_t>(in.simm), sh = a & 3;
      const uint32_t mem = m.read<uint32_t>(a & ~3u);
      static constexpr uint32_t kMask[4] = {0x00FFFFFF, 0x0000FFFF, 0x000000FF, 0x00000000};
      begin_load(c, in.rt, (rt & kMask[sh]) | (mem << (24 - 8 * sh)));
      break;
    }
    case Op::LWR: {
      const uint32_t a = rs + static_cast<uint32_t>(in.simm), sh = a & 3;
      const uint32_t mem = m.read<uint32_t>(a & ~3u);
      static constexpr uint32_t kMask[4] = {0, 0xFF000000, 0xFFFF0000, 0xFFFFFF00};
      begin_load(c, in.rt, (sh == 0 ? mem : ((rt & kMask[sh]) | (mem >> (8 * sh)))));
      break;
    }
    case Op::SB: m.write<uint8_t>(rs + static_cast<uint32_t>(in.simm), static_cast<uint8_t>(rt)); break;
    case Op::SH: {
      const uint32_t a = rs + static_cast<uint32_t>(in.simm);
      align(a, 2, pc);
      m.write<uint16_t>(a, static_cast<uint16_t>(rt));
      break;
    }
    case Op::SW: {
      const uint32_t a = rs + static_cast<uint32_t>(in.simm);
      align(a, 4, pc);
      m.write<uint32_t>(a, rt);
      break;
    }
    case Op::SWL: {
      const uint32_t a = rs + static_cast<uint32_t>(in.simm), sh = a & 3;
      const uint32_t mem = m.read<uint32_t>(a & ~3u);
      static constexpr uint32_t kMask[4] = {0xFFFFFF00, 0xFFFF0000, 0xFF000000, 0x00000000};
      m.write<uint32_t>(a & ~3u, (rt >> (24 - 8 * sh)) | (mem & kMask[sh]));
      break;
    }
    case Op::SWR: {
      const uint32_t a = rs + static_cast<uint32_t>(in.simm), sh = a & 3;
      const uint32_t mem = m.read<uint32_t>(a & ~3u);
      static constexpr uint32_t kMask[4] = {0x00000000, 0x000000FF, 0x0000FFFF, 0x00FFFFFF};
      m.write<uint32_t>(a & ~3u, (rt << (8 * sh)) | (mem & kMask[sh]));
      break;
    }
    // ---- coprocessor 0 ----
    case Op::MFC0: begin_load(c, in.rt, c.cop0[in.rd]); break;
    case Op::MTC0: c.cop0[in.rd] = rt; break;
    case Op::RFE: c.cop0[12] = (c.cop0[12] & ~0x3Cu) | ((c.cop0[12] >> 2) & 0xF); break;
    // ---- system / IOPS ----
    case Op::SYSCALL:
      ++stats_.syscalls;
      if (!syscall_) throw Trap(pc, "IOP syscall with no kernel attached");
      syscall_(c, m, pc);
      break;
    case Op::BREAK: throw Trap(pc, std::format("IOP break 0x{:x}", (in.word >> 6) & 0xFFFFF));
    case Op::LWC0:
    case Op::LWC1:
    case Op::LWC2:
    case Op::LWC3:
    case Op::SWC0:
    case Op::SWC1:
    case Op::SWC2:
    case Op::SWC3: throw Trap(pc, std::format("coprocessor load/store not present on the IOP: {}", disassemble(in)));
    // ---- control flow is handled by step() ----
    case Op::J:
    case Op::JAL:
    case Op::JR:
    case Op::JALR:
    case Op::BEQ:
    case Op::BNE:
    case Op::BLEZ:
    case Op::BGTZ:
    case Op::BLTZ:
    case Op::BGEZ:
    case Op::BLTZAL:
    case Op::BGEZAL: break;
    case Op::Invalid:
    case Op::Count: throw Trap(pc, std::format("invalid IOP instruction 0x{:08x} at 0x{:08x}", in.word, pc));
  }
}

void Interpreter::step(IopCpu& c, IopMemory& m) {
  const uint32_t pc = c.pc;
  const Insn in = decode(m.read<uint32_t>(pc), pc);
  if (!in.valid()) throw Trap(pc, std::format("invalid IOP instruction 0x{:08x} at 0x{:08x}", in.word, pc));

  // The load issued by the *previous* instruction becomes visible now (after this one has read
  // its operands from the old register file).
  const int carry_reg = c.pending_reg;
  const uint32_t carry_value = c.pending_value;
  c.pending_reg = -1;

  uint32_t next_pc = pc + 4;
  if (!in.has(kDelay)) {
    execute(c, m, in);
  } else {
    ++stats_.branches;
    bool taken = true;
    uint32_t target = in.target;
    if (in.has(kBranch)) taken = branch_taken(c, in);
    if (in.op == Op::JR || in.op == Op::JALR) target = c.at(in.rs);
    if (in.has(kLink)) c.set(in.op == Op::JALR ? in.rd : 31, pc + 8);  // visible to the delay slot
    // Delay slot: it must not contain another control transfer.
    const Insn slot = decode(m.read<uint32_t>(pc + 4), pc + 4);
    if (slot.has(kDelay)) throw Trap(pc + 4, "IOP control transfer in a delay slot");
    if (!slot.valid()) throw Trap(pc + 4, std::format("invalid IOP instruction 0x{:08x} in delay slot", slot.word));
    // The slot's own operand reads see the register file *before* a load in the branch instruction
    // lands; a load in the slot delays past the branch.
    execute(c, m, slot);
    ++stats_.steps;
    const int slot_load = c.pending_reg;
    const uint32_t slot_value = c.pending_value;
    c.pending_reg = -1;
    if (carry_reg > 0) c.gpr[carry_reg] = carry_value;
    if (slot_load > 0) {
      c.pending_reg = slot_load;
      c.pending_value = slot_value;
    }
    next_pc = taken ? target : pc + 8;
  }

  if (!in.has(kDelay)) {
    if (carry_reg > 0) c.gpr[carry_reg] = carry_value;
  }
  ++stats_.steps;
  c.pc = next_pc;
}

uint64_t Interpreter::run(IopCpu& c, IopMemory& m, uint32_t stop_pc, uint64_t max_steps) {
  const uint64_t start = stats_.steps;
  while (c.pc != stop_pc) {
    if (stats_.steps - start > max_steps) throw Trap(c.pc, "IOP interpreter step budget exhausted");
    step(c, m);
  }
  return stats_.steps - start;
}

}  // namespace lantern::iop
