// lantern-iop: load the game's IOP modules the way the IOP would, and probe their entry points.
//
//   lantern-iop --extracted DIR [--inspect] [--romdir] [--module NAME] [--steps N] [--trace N]
//
// This is a development tool: it reads the user's own extracted disc data, never writes it, and
// reports what the modules import instead of inventing kernel behaviour.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "iop/hle.h"
#include "iop/iop_interp.h"
#include "iop/irx.h"

namespace fs = std::filesystem;
using namespace lantern;
using namespace lantern::iop;

namespace {
struct Options {
  fs::path extracted;
  std::string module;
  uint64_t steps = 200000;
  unsigned trace = 24;
  bool inspect = false;
  bool romdir = false;
  bool mmio = false;
};

// Unmapped IOP register access, recorded rather than rejected. Returning zero lets a module run
// far enough to show which registers it actually depends on; the log is the evidence for the next
// hardware step, not an emulation of those registers.
struct MmioLog {
  struct Access {
    uint64_t count = 0;
    uint32_t last = 0;
    unsigned size = 0;
  };
  std::map<uint32_t, Access> reads, writes;
  uint64_t read_count = 0, write_count = 0;

  void on_read(uint32_t addr, void* out, unsigned size) {
    ++read_count;
    ++reads[addr].count;
    reads[addr].size = size;
    std::memset(out, 0, size);
  }
  void on_write(uint32_t addr, const void* in, unsigned size) {
    ++write_count;
    uint32_t value = 0;
    std::memcpy(&value, in, size > 4 ? 4 : size);
    auto& a = writes[addr];
    ++a.count;
    a.last = value;
    a.size = size;
  }
  void print() const {
    std::printf("  MMIO: %llu reads (%zu addresses), %llu writes (%zu addresses)\n",
                static_cast<unsigned long long>(read_count), reads.size(), static_cast<unsigned long long>(write_count),
                writes.size());
    unsigned shown = 0;
    for (const auto& [addr, a] : writes) {
      if (shown++ >= 40) {
        std::printf("    ...\n");
        break;
      }
      std::printf("    write 0x%08x = 0x%0*x  (%llu)\n", addr, static_cast<int>(a.size * 2), a.last,
                  static_cast<unsigned long long>(a.count));
    }
    shown = 0;
    for (const auto& [addr, a] : reads) {
      if (shown++ >= 20) {
        std::printf("    ...\n");
        break;
      }
      std::printf("    read  0x%08x  (%llu)\n", addr, static_cast<unsigned long long>(a.count));
    }
  }
};

struct Source {
  std::string label;
  std::vector<uint8_t> bytes;
};

std::vector<uint8_t> read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

bool has_irx_extension(const fs::path& p) {
  std::string ext = p.extension().string();
  for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return ext == ".irx";
}

// IOPRP.IMG: a ROMDIR table at the start, entries of { char name[10]; u16 extinfo_size;
// u32 size; }. The payload of each entry begins at a 16-byte aligned offset after the table.
std::vector<Source> read_romdir(const fs::path& path) {
  std::vector<Source> out;
  auto bytes = read_file(path);
  if (bytes.size() < 64 || std::memcmp(bytes.data(), "RESET\0", 6) != 0) return out;
  uint32_t data_off = 0;
  for (size_t off = 0; off + 16 <= bytes.size(); off += 16) {
    const uint8_t* e = bytes.data() + off;
    if (e[0] == 0) break;
    std::string name;
    for (unsigned i = 0; i < 10 && e[i] >= 0x20 && e[i] < 0x7F; ++i) name.push_back(static_cast<char>(e[i]));
    uint32_t size = 0;
    std::memcpy(&size, e + 12, 4);
    if (size >= 4 && uint64_t{data_off} + size <= bytes.size()) {
      Source s;
      s.label = name;
      s.bytes.assign(bytes.begin() + data_off, bytes.begin() + data_off + size);
      out.push_back(std::move(s));
    }
    data_off += (size + 15) & ~15u;
    if (out.size() > 512) break;
  }
  return out;
}

struct Placed {
  std::string label;
  uint32_t base = 0;
  std::vector<uint8_t> bytes;
  LoadedModule module;
};

// A bump allocator for AllocSysMemory. Modules are placed below 1 MiB, so the probe heap takes the
// space between them and the HLE trampoline page.
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

// Diagnostic thread table: CreateThread records the caller's iop_thread_t, StartThread marks it
// runnable, and the probe runs each body once after the initialiser returns. This is not a
// scheduler yet; it exists to reach the thread bodies and report what they import.
struct ThreadExit {};

struct FakeThread {
  uint32_t id = 0, entry = 0, arg = 0, stack = 0, stack_size = 0, priority = 0;
  bool started = false, ran = false;
};

struct ThreadTable {
  std::vector<FakeThread> threads;
  uint32_t next_id = 1;
  uint32_t current = 0;  // 0 = the module initialiser
};

FakeThread* find_thread(ThreadTable& table, uint32_t id) {
  for (auto& t : table.threads)
    if (t.id == id) return &t;
  return nullptr;
}

// Hardware timer allocation (timrman). The registry and register values are real; the counters do
// not advance yet, because that needs the timer interrupt model.
struct TimerState {
  bool used = false;
  bool running = false;
  uint32_t mode = 0, counter = 0, compare = 0;
  uint32_t handler = 0, arg = 0;
  int source = 0, size = 0, prescale = 0;
};

struct TimerTable {
  TimerState timers[6];

