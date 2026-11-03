#include "analysis/code_analysis.h"

#include <algorithm>
#include <deque>
#include <set>

#include "ee/elf.h"
#include "ee/r5900.h"

namespace lantern::analysis {
using ee::decode;
using ee::Insn;
using ee::Op;

namespace {
constexpr unsigned kRa = 31, kV1 = 3;
constexpr uint32_t kMaxTableEntries = 2048;
constexpr int kBacktrack = 16;

// Find the instruction that last wrote `reg` before `pc`, scanning straight-line code backwards.
std::optional<Insn> find_def(const CodeImage& img, uint32_t pc, unsigned reg, int limit = kBacktrack) {
  for (int i = 1; i <= limit; ++i) {
    uint32_t a = pc - 4u * static_cast<uint32_t>(i);
    if (!img.contains(a)) return std::nullopt;
    Insn in = decode(img.at(a), a);
    if (!in.valid()) return std::nullopt;
    if (ee::written_gpr(in) == static_cast<int>(reg)) return in;
    // Stop at a preceding control transfer other than the one whose delay slot we may be in.
    if (in.has(ee::kEnd) && i > 1) return std::nullopt;
  }
  return std::nullopt;
}

// Resolve a constant built by lui (+ addiu/ori) into `reg` before `pc`.
std::optional<uint32_t> constant_in(const CodeImage& img, uint32_t pc, unsigned reg, int depth = 0) {
  if (depth > 3) return std::nullopt;
  auto def = find_def(img, pc, reg);
  if (!def) return std::nullopt;
  if (def->op == Op::LUI) return static_cast<uint32_t>(def->uimm) << 16;
  if ((def->op == Op::ADDIU || def->op == Op::DADDIU) && def->rs != 0) {
    auto base = constant_in(img, def->pc, def->rs, depth + 1);
    if (base) return *base + static_cast<uint32_t>(static_cast<int32_t>(def->simm));
  }
  if (def->op == Op::ORI && def->rs != 0) {
    auto base = constant_in(img, def->pc, def->rs, depth + 1);
    if (base) return *base | def->uimm;
  }
  return std::nullopt;
}

// GCC MIPS switch lowering: [sltiu at,idx,N] ... sll t,idx,2 ; lui b,%hi(T) ; addu b,b,t ;
// lw r,%lo(T)(b) ; jr r. Returns the table base and (if found) its bound.
std::optional<JumpTable> resolve_jump_table(const CodeImage& img, const Insn& jr) {
  auto load = find_def(img, jr.pc, jr.rs);
  if (!load || load->op != Op::LW) return std::nullopt;
  uint32_t table = 0;
  if (auto direct = constant_in(img, load->pc, load->rs)) {
    table = *direct + static_cast<uint32_t>(static_cast<int32_t>(load->simm));
  } else {
    auto add = find_def(img, load->pc, load->rs);
    if (!add || (add->op != Op::ADDU && add->op != Op::DADDU)) return std::nullopt;
    std::optional<uint32_t> base = constant_in(img, add->pc, add->rs);
    if (!base) base = constant_in(img, add->pc, add->rt);
    if (!base) return std::nullopt;
    table = *base + static_cast<uint32_t>(static_cast<int32_t>(load->simm));
  }
  std::optional<uint32_t> bound;
  for (int i = 1; i <= 2 * kBacktrack; ++i) {
    uint32_t a = jr.pc - 4u * static_cast<uint32_t>(i);
    if (!img.contains(a)) break;
    Insn in = decode(img.at(a), a);
    if (in.op == Op::SLTIU && in.uimm > 0) {
      bound = in.uimm;
      break;
    }
  }
  JumpTable jt{jr.pc, table, {}};
  uint32_t limit = bound.value_or(kMaxTableEntries);
  for (uint32_t i = 0; i < limit; ++i) {
    uint32_t slot = table + 4 * i;
    if (!img.contains(slot)) break;
    uint32_t target = img.at(slot);
    if (!img.contains(target) || !decode(img.at(target), target).valid()) break;
    jt.targets.push_back(target);
  }
  if (jt.targets.empty()) return std::nullopt;
  return jt;
}

struct Walker {
  const CodeImage& img;
  std::vector<int32_t> owner;  // function index per word, -1 = unreached
  std::set<uint32_t> starts;
  CodeAnalysis& out;

