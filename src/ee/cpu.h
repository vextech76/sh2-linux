#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace lantern::ee {

// 128-bit R5900 register (GPRs, HI/LO pairs). Lane 0 is the "normal" 64-bit MIPS register.
union alignas(16) Reg128 {
  uint64_t u64[2];
  int64_t s64[2];
  uint32_t u32[4];
  int32_t s32[4];
  uint16_t u16[8];
  int16_t s16[8];
  uint8_t u8[16];
  int8_t s8[16];
};
static_assert(sizeof(Reg128) == 16);

struct Cpu;
class Memory;

// Host services invoked by guest code (HLE kernel). Unset hooks trap.
struct Hooks {
  virtual ~Hooks() = default;
  virtual void syscall(Cpu& c, Memory& m, uint32_t pc) = 0;
  virtual void breakpoint(Cpu& c, Memory& m, uint32_t pc, uint32_t code) = 0;
};

// Guest CPU state shared by the interpreter and recompiled code.
struct Cpu {
  Hooks* hooks = nullptr;
  Reg128 gpr[32]{};
  Reg128 hi{}, lo{};  // u64[0] = HI/LO, u64[1] = HI1/LO1 (MULT1/DIV1 pipeline)
  uint32_t sa = 0;    // shift amount register (QFSRV), in bytes*8 form as written by MTSA
  uint32_t pc = 0;

  uint32_t fpr[32]{};  // single-precision values as raw bits
  uint32_t fcr31 = 0;  // condition bit 23
  uint32_t acc = 0;    // FPU accumulator (raw float bits)

  uint32_t cop0[32]{};

  // VU0 as seen from the EE in macro mode (COP2).
  struct Vu0 {
    Reg128 vf[32]{};    // float vectors as raw bits; vf0 = (0, 0, 0, 1.0f) and is read-only
    uint16_t vi[16]{};  // integer registers; vi0 = 0 and is read-only
    uint32_t acc[4]{};  // accumulator (raw float bits)
    uint32_t q = 0, p = 0, i = 0, r = 0x3F800000;
    uint32_t mac = 0, status = 0, clip = 0;
    uint8_t mem[4096]{};  // VU0 data memory (VLQI/VSQI/VILWR/VISWR)
    Vu0() { vf[0].u32[3] = 0x3F800000; }
  } vu0;

  uint64_t& r(unsigned i) { return gpr[i].u64[0]; }
  uint64_t r(unsigned i) const { return gpr[i].u64[0]; }
};

// Raised for guest conditions LANTERN does not (yet) handle. Never swallowed silently: the
// runtime reports the guest PC and reason.
struct GuestTrap : std::runtime_error {
  uint32_t pc;
  GuestTrap(uint32_t guest_pc, const std::string& what) : std::runtime_error(what), pc(guest_pc) {}
};

}  // namespace lantern::ee
