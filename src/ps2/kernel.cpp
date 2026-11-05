#include "ps2/kernel.h"

#include <pthread.h>
#include <sys/mman.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <thread>

#include "analysis/ps2_syscalls.h"
#include "core/log.h"

#if defined(__SANITIZE_ADDRESS__)
#include <sanitizer/common_interface_defs.h>
#define LANTERN_ASAN 1
#endif

namespace lantern::ps2 {
namespace {
constexpr const char* kSub = "kernel";
constexpr double kVblankPeriod = 1.0 / 59.94;
constexpr double kHsyncHz = 15734.0;
constexpr uint32_t kInterruptStackTop = 0x0007F000;  // inside the (unused) EE kernel area
constexpr size_t kHostStack = 16u << 20;
constexpr double kDeadlockSeconds = 10.0;
Kernel* g_kernel = nullptr;

// Interrupt-context variants whose positive number differs from their base syscall.
int base_syscall(int n) {
  if (n >= 0) return n;
  switch (n) {
    case -0x43: return 0x42;
    case -0x46: return 0x45;
    case -0x48: return 0x47;
    case -0x49: return 0x41;
    case -0x34: return 0x33;
    case -0x36: return 0x35;
    case -0x38: return 0x37;
    case -0x3a: return 0x39;
    case -0x31: return 0x30;
    case -0x2a: return 0x29;
    case -0x2c: return 0x2b;
    case -0x2e: return 0x2d;
    case -0x26: return 0x25;
    case -0x1a: return 0x14;
    case -0x1b: return 0x15;
    case -0x1c: return 0x16;
    case -0x1d: return 0x17;
    case -0x1e: return 0x18;
    case -0x1f: return 0x19;
    case -0xfd: return 0xfc;
    case -0xff: return 0xfe;
    case -0x68: return 0x64;
    case -0x67: return 0x63;
    case -0x6a: return 0x66;
    default: return -n;
  }
}

std::string syscall_label(int n) {
  auto name = analysis::ee_syscall_name(n);
  return std::format("{}(0x{:x})", name.empty() ? "unknown" : name, static_cast<uint32_t>(n));
}
}  // namespace

Kernel::Kernel(ee::Cpu& cpu, ee::Memory& mem, recomp::Runtime& rt, Hardware& hw) : c_(cpu), m_(mem), rt_(rt), hw_(hw) {
  const auto t0 = std::chrono::steady_clock::now();
  clock_ = [t0] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
  c_.hooks = this;
  g_kernel = this;
  rt_.set_tick_hook([this] { safe_point(true); });
}

Kernel::~Kernel() {
  for (auto& [id, t] : threads_)
    if (t->host_stack) munmap(t->host_stack, t->host_stack_size);
  if (g_kernel == this) g_kernel = nullptr;
  c_.hooks = nullptr;
}

void Kernel::set_clock(Clock c) {
  clock_ = std::move(c);
  hw_.set_clock(clock_);
}

int64_t Kernel::arg(int n) const {
  static constexpr unsigned kRegs[8] = {4, 5, 6, 7, 8, 9, 10, 11};
  return static_cast<int32_t>(c_.gpr[kRegs[n]].u32[0]);
}
void Kernel::ret(int64_t v) { c_.r(2) = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(v))); }

void Kernel::save(Regs& r) const {
  std::copy(std::begin(c_.gpr), std::end(c_.gpr), std::begin(r.gpr));
  r.hi = c_.hi;
  r.lo = c_.lo;
  r.sa = c_.sa;
  r.fcr31 = c_.fcr31;
  r.acc = c_.acc;
  r.pc = c_.pc;
  std::copy(std::begin(c_.fpr), std::end(c_.fpr), std::begin(r.fpr));
}
void Kernel::load(const Regs& r) {
  std::copy(std::begin(r.gpr), std::end(r.gpr), std::begin(c_.gpr));
  c_.hi = r.hi;
  c_.lo = r.lo;
  c_.sa = r.sa;
  c_.fcr31 = r.fcr31;
  c_.acc = r.acc;
  std::copy(std::begin(r.fpr), std::end(r.fpr), std::begin(c_.fpr));
}

Kernel::Thread* Kernel::thread(int id) {
  if (id == 0) return current_;
  auto it = threads_.find(id);
  return it == threads_.end() ? nullptr : it->second.get();
}

void Kernel::make_ready(Thread* t) {
  t->state = State::Ready;
  t->wait = Wait::None;
  t->ready_seq = ++seq_;
  if (current_ && t != current_ && t->prio < current_->prio) resched_pending_ = true;
}

