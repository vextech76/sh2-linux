#include "iop/irx.h"

#include <algorithm>
#include <cstring>
#include <format>

#include "ee/elf.h"

namespace lantern::iop {
namespace {
constexpr uint16_t kIrxType = 0xFF80;
constexpr uint32_t kSectionIopMod = 0x70000080;
constexpr uint32_t kSectionSymtab = 2;
constexpr uint32_t kSectionRel = 9;
constexpr uint32_t kImportMagic = 0x41E00000;
constexpr uint32_t kExportMagic = 0x41C00000;
constexpr uint32_t kJrRa = 0x03E00008;
constexpr uint32_t kAddiuZeroZero = 0x24000000;
constexpr uint32_t kLoadSegmentType = 1;
constexpr uint32_t kBaseAlignment = 0x10000;
constexpr int kMaxExports = 2048;

uint32_t le32(std::span<const uint8_t> s, size_t off) { return ee::load_le32(s.data() + off); }
uint16_t le16(std::span<const uint8_t> s, size_t off) { return static_cast<uint16_t>(s[off] | s[off + 1] << 8); }

std::string fixed_name(std::span<const uint8_t> s, size_t off, size_t n) {
  size_t len = 0;
  while (len < n && s[off + len] >= 0x20 && s[off + len] < 0x7F) ++len;
  return std::string(reinterpret_cast<const char*>(s.data() + off), len);
}

std::string read_name(IopMemory& mem, uint32_t addr, size_t n) {
  std::string out;
  for (size_t i = 0; i < n; ++i) {
    const uint8_t c = mem.read<uint8_t>(addr + static_cast<uint32_t>(i));
    if (c < 0x20 || c >= 0x7F) break;
    out.push_back(static_cast<char>(c));
  }
  return out;
}

// The .iopmod section: { u32 moduleinfo, entry, gp, text_size, data_size, bss_size; u16 version;
// char name[]; } (24 bytes of words, then the version and the name).
struct IopMod {
  uint32_t entry = 0, gp = 0, text = 0, data = 0, bss = 0;
  uint16_t version = 0;
  std::string name;
};

std::optional<IopMod> parse_iopmod(std::span<const uint8_t> m, std::string* error) {
  if (m.size() < 26) {
    if (error) *error = ".iopmod section too small";
    return std::nullopt;
  }
  IopMod mod;
  mod.entry = le32(m, 4);
  mod.gp = le32(m, 8);
  mod.text = le32(m, 12);
  mod.data = le32(m, 16);
  mod.bss = le32(m, 20);
  mod.version = le16(m, 24);
  mod.name = fixed_name(m, 26, m.size() - 26);
  return mod;
}
}  // namespace

std::string format_library(const IrxLibraryId& library) {
  return std::format("{} v{}.{}", library.name, library.version >> 8, library.version & 0xFF);
}

std::optional<uint32_t> place_irx(uint32_t from, uint32_t size) {
  if (size == 0) return std::nullopt;
  const uint32_t base = (from + kBaseAlignment - 1) & ~(kBaseAlignment - 1);
  if (base < from) return std::nullopt;  // overflow
  if (uint64_t{base} + size > IopMemory::kRamSize) return std::nullopt;
  return base;
}

std::optional<uint32_t> irx_image_size(std::span<const uint8_t> bytes, std::string* error) {
  auto elf = ee::ElfImage::parse(std::vector<uint8_t>(bytes.begin(), bytes.end()), error);
  if (!elf) return std::nullopt;
  if (elf->type() != kIrxType) {
    if (error) *error = std::format("ELF type 0x{:04x} is not an IRX module", elf->type());
    return std::nullopt;
  }
  uint32_t size = 0;
  for (const auto& s : elf->segments())
    if (s.type == kLoadSegmentType) size = std::max(size, s.vaddr + s.memsz);
  if (size == 0) {
    if (error) *error = "no PT_LOAD segment";
    return std::nullopt;
  }
  return size;
}

std::optional<LoadedModule> load_irx(IopMemory& mem, std::span<const uint8_t> bytes, const IrxLoadOptions& options,
                                     std::string* error) {
  auto fail = [&](std::string msg) -> std::optional<LoadedModule> {
    if (error) *error = std::move(msg);
    return std::nullopt;
  };
  if (bytes.size() < 52 || std::memcmp(bytes.data(),
                                       "\x7f"
                                       "ELF",
                                       4) != 0)
    return fail("not an ELF file");
  if (options.base % kBaseAlignment != 0)
    return fail(std::format("module base 0x{:08x} is not 64 KiB aligned", options.base));

  auto elf = ee::ElfImage::parse(std::vector<uint8_t>(bytes.begin(), bytes.end()), error);
  if (!elf) return std::nullopt;
  if (elf->type() != kIrxType) return fail(std::format("ELF type 0x{:04x} is not an IRX module", elf->type()));

  const ee::ElfSection* iopmod_section = nullptr;
  for (const auto& s : elf->sections())
    if (s.type == kSectionIopMod) iopmod_section = &s;
  if (!iopmod_section) return fail("no .iopmod section (not an IRX module?)");
  auto mod = parse_iopmod(elf->section_data(*iopmod_section), error);
  if (!mod) return std::nullopt;

  // Validate the whole image before writing anything.
  uint32_t image_size = 0;
  uint32_t load_segments = 0;
  for (const auto& s : elf->segments()) {
    if (s.type != kLoadSegmentType) continue;
    ++load_segments;
    image_size = std::max(image_size, s.vaddr + s.memsz);
  }
  if (!load_segments) return fail("no PT_LOAD segment");
  if (uint64_t{options.base} + image_size > IopMemory::kRamSize)
    return fail(std::format("module image 0x{:x} bytes at 0x{:08x} does not fit in 2 MiB of IOP RAM", image_size,
                            options.base));

  LoadedModule out;
  out.name = mod->name;
  out.version = mod->version;
  out.base = options.base;
  out.text_size = mod->text;
  out.data_size = mod->data;
  out.bss_size = mod->bss;
  out.end = options.base + image_size;
  out.gp = options.base + mod->gp;
  // loadcore calls the entry point unconditionally, including entry 0: several of the shipped
  // sound modules place their initialiser at the start of .text.
  out.entry = options.base + mod->entry;
  if (out.entry % 4 != 0 || out.entry < out.base || out.entry >= out.base + mod->text)
    return fail(std::format(".iopmod entry 0x{:x} is not a word-aligned text offset", mod->entry));
  out.entry_valid = true;

  // Load the segments and clear .bss (the two may be expressed as filesz < memsz).
  for (const auto& s : elf->segments()) {
    if (s.type != kLoadSegmentType) continue;
    auto data = elf->segment_data(s);
    if (data.size()) mem.load(options.base + s.vaddr, data.data(), data.size());
    for (uint32_t i = s.filesz; i < s.memsz; ++i) mem.write<uint8_t>(options.base + s.vaddr + i, 0);
  }

  // Relocations. Sony's IRX modules reference symbol 0 for every entry, which means "the module
  // itself"; the link-time address is already in the instruction or data word, so adding the base
  // address relocates it. Named symbols are supported too (a symbol value is added).
  struct PendingHi16 {
    uint32_t addr;
    uint32_t hi;
    uint32_t value;
  };
  std::vector<PendingHi16> pending;
  const auto& sections = elf->sections();
  uint32_t relocations = 0;
  for (const auto& rel : sections) {
    if (rel.type != kSectionRel) continue;
    if (rel.info >= sections.size() || rel.link >= sections.size())
      return fail("relocation section points outside the section table");
    auto symtab = elf->section_data(sections[rel.link]);
    auto entries = elf->section_data(rel);
    if (entries.size() % 8 != 0) return fail("relocation section size is not a multiple of 8");
    for (size_t i = 0; i < entries.size(); i += 8) {
      // In the shipped modules r_offset is already the link-time virtual address (its section's
      // sh_addr plus the offset), so the base is the only thing that has to be added.
      const uint32_t offset = le32(entries, i);
      const uint32_t info = le32(entries, i + 4);
      const uint32_t type = info & 0xFF;
      const uint32_t sym_index = info >> 8;
      const uint32_t addr = options.base + offset;
      if (addr < options.base || addr + 4 > out.end)
        return fail(std::format("relocation at 0x{:08x} is outside the loaded image", addr));

      uint32_t value = options.base;
      if (sym_index != 0) {
        if (symtab.size() < (size_t{sym_index} + 1) * 16)
          return fail(std::format("relocation references symbol {} outside .symtab", sym_index));
        const uint32_t sym_value = le32(symtab, size_t{sym_index} * 16 + 4);
        const uint16_t shndx = le16(symtab, size_t{sym_index} * 16 + 14);
        if (shndx == 0) return fail(std::format("relocation references undefined symbol {}", sym_index));
        value = (shndx == 0xFFF1 /* SHN_ABS */) ? sym_value : options.base + sym_value;
      }

      ++relocations;
      switch (type) {
        case 0:  // R_MIPS_NONE
          break;
        case 2:  // R_MIPS_32
          mem.write<uint32_t>(addr, mem.read<uint32_t>(addr) + value);
          break;
        case 4: {  // R_MIPS_26
          if (value & 3) return fail(std::format("R_MIPS_26 target 0x{:08x} is not word aligned", value));
          const uint32_t w = mem.read<uint32_t>(addr);
          const uint32_t result = ((w & 0x03FFFFFFu) << 2) + value;
          if (result & 3) return fail(std::format("R_MIPS_26 result 0x{:08x} is not word aligned", result));
          mem.write<uint32_t>(addr, (w & 0xFC000000u) | ((result >> 2) & 0x03FFFFFFu));
          break;
        }
        case 5: {  // R_MIPS_HI16: patched when its LO16 partner is seen
          const uint32_t w = mem.read<uint32_t>(addr);
          pending.push_back({addr, w & 0xFFFF, value});
          break;
        }
        case 6: {  // R_MIPS_LO16
          if (pending.empty()) return fail("R_MIPS_LO16 without a preceding R_MIPS_HI16");
          const uint32_t w = mem.read<uint32_t>(addr);
          const int32_t lo = static_cast<int16_t>(w & 0xFFFF);
          uint32_t result = 0;
          for (const auto& hi : pending) {
            if (hi.value != value) return fail("R_MIPS_HI16/LO16 pair resolves different symbols");
            const uint32_t ahl = (hi.hi << 16) + static_cast<uint32_t>(lo);
            result = ahl + value;
            const uint32_t new_hi = ((result + 0x8000) >> 16) & 0xFFFF;
            const uint32_t old = mem.read<uint32_t>(hi.addr);
            mem.write<uint32_t>(hi.addr, (old & 0xFFFF0000u) | new_hi);
          }
          mem.write<uint32_t>(addr, (w & 0xFFFF0000u) | (result & 0xFFFFu));
          pending.clear();
          break;
        }
        case 1:  // R_MIPS_16 (present in the ABI; not seen in the shipped modules)
          mem.write<uint16_t>(addr, static_cast<uint16_t>(mem.read<uint16_t>(addr) + value));
          break;
        default: return fail(std::format("unsupported MIPS relocation type {}", type));
      }
    }
  }
  if (!pending.empty()) return fail("R_MIPS_HI16 without a following R_MIPS_LO16");
  out.relocations = relocations;

  // Import and export tables live in the loaded image and are found by their magic words. The
  // export entries were made absolute by R_MIPS_32 relocations; verify that instead of guessing.
  const uint32_t scan_end = out.end;
  for (uint32_t addr = options.base; addr + 20 <= scan_end; addr += 4) {
    const uint32_t magic = mem.read<uint32_t>(addr);
    if (magic != kImportMagic && magic != kExportMagic) continue;
    if (mem.read<uint32_t>(addr + 4) != 0) continue;
    const uint16_t version = static_cast<uint16_t>(mem.read<uint32_t>(addr + 8) & 0xFFFF);
    IrxLibraryId library{read_name(mem, addr + 12, 8), version};
    if (library.name.empty()) continue;

    if (magic == kImportMagic) {
      IrxImport import;
      import.library = library;
      for (uint32_t p = addr + 20; p + 8 <= scan_end; p += 8) {
        if (mem.read<uint32_t>(p) != kJrRa) break;
        const uint32_t li = mem.read<uint32_t>(p + 4);
        if ((li & 0xFFFF0000u) != kAddiuZeroZero) break;
        import.stubs.push_back(p);
        import.functions.push_back(static_cast<uint16_t>(li & 0xFFFF));
      }
      if (import.stubs.empty()) continue;
      addr = import.stubs.back() + 8 - 4;  // continue after the table (loop adds 4)
      out.imports.push_back(std::move(import));
    } else {
      IrxExport exp;
      exp.library = library;
      for (uint32_t p = addr + 20; p + 4 <= scan_end && exp.functions.size() < kMaxExports; p += 4) {
        const uint32_t fn = mem.read<uint32_t>(p);
        if (fn == 0) break;
        if (fn < options.base || fn >= out.end)
          return fail(std::format("export {} function {} = 0x{:08x} is outside the module", library.name,
                                  exp.functions.size(), fn));
        exp.functions.push_back(fn);
      }
      if (exp.functions.empty()) continue;
      addr = addr + 20 + static_cast<uint32_t>(exp.functions.size()) * 4 - 4;
      out.exports.push_back(std::move(exp));
    }
  }

  // Patch the import stubs. `j handler; nop` fits the 8-byte stub; the caller's `jal` already set
  // $ra, so the handler returns through it.
  for (const auto& import : out.imports) {
    for (size_t i = 0; i < import.stubs.size(); ++i) {
      const uint16_t function = import.functions[i];
      auto target = options.resolve ? options.resolve(import.library, function) : std::nullopt;
      if (!target) {
        out.unresolved.push_back({import.library, function, import.stubs[i]});
        continue;
      }
      if (*target >= IopMemory::kRamSize || *target % 4 != 0)
        return fail(
            std::format("resolver returned invalid address 0x{:08x} for {}", *target, format_library(import.library)));
      mem.write<uint32_t>(import.stubs[i], 0x08000000u | ((*target >> 2) & 0x03FFFFFFu));
      mem.write<uint32_t>(import.stubs[i] + 4, 0);
    }
  }
  return out;
}

}  // namespace lantern::iop
