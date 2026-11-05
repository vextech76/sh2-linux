#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "iop/iop_threads.h"

using namespace lantern::iop;

namespace {
constexpr uint32_t kBase = 0x00001000;
constexpr uint32_t kSyscall = 0x0000000C;
constexpr uint32_t kJrRa = 0x03E00008;
constexpr uint32_t kNop = 0;

void word(IopMemory& m, uint32_t addr, uint32_t w) { m.write<uint32_t>(addr, w); }
void syscall_then_return(IopMemory& m, uint32_t at = kBase) {
  word(m, at, kSyscall);
  word(m, at + 4, kJrRa);
  word(m, at + 8, kNop);
}
}  // namespace

TEST_CASE("IopScheduler: a thread that returns is marked dead") {
  IopMemory mem;
  Interpreter interp;
  IopScheduler sched(mem, interp);
  word(mem, kBase, kJrRa);
  word(mem, kBase + 4, kNop);
  const uint32_t id = sched.create_thread(kBase, 0, 10, 0x00008000, 0x1000, 0);
  CHECK(sched.start_thread(id, 0));
  auto st = sched.run(100);
  CHECK(st.steps == 1);
  CHECK_FALSE(st.deadlock);
  REQUIRE(sched.thread(id));
  CHECK(sched.thread(id)->state == ThreadState::Dead);
  CHECK(sched.thread(id)->cpu.pc == IopScheduler::kReturnAddress);
}

TEST_CASE("IopScheduler: DelayThread blocks, other work runs, virtual time wakes it") {
  IopMemory mem;
  Interpreter interp;
  IopScheduler sched(mem, interp);
  syscall_then_return(mem);
  const uint32_t high = sched.create_thread(kBase, 0, 10, 0x00008000, 0x1000, 0);
  const uint32_t low = sched.create_thread(kBase, 0, 20, 0x00009000, 0x1000, 0);
  std::vector<uint32_t> ran;
  interp.set_syscall_hook([&](IopCpu& cpu, IopMemory&, uint32_t) {
    const int id = sched.current_id();
    ran.push_back(static_cast<uint32_t>(id));
    if (id == static_cast<int>(high))
      sched.block_current({ThreadWaitKind::Delay, sched.now_us() + 1000, 0});
    else
      cpu.gpr[2] = 1;
  });
  CHECK(sched.start_thread(high, 0));
  CHECK(sched.start_thread(low, 0));
  auto st = sched.run(1000);
  CHECK_FALSE(st.trapped);
  CHECK_FALSE(st.deadlock);
  REQUIRE(ran.size() == 2);
  CHECK(ran[0] == high);  // higher priority ran first
  CHECK(ran[1] == low);
  CHECK(sched.now_us() >= 1000);
  CHECK(sched.thread(high)->state == ThreadState::Dead);
  CHECK(sched.thread(low)->state == ThreadState::Dead);
}

TEST_CASE("IopScheduler: WaitSema blocks and a signal wakes the waiter") {
  IopMemory mem;
  Interpreter interp;
  IopScheduler sched(mem, interp);
  syscall_then_return(mem);
  const uint32_t waiter = sched.create_thread(kBase, 0, 10, 0x00008000, 0x1000, 0);
  const uint32_t signaler = sched.create_thread(kBase, 0, 20, 0x00009000, 0x1000, 0);
  bool woken = false;
  interp.set_syscall_hook([&](IopCpu&, IopMemory&, uint32_t) {
    if (sched.current_id() == static_cast<int>(waiter))
      sched.block_current({ThreadWaitKind::Sema, 0, 7});
    else
      woken = sched.wake_one(ThreadWaitKind::Sema, 7);
  });
  CHECK(sched.start_thread(waiter, 0));
  CHECK(sched.start_thread(signaler, 0));
  auto st = sched.run(1000);
  CHECK_FALSE(st.trapped);
  CHECK(woken);
  CHECK(sched.thread(waiter)->state == ThreadState::Dead);
  CHECK(sched.thread(signaler)->state == ThreadState::Dead);
}

TEST_CASE("IopScheduler: vblank wait advances virtual time to the next frame") {
  IopMemory mem;
  Interpreter interp;
  IopScheduler sched(mem, interp);
  syscall_then_return(mem);
  const uint32_t id = sched.create_thread(kBase, 0, 10, 0x00008000, 0x1000, 0);
  interp.set_syscall_hook([&](IopCpu&, IopMemory&, uint32_t) { sched.block_current({ThreadWaitKind::Vblank, 0, 0}); });
  CHECK(sched.start_thread(id, 0));
  auto st = sched.run(1000);
  CHECK_FALSE(st.trapped);
  CHECK_FALSE(st.deadlock);
  CHECK(sched.now_us() >= IopScheduler::kVblankPeriodUs);
  CHECK(sched.thread(id)->state == ThreadState::Dead);
}

TEST_CASE("IopScheduler: priority decides who runs first") {
  IopMemory mem;
  Interpreter interp;
  IopScheduler sched(mem, interp);
  syscall_then_return(mem);
  const uint32_t low = sched.create_thread(kBase, 0, 100, 0x00008000, 0x1000, 0);
  const uint32_t high = sched.create_thread(kBase, 0, 1, 0x00009000, 0x1000, 0);
  std::vector<uint32_t> order;
  interp.set_syscall_hook(
      [&](IopCpu&, IopMemory&, uint32_t) { order.push_back(static_cast<uint32_t>(sched.current_id())); });
  CHECK(sched.start_thread(low, 0));
  CHECK(sched.start_thread(high, 0));
  auto st = sched.run(1000);
  CHECK_FALSE(st.trapped);
  REQUIRE(order.size() == 2);
  CHECK(order[0] == high);
  CHECK(order[1] == low);
}

TEST_CASE("IopScheduler: a trap is reported and stops the run") {
  IopMemory mem;
  Interpreter interp;
  IopScheduler sched(mem, interp);
  word(mem, kBase, 0xFFFFFFFFu);
  const uint32_t id = sched.create_thread(kBase, 0, 10, 0x00008000, 0x1000, 0);
  CHECK(sched.start_thread(id, 0));
  auto st = sched.run(100);
  CHECK(st.trapped);
  CHECK(st.trap_thread == id);
  CHECK(st.trap_pc == kBase);
  CHECK_FALSE(st.trap.empty());
  CHECK(sched.thread(id)->state == ThreadState::Dead);
}

TEST_CASE("IopScheduler: a dormant thread is not run and reports a deadlock") {
  IopMemory mem;
  Interpreter interp;
  IopScheduler sched(mem, interp);
  word(mem, kBase, kJrRa);
  word(mem, kBase + 4, kNop);
  const uint32_t running = sched.create_thread(kBase, 0, 10, 0x00008000, 0x1000, 0);
  const uint32_t dormant = sched.create_thread(kBase, 0, 10, 0x00009000, 0x1000, 0);
  CHECK(sched.start_thread(running, 0));
  auto st = sched.run(100);
  CHECK_FALSE(st.trapped);
  CHECK(st.deadlock);
  CHECK(sched.thread(running)->state == ThreadState::Dead);
  CHECK(sched.thread(dormant)->state == ThreadState::Dormant);
  CHECK(sched.thread(dormant)->steps == 0);
}
