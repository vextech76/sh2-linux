#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ee/memory.h"
#include "iop/sif_hle.h"
#include "iso/iso9660.h"

namespace lantern::iop {

// CDVDFSV RPC servers (protocol per ps2sdk ee/rpc/cdvd, compatible with SCE libcdvd), served from
// the user's disc image opened read-only:
//   0x80000592 init, 0x80000593 S-commands, 0x80000595 N-commands (read/stream),
//   0x80000596 power-off notifier, 0x80000597 search file, 0x8000059A disk ready.
class CdvdService {
 public:
  CdvdService(ee::Memory& ee, const iso::Image& disc) : ee_(ee), disc_(disc) {}
  void register_with(SifHle& sif);

  struct Stats {
    uint64_t sectors_read = 0, reads = 0, stream_reads = 0, searches = 0, search_misses = 0;
    std::vector<std::string> searched;  // first few file names looked up
  };
  const Stats& stats() const { return stats_; }

 private:
  void init(RpcCall& c);
  void scmd(RpcCall& c);
  void ncmd(RpcCall& c);
  void search(RpcCall& c);
  // Copy `sectors` 2048-byte sectors starting at `lbn` to EE address `dest`; returns sectors read.
  uint32_t read_sectors(uint32_t lbn, uint32_t sectors, uint32_t dest);

  ee::Memory& ee_;
  const iso::Image& disc_;
  uint32_t stream_pos_ = 0;
  Stats stats_;
};

// IOP heap RPC server (0x80000003): sceSifAllocIopHeap / sceSifFreeIopHeap backed by a bump
// allocator over the modelled IOP RAM.
class IopHeapService {
 public:
  void register_with(SifHle& sif);
  uint64_t allocated() const { return next_ - SifHle::kHeapBase; }

 private:
  uint32_t next_ = SifHle::kHeapBase;
};

// LOADFILE RPC server (0x80000006): the HLE IOP already provides every service the game needs at
// the SIF boundary, so module loads are acknowledged (and recorded) instead of executed.
class LoadfileService {
 public:
  void register_with(SifHle& sif);
  const std::vector<std::string>& modules() const { return modules_; }
  void set_version(std::string v) { version_ = std::move(v); }

 private:
  std::vector<std::string> modules_;
  int next_id_ = 1;
  std::string version_ = "2240";
};

}  // namespace lantern::iop
