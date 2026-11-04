#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "ee/memory.h"
#include "iop/sif_hle.h"

namespace lantern::iop {

// A PlayStation 2 memory card as a file on the host. The card is 8 MB of user data in 512-byte
// pages (16 ECC bytes per page on real hardware are not stored here); the on-card filesystem
// (superblock, FAT, directory entries) is modelled so the game's own checks and saves work.
//
// Storage: $XDG_DATA_HOME/lantern/memcards/<name>.ps2 (raw 512-byte pages). Nothing is written
// until the guest formats or saves.
class MemoryCard {
 public:
  static constexpr uint32_t kPageSize = 512;
  static constexpr uint32_t kPages = 16384;       // 8 MB
  static constexpr uint32_t kPagePerCluster = 2;  // 1 KB clusters
  static constexpr uint32_t kClusters = kPages / kPagePerCluster;
  static constexpr uint32_t kDirEntrySize = 512;
  static constexpr uint32_t kEntriesPerCluster = kPagePerCluster * kPageSize / kDirEntrySize;

  explicit MemoryCard(std::string name = "card1");

  bool load(const std::string& path);
  // Write a freshly formatted card (superblock, FAT, root directory) and save it.
  bool format(const std::string& path);
  bool save();

  bool formatted() const { return formatted_; }
  // Clusters still free (the unit mcGetInfo reports; 1 cluster = 1 KB).
  uint32_t free_clusters() const;
  uint32_t total_clusters() const { return kClusters; }

  struct Entry {
    std::string name;
    uint32_t size = 0;     // bytes
    uint32_t cluster = 0;  // first cluster (0 = none)
    uint8_t mode = 0;      // 0x8427 file / 0x8426 dir (PS2 "PS2" mode words)
    bool is_dir = false;
  };
  // Entries of the root directory (read from the on-card directory area).
  std::vector<Entry> root_entries() const;
  bool find(const std::string& path, Entry* out) const;

  const std::string& path() const { return path_; }
  const std::vector<uint8_t>& bytes() const { return data_; }

 private:
  std::string name_;
  std::string path_;
  std::vector<uint8_t> data_;
  bool formatted_ = false;
};

// mcman RPC server (0x80000400) following the classic SCE command numbers used by SLUS-20228
// (0x70..0x80, with 0xFE as the info probe). Unimplemented commands are answered with zeros and
// logged once per command number, so nothing is silent.
class McService {
 public:
  McService(ee::Memory& ee, MemoryCard& card) : ee_(ee), card_(card) {}
  void register_with(SifHle& sif);
  // Experiment hook: selects the reply layout of the info probe (see memcard.cpp).
  void set_probe_layout(char layout) { probe_layout_ = layout; }

  struct Stats {
    std::map<uint32_t, uint64_t> commands;  // function number -> calls
    uint32_t opens = 0, reads = 0, writes = 0, getdirs = 0, formats = 0;
    std::vector<std::string> opened;  // first few names opened by the game
  };
  const Stats& stats() const { return stats_; }

 private:
  void command(RpcCall& c);
  std::string name_arg(const RpcCall& c, size_t off) const;

  ee::Memory& ee_;
  MemoryCard& card_;
  Stats stats_;
  char probe_layout_ = 'A';
};

}  // namespace lantern::iop
