#include "ee/r5900.h"

#include <array>
#include <format>

namespace lantern::ee {
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
  RdRt,
  Code,
  Sync,
  RsImm,
  RsBranch,
  RsRtBranch,
  Branch,
  Jump,
  RtRsImm,
  RtRsUimm,
  RtUimm,
  Mem,
  MemFt,
  MemVf,
  CacheOp,
  RtCop0,
  RtFs,
  RtFcr,
  FdFsFt,
  FdFs,
  FdFt,
  FsFt,
  RtVf,
  RtVi,
  Vu
};

struct OpInfo {
  std::string_view mnemonic;
  Fmt fmt;
  uint32_t flags;
};

constexpr OpInfo kOps[] = {
#define LANTERN_OP_INFO(name, mn, fmt, flags) {mn, Fmt::fmt, flags},
    LANTERN_R5900_OPS(LANTERN_OP_INFO)
#undef LANTERN_OP_INFO
};
static_assert(std::size(kOps) == static_cast<size_t>(Op::Count));

using O = Op;
constexpr O I = Op::Invalid;

constexpr std::array<Op, 64> kPrimary = {
    I,        I,         O::J,    O::JAL,   O::BEQ,  O::BNE,  O::BLEZ,  O::BGTZ,   // 0
    O::ADDI,  O::ADDIU,  O::SLTI, O::SLTIU, O::ANDI, O::ORI,  O::XORI,  O::LUI,    // 8
    I,        I,         I,       I,        O::BEQL, O::BNEL, O::BLEZL, O::BGTZL,  // 16 (COP handled separately)
    O::DADDI, O::DADDIU, O::LDL,  O::LDR,   I,       I,       O::LQ,    O::SQ,     // 24 (MMI handled separately)
    O::LB,    O::LH,     O::LWL,  O::LW,    O::LBU,  O::LHU,  O::LWR,   O::LWU,    // 32
    O::SB,    O::SH,     O::SWL,  O::SW,    O::SDL,  O::SDR,  O::SWR,   O::CACHE,  // 40
    I,        O::LWC1,   I,       O::PREF,  I,       I,       O::LQC2,  O::LD,     // 48
    I,        O::SWC1,   I,       I,        I,       I,       O::SQC2,  O::SD};    // 56

constexpr std::array<Op, 64> kSpecial = {
    O::SLL,  I,        O::SRL,  O::SRA,  O::SLLV,    I,        O::SRLV,   O::SRAV,     // 0
    O::JR,   O::JALR,  O::MOVZ, O::MOVN, O::SYSCALL, O::BREAK, I,         O::SYNC,     // 8
    O::MFHI, O::MTHI,  O::MFLO, O::MTLO, O::DSLLV,   I,        O::DSRLV,  O::DSRAV,    // 16
    O::MULT, O::MULTU, O::DIV,  O::DIVU, I,          I,        I,         I,           // 24
    O::ADD,  O::ADDU,  O::SUB,  O::SUBU, O::AND,     O::OR,    O::XOR,    O::NOR,      // 32
    O::MFSA, O::MTSA,  O::SLT,  O::SLTU, O::DADD,    O::DADDU, O::DSUB,   O::DSUBU,    // 40
    O::TGE,  O::TGEU,  O::TLT,  O::TLTU, O::TEQ,     I,        O::TNE,    I,           // 48
    O::DSLL, I,        O::DSRL, O::DSRA, O::DSLL32,  I,        O::DSRL32, O::DSRA32};  // 56

constexpr std::array<Op, 32> kRegimm = {O::BLTZ,   O::BGEZ,   O::BLTZL,   O::BGEZL,   I,       I, I,       I,   // 0
                                        O::TGEI,   O::TGEIU,  O::TLTI,    O::TLTIU,   O::TEQI, I, O::TNEI, I,   // 8
                                        O::BLTZAL, O::BGEZAL, O::BLTZALL, O::BGEZALL, I,       I, I,       I,   // 16
                                        O::MTSAB,  O::MTSAH,  I,          I,          I,       I, I,       I};  // 24

