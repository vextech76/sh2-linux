// lantern-analyze: static inventory of the EE boot ELF, overlays and IOP modules.
// Reads the extracted boot set (and optionally the ISO, read-only, for overlay files).
// Writes derived data only under $XDG_DATA_HOME/lantern/analysis/<serial>/.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "analysis/code_analysis.h"
#include "analysis/iop.h"
#include "analysis/ps2_syscalls.h"
#include "analysis/vif.h"
#include "core/paths.h"
#include "ee/elf.h"
#include "iso/iso9660.h"

namespace fs = std::filesystem;
using namespace lantern;
using namespace lantern::analysis;

namespace {
std::vector<uint8_t> read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), {});
}

std::string manifest_value(const fs::path& manifest, const std::string& key) {
  std::ifstream in(manifest);
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind(key + " = \"", 0) == 0) {
      auto start = key.size() + 4;
      return line.substr(start, line.find('"', start) - start);
    }
  }
  return "";
}

struct Report {
  std::string text;
  template <typename... A>
  void line(std::format_string<A...> f, A&&... a) {
    text += std::format(f, std::forward<A>(a)...);
    text += '\n';
  }
};

void strings_with(std::span<const uint8_t> data, uint32_t base, const std::vector<std::string>& needles,
                  std::vector<std::pair<uint32_t, std::string>>& out) {
  size_t i = 0;
  while (i < data.size()) {
    size_t j = i;
    while (j < data.size() && data[j] >= 0x20 && data[j] < 0x7F) ++j;
    if (j - i >= 5 && j < data.size() && data[j] == 0) {
      std::string s(reinterpret_cast<const char*>(&data[i]), j - i);
      std::string up = s;
      for (auto& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      for (const auto& n : needles)
        if (up.find(n) != std::string::npos) {
          out.emplace_back(base + static_cast<uint32_t>(i), s);
          break;
        }
    }
    i = j + 1;
  }
}
}  // namespace

