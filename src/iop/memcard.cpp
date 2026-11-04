#include "iop/memcard.h"

#include <cstdlib>
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
  // Argument buffer: mcDescParam_t (48 bytes) { fd, port, slot, size, offset, origin, buffer, param, data[16] }.
  // Reply: mcRpcStat_t { result, mcserv_version, mcman_version }. Commands that return data also
  // write an mcEndParam_t / mcEndParam2_t to the EE address in `param`, which is how the caller
  // learns the card type, free space, formatted flag and transfer results.
  c.result.assign(std::max<size_t>(c.recv_size, 12), 0);
  auto arg = [&](size_t word) {
    uint32_t v = 0;
    if ((word + 1) * 4 <= c.args.size()) std::memcpy(&v, &c.args[word * 4], 4);
    return v;
  };
  auto put = [&](size_t word, uint32_t v) {
    if ((word + 1) * 4 <= c.result.size()) std::memcpy(&c.result[word * 4], &v, 4);
  };
  const uint32_t port = arg(1), slot = arg(2), param = arg(7);
  constexpr uint32_t kMcservVersion = 0x213, kMcmanVersion = 0x223;  // versions of the disc's MCSERV/MCMAN
  constexpr uint32_t kTypePs2 = 2;                                   // mcGetMcType: 2 = PS2 card

  // End parameter structures (see libmc-common.h): classic 64 bytes, "2" variant 192 bytes with
  // the formatted flag at offset 144.
  auto write_end_param = [&](uint32_t dest, uint32_t type, uint32_t free_clusters, bool extended) {
    if (!dest) return;
    const uint32_t bytes = extended ? 192 : 64;
    std::vector<uint8_t> buf(bytes, 0);
    auto w32 = [&](size_t off, uint32_t v) { std::memcpy(&buf[off], &v, 4); };
    w32(0, type);
    w32(4, free_clusters);
    if (extended) w32(144, card_.formatted() ? 0u : 1u);  // 0 = formatted
    ee_.load(dest, buf.data(), buf.size());
  };

  switch (c.fno) {
    case 0xFE:  // XMCSERV INIT: the caller verifies the mcserv/mcman feature levels
      put(0, 0);
      put(1, kMcservVersion);
      put(2, kMcmanVersion);
      break;
    case 0x70:  // classic INIT: 4-byte reply only
      put(0, 0);
      break;
    case 0x01:  // XMCSERV GETINFO -> mcEndParam2_t at `param`
    case 0x78:  // classic GETINFO -> mcEndParam_t at `param`
      log::info(kSub, "getinfo port {} slot {}: type {} free {}KB formatted {}", static_cast<int>(port),
                static_cast<int>(slot), kTypePs2, card_.free_clusters(), card_.formatted() ? "yes" : "no");
      write_end_param(param, card_.formatted() ? kTypePs2 : 0u, card_.free_clusters(), c.fno == 0x01);
      if (card_.formatted()) stat_calls_++;
      put(0, 0);
      break;
    case 0x71: {  // OPEN: reply result = fd (the fd also travels in the end param for some libs)
      const std::string name = name_arg(c, 8);  // mcNameParam_t style buffer
      if (stats_.opened.size() < 16) stats_.opened.push_back(name);
      ++stats_.opens;
      put(0, 1);
      write_end_param(param, 1, 0, false);
      break;
    }
    case 0x72:
      ++stats_.reads;
      put(0, 0);
      break;    // CLOSE
    case 0x76:  // classic GETDIR
    case 0x0D:  // XMCSERV GETDIR
      ++stats_.getdirs;
      put(0, 0);
      break;
    case 0x77:  // classic FORMAT
    case 0x10:  // XMCSERV FORMAT
      card_.format(resolve_paths().memcard_dir() / "card1.ps2");
      ++stats_.formats;
      put(0, 0);
      break;
    case 0x73:
      put(0, 0);
      ++stats_.reads;
      break;  // READ  (file data not modelled yet)
    case 0x74:
      put(0, 0);
      ++stats_.writes;
      break;                      // WRITE
    case 0x75: put(0, 0); break;  // SEEK
    case 0x7A: put(0, 0); break;  // FLUSH
    default:
      if (stats_.commands[c.fno] == 1)
        log::warn(kSub, "PLACEHOLDER: mcserv command 0x{:02x} not modelled yet (send {} recv {} bytes)", c.fno,
                  c.args.size(), c.recv_size);
      break;
  }
}

}  // namespace lantern::iop
