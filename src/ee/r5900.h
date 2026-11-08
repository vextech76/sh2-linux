#pragma once

// R5900 (PS2 Emotion Engine) instruction decoder: MIPS III subset + R5900 extensions
// (MMI 128-bit integer SIMD, MULT1/DIV1 pipeline, SA register), COP0, COP1 (single-precision
// FPU with accumulator ops), and COP2 in VU0 macro mode. Instructions the R5900 lacks
// (LL/SC, 64-bit mult/div, double-precision FPU) decode as Invalid.

#include <cstdint>
#include <string>
#include <string_view>

namespace lantern::ee {

// X(Name, "mnemonic", Format, Flags)
#define LANTERN_R5900_OPS(X)                                          \
  X(Invalid, "(invalid)", None, 0)                                    \
  /* SPECIAL */                                                       \
  X(SLL, "sll", RdRtSa, 0)                                            \
  X(SRL, "srl", RdRtSa, 0)                                            \
  X(SRA, "sra", RdRtSa, 0)                                            \
  X(SLLV, "sllv", RdRtRs, 0)                                          \
  X(SRLV, "srlv", RdRtRs, 0)                                          \
  X(SRAV, "srav", RdRtRs, 0)                                          \
  X(JR, "jr", Rs, kIndirect | kDelay | kEnd)                          \
  X(JALR, "jalr", RdRs, kIndirect | kDelay | kLink)                   \
  X(MOVZ, "movz", RdRsRt, 0)                                          \
  X(MOVN, "movn", RdRsRt, 0)                                          \
  X(SYSCALL, "syscall", Code, kSystem)                                \
  X(BREAK, "break", Code, kSystem)                                    \
  X(SYNC, "sync", Sync, 0)                                            \
  X(MFHI, "mfhi", Rd, 0)                                              \
  X(MTHI, "mthi", Rs, 0)                                              \
  X(MFLO, "mflo", Rd, 0)                                              \
  X(MTLO, "mtlo", Rs, 0)                                              \
  X(DSLLV, "dsllv", RdRtRs, 0)                                        \
  X(DSRLV, "dsrlv", RdRtRs, 0)                                        \
  X(DSRAV, "dsrav", RdRtRs, 0)                                        \
  X(MULT, "mult", RdRsRt, 0)                                          \
  X(MULTU, "multu", RdRsRt, 0)                                        \
  X(DIV, "div", RsRt, 0)                                              \
  X(DIVU, "divu", RsRt, 0)                                            \
  X(ADD, "add", RdRsRt, 0)                                            \
  X(ADDU, "addu", RdRsRt, 0)                                          \
  X(SUB, "sub", RdRsRt, 0)                                            \
  X(SUBU, "subu", RdRsRt, 0)                                          \
  X(AND, "and", RdRsRt, 0)                                            \
  X(OR, "or", RdRsRt, 0)                                              \
  X(XOR, "xor", RdRsRt, 0)                                            \
  X(NOR, "nor", RdRsRt, 0)                                            \
  X(MFSA, "mfsa", Rd, 0)                                              \
  X(MTSA, "mtsa", Rs, 0)                                              \
  X(SLT, "slt", RdRsRt, 0)                                            \
  X(SLTU, "sltu", RdRsRt, 0)                                          \
  X(DADD, "dadd", RdRsRt, 0)                                          \
  X(DADDU, "daddu", RdRsRt, 0)                                        \
  X(DSUB, "dsub", RdRsRt, 0)                                          \
  X(DSUBU, "dsubu", RdRsRt, 0)                                        \
  X(TGE, "tge", RsRt, kSystem)                                        \
  X(TGEU, "tgeu", RsRt, kSystem)                                      \
  X(TLT, "tlt", RsRt, kSystem)                                        \
  X(TLTU, "tltu", RsRt, kSystem)                                      \
  X(TEQ, "teq", RsRt, kSystem)                                        \
  X(TNE, "tne", RsRt, kSystem)                                        \
  X(DSLL, "dsll", RdRtSa, 0)                                          \
  X(DSRL, "dsrl", RdRtSa, 0)                                          \
  X(DSRA, "dsra", RdRtSa, 0)                                          \
  X(DSLL32, "dsll32", RdRtSa, 0)                                      \
  X(DSRL32, "dsrl32", RdRtSa, 0)                                      \
  X(DSRA32, "dsra32", RdRtSa, 0)                                      \
  /* REGIMM */                                                        \
  X(BLTZ, "bltz", RsBranch, kBranch | kDelay)                         \
  X(BGEZ, "bgez", RsBranch, kBranch | kDelay)                         \
  X(BLTZL, "bltzl", RsBranch, kBranch | kDelay | kLikely)             \
  X(BGEZL, "bgezl", RsBranch, kBranch | kDelay | kLikely)             \
  X(TGEI, "tgei", RsImm, kSystem)                                     \
  X(TGEIU, "tgeiu", RsImm, kSystem)                                   \
  X(TLTI, "tlti", RsImm, kSystem)                                     \
  X(TLTIU, "tltiu", RsImm, kSystem)                                   \
  X(TEQI, "teqi", RsImm, kSystem)                                     \
  X(TNEI, "tnei", RsImm, kSystem)                                     \
  X(BLTZAL, "bltzal", RsBranch, kBranch | kDelay | kLink)             \
  X(BGEZAL, "bgezal", RsBranch, kBranch | kDelay | kLink)             \
  X(BLTZALL, "bltzall", RsBranch, kBranch | kDelay | kLink | kLikely) \
  X(BGEZALL, "bgezall", RsBranch, kBranch | kDelay | kLink | kLikely) \
  X(MTSAB, "mtsab", RsImm, 0)                                         \
  X(MTSAH, "mtsah", RsImm, 0)                                         \
  /* primary */                                                       \
  X(J, "j", Jump, kJump | kDelay | kEnd)                              \
  X(JAL, "jal", Jump, kJump | kDelay | kLink)                         \
  X(BEQ, "beq", RsRtBranch, kBranch | kDelay)                         \
  X(BNE, "bne", RsRtBranch, kBranch | kDelay)                         \
  X(BLEZ, "blez", RsBranch, kBranch | kDelay)                         \
  X(BGTZ, "bgtz", RsBranch, kBranch | kDelay)                         \
  X(ADDI, "addi", RtRsImm, 0)                                         \
  X(ADDIU, "addiu", RtRsImm, 0)                                       \
  X(SLTI, "slti", RtRsImm, 0)                                         \
  X(SLTIU, "sltiu", RtRsImm, 0)                                       \
  X(ANDI, "andi", RtRsUimm, 0)                                        \
  X(ORI, "ori", RtRsUimm, 0)                                          \
  X(XORI, "xori", RtRsUimm, 0)                                        \
  X(LUI, "lui", RtUimm, 0)                                            \
  X(BEQL, "beql", RsRtBranch, kBranch | kDelay | kLikely)             \
  X(BNEL, "bnel", RsRtBranch, kBranch | kDelay | kLikely)             \
  X(BLEZL, "blezl", RsBranch, kBranch | kDelay | kLikely)             \
  X(BGTZL, "bgtzl", RsBranch, kBranch | kDelay | kLikely)             \
  X(DADDI, "daddi", RtRsImm, 0)                                       \
  X(DADDIU, "daddiu", RtRsImm, 0)                                     \
  X(LDL, "ldl", Mem, kLoad)                                           \
  X(LDR, "ldr", Mem, kLoad)                                           \
  X(LQ, "lq", Mem, kLoad)                                             \
  X(SQ, "sq", Mem, kStore)                                            \
  X(LB, "lb", Mem, kLoad)                                             \
  X(LH, "lh", Mem, kLoad)                                             \
  X(LWL, "lwl", Mem, kLoad)                                           \
  X(LW, "lw", Mem, kLoad)                                             \
  X(LBU, "lbu", Mem, kLoad)                                           \
  X(LHU, "lhu", Mem, kLoad)                                           \
  X(LWR, "lwr", Mem, kLoad)                                           \
  X(LWU, "lwu", Mem, kLoad)                                           \
  X(SB, "sb", Mem, kStore)                                            \
  X(SH, "sh", Mem, kStore)                                            \
  X(SWL, "swl", Mem, kStore)                                          \
  X(SW, "sw", Mem, kStore)                                            \
  X(SDL, "sdl", Mem, kStore)                                          \
  X(SDR, "sdr", Mem, kStore)                                          \
  X(SWR, "swr", Mem, kStore)                                          \
  X(CACHE, "cache", CacheOp, 0)                                       \
  X(LWC1, "lwc1", MemFt, kLoad | kFpu)                                \
  X(PREF, "pref", CacheOp, 0)                                         \
  X(LQC2, "lqc2", MemVf, kLoad | kVu0)                                \
  X(LD, "ld", Mem, kLoad)                                             \
  X(SWC1, "swc1", MemFt, kStore | kFpu)                               \
  X(SQC2, "sqc2", MemVf, kStore | kVu0)                               \
  X(SD, "sd", Mem, kStore)                                            \
  /* COP0 */                                                          \
  X(MFC0, "mfc0", RtCop0, kCop0)                                      \
  X(MTC0, "mtc0", RtCop0, kCop0)                                      \
  X(BC0F, "bc0f", Branch, kBranch | kDelay | kCop0)                   \
  X(BC0T, "bc0t", Branch, kBranch | kDelay | kCop0)                   \
  X(BC0FL, "bc0fl", Branch, kBranch | kDelay | kLikely | kCop0)       \
  X(BC0TL, "bc0tl", Branch, kBranch | kDelay | kLikely | kCop0)       \
  X(TLBR, "tlbr", None, kCop0)                                        \
  X(TLBWI, "tlbwi", None, kCop0)                                      \
  X(TLBWR, "tlbwr", None, kCop0)                                      \
  X(TLBP, "tlbp", None, kCop0)                                        \
  X(ERET, "eret", None, kCop0 | kEnd | kSystem)                       \
  X(EI, "ei", None, kCop0)                                            \
  X(DI, "di", None, kCop0)                                            \
  /* COP1 */                                                          \
  X(MFC1, "mfc1", RtFs, kFpu)                                         \
  X(CFC1, "cfc1", RtFcr, kFpu)                                        \
  X(MTC1, "mtc1", RtFs, kFpu)                                         \
  X(CTC1, "ctc1", RtFcr, kFpu)                                        \
  X(BC1F, "bc1f", Branch, kBranch | kDelay | kFpu)                    \
  X(BC1T, "bc1t", Branch, kBranch | kDelay | kFpu)                    \
  X(BC1FL, "bc1fl", Branch, kBranch | kDelay | kLikely | kFpu)        \
  X(BC1TL, "bc1tl", Branch, kBranch | kDelay | kLikely | kFpu)        \
  X(ADD_S, "add.s", FdFsFt, kFpu)                                     \
  X(SUB_S, "sub.s", FdFsFt, kFpu)                                     \
  X(MUL_S, "mul.s", FdFsFt, kFpu)                                     \
  X(DIV_S, "div.s", FdFsFt, kFpu)                                     \
  X(SQRT_S, "sqrt.s", FdFt, kFpu)                                     \
  X(ABS_S, "abs.s", FdFs, kFpu)                                       \
  X(MOV_S, "mov.s", FdFs, kFpu)                                       \
  X(NEG_S, "neg.s", FdFs, kFpu)                                       \
  X(RSQRT_S, "rsqrt.s", FdFsFt, kFpu)                                 \
  X(ADDA_S, "adda.s", FsFt, kFpu)                                     \
  X(SUBA_S, "suba.s", FsFt, kFpu)                                     \
  X(MULA_S, "mula.s", FsFt, kFpu)                                     \
  X(MADD_S, "madd.s", FdFsFt, kFpu)                                   \
  X(MSUB_S, "msub.s", FdFsFt, kFpu)                                   \
  X(MADDA_S, "madda.s", FsFt, kFpu)                                   \
  X(MSUBA_S, "msuba.s", FsFt, kFpu)                                   \
  X(CVT_W_S, "cvt.w.s", FdFs, kFpu)                                   \
  X(MAX_S, "max.s", FdFsFt, kFpu)                                     \
  X(MIN_S, "min.s", FdFsFt, kFpu)                                     \
  X(C_F_S, "c.f.s", FsFt, kFpu)                                       \
  X(C_EQ_S, "c.eq.s", FsFt, kFpu)                                     \
  X(C_LT_S, "c.lt.s", FsFt, kFpu)                                     \
  X(C_LE_S, "c.le.s", FsFt, kFpu)                                     \
  X(CVT_S_W, "cvt.s.w", FdFs, kFpu)                                   \
  /* COP2 (VU0 macro mode) */                                         \
  X(QMFC2, "qmfc2", RtVf, kVu0)                                       \
  X(CFC2, "cfc2", RtVi, kVu0)                                         \
  X(QMTC2, "qmtc2", RtVf, kVu0)                                       \
  X(CTC2, "ctc2", RtVi, kVu0)                                         \
  X(BC2F, "bc2f", Branch, kBranch | kDelay | kVu0)                    \
  X(BC2T, "bc2t", Branch, kBranch | kDelay | kVu0)                    \
  X(BC2FL, "bc2fl", Branch, kBranch | kDelay | kLikely | kVu0)        \
  X(BC2TL, "bc2tl", Branch, kBranch | kDelay | kLikely | kVu0)        \
  X(VU0_SPECIAL1, "vu0.special1", Vu, kVu0)                           \
  X(VU0_SPECIAL2, "vu0.special2", Vu, kVu0)                           \
  /* MMI */                                                           \
  X(MADD, "madd", RdRsRt, kMmi)                                       \
  X(MADDU, "maddu", RdRsRt, kMmi)                                     \
  X(PLZCW, "plzcw", RdRs, kMmi)                                       \
  X(MFHI1, "mfhi1", Rd, kMmi)                                         \
  X(MTHI1, "mthi1", Rs, kMmi)                                         \
  X(MFLO1, "mflo1", Rd, kMmi)                                         \
  X(MTLO1, "mtlo1", Rs, kMmi)                                         \
  X(MULT1, "mult1", RdRsRt, kMmi)                                     \
  X(MULTU1, "multu1", RdRsRt, kMmi)                                   \
  X(DIV1, "div1", RsRt, kMmi)                                         \
  X(DIVU1, "divu1", RsRt, kMmi)                                       \
  X(MADD1, "madd1", RdRsRt, kMmi)                                     \
  X(MADDU1, "maddu1", RdRsRt, kMmi)                                   \
  X(PMFHL_LW, "pmfhl.lw", Rd, kMmi)                                   \
  X(PMFHL_UW, "pmfhl.uw", Rd, kMmi)                                   \
  X(PMFHL_SLW, "pmfhl.slw", Rd, kMmi)                                 \
  X(PMFHL_LH, "pmfhl.lh", Rd, kMmi)                                   \
  X(PMFHL_SH, "pmfhl.sh", Rd, kMmi)                                   \
  X(PMTHL_LW, "pmthl.lw", Rs, kMmi)                                   \
  X(PSLLH, "psllh", RdRtSa, kMmi)                                     \
  X(PSRLH, "psrlh", RdRtSa, kMmi)                                     \
  X(PSRAH, "psrah", RdRtSa, kMmi)                                     \
  X(PSLLW, "psllw", RdRtSa, kMmi)                                     \
  X(PSRLW, "psrlw", RdRtSa, kMmi)                                     \
  X(PSRAW, "psraw", RdRtSa, kMmi)                                     \
  /* MMI0 */                                                          \
  X(PADDW, "paddw", RdRsRt, kMmi)                                     \
  X(PSUBW, "psubw", RdRsRt, kMmi)                                     \
  X(PCGTW, "pcgtw", RdRsRt, kMmi)                                     \
  X(PMAXW, "pmaxw", RdRsRt, kMmi)                                     \
  X(PADDH, "paddh", RdRsRt, kMmi)                                     \
  X(PSUBH, "psubh", RdRsRt, kMmi)                                     \
  X(PCGTH, "pcgth", RdRsRt, kMmi)                                     \
  X(PMAXH, "pmaxh", RdRsRt, kMmi)                                     \
  X(PADDB, "paddb", RdRsRt, kMmi)                                     \
  X(PSUBB, "psubb", RdRsRt, kMmi)                                     \
  X(PCGTB, "pcgtb", RdRsRt, kMmi)                                     \
  X(PADDSW, "paddsw", RdRsRt, kMmi)                                   \
  X(PSUBSW, "psubsw", RdRsRt, kMmi)                                   \
  X(PEXTLW, "pextlw", RdRsRt, kMmi)                                   \
  X(PPACW, "ppacw", RdRsRt, kMmi)                                     \
  X(PADDSH, "paddsh", RdRsRt, kMmi)                                   \
  X(PSUBSH, "psubsh", RdRsRt, kMmi)                                   \
  X(PEXTLH, "pextlh", RdRsRt, kMmi)                                   \
  X(PPACH, "ppach", RdRsRt, kMmi)                                     \
  X(PADDSB, "paddsb", RdRsRt, kMmi)                                   \
  X(PSUBSB, "psubsb", RdRsRt, kMmi)                                   \
  X(PEXTLB, "pextlb", RdRsRt, kMmi)                                   \
  X(PPACB, "ppacb", RdRsRt, kMmi)                                     \
  X(PEXT5, "pext5", RdRt, kMmi)                                       \
  X(PPAC5, "ppac5", RdRt, kMmi)                                       \
  /* MMI1 */                                                          \
  X(PABSW, "pabsw", RdRt, kMmi)                                       \
  X(PCEQW, "pceqw", RdRsRt, kMmi)                                     \
  X(PMINW, "pminw", RdRsRt, kMmi)                                     \
  X(PADSBH, "padsbh", RdRsRt, kMmi)                                   \
  X(PABSH, "pabsh", RdRt, kMmi)                                       \
  X(PCEQH, "pceqh", RdRsRt, kMmi)                                     \
  X(PMINH, "pminh", RdRsRt, kMmi)                                     \
  X(PCEQB, "pceqb", RdRsRt, kMmi)                                     \
  X(PADDUW, "padduw", RdRsRt, kMmi)                                   \
  X(PSUBUW, "psubuw", RdRsRt, kMmi)                                   \
  X(PEXTUW, "pextuw", RdRsRt, kMmi)                                   \
  X(PADDUH, "padduh", RdRsRt, kMmi)                                   \
  X(PSUBUH, "psubuh", RdRsRt, kMmi)                                   \
  X(PEXTUH, "pextuh", RdRsRt, kMmi)                                   \
  X(PADDUB, "paddub", RdRsRt, kMmi)                                   \
  X(PSUBUB, "psubub", RdRsRt, kMmi)                                   \
  X(PEXTUB, "pextub", RdRsRt, kMmi)                                   \
  X(QFSRV, "qfsrv", RdRsRt, kMmi)                                     \
  /* MMI2 */                                                          \
  X(PMADDW, "pmaddw", RdRsRt, kMmi)                                   \
  X(PSLLVW, "psllvw", RdRtRs, kMmi)                                   \
  X(PSRLVW, "psrlvw", RdRtRs, kMmi)                                   \
  X(PMSUBW, "pmsubw", RdRsRt, kMmi)                                   \
  X(PMFHI, "pmfhi", Rd, kMmi)                                         \
  X(PMFLO, "pmflo", Rd, kMmi)                                         \
  X(PINTH, "pinth", RdRsRt, kMmi)                                     \
  X(PMULTW, "pmultw", RdRsRt, kMmi)                                   \
  X(PDIVW, "pdivw", RsRt, kMmi)                                       \
  X(PCPYLD, "pcpyld", RdRsRt, kMmi)                                   \
  X(PMADDH, "pmaddh", RdRsRt, kMmi)                                   \
  X(PHMADH, "phmadh", RdRsRt, kMmi)                                   \
  X(PAND, "pand", RdRsRt, kMmi)                                       \
  X(PXOR, "pxor", RdRsRt, kMmi)                                       \
  X(PMSUBH, "pmsubh", RdRsRt, kMmi)                                   \
  X(PHMSBH, "phmsbh", RdRsRt, kMmi)                                   \
  X(PEXEH, "pexeh", RdRt, kMmi)                                       \
  X(PREVH, "prevh", RdRt, kMmi)                                       \
  X(PMULTH, "pmulth", RdRsRt, kMmi)                                   \
  X(PDIVBW, "pdivbw", RsRt, kMmi)                                     \
  X(PEXEW, "pexew", RdRt, kMmi)                                       \
  X(PROT3W, "prot3w", RdRt, kMmi)                                     \
  /* MMI3 */                                                          \
  X(PMADDUW, "pmadduw", RdRsRt, kMmi)                                 \
  X(PSRAVW, "psravw", RdRtRs, kMmi)                                   \
  X(PMTHI, "pmthi", Rs, kMmi)                                         \
  X(PMTLO, "pmtlo", Rs, kMmi)                                         \
  X(PINTEH, "pinteh", RdRsRt, kMmi)                                   \
  X(PMULTUW, "pmultuw", RdRsRt, kMmi)                                 \
  X(PDIVUW, "pdivuw", RsRt, kMmi)                                     \
  X(PCPYUD, "pcpyud", RdRsRt, kMmi)                                   \
  X(POR, "por", RdRsRt, kMmi)                                         \
  X(PNOR, "pnor", RdRsRt, kMmi)                                       \
  X(PEXCH, "pexch", RdRt, kMmi)                                       \
  X(PCPYH, "pcpyh", RdRt, kMmi)                                       \
  X(PEXCW, "pexcw", RdRt, kMmi)

enum InsnFlags : uint32_t {
  kBranch = 1u << 0,    // conditional PC-relative branch
  kJump = 1u << 1,      // J/JAL absolute (within 256 MB region)
  kIndirect = 1u << 2,  // JR/JALR
  kLink = 1u << 3,      // writes return address
  kLikely = 1u << 4,    // delay slot annulled when not taken
  kDelay = 1u << 5,     // has a delay slot
  kEnd = 1u << 6,       // unconditional transfer: straight-line flow stops after the delay slot
  kLoad = 1u << 7,
  kStore = 1u << 8,
  kSystem = 1u << 9,  // syscall/break/trap/eret
  kFpu = 1u << 10,
  kVu0 = 1u << 11,
  kMmi = 1u << 12,
  kCop0 = 1u << 13,
};

enum class Op : uint16_t {
#define LANTERN_OP_ENUM(name, mn, fmt, flags) name,
  LANTERN_R5900_OPS(LANTERN_OP_ENUM)
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
  uint32_t target = 0;     // absolute target for branches/jumps; 0 otherwise
  uint16_t vu_opcode = 0;  // VU0 macro: special1 funct, or 11-bit special2 opcode

  bool valid() const { return op != Op::Invalid; }
  bool has(uint32_t f) const { return (flags & f) != 0; }
  // Unconditional branches encoded as conditional (beq zero,zero / bgez zero).
  bool always_taken() const;
};

Insn decode(uint32_t word, uint32_t pc);
// Decoding for execution: like decode(), but accepts encodings the CPU executes although their
// reserved fields are non-zero (see r5900.cpp). Disassembly and analysis keep using decode().
Insn decode_exec(uint32_t word, uint32_t pc);
std::string_view mnemonic(Op op);
// GNU-objdump-like text (no aliases), e.g. "addiu sp,sp,-16". VU0 macro ops print a VU mnemonic.
std::string disassemble(const Insn& insn);
std::string_view gpr_name(unsigned r);
// General-purpose register written by the instruction (JAL/BxxAL write $ra), or -1 if none.
// Writes to $zero report 0; callers decide whether that matters.
int written_gpr(const Insn& insn);
// VU0 macro-mode mnemonic for VU0_SPECIAL1/2 (e.g. "vmaddx"); "(invalid)" if unassigned.
std::string_view vu0_mnemonic(const Insn& insn);

}  // namespace lantern::ee
