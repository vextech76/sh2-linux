#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "analysis/code_analysis.h"

namespace lantern::recomp {

struct EmitOptions {
  std::string ns = "lantern_generated";  // C++ namespace of the generated module
  std::string register_fn = "register_module";
  int overlay = -1;  // >= 0: register into this overlay id
  size_t functions_per_file = 256;
};

struct EmitStats {
  uint64_t functions = 0;
  uint64_t instructions = 0;
  uint64_t unimplemented = 0;                   // sites emitted as runtime traps
  std::map<std::string, uint64_t> missing_ops;  // mnemonic -> count
  uint64_t indirect_jumps = 0;                  // JR via runtime (unresolved)
  uint64_t jump_tables = 0;
};

struct EmittedModule {
  std::string header;                // declarations + register function prototype
  std::vector<std::string> sources;  // translation units
  EmitStats stats;
};

// Translate the analysed functions of `image` to C++ calling the shared semantics.
EmittedModule emit_module(const analysis::CodeImage& image, const analysis::CodeAnalysis& analysis,
                          const EmitOptions& options);

std::string function_symbol(uint32_t addr, int overlay);

}  // namespace lantern::recomp
