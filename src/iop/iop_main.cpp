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
#include "iop/iop_kernel.h"
#include "iop/iop_threads.h"
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

const char* state_name(ThreadState state) {
  switch (state) {
    case ThreadState::Dormant: return "dormant";
    case ThreadState::Ready: return "ready";
    case ThreadState::Running: return "running";
    case ThreadState::Delay: return "delay";
    case ThreadState::Sleep: return "sleep";
    case ThreadState::Sema: return "sema";
    case ThreadState::Vblank: return "vblank";
    case ThreadState::Dead: return "dead";
  }
  return "?";
}

void probe(Placed& target, IopMemory& mem, IopScheduler& sched, const Options& opt) {
  auto& m = target.module;
  if (!m.has_entry()) {
    std::printf("\n%s has no initialiser\n", target.label.c_str());
    return;
  }

  // Unresolved imports become `break`, so the trap lands on the stub the module called.
  std::map<uint32_t, std::string> unresolved;
  for (const auto& u : m.unresolved) {
    unresolved[u.stub] = std::format("{}.{}", format_library(u.library), u.function);
    mem.write<uint32_t>(u.stub, 0x0000000D);
  }

  struct Traced {
    uint32_t thread, pc, word;
  };
  std::vector<Traced> ring;
  ring.reserve(opt.trace + 1);
  sched.set_observer([&](const IopThread& t, uint32_t pc, uint32_t word) {
    ring.push_back({t.id, pc, word});
    if (ring.size() > opt.trace) ring.erase(ring.begin());
  });

  // The module initialiser runs as the first thread. loadcore calls it with
  // (argc=0, argv=0, module_ptr, module_info=0).
  const uint32_t main_id = sched.create_thread(m.entry, 0, 64, 0x001F1000, 0x0000E000, m.gp);
  IopThread* mt = sched.thread(main_id);
  mt->cpu.gpr[5] = 0;
  mt->cpu.gpr[6] = m.base;
  mt->cpu.gpr[7] = 0;
  sched.start_thread(main_id, 0);

  std::printf("\nprobe %s: %llu step budget\n", target.label.c_str(), static_cast<unsigned long long>(opt.steps));
  const SchedulerStats stats = sched.run(opt.steps);

  std::printf("  %llu steps, %llu cycles (%.1f ms), %u switches%s\n", static_cast<unsigned long long>(stats.steps),
              static_cast<unsigned long long>(stats.cycles), static_cast<double>(stats.cycles) / 36864.0,
              stats.switches,
              stats.trapped            ? ", trapped"
              : stats.deadlock         ? ", deadlock"
              : stats.budget_exhausted ? ", budget exhausted"
                                       : "");
  for (const auto& t : sched.threads()) {
    std::printf("  thread %u prio %d: %-8s %llu instructions", t.id, t.priority, state_name(t.state),
                static_cast<unsigned long long>(t.steps));
    if (t.state == ThreadState::Sema || t.state == ThreadState::Vblank) std::printf(" (object %u)", t.wait_object);
    if (!t.trap.empty()) std::printf(" trap 0x%05x: %s", t.trap_pc, t.trap.c_str());
    std::printf("\n");
  }

  const bool show_trace = stats.trapped || stats.budget_exhausted;
  if (stats.trapped) {
    std::printf("  failure in thread %u at 0x%05x: %s\n", stats.trap_thread, stats.trap_pc, stats.trap.c_str());
    auto it = unresolved.find(stats.trap_pc);
    if (it != unresolved.end()) std::printf("  unimplemented import %s\n", it->second.c_str());
    if (IopThread* ft = sched.thread(stats.trap_thread))
      std::printf("  registers: a0=%08x a1=%08x a2=%08x a3=%08x v0=%08x sp=%08x ra=%08x\n", ft->cpu.gpr[4],
                  ft->cpu.gpr[5], ft->cpu.gpr[6], ft->cpu.gpr[7], ft->cpu.gpr[2], ft->cpu.gpr[29], ft->cpu.gpr[31]);
  }
  if (stats.deadlock) {
    std::printf("  deadlock: no ready thread and no timed wake-up\n");
    for (const auto& t : sched.threads())
      if (t.state != ThreadState::Dead && t.state != ThreadState::Dormant)
        std::printf("    thread %u is %s (sema/vblank object %u)\n", t.id, state_name(t.state), t.wait_object);
  }
  if (show_trace) {
    std::printf("  last instructions:\n");
    for (const auto& tr : ring) {
      const Insn in = decode(tr.word, tr.pc);
      const std::string text = in.valid() ? disassemble(in) : "(invalid)";
      std::printf("    [t%u] 0x%05x  %08x  %s\n", tr.thread, tr.pc, tr.word, text.c_str());
    }
  }
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
  Interpreter interp;
  IopScheduler sched(mem, interp);
  IopKernel kernel(hle, sched);
  kernel.install();
  interp.set_syscall_hook([&hle](IopCpu& c, IopMemory&, uint32_t pc) {
    if (hle.dispatch(c, pc)) return;
    throw Trap(pc, "IOP syscall (no kernel syscall handler)");
  });
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
  kernel.heap.next = std::max(kernel.heap.next, (max_end + 15) & ~15u);
  kernel.module_gp = target->module.gp;
  probe(*target, mem, sched, opt);
  if (opt.mmio || opt.inspect) mmio.print();
  return 0;
}
