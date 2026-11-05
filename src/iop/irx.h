#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "iop/iop_cpu.h"

namespace lantern::iop {

// A PS2 IOP library identity as it appears in an IRX import/export table.
struct IrxLibraryId {
  std::string name;
  uint16_t version = 0;  // 0x0101 prints as 1.1

  bool operator==(const IrxLibraryId&) const = default;
};

struct IrxImport {
  IrxLibraryId library;
  std::vector<uint32_t> stubs;      // absolute address of each 8-byte import stub
  std::vector<uint16_t> functions;  // function number requested by each stub
};

struct IrxExport {
  IrxLibraryId library;
  std::vector<uint32_t> functions;  // absolute addresses, in function-number order
};

struct LoadedModule {
  std::string name;      // from .iopmod
  uint16_t version = 0;  // from .iopmod
  uint32_t base = 0;
  uint32_t entry = 0;  // 0 when the module has no initialiser
  uint32_t gp = 0;
  uint32_t text_size = 0, data_size = 0, bss_size = 0;
  uint32_t end = 0;  // first byte after the loaded image (including bss)
  uint32_t relocations = 0;
  bool entry_valid = false;
  std::vector<IrxImport> imports;
  std::vector<IrxExport> exports;

  struct Unresolved {
    IrxLibraryId library;
    uint16_t function = 0;
    uint32_t stub = 0;
  };
  std::vector<Unresolved> unresolved;

  bool has_entry() const { return entry_valid; }
};

// Answer an imported (library, function) with an absolute guest address. Returning nullopt leaves
// the stub untouched: the module keeps `jr ra; addiu zero,zero,N`, which returns N silently.
using ImportResolver = std::function<std::optional<uint32_t>(const IrxLibraryId&, uint16_t function)>;

struct IrxLoadOptions {
  // Modules are relocated by adding the base address. A 64 KiB aligned base keeps every %lo16
  // value unchanged, which is guaranteed by the loader and checked here.
  uint32_t base = 0;
  ImportResolver resolve;
};

// Load a PS2 IOP module (ELF type 0xFF80 with a .iopmod section) into IOP RAM: copy the PT_LOAD
// segments, zero .bss, apply R_MIPS_32/26/HI16/LO16 relocations, then patch every import stub the
// resolver answers with `j <handler>; nop`. Unresolved imports are reported, not invented.
std::optional<LoadedModule> load_irx(IopMemory& mem, std::span<const uint8_t> bytes, const IrxLoadOptions& options,
                                     std::string* error);

// First 64 KiB aligned address at or after `from` with room for `size` bytes.
std::optional<uint32_t> place_irx(uint32_t from, uint32_t size);

// Highest PT_LOAD vaddr + memsz, i.e. how much RAM the module needs from its base.
std::optional<uint32_t> irx_image_size(std::span<const uint8_t> bytes, std::string* error);

// "name v1.1"
std::string format_library(const IrxLibraryId& library);

}  // namespace lantern::iop
