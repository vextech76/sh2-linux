#include <doctest/doctest.h>

#include <memory>

#include "iop/sif_hle.h"
#include "ps2/hardware.h"

using namespace lantern;
using namespace lantern::iop;

namespace {
constexpr uint32_t kEeRecv = 0x00300000, kEeScratch = 0x00310000, kEeReply = 0x00320000;

struct Rig {
  std::unique_ptr<ee::Memory> ee = std::make_unique<ee::Memory>();
  SifHle sif{*ee};
  // Write a command packet into EE memory and "DMA" it to the IOP command buffer.
  void send(std::initializer_list<uint32_t> words) {
    uint32_t a = kEeScratch;
    for (uint32_t w : words) {
      ee->write<uint32_t>(a, w);
      a += 4;
    }
    sif.ee_to_iop(kEeScratch, SifHle::kCmdBuffer, static_cast<uint32_t>(words.size() * 4), 0);
  }
  uint32_t reply(int word) { return ee->read<uint32_t>(kEeRecv + 4u * static_cast<uint32_t>(word)); }
};
}  // namespace

TEST_CASE("INIT_CMD registers the EE buffer and RPC init is answered with SET_SREG") {
  Rig r;
  r.send({20, 0, kInitCmd, 0, kEeRecv});
  CHECK(r.sif.ee_receive_buffer() == kEeRecv);
  CHECK_FALSE(r.sif.deliver_one());
  r.send({16, 0, kInitCmd, 1});
  REQUIRE(r.sif.deliver_one());
  CHECK(r.reply(2) == kSetSreg);
  CHECK(r.reply(4) == 0);  // sreg index
  CHECK(r.reply(5) == 1);  // value
}

TEST_CASE("bind + call round trip through a registered server") {
  Rig r;
  r.send({20, 0, kInitCmd, 0, kEeRecv});
  uint32_t seen_fno = 0;
  std::vector<uint8_t> seen_args;
  r.sif.register_server(0x12345, "test", [&](RpcCall& c) {
    seen_fno = c.fno;
    seen_args = c.args;
    c.result = {0xAA, 0xBB, 0xCC, 0xDD};
  });
  // RPC_BIND: header(4) rec_id pkt_addr rpc_id client sid
  r.send({36, 0, kRpcBind, 0, 1, 0x1000, 7, 0x00400000, 0x12345});
  REQUIRE(r.sif.deliver_one());
  CHECK(r.reply(2) == kRpcEnd);
  CHECK(r.reply(7) == 0x00400000);  // client
  CHECK(r.reply(8) == kRpcBind);
  const uint32_t server = r.reply(9), buff = r.reply(10);
  CHECK(server != 0);

  // Arguments go to the server buffer, then the call packet to the command buffer.
  r.ee->write<uint32_t>(kEeScratch + 0x100, 0x11223344);
  r.sif.ee_to_iop(kEeScratch + 0x100, buff, 4, 0);
  // RPC_CALL: ..., client, rpc_number, send_size, receive, recv_size, rmode, server
  r.send({56, 0, kRpcCall, 0, 1, 0x1000, 8, 0x00400000, 3, 4, kEeReply, 4, 0, server});
  REQUIRE(r.sif.deliver_one());
  CHECK(r.reply(8) == kRpcCall);
  CHECK(seen_fno == 3);
  REQUIRE(seen_args.size() == 4);
  CHECK(seen_args[0] == 0x44);
  CHECK(r.ee->read<uint32_t>(kEeReply) == 0xDDCCBBAAu);
  CHECK(r.sif.stats().calls.at({0x12345, 3}) == 1);
}

TEST_CASE("unknown servers bind and answer with zeros (recorded as placeholders)") {
  Rig r;
  r.send({20, 0, kInitCmd, 0, kEeRecv});
  r.ee->write<uint32_t>(kEeReply, 0xFFFFFFFF);
  r.send({36, 0, kRpcBind, 0, 1, 0x1000, 7, 0x00400000, 0x80000592});
  REQUIRE(r.sif.deliver_one());
  const uint32_t server = r.reply(9);
  r.send({56, 0, kRpcCall, 0, 1, 0x1000, 8, 0x00400000, 0, 0, kEeReply, 4, 0, server});
  REQUIRE(r.sif.deliver_one());
  CHECK(r.ee->read<uint32_t>(kEeReply) == 0);
  CHECK(r.sif.stats().placeholder_calls.at(0x80000592) == 1);
}

TEST_CASE("SMFLG is write-1-to-clear and an IOP reset re-raises the boot flags on the next poll") {
  ps2::Hardware hw;
  CHECK((hw.sif_reg(4) & 0x70000) == 0x70000);
  hw.set_sif_reg(4, 0x40000);  // EE clears BOOTEND
  CHECK((hw.sif_reg(4) & 0x40000) == 0);
  hw.iop_reset();
  CHECK((hw.sif_reg(4) & 0x70000) == 0x70000);
}
