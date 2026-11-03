#include <doctest/doctest.h>

#include <memory>

#include "mips_asm.h"
#include "ps2/hardware.h"
#include "ps2/kernel.h"

using namespace lantern;
using namespace lantern::test::mips;

namespace {
constexpr uint32_t kMain = 0x00100000, kChild = 0x00100200, kHandler = 0x00100400, kData = 0x00200000;

struct World {
  std::unique_ptr<ee::Memory> mem = std::make_unique<ee::Memory>();
  std::unique_ptr<ee::Cpu> cpu = std::make_unique<ee::Cpu>();
  ps2::Hardware hw;
  recomp::Runtime rt;  // nothing registered: everything runs on the interpreter fallback
  std::unique_ptr<ps2::Kernel> kernel;
  double now = 0;
  World() {
    mem->set_mmio([this](uint32_t a, void* o, unsigned s) { hw.read(a, o, s); },
                  [this](uint32_t a, const void* i, unsigned s) { hw.write(a, i, s); });
    kernel = std::make_unique<ps2::Kernel>(*cpu, *mem, rt, hw);
    kernel->set_clock([this] { return now += 0.001; });  // 1 ms per clock query
  }
  void code(uint32_t at, std::initializer_list<uint32_t> words) {
    for (uint32_t w : words) {
      mem->write<uint32_t>(at, w);
      at += 4;
    }
  }
};
uint32_t sys(int n) { return addiu(v1, zero, n); }
}  // namespace

TEST_CASE("threads + semaphores: child signals, higher-priority main preempts, KExit returns its code") {
  World w;
  // sema param: count, max=1, init=0
  w.mem->write<uint32_t>(kData + 4, 1);
  // thread param: func, stack, stack_size, gp, prio
  w.mem->write<uint32_t>(kData + 0x104, kChild);
  w.mem->write<uint32_t>(kData + 0x108, 0x00300000);
  w.mem->write<uint32_t>(kData + 0x10C, 0x10000);
  w.mem->write<uint32_t>(kData + 0x114, 10);
  w.code(kMain, {lui(a0, 0x20),     sys(0x40),          syscall,   or_(s0, v0, zero),  // CreateSema
                 lui(a0, 0x20),     ori(a0, a0, 0x100), sys(0x20), syscall,
                 or_(s1, v0, zero),                                                          // CreateThread
                 or_(a0, s1, zero), or_(a1, s0, zero),  sys(0x22), syscall,                  // StartThread(id, sema)
                 or_(a0, s0, zero), sys(0x44),          syscall,                             // WaitSema
                 lui(t0, 0x20),     lw(a0, 0x200, t0),  sys(4),    syscall});                // KExit(value)
  w.code(kChild, {or_(s0, a0, zero), lui(t0, 0x20), addiu(t1, zero, 42), sw(t1, 0x200, t0),  //
                  or_(a0, s0, zero), sys(0x42), syscall,                                     // SignalSema
                  jr(ra), nop});
  CHECK(w.kernel->run(kMain, 0x01FFFF00) == 42);
  CHECK(w.kernel->stats().context_switches >= 2);
  CHECK(w.kernel->stats().threads_created == 2);
}

TEST_CASE("VBlank interrupt handler wakes a sleeping thread via iWakeupThread") {
  World w;
  w.code(kMain, {addiu(a0, zero, 2), lui(a1, kHandler >> 16), ori(a1, a1, kHandler & 0xFFFF), addiu(a2, zero, 0),
                 addiu(a3, zero, 0), sys(0x10), syscall,  // AddIntcHandler(VBLANK_START, handler)
                 addiu(a0, zero, 2), sys(0x14), syscall,  // _EnableIntc(2)
                 sys(0x32), syscall,                      // SleepThread
                 addiu(a0, zero, 7), sys(4), syscall});   // KExit(7)
  w.code(kHandler, {addiu(a0, zero, 1), sys(-0x34), syscall, jr(ra), addiu(v0, zero, 0)});  // iWakeupThread(1)
  CHECK(w.kernel->run(kMain, 0x01FFFF00) == 7);
  CHECK(w.kernel->stats().vblanks >= 1);
  CHECK(w.kernel->stats().interrupts_delivered >= 1);
}

TEST_CASE("deadlock and unknown syscalls trap loudly") {
  World w;
  w.mem->write<uint32_t>(kData + 4, 1);
  w.code(kMain, {lui(a0, 0x20), sys(0x40), syscall, or_(a0, v0, zero), sys(0x44), syscall});  // wait forever
  CHECK_THROWS_WITH_AS(w.kernel->run(kMain, 0x01FFFF00), doctest::Contains("deadlock"), ee::GuestTrap);

  World u;
  u.code(kMain, {sys(0x9), syscall});
  CHECK_THROWS_WITH_AS(u.kernel->run(kMain, 0x01FFFF00), doctest::Contains("unimplemented syscall"), ee::GuestTrap);
}

TEST_CASE("kernel patches are ignored; SIF reports a booted IOP") {
  World w;
  w.code(kMain, {addiu(a0, zero, 0xfc), lui(a1, 0x8007), sys(0x74), syscall,  // SetSyscall(SetAlarm, kernel mem)
                 addiu(a0, zero, 4), sys(0x7a), syscall, or_(s0, v0, zero),   // SifGetReg(SMFLG)
                 or_(a0, s0, zero), sys(4), syscall});
  const int flags = w.kernel->run(kMain, 0x01FFFF00);
  CHECK((flags & 0x70000) == 0x70000);
}
