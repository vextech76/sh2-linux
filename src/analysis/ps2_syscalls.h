#pragma once

#include <string_view>

namespace lantern::analysis {

// EE kernel syscall name by number (as loaded into $v1 before `syscall`). Negative numbers are
// the interrupt-context variants. Numbering per ps2sdk ee/kernel/include/syscallnr.h.
// Returns an empty view for unknown numbers.
std::string_view ee_syscall_name(int number);

}  // namespace lantern::analysis
