#include "iop/iop_kernel.h"

#include <cstdio>
#include <format>
#include <string>

namespace lantern::iop {
namespace {
std::string read_cstring(IopMemory& mem, uint32_t addr, size_t max = 256) {
  std::string out;
  for (size_t i = 0; i < max; ++i) {
    const uint8_t c = mem.read<uint8_t>(addr + static_cast<uint32_t>(i));
    if (!c) break;
    out.push_back(static_cast<char>(c));
  }
  return out;
}

// Minimal 32-bit O32 varargs formatter for sysmem.Kprintf/stdio.printf. It exists so the modules
// can tell us what they are doing; malformed format strings are echoed rather than trusted.
std::string format_kprintf(IopMemory& mem, IopCpu& cpu, uint32_t fmt_addr) {
  std::string out;
  uint32_t arg = 0;
  auto next_arg = [&]() -> uint32_t {
    uint32_t v = 0;
    if (arg < 3)
      v = cpu.gpr[5 + arg];
    else
      v = mem.read<uint32_t>(cpu.gpr[29] + 16 + (arg - 3) * 4);
    ++arg;
    return v;
  };
  for (uint32_t i = 0; i < 4096; ++i) {
    const char c = static_cast<char>(mem.read<uint8_t>(fmt_addr + i));
    if (!c) break;
    if (c != '%') {
      out.push_back(c);
      continue;
    }
    std::string spec = "%";
    ++i;
    char f = static_cast<char>(mem.read<uint8_t>(fmt_addr + i));
    while (f == '-' || f == '+' || f == ' ' || f == '#' || f == '0') {
      spec.push_back(f);
      f = static_cast<char>(mem.read<uint8_t>(fmt_addr + ++i));
    }
    while (f >= '0' && f <= '9') {
      spec.push_back(f);
      f = static_cast<char>(mem.read<uint8_t>(fmt_addr + ++i));
    }
    if (f == '.') {
      spec.push_back(f);
      f = static_cast<char>(mem.read<uint8_t>(fmt_addr + ++i));
      while (f >= '0' && f <= '9') {
        spec.push_back(f);
        f = static_cast<char>(mem.read<uint8_t>(fmt_addr + ++i));
      }
    }
    while (f == 'l' || f == 'h' || f == 'z') f = static_cast<char>(mem.read<uint8_t>(fmt_addr + ++i));
    char buf[512];
    if (f == '%') {
      out.push_back('%');
      continue;
    }
    if (f == 's') {
      const std::string str = read_cstring(mem, next_arg(), 400);
      spec += 's';
      std::snprintf(buf, sizeof buf, spec.c_str(), str.c_str());
      out += buf;
    } else if (f == 'c') {
      spec += 'c';
      std::snprintf(buf, sizeof buf, spec.c_str(), static_cast<int>(next_arg()));
      out += buf;
    } else if (f == 'd' || f == 'i') {
      spec += 'd';
      std::snprintf(buf, sizeof buf, spec.c_str(), static_cast<int32_t>(next_arg()));
      out += buf;
    } else if (f == 'u' || f == 'x' || f == 'X' || f == 'o' || f == 'p') {
      spec += f == 'p' ? 'x' : f;
      std::snprintf(buf, sizeof buf, spec.c_str(), next_arg());
      out += buf;
    } else {
      out.push_back('%');
      out.push_back(f);
    }
    if (out.size() > 4096) break;
  }
  return out;
}

}  // namespace

void IopKernel::ensure_callback_stack() {
  if (callback_stack_base) return;
  callback_stack_base = heap.alloc(0x4000);
  callback_stack_top = callback_stack_base + 0x4000;
}

uint32_t IopKernel::schedule_alarm(uint32_t callback, uint32_t arg, uint64_t usec, uint32_t gp) {
  ensure_callback_stack();
  const uint32_t id = next_alarm_id++;
  alarms[id] = {callback, arg};
  sched.schedule_callback(callback, arg, gp, callback_stack_base, 0x4000, sched.now_us() + usec, id);
  return id;
}

void IopKernel::arm_timer(int id) {
  if (id < 0 || id >= 6) return;
  TimerState& t = timers.timers[id];
  if (!t.running || !t.handler || !t.compare) return;
  ensure_callback_stack();
  double hz = 36864000.0;  // TC_SYSCLOCK
  if (t.source & 4)
    hz = 15734.26573;  // TC_HLINE
  else if (t.source & 2)
    hz = 13500000.0;  // TC_PIXEL
  if (t.prescale > 0) hz /= t.prescale;
  uint64_t usec = static_cast<uint64_t>(static_cast<double>(t.compare) * 1000000.0 / hz);
  if (usec == 0) usec = 1;
  const uint32_t generation = ++t.generation;
  sched.schedule_callback(t.handler, t.arg, t.gp, callback_stack_base, 0x4000, sched.now_us() + usec,
                          0x80000000u | static_cast<uint32_t>(id), [this, id, generation](uint32_t, uint32_t result) {
                            TimerState& tt = timers.timers[id];
                            if (tt.generation != generation || !tt.running) return;
                            if (result) {
                              tt.compare = result;
                              arm_timer(id);
                            }
                          });
}

void IopKernel::raise_irq(int irq) {
  auto it = intr_handlers.find(irq);
  if (it == intr_handlers.end() || !it->second.enabled || !it->second.function) return;
  if (!irq_pending.insert(irq).second) return;  // one pending callback per IRQ line
  ensure_callback_stack();
  const uint32_t gp = sched.current() ? sched.current()->gp : module_gp;
  sched.schedule_callback(it->second.function, it->second.arg, gp, callback_stack_base, 0x4000, sched.now_us(),
                          0xA0000000u | static_cast<uint32_t>(irq),
                          [this, irq](uint32_t, uint32_t) { irq_pending.erase(irq); });
}

void IopKernel::install() {
  auto ret0 = [](IopCpu& cpu, IopMemory&) { cpu.gpr[2] = 0; };

  // intrman: 4 = RegisterIntrHandler, 5 = ReleaseIntrHandler, 6/7 = Enable/DisableIntr,
  // 8/9 = CpuDisable/EnableIntr, 17 = CpuSuspendIntr, 18 = CpuResumeIntr.
  hle.add({"intrman", 0x0102}, 4, [this](IopCpu& cpu, IopMemory&) {
    const int irq = static_cast<int>(cpu.gpr[4]);
    IntrHandler& h = intr_handlers[irq];
    h.mode = static_cast<int>(cpu.gpr[5]);
    h.function = cpu.gpr[6];
    h.arg = cpu.gpr[7];
    cpu.gpr[2] = 0;
  });
  hle.add({"intrman", 0x0102}, 5, [this](IopCpu& cpu, IopMemory&) {
    intr_handlers.erase(static_cast<int>(cpu.gpr[4]));
    cpu.gpr[2] = 0;
  });
  hle.add({"intrman", 0x0102}, 6, [this](IopCpu& cpu, IopMemory&) {
    intr_handlers[static_cast<int>(cpu.gpr[4])].enabled = true;
    cpu.gpr[2] = 0;
  });
  hle.add({"intrman", 0x0102}, 7, [this](IopCpu& cpu, IopMemory& m) {
    intr_handlers[static_cast<int>(cpu.gpr[4])].enabled = false;
    const uint32_t res = cpu.gpr[5];
    if (res) m.write<uint32_t>(res, 0);
    cpu.gpr[2] = 0;
  });
  hle.add({"intrman", 0x0102}, 8, ret0);
  hle.add({"intrman", 0x0102}, 9, ret0);
  hle.add({"intrman", 0x0102}, 17, [](IopCpu& cpu, IopMemory& m) {
    const uint32_t state = cpu.gpr[4];
    if (state) m.write<uint32_t>(state, 0);
    cpu.gpr[2] = 0;
  });
  hle.add({"intrman", 0x0102}, 18, ret0);

  // loadcore: 4/5 = FlushIcache/FlushDcache, 6 = RegisterLibraryEntries,
  // 7 = ReleaseLibraryEntries.
  hle.add({"loadcore", 0x0101}, 4, ret0);
  hle.add({"loadcore", 0x0101}, 5, ret0);
  hle.add({"loadcore", 0x0101}, 6, [](IopCpu& cpu, IopMemory& m) {
    const uint32_t table = cpu.gpr[4];
    if (table && table < IopMemory::kRamSize)
      std::fprintf(stderr, "[iop] register library '%s' v%u.%u\n", read_cstring(m, table + 12, 8).c_str(),
                   m.read<uint32_t>(table + 8) >> 8, m.read<uint32_t>(table + 8) & 0xFF);
    cpu.gpr[2] = 0;  // KE_OK; loadcore treats 0 as success
  });
  hle.add({"loadcore", 0x0101}, 7, ret0);

  // sysmem: 4 = AllocSysMemory, 5 = FreeSysMemory, 6..8 = size queries,
  // 14 = Kprintf, 15 = KprintfSet.
  hle.add({"sysmem", 0x0101}, 4, [this](IopCpu& cpu, IopMemory&) {
    const uint32_t mode = cpu.gpr[4];
    const int32_t size = static_cast<int32_t>(cpu.gpr[5]);
    const uint32_t ptr = cpu.gpr[6];
    if (size <= 0) {
      cpu.gpr[2] = 0;
      return;
    }
    if (mode == 2 && ptr) {  // ALLOC_ADDRESS: the caller supplies the address
      cpu.gpr[2] = ptr;
      return;
    }
    cpu.gpr[2] = heap.alloc(static_cast<uint32_t>(size));
  });
  hle.add({"sysmem", 0x0101}, 5, ret0);
  hle.add({"sysmem", 0x0101}, 6, [](IopCpu& cpu, IopMemory&) { cpu.gpr[2] = IopMemory::kRamSize; });
  hle.add({"sysmem", 0x0101}, 7, [this](IopCpu& cpu, IopMemory&) { cpu.gpr[2] = heap.end - heap.next; });
  hle.add({"sysmem", 0x0101}, 8, [this](IopCpu& cpu, IopMemory&) { cpu.gpr[2] = heap.end - heap.next; });
  hle.add({"sysmem", 0x0101}, 9, ret0);
  hle.add({"sysmem", 0x0101}, 10, ret0);
  hle.add({"sysmem", 0x0101}, 14, [](IopCpu& cpu, IopMemory& m) {
    std::fprintf(stderr, "[iop] %s", format_kprintf(m, cpu, cpu.gpr[4]).c_str());
    cpu.gpr[2] = 0;
  });
  hle.add({"sysmem", 0x0101}, 15, ret0);

  // sifman: 5 = sceSifInit, 6 = sceSifSetDChain, 7 = sceSifSetDma, 8 = sceSifDmaStat,
  // 21..27 = MS/SM flags and DMA addresses, 29 = sceSifCheckInit. The runtime owns the SIF side,
  // so Init/CheckInit report a ready bus; transfers complete immediately until SIF DMA is wired.
  hle.add({"sifman", 0x0101}, 5, ret0);
  hle.add({"sifman", 0x0101}, 6, ret0);
  hle.add({"sifman", 0x0101}, 7, [](IopCpu& cpu, IopMemory&) {
    // sceSifSetDma(SifDmaTransfer_t *dmat, int count) -> transfer id
    cpu.gpr[2] = cpu.gpr[5] ? 1 : 0;
  });
  hle.add({"sifman", 0x0101}, 8, ret0);  // sceSifDmaStat: 0 = complete
  hle.add({"sifman", 0x0101}, 21, ret0);
  hle.add({"sifman", 0x0101}, 22, ret0);
  hle.add({"sifman", 0x0101}, 23, ret0);
  hle.add({"sifman", 0x0101}, 24, ret0);
  hle.add({"sifman", 0x0101}, 25, ret0);
  hle.add({"sifman", 0x0101}, 26, ret0);
  hle.add({"sifman", 0x0101}, 27, ret0);
  hle.add({"sifman", 0x0101}, 28, ret0);
  hle.add({"sifman", 0x0101}, 29, [](IopCpu& cpu, IopMemory&) { cpu.gpr[2] = 1; });
  hle.add({"sifman", 0x0101}, 30, ret0);
  hle.add({"sifman", 0x0101}, 31, ret0);
  hle.add({"sifman", 0x0101}, 32, [](IopCpu& cpu, IopMemory&) { cpu.gpr[2] = 1; });

  // sifcmd: 14 = sceSifInitRpc, 17 = sceSifRegisterRpc, 19 = sceSifSetRpcQueue,
  // 22 = sceSifRpcLoop. The RPC dispatch itself belongs to the EE/IOP SIF bridge; for now the
  // registry structure is filled so the module's own bookkeeping is consistent.
  hle.add({"sifcmd", 0x0101}, 4, ret0);
  hle.add({"sifcmd", 0x0101}, 5, ret0);
  hle.add({"sifcmd", 0x0101}, 6, ret0);
  hle.add({"sifcmd", 0x0101}, 7, ret0);
  hle.add({"sifcmd", 0x0101}, 8, ret0);
  hle.add({"sifcmd", 0x0101}, 9, ret0);
  hle.add({"sifcmd", 0x0101}, 10, ret0);
  hle.add({"sifcmd", 0x0101}, 11, ret0);
  hle.add({"sifcmd", 0x0101}, 12, [](IopCpu& cpu, IopMemory&) { cpu.gpr[2] = 1; });
  hle.add({"sifcmd", 0x0101}, 13, [](IopCpu& cpu, IopMemory&) { cpu.gpr[2] = 1; });
  hle.add({"sifcmd", 0x0101}, 14, ret0);
  hle.add({"sifcmd", 0x0101}, 15, [](IopCpu& cpu, IopMemory& m) {
    const uint32_t client = cpu.gpr[4];
    if (client && client + 40 <= IopMemory::kRamSize)
      for (unsigned i = 0; i < 10; ++i) m.write<uint32_t>(client + i * 4, 0);
    cpu.gpr[2] = 0;
  });
  hle.add({"sifcmd", 0x0101}, 16, ret0);
  hle.add({"sifcmd", 0x0101}, 17, [this](IopCpu& cpu, IopMemory& m) {
    const uint32_t sd = cpu.gpr[4];
    const uint32_t sp = cpu.gpr[29];
    const uint32_t cfunc = sp + 16 <= IopMemory::kRamSize ? m.read<uint32_t>(sp + 16) : 0;
    const uint32_t cbuff = sp + 20 <= IopMemory::kRamSize ? m.read<uint32_t>(sp + 20) : 0;
    const uint32_t qd = sp + 24 <= IopMemory::kRamSize ? m.read<uint32_t>(sp + 24) : 0;
    if (sd && sd + 68 <= IopMemory::kRamSize) {
      for (unsigned i = 0; i < 17; ++i) m.write<uint32_t>(sd + i * 4, 0);
      m.write<uint32_t>(sd + 0, cpu.gpr[5]);  // sid
      m.write<uint32_t>(sd + 4, cpu.gpr[6]);  // func
      m.write<uint32_t>(sd + 8, cpu.gpr[7]);  // buff
      m.write<uint32_t>(sd + 16, cfunc);
      m.write<uint32_t>(sd + 20, cbuff);
      m.write<uint32_t>(sd + 64, qd);
    }
    const uint32_t sid = cpu.gpr[5], func = cpu.gpr[6], buff = cpu.gpr[7];
    const uint32_t size = (sd && sd + 68 <= IopMemory::kRamSize) ? m.read<uint32_t>(sd + 12) : 0;
    const uint32_t gp = sched.current() ? sched.current()->gp : module_gp;
    if (sid) rpc_servers[sid] = RpcServerInfo{sid, func, buff, size, sd, gp};
    std::fprintf(stderr, "[iop] RegisterRpc sid=0x%08x func=0x%05x buff=0x%05x size=0x%x gp=0x%05x\n", sid, func, buff,
                 size, gp);
    cpu.gpr[2] = 0;
  });
  hle.add({"sifcmd", 0x0101}, 18, ret0);
  hle.add({"sifcmd", 0x0101}, 19, [](IopCpu& cpu, IopMemory& m) {
    const uint32_t q = cpu.gpr[4];
    if (q && q + 24 <= IopMemory::kRamSize) {
      m.write<uint32_t>(q + 0, cpu.gpr[5]);  // thread id
      m.write<uint32_t>(q + 4, 0);           // active
      m.write<uint32_t>(q + 8, 0);
      m.write<uint32_t>(q + 12, 0);
      m.write<uint32_t>(q + 16, 0);
      m.write<uint32_t>(q + 20, 0);
    }
    cpu.gpr[2] = q;
  });
  hle.add({"sifcmd", 0x0101}, 20, ret0);
  hle.add({"sifcmd", 0x0101}, 21, ret0);
  hle.add({"sifcmd", 0x0101}, 22, ret0);  // sceSifRpcLoop: no requests yet
  hle.add({"sifcmd", 0x0101}, 23, ret0);
  hle.add({"sifcmd", 0x0101}, 24, ret0);
  hle.add({"sifcmd", 0x0101}, 25, ret0);
  hle.add({"sifcmd", 0x0101}, 26, ret0);
  hle.add({"sifcmd", 0x0101}, 27, ret0);
  hle.add({"sifcmd", 0x0101}, 28, [](IopCpu& cpu, IopMemory&) { cpu.gpr[2] = 1; });
  hle.add({"sifcmd", 0x0101}, 29, [](IopCpu& cpu, IopMemory&) { cpu.gpr[2] = 1; });

  // thsemap: 4 = CreateSema, 5 = DeleteSema, 6/7 = SignalSema, 8 = WaitSema, 9 = PollSema,
  // 11/12 = ReferSemaStatus. SignalSema hands a token directly to a waiter instead of incrementing
  // and then waking, so the resumed WaitSema does not consume a second count.
  hle.add({"thsemap", 0x0101}, 4, [this](IopCpu& cpu, IopMemory& m) {
    const uint32_t p = cpu.gpr[4];
    if (!p || p + 16 > IopMemory::kRamSize) {
      cpu.gpr[2] = 0xFFFFFFFFu;
      return;
    }
    auto& s = semas.semas.emplace_back();
    s.used = true;
    s.attr = m.read<uint32_t>(p);
    s.option = m.read<uint32_t>(p + 4);
    s.initial = static_cast<int>(m.read<uint32_t>(p + 8));
    s.max = static_cast<int>(m.read<uint32_t>(p + 12));
    s.current = s.initial;
    cpu.gpr[2] = static_cast<uint32_t>(semas.semas.size() - 1);
  });
  hle.add({"thsemap", 0x0101}, 5, [this](IopCpu& cpu, IopMemory&) {
    if (auto* s = semas.get(cpu.gpr[4])) s->used = false;
    cpu.gpr[2] = 0;
  });
  auto signal = [this](IopCpu& cpu, IopMemory&) {
    const uint32_t id = cpu.gpr[4];
    if (!sched.wake_one(ThreadWaitKind::Sema, id)) {
      if (auto* s = semas.get(id); s && s->current < s->max) ++s->current;
    }
    cpu.gpr[2] = 0;
  };
  hle.add({"thsemap", 0x0101}, 6, signal);
  hle.add({"thsemap", 0x0101}, 7, signal);
  hle.add({"thsemap", 0x0101}, 8, [this](IopCpu& cpu, IopMemory&) {
    const uint32_t id = cpu.gpr[4];
    auto* s = semas.get(id);
    if (s && s->current > 0) {
      --s->current;
      cpu.gpr[2] = 0;
      return;
    }
    if (s) ++s->waits;
    cpu.gpr[2] = 0;
    sched.block_current({ThreadWaitKind::Sema, 0, id});
  });
  hle.add({"thsemap", 0x0101}, 9, [this](IopCpu& cpu, IopMemory&) {
    auto* s = semas.get(cpu.gpr[4]);
    if (s && s->current > 0) {
      --s->current;
      cpu.gpr[2] = 0;
    } else {
      cpu.gpr[2] = 0xFFFFFFFFu;  // busy
    }
  });
  auto refer_sema = [this](IopCpu& cpu, IopMemory& m) {
    const uint32_t info = cpu.gpr[5];
    if (auto* s = semas.get(cpu.gpr[4]); s && info && info + 32 <= IopMemory::kRamSize) {
      m.write<uint32_t>(info + 0, s->attr);
      m.write<uint32_t>(info + 4, s->option);
      m.write<uint32_t>(info + 8, static_cast<uint32_t>(s->initial));
      m.write<uint32_t>(info + 12, static_cast<uint32_t>(s->max));
      m.write<uint32_t>(info + 16, static_cast<uint32_t>(s->current));
      m.write<uint32_t>(info + 20, sched.sleeping_on(ThreadWaitKind::Sema, cpu.gpr[4]));
      m.write<uint32_t>(info + 24, 0);
      m.write<uint32_t>(info + 28, 0);
    }
    cpu.gpr[2] = 0;
  };
  hle.add({"thsemap", 0x0101}, 11, refer_sema);
  hle.add({"thsemap", 0x0101}, 12, refer_sema);

  // vblank: 4..7 = WaitVblankStart/End/WaitVblank/WaitNonVblank. The scheduler owns the virtual
  // vblank clock and wakes every waiter at the next frame; 8/9 register/release handlers.
  for (uint16_t fn = 4; fn <= 7; ++fn)
    hle.add({"vblank", 0x0101}, fn, [this](IopCpu& cpu, IopMemory&) {
      cpu.gpr[2] = 0;
      sched.block_current({ThreadWaitKind::Vblank, 0, 0});
    });
  hle.add({"vblank", 0x0101}, 8, ret0);
  hle.add({"vblank", 0x0101}, 9, ret0);

  // timrman: 4 = AllocHardTimer, 5 = ReferHardTimer, 6 = FreeHardTimer, 7 = SetTimerMode,
  // 8 = GetTimerStatus, 9/10 = Set/GetTimerCounter, 11/12 = Set/GetTimerCompare.
  hle.add({"timrman", 0x0102}, 4, [this](IopCpu& cpu, IopMemory&) {
    cpu.gpr[2] = static_cast<uint32_t>(timers.allocate(cpu.gpr[4], cpu.gpr[5], cpu.gpr[6]));
  });
  hle.add({"timrman", 0x0102}, 5, [this](IopCpu& cpu, IopMemory&) {
    cpu.gpr[2] = static_cast<uint32_t>(timers.allocate(cpu.gpr[4], cpu.gpr[5], 1));
  });
  hle.add({"timrman", 0x0102}, 6, [this](IopCpu& cpu, IopMemory&) {
    const uint32_t id = cpu.gpr[4];
    if (id < 6) timers.timers[id].used = false;
    cpu.gpr[2] = 0;
  });
  hle.add({"timrman", 0x0102}, 7, [this](IopCpu& cpu, IopMemory&) {
    const uint32_t id = cpu.gpr[4];
    if (id < 6) timers.timers[id].mode = cpu.gpr[5];
    cpu.gpr[2] = 0;
  });
  hle.add({"timrman", 0x0102}, 8, [this](IopCpu& cpu, IopMemory&) {
    const uint32_t id = cpu.gpr[4];
    cpu.gpr[2] = id < 6 ? timers.timers[id].mode & 0x3800 : 0;
  });
  hle.add({"timrman", 0x0102}, 9, [this](IopCpu& cpu, IopMemory&) {
    const uint32_t id = cpu.gpr[4];
    if (id < 6) timers.timers[id].counter = cpu.gpr[5];
    cpu.gpr[2] = 0;
  });
  hle.add({"timrman", 0x0102}, 10, [this](IopCpu& cpu, IopMemory&) {
    const uint32_t id = cpu.gpr[4];
    cpu.gpr[2] = id < 6 ? timers.timers[id].counter : 0;
  });
  hle.add({"timrman", 0x0102}, 11, [this](IopCpu& cpu, IopMemory&) {
    const uint32_t id = cpu.gpr[4];
    if (id < 6) timers.timers[id].compare = cpu.gpr[5];
    cpu.gpr[2] = 0;
  });
  hle.add({"timrman", 0x0102}, 12, [this](IopCpu& cpu, IopMemory&) {
    const uint32_t id = cpu.gpr[4];
    cpu.gpr[2] = id < 6 ? timers.timers[id].compare : 0;
  });
  hle.add({"timrman", 0x0102}, 13, ret0);
  hle.add({"timrman", 0x0102}, 14, ret0);
  hle.add({"timrman", 0x0102}, 15, ret0);
  hle.add({"timrman", 0x0102}, 16, [](IopCpu& cpu, IopMemory&) { cpu.gpr[2] = 14; });
  hle.add({"timrman", 0x0102}, 17, [this](IopCpu& cpu, IopMemory&) {
    const uint32_t id = cpu.gpr[4];
    cpu.gpr[2] = id < 6 ? timers.timers[id].mode : 0;
  });
  hle.add({"timrman", 0x0102}, 18, ret0);
  // 20 = SetTimerHandler(timid, compare, handler, arg), 22 = SetupHardTimer,
  // 23 = StartHardTimer, 24 = StopHardTimer.
  hle.add({"timrman", 0x0102}, 20, [this](IopCpu& cpu, IopMemory&) {
    const uint32_t id = cpu.gpr[4];
    if (id < 6) {
      timers.timers[id].compare = cpu.gpr[5];
      timers.timers[id].handler = cpu.gpr[6];
      timers.timers[id].arg = cpu.gpr[7];
      timers.timers[id].gp = sched.current() ? sched.current()->gp : module_gp;
    }
    cpu.gpr[2] = 0;
  });
  hle.add({"timrman", 0x0102}, 22, [this](IopCpu& cpu, IopMemory&) {
    const uint32_t id = cpu.gpr[4];
    if (id < 6) {
      timers.timers[id].source = static_cast<int>(cpu.gpr[5]);
      timers.timers[id].mode = cpu.gpr[6];
      timers.timers[id].prescale = static_cast<int>(cpu.gpr[7]);
    }
    cpu.gpr[2] = 0;
  });
  hle.add({"timrman", 0x0102}, 23, [this](IopCpu& cpu, IopMemory&) {
    const uint32_t id = cpu.gpr[4];
    if (id < 6) {
      timers.timers[id].running = true;
      arm_timer(static_cast<int>(id));
    }
    cpu.gpr[2] = 0;
  });
  hle.add({"timrman", 0x0102}, 24, [this](IopCpu& cpu, IopMemory&) {
    const uint32_t id = cpu.gpr[4];
    if (id < 6) {
      timers.timers[id].running = false;
      ++timers.timers[id].generation;
    }
    cpu.gpr[2] = 0;
  });

  // thbase: 4 = CreateThread, 5 = DeleteThread, 6/7 = StartThread(Args), 8/9 = ExitThread,
  // 14/15 = ChangeThreadPriority, 16/17 = RotateThreadReadyQueue, 18/19 = ReleaseWaitThread,
  // 20 = GetThreadId, 22/23 = ReferThreadStatus, 24 = SleepThread, 25/26 = WakeupThread,
  // 27/28 = CancelWakeupThread, 33 = DelayThread, 34 = GetSystemTime.
  hle.add({"thbase", 0x0101}, 4, [this](IopCpu& cpu, IopMemory& m) {
    const uint32_t p = cpu.gpr[4];
    if (!p || p + 20 > IopMemory::kRamSize) {
      cpu.gpr[2] = 0xFFFFFFFFu;
      return;
    }
    const uint32_t entry = m.read<uint32_t>(p + 8);
    uint32_t stack_size = m.read<uint32_t>(p + 12);
    const uint32_t priority = m.read<uint32_t>(p + 16);
    if (stack_size < 0x400) stack_size = 0x400;
    const uint32_t stack = heap.alloc(stack_size + 16);
    if (stack && stack < 0x100000) std::fprintf(stderr, "[iop] thread stack 0x%05x size 0x%x\n", stack, stack_size);
    if (!entry || !stack) {
      cpu.gpr[2] = 0xFFFFFFFFu;
      return;
    }
    const uint32_t gp = sched.current() ? sched.current()->gp : module_gp;
    const uint32_t id = sched.create_thread(entry, 0, static_cast<int>(priority), stack, stack_size, gp);
    cpu.gpr[2] = id;
    std::fprintf(stderr, "[iop] CreateThread id=%u entry=0x%05x stack=0x%05x size=0x%x prio=%u\n", id, entry, stack,
                 stack_size, priority);
  });
  hle.add({"thbase", 0x0101}, 5, [this](IopCpu& cpu, IopMemory&) {
    cpu.gpr[2] = 0;
    if (!sched.delete_thread(cpu.gpr[4])) cpu.gpr[2] = 0xFFFFFFFFu;
  });
  hle.add({"thbase", 0x0101}, 6, [this](IopCpu& cpu, IopMemory&) {
    const bool ok = sched.start_thread(cpu.gpr[4], cpu.gpr[5]);
    if (ok) std::fprintf(stderr, "[iop] StartThread id=%u arg=0x%05x\n", cpu.gpr[4], cpu.gpr[5]);
    cpu.gpr[2] = ok ? 0 : 0xFFFFFFFFu;
  });
  hle.add({"thbase", 0x0101}, 7, [this](IopCpu& cpu, IopMemory&) {
    const bool ok = sched.start_thread(cpu.gpr[4], cpu.gpr[6]);
    cpu.gpr[2] = ok ? 0 : 0xFFFFFFFFu;
  });
  hle.add({"thbase", 0x0101}, 8, [this](IopCpu&, IopMemory&) { sched.exit_current(); });
  hle.add({"thbase", 0x0101}, 9, [this](IopCpu&, IopMemory&) { sched.exit_current(); });
  auto change_priority = [this](IopCpu& cpu, IopMemory&) {
    sched.set_priority(cpu.gpr[4], static_cast<int>(cpu.gpr[5]));
    cpu.gpr[2] = 0;
  };
  hle.add({"thbase", 0x0101}, 14, change_priority);
  hle.add({"thbase", 0x0101}, 15, change_priority);
  hle.add({"thbase", 0x0101}, 16, [this](IopCpu& cpu, IopMemory&) {
    cpu.gpr[2] = 0;
    sched.yield_current();
  });
  hle.add({"thbase", 0x0101}, 17, [this](IopCpu& cpu, IopMemory&) {
    cpu.gpr[2] = 0;
    sched.yield_current();
  });
  auto release_wait = [this](IopCpu& cpu, IopMemory&) {
    sched.wake_thread(cpu.gpr[4]);
    cpu.gpr[2] = 0;
  };
  hle.add({"thbase", 0x0101}, 18, release_wait);
  hle.add({"thbase", 0x0101}, 19, release_wait);
  hle.add({"thbase", 0x0101}, 20,
          [this](IopCpu& cpu, IopMemory&) { cpu.gpr[2] = static_cast<uint32_t>(sched.current_id()); });
  auto refer = [this](IopCpu& cpu, IopMemory& m) {
    const uint32_t info = cpu.gpr[5];
    IopThread* t = sched.thread(cpu.gpr[4]);
    if (!t || !info || info + 52 > IopMemory::kRamSize) {
      cpu.gpr[2] = 0xFFFFFFFFu;
      return;
    }
    for (unsigned i = 0; i < 13; ++i) m.write<uint32_t>(info + i * 4, 0);
    uint32_t status = 0x10, wait_type = 0;
    switch (t->state) {
      case ThreadState::Running: status = 0x01; break;
      case ThreadState::Ready: status = 0x02; break;
      case ThreadState::Delay:
        status = 0x04;
        wait_type = 2;
        break;
      case ThreadState::Sleep:
        status = 0x04;
        wait_type = 1;
        break;
      case ThreadState::Sema:
        status = 0x04;
        wait_type = 3;
        break;
      case ThreadState::Vblank:
        status = 0x04;
        wait_type = 4;
        break;
      case ThreadState::Dormant:
      case ThreadState::Dead: status = 0x10; break;
    }
    m.write<uint32_t>(info + 8, status);
    m.write<uint32_t>(info + 12, t->entry);
    m.write<uint32_t>(info + 16, t->stack_base);
    m.write<uint32_t>(info + 20, t->stack_size);
    m.write<uint32_t>(info + 24, t->gp);
    m.write<uint32_t>(info + 28, static_cast<uint32_t>(t->priority));
    m.write<uint32_t>(info + 32, static_cast<uint32_t>(t->priority));
    m.write<uint32_t>(info + 36, wait_type);
    m.write<uint32_t>(info + 40, t->wait_object);
    cpu.gpr[2] = 0;
  };
  hle.add({"thbase", 0x0101}, 22, refer);
  hle.add({"thbase", 0x0101}, 23, refer);
  hle.add({"thbase", 0x0101}, 24, [this](IopCpu& cpu, IopMemory&) {
    cpu.gpr[2] = 0;
    sched.block_current({ThreadWaitKind::Sleep, 0, 0});
  });
  hle.add({"thbase", 0x0101}, 25, release_wait);
  hle.add({"thbase", 0x0101}, 26, release_wait);
  hle.add({"thbase", 0x0101}, 27, ret0);
  hle.add({"thbase", 0x0101}, 28, ret0);
  hle.add({"thbase", 0x0101}, 33, [this](IopCpu& cpu, IopMemory&) {
    cpu.gpr[2] = 0;
    sched.block_current({ThreadWaitKind::Delay, sched.now_us() + cpu.gpr[4], 0});
  });
  hle.add({"thbase", 0x0101}, 34, [this](IopCpu& cpu, IopMemory& m) {
    const uint32_t p = cpu.gpr[4];
    const uint64_t clocks = sched.now_us() * 36864 / 1000;
    if (p && p + 8 <= IopMemory::kRamSize) {
      m.write<uint32_t>(p, static_cast<uint32_t>(clocks));
      m.write<uint32_t>(p + 4, static_cast<uint32_t>(clocks >> 32));
    }
    cpu.gpr[2] = 0;
  });

  // Clock conversion: the IOP time base is 36.864 MHz.
  hle.add({"thbase", 0x0101}, 39, [](IopCpu& cpu, IopMemory& m) {
    const uint32_t usec = cpu.gpr[4];
    const uint32_t out = cpu.gpr[5];
    const uint64_t clocks = static_cast<uint64_t>(usec) * 36864 / 1000;
    if (out && out + 8 <= IopMemory::kRamSize) {
      m.write<uint32_t>(out, static_cast<uint32_t>(clocks));
      m.write<uint32_t>(out + 4, static_cast<uint32_t>(clocks >> 32));
    }
    cpu.gpr[2] = 0;
  });
  hle.add({"thbase", 0x0101}, 40, [](IopCpu& cpu, IopMemory& m) {
    const uint32_t clk = cpu.gpr[4];
    const uint32_t secp = cpu.gpr[5], usecp = cpu.gpr[6];
    uint64_t clocks = 0;
    if (clk && clk + 8 <= IopMemory::kRamSize)
      clocks = m.read<uint32_t>(clk) | (static_cast<uint64_t>(m.read<uint32_t>(clk + 4)) << 32);
    const uint64_t usec = clocks * 1000 / 36864;
    if (secp) m.write<uint32_t>(secp, static_cast<uint32_t>(usec / 1000000));
    if (usecp) m.write<uint32_t>(usecp, static_cast<uint32_t>(usec % 1000000));
    cpu.gpr[2] = 0;
  });
  auto set_alarm = [this](IopCpu& cpu, IopMemory& m) {
    const uint32_t clk = cpu.gpr[4];
    uint64_t clocks = 0;
    if (clk && clk + 8 <= IopMemory::kRamSize)
      clocks = m.read<uint32_t>(clk) | (static_cast<uint64_t>(m.read<uint32_t>(clk + 4)) << 32);
    const uint64_t usec = clocks * 1000 / 36864;
    const uint32_t gp = sched.current() ? sched.current()->gp : module_gp;
    cpu.gpr[2] = schedule_alarm(cpu.gpr[5], cpu.gpr[6], usec, gp);
  };
  hle.add({"thbase", 0x0101}, 35, set_alarm);
  hle.add({"thbase", 0x0101}, 36, set_alarm);
  auto cancel_alarm = [this](IopCpu& cpu, IopMemory&) {
    const uint32_t cb = cpu.gpr[4], arg = cpu.gpr[5];
    for (auto it = alarms.begin(); it != alarms.end();) {
      if (it->second.first == cb && it->second.second == arg) {
        sched.cancel_callback(it->first);
        it = alarms.erase(it);
      } else {
        ++it;
      }
    }
    cpu.gpr[2] = 0;
  };
  hle.add({"thbase", 0x0101}, 37, cancel_alarm);
  hle.add({"thbase", 0x0101}, 38, cancel_alarm);
  hle.add({"thbase", 0x0101}, 41, ret0);

  // stdio: 4 = printf (the modules use it for progress messages).
  hle.add({"stdio", 0x0102}, 4, [](IopCpu& cpu, IopMemory& m) {
    std::fprintf(stderr, "[iop] %s", format_kprintf(m, cpu, cpu.gpr[4]).c_str());
    cpu.gpr[2] = 0;
  });
}

}  // namespace lantern::iop
