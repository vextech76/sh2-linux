#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lantern::analysis {

// A contiguous run of R5900 words at a fixed load address (main ELF image or one overlay).
struct CodeImage {
  uint32_t base = 0;
  std::vector<uint32_t> words;

  static CodeImage from_bytes(uint32_t base, std::span<const uint8_t> bytes);
  uint32_t end() const { return base + static_cast<uint32_t>(words.size() * 4); }
  bool contains(uint32_t addr) const { return addr >= base && addr < end() && (addr & 3) == 0; }
  uint32_t at(uint32_t addr) const { return words[(addr - base) / 4]; }
};

struct Function {
  uint32_t start = 0;
  uint32_t end = 0;  // exclusive: one past the highest reached instruction
  uint32_t instructions = 0;
  uint32_t calls = 0;             // direct JAL/BAL sites
  uint32_t indirect_calls = 0;    // JALR sites
  uint32_t jump_tables = 0;       // resolved JR-through-table sites
  uint32_t unresolved_jumps = 0;  // JR (not $ra) without a resolved table
  bool from_pointer = false;      // discovered via an address constant, not a direct call
};

struct JumpTable {
  uint32_t jr_pc = 0;
  uint32_t table = 0;
  std::vector<uint32_t> targets;
};

struct SyscallSite {
  uint32_t pc = 0;
  std::optional<int> number;  // value loaded into $v1, if found
};

struct CodeAnalysis {
  std::vector<Function> functions;  // sorted by start
  std::vector<JumpTable> jump_tables;
  std::vector<uint32_t> unresolved_jr;
  std::vector<SyscallSite> syscalls;
  std::map<uint32_t, uint32_t> call_counts;  // direct-call target -> number of call sites
  std::vector<uint32_t> external_calls;      // JAL targets outside the image (unique)
  uint32_t total_words = 0;
  uint32_t reached_words = 0;
  uint32_t invalid_reached = 0;                          // invalid encodings hit while following control flow
  std::vector<std::pair<uint32_t, uint32_t>> unreached;  // [start, end) ranges

  const Function* function_at(uint32_t addr) const;
};

struct AnalyzeOptions {
  std::vector<uint32_t> seeds;  // entry points known a priori (ELF entry, overlay base, ...)
  bool pointer_seeds = true;    // follow lui/addiu address constants that look like functions
  bool prologue_seeds = true;   // scan unreached gaps for `addiu sp,sp,-N` prologues
};

CodeAnalysis analyze_code(const CodeImage& image, const AnalyzeOptions& options);

// Decode statistics over every word (no control flow): share of valid encodings.
double valid_word_ratio(const CodeImage& image);

// Linear sweep for JAL targets regardless of reachability (used to test link addresses).
struct JalSweep {
  uint32_t total = 0;
  uint32_t inside = 0;           // target within the image
  uint32_t inside_prologue = 0;  // ... and the target starts with a stack-frame prologue
  uint32_t outside = 0;
  std::vector<uint32_t> outside_targets;
};
JalSweep sweep_jal_targets(const CodeImage& image);

bool looks_like_prologue(const CodeImage& image, uint32_t addr);

}  // namespace lantern::analysis
