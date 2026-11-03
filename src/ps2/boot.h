#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "ee/elf.h"
#include "ee/memory.h"
#include "ps2/kernel.h"
#include "recomp/runtime.h"

namespace lantern::ps2 {

// Copy file-backed PT_LOAD data into guest RAM and zero the BSS tails.
void load_elf(ee::Memory& mem, const ee::ElfImage& elf);

// Identifies which overlay currently occupies the overlay window by comparing guest memory
// against the known overlay images (after the game flushes the instruction cache).
class OverlayTracker {
 public:
  void add(int id, std::string name, std::vector<uint8_t> image) {
    overlays_.push_back({id, std::move(name), std::move(image)});
  }
  // Returns the matching overlay id, or -1 if the window holds none of them.
  int identify(ee::Memory& mem, uint32_t base) const;
  const std::string& name(int id) const;
  size_t size() const { return overlays_.size(); }

 private:
  struct Overlay {
    int id;
    std::string name;
    std::vector<uint8_t> image;
  };
  std::vector<Overlay> overlays_;
};

}  // namespace lantern::ps2