constexpr std::array<Op, 64> kMmi = {
    O::MADD,  O::MADDU,  I,        I,        O::PLZCW, I, I,        I,          // 0
    I,        I,         I,        I,        I,        I, I,        I,          // 8 (MMI0/MMI2 handled)
    O::MFHI1, O::MTHI1,  O::MFLO1, O::MTLO1, I,        I, I,        I,          // 16
    O::MULT1, O::MULTU1, O::DIV1,  O::DIVU1, I,        I, I,        I,          // 24
    O::MADD1, O::MADDU1, I,        I,        I,        I, I,        I,          // 32
    I,        I,         I,        I,        I,        I, I,        I,          // 40 (MMI1/MMI3 handled)
    I,        I,         I,        I,        O::PSLLH, I, O::PSRLH, O::PSRAH,   // 48 (PMFHL/PMTHL handled)
    I,        I,         I,        I,        O::PSLLW, I, O::PSRLW, O::PSRAW};  // 56

constexpr std::array<Op, 32> kMmi0 = {O::PADDW,  O::PSUBW,  O::PCGTW,  O::PMAXW,  O::PADDH,  O::PSUBH,  O::PCGTH,
                                      O::PMAXH,  O::PADDB,  O::PSUBB,  O::PCGTB,  I,         I,         I,
                                      I,         I,         O::PADDSW, O::PSUBSW, O::PEXTLW, O::PPACW,  O::PADDSH,
                                      O::PSUBSH, O::PEXTLH, O::PPACH,  O::PADDSB, O::PSUBSB, O::PEXTLB, O::PPACB,
                                      I,         I,         O::PEXT5,  O::PPAC5};

constexpr std::array<Op, 32> kMmi1 = {
    I,         O::PABSW, O::PCEQW,  O::PMINW,  O::PADSBH, O::PABSH,  O::PCEQH,  O::PMINH,  I, I,         O::PCEQB,
    I,         I,        I,         I,         I,         O::PADDUW, O::PSUBUW, O::PEXTUW, I, O::PADDUH, O::PSUBUH,
    O::PEXTUH, I,        O::PADDUB, O::PSUBUB, O::PEXTUB, O::QFSRV,  I,         I,         I, I};

constexpr std::array<Op, 32> kMmi2 = {O::PMADDW, I,         O::PSLLVW, O::PSRLVW, O::PMSUBW, I,         I,
                                      I,         O::PMFHI,  O::PMFLO,  O::PINTH,  I,         O::PMULTW, O::PDIVW,
                                      O::PCPYLD, I,         O::PMADDH, O::PHMADH, O::PAND,   O::PXOR,   O::PMSUBH,
                                      O::PHMSBH, I,         I,         I,         I,         O::PEXEH,  O::PREVH,
                                      O::PMULTH, O::PDIVBW, O::PEXEW,  O::PROT3W};

constexpr std::array<Op, 32> kMmi3 = {O::PMADDUW, I,        I,         O::PSRAVW, I,          I,         I,         I,
                                      O::PMTHI,   O::PMTLO, O::PINTEH, I,         O::PMULTUW, O::PDIVUW, O::PCPYUD, I,
                                      I,          I,        O::POR,    O::PNOR,   I,          I,         I,         I,
                                      I,          I,        O::PEXCH,  O::PCPYH,  I,          I,         O::PEXCW,  I};

constexpr std::array<Op, 64> kFpuS = {
    O::ADD_S,  O::SUB_S,  O::MUL_S,  O::DIV_S, O::SQRT_S,  O::ABS_S,  O::MOV_S,   O::NEG_S,    // 0
    I,         I,         I,         I,        I,          I,         I,          I,           // 8
    I,         I,         I,         I,        I,          I,         O::RSQRT_S, I,           // 16
    O::ADDA_S, O::SUBA_S, O::MULA_S, I,        O::MADD_S,  O::MSUB_S, O::MADDA_S, O::MSUBA_S,  // 24
    I,         I,         I,         I,        O::CVT_W_S, I,         I,          I,           // 32
    O::MAX_S,  O::MIN_S,  I,         I,        I,          I,         I,          I,           // 40
    O::C_F_S,  I,         O::C_EQ_S, I,        O::C_LT_S,  I,         O::C_LE_S,  I,           // 48
    I,         I,         I,         I,        I,          I,         I,          I};          // 56

