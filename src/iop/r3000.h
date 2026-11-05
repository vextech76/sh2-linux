#pragma once

// R3000A (PlayStation / PS2 IOP) instruction decoder: MIPS I plus the PSX-era coprocessor loads
// the IOP keeps. Unlike the EE there are no 64-bit, MMI or FPU instructions, and loads have a
// delay slot (see iop_interp.h).

#include <cstdint>
#include <string>
#include <string_view>

namespace lantern::iop {

#define LANTERN_R3000_OPS(X)                              \
  X(Invalid, "(invalid)", None, 0)                        \
  /* SPECIAL */                                           \
  X(SLL, "sll", RdRtSa, 0)                                \
  X(SRL, "srl", RdRtSa, 0)                                \
  X(SRA, "sra", RdRtSa, 0)                                \
  X(SLLV, "sllv", RdRtRs, 0)                              \
  X(SRLV, "srlv", RdRtRs, 0)                              \
  X(SRAV, "srav", RdRtRs, 0)                              \
  X(JR, "jr", Rs, kIndirect | kDelay | kEnd)              \
  X(JALR, "jalr", RdRs, kIndirect | kDelay | kLink)       \
  X(SYSCALL, "syscall", Code, kSystem)                    \
  X(BREAK, "break", Code, kSystem)                        \
  X(MFHI, "mfhi", Rd, 0)                                  \
  X(MTHI, "mthi", Rs, 0)                                  \
  X(MFLO, "mflo", Rd, 0)                                  \
  X(MTLO, "mtlo", Rs, 0)                                  \
  X(MULT, "mult", RsRt, 0)                                \
  X(MULTU, "multu", RsRt, 0)                              \
  X(DIV, "div", RsRt, 0)                                  \
  X(DIVU, "divu", RsRt, 0)                                \
  X(ADD, "add", RdRsRt, 0)                                \
  X(ADDU, "addu", RdRsRt, 0)                              \
  X(SUB, "sub", RdRsRt, 0)                                \
  X(SUBU, "subu", RdRsRt, 0)                              \
  X(AND, "and", RdRsRt, 0)                                \
  X(OR, "or", RdRsRt, 0)                                  \
  X(XOR, "xor", RdRsRt, 0)                                \
  X(NOR, "nor", RdRsRt, 0)                                \
  X(SLT, "slt", RdRsRt, 0)                                \
  X(SLTU, "sltu", RdRsRt, 0)                              \
  /* REGIMM */                                            \
  X(BLTZ, "bltz", RsBranch, kBranch | kDelay)             \
  X(BGEZ, "bgez", RsBranch, kBranch | kDelay)             \
  X(BLTZAL, "bltzal", RsBranch, kBranch | kDelay | kLink) \
  X(BGEZAL, "bgezal", RsBranch, kBranch | kDelay | kLink) \
  /* primary */                                           \
  X(J, "j", Jump, kJump | kDelay | kEnd)                  \
  X(JAL, "jal", Jump, kJump | kDelay | kLink)             \
  X(BEQ, "beq", RsRtBranch, kBranch | kDelay)             \
  X(BNE, "bne", RsRtBranch, kBranch | kDelay)             \
  X(BLEZ, "blez", RsBranch, kBranch | kDelay)             \
  X(BGTZ, "bgtz", RsBranch, kBranch | kDelay)             \
  X(ADDI, "addi", RtRsImm, 0)                             \
  X(ADDIU, "addiu", RtRsImm, 0)                           \
  X(SLTI, "slti", RtRsImm, 0)                             \
  X(SLTIU, "sltiu", RtRsImm, 0)                           \
  X(ANDI, "andi", RtRsUimm, 0)                            \
  X(ORI, "ori", RtRsUimm, 0)                              \
  X(XORI, "xori", RtRsUimm, 0)                            \
  X(LUI, "lui", RtUimm, 0)                                \
  X(LB, "lb", Mem, kLoad)                                 \
  X(LH, "lh", Mem, kLoad)                                 \
  X(LWL, "lwl", Mem, kLoad)                               \
  X(LW, "lw", Mem, kLoad)                                 \
  X(LBU, "lbu", Mem, kLoad)                               \
  X(LHU, "lhu", Mem, kLoad)                               \
  X(LWR, "lwr", Mem, kLoad)                               \
  X(SB, "sb", Mem, kStore)                                \
  X(SH, "sh", Mem, kStore)                                \
  X(SWL, "swl", Mem, kStore)                              \
  X(SW, "sw", Mem, kStore)                                \
  X(SWR, "swr", Mem, kStore)                              \
  X(LWC0, "lwc0", Mem, kLoad)                             \
  X(LWC1, "lwc1", Mem, kLoad)                             \
  X(LWC2, "lwc2", Mem, kLoad)                             \
  X(LWC3, "lwc3", Mem, kLoad)                             \
  X(SWC0, "swc0", Mem, kStore)                            \
  X(SWC1, "swc1", Mem, kStore)                            \
  X(SWC2, "swc2", Mem, kStore)                            \
  X(SWC3, "swc3", Mem, kStore)                            \
  /* COP0 */                                              \
  X(MFC0, "mfc0", RtCop0, kCop0)                          \
  X(MTC0, "mtc0", RtCop0, kCop0)                          \
  X(RFE, "rfe", None, kCop0 | kEnd)

enum InsnFlags : uint32_t {
  kBranch = 1u << 0,
  kJump = 1u << 1,
  kIndirect = 1u << 2,
  kLink = 1u << 3,
  kDelay = 1u << 4,
  kEnd = 1u << 5,  // unconditional transfer: straight-line flow stops after the delay slot
  kLoad = 1u << 6,
  kStore = 1u << 7,
  kSystem = 1u << 8,
  kCop0 = 1u << 9,
};

enum class Op : uint16_t {
#define LANTERN_OP_ENUM(name, mn, fmt, flags) name,
  LANTERN_R3000_OPS(LANTERN_OP_ENUM)
#undef LANTERN_OP_ENUM
      Count
};

struct Insn {
  Op op = Op::Invalid;
  uint32_t word = 0;
  uint32_t pc = 0;
  uint32_t flags = 0;
  uint8_t rs = 0, rt = 0, rd = 0, sa = 0, funct = 0;
  int16_t simm = 0;
  uint16_t uimm = 0;
  uint32_t target = 0;

  bool valid() const { return op != Op::Invalid; }
  bool has(uint32_t f) const { return (flags & f) != 0; }
  bool always_taken() const;  // beq zero,zero / bgez zero
  // Register written (a load reports its destination; see the load delay in iop_interp.h).
  int written_gpr() const;
};

Insn decode(uint32_t word, uint32_t pc);
std::string disassemble(const Insn& insn);
std::string_view mnemonic(Op op);
std::string_view gpr_name(unsigned r);

}  // namespace lantern::iop
