#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <vector>

#include "iop/irx.h"

using namespace lantern::iop;

namespace {
constexpr uint32_t kTextOff = 0x100;
constexpr uint32_t kTextSize = 0x60;
constexpr uint32_t kModOff = 0x84;
constexpr uint32_t kModSize = 0x30;
constexpr uint32_t kRelOff = 0x180;
constexpr uint32_t kShstrOff = 0x1C0;
constexpr uint32_t kSymOff = 0x200;
constexpr uint32_t kStrOff = 0x210;
constexpr uint32_t kShOff = 0x220;
constexpr uint32_t kEntryPoint = 0x40;

void put16(std::vector<uint8_t>& b, size_t off, uint16_t v) {
  b[off] = static_cast<uint8_t>(v);
  b[off + 1] = static_cast<uint8_t>(v >> 8);
}
void put32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
  b[off] = static_cast<uint8_t>(v);
  b[off + 1] = static_cast<uint8_t>(v >> 8);
  b[off + 2] = static_cast<uint8_t>(v >> 16);
  b[off + 3] = static_cast<uint8_t>(v >> 24);
}
void put_name(std::vector<uint8_t>& b, size_t off, std::string_view s) {
  for (size_t i = 0; i < s.size(); ++i) b[off + i] = static_cast<uint8_t>(s[i]);
}

struct Section {
  uint32_t name, type, flags, addr, offset, size, link, info, align, entsize;
};

// A hand-built IRX-shaped ELF: .text at 0, one import stub and one exported entry, plus
// R_MIPS_HI16/LO16/26/32 relocations all against symbol 0, exactly like Sony's modules.
std::vector<uint8_t> make_irx(uint32_t text_memsz = 0x80, uint16_t entry = kEntryPoint) {
  std::vector<uint8_t> b(0x400, 0);
  // ELF header
  b[0] = 0x7F;
  b[1] = 'E';
  b[2] = 'L';
  b[3] = 'F';
  b[4] = 1;              // 32-bit
  b[5] = 1;              // little-endian
  b[6] = 1;              // version
  put16(b, 16, 0xFF80);  // ET_PS2_IRX
  put16(b, 18, 8);       // EM_MIPS
  put32(b, 20, 1);
  put32(b, 28, 52);  // e_phoff
  put32(b, 32, kShOff);
  put32(b, 36, 1);  // e_flags
  put16(b, 40, 52);
  put16(b, 42, 32);
  put16(b, 44, 1);
  put16(b, 46, 40);
  put16(b, 48, 7);
  put16(b, 50, 4);

  // PT_LOAD over .text
  put32(b, 52, 1);           // PT_LOAD
  put32(b, 56, kTextOff);    // p_offset
  put32(b, 68, kTextSize);   // p_filesz
  put32(b, 72, text_memsz);  // p_memsz
  put32(b, 76, 7);           // RWE
  put32(b, 80, 16);          // p_align

  // .iopmod: moduleinfo, entry, gp, text, data, bss, version, name
  put32(b, kModOff + 0, 0x2064);
  put32(b, kModOff + 4, entry);
  put32(b, kModOff + 8, 0x100);
  put32(b, kModOff + 12, kTextSize);
  put32(b, kModOff + 16, 0);
  put32(b, kModOff + 20, text_memsz - kTextSize);
  put16(b, kModOff + 24, 0x0101);
  put_name(b, kModOff + 26, "testmod");

  // .text
  put32(b, kTextOff + 0x00, 0x3C080000);  // lui t0, 0x0000      (hi of 0x200)
  put32(b, kTextOff + 0x04, 0x25080200);  // addiu t0,t0,0x200   (lo of 0x200)
  put32(b, kTextOff + 0x08, 0x0C000010);  // jal 0x40
  put32(b, kTextOff + 0x0C, 0x00000000);
  put32(b, kTextOff + 0x10, 0x00000040);  // R_MIPS_32 addend: 0x40
  put32(b, kTextOff + 0x14, 0x41E00000);  // import magic
  put32(b, kTextOff + 0x18, 0);
  put32(b, kTextOff + 0x1C, 0x00000101);
  put_name(b, kTextOff + 0x20, "testlib");  // 8 bytes
  put32(b, kTextOff + 0x28, 0x03E00008);    // jr ra
  put32(b, kTextOff + 0x2C, 0x24000005);    // addiu zero,zero,5
  put32(b, kTextOff + 0x30, 0);
  put32(b, kTextOff + 0x34, 0);
  put32(b, kTextOff + 0x38, 0x41C00000);  // export magic
  put32(b, kTextOff + 0x3C, 0);
  put32(b, kTextOff + 0x40, 0x00000100);
  put_name(b, kTextOff + 0x44, "explib");  // 8 bytes
  put32(b, kTextOff + 0x4C, 0x00000040);   // R_MIPS_32: exported function
  put32(b, kTextOff + 0x50, 0);

  // .rel.text: HI16@0, LO16@4, R_MIPS_26@8, R_MIPS_32@0x10, R_MIPS_32@0x4C
  const uint32_t rels[][2] = {{0x00, 5}, {0x04, 6}, {0x08, 4}, {0x10, 2}, {0x4C, 2}};
  for (size_t i = 0; i < 5; ++i) {
    put32(b, kRelOff + i * 8, rels[i][0]);
    put32(b, kRelOff + i * 8 + 4, rels[i][1]);  // symbol 0
  }

  // .shstrtab
  put_name(b, kShstrOff, "\0.iopmod\0.text\0.rel.text\0.shstrtab\0.symtab\0.strtab\0");
  // .symtab: a single null symbol, as in Sony's modules
  put32(b, kSymOff + 0, 0);
  put32(b, kSymOff + 4, 0);
  put32(b, kSymOff + 8, 0);
  put16(b, kSymOff + 12, 0);
  put16(b, kSymOff + 14, 0);
  b[kStrOff] = 0;

  const Section secs[7] = {
      {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
      {1, 0x70000080, 0, 0, kModOff, kModSize, 0, 0, 4, 0},
      {9, 1, 6, 0, kTextOff, kTextSize, 0, 0, 16, 0},
      {15, 9, 0, 0, kRelOff, 5 * 8, 5, 2, 4, 8},
      {25, 3, 0, 0, kShstrOff, 51, 0, 0, 1, 0},
      {35, 2, 0, 0, kSymOff, 16, 6, 1, 4, 16},
      {43, 3, 0, 0, kStrOff, 1, 0, 0, 1, 0},
  };
  for (size_t i = 0; i < 7; ++i) {
    const auto& s = secs[i];
    const size_t o = kShOff + i * 40;
    put32(b, o + 0, s.name);
    put32(b, o + 4, s.type);
    put32(b, o + 8, s.flags);
    put32(b, o + 12, s.addr);
    put32(b, o + 16, s.offset);
    put32(b, o + 20, s.size);
    put32(b, o + 24, s.link);
    put32(b, o + 28, s.info);
    put32(b, o + 32, s.align);
    put32(b, o + 36, s.entsize);
  }
  return b;
}

uint32_t rd(IopMemory& m, uint32_t addr) {
  uint32_t v = 0;
  std::memcpy(&v, m.host(addr, 4), 4);
  return v;
}
}  // namespace

