#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lantern::iso {

inline constexpr uint32_t kSectorSize = 2048;

struct Entry {
  std::string name;  // without ";1" version suffix
  uint32_t lba = 0;
  uint64_t size = 0;
  bool is_dir = false;
};

struct VolumeInfo {
  std::string system_id;
  std::string volume_id;
  uint32_t volume_blocks = 0;
  uint16_t block_size = kSectorSize;
};

// Read-only ISO9660 reader. The image is opened O_RDONLY and only ever read with pread();
// there is no code path that can write to it.
class Image {
 public:
  ~Image();
  Image(Image&&) noexcept;
  Image& operator=(Image&&) noexcept;
  Image(const Image&) = delete;
  Image& operator=(const Image&) = delete;

  static std::optional<Image> open(const std::filesystem::path& path, std::string* error);
  // For tests: parse an in-memory image.
  static std::optional<Image> from_memory(std::vector<uint8_t> bytes, std::string* error);

  const VolumeInfo& volume() const { return volume_; }
  uint64_t image_size() const { return size_; }
  const Entry& root() const { return root_; }

  std::vector<Entry> list(const Entry& dir, std::string* error = nullptr) const;
  // Case-insensitive, '/' or '\\' separated, optional leading separator, optional ";1".
  std::optional<Entry> find(std::string_view path) const;
  bool read(const Entry& file, uint64_t offset, std::span<uint8_t> out) const;
  std::optional<std::vector<uint8_t>> read_all(const Entry& file, uint64_t max_size) const;
  bool read_raw(uint64_t offset, std::span<uint8_t> out) const;

  // Visits every entry (directory by directory); `path` uses '/' and starts with '/'.
  // Return false to stop.
  void walk(const std::function<bool(const std::string& path, const Entry& entry)>& fn) const;

 private:
  Image() = default;
  bool parse(std::string* error);

  int fd_ = -1;
  std::vector<uint8_t> memory_;
  uint64_t size_ = 0;
  VolumeInfo volume_;
  Entry root_;
};

// Parse one ISO9660 directory record. Exposed for tests. Returns nullopt for '.'/'..'.
std::optional<Entry> parse_directory_record(std::span<const uint8_t> rec);

}  // namespace lantern::iso
