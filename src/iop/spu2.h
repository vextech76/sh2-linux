#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace lantern::iop {

// A functional SPU2 core pair: register file, 48 PS-ADPCM voices, 2 MiB of sample RAM, key
// on/off and a 48 kHz stereo mixer. Reverb, noise, pitch modulation and the SPDIF path are not
// modelled; the registers are kept so the driver's setup reads back consistently.
class Spu2 {
 public:
  static constexpr uint32_t kRamSize = 2u << 20;
  static constexpr uint32_t kCoreStride = 0x400;
  static constexpr uint32_t kRegSize = 0x800;
  static constexpr uint32_t kSampleRate = 48000;
  static constexpr int kVoices = 24;

  using IrqCallback = std::function<void(int irq)>;

  Spu2();

  void set_irq_callback(IrqCallback cb) { irq_ = std::move(cb); }

  // Physical register access, 0x1F900000 base (an 0xBF900000 kseg1 address is masked too).
  void read(uint32_t addr, void* out, unsigned size) const;
  void write(uint32_t addr, const void* in, unsigned size);

  // Mix `frames` stereo samples at 48 kHz. `out` is interleaved left/right, clamped to int16.
  void mix(int16_t* out, uint32_t frames);

  uint8_t* ram() { return ram_.data(); }
  const uint8_t* ram() const { return ram_.data(); }
  uint32_t ram_size() const { return kRamSize; }

  // Transfer address is in 8-byte units, as the SPU2 registers are.
  uint32_t tsa(int core) const { return tsa_[core % 2]; }
  void set_tsa(int core, uint32_t value) { tsa_[core % 2] = value & 0x3FFFF; }
  void advance_tsa(int core, uint32_t bytes) { tsa_[core % 2] = (tsa_[core % 2] + bytes / 8) & 0x3FFFF; }

  uint16_t endx(int core) const { return cores_[core % 2].endx; }
  bool take_irq(int core);

  struct Stats {
    uint64_t key_ons = 0, voice_ends = 0, decoded_blocks = 0, nonzero_frames = 0;
  };
  const Stats& stats() const { return stats_; }

 private:
  struct Voice {
    bool active = false;
    uint32_t ssa = 0, lsax = 0, nax = 0;
    uint16_t pitch = 0x1000, voll = 0, volr = 0, adsr1 = 0, adsr2 = 0;
    uint16_t envx = 0, volxl = 0, volxr = 0;
    int16_t prev1 = 0, prev2 = 0;
    int16_t block[28]{};
    int block_pos = 28;
    uint32_t phase = 0;
    bool loop_end = false, loop_repeat = false;
  };

  struct Core {
    std::array<Voice, kVoices> voices;
    std::array<uint16_t, kCoreStride> regs{};  // raw register image, 16-bit units
    uint16_t endx = 0;
    uint16_t status = 0;
    uint32_t irqa = 0;
    bool irq_pending = false;
    int16_t master_l = 0x7FFF, master_r = 0x7FFF;
  };

  uint16_t reg(const Core& core, uint32_t off) const;
  void set_reg(Core& core, uint32_t off, uint16_t value);
  void key_on(Core& core, uint32_t bits);
  void key_off(Core& core, uint32_t bits);
  void decode_block(Voice& v);
  void voice_end(Core& core, int index);

  std::array<Core, 2> cores_{};
  std::array<uint32_t, 2> tsa_{};
  uint16_t master_l_ = 0x7FFF, master_r_ = 0x7FFF;
  uint16_t effect_l_ = 0, effect_r_ = 0;
  std::vector<uint8_t> ram_;
  IrqCallback irq_;
  Stats stats_;
};

}  // namespace lantern::iop
