// lantern-boot: run the recompiled game headless on the HLE kernel and report how far it gets.
//   lantern-boot --extracted DIR --iso ISO [--game-lib PATH] [--seconds N] [--trace]
#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "analysis/ps2_syscalls.h"
#include "config/config.h"
#include "core/log.h"
#include "core/paths.h"
#include "core/png.h"
#include "ee/elf.h"
#include "ee/r5900.h"
#include "gs/gs.h"
#include "gs/rasterizer.h"
#include "input/input_system.h"
#include "iop/memcard.h"
#include "iop/services.h"
#include "iop/sif_hle.h"
#include "iso/iso9660.h"
#include "platform/sdl_platform.h"
#include "ps2/boot.h"
#include "ps2/hardware.h"
#include "ps2/kernel.h"
#include "ps2/vif.h"
#include "render/renderer.h"

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

iop::CdvdService* g_cdvd = nullptr;
gs::Gs* g_gs = nullptr;
iop::McService* g_mc = nullptr;
gs::Rasterizer* g_raster = nullptr;
ps2::Vif1* g_vif1 = nullptr;
iop::LoadfileService* g_loadfile = nullptr;

struct Report {
  ps2::Kernel* kernel = nullptr;
  ps2::Hardware* hw = nullptr;
  recomp::Runtime* rt = nullptr;
  iop::SifHle* sif = nullptr;
  ee::Memory* mem = nullptr;
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
      // Heuristic guest backtraces: stack words that point just after a JAL in the main image.
      for (const auto& t : kernel->thread_contexts()) {
        r += std::format("  thread {} sp=0x{:08x} ra=0x{:08x} stack return addresses:", t.id, t.sp, t.ra);
        int found = 0;
        for (uint32_t a = t.sp; a < t.sp + 0x1000 && found < 10 && mem; a += 4) {
          const uint8_t* p = mem->host(a, 4);
          if (!p) break;
          uint32_t v;
          std::memcpy(&v, p, 4);
          if (v < 0x00100008 || v >= 0x003E6880 || (v & 3)) continue;
          const uint8_t* q = mem->host(v - 8, 4);
          uint32_t w;
          std::memcpy(&w, q, 4);
          if ((w >> 26) != 3) continue;  // previous-but-one instruction must be JAL
          r += std::format(" 0x{:08x}", v);
          ++found;
        }
        r += "\n";
      }
    }
    if (rt)
      r += std::format("guest continuations (non-standard returns followed): {}\n", rt->stats().nonstandard_returns);
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
    if (g_gs) {
      const auto& g = g_gs->stats();
      r += std::format(
          "GS: {} image transfers ({} pixels), {} local copies, {} primitives, FINISH "
          "x{}\n",
          g.image_transfers, g.image_pixels, g.local_copies, g.primitives, g.finish);
      if (g_raster) {
        const auto& rs = g_raster->stats();
        r += "  texture samples by PSM:";
        for (const auto& [psm, n] : rs.texture_samples) r += std::format(" 0x{:02x}:{}", psm, n);
        r += "\n";
        r += std::format("  rasterized: {} triangles, {} sprites, {} lines, {} points; {} pixels written ({} tested)\n",
                         rs.triangles, rs.sprites, rs.lines, rs.points, rs.pixels_written, rs.pixels_tested);
      }
      for (const auto& [t, n] : g.kicks) r += std::format("  prim type {} vertex kicks x{}\n", t, n);
      for (const auto& [p, n] : g.unsupported_psm) r += std::format("  unsupported PSM 0x{:02x} x{}\n", p, n);
      r += std::format("  T8 order [{}] T4 order [{}]\n", gs::LocalMemory::bit_order_name(gs::kT8),
                       gs::LocalMemory::bit_order_name(gs::kT4));
      r += std::format("  PMODE 0x{:x} DISPFB2 0x{:x} DISPLAY2 0x{:x} SMODE2 0x{:x}\n", g_gs->priv(0), g_gs->priv(0x90),
                       g_gs->priv(0xA0), g_gs->priv(0x20));
    }
    if (g_vif1) {
      const auto& v = g_vif1->stats();
      r += std::format(
          "VIF1: {} unpacked qwords, {} DIRECT qwords, {} MPG pairs, {} VU1 starts (PLACEHOLDER: VU1 not executed)\n  "
          "codes:",
          v.unpacked_qwords, v.direct_qwords, v.mpg_pairs, v.vu1_starts);
      for (const auto& [c, n] : v.commands) r += std::format(" {:02x}:{}", c, n);
      r += "\n";
    }
    if (g_mc) {
      const auto& mcst = g_mc->stats();
      r += "memory card: commands";
      for (const auto& [fn, n] : mcst.commands) r += std::format(" 0x{:02x}:{}", fn, n);
      r += std::format("; opens {} reads {} writes {} getdirs {} formats {}\n", mcst.opens, mcst.reads, mcst.writes,
                       mcst.getdirs, mcst.formats);
      for (const auto& nm : mcst.opened) r += "  opened " + nm + "\n";
    }
    if (g_loadfile) {
      r += std::format("IOP modules requested ({}):\n", g_loadfile->modules().size());
      for (const auto& mname : g_loadfile->modules()) r += "  " + mname + "\n";
    }
    if (g_cdvd) {
      const auto& c = g_cdvd->stats();
      r += std::format("CD/DVD: {} reads, {} stream reads, {} sectors ({:.1f} MB); {} searches ({} misses)\n", c.reads,
                       c.stream_reads, c.sectors_read, static_cast<double>(c.sectors_read) * 2048.0 / 1048576.0,
                       c.searches, c.search_misses);
      for (const auto& f : c.searched) r += "  searched " + f + "\n";
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
  bool window = false;
  bool turbo = false;
  char mc_probe = 'A';
  uint64_t stop_frame = 0;             // stop after this many guest frames (0 = no limit)
  int swizzle_t8 = 0, swizzle_t4 = 0;  // indexed in-block orders under test (see gs/gs_memory.cpp)
  std::vector<std::pair<uint64_t, input::PadButton>> presses;  // scripted input: (vblank, button)
  fs::path dump_dir, texture_dir;
  uint64_t dump_every = 60;
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
    else if (a == "--swizzle-variant" && i + 1 < argc) {  // "T8" or "T8,T4"
      std::string spec = argv[++i];
      auto comma = spec.find(',');
      swizzle_t8 = std::stoi(spec.substr(0, comma));
      if (comma != std::string::npos) swizzle_t4 = std::stoi(spec.substr(comma + 1));
    } else if (a == "--window")
      window = true;
    else if (a == "--mc-probe" && i + 1 < argc)
      mc_probe = argv[++i][0];
    else if (a == "--frames" && i + 1 < argc)
      stop_frame = std::stoull(argv[++i]);
    else if (a == "--turbo")
      turbo = true;                             // one game frame per VBlank, unpaced (automated checks only)
    else if (a == "--press" && i + 1 < argc) {  // FRAME:button, e.g. 600:dpad_left (held 8 frames)
      std::string spec = argv[++i];
      auto colon = spec.find(':');
      auto b = colon == std::string::npos ? std::nullopt : input::pad_button_from_name(spec.substr(colon + 1));
      if (!b) {
        std::fprintf(stderr, "lantern-boot: bad --press '%s' (use FRAME:cross|circle|dpad_left|...)\n", spec.c_str());
        return 2;
      }
      presses.emplace_back(std::stoull(spec.substr(0, colon)), *b);
    } else if (a == "--dump-frames" && i + 1 < argc)
      dump_dir = argv[++i];
    else if (a == "--dump-textures" && i + 1 < argc)
      texture_dir = argv[++i];
    else if (a == "--dump-every" && i + 1 < argc)
      dump_every = std::stoull(argv[++i]);
    else {
      std::fputs(
          "usage: lantern-boot --extracted DIR --iso ISO [--game-lib PATH] [--seconds N] [--trace] [--window] "
          "[--dump-frames DIR "
          "[--dump-every N] [--dump-textures DIR] [--swizzle-variant N] [--turbo] [--frames N]\n",
          stderr);
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
  iop::MemoryCard card;
  const auto card_path = resolve_paths().memcard_dir() / "card1.ps2";
  if (!card.load(card_path.string())) {
    // No card yet: create and format one, the state a player has after inserting a new card.
    card.format(card_path.string());
    log::info("mc", "created and formatted a memory card at {}", card_path.string());
  }
  iop::PadService pad(*mem);

  // Graphics: GS + GIF paths, VIF1 + VU1 memory, DMA controller.
  gs::LocalMemory::set_indexed_variant(swizzle_t8, swizzle_t4);
  auto gs = std::make_unique<gs::Gs>();
  gs::Gif gif_path2(*gs), gif_path3(*gs);
  auto vu1 = std::make_unique<ps2::Vu1Memory>();
  ps2::Vif1 vif1(*vu1, gif_path2);
  hw.set_gs(gs.get());
  gs::Rasterizer raster;
  gs->set_rasterizer([&](gs::Gs& g, const gs::Vertex* v, int n, uint64_t prim) { raster.draw(g, v, n, prim); });
  g_raster = &raster;
  ps2::Dmac dmac(hw, [&](uint32_t addr, uint32_t size) { return mem->host(addr, size); });
  dmac.set_sink(1, [&](const uint8_t* q, uint32_t n) { vif1.feed(reinterpret_cast<const uint32_t*>(q), n * 4); });
  dmac.set_tag_sink(1, [&](const uint32_t* w, size_t n) { vif1.feed(w, n); });
  dmac.set_sink(2, [&](const uint8_t* q, uint32_t n) { gif_path3.process(q, n); });
  hw.set_dma_hook([&](int ch) { return dmac.run(ch); });
  g_gs = gs.get();
  g_vif1 = &vif1;

  recomp::Runtime rt;
  reg(rt);
  ps2::Kernel kernel(*cpu, *mem, rt, hw);
  kernel.set_trace(trace);
  kernel.set_turbo(turbo);
  hw.set_clock([&kernel] { return kernel.time(); });  // timers follow guest time (turbo or wall)
  // Optional window: SDL3 + Vulkan presentation of the GS display, input through InputSystem.
  Config cfg;
  if (auto cf = resolve_paths().config_file(); fs::exists(cf)) cfg = load_config(cf).config;
  input::InputSystem input(cfg.input);
  std::unique_ptr<platform::SdlPlatform> plat;
  std::unique_ptr<render::Renderer> renderer;
  if (window) {
    plat = platform::SdlPlatform::create(true, &err);
    if (!plat || !plat->create_window(cfg.graphics, "LANTERN - SLUS-20228 (experimental)", &err)) {
      std::fprintf(stderr, "lantern-boot: %s\n", err.c_str());
      return 1;
    }
    render::RendererCreateInfo rci;
    rci.graphics = cfg.graphics;
    rci.surface = plat.get();
    rci.cache_dir = resolve_paths().cache_dir;
    renderer = render::create_renderer(rci, &err);
    if (!renderer) {
      std::fprintf(stderr, "lantern-boot: %s\n", err.c_str());
      return 1;
    }
  }
  if (!dump_dir.empty()) fs::create_directories(dump_dir);
  if (!texture_dir.empty()) fs::create_directories(texture_dir);
  kernel.set_vblank_hook([&](uint64_t n) {
    if (stop_frame && n >= stop_frame) throw ps2::GuestExit{0};
    gs->on_vblank();
    if (plat) {
      auto ev = plat->pump(input);
      if (ev.quit) throw ps2::GuestExit{0};
      if (ev.resized) renderer->notify_resize();
      for (auto action : input.take_host_actions()) {
        if (action == input::HostAction::ToggleFullscreen) plat->toggle_fullscreen();
        if (action == input::HostAction::ToggleMouseCapture) plat->set_mouse_capture(!plat->mouse_captured());
        if (action == input::HostAction::Quit) throw ps2::GuestExit{0};
      }
    }
    auto padstate = input.sample();
    for (const auto& [at, b] : presses)
      if (n >= at && n < at + 8) {
        padstate.buttons = static_cast<uint16_t>(padstate.buttons | (1u << static_cast<unsigned>(b)));
        padstate.pressure[static_cast<size_t>(b)] = 255;
      }
    pad.update(padstate);
    if (renderer) {
      auto img = gs->display();
      if (img.width) renderer->set_frame_image(img.rgba.data(), img.width, img.height);
      if (renderer->begin_frame() == render::FrameStatus::Ready) {
        if (img.width) renderer->draw_frame_image();
        renderer->end_frame();
      }
    }
    if (!texture_dir.empty() && n == (stop_frame ? stop_frame - 1 : n) && g_raster) {
      // Dump every texture the game actually sampled, decoded with the current layout.
      int i = 0;
      for (const auto& [d, count] : g_raster->stats().textures) {
        if (d.psm != gs::kT4 && d.psm != gs::kT8 && d.psm != gs::kT4hl && d.psm != gs::kT8h) continue;
        auto img = gs->sample_texture(d.tbp, d.tbw, d.psm, d.tw, d.th);
        write_png(texture_dir / std::format("tex_{:02}_psm{:02x}_{}x{}_{}.png", i++, d.psm, d.tw, d.th, count),
                  img.width, img.height, img.rgba);
        if (i > 12) break;
      }
    }
    if (dump_dir.empty() || n % dump_every) return;
    auto img = gs->display();
    if (img.width == 0) return;
    write_png(dump_dir / std::format("frame_{:06}.png", n), img.width, img.height, img.rgba);
  });
  iop::SifHle sif(*mem);
  auto disc = iso::Image::open(iso_path, &err);
  if (!disc) {
    std::fprintf(stderr, "lantern-boot: %s\n", err.c_str());
    return 1;
  }
  iop::CdvdService cdvd(*mem, *disc);
  cdvd.register_with(sif);
  iop::IopHeapService iop_heap;
  iop_heap.register_with(sif);
  pad.register_with(sif);
  iop::McService mc(*mem, card);
  mc.set_probe_layout(mc_probe);
  mc.register_with(sif);
  g_mc = &mc;
  iop::LoadfileService loadfile;
  loadfile.register_with(sif);
  g_cdvd = &cdvd;
  g_loadfile = &loadfile;
  sif.set_reset_hook([&] { hw.iop_reset(); });
  kernel.set_sif(&sif);

  // Overlays (read-only from the disc) for overlay-window identification.
  ps2::OverlayTracker overlays;
  uint32_t ov_base = 0;
  for (const auto& s : elf->segments())
    if (s.type == 1 && s.filesz == 0 && (ov_base == 0 || s.vaddr < ov_base)) ov_base = s.vaddr;
  if (auto* iso = &*disc) {
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
    g_report = {&kernel, &hw, &rt, &sif, mem.get(), ""};
  }
  std::atomic<bool> done{false};
  std::thread watchdog([&] {
    if (seconds <= 0) return;  // --seconds 0: run until the window is closed or the game exits
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