  int32_t idx(uint32_t a) const { return static_cast<int32_t>((a - img.base) / 4); }

  // Walk one function from `start`; returns newly discovered call/pointer targets.
  void walk(uint32_t start, int32_t fn_id, Function& fn, std::vector<uint32_t>& found_calls,
            std::vector<uint32_t>& found_ptrs) {
    std::deque<uint32_t> work{start};
    std::set<uint32_t> seen;
    uint32_t hi = start;
    while (!work.empty()) {
      uint32_t pc = work.front();
      work.pop_front();
      while (img.contains(pc) && !seen.contains(pc)) {
        if (pc != start && starts.contains(pc)) break;  // fell into another function
        seen.insert(pc);
        Insn in = decode(img.at(pc), pc);
        if (!in.valid()) {
          ++out.invalid_reached;
          break;
        }
        mark(pc, fn_id, hi, fn);
        if (in.op == Op::SYSCALL) record_syscall(in);
        if (in.op == Op::ADDIU || in.op == Op::ORI || in.op == Op::DADDIU) {
          if (auto c = constant_in(img, pc + 4, in.rt); c && img.contains(*c)) found_ptrs.push_back(*c);
        }
        if (!in.has(ee::kDelay)) {
          if (in.op == Op::ERET) break;
          pc += 4;
          continue;
        }
        // Delay slot belongs to this function.
        if (img.contains(pc + 4) && !seen.contains(pc + 4)) {
          seen.insert(pc + 4);
          Insn ds = decode(img.at(pc + 4), pc + 4);
          if (ds.valid()) mark(pc + 4, fn_id, hi, fn);
          if (ds.op == Op::SYSCALL) record_syscall(ds);
        }
        const uint32_t next = pc + 8;
        if (in.has(ee::kLink)) {  // calls return
          if (in.op == Op::JALR) {
            ++fn.indirect_calls;
          } else {
            ++fn.calls;
            if (img.contains(in.target)) {
              found_calls.push_back(in.target);
              ++out.call_counts[in.target];
            } else {
              out.external_calls.push_back(in.target);
            }
          }
          pc = next;  // BxxAL / BAL targets are calls too, never intra-function edges
          continue;
        }
        if (in.op == Op::JR) {
          if (in.rs != kRa) {
            if (auto jt = resolve_jump_table(img, in)) {
              ++fn.jump_tables;
              for (uint32_t t : jt->targets) work.push_back(t);
              out.jump_tables.push_back(std::move(*jt));
            } else {
              ++fn.unresolved_jumps;
              out.unresolved_jr.push_back(pc);
            }
          }
          break;
        }
        if (in.op == Op::J) {
          if (starts.contains(in.target) || !img.contains(in.target)) break;  // tail call
          pc = in.target;
          continue;
        }
        if (in.has(ee::kBranch)) {
          if (img.contains(in.target)) work.push_back(in.target);
          if (in.always_taken()) break;
          pc = next;
          continue;
        }
        pc = next;
      }
    }
    fn.end = std::max(fn.end, hi + 4);
  }

  void mark(uint32_t pc, int32_t fn_id, uint32_t& hi, Function& fn) {
    if (owner[static_cast<size_t>(idx(pc))] < 0) {
      owner[static_cast<size_t>(idx(pc))] = fn_id;
      ++out.reached_words;
    }
    ++fn.instructions;
    hi = std::max(hi, pc);
  }

