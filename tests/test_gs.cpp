#include <doctest/doctest.h>

#include <cstring>
#include <memory>
#include <set>
#include <vector>

#include "gs/gs.h"
#include "gs/rasterizer.h"
#include "ps2/hardware.h"
#include "ps2/vif.h"

using namespace lantern;
using namespace lantern::gs;

namespace {
// Build quadwords for the GIF.
struct Packet {
  std::vector<uint64_t> q;
  void tag(uint32_t nloop, bool eop, uint32_t flg, uint32_t nreg, uint64_t regs) {
    q.push_back(nloop | (uint64_t{eop} << 15) | (uint64_t{flg} << 58) | (uint64_t{nreg} << 60));
    q.push_back(regs);
  }
  void ad(uint8_t reg, uint64_t value) {
    q.push_back(value);
    q.push_back(reg);
  }
  void send(Gif& gif) const {
    gif.process(reinterpret_cast<const uint8_t*>(q.data()), static_cast<uint32_t>(q.size() / 2));
  }
};
}  // namespace

TEST_CASE("swizzle mappings are bijective within a page") {
  std::set<uint32_t> w32, h16, b8, n4;
  for (uint32_t y = 0; y < 32; ++y)
    for (uint32_t x = 0; x < 64; ++x) w32.insert(LocalMemory::word_addr32(kCt32, 0, 1, x, y));
  CHECK(w32.size() == 2048);
  CHECK(*w32.rbegin() == 2047);
  for (uint32_t y = 0; y < 64; ++y)
    for (uint32_t x = 0; x < 64; ++x) h16.insert(LocalMemory::half_addr16(kCt16, 0, 1, x, y));
  CHECK(h16.size() == 4096);
  auto mem = std::make_unique<LocalMemory>();
  // Indexed formats round-trip and do not alias within a page.
  for (uint32_t y = 0; y < 64; ++y)
    for (uint32_t x = 0; x < 128; ++x) mem->write_index(kT8, 0, 2, x, y, (x * 7 + y) & 0xFF);
  bool ok = true;
  for (uint32_t y = 0; y < 64; ++y)
    for (uint32_t x = 0; x < 128; ++x) ok &= mem->read_index(kT8, 0, 2, x, y) == ((x * 7 + y) & 0xFF);
  CHECK(ok);
  for (uint32_t y = 0; y < 128; ++y)
    for (uint32_t x = 0; x < 128; ++x) mem->write_index(kT4, 64, 2, x, y, (x + 3 * y) & 0xF);
  ok = true;
  for (uint32_t y = 0; y < 128; ++y)
    for (uint32_t x = 0; x < 128; ++x) ok &= mem->read_index(kT4, 64, 2, x, y) == ((x + 3 * y) & 0xF);
  CHECK(ok);
}

TEST_CASE("PSMCT32 known addresses (GS manual block/column tables)") {
  CHECK(LocalMemory::word_addr32(kCt32, 0, 1, 1, 0) == 1);
  CHECK(LocalMemory::word_addr32(kCt32, 0, 1, 0, 1) == 2);
  CHECK(LocalMemory::word_addr32(kCt32, 0, 1, 8, 0) == 64);     // block 1
  CHECK(LocalMemory::word_addr32(kCt32, 0, 1, 0, 8) == 128);    // block 2
  CHECK(LocalMemory::word_addr32(kCt32, 0, 2, 64, 0) == 2048);  // next page
  CHECK(LocalMemory::word_addr32(kCt32, 1, 1, 0, 0) == 64);     // bp is in 64-word blocks
}

