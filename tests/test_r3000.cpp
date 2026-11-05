#include <doctest/doctest.h>

#include <cstdint>
#include <format>
#include <string>

#include "iop/iop_interp.h"
#include "iop/r3000.h"

using namespace lantern::iop;

namespace {
struct Golden {
  uint32_t pc;
  uint32_t word;
  const char* text;
};
// Assembled with GNU as -march=r3000 -mabi=32 and disassembled with
// objdump -m mips:3000 -M no-aliases,gpr-names=32, at address 0. Synthetic, not game code.
constexpr Golden kGolden[] = {
    {0x00000000, 0x00031080, "sll\tv0,v1,0x2"},
    {0x00000004, 0x000317c2, "srl\tv0,v1,0x1f"},
    {0x00000008, 0x00031103, "sra\tv0,v1,0x4"},
    {0x0000000c, 0x00831004, "sllv\tv0,v1,a0"},
    {0x00000010, 0x00831006, "srlv\tv0,v1,a0"},
    {0x00000014, 0x00831007, "srav\tv0,v1,a0"},
    {0x00000018, 0x03e00008, "jr\tra"},
    {0x0000001c, 0x0320f809, "jalr\tt9"},
    {0x00000020, 0x0000000c, "syscall"},
    {0x00000024, 0x0000000d, "break"},
    {0x00000028, 0x00001010, "mfhi\tv0"},
    {0x0000002c, 0x00800011, "mthi\ta0"},
    {0x00000030, 0x00001012, "mflo\tv0"},
    {0x00000034, 0x00800013, "mtlo\ta0"},
    {0x00000038, 0x00850018, "mult\ta0,a1"},
    {0x0000003c, 0x00850019, "multu\ta0,a1"},
    {0x00000040, 0x14a00002, "bne\ta1,zero,0x4c"},
    {0x00000044, 0x0085001a, "div\tzero,a0,a1"},
    {0x00000048, 0x0007000d, "break\t0x7"},
    {0x0000004c, 0x2401ffff, "addiu\tat,zero,-1"},
    {0x00000050, 0x14a10004, "bne\ta1,at,0x64"},
    {0x00000054, 0x3c018000, "lui\tat,0x8000"},
    {0x00000058, 0x14810002, "bne\ta0,at,0x64"},
    {0x0000005c, 0x00000000, "sll\tzero,zero,0x0"},
    {0x00000060, 0x0006000d, "break\t0x6"},
    {0x00000064, 0x00002012, "mflo\ta0"},
    {0x00000068, 0x14a00002, "bne\ta1,zero,0x74"},
    {0x0000006c, 0x0085001b, "divu\tzero,a0,a1"},
    {0x00000070, 0x0007000d, "break\t0x7"},
    {0x00000074, 0x00002012, "mflo\ta0"},
    {0x00000078, 0x00851020, "add\tv0,a0,a1"},
    {0x0000007c, 0x00851021, "addu\tv0,a0,a1"},
    {0x00000080, 0x00851022, "sub\tv0,a0,a1"},
    {0x00000084, 0x00851023, "subu\tv0,a0,a1"},
    {0x00000088, 0x00851024, "and\tv0,a0,a1"},
    {0x0000008c, 0x00851025, "or\tv0,a0,a1"},
    {0x00000090, 0x00851026, "xor\tv0,a0,a1"},
    {0x00000094, 0x00851027, "nor\tv0,a0,a1"},
    {0x00000098, 0x0085102a, "slt\tv0,a0,a1"},
    {0x0000009c, 0x0085102b, "sltu\tv0,a0,a1"},
    {0x000000a0, 0x04800003, "bltz\ta0,0xb0"},
    {0x000000a4, 0x04810002, "bgez\ta0,0xb0"},
    {0x000000a8, 0x04900001, "bltzal\ta0,0xb0"},
    {0x000000ac, 0x04910000, "bgezal\ta0,0xb0"},
    {0x000000b0, 0x00851021, "addu\tv0,a0,a1"},
    {0x000000b4, 0x2082fffb, "addi\tv0,a0,-5"},
    {0x000000b8, 0x2482fffb, "addiu\tv0,a0,-5"},
    {0x000000bc, 0x28820007, "slti\tv0,a0,7"},
    {0x000000c0, 0x2c820007, "sltiu\tv0,a0,7"},
    {0x000000c4, 0x308200ff, "andi\tv0,a0,0xff"},
    {0x000000c8, 0x34820012, "ori\tv0,a0,0x12"},
    {0x000000cc, 0x38820012, "xori\tv0,a0,0x12"},
    {0x000000d0, 0x3c021234, "lui\tv0,0x1234"},
    {0x000000d4, 0x8082ffff, "lb\tv0,-1(a0)"},
    {0x000000d8, 0x84820002, "lh\tv0,2(a0)"},
    {0x000000dc, 0x88820003, "lwl\tv0,3(a0)"},
    {0x000000e0, 0x8c820004, "lw\tv0,4(a0)"},
    {0x000000e4, 0x90820005, "lbu\tv0,5(a0)"},
    {0x000000e8, 0x94820006, "lhu\tv0,6(a0)"},
    {0x000000ec, 0x98820007, "lwr\tv0,7(a0)"},
    {0x000000f0, 0xa0820001, "sb\tv0,1(a0)"},
    {0x000000f4, 0xa4820002, "sh\tv0,2(a0)"},
    {0x000000f8, 0xa8820003, "swl\tv0,3(a0)"},
    {0x000000fc, 0xac820004, "sw\tv0,4(a0)"},
    {0x00000100, 0xb8820005, "swr\tv0,5(a0)"},
    {0x00000104, 0x40026000, "mfc0\tv0,$12"},
    {0x00000108, 0x40826000, "mtc0\tv0,$12"},
    {0x0000010c, 0x42000010, "rfe"},
};
constexpr uint32_t kBase = 0x00100000;

struct Machine {
  IopMemory mem;
  IopCpu cpu;
  Interpreter interp;
  void code(std::initializer_list<uint32_t> words, uint32_t at = kBase) {
    uint32_t a = at;
    for (uint32_t w : words) {
      mem.write<uint32_t>(a, w);
      a += 4;
    }
  }
  uint64_t run(uint32_t stop = 0xDEAD0000) {
    cpu.pc = kBase;
    cpu.gpr[31] = stop;
    return interp.run(cpu, mem, stop);
  }
};

constexpr uint32_t R(unsigned rs, unsigned rt, unsigned rd, unsigned sa, unsigned f) {
  return rs << 21 | rt << 16 | rd << 11 | sa << 6 | f;
}
constexpr uint32_t I(unsigned op, unsigned rs, unsigned rt, int imm) {
  return op << 26 | rs << 21 | rt << 16 | (static_cast<uint32_t>(imm) & 0xFFFF);
}
constexpr uint32_t addiu(unsigned t, unsigned s, int imm) { return I(9, s, t, imm); }
constexpr uint32_t lw(unsigned t, int off, unsigned b) { return I(35, b, t, off); }
constexpr uint32_t sw(unsigned t, int off, unsigned b) { return I(43, b, t, off); }
constexpr uint32_t lbu(unsigned t, int off, unsigned b) { return I(36, b, t, off); }
constexpr uint32_t addu(unsigned d, unsigned s, unsigned t) { return R(s, t, d, 0, 33); }
constexpr uint32_t jr(unsigned s) { return R(s, 0, 0, 0, 8); }
constexpr uint32_t nop = 0;
}  // namespace

