#include <doctest/doctest.h>

#include <cstring>
#include <vector>

#include "analysis/code_analysis.h"
#include "analysis/iop.h"
#include "analysis/ps2_syscalls.h"
#include "analysis/vif.h"
#include "ee/r5900.h"

using namespace lantern;
using namespace lantern::analysis;

namespace {
// Minimal encoders for synthetic test programs.
constexpr uint32_t R(unsigned rs, unsigned rt, unsigned rd, unsigned sa, unsigned f) {
  return rs << 21 | rt << 16 | rd << 11 | sa << 6 | f;
}
constexpr uint32_t I(unsigned op, unsigned rs, unsigned rt, int imm) {
  return op << 26 | rs << 21 | rt << 16 | (static_cast<uint32_t>(imm) & 0xFFFF);
}
constexpr uint32_t kNop = 0;
constexpr uint32_t kJrRa = 0x03e00008;
uint32_t jal(uint32_t target) { return 3u << 26 | ((target >> 2) & 0x03FFFFFF); }
uint32_t addiu(unsigned rt, unsigned rs, int imm) { return I(9, rs, rt, imm); }
uint32_t lui(unsigned rt, unsigned imm) { return I(15, 0, rt, static_cast<int>(imm)); }
uint32_t lw(unsigned rt, int off, unsigned base) { return I(35, base, rt, off); }
uint32_t sltiu(unsigned rt, unsigned rs, int imm) { return I(11, rs, rt, imm); }
uint32_t beq(unsigned rs, unsigned rt, int off) { return I(4, rs, rt, off); }
uint32_t addu(unsigned rd, unsigned rs, unsigned rt) { return R(rs, rt, rd, 0, 33); }
uint32_t sll(unsigned rd, unsigned rt, unsigned sa) { return R(0, rt, rd, sa, 0); }
uint32_t jr(unsigned rs) { return R(rs, 0, 0, 0, 8); }
constexpr uint32_t kSyscall = 12;
constexpr unsigned SP = 29, RA = 31, V0 = 2, V1 = 3, A0 = 4, AT = 1, T0 = 8, T1 = 9;

struct Prog {
  uint32_t base = 0x100000;
  std::vector<uint32_t> w;
  uint32_t here() const { return base + static_cast<uint32_t>(w.size() * 4); }
  void put(std::initializer_list<uint32_t> words) { w.insert(w.end(), words); }
  CodeImage image() const { return CodeImage{base, w}; }
};
}  // namespace

TEST_CASE("syscall names follow ps2sdk numbering") {
  CHECK(ee_syscall_name(0x3c) == "SetupThread");
  CHECK(ee_syscall_name(0x77) == "SifSetDma");
  CHECK(ee_syscall_name(-0x43) == "iSignalSema");
  CHECK(ee_syscall_name(0x999).empty());
}

