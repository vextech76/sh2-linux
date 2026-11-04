#include <doctest/doctest.h>

#include <cstring>
#include <memory>
#include <string>

#include "iop/services.h"
#include "iop/sif_hle.h"
#include "iso_builder.h"

using namespace lantern;
using namespace lantern::iop;

namespace {
constexpr uint32_t kEeRecv = 0x00300000, kEeScratch = 0x00310000, kEeReply = 0x00320000, kEeData = 0x00330000;

struct Rig {
  std::unique_ptr<ee::Memory> ee = std::make_unique<ee::Memory>();
  SifHle sif{*ee};
  Rig() { send({20, 0, kInitCmd, 0, kEeRecv}); }
  void send(std::initializer_list<uint32_t> words) {
    uint32_t a = kEeScratch;
    for (uint32_t w : words) {
      ee->write<uint32_t>(a, w);
      a += 4;
    }
    sif.ee_to_iop(kEeScratch, SifHle::kCmdBuffer, static_cast<uint32_t>(words.size() * 4), 0);
  }
  uint32_t reply(int word) { return ee->read<uint32_t>(kEeRecv + 4u * static_cast<uint32_t>(word)); }
  // Bind `sid`, returns {server handle, server buffer}.
  std::pair<uint32_t, uint32_t> bind(uint32_t sid) {
    send({36, 0, kRpcBind, 0, 1, 0x1000, 7, 0x00400000, sid});
    REQUIRE(sif.deliver_one());
    return {reply(9), reply(10)};
  }
  // Call with `args` (bytes) DMA'd to the server buffer first.
  void call(std::pair<uint32_t, uint32_t> srv, uint32_t fno, const std::vector<uint8_t>& args, uint32_t recv_size) {
    ee->load(kEeScratch + 0x1000, args.data(), args.size());
    sif.ee_to_iop(kEeScratch + 0x1000, srv.second, static_cast<uint32_t>(args.size()), 0);
    send({56, 0, kRpcCall, 0, 1, 0x1000, 8, 0x00400000, fno, static_cast<uint32_t>(args.size()), kEeReply, recv_size, 0,
          srv.first});
    REQUIRE(sif.deliver_one());
  }
};

std::vector<uint8_t> words(std::initializer_list<uint32_t> w) {
  std::vector<uint8_t> out(w.size() * 4);
  size_t i = 0;
  for (uint32_t v : w) std::memcpy(&out[4 * i++], &v, 4);
  return out;
}

iso::Image disc() {
  test::IsoBuilder b;
  b.add_file("SYSTEM.CNF", "BOOT2 = cdrom0:\\SLUS_000.00;1\n");
  b.add_file("DATA/A.BIN", std::string(3000, 'Q'));
  return *iso::Image::from_memory(b.build(), nullptr);
}
}  // namespace

TEST_CASE("CD search (SCE 296-byte layout) then sector read into EE memory") {
  Rig r;
  auto img = disc();
  CdvdService cd(*r.ee, img);
  cd.register_with(r.sif);
  auto search = r.bind(0x80000597);
  std::vector<uint8_t> pkt(296, 0);
  const std::string name = "\\DATA\\A.BIN;1";
  std::memcpy(&pkt[36], name.data(), name.size());
  const uint32_t dest = kEeData;
  std::memcpy(&pkt[292], &dest, 4);
  r.call(search, 0, pkt, 4);
  CHECK(r.ee->read<uint32_t>(kEeReply) == 1);  // found
  const uint32_t lsn = r.ee->read<uint32_t>(dest), size = r.ee->read<uint32_t>(dest + 4);
  CHECK(size == 3000);
  CHECK(lsn == img.find("DATA/A.BIN")->lba);

  auto ncmd = r.bind(0x80000595);
  const uint32_t buf = 0x00500000, intr = 0x00510000, pos = 0x00510100;
  r.ee->write<uint64_t>(intr, ~0ull);
  r.call(ncmd, 1, words({lsn, 2, buf, 0, intr, pos}), 0);
  CHECK(r.ee->read<uint8_t>(buf) == 'Q');
  CHECK(r.ee->read<uint8_t>(buf + 2999) == 'Q');
  CHECK(r.ee->read<uint64_t>(intr) == 0);  // no unaligned fix-ups needed
  CHECK(r.ee->read<uint32_t>(pos) == 2 * 2048);
  CHECK(cd.stats().sectors_read == 2);
}

TEST_CASE("CD search with the ps2sdk 292-byte layout and a missing file") {
  Rig r;
  auto img = disc();
  CdvdService cd(*r.ee, img);
  cd.register_with(r.sif);
  auto search = r.bind(0x80000597);
  std::vector<uint8_t> pkt(292, 0);
  const std::string name = "\\NOPE.BIN;1";
  std::memcpy(&pkt[32], name.data(), name.size());
  r.call(search, 0, pkt, 4);
  CHECK(r.ee->read<uint32_t>(kEeReply) == 0);
  CHECK(cd.stats().search_misses == 1);
}

TEST_CASE("disk ready, disc type, loader version and IOP heap") {
  Rig r;
  auto img = disc();
  CdvdService cd(*r.ee, img);
  cd.register_with(r.sif);
  r.call(r.bind(0x8000059A), 0, words({0}), 4);
  CHECK(r.ee->read<uint32_t>(kEeReply) == 2);  // SCECdComplete
  r.call(r.bind(0x80000593), 3, {}, 4);
  CHECK(r.ee->read<uint32_t>(kEeReply) == 0x14);  // SCECdPS2DVD

  LoadfileService lf;
  lf.register_with(r.sif);
  auto lfs = r.bind(0x80000006);
  r.call(lfs, 0xFF, {}, 4);
  CHECK(std::string(reinterpret_cast<const char*>(r.ee->host(kEeReply, 4)), 4) == "2240");
  std::vector<uint8_t> mod(512, 0);
  const std::string path = "cdrom0:\\IOP\\MODULES\\PADMAN.IRX;1";
  std::memcpy(&mod[8], path.data(), path.size());
  r.call(lfs, 0, mod, 8);
  CHECK(static_cast<int32_t>(r.ee->read<uint32_t>(kEeReply)) > 0);
  REQUIRE(lf.modules().size() == 1);
  CHECK(lf.modules()[0] == path);

  IopHeapService heap;
  heap.register_with(r.sif);
  auto hs = r.bind(0x80000003);
  r.call(hs, 1, words({0x8000}), 4);
  const uint32_t a1 = r.ee->read<uint32_t>(kEeReply);
  r.call(hs, 1, words({100}), 4);
  const uint32_t a2 = r.ee->read<uint32_t>(kEeReply);
  CHECK(a1 == SifHle::kHeapBase);
  CHECK(a2 == a1 + 0x8000);
}
