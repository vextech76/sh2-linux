#include "gs/gs.h"

#include <algorithm>
#include <bit>
#include <cstring>

#include "core/log.h"

namespace lantern::gs {
namespace {
constexpr const char* kSub = "gs";
enum Reg : uint8_t {
  kPrim = 0x00,
  kRgbaq = 0x01,
  kSt = 0x02,
  kUv = 0x03,
  kXyzf2 = 0x04,
  kXyz2 = 0x05,
  kFog = 0x0A,
  kXyzf3 = 0x0C,
  kXyz3 = 0x0D,
  kPrmode = 0x1B,
  kBitbltbuf = 0x50,
  kTrxpos = 0x51,
  kTrxreg = 0x52,
  kTrxdir = 0x53,
  kHwreg = 0x54,
  kSignal = 0x60,
  kFinish = 0x61,
  kLabel = 0x62,
};
constexpr uint64_t bits(uint64_t v, unsigned lo, unsigned n) { return (v >> lo) & ((1ull << n) - 1); }
constexpr int kVertsPerPrim[8] = {1, 2, 2, 3, 3, 3, 2, 1};  // point, line, strip, tri, strip, fan, sprite, invalid
constexpr uint32_t kCsrSignal = 1u << 0, kCsrFinish = 1u << 1;
}  // namespace

void Gs::write_reg(uint8_t r, uint64_t v) {
  regs_[r] = v;
  switch (r) {
    case kPrim: queued_ = 0; break;
    case kRgbaq:
      cur_.r = static_cast<uint8_t>(v);
      cur_.g = static_cast<uint8_t>(v >> 8);
      cur_.b = static_cast<uint8_t>(v >> 16);
      cur_.a = static_cast<uint8_t>(v >> 24);
      cur_.q = std::bit_cast<float>(static_cast<uint32_t>(v >> 32));
      break;
    case kSt:
      cur_.s = std::bit_cast<float>(static_cast<uint32_t>(v));
      cur_.t = std::bit_cast<float>(static_cast<uint32_t>(v >> 32));
      break;
    case kUv:
      cur_.u = static_cast<uint32_t>(bits(v, 0, 14));
      cur_.v = static_cast<uint32_t>(bits(v, 16, 14));
      break;
    case kFog: cur_.fog = static_cast<uint8_t>(bits(v, 56, 8)); break;
    case kXyzf2:
    case kXyzf3:
      cur_.x = static_cast<int32_t>(bits(v, 0, 16));
      cur_.y = static_cast<int32_t>(bits(v, 16, 16));
      cur_.z = static_cast<uint32_t>(bits(v, 32, 24));
      cur_.fog = static_cast<uint8_t>(bits(v, 56, 8));
      vertex_kick(r == kXyzf2);
      break;
    case kXyz2:
    case kXyz3:
      cur_.x = static_cast<int32_t>(bits(v, 0, 16));
      cur_.y = static_cast<int32_t>(bits(v, 16, 16));
      cur_.z = static_cast<uint32_t>(v >> 32);
      vertex_kick(r == kXyz2);
      break;
    case 0x06:
    case 0x07: load_clut(v); break;  // TEX0_1/2
    case 0x16:
    case 0x17: {  // TEX2_1/2: replaces PSM and CLUT fields of TEX0, then loads the CLUT
      const uint64_t mask = (0x3Full << 20) | (0x7FFFFFFull << 37);  // PSM, CBP, CPSM, CSM, CSA, CLD
      uint64_t& t0 = regs_[r == 0x16 ? 0x06 : 0x07];
      t0 = (t0 & ~mask) | (v & mask);
      load_clut(t0);
      break;
    }
    case kTrxdir: start_transfer(); break;
    case kHwreg: write_hwreg(v); break;
    case kSignal:
      ++stats_.signal;
      csr_ |= kCsrSignal;
      if (!(imr_ & (1u << 8))) irq_ = true;
      break;
    case kFinish:
      ++stats_.finish;
      csr_ |= kCsrFinish;
      if (!(imr_ & (1u << 9))) irq_ = true;
      break;
    case kLabel: break;
    default: break;
  }
}

void Gs::load_clut(uint64_t tex0) {
  const uint32_t cld = static_cast<uint32_t>(bits(tex0, 61, 3));
  const uint32_t cbp = static_cast<uint32_t>(bits(tex0, 37, 14));
  bool load = false;
  switch (cld) {
    case 1: load = true; break;
    case 2:
      load = true;
      cbp0_ = cbp;
      break;
    case 3:
      load = true;
      cbp1_ = cbp;
      break;
    case 4:
      load = cbp != cbp0_;
      cbp0_ = cbp;
      break;
    case 5:
      load = cbp != cbp1_;
      cbp1_ = cbp;
      break;
    default: break;
  }
  if (!load) return;
  const uint32_t psm = static_cast<uint32_t>(bits(tex0, 20, 6)), cpsm = static_cast<uint32_t>(bits(tex0, 51, 4));
  const uint32_t csa = static_cast<uint32_t>(bits(tex0, 56, 5));
  const bool eight = psm == kT8 || psm == kT8h;
  const uint32_t entries = eight ? 256 : 16;
  const uint32_t clut_psm = cpsm == 0 ? kCt32 : cpsm == 2 ? kCt16 : kCt16s;
  for (uint32_t i = 0; i < entries; ++i) {
    // CSM1 layout: 8x2 entries for 4-bit, 16x16 for 8-bit with index bits 3 and 4 swapped.
    uint32_t x, y;
    if (eight) {
      const uint32_t j = (i & 0xE7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1);
      x = j % 16;
      y = j / 16;
    } else {
      x = i % 8;
      y = i / 8;
    }
    const uint32_t slot = eight ? i : (csa * 16 + i) % 256;
    clut_[slot] = mem_.read_pixel(clut_psm, cbp, 1, x, y);
  }
  clut_is16_ = clut_psm != kCt32;
}

void Gs::vertex_kick(bool draw) {
  const uint64_t prim = regs_[kPrim];
  const uint32_t type = static_cast<uint32_t>(prim & 7);
  ++stats_.kicks[type];
  const int need = kVertsPerPrim[type];
  if (type == 7) return;
  // Strips/fans keep the previous vertices (fan keeps the first).
  if (queued_ < need) {
    queue_[queued_++] = cur_;
  } else if (type == 2 || type == 4) {
    queue_[0] = queue_[1];
    if (need == 3) queue_[1] = queue_[2];
    queue_[need - 1] = cur_;
  } else if (type == 5) {
    queue_[1] = queue_[2];
    queue_[2] = cur_;
  } else {
    queue_[0] = cur_;
    queued_ = 1;
  }
  if (queued_ < need || !draw) return;
  ++stats_.primitives;
  if (draw_) draw_(*this, queue_, need, prim);
  if (type == 0 || type == 1 || type == 3 || type == 6) queued_ = 0;
}

// ---- local memory transfers ------------------------------------------------------------------
void Gs::start_transfer() {
  const uint32_t dir = static_cast<uint32_t>(regs_[kTrxdir] & 3);
  xfer_bytes_.clear();
  xfer_active_ = false;
  if (dir == 0) {
    xfer_active_ = true;
    xfer_x_ = xfer_y_ = 0;
    ++stats_.image_transfers;
  } else if (dir == 2) {
    local_to_local();
  } else if (dir == 1) {
    log::warn(kSub, "PLACEHOLDER: GS local->host transfer requested (not modelled)");
  }
}

void Gs::write_hwreg(uint64_t data) {
  if (!xfer_active_) return;
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&data);
  xfer_bytes_.insert(xfer_bytes_.end(), p, p + 8);
  put_pixel_stream();
}

