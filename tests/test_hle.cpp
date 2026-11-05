#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "iop/hle.h"
#include "iop/iop_interp.h"

using namespace lantern::iop;

namespace {
constexpr uint32_t kJal = 0x0C000000;
constexpr uint32_t kAddiu = 0x24000000;
constexpr uint32_t kNop = 0;
constexpr uint32_t kBase = 0x00001000;
constexpr uint32_t kSentinel = 0xFFFFFFF0;

void word(IopMemory& m, uint32_t addr, uint32_t w) { m.write<uint32_t>(addr, w); }
}  // namespace

TEST_CASE("IopHle: an import handler runs through the trampoline and returns") {
  IopMemory mem;
  IopHle hle(mem);
  uint32_t seen_a0 = 0, seen_a1 = 0;
  const IrxLibraryId lib{"test", 0x0101};
  const uint32_t slot = hle.add(lib, 4, [&](IopCpu& cpu, IopMemory&) {
    seen_a0 = cpu.gpr[4];
    seen_a1 = cpu.gpr[5];
    cpu.gpr[2] = cpu.gpr[4] + cpu.gpr[5];
  });
  CHECK(hle.resolve(lib, 4) == slot);
  CHECK_FALSE(hle.resolve(lib, 5).has_value());

  // s0 = ra (sentinel) ; addiu a0,zero,7 ; jal slot ; addiu a1,zero,9 ; jr s0 ; nop
  word(mem, kBase + 0, 0x03E08021);  // addu s0,ra,zero
  word(mem, kBase + 4, kAddiu | (4 << 16) | 7);
  word(mem, kBase + 8, kJal | ((slot >> 2) & 0x03FFFFFF));
  word(mem, kBase + 12, kAddiu | (5 << 16) | 9);
  word(mem, kBase + 16, 0x02000008);  // jr s0
  word(mem, kBase + 20, kNop);

  IopCpu cpu;
  cpu.pc = kBase;
  cpu.gpr[31] = kSentinel;
  Interpreter interp;
  interp.set_syscall_hook([&](IopCpu& c, IopMemory&, uint32_t pc) {
    if (!hle.dispatch(c, pc)) throw Trap(pc, "unexpected syscall");
  });
  interp.run(cpu, mem, kSentinel, 100);
  CHECK(seen_a0 == 7);
  CHECK(seen_a1 == 9);
  CHECK(cpu.gpr[2] == 16);
  CHECK(cpu.pc == kSentinel);
  CHECK(hle.size() == 1);
}

TEST_CASE("IopHle: consecutive handlers do not clobber each other's return value") {
  // Two slots back to back: with 8-byte slots, the first handler's `jr ra` delay slot would run
  // the second slot's `syscall` and overwrite v0. Slots must be syscall; jr ra; nop.
  IopMemory mem;
  IopHle hle(mem);
  const uint32_t a = hle.add({"lib", 0x0101}, 1, [](IopCpu& c, IopMemory&) { c.gpr[2] = 0x11; });
  const uint32_t b = hle.add({"lib", 0x0101}, 2, [](IopCpu& c, IopMemory&) { c.gpr[2] = 0x22; });
  CHECK(b == a + IopHle::kSlotSize);

  // s0 = ra; a0 = 1; jal a; nop; s1 = v0; a0 = 2; jal b; nop; jr s0; nop
  word(mem, kBase + 0, 0x03E08021);
  word(mem, kBase + 4, kAddiu | (4 << 16) | 1);
  word(mem, kBase + 8, kJal | ((a >> 2) & 0x03FFFFFF));
  word(mem, kBase + 12, kNop);
  word(mem, kBase + 16, 0x00408821);  // addu s1,v0,zero
  word(mem, kBase + 20, kAddiu | (4 << 16) | 2);
  word(mem, kBase + 24, kJal | ((b >> 2) & 0x03FFFFFF));
  word(mem, kBase + 28, kNop);
  word(mem, kBase + 32, 0x02000008);  // jr s0
  word(mem, kBase + 36, kNop);

  IopCpu cpu;
  cpu.pc = kBase;
  cpu.gpr[31] = kSentinel;
  Interpreter interp;
  interp.set_syscall_hook([&](IopCpu& c, IopMemory&, uint32_t pc) {
    if (!hle.dispatch(c, pc)) throw Trap(pc, "unexpected syscall");
  });
  interp.run(cpu, mem, kSentinel, 100);
  CHECK(cpu.gpr[17] == 0x11);  // s1: the first result survived
  CHECK(cpu.gpr[2] == 0x22);
}

TEST_CASE("IopHle: dispatch only claims trampoline slots") {
  IopMemory mem;
  IopHle hle(mem);
  auto slot = hle.add({"lib", 0x0101}, 1, [](IopCpu& c, IopMemory&) { c.gpr[2] = 0x55; });
  IopCpu cpu;
  CHECK_FALSE(hle.dispatch(cpu, 0x00000100));
  CHECK_FALSE(hle.dispatch(cpu, slot + 4));  // the jr ra slot, not the syscall
  CHECK(hle.dispatch(cpu, slot));
  CHECK(cpu.gpr[2] == 0x55);

  // A handler can be replaced without consuming a second slot.
  const uint32_t again = hle.add({"lib", 0x0101}, 1, [](IopCpu& c, IopMemory&) { c.gpr[2] = 0x66; });
  CHECK(again == slot);
  CHECK(hle.size() == 1);
  CHECK(hle.dispatch(cpu, slot));
  CHECK(cpu.gpr[2] == 0x66);
}

TEST_CASE("IopHle: a handler that writes through an argument pointer") {
  IopMemory mem;
  IopHle hle(mem);
  // CpuSuspendIntr(int *state): write 0 through a0.
  auto slot = hle.add({"intrman", 0x0102}, 17, [](IopCpu& cpu, IopMemory& m) {
    const uint32_t p = cpu.gpr[4];
    if (p) m.write<uint32_t>(p, 0);
    cpu.gpr[2] = 0;
  });
  const uint32_t out = 0x00002000;
  word(mem, kBase + 0, 0x03E08021);                   // addu s0,ra,zero (sentinel)
  word(mem, kBase + 4, 0x3C040000 | (out >> 16));     // lui a0,hi
  word(mem, kBase + 8, 0x24840000 | (out & 0xFFFF));  // addiu a0,a0,lo
  word(mem, kBase + 12, kJal | ((slot >> 2) & 0x03FFFFFF));
  word(mem, kBase + 16, kNop);
  word(mem, kBase + 20, 0x02000008);  // jr s0
  word(mem, kBase + 24, kNop);
  mem.write<uint32_t>(out, 0xDEADBEEF);

  IopCpu cpu;
  cpu.pc = kBase;
  cpu.gpr[31] = kSentinel;
  Interpreter interp;
  interp.set_syscall_hook([&](IopCpu& c, IopMemory&, uint32_t pc) {
    if (!hle.dispatch(c, pc)) throw Trap(pc, "unexpected syscall");
  });
  interp.run(cpu, mem, kSentinel, 100);
  CHECK(mem.read<uint32_t>(out) == 0u);
  CHECK(cpu.gpr[2] == 0u);
}