void Kernel::block_current(Wait w, int wait_id) {
  if (in_interrupt_) throw ee::GuestTrap(c_.pc, "blocking syscall from an interrupt handler");
  current_->state = State::Waiting;
  current_->wait = w;
  current_->wait_id = wait_id;
  schedule();
}

void Kernel::schedule() {
  for (;;) {
    Thread* best = nullptr;
    for (auto& [id, t] : threads_) {
      if (t->state != State::Ready || t->suspended) continue;
      if (!best || t->prio < best->prio || (t->prio == best->prio && t->ready_seq < best->ready_seq)) best = t.get();
    }
    if (current_ && current_->state == State::Running) {
      if (!best || best->prio >= current_->prio) return;  // keep running
      current_->state = State::Ready;
      current_->ready_seq = 0;  // preempted threads resume first within their priority
    }
    if (best) {
      switch_to(best);
      return;
    }
    idle_until_event();
  }
}

void Kernel::prepare_context(Thread* t) {
  if (!t->host_stack) {
    t->host_stack_size = kHostStack;
    t->host_stack = mmap(nullptr, kHostStack, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_STACK, -1, 0);
    if (t->host_stack == MAP_FAILED) throw ee::GuestTrap(0, "cannot allocate a host stack for a guest thread");
  }
  getcontext(&t->ctx);
  t->ctx.uc_stack.ss_sp = t->host_stack;
  t->ctx.uc_stack.ss_size = t->host_stack_size;
  t->ctx.uc_link = nullptr;
  makecontext(&t->ctx, &Kernel::trampoline, 0);
  t->ctx_ready = true;
}

void Kernel::switch_to(Thread* next) {
  Thread* prev = current_;
  if (prev == next) {
    next->state = State::Running;
    return;
  }
  if (prev) save(prev->regs);
  current_ = next;
  next->state = State::Running;
  load(next->regs);
  if (!next->ctx_ready) prepare_context(next);
  ++stats_.context_switches;
  ucontext_t* from = prev ? &prev->ctx : &boot_ctx_;
#ifdef LANTERN_ASAN
  void* fake = nullptr;
  __sanitizer_start_switch_fiber(&fake, next->host_stack, next->host_stack_size);
  swapcontext(from, &next->ctx);
  __sanitizer_finish_switch_fiber(fake, nullptr, nullptr);
#else
  swapcontext(from, &next->ctx);
#endif
}

void Kernel::trampoline() {
#ifdef LANTERN_ASAN
  __sanitizer_finish_switch_fiber(nullptr, nullptr, nullptr);
#endif
  Kernel& k = *g_kernel;
  bool to_boot = false;
  try {
    k.rt_.call(k.c_, k.m_, k.current_->func);
    k.exit_current_thread(false);
  } catch (const GuestExit& e) {
    k.exit_code_ = e.code;
    to_boot = true;
  } catch (...) {
    k.pending_ = std::current_exception();
    to_boot = true;
  }
  if (to_boot) k.jump_to_boot();
}

void Kernel::jump_to_boot() {
#ifdef LANTERN_ASAN
  __sanitizer_start_switch_fiber(nullptr, boot_stack_, boot_stack_size_);  // this fiber is abandoned
#endif
  setcontext(&boot_ctx_);
  std::abort();  // setcontext does not return
}

void Kernel::exit_current_thread(bool delete_after) {
  Thread* t = current_;
  t->state = State::Dormant;
  t->ctx_ready = false;
  if (delete_after) t->func = 0;
  bool any_alive = false;
  for (auto& [id, th] : threads_) any_alive |= th->state != State::Dormant;
  if (!any_alive) {
    exit_code_ = 0;
    jump_to_boot();
  }
  schedule();
  throw ee::GuestTrap(0, "dormant thread resumed");  // unreachable: dormant threads are never scheduled
}

