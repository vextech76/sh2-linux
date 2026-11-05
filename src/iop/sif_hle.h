#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "ee/memory.h"
#include "ps2/kernel.h"

namespace lantern::iop {

class IopRuntime;

// SIF command IDs (identical in SCE libsif and ps2sdk).
enum SifCmd : uint32_t {
  kChangeSaddr = 0x80000000,
  kSetSreg = 0x80000001,
  kInitCmd = 0x80000002,
  kReset = 0x80000003,
  kRpcEnd = 0x80000008,
  kRpcBind = 0x80000009,
  kRpcCall = 0x8000000A,
  kRpcRdata = 0x8000000C,
};

// One RPC call as seen by an HLE server.
struct RpcCall {
  uint32_t sid = 0;
  uint32_t fno = 0;
  std::vector<uint8_t> args;    // data the EE sent (send_size bytes from the server buffer)
  std::vector<uint8_t> result;  // reply data copied to the EE receive buffer (recv_size max)
  uint32_t recv_size = 0;
};
using RpcServer = std::function<void(RpcCall&)>;

// High-level IOP at the SIF boundary: receives EE->IOP DMA into a modelled IOP RAM, executes SIF
// commands and RPCs, and queues replies that are delivered into the EE receive buffer one
// packet per SIF0 DMA interrupt (so the game's own libsif handler processes them).
class SifHle final : public ps2::SifEndpoint {
 public:
  static constexpr uint32_t kIopRamSize = 2u << 20;
  // Modelled IOP RAM map: command buffer, 16 RPC server buffers (16 KB each), then heap.
  static constexpr uint32_t kCmdBuffer = 0x000A0000;  // matches Hardware's SMCOM value
  // Below the real IOP heap (0x100000..0x1F0000) used by the sound modules.
  static constexpr uint32_t kServerBufferBase = 0x000A1000, kServerBufferStride = 0x4000;
  static constexpr uint32_t kHeapBase = 0x000E1000, kHeapEnd = 0x00100000;

  explicit SifHle(ee::Memory& ee_mem) : ee_(ee_mem), iop_ram_(kIopRamSize) {}

  void set_reset_hook(std::function<void()> fn) { reset_hook_ = std::move(fn); }
  // Route EE→IOP memory and registered RPC servers through a real IOP runtime.
  void set_iop(IopRuntime* iop) { iop_ = iop; }
  // Answer otherwise-unserved sound-driver RPCs with "ready" (PLACEHOLDER: no audio output).
  void set_sound_placeholder(bool on) { placeholder_ = on; }
  void register_server(uint32_t sid, std::string name, RpcServer fn) {
    servers_[sid] = {std::move(name), std::move(fn)};
  }

  void ee_to_iop(uint32_t src, uint32_t iop_dest, uint32_t size, uint32_t attr) override;
  bool deliver_one() override;

  struct Stats {
    std::map<uint32_t, uint64_t> commands;                    // cid -> count
    std::map<uint32_t, uint64_t> binds;                       // sid -> count
    std::map<std::pair<uint32_t, uint32_t>, uint64_t> calls;  // (sid, fno) -> count
    std::map<uint32_t, uint64_t> placeholder_calls;           // sid -> calls answered with zeros
    std::map<uint32_t, uint64_t> real_calls;                  // sid -> calls served by a real IOP module
    uint64_t real_failures = 0;
    uint64_t replies = 0;
  };
  const Stats& stats() const { return stats_; }
  std::string server_name(uint32_t sid) const;
  uint32_t ee_receive_buffer() const { return ee_buf_; }

 private:
  uint32_t iop32(uint32_t a) const;
  uint8_t iop_read8(uint32_t a) const;
  void iop_write8(uint32_t a, uint8_t v);
  void handle_command(uint32_t addr);
  void queue(std::vector<uint32_t> words);
  void rpc_end(uint32_t pkt, uint32_t cid, uint32_t server, uint32_t buff);

  ee::Memory& ee_;
  std::vector<uint8_t> iop_ram_;
  uint32_t ee_buf_ = 0;
  std::deque<std::vector<uint32_t>> outbox_;
  struct Server {
    std::string name;
    RpcServer fn;
  };
  std::map<uint32_t, Server> servers_;
  struct Binding {
    uint32_t sid, buff;
  };
  std::map<uint32_t, Binding> bindings_;  // fake IOP server-struct address -> server
  uint32_t next_handle_ = 0x00180000;
  Stats stats_;
  std::function<void()> reset_hook_;
  IopRuntime* iop_ = nullptr;
  bool placeholder_ = true;
};

std::string sif_command_name(uint32_t cid);

}  // namespace lantern::iop
