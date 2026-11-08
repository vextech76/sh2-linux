#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "ps2/hardware.h"
#include "ps2/ipu.h"
#include "ps2/mpeg_tables.h"

using namespace lantern::ps2;

namespace {
constexpr uint32_t kCmd = 0x10002000, kCtrl = 0x10002010;

// MSB-first bit writer for hand-assembled MPEG bitstreams.
struct Bits {
  std::vector<uint8_t> bytes;
  uint32_t n = 0;
  Bits& put(const std::string& s) {
    for (char c : s) bit(c == '1');
    return *this;
  }
  Bits& bit(bool b) {
    if (n % 8 == 0) bytes.push_back(0);
    if (b) bytes.back() |= static_cast<uint8_t>(0x80 >> (n % 8));
    ++n;
    return *this;
  }
  std::vector<uint8_t> padded() const {
    auto v = bytes;
    v.resize(((v.size() + 15) / 16) * 16 + 16, 0xFF);  // trailing 1s: never a start code
    return v;
  }
};

struct Rig {
  std::vector<uint8_t> ram = std::vector<uint8_t>(1 << 20, 0);
  Hardware hw;
  Ipu ipu{hw,
          [this](uint32_t a, uint32_t size) -> uint8_t* { return a + size <= ram.size() ? ram.data() + a : nullptr; }};
  Rig() { hw.set_ipu(&ipu); }
  void w(uint32_t addr, uint32_t v) { hw.write(addr, &v, 4); }
  uint32_t r(uint32_t addr) {
    uint32_t v = 0;
    hw.read(addr, &v, 4);
    return v;
  }
  std::vector<int16_t> raw16() {
    auto out = ipu.test_take_output();
    std::vector<int16_t> v(out.size() / 2);
    std::memcpy(v.data(), out.data(), v.size() * 2);
    return v;
  }
};

template <typename Entry, size_t N>
bool prefix_free(const Entry (&t)[N]) {
  for (size_t i = 0; i < N; ++i)
    for (size_t j = 0; j < N; ++j)
      if (i != j && std::string(t[j].code).starts_with(t[i].code)) return false;
  return true;
}
template <typename Entry, size_t N>
double kraft(const Entry (&t)[N]) {
  double k = 0;
  for (const auto& e : t) k += 1.0 / static_cast<double>(1ull << std::strlen(e.code));
  return k;
}
}  // namespace

TEST_CASE("IPU VLC tables are prefix-free and have the code space the standard defines") {
  CHECK(prefix_free(mpeg::kMbaIncrement));
  CHECK(prefix_free(mpeg::kMbTypeP));
  CHECK(prefix_free(mpeg::kMbTypeB));
  CHECK(prefix_free(mpeg::kCbp));
  CHECK(prefix_free(mpeg::kMotionCode));
  CHECK(prefix_free(mpeg::kDctTable0));
  CHECK(prefix_free(mpeg::kDctTable1));
  CHECK(kraft(mpeg::kDcSizeLuma) == doctest::Approx(1.0));
  CHECK(kraft(mpeg::kDcSizeChroma) == doctest::Approx(1.0));
  CHECK(kraft(mpeg::kCbp) == doctest::Approx(1.0 - 1.0 / 512));         // only "0000 0000 0" unused
  CHECK(kraft(mpeg::kDctTable0) == doctest::Approx(1.0 - 1.0 / 4096));  // only 12 leading zeros unused
  std::set<int> cbps;
  for (const auto& e : mpeg::kCbp) cbps.insert(e.value);
  CHECK(cbps.size() == 64);
  // Both coefficient tables carry the same 111 run/level pairs.
  std::set<std::pair<int, int>> t0, t1;
  for (const auto& e : mpeg::kDctTable0)
    if (e.kind == mpeg::kDctRunLevel) t0.insert({e.run, e.level});
  for (const auto& e : mpeg::kDctTable1)
    if (e.kind == mpeg::kDctRunLevel) t1.insert({e.run, e.level});
  CHECK(t0.size() == 111);
  CHECK(t0 == t1);
}

TEST_CASE("IPU BDEC: intra macroblock with DC-only blocks and DC prediction") {
  Rig rig;
  Bits b;
  // Y0: dc_size 1 ("00"), diff bit "1" (+1); EOB. Y1..Y3: dc_size 0 ("100"); EOB.
  b.put("00").put("1").put("10");
  for (int i = 0; i < 3; ++i) b.put("100").put("10");
  // Cb, Cr: dc_size 0 ("00"); EOB.
  b.put("00").put("10").put("00").put("10");
  rig.ipu.test_feed(b.padded());
  rig.w(kCmd, (2u << 28) | (1u << 27) | (1u << 26) | (16u << 16));  // BDEC MBI DCR QSC=16
  auto mb = rig.raw16();
  REQUIRE(mb.size() == 384);
  for (int k = 0; k < 256; ++k) CHECK(mb[static_cast<size_t>(k)] == 129);  // predictor carried to Y1..Y3
  for (int k = 256; k < 384; ++k) CHECK(mb[static_cast<size_t>(k)] == 128);
  CHECK(((rig.r(kCtrl) >> 8) & 0x3F) == 0x3F);  // CBP reported
  CHECK((rig.r(kCtrl) & (1u << 14)) == 0);      // no error
}