// VU0 macro-mode mnemonics. Special1 is indexed by funct (0..59); special2 by
// ((bits 10..6) << 2) | (bits 1..0), 0..127.
constexpr std::string_view kVuSpecial1[64] = {
    "vaddx",  "vaddy",   "vaddz",    "vaddw",  "vsubx",  "vsuby", "vsubz",  "vsubw", "vmaddx", "vmaddy", "vmaddz",
    "vmaddw", "vmsubx",  "vmsuby",   "vmsubz", "vmsubw", "vmaxx", "vmaxy",  "vmaxz", "vmaxw",  "vminix", "vminiy",
    "vminiz", "vminiw",  "vmulx",    "vmuly",  "vmulz",  "vmulw", "vmulq",  "vmaxi", "vmuli",  "vminii", "vaddq",
    "vmaddq", "vaddi",   "vmaddi",   "vsubq",  "vmsubq", "vsubi", "vmsubi", "vadd",  "vmadd",  "vmul",   "vmax",
    "vsub",   "vmsub",   "vopmsub",  "vmini",  "viadd",  "visub", "viaddi", "",      "viand",  "vior",   "",
    "",       "vcallms", "vcallmsr", "",       "",       "",      "",       "",      ""};

constexpr std::string_view kVuSpecial2[128] = {
    "vaddax",  "vadday",  "vaddaz",  "vaddaw",  "vsubax",  "vsubay",  "vsubaz",  "vsubaw",  "vmaddax", "vmadday",
    "vmaddaz", "vmaddaw", "vmsubax", "vmsubay", "vmsubaz", "vmsubaw", "vitof0",  "vitof4",  "vitof12", "vitof15",
    "vftoi0",  "vftoi4",  "vftoi12", "vftoi15", "vmulax",  "vmulay",  "vmulaz",  "vmulaw",  "vmulaq",  "vabs",
    "vmulai",  "vclipw",  "vaddaq",  "vmaddaq", "vaddai",  "vmaddai", "vsubaq",  "vmsubaq", "vsubai",  "vmsubai",
    "vadda",   "vmadda",  "vmula",   "",        "vsuba",   "vmsuba",  "vopmula", "vnop",    "vmove",   "vmr32",
    "",        "",        "vlqi",    "vsqi",    "vlqd",    "vsqd",    "vdiv",    "vsqrt",   "vrsqrt",  "vwaitq",
    "vmtir",   "vmfir",   "vilwr",   "viswr",   "vrnext",  "vrget",   "vrinit",  "vrxor"};

Op cop0(uint32_t w) {
  const unsigned rs = (w >> 21) & 31;
  if (rs == 0) return O::MFC0;
  if (rs == 4) return O::MTC0;
  if (rs == 8) {
    constexpr Op bc[4] = {O::BC0F, O::BC0T, O::BC0FL, O::BC0TL};
    unsigned rt = (w >> 16) & 31;
    return rt < 4 ? bc[rt] : I;
  }
  if (rs == 16) {
    switch (w & 63) {
      case 1: return O::TLBR;
      case 2: return O::TLBWI;
      case 6: return O::TLBWR;
      case 8: return O::TLBP;
      case 24: return O::ERET;
      case 56: return O::EI;
      case 57: return O::DI;
      default: return I;
    }
  }
  return I;
}

Op cop1(uint32_t w) {
  const unsigned rs = (w >> 21) & 31;
  switch (rs) {
    case 0: return O::MFC1;
    case 2: return O::CFC1;
    case 4: return O::MTC1;
    case 6: return O::CTC1;
    case 8: {
      constexpr Op bc[4] = {O::BC1F, O::BC1T, O::BC1FL, O::BC1TL};
      unsigned rt = (w >> 16) & 31;
      return rt < 4 ? bc[rt] : I;
    }
    case 16: return kFpuS[w & 63];
    case 20: return (w & 63) == 32 ? O::CVT_S_W : I;
    default: return I;
  }
}

