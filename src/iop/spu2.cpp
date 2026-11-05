#include "iop/spu2.h"

#include <algorithm>
#include <cstring>

namespace lantern::iop {
namespace {
constexpr int16_t kAdpcmCoef[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};

int16_t clamp16(int32_t v) { return static_cast<int16_t>(std::clamp(v, -32768, 32767)); }
}  // namespace

Spu2::Spu2() : ram_(kRamSize, 0) {
  for (auto& core : cores_) {
    core.master_l = 0x7FFF;
    core.master_r = 0x7FFF;
  }
}

uint16_t Spu2::reg(const Core& core, uint32_t off) const {
  if (off < 0x180) {
    const int v = static_cast<int>(off / 0x10);
    const uint32_t p = off % 0x10;
    const Voice& voice = core.voices[static_cast<size_t>(v)];
    switch (p) {
      case 0x0: return voice.voll;
      case 0x2: return voice.volr;
      case 0x4: return voice.pitch;
      case 0x6: return voice.adsr1;
      case 0x8: return voice.adsr2;
      case 0xA: return voice.envx;
      case 0xC: return voice.volxl;
      case 0xE: return voice.volxr;
      default: return 0;
    }
  }
  if (off >= 0x1C0 && off < 0x1C0 + kVoices * 0x0C) {
    const int v = static_cast<int>((off - 0x1C0) / 0x0C);
    const uint32_t p = (off - 0x1C0) % 0x0C;
    const Voice& voice = core.voices[static_cast<size_t>(v)];
    const uint32_t value = p < 4 ? voice.ssa : (p < 8 ? voice.lsax : voice.nax);
    return static_cast<uint16_t>(value >> ((p & 3) * 8));
  }
  const size_t ci = (&core == &cores_[1]) ? 1 : 0;
  switch (off) {
    case 0x19C: return static_cast<uint16_t>(core.irqa);
    case 0x19E: return static_cast<uint16_t>(core.irqa >> 16);
    case 0x1A8: return static_cast<uint16_t>(tsa_[ci]);
    case 0x1AA: return static_cast<uint16_t>(tsa_[ci] >> 16);
    case 0x340: return core.endx;
    case 0x342: return 0;
    case 0x344: return core.status;
    default: return core.regs[off & (kCoreStride - 1)];
  }
}

void Spu2::set_reg(Core& core, uint32_t off, uint16_t value) {
  if (off < 0x180) {
    const int v = static_cast<int>(off / 0x10);
    const uint32_t p = off % 0x10;
    Voice& voice = core.voices[static_cast<size_t>(v)];
    switch (p) {
      case 0x0: voice.voll = value; break;
      case 0x2: voice.volr = value; break;
      case 0x4: voice.pitch = value; break;
      case 0x6: voice.adsr1 = value; break;
      case 0x8: voice.adsr2 = value; break;
      case 0xC: voice.volxl = value; break;
      case 0xE: voice.volxr = value; break;
      default: break;
    }
    core.regs[off] = value;
    return;
  }
  if (off >= 0x1C0 && off < 0x1C0 + kVoices * 0x0C) {
    const int v = static_cast<int>((off - 0x1C0) / 0x0C);
    const uint32_t p = (off - 0x1C0) % 0x0C;
    Voice& voice = core.voices[static_cast<size_t>(v)];
    uint32_t& field = p < 4 ? voice.ssa : (p < 8 ? voice.lsax : voice.nax);
    const unsigned shift = (p & 3) * 8;
    field = (field & ~(0xFFFFu << shift)) | (static_cast<uint32_t>(value) << shift);
    core.regs[off] = value;
    return;
  }
  const size_t ci = (&core == &cores_[1]) ? 1 : 0;
  switch (off) {
    case 0x19C: core.irqa = static_cast<uint32_t>((core.irqa & 0xFFFF0000u) | static_cast<uint32_t>(value)); break;
    case 0x19E: core.irqa = static_cast<uint32_t>((core.irqa & 0xFFFFu) | (static_cast<uint32_t>(value) << 16)); break;
    case 0x1A0: key_on(core, value); break;
    case 0x1A2: key_on(core, static_cast<uint32_t>(value) << 16); break;
    case 0x1A4: key_off(core, value); break;
    case 0x1A6: key_off(core, static_cast<uint32_t>(value) << 16); break;
    case 0x1A8: tsa_[ci] = (tsa_[ci] & 0xFFFF0000u) | value; break;
    case 0x1AA: tsa_[ci] = (tsa_[ci] & 0xFFFFu) | (static_cast<uint32_t>(value) << 16); break;
    case 0x1AC: {
      const uint32_t addr = (tsa_[ci] * 8) % kRamSize;
      ram_[addr] = static_cast<uint8_t>(value);
      ram_[(addr + 1) % kRamSize] = static_cast<uint8_t>(value >> 8);
      tsa_[ci] = (tsa_[ci] + 1) & 0x3FFFF;
      break;
    }
    case 0x340: core.endx &= static_cast<uint16_t>(~value); break;
    case 0x342: core.endx &= static_cast<uint16_t>(~(value << 8)); break;
    case 0x344: core.status = value; break;
    default: core.regs[off & (kCoreStride - 1)] = value; break;
  }
}

void Spu2::key_on(Core& core, uint32_t bits) {
  for (int v = 0; v < kVoices; ++v) {
    if (!(bits & (1u << v))) continue;
    Voice& voice = core.voices[static_cast<size_t>(v)];
    voice.active = true;
    voice.nax = voice.ssa;
    voice.phase = 0;
    voice.block_pos = 28;
    voice.prev1 = voice.prev2 = 0;
    voice.loop_end = voice.loop_repeat = false;
    core.endx &= static_cast<uint16_t>(~(1u << v));
  }
}

void Spu2::voice_end(Core& core, int index) {
  Voice& voice = core.voices[static_cast<size_t>(index)];
  voice.active = false;
  core.endx |= static_cast<uint16_t>(1u << index);
  core.irq_pending = true;
  if (irq_) irq_(9);  // IOP_IRQ_SPU
}

void Spu2::key_off(Core& core, uint32_t bits) {
  for (int v = 0; v < kVoices; ++v)
    if (bits & (1u << v)) voice_end(core, v);
}

void Spu2::decode_block(Voice& voice) {
  const uint32_t addr = (voice.nax * 8) % kRamSize;
  const uint8_t hdr = ram_[addr];
  const uint8_t flags = ram_[(addr + 1) % kRamSize];
  const unsigned shift = hdr & 0x0F;
  const unsigned filter = (hdr >> 4) & 0x07;
  const int f0 = filter < 5 ? kAdpcmCoef[filter][0] : 0;
  const int f1 = filter < 5 ? kAdpcmCoef[filter][1] : 0;
  for (int i = 0; i < 28; ++i) {
    const uint8_t byte = ram_[(addr + 2 + i / 2) % kRamSize];
    const int nib = (i & 1) ? (byte >> 4) : (byte & 0x0F);
    const int sample = ((nib & 0x08) ? nib - 16 : nib) << 12 >> shift;
    const int predicted = (voice.prev1 * f0 + voice.prev2 * f1 + 32) >> 6;
    const int16_t out = clamp16(sample + predicted);
    voice.prev2 = voice.prev1;
    voice.prev1 = out;
    voice.block[i] = out;
  }
  voice.loop_end = (flags & 0x01) != 0;
  voice.loop_repeat = (flags & 0x02) != 0;
  voice.nax = (voice.nax + 2) & 0x3FFFF;
  voice.block_pos = 0;
}

void Spu2::mix(int16_t* out, uint32_t frames) {
  std::memset(out, 0, size_t{frames} * 2 * sizeof(int16_t));
  for (size_t core_index = 0; core_index < cores_.size(); ++core_index) {
    Core& core = cores_[core_index];
    for (uint32_t f = 0; f < frames; ++f) {
      int32_t left = 0, right = 0;
      for (int vi = 0; vi < kVoices; ++vi) {
        Voice& voice = core.voices[static_cast<size_t>(vi)];
        if (!voice.active) continue;
        if (voice.block_pos >= 28) {
          decode_block(voice);
          if (!voice.active) continue;
        }
        const int16_t sample = voice.block[voice.block_pos];
        left += (static_cast<int32_t>(sample) * static_cast<int16_t>(voice.voll)) >> 15;
        right += (static_cast<int32_t>(sample) * static_cast<int16_t>(voice.volr)) >> 15;

        voice.phase += static_cast<uint32_t>(voice.pitch) * 44100u / kSampleRate;
        while (voice.phase >= 0x1000) {
          voice.phase -= 0x1000;
          ++voice.block_pos;
          if (voice.block_pos < 28) continue;
          if (voice.loop_end) {
            if (voice.loop_repeat) {
              voice.nax = voice.lsax;
              decode_block(voice);
              if (!voice.active) break;
            } else {
              voice_end(core, vi);
              break;
            }
          } else {
            decode_block(voice);
            if (!voice.active) break;
          }
        }
      }
      const int16_t l = clamp16((left * static_cast<int32_t>(master_l_)) >> 15);
      const int16_t r = clamp16((right * static_cast<int32_t>(master_r_)) >> 15);
      out[f * 2] = clamp16(out[f * 2] + l);
      out[f * 2 + 1] = clamp16(out[f * 2 + 1] + r);
    }
  }
}

void Spu2::read(uint32_t addr, void* out, unsigned size) const {
  const uint32_t base = addr & 0x7FF;
  uint8_t* dst = static_cast<uint8_t*>(out);
  if (base >= 0x760) {
    uint16_t value = 0;
    switch (base) {
      case 0x760: value = master_l_; break;
      case 0x762: value = master_r_; break;
      case 0x764: value = effect_l_; break;
      case 0x766: value = effect_r_; break;
      case 0x770: value = master_l_; break;
      case 0x772: value = master_r_; break;
      default: break;
    }
    if (size == 4) {
      const uint32_t v = value | (static_cast<uint32_t>(0) << 16);
      std::memcpy(dst, &v, 4);
    } else if (size == 2) {
      std::memcpy(dst, &value, 2);
    } else {
      dst[0] = (base & 1) ? static_cast<uint8_t>(value >> 8) : static_cast<uint8_t>(value);
    }
    return;
  }
  if (size == 4) {
    const uint32_t low = reg(base < 0x400 ? cores_[0] : cores_[1], base & 0x3FF);
    const uint32_t high = reg(base + 2 < 0x400 ? cores_[0] : cores_[1], (base + 2) & 0x3FF);
    const uint32_t value = low | (high << 16);
    std::memcpy(dst, &value, 4);
  } else if (size == 2) {
    const uint16_t value = reg(base < 0x400 ? cores_[0] : cores_[1], base & 0x3FF);
    std::memcpy(dst, &value, 2);
  } else {
    const uint16_t value = reg(base < 0x400 ? cores_[0] : cores_[1], (base & ~1u) & 0x3FF);
    dst[0] = (base & 1) ? static_cast<uint8_t>(value >> 8) : static_cast<uint8_t>(value);
  }
}

void Spu2::write(uint32_t addr, const void* in, unsigned size) {
  const uint32_t base = addr & 0x7FF;
  const uint8_t* src = static_cast<const uint8_t*>(in);
  if (base >= 0x760) {
    uint16_t value = 0;
    if (size >= 2)
      std::memcpy(&value, src, 2);
    else
      value = src[0];
    switch (base) {
      case 0x760: master_l_ = value; break;
      case 0x762: master_r_ = value; break;
      case 0x764: effect_l_ = value; break;
      case 0x766: effect_r_ = value; break;
      default: break;
    }
    return;
  }
  if (size == 4) {
    uint32_t value = 0;
    std::memcpy(&value, src, 4);
    set_reg(base < 0x400 ? cores_[0] : cores_[1], base & 0x3FF, static_cast<uint16_t>(value));
    set_reg((base + 2) < 0x400 ? cores_[0] : cores_[1], (base + 2) & 0x3FF, static_cast<uint16_t>(value >> 16));
  } else if (size == 2) {
    uint16_t value = 0;
    std::memcpy(&value, src, 2);
    set_reg(base < 0x400 ? cores_[0] : cores_[1], base & 0x3FF, value);
  } else {
    set_reg(base < 0x400 ? cores_[0] : cores_[1], (base & ~1u) & 0x3FF, src[0]);
  }
}

bool Spu2::take_irq(int core) {
  const size_t index = static_cast<size_t>(core % 2);
  const bool pending = cores_[index].irq_pending;
  cores_[index].irq_pending = false;
  return pending;
}

}  // namespace lantern::iop
