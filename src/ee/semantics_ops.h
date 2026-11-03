#pragma once

// Ops whose effect is fully described by a sem::NAME(Cpu&, Memory&, word, pc) function with no
// control-flow consequence. Used by both the interpreter and the C++ emitter, so the set of
// implemented instructions is defined exactly once.
// clang-format off
#define LANTERN_PLAIN_OPS(X) \
  X(SLL) X(SRL) X(SRA) X(SLLV) X(SRLV) X(SRAV) X(DSLLV) X(DSRLV) X(DSRAV) X(DSLL) X(DSRL) X(DSRA) \
  X(DSLL32) X(DSRL32) X(DSRA32) X(ADD) X(ADDU) X(SUB) X(SUBU) X(DADD) X(DADDU) X(DSUB) X(DSUBU) \
  X(AND) X(OR) X(XOR) X(NOR) X(SLT) X(SLTU) X(MOVZ) X(MOVN) X(ADDI) X(ADDIU) X(DADDI) X(DADDIU) \
  X(SLTI) X(SLTIU) X(ANDI) X(ORI) X(XORI) X(LUI) X(MULT) X(MULTU) X(DIV) X(DIVU) X(MULT1) X(MULTU1) \
  X(DIV1) X(DIVU1) X(MADD) X(MADDU) X(MADD1) X(MADDU1) X(MFHI) X(MFLO) X(MTHI) X(MTLO) X(MFHI1) \
  X(MFLO1) X(MTHI1) X(MTLO1) X(MFSA) X(MTSA) X(MTSAB) X(MTSAH) X(TGE) X(TGEU) X(TLT) X(TLTU) X(TEQ) \
  X(TNE) X(TGEI) X(TGEIU) X(TLTI) X(TLTIU) X(TEQI) X(TNEI) X(SYSCALL) X(BREAK) X(SYNC) X(CACHE) \
  X(PREF) X(LB) X(LBU) X(LH) X(LHU) X(LW) X(LWU) X(LD) X(LQ) X(SB) X(SH) X(SW) X(SD) X(SQ) X(LWL) \
  X(LWR) X(SWL) X(SWR) X(LDL) X(LDR) X(SDL) X(SDR) X(MFC0) X(MTC0) X(EI) X(DI) X(MFC1) X(MTC1) \
  X(CFC1) X(CTC1) X(LWC1) X(SWC1) X(ADD_S) X(SUB_S) X(MUL_S) X(DIV_S) X(SQRT_S) X(RSQRT_S) X(ABS_S) \
  X(MOV_S) X(NEG_S) X(ADDA_S) X(SUBA_S) X(MULA_S) X(MADD_S) X(MSUB_S) X(MADDA_S) X(MSUBA_S) \
  X(MAX_S) X(MIN_S) X(CVT_W_S) X(CVT_S_W) X(C_F_S) X(C_EQ_S) X(C_LT_S) X(C_LE_S) X(PAND) X(POR) \
  X(PXOR) X(PNOR) X(PCPYLD) X(PCPYUD) X(PCPYH) X(PEXTLW) X(PEXTUW) X(PEXTLH) X(PEXTLB) X(PPACW) \
  X(PADDW) X(PSUBW) X(PADDH) X(PSUBH) X(PADDB) X(PSUBB) X(PCEQW) X(PCEQH) X(PCEQB) X(PCGTW) \
  X(PCGTH) X(PCGTB) X(PMAXW) X(PMINW) X(PMAXH) X(PMINH) X(PSLLW) X(PSRLW) X(PSRAW) X(PSLLH) \
  X(PSRLH) X(PSRAH) X(PMFHI) X(PMFLO) X(PMTHI) X(PMTLO) X(PLZCW) X(QFSRV) X(PADDSW) X(PSUBSW) \
  X(PADDSH) X(PSUBSH) X(PADDSB) X(PSUBSB) X(PADDUW) X(PSUBUW) X(PADDUH) X(PSUBUH) X(PADDUB) \
  X(PSUBUB) X(PABSW) X(PABSH) X(PADSBH) X(PEXTUH) X(PEXTUB) X(PPACH) X(PPACB) X(PINTH) X(PINTEH) \
  X(PEXEH) X(PREVH) X(PEXCH) X(PEXEW) X(PEXCW) X(PROT3W) X(PEXT5) X(PPAC5) X(PSLLVW) X(PSRLVW) \
  X(PSRAVW) X(PMULTW) X(PMULTUW) X(PMADDW) X(PMSUBW) X(PMADDUW) X(PDIVW) X(PDIVUW) X(PDIVBW) \
  X(PMULTH) X(PMADDH) X(PMSUBH) X(PHMADH) X(PHMSBH) X(PMFHL_LW) X(PMFHL_UW) X(PMFHL_SLW) \
  X(PMFHL_LH) X(PMFHL_SH) X(PMTHL_LW) X(VU0_SPECIAL1) X(VU0_SPECIAL2) X(QMFC2) X(QMTC2) X(CFC2) \
  X(CTC2) X(LQC2) X(SQC2)

// Conditional branches: X(OP, condition-function, likely, link)
#define LANTERN_BRANCH_OPS(X)                                                                                     \
  X(BEQ, BEQ, false, false)                                                                                       \
  X(BNE, BNE, false, false) X(BLEZ, BLEZ, false, false) X(BGTZ, BGTZ, false, false) X(BLTZ, BLTZ, false, false)   \
      X(BGEZ, BGEZ, false, false) X(BEQL, BEQ, true, false) X(BNEL, BNE, true, false) X(BLEZL, BLEZ, true, false) \
          X(BGTZL, BGTZ, true, false) X(BLTZL, BLTZ, true, false) X(BGEZL, BGEZ, true, false)                     \
              X(BLTZAL, BLTZ, false, true) X(BGEZAL, BGEZ, false, true) X(BLTZALL, BLTZ, true, true)              \
                  X(BGEZALL, BGEZ, true, true) X(BC1F, BC1F, false, false) X(BC1T, BC1T, false, false)            \
                      X(BC1FL, BC1F, true, false) X(BC1TL, BC1T, true, false)                                    \
  X(BC2F, BC2F, false, false) X(BC2T, BC2T, false, false) X(BC2FL, BC2F, true, false)        \
  X(BC2TL, BC2T, true, false)
// clang-format on
