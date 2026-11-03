// lantern-recomp: recompile the boot ELF (and overlays from the ISO) to C++.
// Output goes to $XDG_DATA_HOME/lantern/recomp/<serial>/ (derived from the user's disc; never
// commit it). A CMakeLists.txt is generated so the output builds as a static library.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "analysis/code_analysis.h"
#include "core/paths.h"
#include "ee/elf.h"
#include "iso/iso9660.h"
#include "recomp/emitter.h"

namespace fs = std::filesystem;
using namespace lantern;

namespace {
std::vector<uint8_t> read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), {});
}
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

void write_module(const fs::path& dir, const recomp::EmittedModule& mod, std::vector<std::string>& sources) {
  fs::create_directories(dir);
  std::ofstream(dir / "module.h") << mod.header;
  for (size_t i = 0; i < mod.sources.size(); ++i) {
    auto name = std::format("part_{:03}.cpp", i);
    std::ofstream(dir / name) << mod.sources[i];
    sources.push_back((dir.filename() / name).string());
  }
}

void merge(recomp::EmitStats& into, const recomp::EmitStats& s) {
  into.functions += s.functions;
  into.instructions += s.instructions;
  into.unimplemented += s.unimplemented;
  into.indirect_jumps += s.indirect_jumps;
  into.jump_tables += s.jump_tables;
  for (auto& [k, v] : s.missing_ops) into.missing_ops[k] += v;
}
}  // namespace

