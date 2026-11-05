#include "iop/r3000.h"

#include <array>
#include <format>

namespace lantern::iop {
namespace {
enum class Fmt : uint8_t {
  None,
  RdRsRt,
  RdRtSa,
  RdRtRs,
  Rs,
  RdRs,
  Rd,
  RsRt,
  Code,
  RsBranch,
  RsRtBranch,
  Branch,
  Jump,
  RtRsImm,
  RtRsUimm,
  RtUimm,
  Mem,
  RtCop0
};

struct OpInfo {
  std::string_view mnemonic;
  Fmt fmt;
  uint32_t flags;
};
constexpr OpInfo kOps[] = {
#define LANTERN_OP_INFO(name, mn, fmt, flags) {mn, Fmt::fmt, flags},
    LANTERN_R3000_OPS(LANTERN_OP_INFO)
#undef LANTERN_OP_INFO
};
static_assert(std::size(kOps) == static_cast<size_t>(Op::Count));

using O = Op;
constexpr O I = Op::Invalid;

constexpr std::array<Op, 64> kSpecial = {
    O::SLL,  I,        O::SRL,  O::SRA,  O::SLLV,    I,        O::SRLV, O::SRAV,  // 0
    O::JR,   O::JALR,  I,       I,       O::SYSCALL, O::BREAK, I,       I,        // 8
    O::MFHI, O::MTHI,  O::MFLO, O::MTLO, I,          I,        I,       I,        // 16
    O::MULT, O::MULTU, O::DIV,  O::DIVU, I,          I,        I,       I,        // 24
    O::ADD,  O::ADDU,  O::SUB,  O::SUBU, O::AND,     O::OR,    O::XOR,  O::NOR,   // 32
    I,       I,        O::SLT,  O::SLTU, I,          I,        I,       I};       // 40

constexpr std::array<Op, 32> kRegimm = {
    O::BLTZ,   O::BGEZ,   I, I, I, I, I, I, I, I, I, I, I, I, I, I,   // 0x00 bltz, 0x01 bgez
    O::BLTZAL, O::BGEZAL, I, I, I, I, I, I, I, I, I, I, I, I, I, I};  // 0x10 bltzal, 0x11 bgezal

constexpr std::array<Op, 64> kPrimary = {
    I,       I,        O::J,    O::JAL,   O::BEQ,  O::BNE, O::BLEZ, O::BGTZ,  // 0
    O::ADDI, O::ADDIU, O::SLTI, O::SLTIU, O::ANDI, O::ORI, O::XORI, O::LUI,   // 8
    I,       I,        I,       I,        I,       I,      I,       I,        // 16 (COPz handled below)
    I,       I,        I,       I,        I,       I,      I,       I,        // 24
    O::LB,   O::LH,    O::LWL,  O::LW,    O::LBU,  O::LHU, O::LWR,  I,        // 32
    O::SB,   O::SH,    O::SWL,  O::SW,    I,       I,      O::SWR,  I,        // 40
    I,       O::LWC1,  I,       I,        I,       I,      I,       I,        // 48
    I,       O::SWC1,  I,       I,        I,       I,      I,       I};       // 56

Op cop0(uint32_t w) {
  const unsigned rs = (w >> 21) & 31;
  if (rs == 0) return O::MFC0;
  if (rs == 4) return O::MTC0;
  if (rs == 16 && (w & 63) == 16) return O::RFE;  // COP0 CO=1, func 0x10
  return I;
}

constexpr std::string_view kGpr[32] = {"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",
                                       "t3",   "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5",
                                       "s6",   "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};
}  // namespace

std::string_view gpr_name(unsigned r) { return kGpr[r & 31]; }
std::string_view mnemonic(Op op) { return kOps[static_cast<size_t>(op)].mnemonic; }

bool Insn::always_taken() const {
  return (op == Op::BEQ && rs == 0 && rt == 0) || (op == Op::BGEZ && rs == 0) || (op == Op::BGEZAL && rs == 0);
}

int Insn::written_gpr() const {
  if (!valid()) return -1;
  if (has(kLink)) return op == Op::JALR ? rd : 31;
  switch (kOps[static_cast<size_t>(op)].fmt) {
    case Fmt::RdRsRt:
    case Fmt::RdRtSa:
    case Fmt::RdRtRs:
    case Fmt::Rd:
    case Fmt::RdRs: return rd;
    case Fmt::RtRsImm:
    case Fmt::RtRsUimm:
    case Fmt::RtUimm: return rt;
    case Fmt::Mem: return has(kLoad) ? rt : -1;
    case Fmt::RtCop0: return op == Op::MFC0 ? rt : -1;
    default: return -1;
  }
}

Insn decode(uint32_t w, uint32_t pc) {
  Insn in;
  in.word = w;
  in.pc = pc;
  in.rs = static_cast<uint8_t>((w >> 21) & 31);
  in.rt = static_cast<uint8_t>((w >> 16) & 31);
  in.rd = static_cast<uint8_t>((w >> 11) & 31);
  in.sa = static_cast<uint8_t>((w >> 6) & 31);
  in.funct = static_cast<uint8_t>(w & 63);
  in.uimm = static_cast<uint16_t>(w & 0xFFFF);
  in.simm = static_cast<int16_t>(in.uimm);

  const unsigned opcode = w >> 26;
  switch (opcode) {
    case 0: in.op = kSpecial[in.funct]; break;
    case 1: in.op = kRegimm[in.rt]; break;
    case 16: in.op = cop0(w); break;
    case 48: in.op = O::LWC0; break;  // COPz loads/stores: keep them decodable (the IOP has no
    case 49: in.op = O::LWC1; break;  // FPU/GTE, so the interpreter traps on use)
    case 50: in.op = O::LWC2; break;
    case 51: in.op = O::LWC3; break;
    case 56: in.op = O::SWC0; break;
    case 57: in.op = O::SWC1; break;
    case 58: in.op = O::SWC2; break;
    case 59: in.op = O::SWC3; break;
    default: in.op = kPrimary[opcode]; break;
  }

  // Reserved fields must be zero, as in the hardware encoding: this is what keeps data from
  // decoding as plausible instructions.
  const auto& info = kOps[static_cast<size_t>(in.op)];
  const bool sa_free = in.has(kSystem);
  const bool sa_zero = sa_free || in.sa == 0;
  bool ok = true;
  if (in.op == Op::SLL || in.op == Op::SRL || in.op == Op::SRA)
    ok = in.rs == 0;
  else if (in.op == Op::JR)
    ok = ((w >> 6) & 0x7FFF) == 0;  // rt, rd and sa must be zero (bits 6..20)
  else if (in.op == Op::JALR)
    ok = in.rt == 0 && in.sa == 0;
  else if (in.op == Op::BLEZ || in.op == Op::BGTZ)
    ok = in.rt == 0;
  else if (in.op == Op::LUI)
    ok = in.rs == 0;
  else if (in.op == Op::RFE)
    ok = (w & 0x01FFFFC0) == 0;
  else if (info.fmt == Fmt::RdRsRt || info.fmt == Fmt::RdRtRs || info.fmt == Fmt::RdRs)
    ok = sa_zero;
  else if (info.fmt == Fmt::RdRtSa)
    ok = in.rs == 0;
  else if (info.fmt == Fmt::Rs)
    ok = in.rt == 0 && in.rd == 0 && sa_zero;
  else if (info.fmt == Fmt::Rd)
    ok = in.rs == 0 && in.rt == 0 && sa_zero;
  else if (info.fmt == Fmt::RsRt)
    ok = sa_zero && (in.has(kSystem) || in.rd == 0);
  else if (info.fmt == Fmt::RtCop0)
    ok = (w & 0x7FF) == 0;
  if (!ok) in.op = Op::Invalid;

  in.flags = kOps[static_cast<size_t>(in.op)].flags;
  if (in.has(kBranch)) in.target = pc + 4 + static_cast<uint32_t>(static_cast<int32_t>(in.simm) * 4);
  if (in.has(kJump)) in.target = ((pc + 4) & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2);
  if (in.always_taken()) in.flags |= kEnd;
  return in;
}

std::string disassemble(const Insn& in) {
  const auto& info = kOps[static_cast<size_t>(in.op)];
  const auto rs = gpr_name(in.rs), rt = gpr_name(in.rt), rd = gpr_name(in.rd);
  const std::string_view mn = info.mnemonic;
  switch (info.fmt) {
    case Fmt::None: return std::string(mn);
    case Fmt::RdRsRt: return std::format("{}\t{},{},{}", mn, rd, rs, rt);
    case Fmt::RdRtSa: return std::format("{}\t{},{},0x{:x}", mn, rd, rt, in.sa);
    case Fmt::RdRtRs: return std::format("{}\t{},{},{}", mn, rd, rt, rs);
    case Fmt::Rs: return std::format("{}\t{}", mn, rs);
    case Fmt::RdRs:
      if (in.op == Op::JALR && in.rd == 31) return std::format("{}\t{}", mn, rs);  // GNU's form
      return std::format("{}\t{},{}", mn, rd, rs);
    case Fmt::Rd: return std::format("{}\t{}", mn, rd);
    case Fmt::RsRt:
      // MIPS I DIV/DIVU have no destination register; GNU prints the (unused) field as `zero`.
      if (in.op == Op::DIV || in.op == Op::DIVU) return std::format("{}\tzero,{},{}", mn, rs, rt);
      return std::format("{}\t{},{}", mn, rs, rt);
    case Fmt::Code: {
      // SYSCALL/BREAK carry a 10-bit code in bits 16..25 (as GNU prints it).
      const unsigned code = (in.word >> 16) & 0x3FF;
      return code ? std::format("{}\t0x{:x}", mn, code) : std::string(mn);
    }
    case Fmt::RsBranch: return std::format("{}\t{},0x{:x}", mn, rs, in.target);
    case Fmt::RsRtBranch: return std::format("{}\t{},{},0x{:x}", mn, rs, rt, in.target);
    case Fmt::Branch:
    case Fmt::Jump: return std::format("{}\t0x{:x}", mn, in.target);
    case Fmt::RtRsImm: return std::format("{}\t{},{},{}", mn, rt, rs, in.simm);
    case Fmt::RtRsUimm: return std::format("{}\t{},{},0x{:x}", mn, rt, rs, in.uimm);
    case Fmt::RtUimm: return std::format("{}\t{},0x{:x}", mn, rt, in.uimm);
    case Fmt::Mem: return std::format("{}\t{},{}({})", mn, rt, in.simm, rs);
    case Fmt::RtCop0: return std::format("{}\t{},${}", mn, rt, in.rd);
  }
  return std::string(mn);
}

}  // namespace lantern::iop