void Kernel::idle_until_event() {
  if (in_interrupt_) throw ee::GuestTrap(c_.pc, "all threads blocked inside an interrupt handler");
  double start = now();
  int idle_rounds = 0;
  for (;;) {
    bool anything_can_happen =
        !intc_handlers_.empty() || !alarms_.empty() || !dmac_handlers_.empty() || vsync_flag_addr_ != 0;
    if (!anything_can_happen || (!turbo_ && now() - start > kDeadlockSeconds) || ++idle_rounds > 2000000) {
      throw ee::GuestTrap(stats_.last_syscall_pc,
                          std::format("deadlock: every guest thread is blocked ({}s idle)\n{}",
                                      anything_can_happen ? kDeadlockSeconds : 0.0, describe_threads()));
    }
    double due = next_vblank_;
    for (auto& [id, a] : alarms_) due = std::min(due, a.due);
    const double cur = now();
    if (due > cur) {
      if (turbo_)
        virtual_time_ = due;  // jump the virtual clock to the next event
      else
        std::this_thread::sleep_for(std::chrono::duration<double>(std::min(due - cur, 0.005)));
    }
    deliver_due(now());
    for (auto& [id, t] : threads_)
      if (t->state == State::Ready && !t->suspended) return;
  }
}

void Kernel::call_handler(uint32_t func, uint32_t a0, uint32_t a1, uint32_t a2) {
  Regs saved;
  save(saved);
  const bool was = in_interrupt_;
  in_interrupt_ = true;
  c_.r(4) = a0;
  c_.r(5) = a1;
  c_.r(6) = a2;
  c_.r(29) = kInterruptStackTop;
  c_.r(31) = kReturnSentinel;
  try {
    rt_.call(c_, m_, func);
  } catch (...) {
    in_interrupt_ = was;
    throw;
  }
  load(saved);
  in_interrupt_ = was;
  ++stats_.interrupts_delivered;
}

void Kernel::fire_vblank() {
  ++stats_.vblanks;
  hw_.on_vblank();
  if (turbo_) virtual_time_ += kVblankPeriod;
  if (vblank_hook_) vblank_hook_(stats_.vblanks);
  if (vsync_flag_addr_) {
    m_.write<uint32_t>(vsync_flag_addr_, 1);
    if (vsync_csr_addr_) m_.write<uint64_t>(vsync_csr_addr_, (stats_.vblanks & 1) ? 1u << 13 : 0);
  }
  for (int cause : {Hardware::kIntcVblankStart, Hardware::kIntcVblankEnd}) {
    hw_.raise_intc(cause);
    auto it = intc_handlers_.find(cause);
    if (it == intc_handlers_.end() || !(hw_.intc_mask() & (1u << cause))) continue;
    const auto handlers = it->second;  // handlers may modify the list
    for (const auto& h : handlers)
      if (h.enabled) call_handler(h.func, static_cast<uint32_t>(cause), h.arg, 0);
  }
}

void Kernel::deliver_due(double now) {
  if (in_interrupt_) return;
  if (!turbo_ && now - next_vblank_ > 0.25) next_vblank_ = now;  // don't replay a long stall frame by frame
  while (now >= next_vblank_) {
    next_vblank_ += kVblankPeriod;
    fire_vblank();
  }
  for (auto it = alarms_.begin(); it != alarms_.end();) {
    if (it->second.due > now) {
      ++it;
      continue;
    }
    const auto [id, a] = *it;
    it = alarms_.erase(it);
    ++stats_.alarms_fired;
    // EE SetAlarm handlers take their user argument in a0; passing the alarm id here prevents
    // handlers from finding their state and re-arming, which stalls timer-driven animations.
    call_handler(a.handler, a.arg, 0, 0);
  }
  // GS FINISH/SIGNAL interrupts (INTC cause 0).
  if (hw_.take_gs_interrupt() && (hw_.intc_mask() & 1u)) {
    hw_.raise_intc(Hardware::kIntcGs);
    if (auto it = intc_handlers_.find(Hardware::kIntcGs); it != intc_handlers_.end()) {
      const auto handlers = it->second;
      for (const auto& h : handlers)
        if (h.enabled) call_handler(h.func, 0, h.arg, 0);
    }
  }
  // IOP -> EE packets: one per SIF0 (DMA channel 5) completion interrupt.
  while (sif_ && sif_->deliver_one()) {
    if (!(dmac_enabled_ & (1u << 5))) continue;
    auto it = dmac_handlers_.find(5);
    if (it == dmac_handlers_.end()) continue;
    const auto handlers = it->second;
    for (const auto& h : handlers)
      if (h.enabled) call_handler(h.func, 5, h.arg, 0);
  }
  if (uint32_t done = hw_.take_completed_dma()) {
    for (int ch = 0; ch < Hardware::kDmaChannels; ++ch) {
      if (!(done & (1u << ch)) || !(dmac_enabled_ & (1u << ch))) continue;
      auto it = dmac_handlers_.find(ch);
      if (it == dmac_handlers_.end()) continue;
      const auto handlers = it->second;
      for (const auto& h : handlers)
        if (h.enabled) call_handler(h.func, static_cast<uint32_t>(ch), h.arg, 0);
    }
  }
}