int main(int argc, char** argv) {
  fs::path extracted, iso_path, out_dir;
  fs::path lantern_src = LANTERN_SOURCE_DIR;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--extracted" && i + 1 < argc)
      extracted = argv[++i];
    else if (a == "--iso" && i + 1 < argc)
      iso_path = argv[++i];
    else if (a == "--out" && i + 1 < argc)
      out_dir = argv[++i];
    else {
      std::fputs("usage: lantern-recomp --extracted DIR [--iso ISO] [--out DIR]\n", stderr);
      return 2;
    }
  }
  if (extracted.empty()) {
    std::fputs("lantern-recomp: --extracted is required\n", stderr);
    return 2;
  }
  const auto manifest = extracted / "manifest.toml";
  const std::string serial = manifest_value(manifest, "serial"), boot = manifest_value(manifest, "boot_file");
  if (serial.empty()) {
    std::fputs("lantern-recomp: cannot read manifest.toml\n", stderr);
    return 1;
  }
  if (out_dir.empty()) out_dir = resolve_paths().data_dir / "recomp" / serial;
  if (!iso_path.empty() && path_is_within(out_dir, fs::absolute(iso_path).parent_path())) {
    std::fputs("lantern-recomp: refusing to write inside the game data directory\n", stderr);
    return 2;
  }
  std::string err;
  auto elf = ee::ElfImage::parse(read_file(extracted / boot), &err);
  if (!elf) {
    std::fprintf(stderr, "lantern-recomp: %s\n", err.c_str());
    return 1;
  }
  const ee::ElfSegment* main = elf->main_segment();
  auto img = analysis::CodeImage::from_bytes(main->vaddr, elf->segment_data(*main));
  auto ca = analysis::analyze_code(img, {{elf->entry()}, true, true});

  fs::create_directories(out_dir);
  std::vector<std::string> sources;
  recomp::EmitStats total;
  recomp::EmitOptions opt;
  opt.ns = "sh2_main";
  opt.register_fn = "register_main";
  auto mod = recomp::emit_module(img, ca, opt);
  write_module(out_dir / "main", mod, sources);
  merge(total, mod.stats);
  std::printf("main: %llu functions, %llu instructions, %llu unimplemented sites\n",
              static_cast<unsigned long long>(mod.stats.functions),
              static_cast<unsigned long long>(mod.stats.instructions),
              static_cast<unsigned long long>(mod.stats.unimplemented));

  std::string overlay_map = "# overlay id -> disc file (derived; do not distribute)\n";
  std::string reg_overlays;
  uint32_t ov_base = 0, ov_end = 0;
  for (const auto& s : elf->segments())
    if (s.type == 1 && s.filesz == 0 && (ov_base == 0 || s.vaddr == ov_base)) {
      ov_base = s.vaddr;
      ov_end = std::max(ov_end, s.vaddr + s.memsz);
    }
  if (!iso_path.empty() && ov_base) {
    auto iso = iso::Image::open(iso_path, &err);
    if (!iso) {
      std::fprintf(stderr, "lantern-recomp: %s\n", err.c_str());
      return 1;
    }
    std::vector<std::pair<std::string, iso::Entry>> files;
    iso->walk([&](const std::string& path, const iso::Entry& e) {
      bool dir_ok = path.starts_with("/GX/") || path.starts_with("/GY/") || path.starts_with("/GZ/");
      if (!e.is_dir && dir_ok && path.ends_with(".BIN")) files.emplace_back(path, e);
      return true;
    });
    std::sort(files.begin(), files.end(), [](auto& a, auto& b) { return a.first < b.first; });
    for (size_t id = 0; id < files.size(); ++id) {
      auto data = iso->read_all(files[id].second, 4u << 20);
      if (!data) continue;
      auto ov = analysis::CodeImage::from_bytes(ov_base, *data);
      auto oa = analysis::analyze_code(ov, {{ov_base}, true, true});
      recomp::EmitOptions o;
      o.ns = std::format("sh2_ov{}", id);
      o.register_fn = "register_overlay";
      o.overlay = static_cast<int>(id);
      auto om = recomp::emit_module(ov, oa, o);
      write_module(out_dir / std::format("ov{:02}", id), om, sources);
      merge(total, om.stats);
      overlay_map += std::format("{} = \"{}\"\n", id, files[id].first);
      reg_overlays += std::format("namespace sh2_ov{} {{ void register_overlay(lantern::recomp::Runtime&); }}\n", id);
    }
    std::printf("overlays: %zu recompiled\n", files.size());
  }
  std::ofstream(out_dir / "overlays.toml") << overlay_map;

  // Module entry point.
  std::string reg =
      "// Generated by LANTERN.\n#include \"recomp/runtime.h\"\n"
      "namespace sh2_main { void register_main(lantern::recomp::Runtime&); }\n" +
      reg_overlays + "void lantern_register_game(lantern::recomp::Runtime& rt) {\n  sh2_main::register_main(rt);\n";
  reg += std::format("  rt.set_overlay_window(0x{:08x}u, 0x{:08x}u);\n", ov_base, ov_end);
  for (size_t id = 0; !reg_overlays.empty() && reg_overlays.find(std::format("sh2_ov{} ", id)) != std::string::npos;
       ++id)
    reg += std::format("  sh2_ov{}::register_overlay(rt);\n", id);
  reg += "}\n";
  std::ofstream(out_dir / "register_game.cpp") << reg;
  sources.push_back("register_game.cpp");

  std::string cml = std::format(
      "# Generated by LANTERN: recompiled {} (derived from the user's disc; never distribute).\n"
      "cmake_minimum_required(VERSION 3.25)\nproject(lantern_game_{} CXX)\nset(CMAKE_CXX_STANDARD 20)\n"
      "add_library(lantern_game STATIC\n",
      serial, serial == "" ? "x" : std::string(1, 'g'));
  for (const auto& s : sources) cml += "  " + s + "\n";
  cml += std::format(
      ")\ntarget_include_directories(lantern_game PRIVATE \"{}\")\n"
      "target_compile_options(lantern_game PRIVATE -O2 -ffp-contract=off -fno-strict-aliasing -Wno-unused-label)\n",
      (lantern_src / "src").string());
  std::ofstream(out_dir / "CMakeLists.txt") << cml;

  std::printf(
      "total: %llu functions, %llu instructions; unimplemented sites %llu (%.2f%%); "
      "runtime-dispatched JR %llu; jump-table switches %llu\n",
      static_cast<unsigned long long>(total.functions), static_cast<unsigned long long>(total.instructions),
      static_cast<unsigned long long>(total.unimplemented),
      100.0 * static_cast<double>(total.unimplemented) / static_cast<double>(std::max<uint64_t>(1, total.instructions)),
      static_cast<unsigned long long>(total.indirect_jumps), static_cast<unsigned long long>(total.jump_tables));
  std::vector<std::pair<uint64_t, std::string>> missing;
  for (auto& [k, v] : total.missing_ops) missing.emplace_back(v, k);
  std::sort(missing.rbegin(), missing.rend());
  std::printf("missing semantics (%zu distinct):\n", missing.size());
  for (auto& [v, k] : missing) std::printf("  %-18s %llu\n", k.c_str(), static_cast<unsigned long long>(v));
  std::printf("wrote %s\n", out_dir.c_str());
  return 0;
}
