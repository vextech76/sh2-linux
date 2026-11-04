#include "iop/services.h"

#include <cstring>
#include <format>

#include "core/log.h"

namespace lantern::iop {
namespace {
constexpr const char* kSub = "iop";
constexpr uint32_t kSectorSize = 2048;
// libcdvd enum values (ps2sdk common/include/libcdvd-common.h)
constexpr int32_t kCdComplete = 2, kCdPs2Dvd = 0x14, kCdStatPause = 0x0A, kCdErNo = 0;

uint32_t arg32(const RpcCall& c, size_t word) {
  uint32_t v = 0;
  if ((word + 1) * 4 <= c.args.size()) std::memcpy(&v, &c.args[word * 4], 4);
  return v;
}
void put32(RpcCall& c, size_t word, uint32_t v) {
  if (c.result.size() < (word + 1) * 4) c.result.resize((word + 1) * 4, 0);
  std::memcpy(&c.result[word * 4], &v, 4);
}
}  // namespace

void CdvdService::register_with(SifHle& sif) {
  sif.register_server(0x80000592, "cdvd-init", [this](RpcCall& c) { init(c); });
  sif.register_server(0x80000593, "cdvd-scmd", [this](RpcCall& c) { scmd(c); });
  sif.register_server(0x80000595, "cdvd-ncmd", [this](RpcCall& c) { ncmd(c); });
  sif.register_server(0x80000596, "cdvd-poweroff", [](RpcCall&) {});
  sif.register_server(0x80000597, "cdvd-search", [this](RpcCall& c) { search(c); });
  sif.register_server(0x8000059A, "cdvd-diskready", [](RpcCall& c) { put32(c, 0, kCdComplete); });
}

void CdvdService::init(RpcCall& c) {
  // CdInitPkt { init_result, cdvdfsv_version, cdvdman_version, verbose }. Versions 0 = "pre-2.0
  // SDK" behaviour, which selects the simplest read-alignment protocol on the EE side.
  put32(c, 0, 1);
  put32(c, 1, 0);
  put32(c, 2, 0);
  put32(c, 3, 0);
}

uint32_t CdvdService::read_sectors(uint32_t lbn, uint32_t sectors, uint32_t dest) {
  std::vector<uint8_t> buf(kSectorSize);
  uint32_t done = 0;
  for (; done < sectors; ++done) {
    if (!disc_.read_raw(uint64_t{lbn + done} * kSectorSize, buf)) break;
    if (uint8_t* p = ee_.host(dest + done * kSectorSize, kSectorSize))
      std::memcpy(p, buf.data(), kSectorSize);
    else
      ee_.load(dest + done * kSectorSize, buf.data(), kSectorSize);
  }
  stats_.sectors_read += done;
  return done;
}

void CdvdService::scmd(RpcCall& c) {
  switch (c.fno) {
    case 1:  // READCLOCK: result + sceCdCLOCK (BCD): stat, sec, min, hour, pad, day, month, year
      put32(c, 0, 1);
      put32(c, 1, 0x00000000);
      put32(c, 2, 0x26110300);  // 03 Nov 2026 (BCD), fixed: the HLE clock is not wired to host time
      break;
    case 3: put32(c, 0, kCdPs2Dvd); break;  // GETDISKTYPE
    case 4: put32(c, 0, kCdErNo); break;    // GETERROR
    case 5:
      put32(c, 0, 1);
      put32(c, 1, 0);
      break;                                    // TRAYREQ: ok, tray closed
    case 12: put32(c, 0, kCdStatPause); break;  // STATUS
    default: put32(c, 0, 1); break;             // MMODE (34), SPIN_CTRL, etc.: success
  }
}

void CdvdService::ncmd(RpcCall& c) {
  switch (c.fno) {
    case 1:    // READ {lbn, sectors, ee_buf, mode, rd_intr_data, &curReadPos}
    case 3: {  // DVDREAD (same layout)
      const uint32_t lbn = arg32(c, 0), sectors = arg32(c, 1), dest = arg32(c, 2), pattern = (arg32(c, 3) >> 16) & 0xFF;
      const uint32_t intr = arg32(c, 4), pos = arg32(c, 5);
      if (pattern != 0)
        log::warn(kSub, "PLACEHOLDER: CD read with sector pattern {} (only 2048-byte sectors are modelled)", pattern);
      const uint32_t n = read_sectors(lbn, sectors, dest);
      ++stats_.reads;
      if (n != sectors) log::warn(kSub, "CD read past end of disc: lbn {} + {} sectors", lbn, sectors);
      if (intr) ee_.write<uint64_t>(intr, 0);  // size1 = size2 = 0: data was delivered fully aligned
      if (pos) ee_.write<uint32_t>(pos, n * kSectorSize);
      break;
    }
    case 9: {  // STREAM {lbn, nsectors, buf, cmd, mode}
      const uint32_t lbn = arg32(c, 0), n = arg32(c, 1), buf = arg32(c, 2), cmd = arg32(c, 3);
      switch (cmd) {
        case 1:  // START
        case 4:  // SEEK
        case 9:  // SEEKF
          stream_pos_ = lbn;
          put32(c, 0, 1);
          break;
        case 2: {  // READ: count in the low 16 bits, error in the high 16
          const uint32_t got = read_sectors(stream_pos_, n, buf);
          stream_pos_ += got;
          ++stats_.stream_reads;
          put32(c, 0, got & 0xFFFF);
          break;
        }
        case 6: put32(c, 0, 0x10000); break;  // STAT: plenty of data "buffered"
        default: put32(c, 0, 1); break;       // INIT/STOP/PAUSE/RESUME
      }
      break;
    }
    case 14: put32(c, 0, kCdComplete); break;  // DISKREADY
    case 4: put32(c, 0, 1); break;             // GETTOC: success, TOC left zeroed
    default: put32(c, 0, 1); break;            // SEEK/STANDBY/STOP/PAUSE/...: accepted
  }
}

void CdvdService::search(RpcCall& c) {
  // ps2sdk: { u8 file[32]; char name[256]; void* dest; } (292 bytes). SCE libcdvd as used by
  // SLUS-20228 sends 296 bytes with the name at offset 36 (observed on the wire). Reply: s32 found.
  const size_t name_off = c.args.size() >= 296 ? 36 : 32;
  std::string name;
  for (size_t i = name_off; i < name_off + 256 && i < c.args.size() && c.args[i]; ++i)
    name += static_cast<char>(c.args[i]);
  const uint32_t dest = arg32(c, (name_off + 256) / 4);
  ++stats_.searches;
  if (stats_.searched.size() < 64) stats_.searched.push_back(name);
  auto e = disc_.find(name);
  if (!e || e->is_dir) {
    ++stats_.search_misses;
    log::warn(kSub, "sceCdSearchFile: '{}' not found on disc", name);
    put32(c, 0, 0);
    return;
  }
  // sceCdlFILE { u32 lsn; u32 size; char name[16]; u8 date[8]; }
  if (dest) {
    ee_.write<uint32_t>(dest, e->lba);
    ee_.write<uint32_t>(dest + 4, static_cast<uint32_t>(e->size));
    std::string base = e->name + ";1";
    for (uint32_t i = 0; i < 16; ++i)
      ee_.write<uint8_t>(dest + 8 + i, i < base.size() ? static_cast<uint8_t>(base[i]) : 0);
    for (uint32_t i = 0; i < 8; ++i) ee_.write<uint8_t>(dest + 24 + i, 0);
  }
  put32(c, 0, 1);
}

void PadService::register_with(SifHle& sif) {
  sif.register_server(0x80000100, "padman", [this](RpcCall& c) { command(c); });
  sif.register_server(0x80000101, "padman-ext", [this](RpcCall& c) { command(c); });
}

void PadService::command(RpcCall& c) {
  // The reply is the same union buffer the EE sent, with results at word 3 (and padBuf at 5).
  c.result.assign(c.args.begin(), c.args.end());
  if (c.result.size() < c.recv_size) c.result.resize(c.recv_size, 0);
  const uint32_t cmd = arg32(c, 0), port = arg32(c, 1), slot = arg32(c, 2);
  uint32_t result = 1;
  switch (cmd) {
    case 0x10: stat_buf_ = arg32(c, 4); break;  // INIT {cmd, -, -, -, statBuf}
    case 0x01: {                                // OPEN {cmd, port, slot, -, padArea}
      areas_[static_cast<int>(port * 8 + slot)] = arg32(c, 4);
      put32(c, 5, 0x001F0000 + port * 0x100);  // padBuf (IOP address, unused by the EE)
      log::info(kSub, "pad port {} slot {} opened (pad area 0x{:08x})", port, slot, arg32(c, 4));
      break;
    }
    case 0x0A: pressure_ = true; break;  // SET_BTNINFO: enable pressure-sensitive buttons
    case 0x09: result = 0x3FFFF; break;  // GET_BTNMASK
    case 0x0C: result = 2; break;        // GET_PORTMAX
    case 0x0D: result = 1; break;        // GET_SLOTMAX
    case 0x0E: areas_.erase(static_cast<int>(port * 8 + slot)); break;  // CLOSE
    default: break;  // SET_MMODE, SET_ACTDIR/ALIGN, SET_VREF, END, GET_MODVER: accepted
  }
  put32(c, 3, result);
}

void PadService::update(const input::PadState& pad) {
  ++frame_;
  if (stat_buf_) {  // open_slot { u32 frame; u32 openSlots[2]; }
    ee_.write<uint32_t>(stat_buf_, frame_);
    ee_.write<uint32_t>(stat_buf_ + 4, 1);
    ee_.write<uint32_t>(stat_buf_ + 8, 0);
  }
  for (const auto& [key, area] : areas_) {
    const bool port0 = key < 8;                       // only port 0 has a controller attached
    const uint32_t base = area + (frame_ & 1) * 128;  // alternate halves; EE reads the newer frame
    uint8_t d[128] = {};
    // data[32]: DualShock 2 report: ok, mode, buttons (active low), RX, RY, LX, LY, 12 pressures
    d[0] = 0x00;
    d[1] = 0x79;
    const uint16_t btn = port0 ? pad.wire_buttons() : 0xFFFF;
    d[2] = static_cast<uint8_t>(btn);
    d[3] = static_cast<uint8_t>(btn >> 8);
    using input::PadAxis;
    using input::PadButton;
    d[4] = port0 ? pad.axis(PadAxis::RightX) : 0x80;
    d[5] = port0 ? pad.axis(PadAxis::RightY) : 0x80;
    d[6] = port0 ? pad.axis(PadAxis::LeftX) : 0x80;
    d[7] = port0 ? pad.axis(PadAxis::LeftY) : 0x80;
    static constexpr PadButton kPressureOrder[12] = {
        PadButton::Right, PadButton::Left,   PadButton::Up, PadButton::Down, PadButton::Triangle, PadButton::Circle,
        PadButton::Cross, PadButton::Square, PadButton::L1, PadButton::R1,   PadButton::L2,       PadButton::R2};
    for (int i = 0; i < 12; ++i) d[8 + i] = port0 ? pad.pressure[static_cast<size_t>(kPressureOrder[i])] : 0;
    auto w32 = [&](int off, uint32_t v) { std::memcpy(&d[off], &v, 4); };
    auto w16 = [&](int off, uint16_t v) { std::memcpy(&d[off], &v, 2); };
    w16(80, 0x0004);  // modeTable: digital, DualShock
    w16(82, 0x0007);
    w32(88, frame_);         // frame
    w32(92, 0);              // findPadRetries
    w32(96, 20);             // length of the report
    d[100] = 1;              // modeConfig
    d[101] = 0x79;           // modeCurId (DualShock 2, analog)
    d[102] = 3;              // model
    d[103] = 1;              // buttonDataReady
    d[104] = 2;              // nrOfModes
    d[105] = 1;              // modeCurOffs
    d[106] = 2;              // nrOfActuators
    d[112] = port0 ? 6 : 0;  // state: STABLE (port 0) / DISCONN
    d[113] = 0;              // reqState: COMPLETE
    (void)pressure_;
    ee_.load(base, d, sizeof d);
  }
}

void IopHeapService::register_with(SifHle& sif) {
  sif.register_server(0x80000003, "iop-heap", [this](RpcCall& c) {
    switch (c.fno) {
      case 1: {  // alloc(size) -> IOP address or 0
        const uint32_t size = (arg32(c, 0) + 63) & ~63u;
        if (next_ + size > SifHle::kHeapEnd) {
          log::warn(kSub, "IOP heap exhausted ({} bytes requested)", size);
          put32(c, 0, 0);
          break;
        }
        put32(c, 0, next_);
        next_ += size;
        break;
      }
      default: put32(c, 0, 0); break;  // free (2) and others: success
    }
  });
}

void LoadfileService::register_with(SifHle& sif) {
  sif.register_server(0x80000006, "loadfile", [this](RpcCall& c) {
    switch (c.fno) {
      case 0:    // MOD_LOAD {arg_len|result, modres, path[252], args[252]} -> {result, modres}
      case 4: {  // MG_MOD_LOAD
        std::string path;
        for (size_t i = 8; i < 8 + 252 && i < c.args.size() && c.args[i]; ++i) path += static_cast<char>(c.args[i]);
        modules_.push_back(path);
        log::info(kSub, "LoadModule '{}' acknowledged (HLE IOP provides its services)", path);
        put32(c, 0, static_cast<uint32_t>(next_id_++));
        put32(c, 1, 0);  // modres: RESIDENT_END
        break;
      }
      case 0xFF:  // GET_VERSION: 4 ASCII digits of the IOP library release.
        // SCE's EE loader client memcmp()s this against the release it was built for and
        // refuses to load modules on mismatch. SLUS-20228's client expects "2240" (read from its
        // comparison at 0x00200238); it is configurable for other titles.
        c.result.assign(version_.begin(), version_.end());
        break;
      default:
        put32(c, 0, 0);
        put32(c, 1, 0);
        break;
    }
  });
}

}  // namespace lantern::iop