Op cop2(uint32_t w, uint16_t* vu_opcode) {
  const unsigned rs = (w >> 21) & 31;
  if (rs & 16) {
    const unsigned funct = w & 63;
    if (funct >= 60) {
      unsigned idx = ((w >> 6) & 31) << 2 | (w & 3);
      *vu_opcode = static_cast<uint16_t>(idx);
      return idx < 128 && !kVuSpecial2[idx].empty() ? O::VU0_SPECIAL2 : I;
    }
    *vu_opcode = static_cast<uint16_t>(funct);
    return !kVuSpecial1[funct].empty() ? O::VU0_SPECIAL1 : I;
  }
  switch (rs) {
    case 1: return O::QMFC2;
    case 2: return O::CFC2;
    case 5: return O::QMTC2;
    case 6: return O::CTC2;
    case 8: {
      constexpr Op bc[4] = {O::BC2F, O::BC2T, O::BC2FL, O::BC2TL};
      unsigned rt = (w >> 16) & 31;
      return rt < 4 ? bc[rt] : I;
    }
    default: return I;
  }
}

Op mmi(uint32_t w) {
  const unsigned funct = w & 63, sub = (w >> 6) & 31;
  switch (funct) {
    case 8: return kMmi0[sub];
    case 40: return kMmi1[sub];
    case 9: return kMmi2[sub];
    case 41: return kMmi3[sub];
    case 48: {
      constexpr Op f[5] = {O::PMFHL_LW, O::PMFHL_UW, O::PMFHL_SLW, O::PMFHL_LH, O::PMFHL_SH};
      return sub < 5 ? f[sub] : I;
    }
    case 49: return sub == 0 ? O::PMTHL_LW : I;
    default: return kMmi[funct];
  }
}

constexpr std::string_view kGpr[32] = {"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",
                                       "t3",   "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5",
                                       "s6",   "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};

bool sa_is_subop(Op op) { return (op >= Op::PADDW && op <= Op::PEXCW) || (op >= Op::PMFHL_LW && op <= Op::PMTHL_LW); }

// VU0 macro ops: I/Q-register forms have no ft; integer ops have no dest; a few ops require a
// fixed dest mask.
bool vu0_reserved_ok(const Insn& in) {
  const uint32_t w = in.word;
  const unsigned dest = (w >> 21) & 15, ft = in.rt, fs = in.rd;
  if (in.op == Op::VU0_SPECIAL1) {
    const unsigned f = in.vu_opcode;
    if (f >= 28 && f <= 39) return ft == 0;
    if (f == 46) return dest == 0xE;  // vopmsub.xyz
    if (f == 48 || f == 49 || f == 50 || f == 52 || f == 53) return dest == 0;
    if (f == 56) return dest == 0;               // vcallms
    if (f == 57) return (w & 0x01FFFFC0u) == 0;  // vcallmsr
    return true;
  }
  const unsigned i = in.vu_opcode;
  if (i == 28 || i == 30 || (i >= 32 && i <= 39)) return ft == 0;
  if (i == 31 || i == 46) return dest == 0xE;                       // vclipw.xyz, vopmula.xyz
  if (i == 47 || i == 59) return (w & 0x01FFF800u) == 0;            // vnop, vwaitq
  if (i == 64 || i == 65) return fs == 0;                           // vrnext, vrget
  if (i == 66 || i == 67) return ft == 0 && (w & (3u << 21)) == 0;  // vrinit, vrxor
  if (i == 60) return (w & (3u << 23)) == 0;
  if (i == 57) return fs == 0 && (w & (3u << 21)) == 0;  // vsqrt Q,ft.ftf
  if (i == 62 || i == 63)
    return dest == 1 || dest == 2 || dest == 4 || dest == 8;  // vilwr/viswr: one field                        // vmtir
  return true;
}

// Must-be-zero fields, derived from which operands the format uses. Matches the strictness of
// GNU binutils' R5900 tables, which helps separate code from data during analysis.
bool reserved_fields_ok(const Insn& in, unsigned opcode) {
  const uint32_t w = in.word;
  const auto& info = kOps[static_cast<size_t>(in.op)];
  const bool sa_free = sa_is_subop(in.op) || (info.flags & kSystem);
  const bool sa_zero = sa_free || in.sa == 0;
  switch (in.op) {
    case Op::BLEZ:
    case Op::BGTZ:
    case Op::BLEZL:
    case Op::BGTZL: return in.rt == 0;
    case Op::LUI: return in.rs == 0;
    case Op::SYNC: return (w & 0x03FFF800u) == 0 && (in.sa == 0 || in.sa == 0x10);
    case Op::JALR: return in.rt == 0 && in.sa == 0;
    case Op::TLBR:
    case Op::TLBWI:
    case Op::TLBWR:
    case Op::TLBP:
    case Op::ERET:
    case Op::EI:
    case Op::DI: return (w & 0x01FFFFC0u) == 0;
    default: break;
  }
  if (in.op == Op::VU0_SPECIAL1 || in.op == Op::VU0_SPECIAL2) return vu0_reserved_ok(in);
  if (opcode != 0 && opcode != 16 && opcode != 17 && opcode != 18 && opcode != 28) return true;
  switch (info.fmt) {
    case Fmt::RdRsRt: return sa_zero;
    case Fmt::RdRtSa: return in.rs == 0;
    case Fmt::RdRtRs: return sa_zero;
    case Fmt::Rs: return in.rt == 0 && in.rd == 0 && sa_zero;
    case Fmt::RdRs: return in.rt == 0 && sa_zero;
    case Fmt::Rd: return in.rs == 0 && in.rt == 0 && sa_zero;
    case Fmt::RsRt: return (info.flags & kSystem) || (in.rd == 0 && sa_zero);
    case Fmt::RdRt: return in.rs == 0 && sa_zero;
    // COP0 $24 (debug) and $25 (performance counters) use the low bits as a sub-register select.
    case Fmt::RtCop0: {
      const uint32_t low = w & 0x7FFu;
      if (in.rd == 24) return low == 0 || (low >= 2 && low <= 7);  // BPC/IAB/IABM/DAB/DABM/DVB/DVBM
      if (in.rd == 25) return low < 0x40;                          // PCCR / PCR0 / PCR1 select
      return low == 0;
    }
    case Fmt::RtFs:
    case Fmt::RtFcr: return (w & 0x7FFu) == 0;
    case Fmt::RtVf:
    case Fmt::RtVi: return (w & 0x7FEu) == 0;
    case Fmt::FdFs: return in.rt == 0;
    case Fmt::FdFt: return in.rd == 0;
    case Fmt::FsFt: return in.sa == 0;
    default: return true;
  }
}

}  // namespace

