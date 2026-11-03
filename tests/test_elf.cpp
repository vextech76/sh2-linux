#include <doctest/doctest.h>

#include <cstring>
#include <vector>

#include "ee/elf.h"

using namespace lantern::ee;

namespace {
void put16(std::vector<uint8_t>& b, size_t o, uint16_t v) {
  b[o] = static_cast<uint8_t>(v);
  b[o + 1] = static_cast<uint8_t>(v >> 8);
}
void put32(std::vector<uint8_t>& b, size_t o, uint32_t v) {
  for (int i = 0; i < 4; ++i) b[o + static_cast<size_t>(i)] = static_cast<uint8_t>(v >> (8 * i));
}

// Synthetic EE-style executable: one file-backed PT_LOAD at 0x100000 plus a zero-file-size
// "overlay" PT_LOAD, and a .text section header with a string table.
std::vector<uint8_t> synthetic_elf() {
  std::vector<uint8_t> b(0x400, 0);
  std::memcpy(b.data(),
              "\x7f"
              "ELF",
              4);
  b[4] = 1;  // 32-bit
  b[5] = 1;  // little-endian
  b[6] = 1;
  put16(b, 16, 2);  // ET_EXEC
  put16(b, 18, 8);  // EM_MIPS
  put32(b, 24, 0x100008);
  put32(b, 28, 52);     // phoff
  put32(b, 32, 0x300);  // shoff
  put32(b, 36, 0x20924000);
  put16(b, 42, 32);
  put16(b, 44, 2);
  put16(b, 46, 40);
  put16(b, 48, 3);
  put16(b, 50, 2);  // shstrndx
  // PT_LOAD #0: file 0x100..0x110 -> 0x100000, memsz 0x1000
  size_t p = 52;
  put32(b, p, 1);
  put32(b, p + 4, 0x100);
  put32(b, p + 8, 0x100000);
  put32(b, p + 16, 0x10);
  put32(b, p + 20, 0x1000);
  put32(b, p + 24, 7);
  // PT_LOAD #1: overlay, no file data
  p += 32;
  put32(b, p, 1);
  put32(b, p + 4, 0x110);
  put32(b, p + 8, 0x1f0bc00);
  put32(b, p + 16, 0);
  put32(b, p + 20, 0x1380);
  // code words
  put32(b, 0x100, 0x27bdfff0);  // addiu sp,sp,-16
  put32(b, 0x104, 0x03e00008);  // jr ra
  // string table at 0x200: "\0.text\0.shstrtab\0"
  const char strtab[] = "\0.text\0.shstrtab";
  std::memcpy(&b[0x200], strtab, sizeof(strtab));
  size_t s = 0x300 + 40;  // section 1: .text
  put32(b, s, 1);
  put32(b, s + 4, 1);
  put32(b, s + 12, 0x100000);
  put32(b, s + 16, 0x100);
  put32(b, s + 20, 0x10);
  s += 40;  // section 2: .shstrtab
  put32(b, s, 7);
  put32(b, s + 4, 3);
  put32(b, s + 16, 0x200);
  put32(b, s + 20, sizeof(strtab));
  return b;
}
}  // namespace

TEST_CASE("parses headers, segments and section names") {
  std::string err;
  auto elf = ElfImage::parse(synthetic_elf(), &err);
  REQUIRE_MESSAGE(elf, err);
  CHECK(elf->type() == 2);
  CHECK(elf->entry() == 0x100008);
  REQUIRE(elf->segments().size() == 2);
  CHECK(elf->segments()[1].filesz == 0);
  REQUIRE(elf->main_segment());
  CHECK(elf->main_segment()->vaddr == 0x100000);
  REQUIRE(elf->sections().size() == 3);
  CHECK(elf->sections()[1].name == ".text");
  CHECK(elf->sections()[2].name == ".shstrtab");
}

TEST_CASE("read32 only reads file-backed bytes") {
  auto elf = ElfImage::parse(synthetic_elf(), nullptr);
  REQUIRE(elf);
  CHECK(elf->read32(0x100000) == 0x27bdfff0u);
  CHECK(elf->read32(0x100004) == 0x03e00008u);
  CHECK_FALSE(elf->read32(0x100010));   // BSS part of the segment
  CHECK_FALSE(elf->read32(0x1f0bc00));  // overlay has no file data
  CHECK_FALSE(elf->read32(0x10000e));   // straddles the end
}

TEST_CASE("rejects malformed input") {
  std::string err;
  CHECK_FALSE(ElfImage::parse({1, 2, 3}, &err));
  auto b = synthetic_elf();
  b[18] = 3;  // x86
  CHECK_FALSE(ElfImage::parse(b, &err));
  CHECK(err.find("MIPS") != std::string::npos);
  b = synthetic_elf();
  put32(b, 52 + 4, 0xFFFFFF00);  // segment offset out of range
  CHECK_FALSE(ElfImage::parse(b, &err));
}
