#include "iso/iso9660.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <format>

namespace lantern::iso {
namespace {

uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
         static_cast<uint32_t>(p[3]) << 24;
}
uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }

std::string trimmed(const uint8_t* p, size_t n) {
  std::string s(reinterpret_cast<const char*>(p), n);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
  return s;
}

bool iequals(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::toupper(static_cast<unsigned char>(x)) == std::toupper(static_cast<unsigned char>(y));
         });
}

std::string_view strip_version(std::string_view name) {
  if (auto semi = name.find(';'); semi != std::string_view::npos) name = name.substr(0, semi);
  if (name.size() > 1 && name.back() == '.') name.remove_suffix(1);  // "NAME." for extension-less files
  return name;
}

constexpr uint8_t kFlagDirectory = 0x02;
constexpr uint8_t kFlagMultiExtent = 0x80;
constexpr uint64_t kMaxDirectorySize = 16u << 20;  // sanity bound against corrupt images

}  // namespace

std::optional<Entry> parse_directory_record(std::span<const uint8_t> rec) {
  if (rec.size() < 34 || rec[0] < 34 || rec[0] > rec.size()) return std::nullopt;
  const uint8_t name_len = rec[32];
  if (33u + name_len > rec[0]) return std::nullopt;
  if (name_len == 1 && (rec[33] == 0 || rec[33] == 1)) return std::nullopt;  // '.' / '..'
  Entry e;
  e.lba = le32(&rec[2]);
  e.size = le32(&rec[10]);
  e.is_dir = (rec[25] & kFlagDirectory) != 0;
  e.name = std::string(strip_version(std::string_view(reinterpret_cast<const char*>(&rec[33]), name_len)));
  return e;
}

Image::~Image() {
  if (fd_ >= 0) ::close(fd_);
}

Image::Image(Image&& o) noexcept
    : fd_(o.fd_), memory_(std::move(o.memory_)), size_(o.size_), volume_(std::move(o.volume_)), root_(o.root_) {
  o.fd_ = -1;
}

Image& Image::operator=(Image&& o) noexcept {
  if (this != &o) {
    if (fd_ >= 0) ::close(fd_);
    fd_ = o.fd_;
    o.fd_ = -1;
    memory_ = std::move(o.memory_);
    size_ = o.size_;
    volume_ = std::move(o.volume_);
    root_ = o.root_;
  }
  return *this;
}

std::optional<Image> Image::open(const std::filesystem::path& path, std::string* error) {
  Image img;
  img.fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (img.fd_ < 0) {
    if (error) *error = std::format("cannot open '{}': {}", path.string(), std::strerror(errno));
    return std::nullopt;
  }
  struct stat st{};
  if (::fstat(img.fd_, &st) != 0 || !S_ISREG(st.st_mode)) {
    if (error) *error = std::format("'{}' is not a regular file", path.string());
    return std::nullopt;
  }
  img.size_ = static_cast<uint64_t>(st.st_size);
  if (!img.parse(error)) return std::nullopt;
  return img;
}

std::optional<Image> Image::from_memory(std::vector<uint8_t> bytes, std::string* error) {
  Image img;
  img.size_ = bytes.size();
  img.memory_ = std::move(bytes);
  if (!img.parse(error)) return std::nullopt;
  return img;
}

bool Image::read_raw(uint64_t offset, std::span<uint8_t> out) const {
  if (offset > size_ || out.size() > size_ - offset) return false;
  if (fd_ < 0) {
    std::memcpy(out.data(), memory_.data() + offset, out.size());
    return true;
  }
  size_t done = 0;
  while (done < out.size()) {
    ssize_t n = ::pread(fd_, out.data() + done, out.size() - done, static_cast<off_t>(offset + done));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    done += static_cast<size_t>(n);
  }
  return true;
}