TEST_CASE("IRX: relocations, bss, imports and exports") {
  IopMemory mem;
  const uint32_t base = 0x00010000;
  for (uint32_t a = base; a < base + 0x80; a += 4) mem.write<uint32_t>(a, 0xAAAAAAAA);  // bss poison
  const auto bytes = make_irx();

  std::string err;
  auto mod = load_irx(mem, bytes, {.base = base, .resolve = {}}, &err);
  REQUIRE_MESSAGE(mod, err);
  CHECK(mod->name == "testmod");
  CHECK(mod->version == 0x0101);
  CHECK(mod->base == base);
  CHECK(mod->entry == base + kEntryPoint);
  CHECK(mod->gp == base + 0x100);
  CHECK(mod->text_size == kTextSize);

  // HI16/LO16: link-time 0x200 + base. The low half is unchanged (64 KiB aligned base).
  CHECK(rd(mem, base + 0x00) == 0x3C080001u);  // lui t0, 0x0001
  CHECK(rd(mem, base + 0x04) == 0x25080200u);  // addiu t0,t0,0x200
  // R_MIPS_26: 0x40 + base
  CHECK(rd(mem, base + 0x08) == 0x0C004010u);
  // R_MIPS_32: addend 0x40 + base
  CHECK(rd(mem, base + 0x10) == base + 0x40);
  // .bss was cleared
  CHECK(rd(mem, base + 0x60) == 0u);
  CHECK(rd(mem, base + 0x7C) == 0u);

  REQUIRE(mod->imports.size() == 1);
  CHECK(mod->imports[0].library.name == "testlib");
  CHECK(mod->imports[0].library.version == 0x0101);
  REQUIRE(mod->imports[0].functions.size() == 1);
  CHECK(mod->imports[0].functions[0] == 5);
  CHECK(mod->imports[0].stubs[0] == base + 0x28);
  REQUIRE(mod->unresolved.size() == 1);
  CHECK(mod->unresolved[0].function == 5);
  CHECK(rd(mem, base + 0x28) == 0x03E00008u);  // untouched: falls through as jr ra

  REQUIRE(mod->exports.size() == 1);
  CHECK(mod->exports[0].library.name == "explib");
  REQUIRE(mod->exports[0].functions.size() == 1);
  CHECK(mod->exports[0].functions[0] == base + 0x40);
  CHECK(mod->relocations == 5);
}

