#include <doctest/doctest.h>

#include <cstdint>
#include <format>
#include <string>

#include "ee/r5900.h"

using namespace lantern::ee;

namespace {
struct Golden {
  uint32_t word;
  const char* text;
};
// Assembled with GNU as -march=r5900 (-mabi=eabi register numbering) and disassembled with
// objdump -m mips:5900 -M no-aliases,gpr-names=32, at address 0. Synthetic, not game code.
constexpr Golden kGolden[] = {
    {0x27bdffe0, "addiu\tsp,sp,-32"},
    {0xafbf001c, "sw\tra,28(sp)"},
    {0x8fbf001c, "lw\tra,28(sp)"},
    {0x3c020012, "lui\tv0,0x12"},
    {0x34423456, "ori\tv0,v0,0x3456"},
    {0x31ac00ff, "andi\tt4,t5,0xff"},
    {0x39ac0001, "xori\tt4,t5,0x1"},
    {0x29acfffb, "slti\tt4,t5,-5"},
    {0x2dac0007, "sltiu\tt4,t5,7"},
    {0x00851021, "addu\tv0,a0,a1"},
    {0x0080102d, "daddu\tv0,a0,zero"},
    {0x00851023, "subu\tv0,a0,a1"},
    {0x00851024, "and\tv0,a0,a1"},
    {0x00851025, "or\tv0,a0,a1"},
    {0x00851026, "xor\tv0,a0,a1"},
    {0x00851027, "nor\tv0,a0,a1"},
    {0x0085102a, "slt\tv0,a0,a1"},
    {0x0085102b, "sltu\tv0,a0,a1"},
    {0x00031080, "sll\tv0,v1,0x2"},
    {0x000317c2, "srl\tv0,v1,0x1f"},
    {0x00031103, "sra\tv0,v1,0x4"},
    {0x00831004, "sllv\tv0,v1,a0"},
    {0x0003103c, "dsll32\tv0,v1,0x0"},
    {0x000310fb, "dsra\tv0,v1,0x3"},
    {0x0085100a, "movz\tv0,a0,a1"},
    {0x0085100b, "movn\tv0,a0,a1"},
    {0x00851018, "mult\tv0,a0,a1"},
    {0x00851019, "multu\tv0,a0,a1"},
    {0x0085001a, "div\tzero,a0,a1"},
    {0x0085001b, "divu\tzero,a0,a1"},
    {0x00001010, "mfhi\tv0"},
    {0x00001812, "mflo\tv1"},
    {0x00800011, "mthi\ta0"},
    {0x00800013, "mtlo\ta0"},
    {0x00006028, "mfsa\tt4"},
    {0x01800029, "mtsa\tt4"},
    {0x05980003, "mtsab\tt4,3"},
    {0x03e00008, "jr\tra"},
    {0x0320f809, "jalr\tt9"},
    {0x0000000c, "syscall"},
    {0x0000000d, "break"},
    {0x0000000f, "sync"},
    {0x0000040f, "sync.p"},
    {0x808cffff, "lb\tt4,-1(a0)"},
    {0x908c0001, "lbu\tt4,1(a0)"},
    {0x848c0002, "lh\tt4,2(a0)"},
    {0x948c0002, "lhu\tt4,2(a0)"},
    {0x888c0003, "lwl\tt4,3(a0)"},
    {0x988c0000, "lwr\tt4,0(a0)"},
    {0x9c8c0004, "lwu\tt4,4(a0)"},
    {0xdc8c0008, "ld\tt4,8(a0)"},
    {0xfc8c0008, "sd\tt4,8(a0)"},
    {0x688c0007, "ldl\tt4,7(a0)"},
    {0x6c8c0000, "ldr\tt4,0(a0)"},
    {0xb08c0007, "sdl\tt4,7(a0)"},
    {0xb48c0000, "sdr\tt4,0(a0)"},
    {0x788c0010, "lq\tt4,16(a0)"},
    {0x7c8c0010, "sq\tt4,16(a0)"},
    {0xa08c0000, "sb\tt4,0(a0)"},
    {0xa48c0000, "sh\tt4,0(a0)"},
    {0xa88c0003, "swl\tt4,3(a0)"},
    {0xb88c0000, "swr\tt4,0(a0)"},
    {0xbc980000, "cache\t0x18,0(a0)"},
    {0xcc800000, "pref\t0x0,0(a0)"},
    {0xc4800004, "lwc1\t$f0,4(a0)"},
    {0xe49f0004, "swc1\t$f31,4(a0)"},
    {0xd8810010, "lqc2\t$vf1,16(a0)"},
    {0xf8810010, "sqc2\t$vf1,16(a0)"},
    {0x400c6000, "mfc0\tt4,$12"},  // GNU: c0_sr (LANTERN prints numeric CP0/CP1 regs)
    {0x408c6000, "mtc0\tt4,$12"},
    {0x42000018, "eret"},
    {0x42000038, "ei"},
    {0x42000039, "di"},
    {0x440c0800, "mfc1\tt4,$f1"},
    {0x448c0800, "mtc1\tt4,$f1"},
    {0x444cf800, "cfc1\tt4,$31"},  // GNU: c1_fcsr
    {0x44ccf800, "ctc1\tt4,$31"},
    {0x46020800, "add.s\t$f0,$f1,$f2"},
    {0x46020801, "sub.s\t$f0,$f1,$f2"},
    {0x46020802, "mul.s\t$f0,$f1,$f2"},
    {0x46020803, "div.s\t$f0,$f1,$f2"},
    {0x46000805, "abs.s\t$f0,$f1"},
    {0x46000806, "mov.s\t$f0,$f1"},
    {0x46000807, "neg.s\t$f0,$f1"},
    {0x46020816, "rsqrt.s\t$f0,$f1,$f2"},
    {0x46020818, "adda.s\t$f1,$f2"},
    {0x4602081a, "mula.s\t$f1,$f2"},
    {0x4602081c, "madd.s\t$f0,$f1,$f2"},
    {0x4602081d, "msub.s\t$f0,$f1,$f2"},
    {0x4602081e, "madda.s\t$f1,$f2"},
    {0x46020828, "max.s\t$f0,$f1,$f2"},
    {0x46020829, "min.s\t$f0,$f1,$f2"},
    {0x46020832, "c.eq.s\t$f1,$f2"},
    {0x46020834, "c.lt.s\t$f1,$f2"},
    {0x46020836, "c.le.s\t$f1,$f2"},
    {0x46020830, "c.f.s\t$f1,$f2"},
    {0x46800820, "cvt.s.w\t$f0,$f1"},
    {0x70851000, "madd\tv0,a0,a1"},
    {0x70851021, "maddu1\tv0,a0,a1"},
    {0x70851018, "mult1\tv0,a0,a1"},
    {0x7085001a, "div1\tzero,a0,a1"},
    {0x70001010, "mfhi1\tv0"},
    {0x70400013, "mtlo1\tv0"},
    {0x70801004, "plzcw\tv0,a0"},
    {0x70851008, "paddw\tv0,a0,a1"},
    {0x70851248, "psubb\tv0,a0,a1"},
    {0x70851488, "pextlw\tv0,a0,a1"},
    {0x708515c8, "ppach\tv0,a0,a1"},
    {0x70051788, "pext5\tv0,a1"},
    {0x70051068, "pabsw\tv0,a1"},
    {0x708510a8, "pceqw\tv0,a0,a1"},
    {0x708516a8, "pextub\tv0,a0,a1"},
    {0x708516e8, "qfsrv\tv0,a0,a1"},
    {0x70851009, "pmaddw\tv0,a0,a1"},
    {0x70851089, "psllvw\tv0,a1,a0"},
    {0x70001209, "pmfhi\tv0"},
    {0x70001249, "pmflo\tv0"},
    {0x70851389, "pcpyld\tv0,a0,a1"},
    {0x70851489, "pand\tv0,a0,a1"},
    {0x708514c9, "pxor\tv0,a0,a1"},
    {0x70051689, "pexeh\tv0,a1"},
    {0x700517c9, "prot3w\tv0,a1"},
    {0x70850349, "pdivw\ta0,a1"},
    {0x708510e9, "psravw\tv0,a1,a0"},
    {0x70800229, "pmthi\ta0"},
    {0x708513a9, "pcpyud\tv0,a0,a1"},
    {0x708514a9, "por\tv0,a0,a1"},
    {0x708514e9, "pnor\tv0,a0,a1"},
    {0x700516e9, "pcpyh\tv0,a1"},
    {0x700517a9, "pexcw\tv0,a1"},
    {0x70001030, "pmfhl.lw\tv0"},
    {0x70001130, "pmfhl.sh\tv0"},
    {0x70800031, "pmthl.lw\ta0"},
    {0x700510f4, "psllh\tv0,a1,0x3"},
    {0x700517ff, "psraw\tv0,a1,0x1f"},
    {0x482c0800, "qmfc2\tt4,$vf1"},
    {0x48ac1000, "qmtc2\tt4,$vf2"},
    {0x484c1800, "cfc2\tt4,$vi3"},
    {0x48cc2000, "ctc2\tt4,$vi4"},
    {0x4be31068, "vadd.xyzw\t$vf1xyzw,$vf2xyzw,$vf3xyzw"},
    {0x4bc31058, "vmulx.xyz\t$vf1xyz,$vf2xyz,$vf3x"},
    {0x4a26290b, "vmaddw.w\t$vf4w,$vf5w,$vf6w"},
    {0x4b83106c, "vsub.xy\t$vf1xy,$vf2xy,$vf3xy"},
    {0x4bc3106e, "vopmsub.xyz\t$vf1xyz,$vf2xyz,$vf3xyz"},
};
}  // namespace

