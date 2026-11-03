#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lantern::ee {

struct ElfSegment {
  uint32_t type = 0;  // PT_LOAD = 1
  uint32_t offset = 0;
  uint32_t vaddr = 0;
  uint32_t filesz = 0;
  uint32_t memsz = 0;
  uint32_t flags = 0;
  uint32_t align = 0;
};

struct ElfSection {
  std::string name;
  uint32_t type = 0;
  uint32_t flags = 0;
  uint32_t addr = 0;
  uint32_t offset = 0;
  uint32_t size = 0;
};

// 32-bit little-endian MIPS ELF (EE executables and IOP IRX modules). Read-only view over a
// caller-owned byte buffer copy.
class ElfImage {
 public:
  static std::optional<ElfImage> parse(std::vector<uint8_t> bytes, std::string* error);

  uint16_t type() const { return type_; }  // 2 = EXEC, 0xFF80 = PS2 IRX
  uint16_t machine() const { return machine_; }
  uint32_t entry() const { return entry_; }
  uint32_t flags() const { return flags_; }
  const std::vector<ElfSegment>& segments() const { return segments_; }
  const std::vector<ElfSection>& sections() const { return sections_; }
  std::span<const uint8_t> bytes() const { return bytes_; }

  // File-backed bytes of a segment (filesz bytes).
  std::span<const uint8_t> segment_data(const ElfSegment& s) const;
  std::span<const uint8_t> section_data(const ElfSection& s) const;
  // Read a 32-bit word at a virtual address inside a file-backed part of a PT_LOAD segment.
  std::optional<uint32_t> read32(uint32_t vaddr) const;
  // Most common load address of PT_LOAD segments with file data = the main image.
  const ElfSegment* main_segment() const;

 private:
  std::vector<uint8_t> bytes_;
  uint16_t type_ = 0, machine_ = 0;
  uint32_t entry_ = 0, flags_ = 0;
  std::vector<ElfSegment> segments_;
  std::vector<ElfSection> sections_;
};

inline uint32_t load_le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
         static_cast<uint32_t>(p[3]) << 24;
}

}  // namespace lantern::ee
