#pragma once
// Tiny R5900 encoder for synthetic test programs (no game code).

#include <cstdint>
#include <vector>

namespace lantern::test::mips {

enum Reg : unsigned {
  zero,
  at,
  v0,
  v1,
  a0,
  a1,
  a2,
  a3,
  t0,
  t1,
  t2,
  t3,
  t4,
  t5,
  t6,
  t7,
  s0,
  s1,
  s2,
  s3,
  s4,
  s5,
  s6,
  s7,
  t8,
  t9,
  k0,
  k1,
  gp,
  sp,
  fp,
  ra
};

constexpr uint32_t R(unsigned rs, unsigned rt, unsigned rd, unsigned sa, unsigned f) {
  return rs << 21 | rt << 16 | rd << 11 | sa << 6 | f;
}
constexpr uint32_t I(unsigned op, unsigned rs, unsigned rt, int imm) {
  return op << 26 | rs << 21 | rt << 16 | (static_cast<uint32_t>(imm) & 0xFFFF);
}
constexpr uint32_t nop = 0;
constexpr uint32_t sll(unsigned d, unsigned t, unsigned s) { return R(0, t, d, s, 0); }
constexpr uint32_t addu(unsigned d, unsigned s, unsigned t) { return R(s, t, d, 0, 33); }
constexpr uint32_t daddu(unsigned d, unsigned s, unsigned t) { return R(s, t, d, 0, 45); }
constexpr uint32_t subu(unsigned d, unsigned s, unsigned t) { return R(s, t, d, 0, 35); }
constexpr uint32_t or_(unsigned d, unsigned s, unsigned t) { return R(s, t, d, 0, 37); }
constexpr uint32_t slt(unsigned d, unsigned s, unsigned t) { return R(s, t, d, 0, 42); }
constexpr uint32_t mult(unsigned d, unsigned s, unsigned t) { return R(s, t, d, 0, 24); }
constexpr uint32_t div(unsigned s, unsigned t) { return R(s, t, 0, 0, 26); }
constexpr uint32_t divu(unsigned s, unsigned t) { return R(s, t, 0, 0, 27); }
constexpr uint32_t mfhi(unsigned d) { return R(0, 0, d, 0, 16); }
constexpr uint32_t mflo(unsigned d) { return R(0, 0, d, 0, 18); }
constexpr uint32_t jr(unsigned s) { return R(s, 0, 0, 0, 8); }
constexpr uint32_t jalr(unsigned d, unsigned s) { return R(s, 0, d, 0, 9); }
constexpr uint32_t syscall = 12;
constexpr uint32_t addiu(unsigned t, unsigned s, int imm) { return I(9, s, t, imm); }
constexpr uint32_t daddiu(unsigned t, unsigned s, int imm) { return I(25, s, t, imm); }
constexpr uint32_t ori(unsigned t, unsigned s, unsigned imm) { return I(13, s, t, static_cast<int>(imm)); }
constexpr uint32_t lui(unsigned t, unsigned imm) { return I(15, 0, t, static_cast<int>(imm)); }
constexpr uint32_t slti(unsigned t, unsigned s, int imm) { return I(10, s, t, imm); }
constexpr uint32_t lw(unsigned t, int off, unsigned b) { return I(35, b, t, off); }
constexpr uint32_t sw(unsigned t, int off, unsigned b) { return I(43, b, t, off); }
constexpr uint32_t lwl(unsigned t, int off, unsigned b) { return I(34, b, t, off); }
constexpr uint32_t lwr(unsigned t, int off, unsigned b) { return I(38, b, t, off); }
constexpr uint32_t swl(unsigned t, int off, unsigned b) { return I(42, b, t, off); }
constexpr uint32_t swr(unsigned t, int off, unsigned b) { return I(46, b, t, off); }
constexpr uint32_t ldl(unsigned t, int off, unsigned b) { return I(26, b, t, off); }
constexpr uint32_t ldr(unsigned t, int off, unsigned b) { return I(27, b, t, off); }
constexpr uint32_t sdl(unsigned t, int off, unsigned b) { return I(44, b, t, off); }
constexpr uint32_t sdr(unsigned t, int off, unsigned b) { return I(45, b, t, off); }
constexpr uint32_t ld(unsigned t, int off, unsigned b) { return I(55, b, t, off); }
constexpr uint32_t sd(unsigned t, int off, unsigned b) { return I(63, b, t, off); }
constexpr uint32_t lq(unsigned t, int off, unsigned b) { return I(30, b, t, off); }
constexpr uint32_t sq(unsigned t, int off, unsigned b) { return I(31, b, t, off); }
constexpr uint32_t beq(unsigned s, unsigned t, int off) { return I(4, s, t, off); }
constexpr uint32_t bne(unsigned s, unsigned t, int off) { return I(5, s, t, off); }
constexpr uint32_t beql(unsigned s, unsigned t, int off) { return I(20, s, t, off); }
constexpr uint32_t bnel(unsigned s, unsigned t, int off) { return I(21, s, t, off); }
constexpr uint32_t blez(unsigned s, int off) { return I(6, s, 0, off); }
constexpr uint32_t bgtz(unsigned s, int off) { return I(7, s, 0, off); }
constexpr uint32_t bltz(unsigned s, int off) { return I(1, s, 0, off); }
constexpr uint32_t bgez(unsigned s, int off) { return I(1, s, 1, off); }
constexpr uint32_t bgezal(unsigned s, int off) { return I(1, s, 17, off); }
inline uint32_t j(uint32_t target) { return 2u << 26 | ((target >> 2) & 0x03FFFFFF); }
inline uint32_t jal(uint32_t target) { return 3u << 26 | ((target >> 2) & 0x03FFFFFF); }
// MMI
constexpr uint32_t paddw(unsigned d, unsigned s, unsigned t) { return 28u << 26 | R(s, t, d, 0, 8); }
constexpr uint32_t pextlw(unsigned d, unsigned s, unsigned t) { return 28u << 26 | R(s, t, d, 18, 8); }
constexpr uint32_t qfsrv(unsigned d, unsigned s, unsigned t) { return 28u << 26 | R(s, t, d, 27, 40); }
constexpr uint32_t mtsab(unsigned s, int imm) { return I(1, s, 24, imm); }
// COP1
constexpr uint32_t mtc1(unsigned t, unsigned f) { return 17u << 26 | 4u << 21 | t << 16 | f << 11; }
constexpr uint32_t mfc1(unsigned t, unsigned f) { return 17u << 26 | 0u << 21 | t << 16 | f << 11; }
constexpr uint32_t fop(unsigned funct, unsigned fd, unsigned fs, unsigned ft) {
  return 17u << 26 | 16u << 21 | ft << 16 | fs << 11 | fd << 6 | funct;
}
constexpr uint32_t add_s(unsigned d, unsigned s, unsigned t) { return fop(0, d, s, t); }
constexpr uint32_t mul_s(unsigned d, unsigned s, unsigned t) { return fop(2, d, s, t); }
constexpr uint32_t div_s(unsigned d, unsigned s, unsigned t) { return fop(3, d, s, t); }
constexpr uint32_t cvt_w_s(unsigned d, unsigned s) { return fop(36, d, s, 0); }
constexpr uint32_t c_lt_s(unsigned s, unsigned t) { return fop(52, 0, s, t); }
constexpr uint32_t bc1t(int off) { return 17u << 26 | 8u << 21 | 1u << 16 | (static_cast<uint32_t>(off) & 0xFFFF); }

}  // namespace lantern::test::mips
