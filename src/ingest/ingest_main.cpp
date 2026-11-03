// lantern-ingest: inspect and extract the user's PS2 disc image. Strictly read-only on the image.
#include <cstdio>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

#include "core/paths.h"
#include "ingest/disc.h"

namespace fs = std::filesystem;
using namespace lantern;

namespace {
constexpr const char* kUsage =
    "Usage:\n"
    "  lantern-ingest identify ISO [--hash]     print serial, region, boot ELF, IRX modules\n"
    "  lantern-ingest list ISO                  list every file with LBA and size\n"
    "  lantern-ingest extract ISO [--all] [--dest DIR]\n"
    "      default set: SYSTEM.CNF + boot ELF + *.IRX; --all extracts everything\n"
    "      default DIR: $XDG_DATA_HOME/lantern/extracted/<serial>\n";

int fail(const std::string& msg) {
  std::fprintf(stderr, "lantern-ingest: %s\n", msg.c_str());
  return 1;
}

void print_info(const ingest::DiscInfo& info) {
  std::printf("volume_id   : %s\n", info.volume_id.c_str());
  std::printf("image_size  : %llu bytes\n", static_cast<unsigned long long>(info.image_size));
  std::printf("boot2       : %s\n", info.cnf.boot2.c_str());
  std::printf("boot_file   : %s\n", info.cnf.boot_file.c_str());
  std::printf("serial      : %s\n", info.serial.empty() ? "(unrecognised)" : info.serial.c_str());
  std::printf("region      : %s\n", info.region.c_str());
  std::printf("version     : %s\n", info.cnf.version.c_str());
  std::printf("video_mode  : %s\n", info.cnf.video_mode.c_str());
  std::printf("irx_modules : %zu\n", info.irx_modules.size());
  for (const auto& m : info.irx_modules) std::printf("  %s\n", m.c_str());
}

std::string hash_with_progress(const iso::Image& img) {
  std::string err;
  int last = -1;
  auto h = ingest::sha256_image(
      img,
      [&](uint64_t done, uint64_t total) {
        int pct = static_cast<int>(done * 100 / total);
        if (pct != last) std::fprintf(stderr, "\rsha256: %3d%%", last = pct);
      },
      &err);
  std::fprintf(stderr, "\n");
  if (!h) {
    std::fprintf(stderr, "lantern-ingest: hashing failed: %s\n", err.c_str());
    return "";
  }
  return *h;
}
}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  if (args.size() < 2) {
    std::fputs(kUsage, stderr);
    return 2;
  }
  const std::string cmd = args[0];
  const fs::path iso_path = args[1];
  bool hash = false, all = false;
  std::optional<fs::path> dest;
  for (size_t i = 2; i < args.size(); ++i) {
    if (args[i] == "--hash")
      hash = true;
    else if (args[i] == "--all")
      all = true;
    else if (args[i] == "--dest" && i + 1 < args.size())
      dest = args[++i];
    else
      return fail("unknown option " + args[i] + "\n" + kUsage);
  }

  std::string err;
  auto img = iso::Image::open(iso_path, &err);
  if (!img) return fail(err);

  if (cmd == "list") {
    img->walk([](const std::string& path, const iso::Entry& e) {
      std::printf("%s %10u %12llu %s\n", e.is_dir ? "d" : "-", e.lba, static_cast<unsigned long long>(e.size),
                  path.c_str());
      return true;
    });
    return 0;
  }

  auto info = ingest::identify(*img, &err);
  if (!info) return fail(err);

  if (cmd == "identify") {
    print_info(*info);
    if (hash) {
      auto h = hash_with_progress(*img);
      if (h.empty()) return 1;
      std::printf("sha256      : %s\n", h.c_str());
    }
    return 0;
  }

  if (cmd == "extract") {
    if (info->serial.empty()) return fail("cannot derive a serial from the boot file; pass --dest explicitly");
    fs::path out = dest ? *dest : resolve_paths().extracted_dir() / info->serial;
    std::string sha = hash_with_progress(*img);
    if (sha.empty()) return 1;
    auto res = ingest::extract(*img, *info, all ? ingest::ExtractSet::All : ingest::ExtractSet::Boot, out,
                               fs::absolute(iso_path).parent_path(), sha, &err);
    if (!res) return fail(err);
    std::printf("extracted %zu files (%llu bytes) to %s\n", res->files.size(),
                static_cast<unsigned long long>(res->bytes), out.string().c_str());
    return 0;
  }
  return fail("unknown command '" + cmd + "'\n" + kUsage);
}