  int allocate(int source, int size, int prescale) {
    for (int i = 0; i < 6; ++i) {
      if (timers[i].used) continue;
      if ((size == 32) != (i >= 3)) continue;  // timers 0..2 are 16-bit, 3..5 are 32-bit
      timers[i] = TimerState{true, false, 0, 0, 0, 0, 0, source, size, prescale};
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

void install_kernel_handlers(IopHle& hle, Heap& heap, ThreadTable& threads, TimerTable& timers, SemaTable& semas) {
  auto ret0 = [](IopCpu& cpu, IopMemory&) { cpu.gpr[2] = 0; };

  // intrman (ps2sdk iop/system/intrman): 17 = CpuSuspendIntr, 18 = CpuResumeIntr.
  hle.add({"intrman", 0x0102}, 17, [](IopCpu& cpu, IopMemory& m) {
    const uint32_t state = cpu.gpr[4];
    if (state) m.write<uint32_t>(state, 0);
    cpu.gpr[2] = 0;
  });
  // intrman 6/7 = EnableIntr/DisableIntr, 8/9 = CpuDisableIntr/CpuEnableIntr.
  hle.add({"intrman", 0x0102}, 6, ret0);
  hle.add({"intrman", 0x0102}, 7, [](IopCpu& cpu, IopMemory& m) {
    const uint32_t res = cpu.gpr[5];
    if (res) m.write<uint32_t>(res, 0);
    cpu.gpr[2] = 0;
  });
  hle.add({"intrman", 0x0102}, 8, ret0);
  hle.add({"intrman", 0x0102}, 9, ret0);
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
  hle.add({"sysmem", 0x0101}, 4, [&heap](IopCpu& cpu, IopMemory&) {
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
  hle.add({"sysmem", 0x0101}, 7, [&heap](IopCpu& cpu, IopMemory&) { cpu.gpr[2] = heap.end - heap.next; });
  hle.add({"sysmem", 0x0101}, 8, [&heap](IopCpu& cpu, IopMemory&) { cpu.gpr[2] = heap.end - heap.next; });
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
  hle.add({"sifcmd", 0x0101}, 17, [](IopCpu& cpu, IopMemory& m) {
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
  // 11/12 = ReferSemaStatus. WaitSema does not block in the probe: a would-block wait is logged
  // and treated as acquired, which is enough to reach the next dependency but is not scheduling.
  hle.add({"thsemap", 0x0101}, 4, [&semas](IopCpu& cpu, IopMemory& m) {
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
  hle.add({"thsemap", 0x0101}, 5, [&semas](IopCpu& cpu, IopMemory&) {
    if (auto* s = semas.get(cpu.gpr[4])) s->used = false;
    cpu.gpr[2] = 0;
  });
  auto signal = [&semas](IopCpu& cpu, IopMemory&) {
    if (auto* s = semas.get(cpu.gpr[4]); s && s->current < s->max) ++s->current;
    cpu.gpr[2] = 0;
  };
  hle.add({"thsemap", 0x0101}, 6, signal);
  hle.add({"thsemap", 0x0101}, 7, signal);
  hle.add({"thsemap", 0x0101}, 8, [&semas](IopCpu& cpu, IopMemory&) {
    if (auto* s = semas.get(cpu.gpr[4])) {
      if (s->current > 0)
        --s->current;
      else {
        ++s->waits;
        std::fprintf(stderr, "[iop] WaitSema(%u) would block (probe continues)\n", cpu.gpr[4]);
      }
    }
    cpu.gpr[2] = 0;
  });
  hle.add({"thsemap", 0x0101}, 9, [&semas](IopCpu& cpu, IopMemory&) {
    auto* s = semas.get(cpu.gpr[4]);
    if (s && s->current > 0) {
      --s->current;
      cpu.gpr[2] = 0;
    } else {
      cpu.gpr[2] = 0xFFFFFFFFu;  // busy
    }
  });
  hle.add({"thsemap", 0x0101}, 11, [&semas](IopCpu& cpu, IopMemory& m) {
    const uint32_t info = cpu.gpr[5];
    if (auto* s = semas.get(cpu.gpr[4]); s && info && info + 32 <= IopMemory::kRamSize) {
      m.write<uint32_t>(info + 0, s->attr);
      m.write<uint32_t>(info + 4, s->option);
      m.write<uint32_t>(info + 8, static_cast<uint32_t>(s->initial));
      m.write<uint32_t>(info + 12, static_cast<uint32_t>(s->max));
      m.write<uint32_t>(info + 16, static_cast<uint32_t>(s->current));
      m.write<uint32_t>(info + 20, s->waits);
      m.write<uint32_t>(info + 24, 0);
      m.write<uint32_t>(info + 28, 0);
    }
    cpu.gpr[2] = 0;
  });
  hle.add({"thsemap", 0x0101}, 12, [&semas](IopCpu& cpu, IopMemory& m) {
    const uint32_t info = cpu.gpr[5];
    if (auto* s = semas.get(cpu.gpr[4]); s && info && info + 32 <= IopMemory::kRamSize) {
      m.write<uint32_t>(info + 0, s->attr);
      m.write<uint32_t>(info + 4, s->option);
      m.write<uint32_t>(info + 8, static_cast<uint32_t>(s->initial));
      m.write<uint32_t>(info + 12, static_cast<uint32_t>(s->max));
      m.write<uint32_t>(info + 16, static_cast<uint32_t>(s->current));
      m.write<uint32_t>(info + 20, s->waits);
      m.write<uint32_t>(info + 24, 0);
      m.write<uint32_t>(info + 28, 0);
    }
    cpu.gpr[2] = 0;
  });

  // vblank: 4..7 = WaitVblankStart/End/WaitVblank/WaitNonVblank. There is no vblank clock in the
  // probe yet, so the waits complete immediately; 8/9 register/release handlers.
  for (uint16_t fn = 4; fn <= 7; ++fn) hle.add({"vblank", 0x0101}, fn, ret0);
  hle.add({"vblank", 0x0101}, 8, ret0);
  hle.add({"vblank", 0x0101}, 9, ret0);

  // timrman: 4 = AllocHardTimer, 5 = ReferHardTimer, 6 = FreeHardTimer, 7 = SetTimerMode,
  // 8 = GetTimerStatus, 9/10 = Set/GetTimerCounter, 11/12 = Set/GetTimerCompare.
  hle.add({"timrman", 0x0102}, 4, [&timers](IopCpu& cpu, IopMemory&) {
    cpu.gpr[2] = static_cast<uint32_t>(timers.allocate(cpu.gpr[4], cpu.gpr[5], cpu.gpr[6]));
  });
  hle.add({"timrman", 0x0102}, 5, [&timers](IopCpu& cpu, IopMemory&) {
    cpu.gpr[2] = static_cast<uint32_t>(timers.allocate(cpu.gpr[4], cpu.gpr[5], 1));
  });
  hle.add({"timrman", 0x0102}, 6, [&timers](IopCpu& cpu, IopMemory&) {
    const uint32_t id = cpu.gpr[4];
    if (id < 6) timers.timers[id].used = false;
    cpu.gpr[2] = 0;
  });
  hle.add({"timrman", 0x0102}, 7, [&timers](IopCpu& cpu, IopMemory&) {
    const uint32_t id = cpu.gpr[4];
    if (id < 6) timers.timers[id].mode = cpu.gpr[5];
    cpu.gpr[2] = 0;
  });
  hle.add({"timrman", 0x0102}, 8, [&timers](IopCpu& cpu, IopMemory&) {
    const uint32_t id = cpu.gpr[4];
    cpu.gpr[2] = id < 6 ? timers.timers[id].mode & 0x3800 : 0;
  });
  hle.add({"timrman", 0x0102}, 9, [&timers](IopCpu& cpu, IopMemory&) {
    const uint32_t id = cpu.gpr[4];
    if (id < 6) timers.timers[id].counter = cpu.gpr[5];
    cpu.gpr[2] = 0;
  });
  hle.add({"timrman", 0x0102}, 10, [&timers](IopCpu& cpu, IopMemory&) {
    const uint32_t id = cpu.gpr[4];
    cpu.gpr[2] = id < 6 ? timers.timers[id].counter : 0;
  });
  hle.add({"timrman", 0x0102}, 11, [&timers](IopCpu& cpu, IopMemory&) {
    const uint32_t id = cpu.gpr[4];
    if (id < 6) timers.timers[id].compare = cpu.gpr[5];
    cpu.gpr[2] = 0;
  });
  hle.add({"timrman", 0x0102}, 12, [&timers](IopCpu& cpu, IopMemory&) {
    const uint32_t id = cpu.gpr[4];
    cpu.gpr[2] = id < 6 ? timers.timers[id].compare : 0;
  });
  hle.add({"timrman", 0x0102}, 13, ret0);
  hle.add({"timrman", 0x0102}, 14, ret0);
  hle.add({"timrman", 0x0102}, 15, ret0);
  hle.add({"timrman", 0x0102}, 16, [](IopCpu& cpu, IopMemory&) { cpu.gpr[2] = 14; });
  hle.add({"timrman", 0x0102}, 17, [&timers](IopCpu& cpu, IopMemory&) {
    const uint32_t id = cpu.gpr[4];
    cpu.gpr[2] = id < 6 ? timers.timers[id].mode : 0;
  });
  hle.add({"timrman", 0x0102}, 18, ret0);
  // 20 = SetTimerHandler(timid, compare, handler, arg), 22 = SetupHardTimer,
  // 23 = StartHardTimer, 24 = StopHardTimer.
  hle.add({"timrman", 0x0102}, 20, [&timers](IopCpu& cpu, IopMemory&) {
    const uint32_t id = cpu.gpr[4];
    if (id < 6) {
      timers.timers[id].compare = cpu.gpr[5];
      timers.timers[id].handler = cpu.gpr[6];
      timers.timers[id].arg = cpu.gpr[7];
    }
    cpu.gpr[2] = 0;
  });
  hle.add({"timrman", 0x0102}, 22, [&timers](IopCpu& cpu, IopMemory&) {
    const uint32_t id = cpu.gpr[4];
    if (id < 6) {
      timers.timers[id].source = static_cast<int>(cpu.gpr[5]);
      timers.timers[id].mode = cpu.gpr[6];
      timers.timers[id].prescale = static_cast<int>(cpu.gpr[7]);
    }
    cpu.gpr[2] = 0;
  });
  hle.add({"timrman", 0x0102}, 23, [&timers](IopCpu& cpu, IopMemory&) {
    const uint32_t id = cpu.gpr[4];
    if (id < 6) timers.timers[id].running = true;
    cpu.gpr[2] = 0;
  });
  hle.add({"timrman", 0x0102}, 24, [&timers](IopCpu& cpu, IopMemory&) {
    const uint32_t id = cpu.gpr[4];
    if (id < 6) timers.timers[id].running = false;
    cpu.gpr[2] = 0;
  });

  // thbase: 4 = CreateThread, 5 = DeleteThread, 6 = StartThread, 8/9 = ExitThread,
  // 14/15 = ChangeThreadPriority, 18/19 = ReleaseWaitThread, 20 = GetThreadId,
  // 22/23 = ReferThreadStatus, 24 = SleepThread, 33 = DelayThread.
  hle.add({"thbase", 0x0101}, 4, [&heap, &threads](IopCpu& cpu, IopMemory& m) {
    const uint32_t p = cpu.gpr[4];
    if (!p || p + 20 > IopMemory::kRamSize) {
      cpu.gpr[2] = 0xFFFFFFFFu;
      return;
    }
    FakeThread t;
    t.entry = m.read<uint32_t>(p + 8);
    t.stack_size = m.read<uint32_t>(p + 12);
    t.priority = m.read<uint32_t>(p + 16);
    if (t.stack_size < 0x400) t.stack_size = 0x400;
    t.stack = heap.alloc(t.stack_size + 16);
    if (!t.entry || !t.stack) {
      cpu.gpr[2] = 0xFFFFFFFFu;
      return;
    }
    t.id = threads.next_id++;
    cpu.gpr[2] = t.id;
    std::fprintf(stderr, "[iop] CreateThread id=%u entry=0x%05x stack=0x%05x size=0x%x prio=%u\n", t.id, t.entry,
                 t.stack, t.stack_size, t.priority);
    threads.threads.push_back(t);
  });
  hle.add({"thbase", 0x0101}, 5, [&threads](IopCpu& cpu, IopMemory&) {
    if (FakeThread* t = find_thread(threads, cpu.gpr[4])) t->started = false;
    cpu.gpr[2] = 0;
  });
  hle.add({"thbase", 0x0101}, 6, [&threads](IopCpu& cpu, IopMemory&) {
    if (FakeThread* t = find_thread(threads, cpu.gpr[4])) {
      t->arg = cpu.gpr[5];
      t->started = true;
      std::fprintf(stderr, "[iop] StartThread id=%u arg=0x%05x\n", t->id, t->arg);
    } else {
      std::fprintf(stderr, "[iop] StartThread unknown id=%u\n", cpu.gpr[4]);
    }
    cpu.gpr[2] = 0;
  });
  hle.add({"thbase", 0x0101}, 7, [&threads](IopCpu& cpu, IopMemory&) {
    if (FakeThread* t = find_thread(threads, cpu.gpr[4])) {
      t->arg = cpu.gpr[6];
      t->started = true;
    }
    cpu.gpr[2] = 0;
  });
  auto exit_thread = [](IopCpu&, IopMemory&) { throw ThreadExit{}; };
  hle.add({"thbase", 0x0101}, 8, exit_thread);
  hle.add({"thbase", 0x0101}, 9, exit_thread);
  hle.add({"thbase", 0x0101}, 14, ret0);
  hle.add({"thbase", 0x0101}, 15, ret0);
  hle.add({"thbase", 0x0101}, 18, ret0);
  hle.add({"thbase", 0x0101}, 19, ret0);
  hle.add({"thbase", 0x0101}, 20, [&threads](IopCpu& cpu, IopMemory&) { cpu.gpr[2] = threads.current; });
  hle.add({"thbase", 0x0101}, 22, [](IopCpu& cpu, IopMemory& m) {
    const uint32_t info = cpu.gpr[5];
    if (info && info + 52 <= IopMemory::kRamSize)
      for (unsigned i = 0; i < 13; ++i) m.write<uint32_t>(info + i * 4, 0);
    cpu.gpr[2] = 0;
  });
  hle.add({"thbase", 0x0101}, 23, [](IopCpu& cpu, IopMemory& m) {
    const uint32_t info = cpu.gpr[5];
    if (info && info + 52 <= IopMemory::kRamSize)
      for (unsigned i = 0; i < 13; ++i) m.write<uint32_t>(info + i * 4, 0);
    cpu.gpr[2] = 0;
  });
  hle.add({"thbase", 0x0101}, 24, ret0);
  hle.add({"thbase", 0x0101}, 33, ret0);
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
  hle.add({"thbase", 0x0101}, 35, ret0);
  hle.add({"thbase", 0x0101}, 36, ret0);
  hle.add({"thbase", 0x0101}, 37, ret0);
  hle.add({"thbase", 0x0101}, 38, ret0);
  hle.add({"thbase", 0x0101}, 34, [](IopCpu& cpu, IopMemory& m) {
    const uint32_t p = cpu.gpr[4];
    if (p && p + 8 <= IopMemory::kRamSize) {
      m.write<uint32_t>(p, 0);
      m.write<uint32_t>(p + 4, 0);
    }
    cpu.gpr[2] = 0;
  });
  hle.add({"thbase", 0x0101}, 41, ret0);

  // stdio: 4 = printf (the modules use it for progress messages).
  hle.add({"stdio", 0x0102}, 4, [](IopCpu& cpu, IopMemory& m) {
    std::fprintf(stderr, "[iop] %s", format_kprintf(m, cpu, cpu.gpr[4]).c_str());
    cpu.gpr[2] = 0;
  });
}

std::vector<Placed> collect_sources(const Options& opt) {
  std::vector<Source> sources;
  std::error_code ec;
  if (!opt.extracted.empty()) {
    std::vector<fs::path> files;
    for (const auto& e : fs::recursive_directory_iterator(opt.extracted, ec))
      if (e.is_regular_file() && has_irx_extension(e.path())) files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (const auto& f : files) sources.push_back({f.lexically_relative(opt.extracted).string(), read_file(f)});
  }
  if (opt.romdir) {
    fs::path img = opt.extracted / "IOP" / "MODULES" / "IOPRP.IMG";
    auto rom = read_romdir(img);
    if (rom.empty()) std::cerr << "note: no modules read from " << img << "\n";
    for (auto& s : rom) sources.push_back(std::move(s));
  }

  std::vector<Placed> placed;
  uint32_t cursor = 0x00010000;
  for (auto& s : sources) {
    std::string err;
    auto size = irx_image_size(s.bytes, &err);
    if (!size) {
      std::cerr << "skip " << s.label << ": " << err << "\n";
      continue;
    }
    auto base = place_irx(cursor, *size);
    if (!base || uint64_t{*base} + *size > IopHle::kTrampolineBase) {
      std::cerr << "skip " << s.label << ": no room below the HLE trampoline\n";
      continue;
    }
    Placed p;
    p.label = s.label;
    p.base = *base;
    p.bytes = std::move(s.bytes);
    placed.push_back(std::move(p));
    cursor = *base + *size;
  }
  return placed;
}

struct Exported {
  IrxLibraryId library;
  std::vector<uint32_t> functions;
  std::string module;
};

// Two passes: the first learns every module's exports; the second resolves imports against them
// (and leaves kernel imports unresolved for the probe to report).
std::vector<Placed> load_modules(const std::vector<Placed>& sources, IopMemory& mem, IopHle& hle, bool verbose) {
  std::vector<Exported> exports;
  std::vector<Placed> first;
  for (const auto& s : sources) {
    std::string err;
    auto mod = load_irx(mem, s.bytes, {.base = s.base, .resolve = {}}, &err);
    if (!mod) {
      std::cerr << "skip " << s.label << ": " << err << "\n";
      continue;
    }
    for (const auto& e : mod->exports) exports.push_back({e.library, e.functions, s.label});
    first.push_back({s.label, s.base, s.bytes, std::move(*mod)});
  }

  auto resolve = [&](const IrxLibraryId& lib, uint16_t function) -> std::optional<uint32_t> {
    if (auto slot = hle.resolve(lib, function)) return slot;
    for (const auto& e : exports) {
      if (e.library.name != lib.name || e.library.version < lib.version) continue;
      if (function < e.functions.size()) return e.functions[function];
    }
    return std::nullopt;
  };

  std::vector<Placed> loaded;
  for (auto& s : first) {
    std::string err;
    auto mod = load_irx(mem, s.bytes, {.base = s.base, .resolve = resolve}, &err);
    if (!mod) {
      std::cerr << "skip " << s.label << ": " << err << "\n";
      continue;
    }
    if (verbose) {
      std::printf("%-28s '%s' v%u.%u base 0x%05x entry 0x%05x  imports %zu  exports %zu\n", s.label.c_str(),
                  mod->name.c_str(), mod->version >> 8, mod->version & 0xFF, mod->base, mod->entry, mod->imports.size(),
                  mod->exports.size());
    }
    loaded.push_back({s.label, s.base, std::move(s.bytes), std::move(*mod)});
  }
  return loaded;
}

// Every jal/j target in .text must land inside a loaded module (its own code, an import handler
// in another module, or one of its stubs). A target outside every module is a relocation bug.
std::pair<uint32_t, uint32_t> verify_calls(IopMemory& mem, const LoadedModule& mod, const std::vector<Placed>& all) {
  uint32_t ok = 0, bad = 0;
  for (uint32_t pc = mod.base; pc + 4 <= mod.base + mod.text_size; pc += 4) {
    const uint32_t w = mem.read<uint32_t>(pc);
    const uint32_t opcode = w >> 26;
    if (opcode != 2 && opcode != 3) continue;  // j, jal
    const uint32_t target = ((pc + 4) & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2);
    bool inside = target >= IopHle::kTrampolineBase && target < IopHle::kTrampolineBase + IopHle::kTrampolineSize;
    for (const auto& p : all)
      if (target >= p.module.base && target < p.module.end) inside = true;
    if (inside)
      ++ok;
    else {
      if (bad < 8) std::printf("   bad target 0x%05x from 0x%05x\n", target, pc);
      ++bad;
    }
  }
  return {ok, bad};
}

void print_module(const Placed& p) {
  const auto& m = p.module;
  std::printf("\n== %s (base 0x%05x, %u relocations)\n", p.label.c_str(), m.base, m.relocations);
  std::printf("   module '%s' v%u.%u entry 0x%05x gp 0x%05x text 0x%x data 0x%x bss 0x%x end 0x%05x\n", m.name.c_str(),
              m.version >> 8, m.version & 0xFF, m.entry, m.gp, m.text_size, m.data_size, m.bss_size, m.end);
  for (const auto& i : m.imports) {
    std::string fns;
    for (size_t k = 0; k < i.functions.size(); ++k) {
      fns += std::format("{}{}", k ? "," : "", i.functions[k]);
      if (k == 23) {
        fns += ",...";
        break;
      }
    }
    std::printf("   import %-10s v%u.%-3u %2zu fns [%s]\n", i.library.name.c_str(), i.library.version >> 8,
                i.library.version & 0xFF, i.functions.size(), fns.c_str());
  }
  for (const auto& e : m.exports)
    std::printf("   export %-10s v%u.%-3u %zu fns\n", e.library.name.c_str(), e.library.version >> 8,
                e.library.version & 0xFF, e.functions.size());
  if (!m.unresolved.empty()) {
    std::map<std::string, int> counts;
    for (const auto& u : m.unresolved) ++counts[format_library(u.library)];
    std::string text;
    for (const auto& [lib, n] : counts) text += std::format("{}{} x{}", text.empty() ? "" : ", ", lib, n);
    std::printf("   unresolved: %s\n", text.c_str());
  }
}

struct CallSite {
  uint32_t pc = 0;
  uint32_t target = 0;
};

struct RunOutcome {
  bool exited = false;  // the thread called ExitThread/ExitDeleteThread
  bool stopped = false;
  std::string failure;
  uint64_t steps = 0;
};

RunOutcome run_one(IopCpu& cpu, IopMemory& mem, IopHle& hle, const Options& opt,
                   const std::map<uint32_t, std::string>& unresolved, const char* label) {
  constexpr uint32_t kStop = 0xFFFFFFF0;
  Interpreter interp;
  interp.set_syscall_hook([&hle](IopCpu& c, IopMemory&, uint32_t pc) {
    if (hle.dispatch(c, pc)) return;
    throw Trap(pc, "IOP syscall (no kernel syscall handler)");
  });

  struct Traced {
    uint32_t pc, word;
    std::string text;
  };
  std::vector<Traced> ring;
  ring.reserve(opt.trace + 1);
  std::optional<CallSite> last_call;
  RunOutcome out;
  std::printf("  %s entry 0x%05x\n", label, cpu.pc);
  while (cpu.pc != kStop) {
    if (out.steps >= opt.steps) {
      out.failure = "step budget exhausted";
      break;
    }
    const uint32_t pc = cpu.pc;
    uint32_t word = 0;
    std::string text;
    try {
      word = mem.read<uint32_t>(pc);
      const Insn in = decode(word, pc);
      text = disassemble(in);
      if (in.op == Op::JAL) last_call = CallSite{pc, in.target};
      if (in.op == Op::JALR) last_call = CallSite{pc, cpu.at(in.rs)};
      ring.push_back({pc, word, text});
      if (ring.size() > opt.trace) ring.erase(ring.begin());
      interp.step(cpu, mem);
    } catch (const ThreadExit&) {
      out.exited = true;
      break;
    } catch (const Trap& t) {
      out.failure = t.what();
      ring.push_back({pc, word, text.empty() ? "(invalid)" : text});
      if (ring.size() > opt.trace) ring.erase(ring.begin());
      auto it = unresolved.find(pc);
      if (it != unresolved.end()) out.failure = std::format("unimplemented import {}", it->second);
      if (last_call && last_call->target == pc) out.failure += std::format(" (called from 0x{:05x})", last_call->pc);
      break;
    }
    ++out.steps;
  }
  out.stopped = cpu.pc == kStop;
  if (out.failure.empty()) {
    std::printf("    %s after %llu instructions (v0=0x%08x)\n", out.exited ? "exited" : "returned",
                static_cast<unsigned long long>(out.steps), cpu.gpr[2]);
    return out;
  }
  std::printf("    stopped after %llu instructions: %s\n", static_cast<unsigned long long>(out.steps),
              out.failure.c_str());
  std::printf("    last instructions:\n");
  for (const auto& t : ring) std::printf("      0x%05x  %08x  %s\n", t.pc, t.word, t.text.c_str());
  std::printf("    registers: a0=%08x a1=%08x a2=%08x a3=%08x v0=%08x sp=%08x ra=%08x\n", cpu.gpr[4], cpu.gpr[5],
              cpu.gpr[6], cpu.gpr[7], cpu.gpr[2], cpu.gpr[29], cpu.gpr[31]);
  return out;
}

void probe(Placed& target, IopMemory& mem, IopHle& hle, ThreadTable& threads, const Options& opt) {
  auto& m = target.module;
  if (!m.has_entry()) {
    std::printf("\n%s has no initialiser\n", target.label.c_str());
    return;
  }

  // Make every unresolved import trap where it is called, so the report names it exactly.
  std::map<uint32_t, std::string> unresolved;
  for (const auto& u : m.unresolved) {
    unresolved[u.stub] = std::format("{}.{}", format_library(u.library), u.function);
    mem.write<uint32_t>(u.stub, 0x0000000D);  // break
  }

  IopCpu cpu;
  cpu.pc = m.entry;
  cpu.gpr[28] = m.gp;
  cpu.gpr[29] = 0x001FF000;  // stack below the top of IOP RAM
  cpu.gpr[31] = 0xFFFFFFF0;  // sentinel: returning here ends the run
  // loadcore calls the entry point with (argc, argv, module_ptr, module_info).
  cpu.gpr[4] = 0;
  cpu.gpr[5] = 0;
  cpu.gpr[6] = m.base;  // cur_module_addr points at the image; the probe passes the base itself
  cpu.gpr[7] = 0;

  std::printf("\nprobe %s: %llu step budget\n", target.label.c_str(), static_cast<unsigned long long>(opt.steps));
  RunOutcome main = run_one(cpu, mem, hle, opt, unresolved, "initialiser");
  if (!main.failure.empty()) return;

  // Run the bodies that StartThread made runnable. The vector may grow while they run.
  for (size_t i = 0; i < threads.threads.size(); ++i) {
    FakeThread& t = threads.threads[i];
    if (!t.started || t.ran) continue;
    t.ran = true;
    threads.current = t.id;
    IopCpu tcpu;
    tcpu.pc = t.entry;
    tcpu.gpr[28] = m.gp;
    tcpu.gpr[29] = (t.stack + t.stack_size) & ~15u;
    tcpu.gpr[31] = 0xFFFFFFF0;
    tcpu.gpr[4] = t.arg;
    const std::string label = std::format("thread {} (priority {})", t.id, t.priority);
    RunOutcome out = run_one(tcpu, mem, hle, opt, unresolved, label.c_str());
    if (!out.failure.empty()) break;
  }
  threads.current = 0;
}

bool matches(const std::string& haystack, const std::string& needle) {
  auto lower = [](std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  };
  return lower(haystack).find(lower(needle)) != std::string::npos;
}
}  // namespace

int main(int argc, char** argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--extracted" && i + 1 < argc)
      opt.extracted = argv[++i];
    else if (a == "--module" && i + 1 < argc)
      opt.module = argv[++i];
    else if (a == "--steps" && i + 1 < argc)
      opt.steps = std::strtoull(argv[++i], nullptr, 0);
    else if (a == "--trace" && i + 1 < argc)
      opt.trace = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 0));
    else if (a == "--inspect")
      opt.inspect = true;
    else if (a == "--romdir")
      opt.romdir = true;
    else if (a == "--mmio")
      opt.mmio = true;
    else if (a == "--help" || a == "-h") {
      std::printf(
          "usage: lantern-iop --extracted DIR [--inspect] [--romdir] [--mmio] [--module NAME] "
          "[--steps N] [--trace N]\n");
      return 0;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
      return 2;
    }
  }
  if (opt.extracted.empty()) {
    std::fprintf(stderr, "usage: lantern-iop --extracted DIR [--inspect] [--module NAME]\n");
    return 2;
  }

  IopMemory mem;
  MmioLog mmio;
  mem.set_mmio([&mmio](uint32_t a, void* o, unsigned n) { mmio.on_read(a, o, n); },
               [&mmio](uint32_t a, const void* i, unsigned n) { mmio.on_write(a, i, n); });
  IopHle hle(mem);
  Heap heap;
  ThreadTable threads;
  TimerTable timers;
  SemaTable semas;
  install_kernel_handlers(hle, heap, threads, timers, semas);
  auto sources = collect_sources(opt);
  if (sources.empty()) {
    std::fprintf(stderr, "no IOP modules found under %s\n", opt.extracted.string().c_str());
    return 1;
  }
  auto loaded = load_modules(sources, mem, hle, true);
  if (loaded.empty()) {
    std::fprintf(stderr, "no module loaded\n");
    return 1;
  }

  if (opt.inspect) {
    uint32_t modules = 0, imports = 0, unresolved = 0, ok = 0, bad = 0;
    for (const auto& p : loaded) {
      print_module(p);
      ++modules;
      for (const auto& i : p.module.imports) imports += static_cast<uint32_t>(i.functions.size());
      unresolved += static_cast<uint32_t>(p.module.unresolved.size());
      auto [o, b] = verify_calls(mem, p.module, loaded);
      ok += o;
      bad += b;
    }
    std::printf("\n%u modules, %u imported functions (%u unresolved), %u j/jal targets\n", modules, imports, unresolved,
                ok);
    if (bad) std::printf("WARNING: %u j/jal targets outside their module after relocation\n", bad);
    return bad ? 1 : 0;
  }

  Placed* target = nullptr;
  for (auto& p : loaded) {
    if (!opt.module.empty() ? (matches(p.label, opt.module) || matches(p.module.name, opt.module))
                            : p.module.has_entry()) {
      target = &p;
      break;
    }
  }
  if (!target) {
    std::fprintf(stderr, "no module matched '%s'\n", opt.module.c_str());
    return 1;
  }
  uint32_t max_end = 0;
  for (const auto& p : loaded) max_end = std::max(max_end, p.module.end);
  heap.next = std::max(heap.next, (max_end + 15) & ~15u);
  probe(*target, mem, hle, threads, opt);
  if (opt.mmio || opt.inspect) mmio.print();
  return 0;
}
