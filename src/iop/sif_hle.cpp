#include "iop/sif_hle.h"

#include <cstring>
#include <format>

#include "core/log.h"
#include "iop/iop_runtime.h"

namespace lantern::iop {
namespace {
constexpr const char* kSub = "sif";
constexpr uint32_t kServerBufferBase = SifHle::kServerBufferBase, kServerBufferStride = SifHle::kServerBufferStride;
}  // namespace

std::string sif_command_name(uint32_t cid) {
  switch (cid) {
    case kChangeSaddr: return "CHANGE_SADDR";
    case kSetSreg: return "SET_SREG";
    case kInitCmd: return "INIT_CMD";
    case kReset: return "RESET";
    case kRpcEnd: return "RPC_END";
    case kRpcBind: return "RPC_BIND";
    case kRpcCall: return "RPC_CALL";
    case kRpcRdata: return "RPC_RDATA";
    default: return std::format("0x{:08x}", cid);
  }
}

std::string SifHle::server_name(uint32_t sid) const {
  auto it = servers_.find(sid);
  return it == servers_.end() ? std::format("unknown 0x{:08x}", sid) : std::format("{} 0x{:08x}", it->second.name, sid);
}

uint8_t SifHle::iop_read8(uint32_t a) const {
  if (iop_) return iop_->mem().read<uint8_t>(a & (kIopRamSize - 1));
  return iop_ram_[a % kIopRamSize];
}

void SifHle::iop_write8(uint32_t a, uint8_t v) {
  if (iop_)
    iop_->mem().write<uint8_t>(a & (kIopRamSize - 1), v);
  else
    iop_ram_[a % kIopRamSize] = v;
}

uint32_t SifHle::iop32(uint32_t a) const {
  if (iop_) return iop_->mem().read<uint32_t>(a & (kIopRamSize - 1));
  uint32_t v;
  std::memcpy(&v, &iop_ram_[a % kIopRamSize], 4);
  return v;
}

void SifHle::ee_to_iop(uint32_t src, uint32_t dest, uint32_t size, uint32_t) {
  dest &= kIopRamSize - 1;
  for (uint32_t i = 0; i < size; ++i) iop_write8(dest + i, ee_.read<uint8_t>(src + i));
  if (dest == kCmdBuffer && size >= 16) handle_command(dest);
}

void SifHle::queue(std::vector<uint32_t> words) {
  // Header word 0: psize (bytes) in the low 8 bits, dsize 0.
  words[0] = static_cast<uint32_t>(words.size() * 4) & 0xFF;
  outbox_.push_back(std::move(words));
}

void SifHle::rpc_end(uint32_t pkt, uint32_t cid, uint32_t server, uint32_t buff) {
  // SifRpcRendPkt: header(4), rec_id, pkt_addr, rpc_id, client, cid, server, buff, cbuff
  queue(
      {0, 0, kRpcEnd, 0, iop32(pkt + 16), iop32(pkt + 20), iop32(pkt + 24), iop32(pkt + 28), cid, server, buff, buff});
}

void SifHle::handle_command(uint32_t a) {
  const uint32_t cid = iop32(a + 8), opt = iop32(a + 12);
  ++stats_.commands[cid];
  switch (cid) {
    case kInitCmd:
      if (opt == 0) {
        ee_buf_ = iop32(a + 16);  // EE command receive buffer
        log::info(kSub, "EE command receive buffer at 0x{:08x}", ee_buf_);
      } else {
        queue({0, 0, kSetSreg, 0, 0, 1});  // RPC system ready: sreg[0] = 1
      }
      return;
    case kChangeSaddr:
    case kSetSreg: return;
    case kReset:
      log::info(kSub, "IOP reset requested (HLE IOP stays booted; RPC bindings cleared)");
      bindings_.clear();
      outbox_.clear();
      if (reset_hook_) reset_hook_();
      return;
    case kRpcBind: {
      const uint32_t sid = iop32(a + 32);
      ++stats_.binds[sid];
      const RpcServerInfo* real = iop_ ? iop_->find_rpc(sid) : nullptr;
      uint32_t handle = 0, buff = 0;
      if (real) {
        handle = real->struct_addr;
        buff = real->buff;
      } else {
        handle = next_handle_;
        next_handle_ += 0x40;
        buff = kServerBufferBase + static_cast<uint32_t>(bindings_.size() % 16) * kServerBufferStride;
      }
      bindings_[handle] = {sid, buff};
      log::info(kSub, "RPC bind {} -> server 0x{:08x}{}", server_name(sid), handle,
                real                     ? " (real IOP module)"
                : servers_.contains(sid) ? ""
                                         : " (PLACEHOLDER: no HLE server, calls answered with zeros)");
      rpc_end(a, kRpcBind, handle, buff);
      return;
    }
    case kRpcCall: {
      // SifRpcCallPkt: ..., client(28), rpc_number(32), send_size(36), receive(40), recv_size(44),
      // rmode(48), server(52)
      const uint32_t fno = iop32(a + 32), send_size = iop32(a + 36), receive = iop32(a + 40), recv_size = iop32(a + 44),
                     server = iop32(a + 52);
      auto bit = bindings_.find(server);
      const uint32_t sid = bit == bindings_.end() ? 0 : bit->second.sid;
      const uint32_t buff = bit == bindings_.end() ? kServerBufferBase : bit->second.buff;
      ++stats_.calls[{sid, fno}];
      RpcCall call{sid, fno, {}, {}, recv_size};
      // Arguments were DMA'd to the server buffer returned at bind time.
      call.args.resize(std::min(send_size, kServerBufferStride));
      for (size_t i = 0; i < call.args.size(); ++i) call.args[i] = iop_ram_[(buff + i) % kIopRamSize];
      if (log::min_level() <= log::Level::Debug && stats_.calls[{sid, fno}] <= 3) {
        std::string hex, txt;
        for (size_t i = 0; i < std::min<size_t>(call.args.size(), 96); ++i) {
          hex += std::format("{:02x}", call.args[i]);
          txt += call.args[i] >= 0x20 && call.args[i] < 0x7F ? static_cast<char>(call.args[i]) : '.';
        }
        log::debug(kSub, "RPC {} fn {} send {} recv {} buff 0x{:x} args {} |{}|", server_name(sid), fno, send_size,
                   recv_size, buff, hex, txt);
      }
      if (const RpcServerInfo* real = iop_ ? iop_->find_rpc(sid) : nullptr) {
        // The EE wrote the send buffer to the address returned at bind time; make sure the real
        // server's own buffer has the same bytes.
        const uint32_t dst = real->buff ? real->buff : buff;
        if (dst != buff)
          for (uint32_t i = 0; i < send_size; ++i) iop_write8(dst + i, iop_read8(buff + i));
        std::vector<uint8_t> reply;
        std::string rerr;
        if (iop_->call_rpc(*real, fno, send_size, recv_size, &reply, &rerr)) {
          ++stats_.real_calls[sid];
          if (receive && recv_size)
            for (uint32_t i = 0; i < recv_size; ++i) ee_.write<uint8_t>(receive + i, reply[i]);
        } else {
          ++stats_.real_failures;
          log::warn(kSub, "real IOP RPC {} function {} failed: {}", server_name(sid), fno, rerr);
          if (receive && recv_size)
            for (uint32_t i = 0; i < recv_size; ++i) ee_.write<uint8_t>(receive + i, 0);
        }
        rpc_end(a, kRpcCall, server, buff);
        return;
      }
      if (auto it = servers_.find(sid); it != servers_.end()) {
        it->second.fn(call);
      } else if (placeholder_ && (sid == 0x15 || sid == 0x80000701 || sid == 0x80000101)) {
        // PLACEHOLDER sound driver: report "ready" so the guest's audio init can complete. No audio
        // is produced (the game's own sound modules and SPU2 are Stage 9).
        if (stats_.placeholder_calls[sid]++ == 0)
          log::warn(kSub, "PLACEHOLDER: sound driver 0x{:08x} answers \"ready\" (no audio output yet)", sid);
        if (!call.result.empty()) std::memset(call.result.data(), 0, call.result.size());
        if (call.result.size() >= 4) call.result[0] = 1;
      } else if (stats_.placeholder_calls[sid]++ < 3) {
        log::warn(kSub, "PLACEHOLDER: RPC {} function {} answered with zeros ({} bytes)", server_name(sid), fno,
                  recv_size);
      }
      if (receive && recv_size) {
        call.result.resize(recv_size);
        for (uint32_t i = 0; i < recv_size; ++i) ee_.write<uint8_t>(receive + i, call.result[i]);
      }
      rpc_end(a, kRpcCall, server, buff);
      return;
    }
    case kRpcRdata: {
      // ..., client(28), src (IOP)(32), dest (EE)(36), size(40)
      const uint32_t src = iop32(a + 32), dst = iop32(a + 36), size = iop32(a + 40);
      for (uint32_t i = 0; i < size; ++i) ee_.write<uint8_t>(dst + i, iop_ram_[(src + i) % kIopRamSize]);
      rpc_end(a, kRpcRdata, 0, 0);
      return;
    }
    default: log::warn(kSub, "unhandled SIF command {}", sif_command_name(cid)); return;
  }
}

bool SifHle::deliver_one() {
  if (outbox_.empty() || !ee_buf_) return false;
  const auto words = std::move(outbox_.front());
  outbox_.pop_front();
  for (size_t i = 0; i < words.size(); ++i) ee_.write<uint32_t>(ee_buf_ + static_cast<uint32_t>(i * 4), words[i]);
  ++stats_.replies;
  return true;
}

}  // namespace lantern::iop
