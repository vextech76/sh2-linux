#pragma once

#include <ucontext.h>

#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ee/cpu.h"
#include "ee/memory.h"
#include "ps2/hardware.h"
#include "recomp/runtime.h"

namespace lantern::ps2 {

// Thrown when the guest calls KExit (or the kernel decides the program has ended).
struct GuestExit {
  int code;
};

// Receives EE->IOP SIF DMA transfers (implemented by the IOP HLE).
class SifEndpoint {
 public:
  virtual ~SifEndpoint() = default;
  virtual void ee_to_iop(uint32_t src, uint32_t iop_dest, uint32_t size, uint32_t attr) = 0;
  // Write one pending IOP->EE packet into EE memory (SIF0). Returns false if none is pending.
  virtual bool deliver_one() { return false; }
};

// High-level emulation of the EE kernel: the 60 syscalls SLUS-20228 uses (see docs/ANALYSIS.md
// §4), cooperative-preemptive threads on host coroutines, INTC/DMAC handlers, alarms, VSync.
class Kernel final : public ee::Hooks {
 public:
  using Clock = std::function<double()>;

  Kernel(ee::Cpu& cpu, ee::Memory& mem, recomp::Runtime& rt, Hardware& hw);
  ~Kernel() override;

  void set_clock(Clock c);
  // Turbo: do not pace vertical blanks to wall time (advance a virtual clock one frame each
  // VBlank). Used for automated checks that need to reach a screen quickly; not for playing.
  void set_turbo(bool on) { turbo_ = on; }
  // Guest-visible time (virtual time in turbo mode). Hardware timers must use this clock too.
  double time() const { return now(); }
  void set_sif(SifEndpoint* sif) { sif_ = sif; }
  // Called after FlushCache(instruction cache) so the runtime can detect overlay loads.
  void set_code_modified_hook(std::function<void()> fn) { code_modified_ = std::move(fn); }
  void set_trace(bool on) { trace_ = on; }
  void set_vblank_hook(std::function<void(uint64_t)> fn) { vblank_hook_ = std::move(fn); }

  // Run the program from `entry` as thread 1 until it exits or traps. Rethrows GuestTrap.
  int run(uint32_t entry, uint32_t initial_sp);

  void syscall(ee::Cpu& c, ee::Memory& m, uint32_t pc) override;
  void breakpoint(ee::Cpu& c, ee::Memory& m, uint32_t pc, uint32_t code) override;
  // Safe point (loop back-edges, MMIO polls): deliver due interrupts, maybe preempt.
  void safe_point(bool allow_switch);

  struct Stats {
    std::map<int, uint64_t> syscalls;
    std::map<int, uint64_t> unknown_syscalls;
    uint64_t vblanks = 0, interrupts_delivered = 0, context_switches = 0, alarms_fired = 0;
    uint32_t last_syscall_pc = 0;
    int last_syscall = 0;
    int threads_created = 0;
  };
  const Stats& stats() const { return stats_; }
  std::string describe_threads() const;
  struct ThreadContext {
    int id;
    bool running;
    uint32_t sp, ra;
  };
  std::vector<ThreadContext> thread_contexts() const;

  static constexpr uint32_t kReturnSentinel = 0x00000100;  // guest "return to kernel" address

 private:
  struct Regs {
    ee::Reg128 gpr[32]{};
    ee::Reg128 hi{}, lo{};
    uint32_t sa = 0, fcr31 = 0, acc = 0;
    uint32_t fpr[32]{};
  };
  enum class State { Ready, Running, Waiting, Suspended, Dormant };
  enum class Wait { None, Sleep, Sema };
  struct Thread {
    int id = 0;
    State state = State::Dormant;
    Wait wait = Wait::None;
    int wait_id = 0;
    int prio = 0, init_prio = 0;
    uint32_t func = 0, stack = 0, stack_size = 0, gp = 0, attr = 0, option = 0, arg = 0;
    int wakeups = 0;
    bool suspended = false;
    Regs regs;
    ucontext_t ctx{};
    void* host_stack = nullptr;
    size_t host_stack_size = 0;
    bool ctx_ready = false;
    uint64_t ready_seq = 0;  // FIFO order within a priority
  };
  struct Sema {
    int count = 0, max = 0, init = 0;
    uint32_t attr = 0, option = 0;
    std::vector<int> waiters;
  };
  struct Handler {
    int id;
    uint32_t func, arg;
    bool enabled = true;
  };
  struct Alarm {
    double due;
    uint32_t handler, arg;
    uint16_t time;
  };

  int64_t arg(int n) const;  // a0..a3, t0..t3 as signed 32-bit
  uint32_t uarg(int n) const { return static_cast<uint32_t>(arg(n)); }
  void ret(int64_t v);
  void dispatch(int num);

  Thread* thread(int id);  // id 0 = current
  void make_ready(Thread* t);
  void block_current(Wait w, int wait_id);
  void schedule();  // switch to the best ready thread (may be the current one)
  void switch_to(Thread* next);
  void prepare_context(Thread* t);
  [[noreturn]] void exit_current_thread(bool delete_after);
  void save(Regs& r) const;
  void load(const Regs& r);
  static void trampoline();
  [[noreturn]] void jump_to_boot();

  void deliver_due(double now);
  void call_handler(uint32_t func, uint32_t a0, uint32_t a1, uint32_t a2);
  void fire_vblank();
  void idle_until_event();

  ee::Cpu& c_;
  ee::Memory& m_;
  recomp::Runtime& rt_;
  Hardware& hw_;
  SifEndpoint* sif_ = nullptr;
  Clock clock_;
  std::function<void()> code_modified_;
  std::function<void(uint64_t)> vblank_hook_;
  bool trace_ = false;

  std::map<int, std::unique_ptr<Thread>> threads_;
  Thread* current_ = nullptr;
  int next_thread_id_ = 1;
  uint64_t seq_ = 0;
  std::map<int, Sema> semas_;
  int next_sema_id_ = 1;
  std::map<int, std::vector<Handler>> intc_handlers_, dmac_handlers_;
  int next_handler_id_ = 1;
  uint32_t dmac_enabled_ = 0;
  std::map<int, Alarm> alarms_;
  int next_alarm_id_ = 1;
  std::map<int, uint32_t> custom_syscalls_;
  std::map<uint32_t, uint32_t> sif_sysregs_;
  uint32_t vsync_flag_addr_ = 0, vsync_csr_addr_ = 0;
  uint32_t gs_imr_ = 0xFF00;
  uint32_t heap_end_ = 0;
  int ignored_patches_ = 0;
  double next_vblank_ = 0;
  bool turbo_ = false;
  double virtual_time_ = 0;
  double now() const { return turbo_ ? virtual_time_ : clock_(); }
  bool in_interrupt_ = false;
  bool resched_pending_ = false;

  ucontext_t boot_ctx_{};
  void* boot_stack_ = nullptr;
  size_t boot_stack_size_ = 0;
  std::exception_ptr pending_;
  std::optional<int> exit_code_;
  Stats stats_;
};

}  // namespace lantern::ps2