  void record_syscall(const Insn& in) {
    SyscallSite site{in.pc, std::nullopt};
    if (auto def = find_def(img, in.pc, kV1, 4)) {
      if ((def->op == Op::ADDIU || def->op == Op::DADDIU) && def->rs == 0) site.number = def->simm;
      if (def->op == Op::ORI && def->rs == 0) site.number = def->uimm;
    }
    out.syscalls.push_back(site);
  }
};

}  // namespace

CodeImage CodeImage::from_bytes(uint32_t base, std::span<const uint8_t> bytes) {
  CodeImage img;
  img.base = base;
  img.words.reserve(bytes.size() / 4);
  for (size_t i = 0; i + 4 <= bytes.size(); i += 4) img.words.push_back(ee::load_le32(&bytes[i]));
  return img;
}

bool looks_like_prologue(const CodeImage& img, uint32_t addr) {
  for (int i = 0; i < 3; ++i) {
    uint32_t a = addr + 4u * static_cast<uint32_t>(i);
    if (!img.contains(a)) return false;
    Insn in = decode(img.at(a), a);
    if (!in.valid()) return false;
    if ((in.op == Op::ADDIU || in.op == Op::DADDIU) && in.rt == 29 && in.rs == 29 && in.simm < 0) return true;
  }
  return false;
}

const Function* CodeAnalysis::function_at(uint32_t addr) const {
  auto it = std::upper_bound(functions.begin(), functions.end(), addr,
                             [](uint32_t a, const Function& f) { return a < f.start; });
  if (it == functions.begin()) return nullptr;
  --it;
  return addr < it->end ? &*it : nullptr;
}

CodeAnalysis analyze_code(const CodeImage& img, const AnalyzeOptions& opt) {
  CodeAnalysis out;
  out.total_words = static_cast<uint32_t>(img.words.size());
  Walker w{img, std::vector<int32_t>(img.words.size(), -1), {}, out};
  std::deque<std::pair<uint32_t, bool>> pending;  // (start, from_pointer)
  for (uint32_t s : opt.seeds)
    if (img.contains(s)) pending.emplace_back(s, false);

  std::vector<Function> fns;
  auto drain = [&] {
    while (!pending.empty()) {
      auto [start, from_ptr] = pending.front();
      pending.pop_front();
      if (w.starts.contains(start)) continue;
      // A pointer into the middle of an already-walked function is data or a label, not a
      // new function; direct calls always create a function boundary.
      if (from_ptr && (w.owner[static_cast<size_t>(w.idx(start))] >= 0 || !looks_like_prologue(img, start))) continue;
      w.starts.insert(start);
      Function fn{start, start, 0, 0, 0, 0, 0, from_ptr};
      std::vector<uint32_t> calls, ptrs;
      w.walk(start, static_cast<int32_t>(fns.size()), fn, calls, ptrs);
      fns.push_back(fn);
      for (uint32_t c : calls) pending.emplace_back(c, false);
      if (opt.pointer_seeds)
        for (uint32_t p : ptrs) pending.emplace_back(p, true);
    }
  };
  drain();

  if (opt.prologue_seeds) {
    // Functions only reachable through tables of pointers in data: scan unreached gaps.
    for (uint32_t a = img.base; a < img.end(); a += 4) {
      if (w.owner[static_cast<size_t>(w.idx(a))] >= 0 || w.starts.contains(a)) continue;
      if (looks_like_prologue(img, a)) {
        pending.emplace_back(a, true);
        drain();
      }
    }
  }

  std::sort(fns.begin(), fns.end(), [](const Function& a, const Function& b) { return a.start < b.start; });
  // Clamp extents so functions never overlap their successor.
  for (size_t i = 0; i + 1 < fns.size(); ++i) fns[i].end = std::min(fns[i].end, fns[i + 1].start);
  out.functions = std::move(fns);

  std::sort(out.external_calls.begin(), out.external_calls.end());
  out.external_calls.erase(std::unique(out.external_calls.begin(), out.external_calls.end()), out.external_calls.end());
  for (uint32_t a = img.base; a < img.end();) {
    if (w.owner[static_cast<size_t>(w.idx(a))] >= 0) {
      a += 4;
      continue;
    }
    uint32_t s = a;
    while (a < img.end() && w.owner[static_cast<size_t>(w.idx(a))] < 0) a += 4;
    out.unreached.emplace_back(s, a);
  }
  return out;
}

double valid_word_ratio(const CodeImage& img) {
  if (img.words.empty()) return 0.0;
  size_t ok = 0;
  for (size_t i = 0; i < img.words.size(); ++i)
    if (decode(img.words[i], img.base + static_cast<uint32_t>(i * 4)).valid()) ++ok;
  return static_cast<double>(ok) / static_cast<double>(img.words.size());
}

JalSweep sweep_jal_targets(const CodeImage& img) {
  JalSweep s;
  for (size_t i = 0; i < img.words.size(); ++i) {
    uint32_t pc = img.base + static_cast<uint32_t>(i * 4);
    Insn in = decode(img.words[i], pc);
    if (in.op != Op::JAL) continue;
    ++s.total;
    if (img.contains(in.target)) {
      ++s.inside;
      if (looks_like_prologue(img, in.target)) ++s.inside_prologue;
    } else {
      ++s.outside;
      s.outside_targets.push_back(in.target);
    }
  }
  return s;
}

}  // namespace lantern::analysis
