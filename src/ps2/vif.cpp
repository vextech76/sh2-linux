#include "ps2/vif.h"

#include <cstdlib>
#include <cstring>

#include "core/log.h"
#include "ps2/hardware.h"

namespace lantern::ps2 {
namespace {
constexpr const char* kSub = "vif1";
constexpr uint32_t kDmaBase[Hardware::kDmaChannels] = {0x10008000, 0x10009000, 0x1000A000, 0x1000B000, 0x1000B400,
                                                       0x1000C000, 0x1000C400, 0x1000C800, 0x1000D000, 0x1000D400};
}  // namespace

// ---- VIF1 ----------------------------------------------------------------------------------------

void Vif1::begin(uint32_t code) {
  cmd_ = (code >> 24) & 0x7F;
  num_ = (code >> 16) & 0xFF;
  imm_ = code & 0xFFFF;
  ++stats_.commands[cmd_];
  buf_.clear();
  need_ = 0;
  if ((cmd_ & 0x60) == 0x60) {  // UNPACK
    const uint32_t vn = (cmd_ >> 2) & 3, vl = cmd_ & 3;
    const uint32_t num = num_ ? num_ : 256;
    const uint32_t bits = (vn == 3 && vl == 3) ? 16 : (vn + 1) * (32u >> vl);
    const uint32_t elems = wl_ <= cl_ ? num : (num / wl_) * cl_ + std::min(num % wl_, cl_);
    need_ = (size_t{elems} * bits + 31) / 32;
    if (need_ == 0) unpack_word_stream();
    return;
  }
  switch (cmd_) {
    case 0x00: break;  // NOP
    case 0x01:         // STCYCL
      // ASSUMPTION (undocumented edge case): a zero CL/WL behaves like 256, as an 8-bit cycle
      // counter would. Only seen so far with misparsed VIF streams; keeps addressing defined.
      cl_ = imm_ & 0xFF;
      wl_ = (imm_ >> 8) & 0xFF;
      if (cl_ == 0) cl_ = 256;
      if (wl_ == 0) wl_ = 256;
      break;
    case 0x02: ofst_ = imm_ & 0x3FF; break;  // OFFSET
    case 0x03:                               // BASE
      base_ = imm_ & 0x3FF;
      tops_ = base_;
      break;
    case 0x04: itop_ = imm_ & 0x3FF; break;  // ITOP
    case 0x05: mode_ = imm_ & 3; break;      // STMOD
    case 0x06:                               // MSKPATH3
    case 0x07:                               // MARK
    case 0x10:                               // FLUSHE
    case 0x11:                               // FLUSH
    case 0x13: break;                        // FLUSHA (VU1/GIF are synchronous here)
    case 0x14:                               // MSCAL
    case 0x15:                               // MSCALF
    case 0x17: {                             // MSCNT (continue: VU1 tracks its own PC)
      top_ = tops_;
      tops_ = tops_ == base_ ? base_ + ofst_ : base_;
      ++stats_.vu1_starts;
      if (std::getenv("LANTERN_MPG_TRACE"))
        log::info(kSub, "VU1 start cmd 0x{:02x} word 0x{:08x} num {} imm 0x{:x} top 0x{:x} itop 0x{:x}", cmd_, code,
                  num_, imm_, tops_, itop_);
      if (start_) start_(cmd_ == 0x17 ? 0xFFFFFFFFu : imm_ * 8);
      break;
    }
    case 0x20: need_ = 1; break;  // STMASK
    case 0x30:
    case 0x31: need_ = 4; break;                               // STROW / STCOL
    case 0x4A: need_ = size_t{num_ ? num_ : 256u} * 2; break;  // MPG
    case 0x50:
    case 0x51: need_ = size_t{imm_ ? imm_ : 65536u} * 4; break;  // DIRECT / DIRECTHL
    default:
      if (stats_.commands[cmd_] == 1) log::warn(kSub, "unknown VIF code 0x{:02x}", cmd_);
      break;
  }
}

void Vif1::feed(const uint32_t* w, size_t count) {
  for (size_t i = 0; i < count;) {
    if (need_ == 0) {
      begin(w[i++]);
      continue;
    }
    // DIRECT streams can be large: pass whole quadwords straight to the GIF.
    if (cmd_ == 0x50 || cmd_ == 0x51) {
      while (need_ >= 4 && buf_.empty() && i + 4 <= count) {
        gif_.process(reinterpret_cast<const uint8_t*>(&w[i]), 1);
        ++stats_.direct_qwords;
        i += 4;
        need_ -= 4;
      }
      if (need_ == 0) continue;
    }
    const size_t take = std::min(need_, count - i);
    buf_.insert(buf_.end(), w + i, w + i + take);
    i += take;
    need_ -= take;
    if (need_ != 0) continue;
    switch (cmd_) {
      case 0x20: mask_ = buf_[0]; break;
      case 0x30: std::memcpy(row_, buf_.data(), 16); break;
      case 0x31: std::memcpy(col_, buf_.data(), 16); break;
      case 0x4A: {
        const uint32_t pairs = num_ ? num_ : 256;
        for (uint32_t p = 0; p < pairs; ++p) {
          const uint32_t at = ((imm_ + p) % 2048) * 8;
          std::memcpy(&vu_.micro[at], &buf_[p * 2], 8);
        }
        stats_.mpg_pairs += pairs;
        if (std::getenv("LANTERN_MPG_TRACE")) log::info(kSub, "MPG {} pairs at micro 0x{:x}", pairs, imm_ * 8);
        break;
      }
      case 0x50:
      case 0x51:
        gif_.process(reinterpret_cast<const uint8_t*>(buf_.data()), static_cast<uint32_t>(buf_.size() / 4));
        stats_.direct_qwords += buf_.size() / 4;
        break;
      default:
        if ((cmd_ & 0x60) == 0x60) unpack_word_stream();
        break;
    }
    buf_.clear();
  }
}

void Vif1::unpack_word_stream() {
  const uint32_t vn = (cmd_ >> 2) & 3, vl = cmd_ & 3;
  const bool masked = cmd_ & 0x10, usn = imm_ & 0x4000, flg = imm_ & 0x8000;
  const uint32_t num = num_ ? num_ : 256;
  uint32_t addr = (imm_ & 0x3FF) + (flg ? tops_ : 0);
  const bool v45 = vn == 3 && vl == 3;
  const uint32_t comp_bits = v45 ? 16 : 32u >> vl;
  const uint32_t comps = v45 ? 1 : vn + 1;
  size_t bitpos = 0;
  auto read_bits = [&](uint32_t n) -> uint32_t {
    uint64_t v = 0;
    for (uint32_t b = 0; b < n; ++b, ++bitpos) {
      const size_t word = bitpos / 32;
      if (word < buf_.size() && ((buf_[word] >> (bitpos % 32)) & 1)) v |= 1ull << b;
    }
    return static_cast<uint32_t>(v);
  };
  auto element = [&](uint32_t out[4], bool* present) {
    uint32_t raw[4] = {0, 0, 0, 0};
    for (uint32_t c = 0; c < comps; ++c) {
      uint32_t v = read_bits(comp_bits);
      if (!usn && comp_bits < 32 && !v45 && (v >> (comp_bits - 1)) & 1) v |= ~0u << comp_bits;  // sign-extend
      raw[c] = v;
    }
    if (v45) {
      const uint32_t v = raw[0];
      out[0] = (v & 0x1F) << 3;
      out[1] = ((v >> 5) & 0x1F) << 3;
      out[2] = ((v >> 10) & 0x1F) << 3;
      out[3] = ((v >> 15) & 1) << 7;
      present[0] = present[1] = present[2] = present[3] = true;
      return;
    }
    if (vn == 0) {
      for (int c = 0; c < 4; ++c) {
        out[c] = raw[0];
        present[c] = true;
      }
      return;
    }
    for (uint32_t c = 0; c < 4; ++c) {
      out[c] = raw[c];
      present[c] = c < comps;
    }
  };
  const uint32_t writes = num;
  uint32_t elem_index = 0;
  for (uint32_t n = 0; n < writes; ++n) {
    uint32_t qw, cycle;
    bool from_data = true;
    if (wl_ <= cl_) {
      qw = addr + (n / wl_) * cl_ + n % wl_;
      cycle = n % wl_;
    } else {
      qw = addr + n;
      cycle = n % wl_;
      from_data = cycle < cl_;
    }
    uint32_t data[4] = {0, 0, 0, 0};
    bool present[4] = {false, false, false, false};
    if (from_data) {
      element(data, present);
      ++elem_index;
    }
    uint32_t* dst = reinterpret_cast<uint32_t*>(&vu_.data[(qw % 1024) * 16]);
    const uint32_t row_sel = std::min<uint32_t>(cycle, 3);
    for (int c = 0; c < 4; ++c) {
      uint32_t m = masked ? (mask_ >> (row_sel * 8 + c * 2)) & 3 : (from_data ? 0 : 1);
      if (m == 0 && !present[c]) continue;  // component not supplied by this format
      uint32_t v;
      switch (m) {
        case 0:
          v = data[c];
          if (mode_ == 1)
            v += row_[c];
          else if (mode_ == 2)
            row_[c] = v = v + row_[c];
          break;
        case 1: v = row_[c]; break;
        case 2: v = col_[row_sel]; break;
        default: continue;  // write-protected
      }
      dst[c] = v;
    }
    ++stats_.unpacked_qwords;
  }
  (void)elem_index;
}

// ---- DMAC ----------------------------------------------------------------------------------------

void Dmac::move(int ch, uint32_t addr, uint32_t qwc) {
  if (qwc == 0) return;
  uint8_t* p = host_(addr, qwc * 16);
  if (!p) {
    ++stats_.bad_addresses;
    return;
  }
  stats_.qwords[ch] += qwc;
  if (auto it = sinks_.find(ch); it != sinks_.end()) it->second(p, qwc);
}

bool Dmac::run(int ch) {
  const uint32_t base = kDmaBase[ch];
  const uint32_t chcr = hw_.dma_reg(base);
  uint32_t madr = hw_.dma_reg(base + 0x10), qwc = hw_.dma_reg(base + 0x20) & 0xFFFF, tadr = hw_.dma_reg(base + 0x30);
  const uint32_t mode = (chcr >> 2) & 3;
  ++stats_.transfers[ch];

  if (ch == 8 || ch == 9) {  // scratchpad <-> RAM (normal mode; SPR_TO also chains)
    uint32_t sadr = hw_.dma_reg(base + 0x80) & 0x3FF0;
    auto copy = [&](uint32_t ram, uint32_t n) {
      uint8_t* r = host_(ram, n * 16);
      uint8_t* s = host_(0x70000000 + sadr, n * 16);
      if (!r || !s) {
        ++stats_.bad_addresses;
        return;
      }
      if (ch == 8)
        std::memcpy(r, s, n * 16);
      else
        std::memcpy(s, r, n * 16);
      sadr = (sadr + n * 16) & 0x3FF0;
      stats_.qwords[ch] += n;
    };
    if (mode == 0 || ch == 8) {
      copy(madr, qwc);
      madr += qwc * 16;
    } else {
      for (int guard = 0; guard < (1 << 20); ++guard) {
        const uint8_t* t = host_(tadr, 16);
        if (!t) break;
        uint64_t tag;
        std::memcpy(&tag, t, 8);
        const uint32_t n = static_cast<uint32_t>(tag & 0xFFFF), id = static_cast<uint32_t>((tag >> 28) & 7);
        const uint32_t a = static_cast<uint32_t>(tag >> 32) & 0x7FFFFFF0;
        copy(id == 1 || id == 7 ? tadr + 16 : a, n);
        if (id == 1)
          tadr += 16 + n * 16;
        else if (id == 0 || id == 3 || id == 4)
          tadr += 16;
        else
          break;  // end and others terminate
        if (id == 0) break;
      }
    }
    hw_.set_dma_reg(base + 0x80, sadr);
    hw_.set_dma_reg(base + 0x10, madr);
    hw_.set_dma_reg(base + 0x20, 0);
    return true;
  }

  if (!sinks_.contains(ch)) return false;
  if (mode == 0) {
    move(ch, madr, qwc);
    madr += qwc * 16;
    qwc = 0;
  } else {
    uint32_t asr[2] = {hw_.dma_reg(base + 0x40), hw_.dma_reg(base + 0x50)};
    int sp = static_cast<int>((chcr >> 4) & 3);
    uint64_t last_tag = 0;
    for (int guard = 0;; ++guard) {
      if (guard > (1 << 20)) {
        log::error("dmac", "channel {}: DMA chain did not terminate; aborting transfer", ch);
        break;
      }
      const uint8_t* t = host_(tadr, 16);
      if (!t) {
        ++stats_.bad_addresses;
        break;
      }
      ++stats_.chain_tags;
      uint64_t tag;
      std::memcpy(&tag, t, 8);
      last_tag = tag;
      const uint32_t n = static_cast<uint32_t>(tag & 0xFFFF), id = static_cast<uint32_t>((tag >> 28) & 7);
      const bool irq = (tag >> 31) & 1;
      uint32_t a = static_cast<uint32_t>(tag >> 32) & 0x7FFFFFF0;
      if ((tag >> 63) & 1) a = 0x70000000 + (a & 0x3FF0);  // SPR bit
      if ((chcr & 0x40) && tag_sinks_.contains(ch)) {      // TTE: tag's upper 64 bits are VIF codes
        uint32_t tw[2];
        std::memcpy(tw, t + 8, 8);
        tag_sinks_[ch](tw, 2);
      }
      const bool inline_data = id == 1 || id == 2 || id == 5 || id == 6 || id == 7;
      move(ch, inline_data ? tadr + 16 : a, n);
      bool done = false;
      switch (id) {
        case 0: done = true; break;                // refe
        case 1: tadr = tadr + 16 + n * 16; break;  // cnt
        case 2: tadr = a; break;                   // next
        case 3:
        case 4: tadr += 16; break;  // ref / refs
        case 5:                     // call
          if (sp < 2) asr[sp++] = tadr + 16 + n * 16;
          tadr = a;
          break;
        case 6:  // ret
          if (sp > 0)
            tadr = asr[--sp];
          else
            done = true;
          break;
        default: done = true; break;  // end
      }
      if (done || (irq && (chcr & 0x80))) break;
    }
    hw_.set_dma_reg(base + 0x40, asr[0]);
    hw_.set_dma_reg(base + 0x50, asr[1]);
    hw_.set_dma_reg(base + 0x30, tadr);
    hw_.set_dma_reg(base, (chcr & 0x0000FFFFu & ~0x100u) | (static_cast<uint32_t>(last_tag >> 16) & 0xFFFF0000u));
  }
  hw_.set_dma_reg(base + 0x10, madr);
  hw_.set_dma_reg(base + 0x20, qwc);
  return true;
}

}  // namespace lantern::ps2