TEST_CASE("GIF A+D and IMAGE upload into local memory") {
  auto gs = std::make_unique<Gs>();
  Gif gif(*gs);
  Packet p;
  p.tag(4, false, 0, 1, 0xE);
  p.ad(0x50, uint64_t{0} << 32 | uint64_t{1} << 48 | uint64_t{kCt32} << 56);  // BITBLTBUF: DBP 0, DBW 1
  p.ad(0x51, uint64_t{2} << 32 | uint64_t{3} << 48);                          // TRXPOS: DSAX 2, DSAY 3
  p.ad(0x52, 2 | uint64_t{2} << 32);                                          // TRXREG: 2x2
  p.ad(0x53, 0);                                                              // TRXDIR: host->local
  p.tag(2, true, 2, 0, 0);                                                    // IMAGE, 2 qwords
  p.q.insert(p.q.end(), {0x2222222211111111ull, 0x4444444433333333ull, 0, 0});
  p.send(gif);
  CHECK(gs->mem().read_pixel(kCt32, 0, 1, 2, 3) == 0x11111111u);
  CHECK(gs->mem().read_pixel(kCt32, 0, 1, 3, 3) == 0x22222222u);
  CHECK(gs->mem().read_pixel(kCt32, 0, 1, 2, 4) == 0x33333333u);
  CHECK(gs->mem().read_pixel(kCt32, 0, 1, 3, 4) == 0x44444444u);
  CHECK(gs->stats().image_pixels == 4);
}

namespace {
void setup_frame(Gs& gs) {
  gs.write_reg(0x4C, uint64_t{1} << 16);                        // FRAME_1: FBP 0, FBW 1 (64 px), CT32
  gs.write_reg(0x40, uint64_t{63} << 16 | uint64_t{63} << 48);  // SCISSOR_1: 0..63
  gs.write_reg(0x18, 0);                                        // XYOFFSET_1
  gs.write_reg(0x47, 0);                                        // TEST_1: no tests
}
uint64_t xyz(int x, int y) { return static_cast<uint64_t>(x * 16) | static_cast<uint64_t>(y * 16) << 16; }
}  // namespace

TEST_CASE("sprite covers exactly [x0,x1) x [y0,y1)") {
  auto gs = std::make_unique<Gs>();
  Rasterizer r;
  gs->set_rasterizer([&](Gs& g, const Vertex* v, int n, uint64_t prim) { r.draw(g, v, n, prim); });
  setup_frame(*gs);
  gs->write_reg(0x00, 6);              // PRIM: sprite
  gs->write_reg(0x01, 0x80FF0000ull);  // RGBA: blue
  gs->write_reg(0x05, xyz(4, 4));
  gs->write_reg(0x05, xyz(8, 6));
  int count = 0;
  for (uint32_t y = 0; y < 16; ++y)
    for (uint32_t x = 0; x < 16; ++x)
      if (gs->mem().read_pixel(kCt32, 0, 1, x, y)) ++count;
  CHECK(count == 4 * 2);
  CHECK((gs->mem().read_pixel(kCt32, 0, 1, 4, 4) & 0xFFFFFF) == 0xFF0000u);
  CHECK(gs->mem().read_pixel(kCt32, 0, 1, 8, 4) == 0);
}

TEST_CASE("top-left fill rule: shared triangle edge is drawn once under additive blending") {
  auto gs = std::make_unique<Gs>();
  Rasterizer r;
  gs->set_rasterizer([&](Gs& g, const Vertex* v, int n, uint64_t prim) { r.draw(g, v, n, prim); });
  setup_frame(*gs);
  gs->write_reg(0x42, uint64_t{0} | uint64_t{2} << 2 | uint64_t{2} << 4 | uint64_t{1} << 6 | uint64_t{128} << 32);
  // ALPHA: (Cs - 0) * FIX(128) >> 7 + Cd  -> additive
  gs->write_reg(0x00, 3 | (1u << 6));  // triangle list, ABE
  gs->write_reg(0x01, 0x80000010ull);  // red = 16
  for (auto [x, y] : {std::pair{0, 0}, {16, 0}, {0, 16}, {16, 0}, {16, 16}, {0, 16}}) gs->write_reg(0x05, xyz(x, y));
  int doubled = 0, single = 0;
  for (uint32_t y = 0; y < 16; ++y)
    for (uint32_t x = 0; x < 16; ++x) {
      const uint32_t red = gs->mem().read_pixel(kCt32, 0, 1, x, y) & 0xFF;
      doubled += red == 32;
      single += red == 16;
    }
  CHECK(doubled == 0);
  CHECK(single == 16 * 16);
}