TEST_CASE("golden encodings match GNU binutils text") {
  for (const auto& g : kGolden) {
    Insn insn = decode(g.word, 0);
    INFO(std::format("{:08x}", g.word));
    CHECK(insn.valid());
    CHECK(disassemble(insn) == g.text);
  }
}

TEST_CASE("R5900 deviations from GNU are intentional") {
  // CVT.W.S truncates on the R5900; we keep the manual's name.
  CHECK(disassemble(decode(0x46000024, 0)) == "cvt.w.s\t$f0,$f0");
  // SQRT.S is "fd, ft" on the R5900 (fs must be 0).
  CHECK(disassemble(decode(0x460c0304, 0)) == "sqrt.s\t$f12,$f12");
  CHECK_FALSE(decode(0x4600a144, 0).valid());  // fs != 0
}

TEST_CASE("instructions the R5900 lacks are invalid") {
  CHECK_FALSE(decode(0xC0000000, 0).valid());  // ll
  CHECK_FALSE(decode(0xE0000000, 0).valid());  // sc
  CHECK_FALSE(decode(0xD4000000, 0).valid());  // ldc1 (no double FPU)
  CHECK_FALSE(decode(0x0000001C, 0).valid());  // dmult
  CHECK_FALSE(decode(0x74000000, 0).valid());  // jalx
  CHECK_FALSE(decode(0x00000441, 0).valid());  // movci-style funct 1
  CHECK_FALSE(decode(0x40400000, 0).valid());  // cfc0 (often a float constant: 3.0f)
}

