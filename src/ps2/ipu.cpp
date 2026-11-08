#include "ps2/ipu.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <optional>

#include "core/log.h"
#include "ps2/hardware.h"
#include "ps2/mpeg_tables.h"

namespace lantern::ps2 {
namespace {
constexpr uint32_t kIpuCmd = 0x10002000, kIpuCmdHi = 0x10002004, kIpuCtrl = 0x10002010, kIpuBp = 0x10002020,
                   kIpuTop = 0x10002030, kIpuTopHi = 0x10002034;
constexpr uint32_t kDmaFrom = 0x1000B000, kDmaTo = 0x1000B400;
constexpr uint32_t kFifoQwords = 8, kInternalQwords = 2;

// Direct lookup tables built from the bit-string VLC definitions. Index = next `maxlen` bits.
struct Lut {
  uint32_t maxlen = 0;
  std::vector<int32_t> slots;  // (entry index << 8) | code length, -1 = invalid
};

template <typename Entry, size_t N>
Lut build_lut(const Entry (&table)[N]) {
  Lut lut;
  for (const auto& e : table) lut.maxlen = std::max<uint32_t>(lut.maxlen, static_cast<uint32_t>(std::strlen(e.code)));
  lut.slots.assign(size_t{1} << lut.maxlen, -1);
  for (size_t i = 0; i < N; ++i) {
    const uint32_t len = static_cast<uint32_t>(std::strlen(table[i].code));
    uint32_t code = 0;
    for (uint32_t b = 0; b < len; ++b) code = (code << 1) | (table[i].code[b] == '1' ? 1u : 0u);
    const uint32_t shift = lut.maxlen - len;
    for (uint32_t fill = 0; fill < (1u << shift); ++fill)
      lut.slots[(code << shift) | fill] = static_cast<int32_t>((i << 8) | len);
  }
  return lut;
}

const Lut& lut_for(const void* table) {
  static const Lut mba = build_lut(mpeg::kMbaIncrement), mbi = build_lut(mpeg::kMbTypeI),
                   mbp = build_lut(mpeg::kMbTypeP), mbb = build_lut(mpeg::kMbTypeB), mbd = build_lut(mpeg::kMbTypeD),
                   cbp = build_lut(mpeg::kCbp), mv = build_lut(mpeg::kMotionCode), dmv = build_lut(mpeg::kDmVector),
                   dcl = build_lut(mpeg::kDcSizeLuma), dcc = build_lut(mpeg::kDcSizeChroma),
                   dct0 = build_lut(mpeg::kDctTable0), dct1 = build_lut(mpeg::kDctTable1);
  if (table == mpeg::kMbaIncrement) return mba;
  if (table == mpeg::kMbTypeI) return mbi;
  if (table == mpeg::kMbTypeP) return mbp;
  if (table == mpeg::kMbTypeB) return mbb;
  if (table == mpeg::kMbTypeD) return mbd;
  if (table == mpeg::kCbp) return cbp;
  if (table == mpeg::kMotionCode) return mv;
  if (table == mpeg::kDmVector) return dmv;
  if (table == mpeg::kDcSizeLuma) return dcl;
  if (table == mpeg::kDcSizeChroma) return dcc;
  if (table == mpeg::kDctTable0) return dct0;
  return dct1;
}

class VlcError {};

// Separable 8x8 inverse DCT in double precision (meets IEEE 1180 accuracy comfortably).
void idct8x8(const int32_t* in, int32_t* out) {
  static const auto kCos = [] {
    std::array<double, 64> c{};
    for (int x = 0; x < 8; ++x)
      for (int u = 0; u < 8; ++u)
        c[static_cast<size_t>(x * 8 + u)] =
            (u == 0 ? std::sqrt(0.125) : 0.5) * std::cos((2 * x + 1) * u * 3.14159265358979323846 / 16.0);
    return c;
  }();
  double tmp[64];
  for (int y = 0; y < 8; ++y)  // rows: horizontal transform
    for (int x = 0; x < 8; ++x) {
      double s = 0;
      for (int u = 0; u < 8; ++u) s += kCos[static_cast<size_t>(x * 8 + u)] * in[y * 8 + u];
      tmp[y * 8 + x] = s;
    }
  for (int x = 0; x < 8; ++x)
    for (int y = 0; y < 8; ++y) {
      double s = 0;
      for (int v = 0; v < 8; ++v) s += kCos[static_cast<size_t>(y * 8 + v)] * tmp[v * 8 + x];
      out[y * 8 + x] = std::clamp(static_cast<int32_t>(std::floor(s + 0.5)), -256, 255);
    }
}
}  // namespace

Ipu::Ipu(Hardware& hw, Host host) : hw_(hw), host_(std::move(host)) { reset(); }

void Ipu::reset() {
  in_.clear();
  out_.clear();
  in_bits_consumed_ = 0;
  bclr_skip_ = 0;
  pulled_qwords_ = 0;
  ctrl_ = 0;
  result_ = 0;
  busy_ = false;
  pending_cmd_ = 0;
  any_command_ = false;
  std::copy(std::begin(mpeg::kDefaultIntraMatrix), std::end(mpeg::kDefaultIntraMatrix), intra_matrix_.begin());
  nonintra_matrix_.fill(16);
  th0_ = th1_ = 0;
  for (int& p : dc_pred_) p = 128;
}

// ---------------------------------------------------------------------------------------------
// Input

bool Ipu::pull_qword() {
  if (!to_active_) return false;
  for (int guard = 0; guard < 64; ++guard) {
    const uint32_t chcr = hw_.dma_reg(kDmaTo);
    uint32_t qwc = hw_.dma_reg(kDmaTo + 0x20) & 0xFFFF;
    const bool chain = ((chcr >> 2) & 3) == 1;
    if (qwc == 0) {
      if (!chain || to_chain_end_) {
        to_active_ = false;
        hw_.dma_done(4);
        return false;
      }
      // Source chain: read the next DMAtag at TADR.
      const uint32_t tadr = hw_.dma_reg(kDmaTo + 0x30);
      const uint8_t* t = host_(tadr, 16);
      if (!t) {
        to_active_ = false;
        hw_.dma_done(4);
        return false;
      }
      uint64_t tag;
      std::memcpy(&tag, t, 8);
      const uint32_t n = static_cast<uint32_t>(tag & 0xFFFF), id = static_cast<uint32_t>((tag >> 28) & 7);
      const uint32_t addr = static_cast<uint32_t>(tag >> 32) & 0x7FFFFFF0;
      hw_.set_dma_reg(kDmaTo + 0x20, n);
      switch (id) {
        case 0:  // refe
          hw_.set_dma_reg(kDmaTo + 0x10, addr);
          hw_.set_dma_reg(kDmaTo + 0x30, tadr + 16);
          to_chain_end_ = true;
          break;
        case 1:  // cnt
          hw_.set_dma_reg(kDmaTo + 0x10, tadr + 16);
          hw_.set_dma_reg(kDmaTo + 0x30, tadr + 16 + n * 16);
          break;
        case 2:  // next
          hw_.set_dma_reg(kDmaTo + 0x10, tadr + 16);
          hw_.set_dma_reg(kDmaTo + 0x30, addr);
          break;
        case 3:  // ref
        case 4:  // refs
          hw_.set_dma_reg(kDmaTo + 0x10, addr);
          hw_.set_dma_reg(kDmaTo + 0x30, tadr + 16);
          break;
        default:  // end (7) and anything unexpected
          hw_.set_dma_reg(kDmaTo + 0x10, tadr + 16);
          to_chain_end_ = true;
          break;
      }
      // The DMAC copies the tag's upper half into CHCR; IRQ + TIE ends the chain after this packet.
      hw_.set_dma_reg(kDmaTo, (chcr & 0xFFFFu) | (static_cast<uint32_t>(tag) & 0xFFFF0000u));
      if ((tag & (1ull << 31)) && (chcr & 0x80)) to_chain_end_ = true;
      continue;
    }
    uint32_t madr = hw_.dma_reg(kDmaTo + 0x10);
    const uint32_t addr = (madr & 0x80000000u) ? (0x70000000u | (madr & 0x3FF0)) : madr;
    const uint8_t* p = host_(addr, 16);
    if (!p) {
      to_active_ = false;
      hw_.dma_done(4);
      return false;
    }
    for (int i = 0; i < 16; ++i) in_.push_back(p[i]);
    hw_.set_dma_reg(kDmaTo + 0x10, madr + 16);
    hw_.set_dma_reg(kDmaTo + 0x20, qwc - 1);
    ++pulled_qwords_;
    ++stats_.input_qwords;
    if (bclr_skip_) {
      in_bits_consumed_ += bclr_skip_;
      bclr_skip_ = 0;
    }
    if (qwc - 1 == 0) finish_dma_to_if_drained();
    return true;
  }
  return false;
}

void Ipu::finish_dma_to_if_drained() {
  const uint32_t chcr = hw_.dma_reg(kDmaTo);
  const bool chain = ((chcr >> 2) & 3) == 1;
  if ((hw_.dma_reg(kDmaTo + 0x20) & 0xFFFF) == 0 && (!chain || to_chain_end_)) {
    to_active_ = false;
    hw_.dma_done(4);
  }
}

bool Ipu::ensure_bits(uint32_t n) {
  while (in_.size() * 8 < in_bits_consumed_ + n)
    if (!pull_qword()) return false;
  return true;
}

uint32_t Ipu::peek(uint32_t n) {
  if (n == 0) return 0;
  if (!ensure_bits(n)) throw OutOfData{};
  uint32_t v = 0;
  uint64_t pos = in_bits_consumed_;
  for (uint32_t i = 0; i < n; ++i, ++pos) v = (v << 1) | ((in_[pos >> 3] >> (7 - (pos & 7))) & 1u);
  return v;
}

void Ipu::skip(uint32_t n) {
  if (!ensure_bits(n)) throw OutOfData{};
  in_bits_consumed_ += n;
}

template <typename Table>
int Ipu::decode_vlc(const Table& table, uint32_t* length) {
  const Lut& lut = lut_for(table);
  // Near the end of the stream fewer than maxlen bits may exist; pad with zeros.
  uint32_t bits;
  if (ensure_bits(lut.maxlen)) {
    bits = peek(lut.maxlen);
  } else {
    const uint32_t avail = static_cast<uint32_t>(in_.size() * 8 - in_bits_consumed_);
    if (avail == 0) throw OutOfData{};
    bits = peek(avail) << (lut.maxlen - avail);
  }
  const uint32_t avail = static_cast<uint32_t>(std::min<uint64_t>(in_.size() * 8 - in_bits_consumed_, lut.maxlen));
  const int32_t slot = lut.slots[bits];
  // With fewer than maxlen bits buffered, only a code that fits in them is a real match; anything
  // else means the IPU must wait for more input (the codes are prefix-free).
  if (slot < 0) {
    if (avail < lut.maxlen) throw OutOfData{};
    throw VlcError{};
  }
  const uint32_t len = static_cast<uint32_t>(slot & 0xFF);
  if (len > avail) throw OutOfData{};
  skip(len);
  if (length) *length = len;
  return slot >> 8;
}

std::string Ipu::describe() const {
  std::string s = std::format(
      "busy {} cmd 0x{:08x} in {} bytes ({} bits used) out {} bytes; IPU_TO active {} chcr 0x{:x} madr 0x{:x} qwc {} "
      "tadr 0x{:x}; IPU_FROM active {} madr 0x{:x} qwc {}",
      busy_, pending_cmd_, in_.size(), in_bits_consumed_, out_.size(), to_active_, hw_.dma_reg(kDmaTo),
      hw_.dma_reg(kDmaTo + 0x10), hw_.dma_reg(kDmaTo + 0x20), hw_.dma_reg(kDmaTo + 0x30), from_active_,
      hw_.dma_reg(kDmaFrom + 0x10), hw_.dma_reg(kDmaFrom + 0x20));
  if (stall_block_ >= 0)
    s += std::format("; stalled in MB {} block {} cbp 0x{:02x} intra {} after {} of {} bits", stall_mb_, stall_block_,
                     stall_cbp_, stall_intra_, stall_bits_, in_.size() * 8);
  s += std::format("; picture type {} since MB {}", (ctrl_ >> 24) & 7, last_picture_mb_);
  return s;
}

void Ipu::test_feed(const std::vector<uint8_t>& bytes) {
  for (uint8_t b : bytes) in_.push_back(b);
  pulled_qwords_ += (bytes.size() + 15) / 16;
}

std::vector<uint8_t> Ipu::test_take_output() {
  std::vector<uint8_t> v(out_.begin(), out_.end());
  out_.clear();
  return v;
}

// ---------------------------------------------------------------------------------------------
// Registers

uint64_t Ipu::consumed_since_bclr() const {
  const uint64_t remaining_bits = in_.size() * 8 - in_bits_consumed_;
  const uint64_t pulled_bits_total = pulled_qwords_ * 128;
  return pulled_bits_total - std::min<uint64_t>(remaining_bits, pulled_bits_total);
}

uint32_t Ipu::buffered_qwords() const {
  // Includes the partially consumed qword: the game rewinds DMA by (IFC + FP) qwords and
  // re-enters at bit BP, so this must match what was really pulled.
  return static_cast<uint32_t>(pulled_qwords_ - consumed_since_bclr() / 128);
}

void Ipu::top_up() {
  // Prefetch only what the FIFO and the internal buffer can hold. Anything pulled beyond that is
  // invisible to IFC and lost when the guest rewinds the DMA by (IFC + FP) qwords. A command that
  // is already running may drain past that limit: its data was pulled on demand.
  while (to_active_ && buffered_qwords() < kFifoQwords + kInternalQwords && pull_qword()) {
  }
}

uint32_t Ipu::read32(uint32_t addr) {
  run_pending();
  const uint64_t remaining_bits = in_.size() * 8 - in_bits_consumed_;
  const uint64_t consumed_total = consumed_since_bclr();
  const uint32_t remaining_qw = buffered_qwords();
  const uint32_t fp = std::min(remaining_qw, kInternalQwords);
  const uint32_t ifc = std::min(remaining_qw - fp, kFifoQwords);
  const uint32_t ofc = std::min<uint32_t>(static_cast<uint32_t>(out_.size() / 16), kFifoQwords);
  switch (addr) {
    case kIpuCmd:
      if (!any_command_ && remaining_bits >= 32) {  // documented quirk: first 32 bits before any command
        try {
          return peek(32);
        } catch (const OutOfData&) {
        }
      }
      return result_;
    case kIpuCmdHi: return busy_ ? 0x80000000u : 0;
    case kIpuCtrl: return (ctrl_ & ~0x800000FFu) | ifc | (ofc << 4) | (busy_ ? 0x80000000u : 0);
    case kIpuBp: return static_cast<uint32_t>(consumed_total % 128) | (ifc << 8) | (fp << 16);
    case kIpuTop: {
      if (remaining_bits >= 32 || ensure_bits(32)) return peek(32);
      const uint32_t avail = static_cast<uint32_t>(in_.size() * 8 - in_bits_consumed_);
      return avail ? peek(avail) << (32 - avail) : 0;
    }
    case kIpuTopHi: return (in_.size() * 8 - in_bits_consumed_ >= 32 || ensure_bits(32)) && !busy_ ? 0 : 0x80000000u;
    default: break;
  }
  if (addr >= 0x10007000 && addr < 0x10007010) {  // output FIFO, one word at a time
    uint32_t v = 0;
    for (int i = 0; i < 4 && !out_.empty(); ++i) {
      v |= static_cast<uint32_t>(out_.front()) << (8 * i);
      out_.pop_front();
    }
    return v;
  }
  return 0;
}

void Ipu::write32(uint32_t addr, uint32_t value) {
  switch (addr) {
    case kIpuCmd:
      any_command_ = true;
      pending_cmd_ = value;
      busy_ = true;
      ctrl_ &= ~((1u << 14) | (1u << 15));  // ECD, SCD
      ++stats_.commands[value >> 28];
      run_pending();
      return;
    case kIpuCtrl: {
      if (value & (1u << 30)) {
        const bool to = to_active_, from = from_active_;
        reset();
        to_active_ = to;
        from_active_ = from;
      }
      // Configuration bits: IDP, AS, IVF, QST, MP1, PCT.
      const uint32_t old_pct = (ctrl_ >> 24) & 7, new_pct = (value >> 24) & 7;
      ctrl_ = (ctrl_ & ~0x07F30000u) | (value & 0x07F30000u);
      if (new_pct != old_pct) {
        last_picture_mb_ = stats_.macroblocks;
        static const bool trace =
            std::getenv("LANTERN_IPU_TRACE") != nullptr || std::getenv("LANTERN_IPU_SUMMARY") != nullptr;
        if (trace)
          log::info("ipu", "picture coding type {} at macroblock {} (ctrl 0x{:08x})", new_pct, stats_.macroblocks,
                    ctrl_);
      }
      return;
    }
    default: break;
  }
  if (addr >= 0x10007010 && addr < 0x10007020) {  // input FIFO, one word at a time
    for (int i = 0; i < 4; ++i) in_.push_back(static_cast<uint8_t>(value >> (8 * i)));
    if (addr == 0x1000701C) ++pulled_qwords_;
    run_pending();
  }
}

void Ipu::read_fifo(uint8_t* out16) {
  for (int i = 0; i < 16; ++i) {
    out16[i] = out_.empty() ? 0 : out_.front();
    if (!out_.empty()) out_.pop_front();
  }
}

void Ipu::write_fifo(const uint8_t* in16) {
  for (int i = 0; i < 16; ++i) in_.push_back(in16[i]);
  ++pulled_qwords_;
  run_pending();
}

void Ipu::dma_stopped(int channel) {
  // Stopping IPU_TO only halts fetching. The DMAC registers stay as they are and the IPU keeps
  // its input FIFO and bit pointer: the guest snapshots IPU_BP here and, when it resumes, rewinds
  // MADR by (IFC+FP) itself and re-enters the bitstream with BCLR(BP) (see the stream reader at
  // 0x2a4890/0x2a4980). Rewinding or dropping data here would move the snapshot's reference point.
  if (channel == 4) to_active_ = false;
}

void Ipu::dma_started(int channel) {
  if (channel == 4) {
    to_active_ = true;
    to_chain_end_ = false;
    // Like the hardware, prefetch into the FIFO; the channel completes when its data is in.
    top_up();
    run_pending();
  } else if (channel == 3) {
    from_active_ = true;
    flush_output();
    run_pending();
  }
}

// ---------------------------------------------------------------------------------------------
// Command execution

void Ipu::run_pending() {
  if (!busy_) {
    // Keep the FIFO topped up from an active IPU_TO transfer, as the hardware does.
    top_up();
    return;
  }
  const uint64_t saved_consumed = in_bits_consumed_;
  const uint32_t saved_ctrl = ctrl_;
  int saved_dc[3];
  std::copy(std::begin(dc_pred_), std::end(dc_pred_), saved_dc);
  try {
    const uint64_t before = in_bits_consumed_;
    execute(pending_cmd_);
    busy_ = false;
    static const bool trace = std::getenv("LANTERN_IPU_TRACE") != nullptr;
    static uint64_t traced = 0;
    if (trace && traced++ < 12000)
      log::info("ipu", "cmd 0x{:08x} -> result 0x{:08x} ctrl 0x{:08x} consumed {} bits", pending_cmd_, result_, ctrl_,
                in_bits_consumed_ - before);
  } catch (const OutOfData&) {
    in_bits_consumed_ = saved_consumed;
    ctrl_ = saved_ctrl;
    std::copy(std::begin(saved_dc), std::end(saved_dc), dc_pred_);
    ++stats_.stalls;
    static uint64_t logged_stall_mb = ~0ull;
    if (stall_block_ >= 0 && stall_mb_ != logged_stall_mb &&
        (std::getenv("LANTERN_IPU_SUMMARY") || std::getenv("LANTERN_IPU_TRACE"))) {
      logged_stall_mb = stall_mb_;
      log::info("ipu", "stall: MB {} block {} cbp 0x{:02x} intra {} cmd 0x{:08x}; {} bits in buffer, {} consumed",
                stall_mb_, stall_block_, stall_cbp_, stall_intra_, pending_cmd_, in_.size() * 8, stall_bits_);
    }
    return;
  } catch (const VlcError&) {
    ctrl_ |= 1u << 14;  // ECD
    if (stats_.vlc_errors++ < 8)
      log::warn("ipu", "VLC error in command 0x{:08x} after {} macroblocks (next bits 0x{:08x})", pending_cmd_,
                stats_.macroblocks, in_.size() * 8 >= in_bits_consumed_ + 32 ? peek(32) : 0u);
    busy_ = false;
  }
  // Drop fully consumed bytes.
  const uint64_t drop = in_bits_consumed_ / 8;
  in_.erase(in_.begin(), in_.begin() + static_cast<std::ptrdiff_t>(drop));
  in_bits_consumed_ -= drop * 8;
  flush_output();
}

void Ipu::set_start_code_flag() {
  try {
    if (ensure_bits(23) && peek(23) == 0) ctrl_ |= 1u << 15;  // SCD
  } catch (const OutOfData&) {
  }
}

void Ipu::execute(uint32_t cmd) {
  const uint32_t code = cmd >> 28;
  switch (code) {
    case 0x0:  // BCLR
      in_.clear();
      in_bits_consumed_ = 0;
      pulled_qwords_ = 0;
      bclr_skip_ = cmd & 0x7F;
      return;
    case 0x1: cmd_idec(cmd); return;
    case 0x2: cmd_bdec(cmd); return;
    case 0x3: cmd_vdec(cmd); return;
    case 0x4:  // FDEC
      skip(cmd & 0x3F);
      result_ = peek(32);
      return;
    case 0x5: cmd_setiq(cmd); return;
    case 0x6: cmd_setvq(cmd); return;
    case 0x7: cmd_csc(cmd); return;
    case 0x8: cmd_pack(cmd); return;
    case 0x9:  // SETTH
      th0_ = cmd & 0x1FF;
      th1_ = (cmd >> 16) & 0x1FF;
      return;
    default:
      if (stats_.commands[code] == 1) log::warn("ipu", "unknown IPU command 0x{:x}", code);
      return;
  }
}

void Ipu::cmd_vdec(uint32_t cmd) {
  skip(cmd & 0x3F);
  const uint32_t tbl = (cmd >> 26) & 3;
  uint32_t len = 0;
  int value = 0;
  try {
    switch (tbl) {
      case 0: {
        uint32_t total = 0;
        for (;;) {  // MPEG-1 stuffing codes are skipped
          const int i = decode_vlc(mpeg::kMbaIncrement, &len);
          total += len;
          value = mpeg::kMbaIncrement[i].value;
          if (value != mpeg::kMbaStuffing) break;
        }
        if (value == mpeg::kMbaEscape) value = 33;
        len = total;
        break;
      }
      case 1: {
        const uint32_t pct = (ctrl_ >> 24) & 7;
        if (pct == 1)
          value = mpeg::kMbTypeI[decode_vlc(mpeg::kMbTypeI, &len)].value;
        else if (pct == 2)
          value = mpeg::kMbTypeP[decode_vlc(mpeg::kMbTypeP, &len)].value;
        else if (pct == 3)
          value = mpeg::kMbTypeB[decode_vlc(mpeg::kMbTypeB, &len)].value;
        else
          value = mpeg::kMbTypeD[decode_vlc(mpeg::kMbTypeD, &len)].value;
        break;
      }
      case 2: value = mpeg::kMotionCode[decode_vlc(mpeg::kMotionCode, &len)].value; break;
      case 3: value = mpeg::kDmVector[decode_vlc(mpeg::kDmVector, &len)].value; break;
    }
  } catch (const VlcError&) {
    result_ = 0;
    set_start_code_flag();
    throw;
  }
  result_ = (static_cast<uint32_t>(value) & 0xFFFF) | (len << 16);
  set_start_code_flag();
}

int Ipu::quantiser_scale(int code) const {
  return (ctrl_ & (1u << 22)) ? mpeg::kNonLinearQuant[code & 31] : 2 * (code & 31);
}

void Ipu::decode_block(int component, bool intra, int qscale, int16_t* out) {
  static const int mode = std::getenv("LANTERN_IPU_MP1") ? std::atoi(std::getenv("LANTERN_IPU_MP1")) : 0;
  const bool mpeg1 = mode == 1 ? (ctrl_ & (1u << 23)) != 0 : mode == 2 ? (ctrl_ & (1u << 23)) == 0 : false;
  const uint8_t* scan = (ctrl_ & (1u << 20)) ? mpeg::kAlternate : mpeg::kZigzag;
  const int idp = static_cast<int>((ctrl_ >> 16) & 3);
  int32_t qf[64] = {};
  int i;
  if (intra) {
    uint32_t len;
    const int size = component == 0 ? mpeg::kDcSizeLuma[decode_vlc(mpeg::kDcSizeLuma, &len)].value
                                    : mpeg::kDcSizeChroma[decode_vlc(mpeg::kDcSizeChroma, &len)].value;
    int diff = 0;
    if (size) {
      const int v = static_cast<int>(get(static_cast<uint32_t>(size)));
      diff = v < (1 << (size - 1)) ? v - (1 << size) + 1 : v;
    }
    dc_pred_[component] += diff;
    qf[0] = dc_pred_[component];
    i = 0;
  } else {
    i = -1;
  }

  const bool table1 = intra && (ctrl_ & (1u << 21));
  bool first = !intra;
  for (;;) {
    int run, level;
    if (first && peek(1) == 1) {  // non-intra first coefficient: "1s" = run 0, level 1
      skip(1);
      run = 0;
      level = get(1) ? -1 : 1;
    } else {
      uint32_t len;
      const auto& e = table1 ? mpeg::kDctTable1[decode_vlc(mpeg::kDctTable1, &len)]
                             : mpeg::kDctTable0[decode_vlc(mpeg::kDctTable0, &len)];
      if (e.kind == mpeg::kDctEob) break;
      if (e.kind == mpeg::kDctEscape) {
        run = static_cast<int>(get(6));
        if (mpeg1) {
          int l = static_cast<int>(get(8));
          if (l == 0)
            l = static_cast<int>(get(8));
          else if (l == 128)
            l = static_cast<int>(get(8)) - 256;
          else if (l > 128)
            l -= 256;
          level = l;
        } else {
          int l = static_cast<int>(get(12));
          if (l & 0x800) l -= 4096;
          level = l;
        }
        if (level == 0) throw VlcError{};
      } else {
        run = e.run;
        level = get(1) ? -e.level : e.level;
      }
    }
    static const uint64_t trace_mb = std::getenv("LANTERN_IPU_BLOCKTRACE")
                                         ? std::strtoull(std::getenv("LANTERN_IPU_BLOCKTRACE"), nullptr, 10)
                                         : ~0ull;
    if (stats_.macroblocks + 1 == trace_mb)
      log::info("ipu", "  comp {} intra {} coef run {} level {} -> idx {} (bitpos {})", component, intra, run, level,
                i + run + 1, in_bits_consumed_);
    first = false;
    i += run + 1;
    if (i > 63) throw VlcError{};
    qf[scan[i]] = level;
  }

  // Inverse quantisation (ISO/IEC 13818-2 7.4; MPEG-1 uses the same arithmetic with oddification).
  const uint8_t* w = intra ? intra_matrix_.data() : nonintra_matrix_.data();
  int32_t f[64];
  int32_t sum = 0;
  for (int k = 0; k < 64; ++k) {
    int32_t v;
    if (intra && k == 0) {
      v = qf[0] * (mpeg1 ? 8 : (8 >> idp));
    } else if (qf[k] == 0) {
      v = 0;
    } else {
      const int32_t kk = intra ? 0 : (qf[k] > 0 ? 1 : -1);
      v = static_cast<int32_t>((static_cast<int64_t>(2 * qf[k] + kk) * w[k] * qscale) / 32);
      if (mpeg1 && v != 0 && (v & 1) == 0) v -= v > 0 ? 1 : -1;
    }
    v = std::clamp(v, -2048, 2047);
    f[k] = v;
    sum += v;
  }
  if (!mpeg1 && (sum & 1) == 0) f[63] += (f[63] & 1) ? -1 : 1;

  int32_t pix[64];
  idct8x8(f, pix);
  for (int k = 0; k < 64; ++k) out[k] = static_cast<int16_t>(intra ? std::clamp(pix[k], 0, 255) : pix[k]);
}

namespace {
// Place six decoded 8x8 blocks into a 16x16 luma + two 8x8 chroma macroblock.
template <typename T>
void assemble(const int16_t blocks[6][64], bool field_dct, T* y, T* cb, T* cr) {
  for (int b = 0; b < 4; ++b) {
    const int bx = (b & 1) * 8;
    for (int r = 0; r < 8; ++r) {
      const int row = field_dct ? (r * 2 + (b >> 1)) : (r + (b >> 1) * 8);
      for (int c = 0; c < 8; ++c) y[row * 16 + bx + c] = static_cast<T>(blocks[b][r * 8 + c]);
    }
  }
  for (int k = 0; k < 64; ++k) {
    cb[k] = static_cast<T>(blocks[4][k]);
    cr[k] = static_cast<T>(blocks[5][k]);
  }
}
}  // namespace

void Ipu::cmd_bdec(uint32_t cmd) {
  if (const char* t = std::getenv("LANTERN_IPU_BLOCKTRACE");
      t && stats_.macroblocks + 1 == std::strtoull(t, nullptr, 10)) {
    std::string bits;
    for (uint64_t k = in_bits_consumed_; k < in_bits_consumed_ + 256 && k / 8 < in_.size(); ++k)
      bits += ((in_[k >> 3] >> (7 - (k & 7))) & 1) ? '1' : '0';
    log::info("ipu", "  BDEC bits: {}", bits);
  }
  skip(cmd & 0x3F);
  const int qscale = quantiser_scale(static_cast<int>((cmd >> 16) & 31));
  const bool field_dct = (cmd >> 25) & 1, dc_reset = (cmd >> 26) & 1, intra = (cmd >> 27) & 1;
  const int reset_value = 1 << (7 + static_cast<int>((ctrl_ >> 16) & 3));
  if (dc_reset || !intra)
    for (int& p : dc_pred_) p = reset_value;

  uint32_t cbp = 0x3F;
  if (!intra) {
    uint32_t len;
    cbp = static_cast<uint32_t>(mpeg::kCbp[decode_vlc(mpeg::kCbp, &len)].value);
  }
  int16_t blocks[6][64] = {};
  for (int b = 0; b < 6; ++b)
    if (cbp & (32u >> b)) {
      try {
        decode_block(b < 4 ? 0 : b - 3, intra, qscale, blocks[b]);
      } catch (const OutOfData&) {
        stall_mb_ = stats_.macroblocks;
        stall_block_ = b;
        stall_cbp_ = static_cast<int>(cbp);
        stall_intra_ = intra ? 1 : 0;
        stall_bits_ = in_bits_consumed_;
        throw;
      }
    }

  int16_t mb[384];
  assemble(blocks, field_dct, mb, mb + 256, mb + 320);
  std::vector<uint8_t> bytes(sizeof(mb));
  std::memcpy(bytes.data(), mb, sizeof(mb));
  ctrl_ = (ctrl_ & ~(0x3Fu << 8)) | (cbp << 8);
  if (const char* path = std::getenv("LANTERN_IPU_DUMP"); path && stats_.macroblocks < 768) {
    static std::vector<uint8_t> luma(512 * 384);
    const uint32_t mbx = static_cast<uint32_t>(stats_.macroblocks % 32),
                   mby = static_cast<uint32_t>(stats_.macroblocks / 32);
    for (int y = 0; y < 16; ++y)
      for (int x = 0; x < 16; ++x)
        luma[(mby * 16 + y) * 512 + mbx * 16 + x] = static_cast<uint8_t>(std::clamp<int>(mb[y * 16 + x], 0, 255));
    if (stats_.macroblocks == 767)
      if (FILE* f = std::fopen(path, "wb")) {
        std::fprintf(f, "P5 512 384 255\n");
        std::fwrite(luma.data(), 1, luma.size(), f);
        std::fclose(f);
      }
  }
  ++stats_.macroblocks;
  emit(bytes);
  set_start_code_flag();
}

void Ipu::cmd_idec(uint32_t cmd) {
  skip(cmd & 0x3F);
  int qscale_code = static_cast<int>((cmd >> 16) & 31);
  const bool decode_dt = (cmd >> 24) & 1, sign = (cmd >> 25) & 1, dither = (cmd >> 26) & 1, rgb16 = (cmd >> 27) & 1;
  const int reset_value = 1 << (7 + static_cast<int>((ctrl_ >> 16) & 3));
  for (int& p : dc_pred_) p = reset_value;

  std::vector<uint8_t> out;
  for (int mbs = 0; mbs < 8192; ++mbs) {
    uint32_t len;
    const int type = mpeg::kMbTypeI[decode_vlc(mpeg::kMbTypeI, &len)].value;
    if (type & mpeg::kMbQuant) qscale_code = static_cast<int>(get(5));
    const bool field_dct = decode_dt ? get(1) != 0 : false;
    int16_t blocks[6][64];
    for (int b = 0; b < 6; ++b) decode_block(b < 4 ? 0 : b - 3, true, quantiser_scale(qscale_code), blocks[b]);
    uint8_t raw[384];
    assemble(blocks, field_dct, raw, raw + 256, raw + 320);
    rgb_from_raw8(raw, rgb16, dither, sign, out);
    ++stats_.macroblocks;

    // Slice continues while the next macroblock follows immediately.
    if (ensure_bits(23) && peek(23) == 0) {
      ctrl_ |= 1u << 15;  // SCD: end of slice
      break;
    }
    int inc = 0;
    for (;;) {
      const int i = decode_vlc(mpeg::kMbaIncrement, &len);
      const int v = mpeg::kMbaIncrement[i].value;
      if (v == mpeg::kMbaStuffing) continue;
      inc += v == mpeg::kMbaEscape ? 33 : v;
      if (v != mpeg::kMbaEscape) break;
    }
    if (inc != 1) {
      ctrl_ |= 1u << 14;  // ECD: skipped macroblocks are invalid in intra slices
      break;
    }
  }
  emit(out);
}

void Ipu::cmd_setiq(uint32_t cmd) {
  skip(cmd & 0x3F);
  auto& m = (cmd >> 27) & 1 ? nonintra_matrix_ : intra_matrix_;
  std::array<uint8_t, 64> tmp{};
  for (int k = 0; k < 64; ++k) tmp[mpeg::kZigzag[k]] = static_cast<uint8_t>(get(8));
  m = tmp;
}

void Ipu::cmd_setvq(uint32_t) {
  for (auto& e : vqclut_) {
    const uint32_t lo = get(8), hi = get(8);
    e = static_cast<uint16_t>(lo | (hi << 8));
  }
}

void Ipu::rgb_from_raw8(const uint8_t* raw, bool rgb16, bool dither, bool sign, std::vector<uint8_t>& out) {
  (void)dither;  // dithering for RGB16 output is not modelled
  for (int y = 0; y < 16; ++y)
    for (int x = 0; x < 16; ++x) {
      const double Y = raw[y * 16 + x];
      const double Cb = raw[256 + (y / 2) * 8 + x / 2] - 128.0, Cr = raw[320 + (y / 2) * 8 + x / 2] - 128.0;
      const double yy = 1.164 * (Y - 16.0);
      int r = std::clamp(static_cast<int>(std::lround(yy + 1.596 * Cr)), 0, 255);
      int g = std::clamp(static_cast<int>(std::lround(yy - 0.391 * Cb - 0.813 * Cr)), 0, 255);
      int b = std::clamp(static_cast<int>(std::lround(yy + 2.018 * Cb)), 0, 255);
      uint8_t a = 0x80;
      if (static_cast<uint32_t>(r) < th0_ && static_cast<uint32_t>(g) < th0_ && static_cast<uint32_t>(b) < th0_)
        a = 0;
      else if (static_cast<uint32_t>(r) < th1_ && static_cast<uint32_t>(g) < th1_ && static_cast<uint32_t>(b) < th1_)
        a = 0x40;
      if (sign) {
        r = (r - 128) & 0xFF;
        g = (g - 128) & 0xFF;
        b = (b - 128) & 0xFF;
      }
      if (rgb16) {
        const uint16_t p = static_cast<uint16_t>((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | (a ? 0x8000 : 0));
        out.push_back(static_cast<uint8_t>(p));
        out.push_back(static_cast<uint8_t>(p >> 8));
      } else {
        out.push_back(static_cast<uint8_t>(r));
        out.push_back(static_cast<uint8_t>(g));
        out.push_back(static_cast<uint8_t>(b));
        out.push_back(a);
      }
    }
}

void Ipu::cmd_csc(uint32_t cmd) {
  const uint32_t count = cmd & 0x7FF;
  const bool dither = (cmd >> 26) & 1, rgb16 = (cmd >> 27) & 1;
  // Read everything first so a stall cannot leave partial output behind.
  if (!ensure_bits(count * 384 * 8)) throw OutOfData{};
  std::vector<uint8_t> out;
  uint8_t raw[384];
  for (uint32_t m = 0; m < count; ++m) {
    for (auto& v : raw) v = static_cast<uint8_t>(get(8));
    rgb_from_raw8(raw, rgb16, dither, false, out);
    ++stats_.csc_macroblocks;
  }
  emit(out);
}

void Ipu::cmd_pack(uint32_t cmd) {
  const uint32_t count = cmd & 0x7FF;
  const bool indx4 = ((cmd >> 27) & 1) == 0;
  if (!ensure_bits(count * 1024 * 8)) throw OutOfData{};
  std::vector<uint8_t> out;
  for (uint32_t m = 0; m < count; ++m) {
    uint8_t nibble_pair = 0;
    for (int p = 0; p < 256; ++p) {
      const uint32_t r = get(8), g = get(8), b = get(8), a = get(8);
      if (!indx4) {
        const uint16_t v = static_cast<uint16_t>((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | (a ? 0x8000 : 0));
        out.push_back(static_cast<uint8_t>(v));
        out.push_back(static_cast<uint8_t>(v >> 8));
        continue;
      }
      // Nearest VQ CLUT entry (RGB555 compared in 8-bit space).
      int best = 0;
      uint32_t best_d = ~0u;
      for (int k = 0; k < 16; ++k) {
        const uint16_t c = vqclut_[static_cast<size_t>(k)];
        const int cr = (c & 31) << 3, cg = ((c >> 5) & 31) << 3, cb = ((c >> 10) & 31) << 3;
        const uint32_t d = static_cast<uint32_t>((cr - static_cast<int>(r)) * (cr - static_cast<int>(r)) +
                                                 (cg - static_cast<int>(g)) * (cg - static_cast<int>(g)) +
                                                 (cb - static_cast<int>(b)) * (cb - static_cast<int>(b)));
        if (d < best_d) {
          best_d = d;
          best = k;
        }
      }
      if (p & 1) {
        out.push_back(static_cast<uint8_t>(nibble_pair | (best << 4)));
      } else {
        nibble_pair = static_cast<uint8_t>(best);
      }
    }
  }
  emit(out);
}

// ---------------------------------------------------------------------------------------------
// Output

void Ipu::emit(const std::vector<uint8_t>& bytes) {
  out_.insert(out_.end(), bytes.begin(), bytes.end());
  stats_.output_qwords += bytes.size() / 16;
  flush_output();
}

void Ipu::flush_output() {
  if (!from_active_) return;
  uint32_t qwc = hw_.dma_reg(kDmaFrom + 0x20) & 0xFFFF;
  uint32_t madr = hw_.dma_reg(kDmaFrom + 0x10);
  while (qwc > 0 && out_.size() >= 16) {
    const uint32_t addr = (madr & 0x80000000u) ? (0x70000000u | (madr & 0x3FF0)) : madr;
    if (uint8_t* p = host_(addr, 16)) {
      for (int i = 0; i < 16; ++i) {
        p[i] = out_.front();
        out_.pop_front();
      }
    } else {
      out_.erase(out_.begin(), out_.begin() + 16);
    }
    madr += 16;
    --qwc;
  }
  hw_.set_dma_reg(kDmaFrom + 0x10, madr);
  hw_.set_dma_reg(kDmaFrom + 0x20, qwc);
  if (qwc == 0) {
    from_active_ = false;
    hw_.dma_done(3);
  }
}

}  // namespace lantern::ps2
