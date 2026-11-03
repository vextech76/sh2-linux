#include "ingest/disc.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>

#include "core/paths.h"
#include "core/sha256.h"

namespace lantern::ingest {
namespace fs = std::filesystem;

namespace {
std::string trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return std::string(s);
}
std::string upper(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}
bool ends_with_ci(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() &&
         upper(std::string(s.substr(s.size() - suffix.size()))) == upper(std::string(suffix));
}
// Disc paths become relative host paths; reject anything that could escape `dest`.
bool safe_relative(const std::string& disc_path) {
  fs::path p = fs::path(disc_path).relative_path();
  for (const auto& part : p)
    if (part == ".." || part == ".") return false;
  return !p.empty();
}
// IOP replacement images (IOPRP*.IMG) bundle updated IOP modules loaded at boot.
bool is_ioprp(std::string_view path) {
  std::string name = upper(fs::path(std::string(path)).filename().string());
  return name.starts_with("IOPRP") && name.ends_with(".IMG");
}
}  // namespace

std::optional<SystemCnf> parse_system_cnf(std::string_view text, std::string* error) {
  SystemCnf cnf;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find_first_of("\r\n", pos);
    if (end == std::string_view::npos) end = text.size();
    std::string_view line = text.substr(pos, end - pos);
    pos = end + 1;
    auto eq = line.find('=');
    if (eq == std::string_view::npos) continue;
    std::string key = upper(trim(line.substr(0, eq)));
    std::string value = trim(line.substr(eq + 1));
    if (key == "BOOT2")
      cnf.boot2 = value;
    else if (key == "VER")
      cnf.version = value;
    else if (key == "VMODE")
      cnf.video_mode = value;
  }
  if (cnf.boot2.empty()) {
    if (error) *error = "SYSTEM.CNF has no BOOT2 entry (PS1 disc or not a PS2 game?)";
    return std::nullopt;
  }
  std::string_view b = cnf.boot2;
  if (auto colon = b.find(':'); colon != std::string_view::npos) b = b.substr(colon + 1);
  while (!b.empty() && (b.front() == '\\' || b.front() == '/')) b.remove_prefix(1);
  if (auto semi = b.find(';'); semi != std::string_view::npos) b = b.substr(0, semi);
  cnf.boot_file = std::string(b);
  std::replace(cnf.boot_file.begin(), cnf.boot_file.end(), '\\', '/');
  return cnf;
}