int main(int argc, char** argv) {
  fs::path extracted, iso_path, out_dir;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--extracted" && i + 1 < argc)
      extracted = argv[++i];
    else if (a == "--iso" && i + 1 < argc)
      iso_path = argv[++i];
    else if (a == "--out" && i + 1 < argc)
      out_dir = argv[++i];
    else {
      std::fputs("usage: lantern-analyze --extracted DIR [--iso ISO] [--out DIR]\n", stderr);
      return 2;
    }
  }
  if (extracted.empty()) {
    std::fputs("lantern-analyze: --extracted is required (see lantern-ingest extract)\n", stderr);
    return 2;
  }
  const fs::path manifest = extracted / "manifest.toml";
  const std::string serial = manifest_value(manifest, "serial");
  const std::string boot = manifest_value(manifest, "boot_file");
  if (serial.empty() || boot.empty()) {
    std::fprintf(stderr, "lantern-analyze: cannot read serial/boot_file from %s\n", manifest.c_str());
    return 1;
  }
  if (out_dir.empty()) out_dir = resolve_paths().data_dir / "analysis" / serial;
  if (!iso_path.empty() && path_is_within(out_dir, fs::absolute(iso_path).parent_path())) {
    std::fputs("lantern-analyze: refusing to write inside the game data directory\n", stderr);
    return 2;
  }

  std::string err;
  auto elf = ee::ElfImage::parse(read_file(extracted / boot), &err);
  if (!elf) {
    std::fprintf(stderr, "lantern-analyze: %s\n", err.c_str());
    return 1;
  }
  Report r;
  r.line("# LANTERN static analysis: {} ({})", serial, boot);
  r.line("Derived from the owner's disc. Local use only; do not distribute.\n");
  r.line("## ELF");
  r.line("entry 0x{:08x}  flags 0x{:08x}  segments {}  sections {}", elf->entry(), elf->flags(), elf->segments().size(),
         elf->sections().size());

  const ee::ElfSegment* main = elf->main_segment();
  std::map<uint32_t, std::vector<uint32_t>> overlay_sizes;  // vaddr -> memsz list
  for (const auto& s : elf->segments())
    if (s.type == 1 && s.filesz == 0) overlay_sizes[s.vaddr].push_back(s.memsz);
  r.line("main image 0x{:08x}..0x{:08x} (file {} bytes, mem {} bytes)", main->vaddr, main->vaddr + main->filesz,
         main->filesz, main->memsz);
  for (const auto& [va, sizes] : overlay_sizes)
    r.line("overlay region 0x{:08x}: {} zero-file segments, memsz {}..{} bytes", va, sizes.size(),
           *std::min_element(sizes.begin(), sizes.end()), *std::max_element(sizes.begin(), sizes.end()));

  // --- main image code analysis
  auto img = CodeImage::from_bytes(main->vaddr, elf->segment_data(*main));
  auto ca = analyze_code(img, {{elf->entry()}, true, true});
  uint32_t from_ptr = 0, leaf = 0, jt_targets = 0;
  for (const auto& f : ca.functions) {
    from_ptr += f.from_pointer;
    leaf += f.calls == 0 && f.indirect_calls == 0;
  }
  for (const auto& jt : ca.jump_tables) jt_targets += static_cast<uint32_t>(jt.targets.size());
  uint32_t jalr = 0;
  for (const auto& f : ca.functions) jalr += f.indirect_calls;
  r.line("\n## Main image control flow");
  r.line("words {}  valid-encoding ratio {:.1f}%", ca.total_words, 100.0 * valid_word_ratio(img));
  r.line("functions {} (via calls/entry {}, via address constants/prologues {}), leaf {}", ca.functions.size(),
         ca.functions.size() - from_ptr, from_ptr, leaf);
  r.line("reached {} words = {:.1f}% of image; invalid encodings on reached paths: {}", ca.reached_words,
         100.0 * ca.reached_words / std::max(1u, ca.total_words), ca.invalid_reached);
  r.line("jump tables resolved {} ({} targets), unresolved JR {}, indirect calls (JALR) {}", ca.jump_tables.size(),
         jt_targets, ca.unresolved_jr.size(), jalr);
  r.line("direct calls to addresses outside the main image: {} unique targets", ca.external_calls.size());
  uint32_t big_gaps = 0, gap_words = 0;
  for (auto [s, e] : ca.unreached) {
    gap_words += (e - s) / 4;
    if (e - s >= 4096) ++big_gaps;
  }
  r.line("unreached ranges {} ({} words; {} ranges >= 4 KB, likely data)", ca.unreached.size(), gap_words, big_gaps);

  // --- syscalls
  std::map<int, uint32_t> sys_sites;
  uint32_t unknown_sys = 0;
  for (const auto& s : ca.syscalls)
    if (s.number)
      ++sys_sites[*s.number];
    else
      ++unknown_sys;
  r.line("\n## EE kernel syscalls ({} sites, {} distinct numbers, {} with unknown number)", ca.syscalls.size(),
         sys_sites.size(), unknown_sys);
  for (const auto& [n, c] : sys_sites) {
    auto name = ee_syscall_name(n);
    r.line("  {:>6}  {:<26} sites {}", n < 0 ? std::format("-0x{:x}", -n) : std::format("0x{:x}", n),
           name.empty() ? "(unknown)" : name, c);
  }

  // --- VU microcode (VIF MPG) inside the main image
  auto mpgs = scan_vif_mpg(elf->segment_data(*main));
  uint32_t mpg_pairs = 0;
  for (const auto& m : mpgs) mpg_pairs += m.count;
  r.line("\n## VU microcode (heuristic VIF MPG scan of the main image)");
  r.line("MPG blocks {}  micro-instruction pairs {} ({} bytes)", mpgs.size(), mpg_pairs, mpg_pairs * 8);

  // --- strings that look like file references
  std::vector<std::pair<uint32_t, std::string>> refs;
  strings_with(elf->segment_data(*main), main->vaddr, {".BIN", ".IRX", ".IMG", ".MGF", ".DAT", "CDROM", "HOST"}, refs);
  r.line("\n## File-reference strings in the main image ({} found)", refs.size());
  for (size_t i = 0; i < refs.size() && i < 60; ++i) r.line("  0x{:08x}  {}", refs[i].first, refs[i].second);

  // --- overlays from the disc
  std::string overlay_toml;
  if (!iso_path.empty() && !overlay_sizes.empty()) {
    auto iso = iso::Image::open(iso_path, &err);
    if (!iso) {
      std::fprintf(stderr, "lantern-analyze: %s\n", err.c_str());
      return 1;
    }
    const uint32_t ov_base = overlay_sizes.begin()->first;
    const auto& sizes = overlay_sizes.begin()->second;
    std::set<uint32_t> main_starts;
    for (const auto& f : ca.functions) main_starts.insert(f.start);
    r.line("\n## Overlay candidates (*.BIN under GX/GY/GZ) decoded at 0x{:08x}", ov_base);
    r.line("  {:<16} {:>7} {:>7} {:>6} {:>9} {:>11} {:>9} {:>7}", "file", "bytes", "valid%", "fns", "jal-in",
           "jal->main", "jal-other", "fit");
    uint32_t files = 0, fit_count = 0, main_hits_total = 0, main_calls_total = 0;
    iso->walk([&](const std::string& path, const iso::Entry& e) {
      if (e.is_dir) return true;
      bool in_dir = path.starts_with("/GX/") || path.starts_with("/GY/") || path.starts_with("/GZ/");
      if (!in_dir || !path.ends_with(".BIN")) return true;
      auto data = iso->read_all(e, 4u << 20);
      if (!data) return true;
      ++files;
      auto ov = CodeImage::from_bytes(ov_base, *data);
      auto js = sweep_jal_targets(ov);
      uint32_t to_main = 0;
      for (uint32_t t : js.outside_targets) to_main += main_starts.contains(t);
      main_hits_total += to_main;
      main_calls_total += js.outside;
      auto oa = analyze_code(ov, {{ov_base}, true, true});
      // Overlay segment that can hold this file: memsz >= size, smallest slack.
      uint32_t best = 0;
      for (uint32_t m : sizes)
        if (m >= e.size && (best == 0 || m < best)) best = m;
      bool fits = best != 0 && best - e.size <= 0x400;
      fit_count += fits;
      r.line("  {:<16} {:>7} {:>6.1f}% {:>6} {:>4}/{:<4} {:>11} {:>9} {:>7}", path, e.size,
             100.0 * valid_word_ratio(ov), oa.functions.size(), js.inside_prologue, js.inside, to_main,
             js.outside - to_main, fits ? std::format("+{}", best - e.size) : "none");
      overlay_toml += std::format("[[overlay]]\nfile = \"{}\"\nsize = {}\nbase = 0x{:08x}\nfunctions = {}\n\n", path,
                                  e.size, ov_base, oa.functions.size());
      return true;
    });
    r.line("overlay files {}; fitting an ELF overlay segment (slack <= 1 KB): {}", files, fit_count);
    r.line("JAL from overlays to outside the overlay: {}, landing exactly on a main-image function start: {}",
           main_calls_total, main_hits_total);
  }

  // --- IOP modules
  r.line("\n## IOP modules");
  for (const auto& entry : fs::recursive_directory_iterator(extracted)) {
    if (!entry.is_regular_file()) continue;
    std::string ext = entry.path().extension().string();
    for (auto& c : ext) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (ext == ".IRX") {
      auto irx_elf = ee::ElfImage::parse(read_file(entry.path()), &err);
      auto info = irx_elf ? parse_irx(*irx_elf, &err) : std::nullopt;
      if (!info) {
        r.line("  {:<14} (unparsed: {})", entry.path().filename().string(), err);
        continue;
      }
      std::string imports;
      for (const auto& lib : info->imports) imports += std::format("{}({}) ", lib.name, lib.functions.size());
      std::string exports;
      for (const auto& lib : info->exports) exports += std::format("{}({}) ", lib.name, lib.functions.size());
      r.line("  {:<14} module '{}' v{}.{}  text {}  imports: {} exports: {}", entry.path().filename().string(),
             info->name, info->version >> 8, info->version & 0xFF, info->text_size, imports,
             exports.empty() ? "-" : exports);
    } else if (ext == ".IMG") {
      auto bytes = read_file(entry.path());
      auto dir = parse_romdir(bytes, &err);
      if (!dir) {
        r.line("  {:<14} (unparsed: {})", entry.path().filename().string(), err);
        continue;
      }
      std::string names;
      for (const auto& d : *dir) names += std::format("{}({}) ", d.name, d.size);
      r.line("  {:<14} ROMDIR: {}", entry.path().filename().string(), names);
    }
  }

  // --- write outputs
  std::error_code ec;
  fs::create_directories(out_dir, ec);
  std::ofstream(out_dir / "report.md") << r.text;
  std::ofstream fm(out_dir / "functions.toml");
  fm << std::format("# LANTERN function map for {} (derived; do not distribute)\nimage_base = 0x{:08x}\n\n", serial,
                    main->vaddr);
  for (const auto& f : ca.functions)
    fm << std::format("[[function]]\nstart = 0x{:08x}\nend = 0x{:08x}\nsource = \"{}\"\n\n", f.start, f.end,
                      f.from_pointer ? "pointer" : "call");
  fm << overlay_toml;
  std::fputs(r.text.c_str(), stdout);
  std::printf("\nwrote %s and functions.toml\n", (out_dir / "report.md").c_str());
  return 0;
}
