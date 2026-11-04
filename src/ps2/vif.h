#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

#include "gs/gs.h"

namespace lantern::ps2 {

// VU1 memories as seen by VIF1 (the VU1 interpreter shares them).
struct Vu1Memory {
  std::array<uint8_t, 16384> data{};   // 1024 quadwords
  std::array<uint8_t, 16384> micro{};  // 2048 64-bit instruction pairs
};

// VIF1: decodes the VIF code stream carried by DMA channel 1 into VU1 memory uploads, VU1
// program starts and GIF PATH2 transfers.
class Vif1 {
 public:
  using StartFn = std::function<void(uint32_t addr)>;  // MSCAL/MSCNT/MSCALF -> VU1

  Vif1(Vu1Memory& vu, gs::Gif& gif) : vu_(vu), gif_(gif) {}
  void set_vu1_start(StartFn fn) { start_ = std::move(fn); }

  // Feed 32-bit words (any chunking; commands may span calls).
  void feed(const uint32_t* words, size_t count);

  struct Stats {
    std::map<uint32_t, uint64_t> commands;  // command byte -> count
    uint64_t unpacked_qwords = 0, direct_qwords = 0, mpg_pairs = 0, vu1_starts = 0;
  };
  const Stats& stats() const { return stats_; }

 private:
  void begin(uint32_t code);
  void unpack_word_stream();

  Vu1Memory& vu_;
  gs::Gif& gif_;
  StartFn start_;

  // Decoder state: command in progress and the words it still needs.
  uint32_t cmd_ = 0, num_ = 0, imm_ = 0;
  size_t need_ = 0;
  std::vector<uint32_t> buf_;
  // VIF registers
  uint32_t cl_ = 1, wl_ = 1, mode_ = 0, mask_ = 0, base_ = 0, ofst_ = 0, tops_ = 0, top_ = 0, itop_ = 0;
  uint32_t row_[4]{}, col_[4]{};
  Stats stats_;
};

// DMA controller transfers (executed synchronously when CHCR.STR is set).
class Hardware;
class Dmac {
 public:
  using Sink = std::function<void(const uint8_t* qwords, uint32_t count)>;
  using TagSink = std::function<void(const uint32_t* words, size_t count)>;  // VIF1 TTE tag words

  Dmac(Hardware& hw, std::function<uint8_t*(uint32_t addr, uint32_t size)> host) : hw_(hw), host_(std::move(host)) {}
  void set_sink(int channel, Sink s) { sinks_[channel] = std::move(s); }
  void set_tag_sink(int channel, TagSink s) { tag_sinks_[channel] = std::move(s); }

  // Run the transfer described by the channel registers; returns false if the channel has no
  // modelled sink (the caller then treats it as a PLACEHOLDER).
  bool run(int channel);

  struct Stats {
    std::map<int, uint64_t> transfers, qwords;
    uint64_t chain_tags = 0, bad_addresses = 0;
  };
  const Stats& stats() const { return stats_; }

 private:
  void move(int channel, uint32_t addr, uint32_t qwc);

  Hardware& hw_;
  std::function<uint8_t*(uint32_t, uint32_t)> host_;
  std::map<int, Sink> sinks_;
  std::map<int, TagSink> tag_sinks_;
  Stats stats_;
};

}  // namespace lantern::ps2
