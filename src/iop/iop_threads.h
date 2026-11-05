#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "iop/iop_cpu.h"
#include "iop/iop_interp.h"

namespace lantern::iop {

// Thrown by an HLE handler after it has put the current thread into a non-ready state. The
// scheduler catches it and picks the next thread.
struct ThreadSwitch {};

// Thrown by ExitThread/ExitDeleteThread.
struct ThreadExit {};

enum class ThreadWaitKind : uint8_t { Sleep, Delay, Sema, Vblank };

struct ThreadWait {
  ThreadWaitKind kind = ThreadWaitKind::Sleep;
  uint64_t until_us = 0;  // Delay: absolute virtual time
  uint32_t object = 0;    // Sema: semaphore id
};

enum class ThreadState : uint8_t { Dormant, Ready, Running, Delay, Sleep, Sema, Vblank, Dead };

// One IOP thread. The CPU state is complete except for the memory and interpreter, which are
// shared; switching is therefore just picking another entry from this table.
struct IopThread {
  uint32_t id = 0;
  IopCpu cpu;
  ThreadState state = ThreadState::Dormant;
  int priority = 64;  // IOP convention: lower value = higher priority
  uint32_t entry = 0, gp = 0, stack_base = 0, stack_size = 0;
  uint64_t wake_us = 0;
  uint32_t wait_object = 0;
  uint64_t steps = 0;
  uint32_t trap_pc = 0;
  std::string trap;
  bool callback = false;
  uint32_t callback_token = 0;
  std::function<void(uint32_t, uint32_t)> callback_done;

  uint32_t stack_top() const { return (stack_base + stack_size) & ~15u; }
};

struct SchedulerStats {
  uint64_t steps = 0;
  uint64_t cycles = 0;
  uint32_t switches = 0;
  bool deadlock = false;
  bool budget_exhausted = false;
  bool trapped = false;
  uint32_t trap_thread = 0;
  uint32_t trap_pc = 0;
  std::string trap;
};

// Cooperative, priority-based IOP scheduler. HLE handlers block by throwing ThreadSwitch; the
// scheduler owns virtual time (36.864 MHz) and wakes delays and vblank waiters itself. Interrupt
// delivery and hardware-timer callbacks are not part of this layer yet.
class IopScheduler {
 public:
  static constexpr uint32_t kReturnAddress = 0xFFFFFFF0;  // a thread entry returning here is done
  static constexpr uint32_t kVblankPeriodUs = 16683;      // 59.94 Hz
  static constexpr uint64_t kQuantum = 4096;              // instructions before a ready thread rotates

  IopScheduler(IopMemory& mem, Interpreter& interp);

  // Create a dormant thread. `entry` is absolute; the scheduler sets gp, sp, a0 and ra.
  uint32_t create_thread(uint32_t entry, uint32_t arg, int priority, uint32_t stack_base, uint32_t stack_size,
                         uint32_t gp);
  bool start_thread(uint32_t id, uint32_t arg);
  bool delete_thread(uint32_t id);
  bool remove_thread(uint32_t id);  // erase a dead thread (RPC call threads)

  int current_index() const { return current_; }
  int current_id() const;
  int find(uint32_t id) const;
  IopThread* current();
  IopThread* thread(uint32_t id);
  const std::deque<IopThread>& threads() const { return threads_; }

  uint64_t now_us() const { return cycles_ * 1000 / 36864; }
  uint64_t cycles() const { return cycles_; }
  uint64_t next_vblank_us() const { return next_vblank_us_; }

  // Called from HLE handlers. Each advances the thread past the trampoline syscall before the
  // scheduler switches away, so the thread resumes at `jr ra`.
  [[noreturn]] void block_current(ThreadWait wait);
  [[noreturn]] void yield_current();
  [[noreturn]] void exit_current();

  bool wake_thread(uint32_t id);                        // Sleep/Delay -> Ready
  bool wake_one(ThreadWaitKind kind, uint32_t object);  // highest-priority waiter
  void set_priority(uint32_t id, int priority);
  uint32_t sleeping_on(ThreadWaitKind kind, uint32_t object) const;

  using StepObserver = std::function<void(const IopThread&, uint32_t pc, uint32_t word)>;
  void set_observer(StepObserver observer) { observer_ = std::move(observer); }

  SchedulerStats run(uint64_t max_steps);
  // Run until `id` is Dead. Other threads run normally, so blocking RPC servers can be woken.
  // Returns false on trap, deadlock or budget exhaustion.
  bool run_until_done(uint32_t id, uint64_t max_steps, SchedulerStats* stats);

  // Guest timer/alarm callbacks. They run one at a time as high-priority threads; the callback's
  // v0 is reported to `done` (timrman uses it as the next compare value).
  void schedule_callback(uint32_t pc, uint32_t arg, uint32_t gp, uint32_t stack_base, uint32_t stack_size,
                         uint64_t due_us, uint32_t token, std::function<void(uint32_t, uint32_t)> done = {});
  bool cancel_callback(uint32_t token);

 private:
  struct PendingCallback {
    uint32_t pc = 0, arg = 0, gp = 0, stack_base = 0, stack_size = 0, token = 0;
    uint64_t due_us = 0;
    std::function<void(uint32_t, uint32_t)> done;
  };
  struct CompletedCallback {
    uint32_t token = 0, result = 0;
    std::function<void(uint32_t, uint32_t)> done;
  };

  void update_wakeups();
  void activate_callbacks();
  void advance_to(uint64_t us);
  int pick_next();
  bool timed_wake_pending(uint64_t* next_us) const;

  IopMemory& mem_;
  Interpreter& interp_;
  std::deque<IopThread> threads_;  // stable references: handlers may create threads mid-step
  uint32_t next_id_ = 1;
  int current_ = -1;
  uint64_t cycles_ = 0;
  uint64_t next_vblank_us_ = kVblankPeriodUs;
  uint32_t last_pick_id_ = 0;
  bool reschedule_ = false;
  std::vector<PendingCallback> callbacks_;
  std::vector<CompletedCallback> completed_;
  uint32_t active_callback_ = 0;
  StepObserver observer_;
};

}  // namespace lantern::iop
