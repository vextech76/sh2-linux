#include "ee/elf.h"

#include <cstring>
#include <format>

namespace lantern::ee {
namespace {
uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
constexpr uint16_t kMachineMips = 8;
}  // namespace

std::optional<ElfImage> ElfImage::parse(std::vector<uint8_t> bytes, std::string* error) {
  auto fail = [&](std::string msg) -> std::optional<ElfImage> {
    if (error) *error = std::move(msg);
    return std::nullopt;
  };
  if (bytes.size() < 52 || std::memcmp(bytes.data(),
                                       "\x7f"
                                       "ELF",
                                       4) != 0)
    return fail("not an ELF file");
  if (bytes[4] != 1) return fail("not a 32-bit ELF");
  if (bytes[5] != 1) return fail("not little-endian");
  ElfImage img;
  const uint8_t* h = bytes.data();
  img.type_ = le16(h + 16);
  img.machine_ = le16(h + 18);
  img.entry_ = load_le32(h + 24);
  const uint32_t phoff = load_le32(h + 28), shoff = load_le32(h + 32);
  img.flags_ = load_le32(h + 36);
  const uint16_t phentsize = le16(h + 42), phnum = le16(h + 44);
  const uint16_t shentsize = le16(h + 46), shnum = le16(h + 48), shstrndx = le16(h + 50);
  if (img.machine_ != kMachineMips) return fail(std::format("ELF machine {} is not MIPS", img.machine_));

  if (phnum && (phentsize < 32 || uint64_t{phoff} + uint64_t{phnum} * phentsize > bytes.size()))
    return fail("program header table out of range");
  for (uint16_t i = 0; i < phnum; ++i) {
    const uint8_t* p = h + phoff + size_t{i} * phentsize;
    ElfSegment s{load_le32(p),      load_le32(p + 4),  load_le32(p + 8), load_le32(p + 16),
                 load_le32(p + 20), load_le32(p + 24), load_le32(p + 28)};
    if (s.filesz && uint64_t{s.offset} + s.filesz > bytes.size()) return fail("segment data out of range");
    img.segments_.push_back(s);
  }

  if (shnum && shentsize >= 40 && uint64_t{shoff} + uint64_t{shnum} * shentsize <= bytes.size()) {
    std::vector<std::pair<uint32_t, ElfSection>> raw;
    for (uint16_t i = 0; i < shnum; ++i) {
      const uint8_t* p = h + shoff + size_t{i} * shentsize;
      ElfSection s;
      s.type = load_le32(p + 4);
      s.flags = load_le32(p + 8);
      s.addr = load_le32(p + 12);
      s.offset = load_le32(p + 16);
      s.size = load_le32(p + 20);
      s.link = load_le32(p + 24);
      s.info = load_le32(p + 28);
      s.entsize = load_le32(p + 36);
      raw.emplace_back(load_le32(p), s);
    }
    const ElfSection* strtab = shstrndx < raw.size() ? &raw[shstrndx].second : nullptr;
    for (auto& [name_off, s] : raw) {
      if (strtab && strtab->type == 3 && name_off < strtab->size &&
          uint64_t{strtab->offset} + strtab->size <= bytes.size()) {
        const char* base = reinterpret_cast<const char*>(h + strtab->offset);
        s.name.assign(base + name_off, strnlen(base + name_off, strtab->size - name_off));
      }
      img.sections_.push_back(std::move(s));
    }
  }
  img.bytes_ = std::move(bytes);
  return img;
}

std::span<const uint8_t> ElfImage::segment_data(const ElfSegment& s) const {
  return std::span<const uint8_t>(bytes_).subspan(s.offset, s.filesz);
}

std::span<const uint8_t> ElfImage::section_data(const ElfSection& s) const {
  if (s.type == 8 /* NOBITS */ || uint64_t{s.offset} + s.size > bytes_.size()) return {};
  return std::span<const uint8_t>(bytes_).subspan(s.offset, s.size);
}

std::optional<uint32_t> ElfImage::read32(uint32_t vaddr) const {
  for (const auto& s : segments_) {
    if (s.type != 1 || vaddr < s.vaddr || vaddr - s.vaddr + 4 > s.filesz) continue;
    return load_le32(bytes_.data() + s.offset + (vaddr - s.vaddr));
  }
  return std::nullopt;
}

const ElfSegment* ElfImage::main_segment() const {
  const ElfSegment* best = nullptr;
  for (const auto& s : segments_)
    if (s.type == 1 && (!best || s.filesz > best->filesz)) best = &s;
  return best;
}

}  // namespace lantern::ee