TEST_CASE("R3000A golden encodings match GNU binutils") {
  for (const auto& g : kGolden) {
    const Insn in = decode(g.word, g.pc);  // branch/jump text depends on the address
    INFO(std::format("{:08x}", g.word));
    CHECK(in.valid());
    CHECK(disassemble(in) == g.text);
  }
}

TEST_CASE("load delay: the instruction after a load still sees the old value") {
  Machine m;
  m.cpu.gpr[4] = 0x00180000;  // a0
  m.cpu.gpr[5] = 0xAAAAAAAA;  // a1 (old value)
  m.mem.write<uint32_t>(0x00180000, 0x12345678);
  m.code({lw(5, 0, 4),    // a1 <- load (delayed)
          addu(6, 5, 0),  // delay slot: must see the OLD a1
          addu(7, 5, 0),  // now sees the loaded value
          jr(31), nop});
  m.run();
  CHECK(m.cpu.gpr[6] == 0xAAAAAAAAu);
  CHECK(m.cpu.gpr[7] == 0x12345678u);
}

TEST_CASE("back-to-back loads: only the last one lands (hardware behaviour)") {
  Machine m;
  m.cpu.gpr[4] = 0x00180000;
  m.mem.write<uint32_t>(0x00180000, 1);
  m.mem.write<uint32_t>(0x00180004, 2);
  m.code({lw(5, 0, 4), lw(6, 4, 4), addu(7, 5, 0), addu(8, 6, 0), jr(31), nop});
  m.run();
  CHECK(m.cpu.gpr[7] == 1);  // the first load did reach its delay slot
  CHECK(m.cpu.gpr[8] == 2);
}