bool Image::parse(std::string* error) {
  std::vector<uint8_t> sector(kSectorSize);
  for (uint32_t lba = 16; lba < 16 + 64; ++lba) {
    if (!read_raw(uint64_t{lba} * kSectorSize, sector)) break;
    if (std::memcmp(&sector[1], "CD001", 5) != 0) break;
    if (sector[0] == 255) break;  // set terminator
    if (sector[0] != 1) continue;
    volume_.system_id = trimmed(&sector[8], 32);
    volume_.volume_id = trimmed(&sector[40], 32);
    volume_.volume_blocks = le32(&sector[80]);
    volume_.block_size = le16(&sector[128]);
    if (volume_.block_size != kSectorSize) {
      if (error) *error = std::format("unsupported logical block size {}", volume_.block_size);
      return false;
    }
    std::span<const uint8_t> root_rec(&sector[156], 34);
    root_.lba = le32(&root_rec[2]);
    root_.size = le32(&root_rec[10]);
    root_.is_dir = true;
    root_.name = "";
    return true;
  }
  if (error) *error = "no ISO9660 primary volume descriptor found (not a PS2 DVD/CD image?)";
  return false;
}

std::vector<Entry> Image::list(const Entry& dir, std::string* error) const {
  std::vector<Entry> out;
  if (!dir.is_dir || dir.size > kMaxDirectorySize) {
    if (error) *error = "not a directory or directory too large";
    return out;
  }
  std::vector<uint8_t> data(static_cast<size_t>(dir.size));
  if (!read_raw(uint64_t{dir.lba} * kSectorSize, data)) {
    if (error) *error = "directory extent lies outside the image";
    return out;
  }
  size_t pos = 0;
  while (pos < data.size()) {
    uint8_t len = data[pos];
    if (len == 0) {  // records never span sectors; zero padding to next sector
      pos = (pos / kSectorSize + 1) * kSectorSize;
      continue;
    }
    if (pos + len > data.size()) break;
    std::span<const uint8_t> rec(&data[pos], len);
    if (rec.size() > 25 && (rec[25] & kFlagMultiExtent)) {
      if (error) *error = "multi-extent files are not supported";
    }
    if (auto e = parse_directory_record(rec)) out.push_back(std::move(*e));
    pos += len;
  }
  return out;
}

std::optional<Entry> Image::find(std::string_view path) const {
  Entry cur = root_;
  size_t i = 0;
  while (i < path.size()) {
    while (i < path.size() && (path[i] == '/' || path[i] == '\\')) ++i;
    if (i >= path.size()) break;
    size_t j = i;
    while (j < path.size() && path[j] != '/' && path[j] != '\\') ++j;
    std::string_view part = strip_version(path.substr(i, j - i));
    if (!cur.is_dir) return std::nullopt;
    bool found = false;
    for (auto& e : list(cur)) {
      if (iequals(e.name, part)) {
        cur = e;
        found = true;
        break;
      }
    }
    if (!found) return std::nullopt;
    i = j;
  }
  return cur;
}

bool Image::read(const Entry& file, uint64_t offset, std::span<uint8_t> out) const {
  if (offset > file.size || out.size() > file.size - offset) return false;
  return read_raw(uint64_t{file.lba} * kSectorSize + offset, out);
}

std::optional<std::vector<uint8_t>> Image::read_all(const Entry& file, uint64_t max_size) const {
  if (file.is_dir || file.size > max_size) return std::nullopt;
  std::vector<uint8_t> data(static_cast<size_t>(file.size));
  if (!read(file, 0, data)) return std::nullopt;
  return data;
}

void Image::walk(const std::function<bool(const std::string&, const Entry&)>& fn) const {
  struct Item {
    std::string path;
    Entry entry;
    int depth;
  };
  std::vector<Item> stack{{"", root_, 0}};
  while (!stack.empty()) {
    Item it = std::move(stack.back());
    stack.pop_back();
    auto children = list(it.entry);
    for (auto c = children.rbegin(); c != children.rend(); ++c) {
      std::string p = it.path + "/" + c->name;
      if (c->is_dir && it.depth < 32) stack.push_back({p, *c, it.depth + 1});
    }
    for (const auto& c : children)
      if (!fn(it.path + "/" + c.name, c)) return;
  }
}

}  // namespace lantern::iso