void Kernel::safe_point(bool allow_switch) {
  if (in_interrupt_ || !current_) return;
  ++pc_histogram_[c_.pc];
  deliver_due(now());
  if (allow_switch && resched_pending_) {
    resched_pending_ = false;
    schedule();
  }
}

int Kernel::run(uint32_t entry, uint32_t initial_sp) {
  {
    pthread_attr_t attr;
    pthread_getattr_np(pthread_self(), &attr);
    pthread_attr_getstack(&attr, &boot_stack_, &boot_stack_size_);
    pthread_attr_destroy(&attr);
  }
  auto main = std::make_unique<Thread>();
  main->id = next_thread_id_++;
  main->func = entry;
  main->prio = main->init_prio = 0;
  main->regs.gpr[29].u64[0] = initial_sp;
  main->regs.gpr[31].u64[0] = kReturnSentinel;
  Thread* t = main.get();
  threads_[t->id] = std::move(main);
  ++stats_.threads_created;
  next_vblank_ = now() + kVblankPeriod;
  make_ready(t);
  current_ = nullptr;
  Thread* first = t;
  switch_to(first);  // returns here (boot context) when the program exits or traps
  current_ = nullptr;
  if (pending_) {
    auto p = pending_;
    pending_ = nullptr;
    std::rethrow_exception(p);
  }
  return exit_code_.value_or(0);
}

void Kernel::breakpoint(ee::Cpu&, ee::Memory&, uint32_t pc, uint32_t code) {
  throw ee::GuestTrap(pc, std::format("BREAK 0x{:x} executed (guest assertion or debugger trap)", code));
}

void Kernel::syscall(ee::Cpu& c, ee::Memory&, uint32_t pc) {
  const int num = static_cast<int32_t>(c.gpr[3].u32[0]);
  stats_.last_syscall = num;
  stats_.last_syscall_pc = pc;
  c.pc = pc;
  ++stats_.syscalls[num];
  if (trace_)
    log::debug(kSub, "{} a0=0x{:x} a1=0x{:x} a2=0x{:x} @0x{:08x} ra=0x{:08x} thread {}", syscall_label(num), uarg(0),
               uarg(1), uarg(2), pc, c.gpr[31].u32[0], current_ ? current_->id : 0);
  if (auto it = custom_syscalls_.find(num); it != custom_syscalls_.end()) {
    // Handler installed by the game with SetSyscall: run it like a call, preserving the caller.
    Regs saved;
    save(saved);
    c.r(29) = (c.r(29) - 0x400) & ~0xFull;
    c.r(31) = kReturnSentinel;
    rt_.call(c_, m_, it->second);
    const auto v0 = c.gpr[2], v1 = c.gpr[3];
    load(saved);
    c.gpr[2] = v0;
    c.gpr[3] = v1;
  } else {
    dispatch(num);
  }
  safe_point(true);
}

