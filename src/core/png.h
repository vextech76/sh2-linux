#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace lantern {

// Minimal PNG writer (RGBA8, zlib "stored" blocks: no compression dependency). For debug frame
// dumps; `rgba` is 0xAABBGGRR per pixel, row-major.
bool write_png(const std::filesystem::path& path, uint32_t width, uint32_t height, const std::vector<uint32_t>& rgba,
               std::string* error = nullptr);

}  // namespace lantern