TEST_CASE("display readout honours interlaced frame mode") {
  auto gs = std::make_unique<Gs>();
  gs->write_priv32(0x0000, 0x2);                   // PMODE: EN2
  gs->write_priv32(0x0020, 0x1);                   // SMODE2: INT=1, FFMD=0
  gs->write_priv32(0x0090, 8u << 9);               // DISPFB2: FBW 8 (512), CT32
  gs->write_priv32(0x00A0, 4u << 23);              // DISPLAY2: MAGH 4
  gs->write_priv32(0x00A4, 2559u | (895u << 12));  // DW 2559, DH 895
  auto img = gs->display();
  CHECK(img.width == 512);
  CHECK(img.height == 448);
}

TEST_CASE("DMA source chain: cnt / call / ret / ref / end deliver data in order") {
  auto mem = std::make_unique<std::vector<uint8_t>>(1 << 20, 0);
  auto host = [&](uint32_t a, uint32_t n) -> uint8_t* { return a + n <= mem->size() ? mem->data() + a : nullptr; };
  auto tag = [&](uint32_t at, uint32_t qwc, uint32_t id, uint32_t addr) {
    const uint64_t t = qwc | (uint64_t{id} << 28) | (uint64_t{addr} << 32);
    std::memcpy(&(*mem)[at], &t, 8);
  };
  auto data = [&](uint32_t at, uint64_t v) { std::memcpy(&(*mem)[at], &v, 8); };
  tag(0x1000, 1, 1, 0);  // cnt: 1 qword inline
  data(0x1010, 1);
  tag(0x1020, 1, 5, 0x2000);  // call 0x2000 with 1 inline qword
  data(0x1030, 2);
  tag(0x2000, 1, 6, 0);  // ret with 1 inline qword
  data(0x2010, 3);
  tag(0x1040, 1, 3, 0x3000);  // ref: data elsewhere
  data(0x3000, 4);
  tag(0x1050, 1, 7, 0);  // end with 1 inline qword
  data(0x1060, 5);
  ps2::Hardware hw;
  ps2::Dmac dmac(hw, host);
  std::vector<uint64_t> got;
  dmac.set_sink(2, [&](const uint8_t* q, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
      uint64_t v;
      std::memcpy(&v, q + 16 * i, 8);
      got.push_back(v);
    }
  });
  hw.set_dma_reg(0x1000A030, 0x1000);  // TADR
  hw.set_dma_reg(0x1000A000, 0x104);   // CHCR: chain mode, STR
  REQUIRE(dmac.run(2));
  CHECK(got == std::vector<uint64_t>{1, 2, 3, 4, 5});
}

TEST_CASE("VIF1: DIRECT to GIF, skipping UNPACK V4-32 and MPG") {
  auto gs = std::make_unique<Gs>();
  Gif gif(*gs);
  auto vu = std::make_unique<ps2::Vu1Memory>();
  ps2::Vif1 vif(*vu, gif);
  std::vector<uint32_t> w;
  // DIRECT 2 qwords: GIFtag (1 loop, A+D) + FRAME_1 write
  w.push_back(0x50000002);
  const uint64_t tagq[4] = {1 | (1ull << 15) | (1ull << 60), 0xE, 0x1234ull << 16, 0x4C};
  for (int i = 0; i < 4; ++i) {
    w.push_back(static_cast<uint32_t>(tagq[i]));
    w.push_back(static_cast<uint32_t>(tagq[i] >> 32));
  }
  // STCYCL CL=2 WL=1, then UNPACK V4-32 (cmd 0x6C) 2 elements at addr 10
  w.push_back(0x01000102);
  w.push_back(0x6C02000A);
  for (uint32_t v = 1; v <= 8; ++v) w.push_back(v);
  // MPG 1 pair at 5
  w.push_back(0x4A010005);
  w.push_back(0xAAAAAAAA);
  w.push_back(0xBBBBBBBB);
  vif.feed(w.data(), w.size());
  CHECK(gs->reg(0x4C) == (0x1234ull << 16));
  uint32_t q10[4], q12[4];
  std::memcpy(q10, &vu->data[10 * 16], 16);
  std::memcpy(q12, &vu->data[12 * 16], 16);  // skipping: second element lands CL=2 later
  CHECK(q10[0] == 1);
  CHECK(q10[3] == 4);
  CHECK(q12[0] == 5);
  uint32_t m[2];
  std::memcpy(m, &vu->micro[5 * 8], 8);
  CHECK(m[0] == 0xAAAAAAAA);
  CHECK(vif.stats().direct_qwords == 2);
}
