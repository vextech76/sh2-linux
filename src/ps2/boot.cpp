#include "ps2/boot.h"

#include <cstring>
#include <format>

#include "core/log.h"

namespace lantern::ps2 {

void load_elf(ee::Memory& mem, const ee::ElfImage& elf) {
  for (const auto& s : elf.segments()) {
    if (s.type != 1) continue;
    auto data = elf.segment_data(s);
    for (uint32_t i = 0; i < s.memsz; ++i) mem.write<uint8_t>(s.vaddr + i, i < data.size() ? data[i] : 0);
  }
}

int OverlayTracker::identify(ee::Memory& mem, uint32_t base) const {
  for (const auto& ov : overlays_) {
    const size_t n = std::min<size_t>(ov.image.size(), 256);
    const uint8_t* p = mem.host(base, static_cast<unsigned>(n));
    if (p && std::memcmp(p, ov.image.data(), n) == 0) return ov.id;
  }
  return -1;
}

const std::string& OverlayTracker::name(int id) const {
  static const std::string kNone = "(none)";
  for (const auto& ov : overlays_)
    if (ov.id == id) return ov.name;
  return kNone;
}

}  // namespace lantern::ps2
