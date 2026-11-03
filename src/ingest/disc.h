#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "iso/iso9660.h"

namespace lantern::ingest {

struct SystemCnf {
  std::string boot2;       // raw, e.g. "cdrom0:\SLUS_123.45;1"
  std::string boot_file;   // path within the disc, e.g. "SLUS_123.45"
  std::string version;     // VER
  std::string video_mode;  // VMODE (NTSC / PAL)
};

std::optional<SystemCnf> parse_system_cnf(std::string_view text, std::string* error);

struct DiscInfo {
  std::string volume_id;
  uint64_t image_size = 0;
  SystemCnf cnf;
  std::string serial;                    // "SLUS-12345" form, derived from the boot file name
  std::string region;                    // derived from the serial prefix; "unknown" if not recognised
  std::vector<std::string> irx_modules;  // every *.IRX path on the disc
};

// Read SYSTEM.CNF and enumerate modules. Pure read; does not hash.
std::optional<DiscInfo> identify(const iso::Image& image, std::string* error);

// "SLUS_123.45" -> "SLUS-12345". Empty if the name doesn't look like a PS2 serial.
std::string serial_from_boot_file(std::string_view boot_file);
std::string region_from_serial(std::string_view serial);

using Progress = std::function<void(uint64_t done, uint64_t total)>;
std::optional<std::string> sha256_image(const iso::Image& image, const Progress& progress, std::string* error);

enum class ExtractSet {
  Boot,  // SYSTEM.CNF, boot ELF, all IRX modules, IOPRP*.IMG (small; what analysis needs)
  All,   // every file on the disc
};

struct ExtractResult {
  std::vector<std::string> files;
  uint64_t bytes = 0;
};

// Extract into `dest` (created). Refuses if `dest` is inside `forbidden_root` (the directory
// holding the supplied disc image). Writes manifest.toml describing the source and files.
std::optional<ExtractResult> extract(const iso::Image& image, const DiscInfo& info, ExtractSet set,
                                     const std::filesystem::path& dest, const std::filesystem::path& forbidden_root,
                                     const std::string& image_sha256, std::string* error);

}  // namespace lantern::ingest