void Kernel::dispatch(int num) {
  const int n = base_syscall(num);
  auto& m = m_;
  switch (n) {
    // ---- program / boot ----
    case 0x04: throw GuestExit{static_cast<int>(arg(0))};  // KExit
    case 0x3c: {                                           // SetupThread(gp, stack, stack_size, args, root)
      const uint32_t gp = uarg(0), stack = uarg(1), size = uarg(2), args = uarg(3);
      const uint32_t top = stack == 0xFFFFFFFFu ? 0x02000000u : stack + size;
      current_->gp = gp;
      current_->stack = top - size;
      current_->stack_size = size;
      c_.r(28) = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(gp)));
      if (args) {  // struct { int argc; char* argv[16]; char payload[256]; }
        const char* argv0 = "cdrom0:\\SLUS_202.28;1";
        m.write<uint32_t>(args, 1);
        m.write<uint32_t>(args + 4, args + 4 + 16 * 4);
        for (size_t i = 0; argv0[i] || i == 0; ++i) m.write<uint8_t>(args + 68 + static_cast<uint32_t>(i), argv0[i]);
        m.write<uint8_t>(args + 68 + 21, 0);
      }
      // Returned stack pointer: top of the stack minus a small reserved frame (approximation of
      // the BIOS value; only alignment matters to crt0).
      return ret(static_cast<int64_t>((top - 0x40) & ~0xFu));
    }
    case 0x3d: {  // SetupHeap(start, size)
      const uint32_t start = uarg(0);
      const int64_t size = arg(1);
      heap_end_ = size < 0 ? current_->stack : start + static_cast<uint32_t>(size);
      return ret(heap_end_);
    }
    case 0x3e: return ret(heap_end_);  // EndOfHeap
    case 0x7f: return ret(32 << 20);   // GetMemorySize
    case 0x7e: return ret(0);          // MachineType
    case 0x7d: return ret(0);          // PSMode
    case 0x5b: return ret(0);          // GetEntryAddress
    case 0x74: {                       // SetSyscall(num, handler)
      const int which = static_cast<int>(arg(0));
      const uint32_t addr = uarg(1);
      // SCE libkernel installs "kernel patches" that run in kernel memory and edit BIOS-internal
      // structures. LANTERN's HLE kernel has no such structures, so it keeps its own
      // implementation for those numbers and honours only user-space handlers.
      if (addr == 0 || (addr >= 0x80000000u && addr < 0x80080000u)) {
        if (ignored_patches_++ < 4)
          log::info(kSub, "ignoring kernel patch for syscall {} at 0x{:08x} (HLE implementation kept)",
                    syscall_label(which), addr);
        return ret(0);
      }
      custom_syscalls_[which] = addr;
      log::info(kSub, "game installed a user-space handler for syscall {} at 0x{:08x}", syscall_label(which), addr);
      return ret(0);
    }
    case 0x7c: return ret(-1);  // Deci2Call: no debugger attached
    case 0x5a: {                // Copy(dst, src, size)
      for (uint32_t i = 0; i < uarg(2); ++i) m.write<uint8_t>(uarg(0) + i, m.read<uint8_t>(uarg(1) + i));
      return ret(0);
    }
    case 0x64:  // FlushCache(mode)
      if (arg(0) == 2 && code_modified_) code_modified_();
      return ret(0);
    case 0x63:
      return ret(0);  // GetCop0

    // ---- GS / OSD ----
    case 0x02:  // SetGsCrt(interlace, mode, field)
      log::info(kSub, "SetGsCrt interlace={} mode={} field={}", arg(0), arg(1), arg(2));
      return ret(0);
    case 0x70: return ret(gs_imr_);  // GsGetIMR
    case 0x71: {                     // GsPutIMR
      const uint32_t old = gs_imr_;
      gs_imr_ = uarg(0);
      hw_.set_gs_imr(gs_imr_);
      return ret(old);
    }
    case 0x73:  // SetVSyncFlag(flag*, csr*)
      vsync_flag_addr_ = uarg(0);
      vsync_csr_addr_ = uarg(1);
      return ret(0);
    case 0x4b:  // GetOsdConfigParam(ptr): English, 4:3, RGB
      m.write<uint32_t>(uarg(0), 1u << 16);
      return ret(0);
    case 0x6f:  // GetOsdConfigParam2(ptr, size, offset)
      for (uint32_t i = 0; i < uarg(1); ++i) m.write<uint8_t>(uarg(0) + i, 0);
      return ret(0);
    case 0x4a:
    case 0x6e:
      return ret(0);  // SetOsdConfigParam(2)

    // ---- threads ----
    case 0x20: {  // CreateThread(param*)
      const uint32_t p = uarg(0);
      auto t = std::make_unique<Thread>();
      t->id = next_thread_id_++;
      t->func = m.read<uint32_t>(p + 4);
      t->stack = m.read<uint32_t>(p + 8);
      t->stack_size = m.read<uint32_t>(p + 12);
      t->gp = m.read<uint32_t>(p + 16);
      t->prio = t->init_prio = static_cast<int>(m.read<uint32_t>(p + 20));
      t->attr = m.read<uint32_t>(p + 28);
      t->option = m.read<uint32_t>(p + 32);
      const int id = t->id;
      threads_[id] = std::move(t);
      ++stats_.threads_created;
      return ret(id);
    }
    case 0x22: {  // StartThread(id, arg)
      Thread* t = thread(static_cast<int>(arg(0)));
      if (!t || t->state != State::Dormant) return ret(-1);
      t->regs = Regs{};
      t->regs.gpr[4].u64[0] = uarg(1);
      t->regs.gpr[28].u64[0] = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(t->gp)));
      t->regs.gpr[29].u64[0] = (t->stack + t->stack_size - 0x40) & ~0xFu;
      t->regs.gpr[31].u64[0] = kReturnSentinel;
      t->prio = t->init_prio;
      t->wakeups = 0;
      t->ctx_ready = false;
      make_ready(t);
      return ret(t->id);
    }
    case 0x23: log::debug(kSub, "ExitThread by thread {}", current_->id); exit_current_thread(false);  // ExitThread
    case 0x24:
      log::debug(kSub, "ExitDeleteThread by thread {}", current_->id);
      exit_current_thread(true);  // ExitDeleteThread
    case 0x25: {                  // TerminateThread(id)
      Thread* t = thread(static_cast<int>(arg(0)));
      log::debug(kSub, "TerminateThread({}) by thread {}", arg(0), current_->id);
      if (!t || (t == current_ && t->state == State::Running)) return ret(-1);
      t->state = State::Dormant;
      t->ctx_ready = false;
      for (auto& [sid, s] : semas_) std::erase(s.waiters, t->id);
      return ret(t->id);
    }
    case 0x21: {  // DeleteThread(id)
      Thread* t = thread(static_cast<int>(arg(0)));
      log::debug(kSub, "DeleteThread({}) by thread {}", arg(0), current_->id);
      if (!t || t->state != State::Dormant || t == current_) return ret(-1);
      if (t->host_stack) munmap(t->host_stack, t->host_stack_size);
      threads_.erase(t->id);
      return ret(arg(0));
    }
    case 0x2f: return ret(current_->id);  // GetThreadId
    case 0x29: {                          // ChangeThreadPriority(id, prio)
      Thread* t = thread(static_cast<int>(arg(0)));
      if (!t) return ret(-1);
      const int old = t->prio;
      t->prio = static_cast<int>(arg(1));
      if (t == current_ || t->prio < current_->prio) resched_pending_ = true;
      return ret(old);
    }
    case 0x2b: {  // RotateThreadReadyQueue(prio)
      const int prio = static_cast<int>(arg(0));
      if (!in_interrupt_ && current_->prio == prio) {
        current_->state = State::Ready;
        current_->ready_seq = ++seq_;
        schedule();
      }
      return ret(0);
    }
    case 0x2d: {  // ReleaseWaitThread(id)
      Thread* t = thread(static_cast<int>(arg(0)));
      if (!t || t->state != State::Waiting) return ret(-1);
      for (auto& [sid, s] : semas_) std::erase(s.waiters, t->id);
      t->regs.gpr[2].u64[0] = static_cast<uint64_t>(-418);  // KE_RELEASE_WAIT
      make_ready(t);
      return ret(t->id);
    }
    case 0x30: {  // ReferThreadStatus(id, status*)
      Thread* t = thread(static_cast<int>(arg(0)));
      if (!t) return ret(-1);
      const uint32_t p = uarg(1);
      static constexpr uint32_t kStatus[5] = {2, 1, 4, 8, 16};
      const uint32_t st = kStatus[static_cast<int>(t->state)];
      if (p) {
        const uint32_t fields[12] = {st,
                                     t->func,
                                     t->stack,
                                     t->stack_size,
                                     t->gp,
                                     static_cast<uint32_t>(t->init_prio),
                                     static_cast<uint32_t>(t->prio),
                                     t->attr,
                                     t->option,
                                     t->wait == Wait::Sleep  ? 1u
                                     : t->wait == Wait::Sema ? 2u
                                                             : 0u,
                                     static_cast<uint32_t>(t->wait_id),
                                     static_cast<uint32_t>(t->wakeups)};
        for (int i = 0; i < 12; ++i) m.write<uint32_t>(p + 4 * static_cast<uint32_t>(i), fields[i]);
      }
      return ret(st);
    }
    case 0x32:  // SleepThread
      if (current_->wakeups > 0) {
        --current_->wakeups;
        return ret(current_->id);
      }
      block_current(Wait::Sleep, 0);
      return ret(current_->id);
    case 0x33: {  // WakeupThread(id)
      Thread* t = thread(static_cast<int>(arg(0)));
      if (!t || (t == current_ && t->state == State::Running)) return ret(-1);
      if (t->state == State::Waiting && t->wait == Wait::Sleep)
        make_ready(t);
      else
        ++t->wakeups;
      return ret(t->id);
    }
    case 0x35: {  // CancelWakeupThread(id)
      Thread* t = thread(static_cast<int>(arg(0)));
      if (!t) return ret(-1);
      const int old = t->wakeups;
      t->wakeups = 0;
      return ret(old);
    }
    case 0x37: {  // SuspendThread(id)
      Thread* t = thread(static_cast<int>(arg(0)));
      if (!t || (t == current_ && t->state == State::Running)) return ret(-1);
      t->suspended = true;
      return ret(t->id);
    }
    case 0x39: {  // ResumeThread(id)
      Thread* t = thread(static_cast<int>(arg(0)));
      if (!t) return ret(-1);
      t->suspended = false;
      if (t->state == State::Ready && current_ && t->prio < current_->prio) resched_pending_ = true;
      return ret(t->id);
    }
    case 0x27:
    case 0x28:
      return ret(0);  // Disable/EnableDispatchThread

    // ---- semaphores ----
    case 0x40: {  // CreateSema(param*)
      const uint32_t p = uarg(0);
      Sema s;
      s.max = static_cast<int>(m.read<uint32_t>(p + 4));
      s.init = s.count = static_cast<int>(m.read<uint32_t>(p + 8));
      s.attr = m.read<uint32_t>(p + 16);
      s.option = m.read<uint32_t>(p + 20);
      const int id = next_sema_id_++;
      semas_[id] = s;
      return ret(id);
    }
    case 0x41: {  // DeleteSema
      auto it = semas_.find(static_cast<int>(arg(0)));
      if (it == semas_.end()) return ret(-1);
      for (int tid : it->second.waiters)
        if (Thread* t = thread(tid)) make_ready(t);
      semas_.erase(it);
      return ret(arg(0));
    }
    case 0x42: {  // SignalSema / iSignalSema
      auto it = semas_.find(static_cast<int>(arg(0)));
      if (it == semas_.end()) return ret(-1);
      Sema& s = it->second;
      if (!s.waiters.empty()) {
        Thread* t = thread(s.waiters.front());
        s.waiters.erase(s.waiters.begin());
        if (t) {
          t->regs.gpr[2].u64[0] = static_cast<uint64_t>(arg(0));
          make_ready(t);
        }
      } else {
        ++s.count;
      }
      return ret(arg(0));
    }
    case 0x44: {  // WaitSema
      const int id = static_cast<int>(arg(0));
      auto it = semas_.find(id);
      if (it == semas_.end()) return ret(-1);
      if (it->second.count > 0) {
        --it->second.count;
        return ret(id);
      }
      it->second.waiters.push_back(current_->id);
      ret(id);
      block_current(Wait::Sema, id);
      return;  // v0 set by the signaller (or ReleaseWaitThread)
    }
    case 0x45: {  // PollSema
      auto it = semas_.find(static_cast<int>(arg(0)));
      if (it == semas_.end() || it->second.count <= 0) return ret(-1);
      --it->second.count;
      return ret(arg(0));
    }
    case 0x47: {  // ReferSemaStatus(id, info*)
      auto it = semas_.find(static_cast<int>(arg(0)));
      if (it == semas_.end()) return ret(-1);
      const Sema& s = it->second;
      const uint32_t p = uarg(1);
      m.write<uint32_t>(p, static_cast<uint32_t>(s.count));
      m.write<uint32_t>(p + 4, static_cast<uint32_t>(s.max));
      m.write<uint32_t>(p + 8, static_cast<uint32_t>(s.init));
      m.write<uint32_t>(p + 12, static_cast<uint32_t>(s.waiters.size()));
      m.write<uint32_t>(p + 16, s.attr);
      m.write<uint32_t>(p + 20, s.option);
      return ret(arg(0));
    }

    // ---- interrupts / DMA handlers ----
    case 0x10:    // AddIntcHandler(cause, handler, next, arg)
    case 0x12: {  // AddDmacHandler(channel, handler, next, arg)
      auto& list = (n == 0x10 ? intc_handlers_ : dmac_handlers_)[static_cast<int>(arg(0))];
      const int id = next_handler_id_++;
      Handler h{id, uarg(1), uarg(3), true};
      if (arg(2) < 0)
        list.insert(list.begin(), h);
      else
        list.push_back(h);
      return ret(id);
    }
    case 0x11:
    case 0x13: {  // RemoveIntcHandler / RemoveDmacHandler(cause, id)
      auto& map = n == 0x11 ? intc_handlers_ : dmac_handlers_;
      auto it = map.find(static_cast<int>(arg(0)));
      if (it != map.end()) std::erase_if(it->second, [&](const Handler& h) { return h.id == arg(1); });
      return ret(0);
    }
    case 0x5c:
    case 0x5d:
    case 0x5e:
    case 0x5f: {  // Enable/DisableIntcHandler, Enable/DisableDmacHandler(cause, id)
      auto& map = (n == 0x5c || n == 0x5d) ? intc_handlers_ : dmac_handlers_;
      for (auto& h : map[static_cast<int>(arg(0))])
        if (h.id == arg(1)) h.enabled = (n == 0x5c || n == 0x5e);
      return ret(0);
    }
    case 0x14:
    case 0x15: {  // _EnableIntc / _DisableIntc(cause)
      const uint32_t bit = 1u << (arg(0) & 31);
      const bool was = hw_.intc_mask() & bit;
      const bool want = n == 0x14;
      if (was != want) {
        uint32_t toggle = bit;
        hw_.write(0x1000F010, &toggle, 4);
      }
      return ret(was != want ? 1 : 0);
    }
    case 0x16:
    case 0x17: {  // _EnableDmac / _DisableDmac(channel)
      const uint32_t bit = 1u << (arg(0) & 31);
      const bool was = dmac_enabled_ & bit;
      if (n == 0x16)
        dmac_enabled_ |= bit;
      else
        dmac_enabled_ &= ~bit;
      return ret(was != (n == 0x16) ? 1 : 0);
    }

    // ---- alarms (time in HSYNC units) ----
    case 0x18:
    case 0xfc: {  // SetAlarm(time, handler, arg)
      const int id = next_alarm_id_++;
      alarms_[id] = Alarm{now() + static_cast<double>(uarg(0) & 0xFFFF) / kHsyncHz, uarg(1), uarg(2),
                          static_cast<uint16_t>(uarg(0))};
      return ret(id);
    }
    case 0x19:
    case 0xfe:
      return ret(alarms_.erase(static_cast<int>(arg(0))) ? arg(0) : -1);  // ReleaseAlarm

    // ---- SIF ----
    case 0x77: {  // SifSetDma(dma_t* list, count)
      const uint32_t list = uarg(0);
      for (uint32_t i = 0; i < uarg(1); ++i) {
        const uint32_t e = list + i * 16;
        const uint32_t src = m.read<uint32_t>(e), dst = m.read<uint32_t>(e + 4), size = m.read<uint32_t>(e + 8),
                       attr = m.read<uint32_t>(e + 12);
        if (sif_)
          sif_->ee_to_iop(src, dst, size, attr);
        else
          log::warn(kSub, "PLACEHOLDER: SIF DMA EE->IOP {} bytes discarded (no IOP attached)", size);
      }
      return ret(1);
    }
    case 0x76: return ret(-1);  // SifDmaStat: every transfer completes immediately
    case 0x78:                  // SifSetDChain
    case 0x6b: return ret(0);   // SifStopDma
    case 0x79: {                // SifSetReg(reg, value)
      const uint32_t reg = uarg(0);
      if (reg & 0x80000000u)
        sif_sysregs_[reg] = uarg(1);
      else
        hw_.set_sif_reg(reg, uarg(1));
      return ret(0);
    }
    case 0x7a: {  // SifGetReg(reg)
      const uint32_t reg = uarg(0);
      if (reg & 0x80000000u) return ret(sif_sysregs_[reg]);
      return ret(hw_.sif_reg(reg));
    }

    default:
      ++stats_.unknown_syscalls[num];
      throw ee::GuestTrap(stats_.last_syscall_pc,
                          std::format("unimplemented syscall {} (v1={})", syscall_label(n), num));
  }
}

std::vector<Kernel::ThreadContext> Kernel::thread_contexts() const {
  std::vector<ThreadContext> out;
  for (const auto& [id, t] : threads_) {
    const bool running = t.get() == current_;
    const auto& g = running ? c_.gpr : t->regs.gpr;
    out.push_back({id, running, g[29].u32[0], g[31].u32[0], running ? c_.pc : t->regs.pc});
  }
  return out;
}

std::string Kernel::describe_threads() const {
  static constexpr const char* kState[5] = {"ready", "running", "waiting", "suspended", "dormant"};
  static constexpr const char* kWait[3] = {"", " on sleep", " on sema"};
  std::string out;
  for (const auto& [id, t] : threads_)
    out += std::format("  thread {} prio {} {}{}{} entry 0x{:08x}\n", id, t->prio, kState[static_cast<int>(t->state)],
                       kWait[static_cast<int>(t->wait)], t->wait == Wait::Sema ? std::format(" {}", t->wait_id) : "",
                       t->func);
  return out;
}

}  // namespace lantern::ps2