TEST_CASE("IRX: HI16/LO16 pairing carries a named symbol's low half") {
  // Target 0x1FFF0 (hi 1, lo 0xFFF0) plus symbol value 0x1234 and base 0x10000 = 0x31224, which
  // carries into the high half. This is the case a base-relative shortcut would get wrong.
  auto bytes = make_irx();
  put32(bytes, kTextOff + 0x00, 0x3C080002);  // lui t0, 0x0002
  put32(bytes, kTextOff + 0x04, 0x2508FFF0);  // addiu t0,t0,0xFFF0
  put32(bytes, kRelOff + 4, 0x00000105);      // HI16 against symbol 1
  put32(bytes, kRelOff + 12, 0x00000106);     // LO16 against symbol 1
  // Add symbol 1 to .symtab (defined in .text, value 0x1234).
  put32(bytes, kSymOff + 16, 0);
  put32(bytes, kSymOff + 20, 0x1234);
  put32(bytes, kSymOff + 24, 0);
  put16(bytes, kSymOff + 28, 0);
  put16(bytes, kSymOff + 30, 2);
  put32(bytes, kShOff + 5 * 40 + 20, 32);  // .symtab size

  IopMemory mem;
  std::string err;
  auto mod = load_irx(mem, bytes, {.base = 0x00010000, .resolve = {}}, &err);
  REQUIRE_MESSAGE(mod, err);
  CHECK(rd(mem, 0x00010000) == 0x3C080003u);  // result 0x31224 -> hi 3
  CHECK(rd(mem, 0x00010004) == 0x25081224u);  // low half rewritten
}

TEST_CASE("IRX: entry 0 is a valid initialiser, not \"no entry\"") {
  // Several shipped sound modules place their initialiser at the start of .text.
  IopMemory mem;
  std::string err;
  auto mod = load_irx(mem, make_irx(0x80, 0), {.base = 0x00010000, .resolve = {}}, &err);
  REQUIRE_MESSAGE(mod, err);
  CHECK(mod->entry == 0x00010000u);
  CHECK(mod->has_entry());
}

TEST_CASE("IRX: a resolver patches the import stub with an absolute jump") {
  IopMemory mem;
  const uint32_t base = 0x00020000, handler = 0x001F0000;
  const auto bytes = make_irx();
  std::string err;
  auto mod = load_irx(mem, bytes,
                      {.base = base,
                       .resolve = [&](const IrxLibraryId& lib, uint16_t fn) -> std::optional<uint32_t> {
                         if (lib.name == "testlib" && fn == 5) return handler;
                         return std::nullopt;
                       }},
                      &err);
  REQUIRE_MESSAGE(mod, err);
  CHECK(mod->unresolved.empty());
  CHECK(rd(mem, base + 0x28) == (0x08000000u | (handler >> 2)));
  CHECK(rd(mem, base + 0x2C) == 0u);
}

TEST_CASE("IRX: rejects what it cannot load") {
  std::string err;
  IopMemory mem;
  CHECK_FALSE(load_irx(mem, std::vector<uint8_t>{1, 2, 3}, {.base = 0x10000, .resolve = {}}, &err));
  CHECK(err.find("ELF") != std::string::npos);

  auto bytes = make_irx();
  CHECK_FALSE(load_irx(mem, bytes, {.base = 0x10800, .resolve = {}}, &err));  // not 64 KiB aligned
  CHECK(err.find("aligned") != std::string::npos);

  bytes[16] = 2;  // ET_EXEC, not an IRX
  bytes[17] = 0;
  CHECK_FALSE(load_irx(mem, bytes, {.base = 0x10000, .resolve = {}}, &err));
  CHECK(err.find("IRX") != std::string::npos);

  auto big = make_irx(0x20000);
  CHECK_FALSE(load_irx(mem, big, {.base = 0x1F0000, .resolve = {}}, &err));  // does not fit in 2 MiB
  CHECK(err.find("2 MiB") != std::string::npos);

  CHECK_FALSE(place_irx(0x1FFF00, 0x200));
  CHECK(place_irx(0x1FF00, 0x100) == 0x00020000u);
  CHECK(format_library({"libsd", 0x0104}) == "libsd v1.4");
}
