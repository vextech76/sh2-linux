#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

#include "gs/gs_memory.h"

namespace lantern::gs {

struct Image {
  uint32_t width = 0, height = 0;
  std::vector<uint32_t> rgba;  // 0xAABBGGRR, row-major
};

struct Vertex {
  int32_t x = 0, y = 0;  // 12.4 fixed point, primitive coordinate space
  uint32_t z = 0;
  uint8_t r = 0, g = 0, b = 0, a = 0;
  float s = 0, t = 0, q = 1;
  uint32_t u = 0, v = 0;  // 10.4 fixed point
  uint8_t fog = 0;
};

// Graphics Synthesizer: general registers (written through GIF), privileged registers (EE MMIO),
// local-memory image transfers and display readout. Drawing kicks are handed to a rasterizer
// callback; without one they are counted as PLACEHOLDER work.
class Gs {
 public:
  using DrawFn = std::function<void(Gs& gs, const Vertex* v, int count, uint64_t prim)>;

  LocalMemory& mem() { return mem_; }
  const LocalMemory& mem() const { return mem_; }
  void set_rasterizer(DrawFn fn) { draw_ = std::move(fn); }

  void write_reg(uint8_t reg, uint64_t value);
  void write_hwreg(uint64_t data);  // IMAGE / HWREG pixel data
  uint64_t reg(uint8_t r) const { return regs_[r]; }
  // CLUT buffer (filled from local memory on TEX0/TEX2 writes with CLD != 0).
  uint32_t clut(uint32_t i) const { return clut_[i & 255]; }
  bool clut_is16() const { return clut_is16_; }

  // Privileged registers at 0x12000000 + offset (offset < 0x2000), 32-bit halves.
  void write_priv32(uint32_t offset, uint32_t value);
  uint32_t read_priv32(uint32_t offset) const;
  uint64_t priv(uint32_t offset) const;

  // Interrupt line for FINISH/SIGNAL (CSR bits not masked by IMR); polled by the kernel.
  bool take_interrupt() {
    bool v = irq_;
    irq_ = false;
    return v;
  }
  void set_imr(uint32_t imr) { imr_ = imr; }
  void on_vblank() { field_ ^= 1; }

  // Compose the currently displayed image (read circuit 2, else 1) from local memory.
  Image display() const;
  // Sample a texture region as an image (debug aid: shows the decoded texture content).
  Image sample_texture(uint32_t tbp, uint32_t tbw, uint32_t psm, uint32_t tw, uint32_t th) const;

  struct Stats {
    uint64_t image_transfers = 0, image_pixels = 0, local_copies = 0;
    std::map<uint32_t, uint64_t> kicks;  // primitive type -> vertex kicks
    uint64_t primitives = 0, finish = 0, signal = 0;
    std::map<uint32_t, uint64_t> unsupported_psm;
  };
  const Stats& stats() const { return stats_; }

 private:
  void vertex_kick(bool draw);
  void load_clut(uint64_t tex0);
  void start_transfer();
  void local_to_local();
  void put_pixel_stream();

  LocalMemory mem_;
  std::array<uint64_t, 256> regs_{};
  std::map<uint32_t, uint64_t> priv_;
  DrawFn draw_;
  Vertex cur_{};
  float q_ = 1.0f;
  Vertex queue_[3]{};
  int queued_ = 0;
  // host->local transfer cursor
  bool xfer_active_ = false;
  uint32_t xfer_x_ = 0, xfer_y_ = 0;
  std::vector<uint8_t> xfer_bytes_;
  uint32_t csr_ = 0, imr_ = 0xFF00;
  bool irq_ = false;
  uint32_t field_ = 0;
  std::array<uint32_t, 256> clut_{};
  bool clut_is16_ = false;
  uint32_t cbp0_ = ~0u, cbp1_ = ~0u;
  Stats stats_;
};

// GIF: parses GIFtag packets from PATH1/2/3 into GS register writes.
class Gif {
 public:
  explicit Gif(Gs& gs) : gs_(gs) {}
  // Feed whole quadwords (16 bytes each). Packets may span calls.
  void process(const uint8_t* qwords, uint32_t count);
  uint64_t packets() const { return packets_; }

 private:
  Gs& gs_;
  uint32_t nloop_ = 0, nreg_ = 0, flg_ = 0, reg_index_ = 0;
  uint64_t regs_ = 0;
  bool eop_ = false, in_packet_ = false;
  uint64_t packets_ = 0;
};

// Quadwords in the GIF packet that starts at `addr` in a circular buffer of `size` bytes,
// counting the GIFtag and data of every tag up to and including the one with EOP set.
uint32_t gif_packet_qwords(const uint8_t* mem, uint32_t size, uint32_t addr);

}  // namespace lantern::gs
