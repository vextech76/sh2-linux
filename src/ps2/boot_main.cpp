// lantern-boot: run the recompiled game headless on the HLE kernel and report how far it gets.
//   lantern-boot --extracted DIR --iso ISO [--game-lib PATH] [--seconds N] [--trace]
#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "analysis/ps2_syscalls.h"
#include "core/log.h"
#include "core/paths.h"
#include "ee/elf.h"
#include "ee/r5900.h"
#include "iop/sif_hle.h"
#include "iso/iso9660.h"
#include "ps2/boot.h"
#include "ps2/hardware.h"
#include "ps2/kernel.h"

namespace fs = std::filesystem;
using namespace lantern;

namespace {
std::string manifest_value(const fs::path& manifest, const std::string& key) {
  std::ifstream in(manifest);
  std::string line;
  while (std::getline(in, line))
    if (line.rfind(key + " = \"", 0) == 0) {
      auto start = key.size() + 4;
      return line.substr(start, line.find('"', start) - start);
    }
  return "";
}

struct Report {
  ps2::Kernel* kernel = nullptr;
  ps2::Hardware* hw = nullptr;
  recomp::Runtime* rt = nullptr;
  iop::SifHle* sif = nullptr;
  std::string outcome;

  std::string render() const {
    std::string r = "\n===== LANTERN boot report =====\n" + outcome + "\n";
    if (kernel) {
      const auto& s = kernel->stats();
      r += std::format("vblanks {}  interrupts {}  context switches {}  alarms {}  threads created {}\n", s.vblanks,
                       s.interrupts_delivered, s.context_switches, s.alarms_fired, s.threads_created);
      r += std::format("last syscall {} at 0x{:08x}\nsyscalls used:\n", analysis::ee_syscall_name(s.last_syscall),
                       s.last_syscall_pc);
      for (const auto& [n, count] : s.syscalls) {
        auto name = analysis::ee_syscall_name(n);
        r += std::format("  {:<24} x{}\n", name.empty() ? std::format("#{}", n) : std::string(name), count);
      }
      r += "threads:\n" + kernel->describe_threads();
    }
    if (rt)
      r += std::format("calls: native {}  interpreted {} ({} interpreted instructions)\n", rt->stats().native_calls,
                       rt->stats().interpreted_calls, rt->stats().interpreted_steps);
    if (hw) {
      std::map<std::string, std::pair<uint64_t, uint64_t>> regions;
      for (const auto& [a, acc] : hw->unmodelled()) {
        auto& e = regions[ps2::Hardware::region_name(a)];
        e.first += acc.reads;
        e.second += acc.writes;
      }
      r += "unmodelled hardware touched (reads/writes):\n";
      for (const auto& [name, rw] : regions) r += std::format("  {:<16} {}/{}\n", name, rw.first, rw.second);
      for (const auto& [ch, n] : hw->dma_starts())
        r += std::format("  DMA channel {} started {} times (PLACEHOLDER)\n", ch, n);
    }
    if (sif) {
      const auto& st = sif->stats();
      r += std::format("SIF: {} IOP->EE replies; commands:", st.replies);
      for (const auto& [cid, n] : st.commands) r += std::format(" {} x{}", iop::sif_command_name(cid), n);
      r += "\nRPC servers bound:\n";
      for (const auto& [sid, n] : st.binds) r += std::format("  {} (bind x{})\n", sif->server_name(sid), n);
      r += "RPC calls (server, function):\n";
      for (const auto& [k, n] : st.calls) r += std::format("  {} fn {} x{}\n", sif->server_name(k.first), k.second, n);
    }
    return r;
  }
};
Report g_report;
std::mutex g_report_mutex;
}  // namespace