std::string_view gpr_name(unsigned r) { return kGpr[r & 31]; }
std::string_view mnemonic(Op op) { return kOps[static_cast<size_t>(op)].mnemonic; }

bool Insn::always_taken() const {
  return (op == Op::BEQ && rs == 0 && rt == 0) || (op == Op::BGEZ && rs == 0) || (op == Op::BGEZAL && rs == 0);
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
    case 17: in.op = cop1(w); break;
    case 18: in.op = cop2(w, &in.vu_opcode); break;
    case 28: in.op = mmi(w); break;
    default: in.op = kPrimary[opcode]; break;
  }
  if (!reserved_fields_ok(in, opcode)) in.op = Op::Invalid;
  in.flags = kOps[static_cast<size_t>(in.op)].flags;
  if (in.has(kBranch)) in.target = pc + 4 + static_cast<uint32_t>(static_cast<int32_t>(in.simm) * 4);
  if (in.has(kJump)) in.target = ((pc + 4) & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2);
  if (in.always_taken()) in.flags |= kEnd;
  return in;
}

std::string_view vu0_mnemonic(const Insn& in) {
  if (in.op == Op::VU0_SPECIAL1) return kVuSpecial1[in.vu_opcode & 63];
  if (in.op == Op::VU0_SPECIAL2) return kVuSpecial2[in.vu_opcode & 127];
  return "(invalid)";
}

