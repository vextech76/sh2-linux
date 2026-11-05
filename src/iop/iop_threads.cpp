#include "iop/iop_threads.h"

#include <algorithm>
#include <format>
#include <limits>

namespace lantern::iop {

IopScheduler::IopScheduler(IopMemory& mem, Interpreter& interp) : mem_(mem), interp_(interp) {}

uint32_t IopScheduler::create_thread(uint32_t entry, uint32_t arg, int priority, uint32_t stack_base,
                                     uint32_t stack_size, uint32_t gp) {
  IopThread t;
  t.id = next_id_++;
  t.entry = entry;
  t.gp = gp;
  t.priority = priority;
  t.stack_base = stack_base;
  t.stack_size = stack_size;
  t.state = ThreadState::Dormant;
  t.cpu.pc = entry;
  t.cpu.gpr[4] = arg;
  t.cpu.gpr[28] = gp;
  t.cpu.gpr[29] = t.stack_top();
  t.cpu.gpr[31] = kReturnAddress;
  threads_.push_back(t);
  return t.id;
}

bool IopScheduler::start_thread(uint32_t id, uint32_t arg) {
  IopThread* t = thread(id);
  if (!t || t->state == ThreadState::Dead) return false;
  t->cpu.gpr[4] = arg;
  t->state = ThreadState::Ready;
  return true;
}

bool IopScheduler::delete_thread(uint32_t id) {
  const int idx = find(id);
  if (idx < 0) return false;
  threads_[static_cast<size_t>(idx)].state = ThreadState::Dead;
  if (idx == current_) throw ThreadSwitch{};
  return true;
}

bool IopScheduler::remove_thread(uint32_t id) {
  const int idx = find(id);
  if (idx < 0 || idx == current_) return false;
  threads_.erase(threads_.begin() + idx);
  return true;
}

int IopScheduler::current_id() const {
  if (current_ < 0 || static_cast<size_t>(current_) >= threads_.size()) return -1;
  return static_cast<int>(threads_[static_cast<size_t>(current_)].id);
}

int IopScheduler::find(uint32_t id) const {
  for (size_t i = 0; i < threads_.size(); ++i)
    if (threads_[i].id == id) return static_cast<int>(i);
  return -1;
}

IopThread* IopScheduler::current() {
  if (current_ < 0 || static_cast<size_t>(current_) >= threads_.size()) return nullptr;
  return &threads_[static_cast<size_t>(current_)];
}

IopThread* IopScheduler::thread(uint32_t id) {
  const int idx = find(id);
  return idx < 0 ? nullptr : &threads_[static_cast<size_t>(idx)];
}

void IopScheduler::block_current(ThreadWait wait) {
  if (current_ < 0) throw ThreadSwitch{};
  IopThread& t = threads_[static_cast<size_t>(current_)];
  switch (wait.kind) {
    case ThreadWaitKind::Sleep: t.state = ThreadState::Sleep; break;
    case ThreadWaitKind::Delay: t.state = ThreadState::Delay; break;
    case ThreadWaitKind::Sema: t.state = ThreadState::Sema; break;
    case ThreadWaitKind::Vblank: t.state = ThreadState::Vblank; break;
  }
  t.wait_object = wait.object;
  t.wake_us = wait.kind == ThreadWaitKind::Vblank ? next_vblank_us_ : wait.until_us;
  // The handler was entered through a trampoline syscall; resume at the following `jr ra`.
  t.cpu.pc += 4;
  throw ThreadSwitch{};
}

void IopScheduler::yield_current() {
  if (current_ < 0) throw ThreadSwitch{};
  IopThread& t = threads_[static_cast<size_t>(current_)];
  if (t.state == ThreadState::Running) t.state = ThreadState::Ready;
  t.cpu.pc += 4;
  throw ThreadSwitch{};
}

void IopScheduler::exit_current() {
  if (current_ < 0) throw ThreadExit{};
  threads_[static_cast<size_t>(current_)].state = ThreadState::Dead;
  throw ThreadExit{};
}

bool IopScheduler::wake_thread(uint32_t id) {
  IopThread* t = thread(id);
  if (!t) return false;
  if (t->state != ThreadState::Sleep && t->state != ThreadState::Delay) return false;
  t->state = ThreadState::Ready;
  return true;
}

bool IopScheduler::wake_one(ThreadWaitKind kind, uint32_t object) {
  int best = -1;
  for (size_t i = 0; i < threads_.size(); ++i) {
    const IopThread& t = threads_[i];
    const bool matches = (kind == ThreadWaitKind::Sema && t.state == ThreadState::Sema && t.wait_object == object) ||
                         (kind == ThreadWaitKind::Vblank && t.state == ThreadState::Vblank);
    if (!matches) continue;
    if (best < 0 || t.priority < threads_[static_cast<size_t>(best)].priority ||
        (t.priority == threads_[static_cast<size_t>(best)].priority && t.id < threads_[static_cast<size_t>(best)].id))
      best = static_cast<int>(i);
  }
  if (best < 0) return false;
  threads_[static_cast<size_t>(best)].state = ThreadState::Ready;
  return true;
}

void IopScheduler::set_priority(uint32_t id, int priority) {
  IopThread* t = thread(id);
  if (!t) return;
  t->priority = priority;
  if (static_cast<int>(id) == current_id()) reschedule_ = true;
}

uint32_t IopScheduler::sleeping_on(ThreadWaitKind kind, uint32_t object) const {
  uint32_t n = 0;
  for (const auto& t : threads_)
    if ((kind == ThreadWaitKind::Sema && t.state == ThreadState::Sema && t.wait_object == object) ||
        (kind == ThreadWaitKind::Vblank && t.state == ThreadState::Vblank))
      ++n;
  return n;
}

void IopScheduler::schedule_callback(uint32_t pc, uint32_t arg, uint32_t gp, uint32_t stack_base, uint32_t stack_size,
                                     uint64_t due_us, uint32_t token, std::function<void(uint32_t, uint32_t)> done) {
  callbacks_.push_back({pc, arg, gp, stack_base, stack_size, token, due_us, std::move(done)});
}

bool IopScheduler::cancel_callback(uint32_t token) {
  for (auto it = callbacks_.begin(); it != callbacks_.end(); ++it) {
    if (it->token != token) continue;
    callbacks_.erase(it);
    return true;
  }
  return false;
}

void IopScheduler::activate_callbacks() {
  if (active_callback_) return;
  const uint64_t now = now_us();
  for (auto it = callbacks_.begin(); it != callbacks_.end(); ++it) {
    if (it->due_us > now) continue;
    const PendingCallback cb = std::move(*it);
    callbacks_.erase(it);
    const uint32_t id = create_thread(cb.pc, cb.arg, 1, cb.stack_base, cb.stack_size, cb.gp);
    IopThread* t = thread(id);
    if (!t) continue;
    t->callback = true;
    t->callback_token = cb.token;
    t->callback_done = cb.done;
    t->state = ThreadState::Ready;
    active_callback_ = id;
    return;
  }
}

void IopScheduler::advance_to(uint64_t us) {
  const uint64_t target = (us * 36864 + 999) / 1000;  // ceil, so now_us() >= us
  if (target > cycles_) cycles_ = target;
}

void IopScheduler::update_wakeups() {
  const uint64_t now = now_us();
  for (auto& t : threads_)
    if (t.state == ThreadState::Delay && t.wake_us <= now) t.state = ThreadState::Ready;

  if (now >= next_vblank_us_) {
    for (auto& t : threads_)
      if (t.state == ThreadState::Vblank) t.state = ThreadState::Ready;
    do {
      next_vblank_us_ += kVblankPeriodUs;
    } while (next_vblank_us_ <= now);
  }
}

bool IopScheduler::timed_wake_pending(uint64_t* next_us) const {
  bool found = false;
  uint64_t best = std::numeric_limits<uint64_t>::max();
  for (const auto& t : threads_) {
    if (t.state == ThreadState::Delay && t.wake_us < best) {
      best = t.wake_us;
      found = true;
    }
    if (t.state == ThreadState::Vblank && next_vblank_us_ < best) {
      best = next_vblank_us_;
      found = true;
    }
  }
  for (const auto& cb : callbacks_) {
    if (cb.due_us < best) {
      best = cb.due_us;
      found = true;
    }
  }
  if (found) *next_us = best;
  return found;
}

int IopScheduler::pick_next() {
  int best = -1;
  for (size_t i = 0; i < threads_.size(); ++i) {
    const IopThread& t = threads_[i];
    if (t.state != ThreadState::Ready) continue;
    if (best < 0) {
      best = static_cast<int>(i);
      continue;
    }
    const IopThread& b = threads_[static_cast<size_t>(best)];
    if (t.priority < b.priority) {
      best = static_cast<int>(i);
    } else if (t.priority == b.priority) {
      const bool t_after = t.id > last_pick_id_;
      const bool b_after = b.id > last_pick_id_;
      if (t_after != b_after ? t_after : t.id < b.id) best = static_cast<int>(i);
    }
  }
  return best;
}

bool IopScheduler::run_until_done(uint32_t id, uint64_t max_steps, SchedulerStats* stats) {
  SchedulerStats total;
  uint64_t used = 0;
  while (true) {
    IopThread* t = thread(id);
    if (!t) return false;
    if (t->state == ThreadState::Dead) break;
    if (used >= max_steps) {
      total.budget_exhausted = true;
      break;
    }
    const uint64_t chunk = std::min<uint64_t>(kQuantum, max_steps - used);
    SchedulerStats s = run(chunk);
    used += s.steps;
    total.steps += s.steps;
    total.switches += s.switches;
    total.cycles = s.cycles;
    if (s.trapped) {
      total.trapped = true;
      total.trap_thread = s.trap_thread;
      total.trap_pc = s.trap_pc;
      total.trap = s.trap;
      break;
    }
    if (s.deadlock) {
      total.deadlock = true;
      break;
    }
    if (s.steps == 0) {
      total.deadlock = true;
      break;
    }
  }
  if (stats) *stats = total;
  IopThread* done = thread(id);
  return done && done->state == ThreadState::Dead && !total.trapped;
}

SchedulerStats IopScheduler::run(uint64_t max_steps) {
  SchedulerStats stats;
  current_ = -1;
  reschedule_ = false;

  while (true) {
    update_wakeups();
    activate_callbacks();
    if (stats.steps >= max_steps) {
      stats.budget_exhausted = true;
      break;
    }
    const int idx = pick_next();
    if (idx < 0) {
      // All threads dead is completion; a live thread with no wake source is a deadlock.
      bool any_alive = !callbacks_.empty() || active_callback_ != 0;
      for (const auto& t : threads_)
        if (t.state != ThreadState::Dead) any_alive = true;
      if (!any_alive) break;
      uint64_t next = 0;
      if (timed_wake_pending(&next) && next > now_us()) {
        advance_to(next);
        continue;
      }
      stats.deadlock = true;
      break;
    }

    current_ = idx;
    IopThread& t = threads_[static_cast<size_t>(idx)];
    t.state = ThreadState::Running;
    last_pick_id_ = t.id;
    ++stats.switches;
    const uint64_t quantum_end = stats.steps + kQuantum;

    while (t.state == ThreadState::Running) {
      if (stats.steps >= max_steps) {
        stats.budget_exhausted = true;
        break;
      }
      if (t.cpu.pc == kReturnAddress) {
        t.state = ThreadState::Dead;
        if (t.callback) {
          completed_.push_back({t.callback_token, t.cpu.gpr[2], std::move(t.callback_done)});
          if (active_callback_ == t.id) active_callback_ = 0;
        }
        break;
      }
      const uint32_t pc = t.cpu.pc;
      uint32_t word = 0;
      try {
        word = mem_.read<uint32_t>(pc);
      } catch (const Trap&) {
        // Let the interpreter report the same trap with its own message.
      }
      if (observer_) observer_(t, pc, word);

      try {
        interp_.step(t.cpu, mem_);
        ++stats.steps;
        ++t.steps;
        ++cycles_;
      } catch (const ThreadSwitch&) {
        break;
      } catch (const ThreadExit&) {
        t.state = ThreadState::Dead;
        if (t.callback) {
          completed_.push_back({t.callback_token, t.cpu.gpr[2], std::move(t.callback_done)});
          if (active_callback_ == t.id) active_callback_ = 0;
        }
        break;
      } catch (const Trap& e) {
        t.state = ThreadState::Dead;
        t.trap_pc = e.pc;
        t.trap = e.what();
        stats.trapped = true;
        stats.trap_thread = t.id;
        stats.trap_pc = e.pc;
        stats.trap = e.what();
        if (t.callback && active_callback_ == t.id) active_callback_ = 0;
        break;
      }

      if (reschedule_) {
        reschedule_ = false;
        if (t.state == ThreadState::Running) t.state = ThreadState::Ready;
        break;
      }
      if (stats.steps >= quantum_end && t.state == ThreadState::Running) {
        t.state = ThreadState::Ready;
        break;
      }
    }

    for (auto& c : completed_)
      if (c.done) c.done(c.token, c.result);
    completed_.clear();

    if (stats.trapped || stats.budget_exhausted) break;
  }

  stats.cycles = cycles_;
  current_ = -1;
  return stats;
}

}  // namespace lantern::iop