void Gs::put_pixel_stream() {
  const uint64_t bb = regs_[kBitbltbuf], pos = regs_[kTrxpos], rg = regs_[kTrxreg];
  const uint32_t dbp = static_cast<uint32_t>(bits(bb, 32, 14)), dbw = static_cast<uint32_t>(bits(bb, 48, 6)),
                 dpsm = static_cast<uint32_t>(bits(bb, 56, 6));
  const uint32_t dx = static_cast<uint32_t>(bits(pos, 32, 11)), dy = static_cast<uint32_t>(bits(pos, 48, 11));
  const uint32_t w = static_cast<uint32_t>(bits(rg, 0, 12)), h = static_cast<uint32_t>(bits(rg, 32, 12));
  if (!w || !h) return;
  if (!LocalMemory::supported(dpsm)) {
    if (stats_.unsupported_psm[dpsm]++ == 0)
      log::warn(kSub, "PLACEHOLDER: image transfer in pixel format 0x{:02x} not modelled yet; data dropped", dpsm);
    xfer_bytes_.clear();
    return;
  }
  const bool indexed = dpsm == kT8 || dpsm == kT4 || dpsm == kT8h || dpsm == kT4hl || dpsm == kT4hh;
  if (indexed) {  // 8 or 4 bits per pixel, packed LSB-first
    const bool four = dpsm == kT4 || dpsm == kT4hl || dpsm == kT4hh;
    size_t used = 0;
    for (size_t bi = 0; xfer_active_ && bi < xfer_bytes_.size(); ++bi) {
      const uint8_t byte = xfer_bytes_[bi];
      for (int k = 0; k < (four ? 2 : 1) && xfer_active_; ++k) {
        const uint32_t v = four ? (byte >> (4 * k)) & 0xF : byte;
        mem_.write_index(dpsm, dbp, dbw, (dx + xfer_x_) % 2048, (dy + xfer_y_) % 2048, v);
        ++stats_.image_pixels;
        if (++xfer_x_ == w) {
          xfer_x_ = 0;
          if (++xfer_y_ == h) xfer_active_ = false;
        }
      }
      used = bi + 1;
    }
    xfer_bytes_.erase(xfer_bytes_.begin(), xfer_bytes_.begin() + static_cast<std::ptrdiff_t>(used));
    return;
  }
  const uint32_t bpp = dpsm == kCt32 || dpsm == kZ32 ? 4 : (dpsm == kCt24 || dpsm == kZ24) ? 3 : 2;
  size_t i = 0;
  while (xfer_active_ && i + bpp <= xfer_bytes_.size()) {
    uint32_t v = 0;
    std::memcpy(&v, &xfer_bytes_[i], bpp);
    i += bpp;
    mem_.write_pixel(dpsm, dbp, dbw, (dx + xfer_x_) % 2048, (dy + xfer_y_) % 2048, v);
    ++stats_.image_pixels;
    if (++xfer_x_ == w) {
      xfer_x_ = 0;
      if (++xfer_y_ == h) xfer_active_ = false;
    }
  }
  xfer_bytes_.erase(xfer_bytes_.begin(), xfer_bytes_.begin() + static_cast<std::ptrdiff_t>(i));
}

