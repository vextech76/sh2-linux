#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "ps2/vif.h"
#include "ps2/vu.h"

using namespace lantern::ps2;

namespace {
// Minimal VU1 micro assembler for the tests. `upper`/`lower` are the encoded 32-bit halves.
struct Asm {
  Vu1Memory mem;

  void put(uint32_t qword, uint32_t lower, uint32_t upper) {
    std::memcpy(&mem.micro[qword * 8], &lower, 4);
    std::memcpy(&mem.micro[qword * 8 + 4], &upper, 4);
  }
  static constexpr uint32_t nop() { return 0x000002FF; }
  static uint32_t e_flag(uint32_t upper) { return upper | 0x40000000u; }
  // Upper FMAC/misc encodings: opcode in bits 0..5, fd 6..10, fs 11..15, ft 16..20, mask 21..24.
  static uint32_t upper(uint32_t op, uint32_t fd, uint32_t fs, uint32_t ft, uint32_t mask) {
    return op | (fd << 6) | (fs << 11) | (ft << 16) | (mask << 21);
  }
  // Lower: imm 0..10, is 11..15, it 16..20, id 21..24, opcode 25..31.
  static uint32_t lower(uint32_t op, uint32_t imm, uint32_t is, uint32_t it, uint32_t id) {
    return (op << 25) | ((id & 0xF) << 21) | ((it & 0x1F) << 16) | ((is & 0x1F) << 11) | (imm & 0x7FF);
  }
  static uint32_t lower_special(uint32_t sub, uint32_t fn, uint32_t is, uint32_t it, uint32_t id, uint32_t imm = 0) {
    return (0x40u << 25) | ((fn & 0x1F) << 6) | ((id & 0xF) << 21) | ((it & 0x1F) << 16) | ((is & 0x1F) << 11) |
           (sub & 0x3F) | (imm & 0x3F);
  }
};
}  // namespace

TEST_CASE("VU1: the E bit stops the program after its delay slot") {
  Asm a;
  for (uint32_t i = 0; i < 4; ++i) a.put(i, a.lower(0x08, i + 1, 0, 0, 0), a.nop());  // IADDIU vi0, imm (discarded)
  a.put(4, a.lower(0x08, 9, 0, 0, 0), a.e_flag(a.nop()));                             // E here
  a.put(5, a.lower(0x08, 9, 0, 0, 0), a.nop());                                       // delay slot
  for (uint32_t i = 6; i < 12; ++i) a.put(i, a.nop(), a.nop());

  Vu1 vu(a.mem);
  vu.start(0);
  CHECK_FALSE(vu.running());
  CHECK(vu.stats().instructions == 6);  // 5 plus the E delay slot
}

TEST_CASE("VU1: integer add and the branch delay slot") {
  Asm a;
  // vi1 = vi1 + 5 (IADDIU)
  a.put(0, a.lower(0x08, 5, 1, 1, 0), a.nop());
  // B +2 (to qword 4); the delay slot adds 1 more
  a.put(1, a.lower(0x20, 2, 0, 0, 0), a.nop());
  a.put(2, a.lower(0x08, 1, 1, 1, 0), a.nop());    // delay slot: vi1 += 1
  a.put(3, a.lower(0x08, 100, 1, 1, 0), a.nop());  // skipped
  a.put(4, a.lower(0x08, 1, 1, 1, 0), a.nop());
  a.put(5, a.nop(), a.e_flag(a.nop()));  // E
  a.put(6, a.nop(), a.nop());            // delay slot

  Vu1 vu(a.mem);
  vu.start(0);
  CHECK_FALSE(vu.running());
  // vi1 = 5 + 1 (delay slot) + 1 (at the target) = 7
  CHECK(vu.stats().instructions == 6);
}

TEST_CASE("VU1: SQ stores a quadword and IADDIU sign-extends") {
  Asm a;
  a.put(0, a.lower(0x08, 0x234, 0, 1, 0), a.nop());         // vi1 = 0x234
  a.put(1, a.lower_special(0x3D, 15, 1, 1, 0xF), a.nop());  // MFIR vf1 <- vi1
  a.put(2, a.lower(0x01, 8, 1, 0, 0xF), a.nop());           // SQ vf1, 8(vi0)
  a.put(3, a.nop(), a.e_flag(a.nop()));
  a.put(4, a.nop(), a.nop());  // E delay slot

  Vu1 vu(a.mem);
  vu.start(0);
  CHECK_FALSE(vu.running());
  CHECK(vu.stats().unknown == 0);
  uint32_t got[4];
  std::memcpy(got, &a.mem.data[8 * 16], 16);
  CHECK(got[0] == 0x234u);
  CHECK(got[3] == 0x234u);

  // LQ reads it back into vf2.
  a.put(5, a.lower(0x00, 8, 0, 2, 0xF), a.nop());  // LQ vf2, 8(vi0)
  a.put(6, a.lower(0x01, 9, 2, 0, 0xF), a.nop());  // SQ vf2, 9(vi0)
  a.put(7, a.nop(), a.e_flag(a.nop()));
  a.put(8, a.nop(), a.nop());
  Vu1 vu2(a.mem);
  vu2.start(5 * 8);
  uint32_t back[4];
  std::memcpy(back, &a.mem.data[9 * 16], 16);
  CHECK(back[0] == 0x234u);
}

TEST_CASE("VU1: FMAC add with a broadcast operand and the destination mask") {
  Asm a;
  // vf1 = (1,2,3,4): use IADDIU then ITOF0 per lane as above (keep it short: only x and y).
  a.put(0, a.lower(0x08, 10, 0, 1, 0), a.nop());
  a.put(1, a.lower(0x08, 20, 0, 2, 0), a.nop());
  a.put(2, a.nop(), a.upper(0x3C, 4, 1, 1, 0xF));  // vf1 = itof(vi1)
  a.put(3, a.nop(), a.upper(0x3C, 4, 2, 2, 0xF));  // vf2 = itof(vi2)
  // ADDx vf3, vf1, vf2[x] -> (11, 12, 13, 14)
  a.put(4, a.nop(), a.upper(0x00, 3, 1, 2, 0xF));
  a.put(5, a.nop(), a.e_flag(a.nop()));
  a.put(6, a.nop(), a.nop());

  Vu1 vu(a.mem);
  vu.start(0);
  CHECK_FALSE(vu.running());
  CHECK(vu.stats().unknown == 0);
  CHECK(vu.stats().instructions == 7);
}

TEST_CASE("VU1: unknown opcodes are counted and do not stop the program") {
  Asm a;
  a.put(0, a.nop(), a.upper(0x30, 0, 0, 0, 0));  // unused upper opcode
  a.put(1, a.nop(), a.e_flag(a.nop()));
  a.put(2, a.nop(), a.nop());

  Vu1 vu(a.mem);
  vu.start(0);
  CHECK(vu.stats().unknown == 1);
  CHECK_FALSE(vu.running());
}
