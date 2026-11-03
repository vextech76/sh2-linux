#include "recomp/emitter.h"

#include <algorithm>
#include <format>
#include <set>
#include <unordered_map>

#include "ee/interpreter.h"
#include "ee/r5900.h"
#include "ee/semantics_ops.h"

namespace lantern::recomp {
using analysis::CodeAnalysis;
using analysis::CodeImage;
using analysis::Function;
using ee::Insn;
using ee::Op;

std::string function_symbol(uint32_t addr, int overlay) {
  return overlay < 0 ? std::format("f_{:08x}", addr) : std::format("ov{}_{:08x}", overlay, addr);
}

namespace {

const char* sem_name(Op op) {
  switch (op) {
#define LANTERN_NAME(name) \
  case Op::name: return #name;
    LANTERN_PLAIN_OPS(LANTERN_NAME)
#undef LANTERN_NAME
    default: return nullptr;
  }
}

const char* cond_name(Op op, bool* likely) {
  switch (op) {
#define LANTERN_COND(name, cond, is_likely, link) \
  case Op::name: *likely = is_likely; return #cond;
    LANTERN_BRANCH_OPS(LANTERN_COND)
#undef LANTERN_COND
    default: return nullptr;
  }
}

struct FunctionEmitter {
  const CodeImage& img;
  const Function& fn;
  const std::set<uint32_t>& known;  // all function starts in this module
  const std::unordered_map<uint32_t, const analysis::JumpTable*>& tables;
  int overlay;
  EmitStats& stats;
  std::string out;
  std::set<uint32_t> labels;

  bool inside(uint32_t a) const { return a >= fn.start && a < fn.end; }

  void line(std::string_view s) {
    out += "  ";
    out += s;
    out += '\n';
  }

  // Plain instruction (or a runtime trap if it has no semantics yet).
  void plain(const Insn& in) {
    ++stats.instructions;
    if (in.word == 0) return;  // nop (sll zero,zero,0)
    if (in.op == Op::VU0_SPECIAL1 && ((in.word & 63) == 56 || (in.word & 63) == 57)) {
      // VU0 micro-program call: has a semantic entry point but traps until VU micro mode exists.
      ++stats.unimplemented;
      ++stats.missing_ops["vcallms/vcallmsr (VU0 micro mode, traps)"];
    }
    if (const char* n = in.valid() ? sem_name(in.op) : nullptr) {
      line(std::format("sem::{}(c, m, 0x{:08x}u, 0x{:08x}u);", n, in.word, in.pc));
      return;
    }
    ++stats.unimplemented;
    ++stats.missing_ops[in.valid()
                            ? std::string(in.op == Op::VU0_SPECIAL1 || in.op == Op::VU0_SPECIAL2 ? ee::vu0_mnemonic(in)
                                                                                                 : ee::mnemonic(in.op))
                            : "(invalid)"];
    line(std::format("Runtime::unimplemented(0x{:08x}u, 0x{:08x}u);", in.word, in.pc));
  }

  // Transfer control to `target` (outside-or-inside the function) as a jump.
  std::string goto_or_tail(uint32_t target) {
    if (inside(target)) {
      labels.insert(target);
      return std::format("goto L_{:08x};", target);
    }
    if (known.contains(target)) return std::format("return {}(c, m, rt);", function_symbol(target, overlay));
    return std::format("return rt.jump(c, m, 0x{:08x}u);", target);
  }

  void call_known_or_dispatch(uint32_t pc, const std::string& target_expr, std::optional<uint32_t> target) {
    const uint32_t ret = pc + 8;
    std::string callee = target && known.contains(*target)
                             ? std::format("{}(c, m, rt)", function_symbol(*target, overlay))
                             : std::format("rt.call(c, m, {})", target_expr);
    line(std::format(
        "if (uint32_t r_ = {}; r_ != 0x{:08x}u) [[unlikely]] Runtime::bad_return(0x{:08x}u, 0x{:08x}u, r_);", callee,
        ret, pc, ret));
  }

