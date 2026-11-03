#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace lantern::analysis {

// VIF "MPG" command found in a byte image: uploads `count` 64-bit VU micro-instructions to VU
// micro memory at `vu_addr` (in 64-bit units). Detection is heuristic.
struct MpgBlock {
  uint32_t offset = 0;  // byte offset of the MPG command word
  uint32_t vu_addr = 0;
  uint32_t count = 0;   // micro-instruction pairs
  uint32_t e_bits = 0;  // upper words with the E (end) bit set
};

// Accepts candidates whose payload is 8-byte aligned, fits the image, targets VU1 micro memory
// (2048 pairs), and contains at least one E bit within the block.
std::vector<MpgBlock> scan_vif_mpg(std::span<const uint8_t> image);

}  // namespace lantern::analysis