TEST_CASE("function discovery: calls, branches, jump tables, pointers, syscalls") {
  Prog p;
  // f_entry @0x100000: calls f_switch and f_stub; takes address of f_ptr; returns.
  const uint32_t f_entry = p.here();
  p.put({addiu(SP, SP, -16), 0 /*jal f_switch*/, kNop, 0 /*jal f_stub*/, kNop, lui(T0, 0x10),
         0 /*addiu t0,t0,lo(f_ptr)*/, kJrRa, addiu(SP, SP, 16)});
  // f_switch: bounds-checked jump table with 3 cases.
  const uint32_t f_switch = p.here();
  p.put({sltiu(AT, A0, 3), beq(AT, 0, 9), sll(T0, A0, 2), lui(T1, 0x10), addu(T1, T1, T0), 0 /*lw t1,lo(table)(t1)*/,
         jr(T1), kNop});
  const uint32_t case0 = p.here();
  p.put({addiu(V0, 0, 1), kJrRa, kNop});
  const uint32_t case1 = p.here();
  p.put({addiu(V0, 0, 2), kJrRa, kNop});
  const uint32_t case2 = p.here();
  p.put({addiu(V0, 0, 3), kJrRa, kNop});
  p.put({kJrRa, kNop});  // default target of the beq
  // f_stub: kernel syscall stub.
  const uint32_t f_stub = p.here();
  p.put({addiu(V1, 0, 0x3c), kSyscall, kJrRa, kNop});
  // f_ptr: only referenced by address constant.
  const uint32_t f_ptr = p.here();
  p.put({addiu(SP, SP, -32), kJrRa, addiu(SP, SP, 32)});
  // Jump table data.
  const uint32_t table = p.here();
  p.put({case0, case1, case2});
  // Unreferenced function only discoverable by prologue scan.
  const uint32_t f_orphan = p.here();
  p.put({addiu(SP, SP, -8), kJrRa, addiu(SP, SP, 8)});

  auto at = [&](uint32_t addr) -> uint32_t& { return p.w[(addr - p.base) / 4]; };
  at(f_entry + 4) = jal(f_switch);
  at(f_entry + 12) = jal(f_stub);
  at(f_entry + 24) = addiu(T0, T0, static_cast<int>(f_ptr & 0xFFFF));
  at(f_switch + 20) = lw(T1, static_cast<int>(table & 0xFFFF), T1);

  auto img = p.image();
  auto ca = analyze_code(img, {{f_entry}, true, true});
  std::vector<uint32_t> starts;
  for (const auto& f : ca.functions) starts.push_back(f.start);
  CHECK(starts == std::vector<uint32_t>{f_entry, f_switch, f_stub, f_ptr, f_orphan});

  REQUIRE(ca.jump_tables.size() == 1);
  CHECK(ca.jump_tables[0].table == table);
  CHECK(ca.jump_tables[0].targets == std::vector<uint32_t>{case0, case1, case2});
  CHECK(ca.unresolved_jr.empty());

  REQUIRE(ca.syscalls.size() == 1);
  CHECK(ca.syscalls[0].number == 0x3c);
  CHECK(ca.call_counts.at(f_switch) == 1);

  const Function* sw = ca.function_at(case1);
  REQUIRE(sw);
  CHECK(sw->start == f_switch);  // case bodies belong to the switch function
  CHECK(ca.functions[3].from_pointer);
  CHECK(ca.invalid_reached == 0);
  // The table words are data: unreached.
  bool table_unreached = false;
  for (auto [s, e] : ca.unreached) table_unreached |= (s <= table && table < e);
  CHECK(table_unreached);
}

TEST_CASE("tail calls via j end the caller") {
  Prog p;
  const uint32_t a = p.here();
  p.put({addiu(SP, SP, -8), 0, kNop});  // j b (patched)
  const uint32_t b = p.here();
  p.put({addiu(V0, 0, 7), kJrRa, kNop});
  p.w[1] = 2u << 26 | ((b >> 2) & 0x03FFFFFF);
  // b is a known function start because something calls it.
  const uint32_t c = p.here();
  p.put({jal(b), kNop, kJrRa, kNop});
  auto ca = analyze_code(p.image(), {{a, c}, false, false});
  REQUIRE(ca.functions.size() == 3);
  CHECK(ca.function_at(a)->end <= b);
}

TEST_CASE("JAL sweep distinguishes in-image and external targets") {
  Prog p;
  p.put({jal(0x100010), kNop, jal(0x200000), kNop, addiu(SP, SP, -16), kJrRa, kNop});
  auto s = sweep_jal_targets(p.image());
  CHECK(s.total == 2);
  CHECK(s.inside == 1);
  CHECK(s.inside_prologue == 1);
  CHECK(s.outside == 1);
  CHECK(s.outside_targets == std::vector<uint32_t>{0x200000});
}

