#include "iop/memcard.h"

#include <cstring>
#include <format>
#include <fstream>

#include "core/log.h"
#include "core/paths.h"

namespace lantern::iop {
namespace {
constexpr const char* kSub = "mc";
// On-card layout (PS2 memory card standard): page 0 holds the superblock; clusters 1.. are the
// FAT, then the root directory. Values are little-endian.
constexpr uint32_t kSuperblockPage = 0;
constexpr uint32_t kFatCluster = 1;
constexpr uint32_t kRootCluster = 3;
constexpr uint8_t kModeFile = 0x27;
constexpr uint8_t kModeDir = 0x26;
constexpr uint32_t kEntrySize = MemoryCard::kDirEntrySize;

uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
         static_cast<uint32_t>(p[3]) << 24;
}
void put32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
}  // namespace

MemoryCard::MemoryCard(std::string name) : name_(std::move(name)), data_(size_t{kPages} * kPageSize, 0) {}

bool MemoryCard::load(const std::string& path) {
  path_ = path;
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  in.read(reinterpret_cast<char*>(data_.data()), static_cast<std::streamsize>(data_.size()));
  // Superblock magic "Sony PS2 Memory Card Format" in the first 28 bytes.
  formatted_ = std::memcmp(data_.data(), "Sony PS2 Memory Card Format", 27) == 0;
  return formatted_;
}

uint32_t MemoryCard::free_clusters() const {
  if (!formatted_) return 0;
  // Walk the FAT (cluster 1) counting clusters whose "next" word is free (0x7FFFFFFF).
  uint32_t free_count = 0;
  for (uint32_t c = 1; c < kClusters; ++c) {
    const uint32_t off = (kFatCluster * kPagePerCluster * kPageSize) + c * 4;
    if (le32(&data_[off]) == 0x7FFFFFFFu) ++free_count;
  }
  return free_count;
}

bool MemoryCard::format(const std::string& path) {
  path_ = path;
  std::fill(data_.begin(), data_.end(), 0);
  uint8_t* sb = &data_[kSuperblockPage * kPageSize];
  std::memcpy(sb, "Sony PS2 Memory Card Format", 27);
  sb[0x1C] = 0x2B;                    // version 2.11
  put32(sb + 0x20, kPages);           // page length
  put32(sb + 0x24, kPagePerCluster);  // pages per cluster
  put32(sb + 0x2C, kClusters);        // number of clusters
  put32(sb + 0x30, kRootCluster);     // root dir cluster
  put32(sb + 0x34, 1);                // backup block 1
  put32(sb + 0x38, 2);                // backup block 2
  put32(sb + 0x3C, 0);                // first free
  put32(sb + 0x28, 0xFFFFFFFFu);      // (reserved)
  // FAT: every cluster free except the superblock block, FAT and root.
  uint8_t* fat = &data_[kFatCluster * kPagePerCluster * kPageSize];
  for (uint32_t c = 0; c < kClusters; ++c) put32(fat + c * 4, 0x7FFFFFFFu);
  put32(fat + kFatCluster * 4, 0xFFFFFFFFu);  // FAT is allocated
  put32(fat + kRootCluster * 4, 0xFFFFFFFFu);
  // Root directory: single entry "." (self), then free entries.
  uint8_t* root = &data_[kRootCluster * kPagePerCluster * kPageSize];
  root[0] = '.';                     // name (stops at the first zero byte)
  put32(root + 0x1C, kRootCluster);  // first cluster
  put32(root + 0x20, 2);             // size (dir = one entry here)
  root[0x3C] = kModeDir;
  for (uint32_t i = 1; i < kEntriesPerCluster; ++i) {
    uint8_t* e = root + i * kEntrySize;
    put32(e + 0x1C, 0xFFFFFFFFu);  // free entry marker
  }
  formatted_ = true;
  return save();
}

bool MemoryCard::save() {
  if (path_.empty()) return false;
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(path_).parent_path(), ec);
  std::ofstream out(path_, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(data_.data()), static_cast<std::streamsize>(data_.size()));
  return static_cast<bool>(out);
}