void Gs::local_to_local() {
  const uint64_t bb = regs_[kBitbltbuf], pos = regs_[kTrxpos], rg = regs_[kTrxreg];
  const uint32_t sbp = static_cast<uint32_t>(bits(bb, 0, 14)), sbw = static_cast<uint32_t>(bits(bb, 16, 6)),
                 spsm = static_cast<uint32_t>(bits(bb, 24, 6));
  const uint32_t dbp = static_cast<uint32_t>(bits(bb, 32, 14)), dbw = static_cast<uint32_t>(bits(bb, 48, 6)),
                 dpsm = static_cast<uint32_t>(bits(bb, 56, 6));
  const uint32_t sx = static_cast<uint32_t>(bits(pos, 0, 11)), sy = static_cast<uint32_t>(bits(pos, 16, 11));
  const uint32_t dx = static_cast<uint32_t>(bits(pos, 32, 11)), dy = static_cast<uint32_t>(bits(pos, 48, 11));
  const uint32_t w = static_cast<uint32_t>(bits(rg, 0, 12)), h = static_cast<uint32_t>(bits(rg, 32, 12));
  if (!LocalMemory::supported(spsm) || !LocalMemory::supported(dpsm)) {
    if (stats_.unsupported_psm[spsm]++ == 0)
      log::warn(kSub, "PLACEHOLDER: local copy in format 0x{:02x} skipped", spsm);
    return;
  }
  ++stats_.local_copies;
  std::vector<uint32_t> tmp(size_t{w} * h);
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x)
      tmp[size_t{y} * w + x] = mem_.read_pixel(spsm, sbp, sbw, (sx + x) % 2048, (sy + y) % 2048);
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x)
      mem_.write_pixel(dpsm, dbp, dbw, (dx + x) % 2048, (dy + y) % 2048, tmp[size_t{y} * w + x]);
}

// ---- privileged registers ----------------------------------------------------------------------
void Gs::write_priv32(uint32_t off, uint32_t value) {
  const uint32_t base = off & ~7u;
  uint64_t& r = priv_[base];
  if (off & 4)
    r = (r & 0xFFFFFFFFull) | (uint64_t{value} << 32);
  else
    r = (r & ~0xFFFFFFFFull) | value;
  if (base == 0x1000) {  // CSR: write 1 to SIGNAL/FINISH/HSINT/VSINT/EDWINT clears them
    csr_ &= ~(value & 0x1F);
  } else if (base == 0x1010) {
    imr_ = static_cast<uint32_t>(r);
  }
}