TEST_CASE("reserved fields make an encoding invalid") {
  CHECK(decode(0x00031080, 0).valid());        // sll v0,v1,2
  CHECK_FALSE(decode(0x00231080, 0).valid());  // sll with rs != 0
  CHECK_FALSE(decode(0x008510e1, 0).valid());  // addu with sa != 0
  CHECK_FALSE(decode(0x1c22ffff, 0).valid());  // bgtz with rt != 0
  CHECK_FALSE(decode(0x3c220001, 0).valid());  // lui with rs != 0
}

TEST_CASE("control-flow metadata") {
  auto jal = decode(0x0c040040, 0x00100000);  // jal 0x100100
  CHECK(jal.op == Op::JAL);
  CHECK(jal.target == 0x00100100);
  CHECK(jal.has(kLink));
  CHECK(jal.has(kDelay));
  CHECK_FALSE(jal.has(kEnd));

  auto beq = decode(0x1040fffe, 0x00100010);  // beq v0,zero,-2 insns
  CHECK(beq.has(kBranch));
  CHECK(beq.target == 0x0010000c);
  CHECK_FALSE(beq.has(kEnd));

  auto b = decode(0x10000003, 0x00100000);  // beq zero,zero = unconditional
  CHECK(b.always_taken());
  CHECK(b.has(kEnd));

  auto jr = decode(0x03e00008, 0);
  CHECK(jr.op == Op::JR);
  CHECK(jr.has(kIndirect));
  CHECK(jr.has(kEnd));

  auto bl = decode(0x5040ffff, 0x100);  // beql
  CHECK(bl.has(kLikely));

  auto sys = decode(0x0000000c, 0);
  CHECK(sys.has(kSystem));
  auto eret = decode(0x42000018, 0);
  CHECK(eret.op == Op::ERET);
  CHECK(eret.has(kEnd));
}

TEST_CASE("VU0 macro special2 decoding") {
  // vnop: special2 index 47 -> bits 10..6 = 11, bits 1..0 = 3, funct = 0x3F
  auto nop = decode(0x4a0002ff, 0);
  CHECK(nop.op == Op::VU0_SPECIAL2);
  CHECK(vu0_mnemonic(nop) == "vnop");
  // vcallms 0x10
  auto call = decode(0x4a0000b8, 0);
  CHECK(vu0_mnemonic(call) == "vcallms");
}

TEST_CASE("every opcode has a mnemonic") {
  for (size_t i = 0; i < static_cast<size_t>(Op::Count); ++i) CHECK_FALSE(mnemonic(static_cast<Op>(i)).empty());
}
