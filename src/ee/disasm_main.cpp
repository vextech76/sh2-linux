// lantern-disasm: disassemble raw little-endian R5900 words, or an ELF's main segment.
//   lantern-disasm raw FILE [BASE_HEX]
//   lantern-disasm elf FILE [START_HEX COUNT]
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "ee/elf.h"
#include "ee/r5900.h"

using namespace lantern::ee;

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fputs("usage: lantern-disasm raw FILE [BASE_HEX] | elf FILE [START_HEX COUNT]\n", stderr);
    return 2;
  }
  std::ifstream in(argv[2], std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
  if (!in && bytes.empty()) {
    std::fprintf(stderr, "cannot read %s\n", argv[2]);
    return 1;
  }
  const std::string mode = argv[1];
  if (mode == "raw") {
    uint32_t base = argc > 3 ? static_cast<uint32_t>(std::stoul(argv[3], nullptr, 16)) : 0;
    for (size_t i = 0; i + 4 <= bytes.size(); i += 4) {
      uint32_t pc = base + static_cast<uint32_t>(i);
      Insn insn = decode(load_le32(&bytes[i]), pc);
      std::printf("%8x:\t%08x \t%s\n", pc, insn.word, disassemble(insn).c_str());
    }
    return 0;
  }
  if (mode == "elf") {
    std::string err;
    auto elf = ElfImage::parse(std::move(bytes), &err);
    if (!elf) {
      std::fprintf(stderr, "%s\n", err.c_str());
      return 1;
    }
    const ElfSegment* seg = elf->main_segment();
    if (!seg) return 1;
    uint32_t start = argc > 3 ? static_cast<uint32_t>(std::stoul(argv[3], nullptr, 16)) : seg->vaddr;
    uint32_t count = argc > 4 ? static_cast<uint32_t>(std::stoul(argv[4])) : seg->filesz / 4;
    for (uint32_t i = 0; i < count; ++i) {
      auto w = elf->read32(start + i * 4);
      if (!w) break;
      std::printf("%8x:\t%08x \t%s\n", start + i * 4, *w, disassemble(decode(*w, start + i * 4)).c_str());
    }
    return 0;
  }
  return 2;
}