uint32_t Gs::read_priv32(uint32_t off) const {
  if ((off & ~7u) == 0x1000) {
    if (off & 4) return 0;
    return csr_ | (field_ ? 1u << 13 : 0) | (0x1Bu << 16) | (0x55u << 24);  // FIELD, REV, ID
  }
  auto it = priv_.find(off & ~7u);
  if (it == priv_.end()) return 0;
  return off & 4 ? static_cast<uint32_t>(it->second >> 32) : static_cast<uint32_t>(it->second);
}

uint64_t Gs::priv(uint32_t off) const {
  auto it = priv_.find(off);
  return it == priv_.end() ? 0 : it->second;
}

Image Gs::display() const {
  const uint64_t pmode = priv(0x0000);
  // Prefer read circuit 2 (DISPFB2/DISPLAY2), else circuit 1.
  const bool use2 = pmode & 2, use1 = pmode & 1;
  if (!use1 && !use2) return {};
  const uint64_t dispfb = priv(use2 ? 0x0090 : 0x0070), display = priv(use2 ? 0x00A0 : 0x0080);
  const uint32_t fbp = static_cast<uint32_t>(bits(dispfb, 0, 9)), fbw = static_cast<uint32_t>(bits(dispfb, 9, 6)),
                 psm = static_cast<uint32_t>(bits(dispfb, 15, 5));
  const uint32_t dbx = static_cast<uint32_t>(bits(dispfb, 32, 11)), dby = static_cast<uint32_t>(bits(dispfb, 43, 11));
  const uint32_t magh = static_cast<uint32_t>(bits(display, 23, 4)) + 1,
                 magv = static_cast<uint32_t>(bits(display, 27, 2)) + 1;
  const uint32_t dw = static_cast<uint32_t>(bits(display, 32, 12)) + 1,
                 dh = static_cast<uint32_t>(bits(display, 44, 11)) + 1;
  Image img;
  img.width = std::clamp<uint32_t>(dw / magh, 1, 2048);
  // SMODE2: INT (bit 0) with FFMD (bit 1) = 0 is interlaced frame mode, where DH counts
  // half-lines: the framebuffer holds half as many rows.
  const uint64_t smode2 = priv(0x0020);
  const uint32_t lines = (smode2 & 1) && !(smode2 & 2) ? dh / 2 : dh;
  img.height = std::clamp<uint32_t>(lines / magv, 1, 2048);
  img.rgba.resize(size_t{img.width} * img.height);
  const uint32_t bp = fbp * 32;  // FBP is in 2048-word pages = 32 blocks
  for (uint32_t y = 0; y < img.height; ++y)
    for (uint32_t x = 0; x < img.width; ++x) {
      uint32_t c = mem_.read_pixel(psm, bp, fbw, (dbx + x) % 2048, (dby + y) % 2048);
      if (psm == kCt16 || psm == kCt16s) {
        const uint32_t r = (c & 0x1F) << 3, g = ((c >> 5) & 0x1F) << 3, b = ((c >> 10) & 0x1F) << 3;
        c = r | (g << 8) | (b << 16);
      }
      img.rgba[size_t{y} * img.width + x] = c | 0xFF000000u;
    }
  return img;
}

Image Gs::sample_texture(uint32_t tbp, uint32_t tbw, uint32_t psm, uint32_t tw, uint32_t th) const {
  Image img;
  img.width = std::clamp<uint32_t>(tw, 1, 1024);
  img.height = std::clamp<uint32_t>(th, 1, 1024);
  img.rgba.resize(size_t{img.width} * img.height);
  for (uint32_t y = 0; y < img.height; ++y)
    for (uint32_t x = 0; x < img.width; ++x) {
      uint32_t c;
      switch (psm) {
        case kCt32:
        case kCt24:
        case kCt16:
        case kCt16s: c = mem_.read_pixel(psm, tbp, tbw, x, y); break;
        default: {
          const uint32_t i = mem_.read_index(psm, tbp, tbw, x, y);
          c = clut_is16_
                  ? ((clut_[i & 255] & 0x1F) << 3) | ((clut_[i & 255] & 0x3E0) << 6) | ((clut_[i & 255] & 0x7C00) << 9)
                  : clut_[i & 255];
          break;
        }
      }
      img.rgba[size_t{y} * img.width + x] = c | 0xFF000000u;
    }
  return img;
}