int main(int argc, char** argv) {
  fs::path extracted, iso_path, game_lib;
  double seconds = 20;
  bool trace = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--extracted" && i + 1 < argc)
      extracted = argv[++i];
    else if (a == "--iso" && i + 1 < argc)
      iso_path = argv[++i];
    else if (a == "--game-lib" && i + 1 < argc)
      game_lib = argv[++i];
    else if (a == "--seconds" && i + 1 < argc)
      seconds = std::stod(argv[++i]);
    else if (a == "--trace")
      trace = true;
    else {
      std::fputs("usage: lantern-boot --extracted DIR --iso ISO [--game-lib PATH] [--seconds N] [--trace]\n", stderr);
      return 2;
    }
  }
  if (trace) log::set_min_level(log::Level::Debug);
  const auto manifest = extracted / "manifest.toml";
  const std::string serial = manifest_value(manifest, "serial"), boot = manifest_value(manifest, "boot_file");
  if (serial.empty()) {
    std::fputs("lantern-boot: cannot read manifest.toml (run lantern-ingest extract)\n", stderr);
    return 1;
  }
  if (game_lib.empty()) game_lib = resolve_paths().data_dir / "recomp" / serial / "build" / "liblantern_game.so";

  std::string err;
  std::ifstream in(extracted / boot, std::ios::binary);
  auto elf = ee::ElfImage::parse(std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), {}), &err);
  if (!elf) {
    std::fprintf(stderr, "lantern-boot: %s\n", err.c_str());
    return 1;
  }

  void* lib = dlopen(game_lib.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!lib) {
    std::fprintf(stderr, "lantern-boot: cannot load recompiled game '%s': %s\n(build it with lantern-recomp + cmake)\n",
                 game_lib.c_str(), dlerror());
    return 1;
  }
  using RegisterFn = void (*)(recomp::Runtime&);
  auto reg = reinterpret_cast<RegisterFn>(dlsym(lib, "lantern_register_game"));
  if (!reg) {
    std::fprintf(stderr, "lantern-boot: %s has no lantern_register_game\n", game_lib.c_str());
    return 1;
  }

  auto mem = std::make_unique<ee::Memory>();
  auto cpu = std::make_unique<ee::Cpu>();
  ps2::Hardware hw;
  mem->set_mmio(
      [&](uint32_t a, void* out, unsigned size) {
        if (!hw.handles(a)) throw ee::GuestTrap(cpu->pc, std::format("read from unmapped address 0x{:08x}", a));
        hw.read(a, out, size);
      },
      [&](uint32_t a, const void* data, unsigned size) {
        if (!hw.handles(a)) throw ee::GuestTrap(cpu->pc, std::format("write to unmapped address 0x{:08x}", a));
        hw.write(a, data, size);
      });
  ps2::load_elf(*mem, *elf);

  recomp::Runtime rt;
  reg(rt);
  ps2::Kernel kernel(*cpu, *mem, rt, hw);
  kernel.set_trace(trace);
  iop::SifHle sif(*mem);
  sif.set_reset_hook([&] { hw.iop_reset(); });
  kernel.set_sif(&sif);

  // Overlays (read-only from the disc) for overlay-window identification.
  ps2::OverlayTracker overlays;
  uint32_t ov_base = 0;
  for (const auto& s : elf->segments())
    if (s.type == 1 && s.filesz == 0 && (ov_base == 0 || s.vaddr < ov_base)) ov_base = s.vaddr;
  if (auto iso = iso::Image::open(iso_path, &err)) {
    std::vector<std::pair<std::string, iso::Entry>> files;
    iso->walk([&](const std::string& p, const iso::Entry& e) {
      if (!e.is_dir && (p.starts_with("/GX/") || p.starts_with("/GY/") || p.starts_with("/GZ/")) && p.ends_with(".BIN"))
        files.emplace_back(p, e);
      return true;
    });
    std::sort(files.begin(), files.end(), [](auto& a, auto& b) { return a.first < b.first; });
    for (size_t id = 0; id < files.size(); ++id)
      if (auto data = iso->read_all(files[id].second, 4u << 20))
        overlays.add(static_cast<int>(id), files[id].first, *data);
  } else {
    log::warn("boot", "ISO not available ({}); overlay switching disabled", err);
  }
  kernel.set_code_modified_hook([&] {
    const int id = overlays.identify(*mem, ov_base);
    rt.set_active_overlay(id);
    log::info("boot", "instruction cache flushed: overlay window holds {}", overlays.name(id));
  });

  {
    std::lock_guard lock(g_report_mutex);
    g_report = {&kernel, &hw, &rt, &sif, ""};
  }
  std::atomic<bool> done{false};
  std::thread watchdog([&] {
    for (double t = 0; t < seconds && !done; t += 0.1) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (done) return;
    // The guest is still running (or spinning without safe points): report and stop the process.
    std::string r;
    {
      std::lock_guard lock(g_report_mutex);
      g_report.outcome = std::format("STILL RUNNING after {}s (watchdog stop)", seconds);
      r = g_report.render();
    }
    std::fputs(r.c_str(), stdout);
    std::fflush(stdout);
    _exit(3);
  });

  int code = 0;
  std::string outcome;
  try {
    code = kernel.run(elf->entry(), 0x01FFFF00);
    outcome = std::format("program exited with code {}", code);
  } catch (const ee::GuestTrap& t) {
    outcome = std::format("TRAP at guest pc 0x{:08x}: {}", t.pc, t.what());
    code = 4;
  }
  done = true;
  watchdog.join();
  std::lock_guard lock(g_report_mutex);
  g_report.outcome = outcome;
  std::fputs(g_report.render().c_str(), stdout);
  return code;
}