std::string disassemble(const Insn& in) {
  const auto& info = kOps[static_cast<size_t>(in.op)];
  const auto rs = gpr_name(in.rs), rt = gpr_name(in.rt), rd = gpr_name(in.rd);
  const std::string_view mn = info.fmt == Fmt::Vu ? vu0_mnemonic(in) : info.mnemonic;
  const unsigned fs = in.rd, ft = in.rt, fd = in.sa;
  switch (info.fmt) {
    case Fmt::None: return std::string(mn);
    case Fmt::RdRsRt: return std::format("{}\t{},{},{}", mn, rd, rs, rt);
    case Fmt::RdRtSa: return std::format("{}\t{},{},0x{:x}", mn, rd, rt, in.sa);
    case Fmt::RdRtRs: return std::format("{}\t{},{},{}", mn, rd, rt, rs);
    case Fmt::Rs: return std::format("{}\t{}", mn, rs);
    case Fmt::RdRs:
      if (in.op == Op::JALR && in.rd == 31) return std::format("{}\t{}", mn, rs);
      return std::format("{}\t{},{}", mn, rd, rs);
    case Fmt::Rd: return std::format("{}\t{}", mn, rd);
    case Fmt::RsRt:
      if (in.op == Op::DIV || in.op == Op::DIVU || in.op == Op::DIV1 || in.op == Op::DIVU1)
        return std::format("{}\tzero,{},{}", mn, rs, rt);  // GNU's 3-operand form
      return std::format("{}\t{},{}", mn, rs, rt);
    case Fmt::RdRt: return std::format("{}\t{},{}", mn, rd, rt);
    case Fmt::Code: {
      unsigned code = (in.word >> 6) & 0xFFFFF;
      return code ? std::format("{}\t0x{:x}", mn, code) : std::string(mn);
    }
    case Fmt::Sync: return (in.word & (1u << 10)) ? "sync.p" : "sync";
    case Fmt::RsImm: return std::format("{}\t{},{}", mn, rs, in.simm);
    case Fmt::RsBranch: return std::format("{}\t{},0x{:x}", mn, rs, in.target);
    case Fmt::RsRtBranch: return std::format("{}\t{},{},0x{:x}", mn, rs, rt, in.target);
    case Fmt::Branch:
    case Fmt::Jump: return std::format("{}\t0x{:x}", mn, in.target);
    case Fmt::RtRsImm: return std::format("{}\t{},{},{}", mn, rt, rs, in.simm);
    case Fmt::RtRsUimm: return std::format("{}\t{},{},0x{:x}", mn, rt, rs, in.uimm);
    case Fmt::RtUimm: return std::format("{}\t{},0x{:x}", mn, rt, in.uimm);
    case Fmt::Mem: return std::format("{}\t{},{}({})", mn, rt, in.simm, rs);
    case Fmt::MemFt: return std::format("{}\t$f{},{}({})", mn, in.rt, in.simm, rs);
    case Fmt::MemVf: return std::format("{}\t$vf{},{}({})", mn, in.rt, in.simm, rs);
    case Fmt::CacheOp: return std::format("{}\t0x{:x},{}({})", mn, in.rt, in.simm, rs);
    case Fmt::RtCop0: return std::format("{}\t{},${}", mn, rt, in.rd);
    case Fmt::RtFs: return std::format("{}\t{},$f{}", mn, rt, fs);
    case Fmt::RtFcr: return std::format("{}\t{},${}", mn, rt, fs);
    case Fmt::FdFsFt: return std::format("{}\t$f{},$f{},$f{}", mn, fd, fs, ft);
    case Fmt::FdFs: return std::format("{}\t$f{},$f{}", mn, fd, fs);
    case Fmt::FdFt: return std::format("{}\t$f{},$f{}", mn, fd, ft);
    case Fmt::FsFt: return std::format("{}\t$f{},$f{}", mn, fs, ft);
    case Fmt::RtVf: return std::format("{}\t{},$vf{}", mn, rt, in.rd);
    case Fmt::RtVi: return std::format("{}\t{},$vi{}", mn, rt, in.rd);
    case Fmt::Vu: {
      const unsigned dest = (in.word >> 21) & 15;
      std::string d;
      for (int b = 3; b >= 0; --b)
        if (dest & (1u << b)) d += "wzyx"[b];
      if (in.op == Op::VU0_SPECIAL1 && (in.vu_opcode < 28 || (in.vu_opcode >= 40 && in.vu_opcode <= 47))) {
        // vOPbc / vOP: fd = bits 10..6, fs = 15..11, ft = 20..16; bc ops broadcast one ft field.
        std::string ftsuf = in.vu_opcode < 28 ? std::string(1, "xyzw"[in.vu_opcode & 3]) : d;
        return std::format("{}.{}\t$vf{}{},$vf{}{},$vf{}{}", mn, d, in.sa, d, in.rd, d, in.rt, ftsuf);
      }
      return std::format("{}.{}\tft={},fs={},fd={}", mn, d, in.rt, in.rd, in.sa);
    }
  }
  return std::string(mn);
}

}  // namespace lantern::ee