// ---- GIF ----------------------------------------------------------------------------------------
void Gif::process(const uint8_t* q, uint32_t count) {
  for (uint32_t n = 0; n < count; ++n, q += 16) {
    uint64_t lo, hi;
    std::memcpy(&lo, q, 8);
    std::memcpy(&hi, q + 8, 8);
    if (!in_packet_ || nloop_ == 0) {
      // GIFtag: NLOOP 0-14, EOP 15, PRE 46, PRIM 47-57, FLG 58-59, NREG 60-63, REGS (hi)
      nloop_ = static_cast<uint32_t>(lo & 0x7FFF);
      eop_ = (lo >> 15) & 1;
      flg_ = static_cast<uint32_t>((lo >> 58) & 3);
      nreg_ = static_cast<uint32_t>((lo >> 60) & 0xF);
      if (nreg_ == 0) nreg_ = 16;
      regs_ = hi;
      reg_index_ = 0;
      in_packet_ = nloop_ != 0;
      ++packets_;
      if ((lo >> 46) & 1) gs_.write_reg(0x00, (lo >> 47) & 0x7FF);  // PRE: set PRIM
      continue;
    }
    switch (flg_) {
      case 0: {  // PACKED: one register per quadword
        const uint32_t desc = static_cast<uint32_t>((regs_ >> (reg_index_ * 4)) & 0xF);
        switch (desc) {
          case 0x0: gs_.write_reg(0x00, lo & 0x7FF); break;
          case 0x1:  // RGBA (Q from the last STQ)
            gs_.write_reg(0x01, (lo & 0xFF) | ((lo >> 24) & 0xFF00) | ((hi & 0xFF) << 16) |
                                    (((hi >> 32) & 0xFF) << 24) | (gs_.reg(0x01) & 0xFFFFFFFF00000000ull));
            break;
          case 0x2:  // STQ
            gs_.write_reg(0x02, lo);
            gs_.write_reg(0x01, (gs_.reg(0x01) & 0xFFFFFFFFull) | (hi << 32));
            break;
          case 0x3: gs_.write_reg(0x03, (lo & 0x3FFF) | (((lo >> 32) & 0x3FFF) << 16)); break;
          case 0x4: {  // XYZF2 (ADC bit 111 -> no drawing kick)
            const uint64_t v = (lo & 0xFFFF) | (((lo >> 32) & 0xFFFF) << 16) | (((hi >> 4) & 0xFFFFFF) << 32) |
                               (((hi >> 36) & 0xFF) << 56);
            gs_.write_reg((hi >> 47) & 1 ? 0x0C : 0x04, v);
            break;
          }
          case 0x5: {  // XYZ2
            const uint64_t v = (lo & 0xFFFF) | (((lo >> 32) & 0xFFFF) << 16) | ((hi & 0xFFFFFFFF) << 32);
            gs_.write_reg((hi >> 47) & 1 ? 0x0D : 0x05, v);
            break;
          }
          case 0xA: gs_.write_reg(0x0A, ((hi >> 36) & 0xFF) << 56); break;
          case 0xE: gs_.write_reg(static_cast<uint8_t>(hi & 0xFF), lo); break;  // A+D
          case 0xF: break;                                                      // NOP
          default: gs_.write_reg(static_cast<uint8_t>(desc), lo); break;        // TEX0/CLAMP/XYZF3/XYZ3
        }
        if (++reg_index_ == nreg_) {
          reg_index_ = 0;
          --nloop_;
        }
        break;
      }
      case 1: {  // REGLIST: two 64-bit register values per quadword
        for (const uint64_t v : {lo, hi}) {
          if (nloop_ == 0) break;
          const uint32_t desc = static_cast<uint32_t>((regs_ >> (reg_index_ * 4)) & 0xF);
          if (desc != 0xE && desc != 0xF) gs_.write_reg(static_cast<uint8_t>(desc), v);
          if (++reg_index_ == nreg_) {
            reg_index_ = 0;
            --nloop_;
          }
        }
        break;
      }
      default:  // IMAGE (2/3): NLOOP quadwords of HWREG data
        gs_.write_hwreg(lo);
        gs_.write_hwreg(hi);
        --nloop_;
        break;
    }
    if (nloop_ == 0) in_packet_ = false;
  }
}

}  // namespace lantern::gs
