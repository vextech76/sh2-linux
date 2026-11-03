#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ee/elf.h"

namespace lantern::analysis {

struct IrxLibrary {
  std::string name;
  uint16_t version = 0;
  std::vector<uint16_t> functions;  // import: function numbers used; export: count = size()
};

struct IrxInfo {
  std::string name;  // from .iopmod
  uint16_t version = 0;
  uint32_t text_size = 0, data_size = 0, bss_size = 0;
  std::vector<IrxLibrary> imports;
  std::vector<IrxLibrary> exports;
};

// Parse a PS2 IOP module (ELF type 0xFF80 with a .iopmod section). Import/export tables are
// found by their magic words (0x41E00000 / 0x41C00000) in the code.
std::optional<IrxInfo> parse_irx(const ee::ElfImage& elf, std::string* error);

struct RomdirEntry {
  std::string name;
  uint32_t size = 0;
  uint32_t offset = 0;  // within the image, 16-byte aligned
};

// IOPRP*.IMG / ROM images: a ROMDIR table beginning with "RESET".
std::optional<std::vector<RomdirEntry>> parse_romdir(std::span<const uint8_t> image, std::string* error);

}  // namespace lantern::analysis
