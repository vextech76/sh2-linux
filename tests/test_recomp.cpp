#include <doctest/doctest.h>

#include <cstring>
#include <format>
#include <memory>

#include "ee/interpreter.h"
#include "recomp/runtime.h"
#include "recomp_programs.h"

namespace selftest {
using RegisterFn = void (*)(lantern::recomp::Runtime&);
extern const RegisterFn kRegister[];
extern const size_t kProgramCount;
}  // namespace selftest

using namespace lantern;
using namespace lantern::test;

namespace {
constexpr uint32_t kSentinel = 0x00FF0000;

struct Machine {
  ee::Memory mem;
  ee::Cpu cpu;
};

void init(Machine& m, const TestProgram& p, uint64_t seed) {
  detail::Rng r{seed * 7919 + 3};
  for (size_t i = 0; i < p.code.size(); ++i) m.mem.write<uint32_t>(p.base + static_cast<uint32_t>(i * 4), p.code[i]);
  for (uint32_t i = 0; i < kDataSize; i += 4) m.mem.write<uint32_t>(kDataBase + i, r.next());
  for (unsigned g = 1; g < 32; ++g)
    for (int lane = 0; lane < 4; ++lane) m.cpu.gpr[g].u32[lane] = r.next();
  for (auto& f : m.cpu.fpr) f = r.next();
  for (int lane = 0; lane < 4; ++lane) {
    m.cpu.hi.u32[lane] = r.next();
    m.cpu.lo.u32[lane] = r.next();
  }
  m.cpu.fcr31 = r.next() & (1u << 23);
  m.cpu.acc = r.next();
  m.cpu.r(29) = kStackTop;  // sp
  m.cpu.r(31) = kSentinel;  // ra
  m.cpu.r(23) = kDataBase;  // s7: data window base
  m.cpu.pc = p.entry;
}

std::string diff(const Machine& a, const Machine& b) {
  std::string d;
  for (unsigned g = 0; g < 32; ++g)
    if (std::memcmp(&a.cpu.gpr[g], &b.cpu.gpr[g], 16) != 0)
      d += std::format(" gpr{}: {:016x}{:016x} vs {:016x}{:016x}", g, a.cpu.gpr[g].u64[1], a.cpu.gpr[g].u64[0],
                       b.cpu.gpr[g].u64[1], b.cpu.gpr[g].u64[0]);
  if (std::memcmp(&a.cpu.hi, &b.cpu.hi, 16) || std::memcmp(&a.cpu.lo, &b.cpu.lo, 16)) d += " hi/lo";
  if (a.cpu.sa != b.cpu.sa) d += " sa";
  if (std::memcmp(a.cpu.fpr, b.cpu.fpr, sizeof a.cpu.fpr)) d += " fpr";
  if (a.cpu.fcr31 != b.cpu.fcr31 || a.cpu.acc != b.cpu.acc) d += " fcr31/acc";
  auto* ma = const_cast<ee::Memory&>(a.mem).host(kDataBase, kDataSize);
  auto* mb = const_cast<ee::Memory&>(b.mem).host(kDataBase, kDataSize);
  if (std::memcmp(ma, mb, kDataSize)) d += " data-memory";
  auto* sa = const_cast<ee::Memory&>(a.mem).host(kStackTop - 4096, 4096);
  auto* sb = const_cast<ee::Memory&>(b.mem).host(kStackTop - 4096, 4096);
  if (std::memcmp(sa, sb, 4096)) d += " stack-memory";
  return d;
}
}  // namespace

TEST_CASE("recompiled code matches the reference interpreter on every test program") {
  const auto programs = test_programs();
  REQUIRE(programs.size() == selftest::kProgramCount);
  for (size_t i = 0; i < programs.size(); ++i) {
    const auto& p = programs[i];
    INFO(p.name);
    auto ref = std::make_unique<Machine>();
    auto rec = std::make_unique<Machine>();
    init(*ref, p, i);
    init(*rec, p, i);

    ee::Interpreter interp;
    interp.run(ref->cpu, ref->mem, kSentinel, 10'000'000);

    recomp::Runtime rt;
    rt.set_interpreter_fallback(false);  // everything must be recompiled
    selftest::kRegister[i](rt);
    const uint32_t returned = rt.call(rec->cpu, rec->mem, p.entry);
    CHECK(returned == kSentinel);
    rec->cpu.pc = kSentinel;
    CHECK_MESSAGE(diff(*ref, *rec).empty(), diff(*ref, *rec));
  }
}

TEST_CASE("known results of the hand-written programs") {
  const auto programs = test_programs();
  auto m = std::make_unique<Machine>();
  init(*m, programs[0], 0);
  recomp::Runtime rt;
  rt.set_interpreter_fallback(false);
  selftest::kRegister[0](rt);
  rt.call(m->cpu, m->mem, programs[0].entry);
  CHECK(m->cpu.r(2) == 144);  // fib(12)

  auto s = std::make_unique<Machine>();
  init(*s, programs[1], 1);
  const uint64_t v0_before = s->cpu.r(2);
  recomp::Runtime rt2;
  rt2.set_interpreter_fallback(false);
  selftest::kRegister[1](rt2);
  rt2.call(s->cpu, s->mem, programs[1].entry);
  CHECK(static_cast<uint32_t>(s->cpu.r(2)) == static_cast<uint32_t>(v0_before + 600));
}

TEST_CASE("missing functions trap without fallback and run interpreted with it") {
  const auto programs = test_programs();
  auto m = std::make_unique<Machine>();
  init(*m, programs[0], 0);
  recomp::Runtime none;
  none.set_interpreter_fallback(false);
  CHECK_THROWS_AS(none.call(m->cpu, m->mem, programs[0].entry), ee::GuestTrap);

  auto f = std::make_unique<Machine>();
  init(*f, programs[0], 0);
  recomp::Runtime fallback;  // nothing registered: fully interpreted
  CHECK(fallback.call(f->cpu, f->mem, programs[0].entry) == kSentinel);
  CHECK(f->cpu.r(2) == 144);
  CHECK(fallback.stats().interpreted_calls == 1);
}