  void emit() {
    // Label targets first: every intra-function branch/jump target and post-call continuation.
    for (uint32_t pc = fn.start; pc < fn.end; pc += 4) {
      Insn in = ee::decode(img.at(pc), pc);
      if (!in.has(ee::kDelay)) continue;
      labels.insert(pc + 8);
      if ((in.has(ee::kBranch) || in.op == Op::J) && inside(in.target)) labels.insert(in.target);
      if (auto it = tables.find(pc); it != tables.end())
        for (uint32_t t : it->second->targets)
          if (inside(t)) labels.insert(t);
    }

    std::string body;
    out.clear();
    for (uint32_t pc = fn.start; pc < fn.end;) {
      if (labels.contains(pc)) out += std::format("L_{:08x}:\n", pc);
      Insn in = ee::decode(img.at(pc), pc);
      if (!in.has(ee::kDelay)) {
        if (in.op == Op::ERET || !in.valid()) {
          ++stats.instructions;
          ++stats.unimplemented;
          ++stats.missing_ops[in.valid() ? "eret" : "(invalid)"];
          line(std::format("Runtime::unimplemented(0x{:08x}u, 0x{:08x}u);", in.word, in.pc));
        } else {
          plain(in);
        }
        pc += 4;
        continue;
      }
      ++stats.instructions;
      const Insn slot = ee::decode(pc + 4 < img.end() ? img.at(pc + 4) : 0, pc + 4);
      if (slot.has(ee::kDelay)) {
        line(std::format("throw ee::GuestTrap(0x{:08x}u, \"control transfer in a delay slot\");", pc + 4));
        pc += 8;
        continue;
      }
      line("{");
      if (in.has(ee::kBranch)) {
        bool likely = false;
        const char* cond = cond_name(in.op, &likely);
        if (!cond) {
          ++stats.unimplemented;
          ++stats.missing_ops[std::string(ee::mnemonic(in.op))];
          line(std::format("Runtime::unimplemented(0x{:08x}u, 0x{:08x}u);", in.word, in.pc));
          line("}");
          pc += 8;
          continue;
        }
        line(std::format("const bool t_ = sem::{}(c, 0x{:08x}u);", cond, in.word));
        if (in.has(ee::kLink)) line(std::format("c.r(31) = 0x{:08x}u;", pc + 8));
        if (likely) {
          line("if (t_) {");
          plain(slot);
        } else {
          plain(slot);
          line("if (t_) {");
        }
        if (in.has(ee::kLink)) {
          call_known_or_dispatch(pc, std::format("0x{:08x}u", in.target), in.target);
          line(std::format("goto L_{:08x};", pc + 8));
        } else {
          line(goto_or_tail(in.target));
        }
        line("}");
        line(std::format("goto L_{:08x};", pc + 8));
      } else if (in.op == Op::J) {
        plain(slot);
        line(goto_or_tail(in.target));
      } else if (in.op == Op::JAL) {
        line(std::format("c.r(31) = 0x{:08x}u;", pc + 8));
        plain(slot);
        call_known_or_dispatch(pc, std::format("0x{:08x}u", in.target), in.target);
        line(std::format("goto L_{:08x};", pc + 8));
      } else if (in.op == Op::JALR) {
        line(std::format("const uint32_t t_ = static_cast<uint32_t>(c.r({}));", in.rs));
        if (in.rd != 31) {
          // The runtime's call convention returns through $ra; other link registers are not
          // modelled yet, so trap instead of approximating.
          ++stats.unimplemented;
          ++stats.missing_ops["jalr(non-ra link)"];
          line(std::format("Runtime::unimplemented(0x{:08x}u, 0x{:08x}u);", in.word, in.pc));
        } else {
          line(std::format("c.r(31) = 0x{:08x}u;", pc + 8));
          plain(slot);
          call_known_or_dispatch(pc, "t_", std::nullopt);
          line(std::format("goto L_{:08x};", pc + 8));
        }
      } else if (in.op == Op::JR) {
        line(std::format("const uint32_t t_ = static_cast<uint32_t>(c.r({}));", in.rs));
        plain(slot);
        if (in.rs == 31) {
          line("return t_;");
        } else if (auto it = tables.find(pc); it != tables.end()) {
          ++stats.jump_tables;
          line("switch (t_) {");
          std::set<uint32_t> seen;
          for (uint32_t t : it->second->targets)
            if (seen.insert(t).second) line(std::format("  case 0x{:08x}u: {}", t, goto_or_tail(t)));
          line(std::format("  default: Runtime::bad_jump_table(0x{:08x}u, t_);", pc));
          line("}");
        } else {
          ++stats.indirect_jumps;
          line("return rt.jump(c, m, t_);");
        }
      } else {
        ++stats.unimplemented;
        ++stats.missing_ops[std::string(ee::mnemonic(in.op))];
        line(std::format("Runtime::unimplemented(0x{:08x}u, 0x{:08x}u);", in.word, in.pc));
      }
      line("}");
      // A branch elsewhere may target this delay slot directly: run the slot, then continue.
      if (labels.contains(pc + 4)) {
        out += std::format("L_{:08x}:\n", pc + 4);
        plain(slot);
        line(std::format("goto L_{:08x};", pc + 8));
      }
      pc += 8;
    }
    // Falling off the end continues into whatever follows.
    if (labels.contains(fn.end)) out += std::format("L_{:08x}:\n", fn.end);
    line(goto_or_tail(fn.end));
  }
};

}  // namespace

EmittedModule emit_module(const CodeImage& img, const CodeAnalysis& ca, const EmitOptions& opt) {
  EmittedModule mod;
  std::set<uint32_t> known;
  for (const auto& f : ca.functions) known.insert(f.start);
  std::unordered_map<uint32_t, const analysis::JumpTable*> tables;
  for (const auto& jt : ca.jump_tables) tables[jt.jr_pc] = &jt;

  mod.header = std::format(
      "// Generated by LANTERN. Derived from the user's own disc: never distribute or commit.\n"
      "#pragma once\n#include \"recomp/runtime.h\"\nnamespace {} {{\n"
      "using lantern::ee::Cpu; using lantern::ee::Memory; using lantern::recomp::Runtime;\n",
      opt.ns);
  for (const auto& f : ca.functions)
    mod.header += std::format("uint32_t {}(Cpu& c, Memory& m, Runtime& rt);\n", function_symbol(f.start, opt.overlay));
  mod.header += std::format("void {}(Runtime& rt);\n}}  // namespace {}\n", opt.register_fn, opt.ns);

  const std::string prologue = std::format(
      "// Generated by LANTERN. Derived from the user's own disc: never distribute or commit.\n"
      "#include \"module.h\"\n#include \"ee/semantics.h\"\n"
      "namespace {} {{\nnamespace sem = lantern::ee::sem;\nnamespace ee = lantern::ee;\n",
      opt.ns);
  std::string cur;
  size_t in_file = 0;
  for (const auto& f : ca.functions) {
    if (in_file == 0) cur = prologue;
    FunctionEmitter fe{img, f, known, tables, opt.overlay, mod.stats, {}, {}};
    fe.emit();
    ++mod.stats.functions;
    cur += std::format("uint32_t {}(Cpu& c, Memory& m, Runtime& rt) {{\n  (void)rt;\n{}}}\n\n",
                       function_symbol(f.start, opt.overlay), fe.out);
    if (++in_file == opt.functions_per_file) {
      mod.sources.push_back(cur + "}\n");
      in_file = 0;
    }
  }
  if (in_file) mod.sources.push_back(cur + "}\n");

  std::string reg =
      std::format("// Generated by LANTERN.\n#include \"module.h\"\nnamespace {} {{\nvoid {}(Runtime& rt) {{\n", opt.ns,
                  opt.register_fn);
  for (const auto& f : ca.functions)
    reg += opt.overlay < 0 ? std::format("  rt.add(0x{:08x}u, &{});\n", f.start, function_symbol(f.start, opt.overlay))
                           : std::format("  rt.add_overlay({}, 0x{:08x}u, &{});\n", opt.overlay, f.start,
                                         function_symbol(f.start, opt.overlay));
  reg += "}\n}\n";
  mod.sources.push_back(reg);
  return mod;
}

}  // namespace lantern::recomp
