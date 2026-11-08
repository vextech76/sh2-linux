#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace lantern::ps2 {

class Hardware;

// Image Processing Unit: the EE's MPEG-1/MPEG-2 macroblock decoder (public hardware
// documentation: ps2tek "IPU"; bitstream syntax and tables: ISO/IEC 13818-2).
//
// Input arrives through DMA channel 4 (IPU_TO) and is pulled lazily as commands need bits; output
// leaves through DMA channel 3 (IPU_FROM) or the output FIFO. Commands run synchronously when
// written; one that runs out of input stays busy and resumes when more data is supplied.
class Ipu {
 public:
  using Host = std::function<uint8_t*(uint32_t addr, uint32_t size)>;

  Ipu(Hardware& hw, Host host);

  static bool handles(uint32_t addr) {
    return (addr >= 0x10002000 && addr < 0x10002040) || (addr >= 0x10007000 && addr < 0x10007020);
  }
  uint32_t read32(uint32_t addr);
  void write32(uint32_t addr, uint32_t value);
  void read_fifo(uint8_t* out16);        // 0x10007000: one quadword of output
  void write_fifo(const uint8_t* in16);  // 0x10007010: one quadword of input

  // DMA channel 3 (IPU_FROM) or 4 (IPU_TO) was started by the guest.
  void dma_started(int channel);
  // The guest stopped an IPU DMA channel (CHCR written with STR clear).
  void dma_stopped(int channel);

  struct Stats {
    std::array<uint64_t, 16> commands{};
    uint64_t macroblocks = 0, csc_macroblocks = 0, input_qwords = 0, output_qwords = 0;
    uint64_t vlc_errors = 0, stalls = 0;
  };
  const Stats& stats() const { return stats_; }
  std::string describe() const;  // state summary for the boot report

  // Exposed for unit tests: decode one macroblock's worth of 8x8 blocks from raw bits.
  void test_feed(const std::vector<uint8_t>& bytes);
  std::vector<uint8_t> test_take_output();

 private:
  class OutOfData {};

  // Bitstream access over the pulled input. All reads throw OutOfData when the IPU must wait.
  bool ensure_bits(uint32_t n);
  uint32_t peek(uint32_t n);
  void skip(uint32_t n);
  uint32_t get(uint32_t n) {
    const uint32_t v = peek(n);
    skip(n);
    return v;
  }
  template <typename Table>
  int decode_vlc(const Table& table, uint32_t* length);
  bool pull_qword();
  void top_up();  // prefetch from IPU_TO until the FIFO and internal buffer are full
  uint64_t consumed_since_bclr() const;
  uint32_t buffered_qwords() const;
  void finish_dma_to_if_drained();

  void reset();
  void run_pending();
  void execute(uint32_t cmd);
  void cmd_vdec(uint32_t cmd);
  void cmd_bdec(uint32_t cmd);
  void cmd_idec(uint32_t cmd);
  void cmd_setiq(uint32_t cmd);
  void cmd_setvq(uint32_t cmd);
  void cmd_csc(uint32_t cmd);
  void cmd_pack(uint32_t cmd);

  // Decode one 8x8 block (intra or non-intra) into `out` (raster order, after IDCT).
  void decode_block(int component, bool intra, int qscale, int16_t* out);
  int quantiser_scale(int code) const;
  void rgb_from_raw8(const uint8_t* raw, bool rgb16, bool dither, bool sign, std::vector<uint8_t>& out);
  void emit(const std::vector<uint8_t>& bytes);
  void flush_output();
  void set_start_code_flag();

  Hardware& hw_;
  Host host_;

  // Input: pulled bytes and the consumed bit count within them.
  std::deque<uint8_t> in_;
  uint64_t in_bits_consumed_ = 0;  // bits consumed from the front of in_
  uint32_t bclr_skip_ = 0;         // BCLR bit position applied to the next pulled data
  uint64_t pulled_qwords_ = 0;     // since the last BCLR (for IPU_BP)
  bool to_active_ = false;
  uint32_t to_tag_addr_ = 0;
  bool to_chain_end_ = false;

  // Output.
  std::deque<uint8_t> out_;
  bool from_active_ = false;

  // Registers and decoder state.
  uint32_t ctrl_ = 0;  // IDP/AS/IVF/QST/MP1/PCT (+ CBP/ECD/SCD written by commands)
  uint32_t result_ = 0;
  bool busy_ = false;
  uint32_t pending_cmd_ = 0;
  bool any_command_ = false;
  std::array<uint8_t, 64> intra_matrix_{};     // natural order
  std::array<uint8_t, 64> nonintra_matrix_{};  // natural order
  std::array<uint16_t, 16> vqclut_{};
  uint32_t th0_ = 0, th1_ = 0;
  int dc_pred_[3] = {128, 128, 128};

  // Diagnostics: where the most recent BDEC ran out of input (all -1 when it did not).
  uint64_t stall_mb_ = ~0ull;
  int stall_block_ = -1, stall_cbp_ = -1, stall_intra_ = -1;
  uint64_t stall_bits_ = 0;
  uint64_t last_picture_mb_ = 0;  // macroblock index when the picture coding type last changed

  Stats stats_;
};

}  // namespace lantern::ps2