std::string serial_from_boot_file(std::string_view boot_file) {
  std::string name = upper(fs::path(std::string(boot_file)).filename().string());
  // Expected shape: AAAA_DDD.DD
  if (name.size() != 11 || name[4] != '_' || name[8] != '.') return "";
  for (int i = 0; i < 4; ++i)
    if (!std::isalpha(static_cast<unsigned char>(name[static_cast<size_t>(i)]))) return "";
  std::string digits = name.substr(5, 3) + name.substr(9, 2);
  if (!std::all_of(digits.begin(), digits.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
    return "";
  return name.substr(0, 4) + "-" + digits;
}

std::string region_from_serial(std::string_view serial) {
  std::string p = upper(std::string(serial.substr(0, std::min<size_t>(4, serial.size()))));
  if (p == "SLUS" || p == "SCUS") return "NTSC-U";
  if (p == "SLES" || p == "SCES" || p == "SCED") return "PAL";
  if (p == "SLPM" || p == "SLPS" || p == "SCPS" || p == "SLAJ" || p == "SCAJ") return "NTSC-J";
  if (p == "SLKA" || p == "SCKA") return "NTSC-K";
  return "unknown";
}

std::optional<DiscInfo> identify(const iso::Image& image, std::string* error) {
  auto cnf_entry = image.find("SYSTEM.CNF");
  if (!cnf_entry) {
    if (error) *error = "SYSTEM.CNF not found in disc root";
    return std::nullopt;
  }
  auto bytes = image.read_all(*cnf_entry, 64 * 1024);
  if (!bytes) {
    if (error) *error = "cannot read SYSTEM.CNF";
    return std::nullopt;
  }
  auto cnf = parse_system_cnf(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()), error);
  if (!cnf) return std::nullopt;
  if (!image.find(cnf->boot_file)) {
    if (error) *error = std::format("boot file '{}' named in SYSTEM.CNF is missing", cnf->boot_file);
    return std::nullopt;
  }
  DiscInfo info;
  info.volume_id = image.volume().volume_id;
  info.image_size = image.image_size();
  info.cnf = *cnf;
  info.serial = serial_from_boot_file(cnf->boot_file);
  info.region = info.serial.empty() ? "unknown" : region_from_serial(info.serial);
  image.walk([&](const std::string& path, const iso::Entry& e) {
    if (!e.is_dir && ends_with_ci(path, ".IRX")) info.irx_modules.push_back(path);
    return true;
  });
  std::sort(info.irx_modules.begin(), info.irx_modules.end());
  return info;
}

std::optional<std::string> sha256_image(const iso::Image& image, const Progress& progress, std::string* error) {
  Sha256 sha;
  std::vector<uint8_t> buf(8u << 20);
  const uint64_t total = image.image_size();
  uint64_t done = 0;
  while (done < total) {
    size_t n = static_cast<size_t>(std::min<uint64_t>(buf.size(), total - done));
    if (!image.read_raw(done, {buf.data(), n})) {
      if (error) *error = std::format("read failed at offset {}", done);
      return std::nullopt;
    }
    sha.update({buf.data(), n});
    done += n;
    if (progress) progress(done, total);
  }
  return Sha256::hex(sha.finish());
}

std::optional<ExtractResult> extract(const iso::Image& image, const DiscInfo& info, ExtractSet set,
                                     const fs::path& dest, const fs::path& forbidden_root,
                                     const std::string& image_sha256, std::string* error) {
  if (!forbidden_root.empty() && path_is_within(dest, forbidden_root)) {
    if (error)
      *error = std::format("refusing to extract into '{}': it is inside the game data directory '{}'", dest.string(),
                           forbidden_root.string());
    return std::nullopt;
  }
  std::vector<std::pair<std::string, iso::Entry>> files;
  const std::string boot = "/" + upper(info.cnf.boot_file);
  image.walk([&](const std::string& path, const iso::Entry& e) {
    if (e.is_dir) return true;
    bool want = set == ExtractSet::All || upper(path) == "/SYSTEM.CNF" || upper(path) == boot ||
                ends_with_ci(path, ".IRX") || is_ioprp(path);
    if (want) files.emplace_back(path, e);
    return true;
  });

  std::error_code ec;
  fs::create_directories(dest, ec);
  if (ec) {
    if (error) *error = std::format("cannot create '{}': {}", dest.string(), ec.message());
    return std::nullopt;
  }
  ExtractResult result;
  std::vector<uint8_t> buf(4u << 20);
  for (const auto& [path, entry] : files) {
    if (!safe_relative(path)) {
      if (error) *error = std::format("unsafe path on disc: '{}'", path);
      return std::nullopt;
    }
    fs::path out_path = dest / fs::path(path).relative_path();
    fs::create_directories(out_path.parent_path(), ec);
    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out) {
      if (error) *error = std::format("cannot write '{}'", out_path.string());
      return std::nullopt;
    }
    for (uint64_t off = 0; off < entry.size;) {
      size_t n = static_cast<size_t>(std::min<uint64_t>(buf.size(), entry.size - off));
      if (!image.read(entry, off, {buf.data(), n})) {
        if (error) *error = std::format("read failed for '{}'", path);
        return std::nullopt;
      }
      out.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(n));
      off += n;
    }
    if (!out) {
      if (error) *error = std::format("write failed for '{}'", out_path.string());
      return std::nullopt;
    }
    result.files.push_back(path);
    result.bytes += entry.size;
  }

  std::ofstream manifest(dest / "manifest.toml", std::ios::trunc);
  manifest << "# Generated by lantern-ingest. Derived from the user's own disc; do not distribute.\n"
           << std::format("serial = \"{}\"\nregion = \"{}\"\nvolume_id = \"{}\"\nboot_file = \"{}\"\n", info.serial,
                          info.region, info.volume_id, info.cnf.boot_file)
           << std::format("version = \"{}\"\nvideo_mode = \"{}\"\nimage_size = {}\nimage_sha256 = \"{}\"\n",
                          info.cnf.version, info.cnf.video_mode, info.image_size, image_sha256)
           << std::format("set = \"{}\"\nfiles = [\n", set == ExtractSet::All ? "all" : "boot");
  for (const auto& f : result.files) manifest << std::format("  \"{}\",\n", f);
  manifest << "]\n";
  return result;
}

}  // namespace lantern::ingest