TEST_CASE("VIF MPG scan") {
  std::vector<uint8_t> img(64, 0);
  auto put = [&](size_t off, uint32_t v) { std::memcpy(&img[off], &v, 4); };
  put(4, 0x4A020010);       // MPG 2 pairs to VU addr 0x10; payload at 8 (aligned)
  put(8 + 4, 0x000002FF);   // pair 0 upper: no E bit
  put(16 + 4, 0x400002FF);  // pair 1 upper: E bit
  put(40, 0x4A010000);      // misaligned payload (44) -> rejected
  auto blocks = scan_vif_mpg(img);
  REQUIRE(blocks.size() == 1);
  CHECK(blocks[0].offset == 4);
  CHECK(blocks[0].vu_addr == 0x10);
  CHECK(blocks[0].count == 2);
  CHECK(blocks[0].e_bits == 1);
}

TEST_CASE("ROMDIR parsing") {
  std::vector<uint8_t> img(256, 0);
  auto entry = [&](size_t off, const char* name, uint32_t size) {
    std::memcpy(&img[off], name, std::strlen(name));
    std::memcpy(&img[off + 12], &size, 4);
  };
  entry(0, "RESET", 0);
  entry(16, "ROMDIR", 64);
  entry(32, "CDVDMAN", 100);
  std::string err;
  auto dir = parse_romdir(img, &err);
  REQUIRE_MESSAGE(dir, err);
  REQUIRE(dir->size() == 3);
  CHECK((*dir)[2].name == "CDVDMAN");
  CHECK((*dir)[2].size == 100);
  CHECK((*dir)[2].offset == 64);
  CHECK_FALSE(parse_romdir(std::vector<uint8_t>(64, 1), &err));
}

TEST_CASE("IRX module info and import tables") {
  // ELF with one PT_LOAD (code containing an import table) and an .iopmod section.
  std::vector<uint8_t> b(0x200, 0);
  auto p16 = [&](size_t o, uint16_t v) { std::memcpy(&b[o], &v, 2); };
  auto p32 = [&](size_t o, uint32_t v) { std::memcpy(&b[o], &v, 4); };
  std::memcpy(b.data(),
              "\x7f"
              "ELF",
              4);
  b[4] = 1;
  b[5] = 1;
  p16(16, 0xFF80);
  p16(18, 8);
  p32(28, 52);
  p32(32, 0x180);
  p16(42, 32);
  p16(44, 1);
  p16(46, 40);
  p16(48, 2);
  p32(52, 1);      // PT_LOAD
  p32(56, 0x100);  // offset
  p32(68, 0x40);   // filesz
  p32(72, 0x40);
  // import table at 0x100: magic, 0, version 0x0101, "thbase\0\0", stubs (jr ra; li zero,N)
  p32(0x100, 0x41E00000);
  p32(0x108, 0x0101);
  std::memcpy(&b[0x10C], "thbase", 6);
  p32(0x114, 0x03E00008);
  p32(0x118, 0x24000004);
  p32(0x11C, 0x03E00008);
  p32(0x120, 0x24000014);
  // .iopmod data at 0x140
  p32(0x140 + 12, 1234);  // text size
  p16(0x140 + 24, 0x0102);
  std::memcpy(&b[0x140 + 26], "demo_mod", 8);
  // section 1: .iopmod (type 0x70000080)
  p32(0x180 + 40 + 4, 0x70000080);
  p32(0x180 + 40 + 16, 0x140);
  p32(0x180 + 40 + 20, 0x30);
  std::string err;
  auto elf = ee::ElfImage::parse(b, &err);
  REQUIRE_MESSAGE(elf, err);
  auto info = parse_irx(*elf, &err);
  REQUIRE_MESSAGE(info, err);
  CHECK(info->name == "demo_mod");
  CHECK(info->version == 0x0102);
  CHECK(info->text_size == 1234);
  REQUIRE(info->imports.size() == 1);
  CHECK(info->imports[0].name == "thbase");
  CHECK(info->imports[0].functions == std::vector<uint16_t>{4, 20});
}