TEST_CASE("branch delay slot executes; jal links before the slot runs") {
  Machine m;
  const uint32_t sub = kBase + 0x40;
  // Save $ra (jal clobbers it), call, and return through the saved copy.
  m.code({addu(8, 0, 0),                         // v0 = 0
          addu(16, 31, 0),                       // s0 = return address
          3u << 26 | ((sub >> 2) & 0x03FFFFFF),  // jal sub
          addiu(8, 0, 7),                        // delay slot: runs before the call body
          jr(16), nop});
  m.code({addiu(9, 0, 9), jr(31), nop}, sub);
  m.run();
  CHECK(m.cpu.gpr[8] == 7);             // the delay slot executed exactly once, before the call
  CHECK(m.cpu.gpr[9] == 9);             // the callee ran
  CHECK(m.cpu.gpr[16] == 0xDEAD0000u);  // and it was reached through the jal link
}

TEST_CASE("HI/LO: MULT is signed 64-bit, DIV follows MIPS I results") {
  Machine m;
  m.code({addiu(4, 0, -3), addiu(5, 0, 7), R(4, 5, 0, 0, 24) /*mult*/, R(0, 0, 6, 0, 16) /*mfhi*/,  //
          R(0, 0, 7, 0, 18) /*mflo*/, addiu(4, 0, 17), addiu(5, 0, -5), R(4, 5, 0, 0, 26) /*div*/,  //
          R(0, 0, 8, 0, 18), addiu(4, 0, 5), R(4, 0, 0, 0, 26) /*div by zero*/, R(0, 0, 9, 0, 18),  //
          R(0, 0, 10, 0, 16), jr(31), nop});
  m.run();
  CHECK(m.cpu.gpr[6] == 0xFFFFFFFFu);  // -3*7 = -21 -> hi = -1
  CHECK(m.cpu.gpr[7] == 0xFFFFFFEBu);  // -21
  CHECK(m.cpu.gpr[8] == 0xFFFFFFFDu);  // 17 / -5 = -3
  CHECK(m.cpu.gpr[9] == 0xFFFFFFFFu);  // 5 / 0 = -1
  CHECK(m.cpu.gpr[10] == 5u);          // remainder = dividend
}

TEST_CASE("traps: unaligned word, invalid encoding, syscall without a kernel") {
  Machine a;
  a.cpu.gpr[4] = 0x00200002;
  a.code({lw(5, 0, 4), jr(31), nop});
  CHECK_THROWS_AS(a.run(), Trap);

  Machine b;
  b.code({0xFFFFFFFFu});
  CHECK_THROWS_AS(b.run(), Trap);

  Machine c;
  c.code({0x0000000Cu /* syscall */, jr(31), nop});
  CHECK_THROWS_AS(c.run(), Trap);
  c.interp.set_syscall_hook([](IopCpu& cpu, IopMemory&, uint32_t) { cpu.gpr[2] = 42; });
  Machine d;
  d.code({0x0000000Cu, jr(31), nop});
  d.interp.set_syscall_hook([](IopCpu& cpu, IopMemory&, uint32_t) { cpu.gpr[2] = 42; });
  d.run();
  CHECK(d.cpu.gpr[2] == 42);
}

TEST_CASE("the IOP has no 64-bit or MMI instructions") {
  CHECK_FALSE(decode(0x0000001Cu, 0).valid());  // dmult (EE only)
  CHECK_FALSE(decode(0x7C000000u, 0).valid());  // MMI (EE only)
  CHECK_FALSE(decode(0x01000000u, 0).valid());  // sync (EE only)
}