std::vector<MemoryCard::Entry> MemoryCard::root_entries() const {
  std::vector<Entry> out;
  if (!formatted_) return out;
  const uint8_t* root = &data_[kRootCluster * kPagePerCluster * kPageSize];
  for (uint32_t i = 0; i < kEntriesPerCluster; ++i) {
    const uint8_t* e = root + i * kEntrySize;
    if (le32(e + 0x1C) == 0xFFFFFFFFu) continue;
    Entry ent;
    ent.name.assign(reinterpret_cast<const char*>(e), strnlen(reinterpret_cast<const char*>(e), 32));
    ent.cluster = le32(e + 0x1C);
    ent.size = le32(e + 0x20);
    ent.mode = e[0x3C];
    ent.is_dir = (ent.mode & 0x0F) == 0x06;
    out.push_back(std::move(ent));
  }
  return out;
}

bool MemoryCard::find(const std::string& path, Entry* out) const {
  for (const auto& e : root_entries())
    if (e.name == path) {
      if (out) *out = e;
      return true;
    }
  return false;
}

void McService::register_with(SifHle& sif) {
  sif.register_server(0x80000400, "mcman", [this](RpcCall& c) { command(c); });
}

std::string McService::name_arg(const RpcCall& c, size_t off) const {
  std::string s;
  for (size_t i = off; i < c.args.size() && c.args[i] && s.size() < 32; ++i) s += static_cast<char>(c.args[i]);
  return s;
}

void McService::command(RpcCall& c) {
  ++stats_.commands[c.fno];
  c.result.assign(c.recv_size, 0);
  auto put = [&](size_t word, uint32_t v) {
    if ((word + 1) * 4 <= c.result.size()) std::memcpy(&c.result[word * 4], &v, 4);
  };
  const uint32_t type_ps2 = 2, format_ok = 0;
  switch (c.fno) {
    case 0xFE:    // info probe used by SLUS-20228: 12-byte reply, layout under investigation
    case 0x78: {  // GET_INFO (classic numbering)
      const uint32_t free_clusters = card_.free_clusters();
      switch (probe_layout_) {
        case 'A':
          put(0, 0);
          put(1, type_ps2);
          put(2, free_clusters);
          break;  // result, type, free
        case 'B':
          put(0, type_ps2);
          put(1, free_clusters);
          put(2, format_ok);
          break;  // type, free, format
        case 'C':
          put(0, 1);
          put(1, type_ps2);
          put(2, free_clusters);
          break;  // ok, type, free
        default:
          put(0, 0);
          put(1, free_clusters);
          put(2, type_ps2);
          break;  // result, free, type
      }
      break;
    }
    case 0x70:  // INIT
      put(0, card_.formatted() ? 1u : 0u);
      break;
    case 0x71: {  // OPEN: reply holds the file descriptor
      const std::string name = name_arg(c, 4);
      if (stats_.opened.size() < 16) stats_.opened.push_back(name);
      ++stats_.opens;
      put(0, 1);  // fd 1 (single open file; games open one at a time for saves)
      put(1, 0);
      break;
    }
    case 0x72: ++stats_.reads; break;    // CLOSE
    case 0x73: ++stats_.reads; break;    // READ: no file data modelled yet
    case 0x74: ++stats_.writes; break;   // WRITE
    case 0x75: break;                    // SEEK
    case 0x76: ++stats_.getdirs; break;  // GET_DIR: empty listing
    case 0x77:                           // FORMAT
      card_.format(resolve_paths().memcard_dir() / "card1.ps2");
      ++stats_.formats;
      break;
    default:
      if (stats_.commands[c.fno] == 1)
        log::warn(kSub, "PLACEHOLDER: mcman command 0x{:02x} not modelled yet (send {} recv {} bytes)", c.fno,
                  c.args.size(), c.recv_size);
      break;
  }
}

}  // namespace lantern::iop
