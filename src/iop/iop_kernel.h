#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "iop/hle.h"
#include "iop/iop_cpu.h"
#include "iop/iop_threads.h"

namespace lantern::iop {

// Bump allocator for sysmem.AllocSysMemory and thread stacks. Modules are loaded below 1 MiB.
struct Heap {
  uint32_t next = 0x00100000;
  uint32_t end = IopHle::kTrampolineBase;

  uint32_t alloc(uint32_t size) {
    if (size == 0) return 0;
    const uint32_t base = (next + 15) & ~15u;
    if (base < next || uint64_t{base} + size > end) return 0;
    next = base + size;
    return base;
  }
};

// Hardware timer allocation (timrman). The registry and register values are real; the counters do
// not advance yet, because that needs the timer interrupt model.
struct TimerState {
  bool used = false;
  bool running = false;
  uint32_t mode = 0, counter = 0, compare = 0;
  uint32_t handler = 0, arg = 0, gp = 0;
  uint32_t generation = 0;
  int source = 0, size = 0, prescale = 0;
};

struct TimerTable {
  TimerState timers[6];

  int allocate(int source, int size, int prescale) {
    for (int i = 0; i < 6; ++i) {
      if (timers[i].used) continue;
      if ((size == 32) != (i >= 3)) continue;  // timers 0..2 are 16-bit, 3..5 are 32-bit
      timers[i] = TimerState{};
      timers[i].used = true;
      timers[i].source = source;
      timers[i].size = size;
      timers[i].prescale = prescale;
      return i;
    }
    return -1;
  }
};

struct SemaTable {
  struct Sema {
    bool used = false;
    uint32_t attr = 0, option = 0;
    int initial = 0, max = 0, current = 0;
    uint32_t waits = 0;
  };
  std::vector<Sema> semas;  // index 0 is unused; ids start at 1
  uint32_t next_id = 1;

  SemaTable() : semas(1) {}
  Sema* get(uint32_t id) { return id < semas.size() && semas[id].used ? &semas[id] : nullptr; }
};

// An RPC server registered by an IOP module through sifcmd.sceSifRegisterRpc. `struct_addr` is the
// module's SifRpcServerData_t; `buff` is the buffer the EE writes arguments into.
struct RpcServerInfo {
  uint32_t sid = 0;
  uint32_t function = 0;
  uint32_t buff = 0;
  uint32_t size = 0;
  uint32_t struct_addr = 0;
  uint32_t gp = 0;
};

// The IOP kernel/library HLE: loadcore, intrman, sysmem, sifman/sifcmd, thbase, thsemap, timrman,
// vblank and stdio. Shared by the development probe and the boot runtime.
struct IopKernel {
  IopHle& hle;
  IopScheduler& sched;
  Heap heap;
  TimerTable timers;
  SemaTable semas;
  uint32_t module_gp = 0;
  std::map<uint32_t, RpcServerInfo> rpc_servers;
  struct IntrHandler {
    uint32_t function = 0, arg = 0;
    int mode = 0;
    bool enabled = false;
  };
  std::map<int, IntrHandler> intr_handlers;
  std::set<int> irq_pending;
  std::map<uint32_t, std::pair<uint32_t, uint32_t>> alarms;  // id -> (callback, arg)
  uint32_t next_alarm_id = 1;
  uint32_t callback_stack_base = 0;
  uint32_t callback_stack_top = 0;

  IopKernel(IopHle& hle_ref, IopScheduler& sched_ref) : hle(hle_ref), sched(sched_ref) {}

  void install();

  void ensure_callback_stack();
  uint32_t schedule_alarm(uint32_t callback, uint32_t arg, uint64_t usec, uint32_t gp);
  void arm_timer(int id);
  void raise_irq(int irq);

  const RpcServerInfo* find_rpc(uint32_t sid) const {
    auto it = rpc_servers.find(sid);
    return it == rpc_servers.end() ? nullptr : &it->second;
  }
};

}  // namespace lantern::iop