TEST_CASE("IPU BDEC: non-intra macroblock decodes CBP and the first-coefficient code") {
  Rig rig;
  Bits b;
  b.put("1101");        // coded_block_pattern = 4: only Y3 is coded
  b.put("1").put("0");  // first coefficient "1s": run 0, level +1
  b.put("10");          // EOB
  rig.ipu.test_feed(b.padded());
  rig.w(kCmd, (2u << 28) | (16u << 16));  // BDEC non-intra, QSC=16 (linear: quantiser_scale 32)
  auto mb = rig.raw16();
  REQUIRE(mb.size() == 384);
  // F[0] = ((2*1+1) * 16 * 32) / 32 = 48 -> a flat residual of 48/8 = 6 in the bottom-right block.
  for (int y = 0; y < 16; ++y)
    for (int x = 0; x < 16; ++x) CHECK(mb[static_cast<size_t>(y * 16 + x)] == ((y >= 8 && x >= 8) ? 6 : 0));
  CHECK(((rig.r(kCtrl) >> 8) & 0x3F) == 4);
}

TEST_CASE("IPU VDEC and FDEC report value and length; BCLR restarts the bitstream") {
  Rig rig;
  Bits b;
  b.put("011");                               // macroblock_address_increment = 2
  b.put("10110011100011110000111111110000");  // 32 bits for FDEC
  rig.ipu.test_feed(b.padded());
  rig.w(kCmd, 3u << 28);  // VDEC table 0
  CHECK(rig.r(kCmd) == ((3u << 16) | 2u));
  rig.w(kCmd, 4u << 28);  // FDEC: peek 32 bits
  CHECK(rig.r(kCmd) == 0xB38F0FF0u);
  rig.w(kCmd, (4u << 28) | 4);                  // FDEC skipping 4 bits
  CHECK(rig.r(kCmd) == 0x38F0FF00u);            // the 4 bits after the pattern are zero padding
  CHECK((rig.r(kCmd + 4) & 0x80000000u) == 0);  // not busy
}

TEST_CASE("IPU CSC converts a grey RAW8 macroblock to RGB32") {
  Rig rig;
  std::vector<uint8_t> raw(384, 128);
  raw.resize(400, 0xFF);
  rig.ipu.test_feed(raw);
  rig.w(kCmd, (7u << 28) | 1);  // CSC, one macroblock, RGB32
  auto out = rig.ipu.test_take_output();
  REQUIRE(out.size() == 1024);
  CHECK(out[0] == 130);
  CHECK(out[1] == 130);
  CHECK(out[2] == 130);
  CHECK(out[3] == 0x80);
}

TEST_CASE("IPU pulls input from IPU_TO DMA, completes it, and writes output through IPU_FROM") {
  Rig rig;
  std::vector<uint8_t> raw(384, 128);
  std::memcpy(rig.ram.data() + 0x1000, raw.data(), raw.size());
  rig.w(0x1000B400 + 0x10, 0x1000);  // IPU_TO MADR
  rig.w(0x1000B400 + 0x20, 24);      // QWC = 384 bytes
  rig.w(0x1000B400, 0x101);          // CHCR: from memory, STR
  rig.w(0x1000B000 + 0x10, 0x8000);  // IPU_FROM MADR
  rig.w(0x1000B000 + 0x20, 64);      // QWC = 1024 bytes
  rig.w(0x1000B000, 0x100);
  CHECK((rig.r(0x1000B000) & 0x100) != 0);  // output channel still running
  rig.w(kCmd, (7u << 28) | 1);              // CSC one macroblock
  CHECK((rig.r(0x1000B400) & 0x100) == 0);  // input drained -> IPU_TO done
  CHECK((rig.r(0x1000B000) & 0x100) == 0);  // output delivered -> IPU_FROM done
  CHECK(rig.ram[0x8000] == 130);
  CHECK(rig.ram[0x8000 + 1023] == 0x80);
  CHECK((rig.hw.take_completed_dma() & 0x18) == 0x18);
}

TEST_CASE("IPU command waiting for data stays busy and resumes when DMA supplies it") {
  Rig rig;
  rig.w(kCmd, 4u << 28);  // FDEC with an empty FIFO
  CHECK((rig.r(kCmd + 4) & 0x80000000u) != 0);
  const uint32_t word = 0x78563412;  // bytes 12 34 56 78 in memory
  std::memcpy(rig.ram.data() + 0x2000, &word, 4);
  rig.w(0x1000B400 + 0x10, 0x2000);
  rig.w(0x1000B400 + 0x20, 1);
  rig.w(0x1000B400, 0x101);
  CHECK((rig.r(kCmd + 4) & 0x80000000u) == 0);
  CHECK(rig.r(kCmd) == 0x12345678u);
}
