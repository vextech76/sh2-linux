#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "iop/iop_cpu.h"
#include "iop/irx.h"

namespace lantern::iop {

// Host implementations of IOP kernel/library functions. Each handler gets the trampoline address:
//
//   caller: jal import_stub        (ra = return address)
//   stub:   j handler_slot         (patched by the IRX loader)
//   slot:   syscall                (the interpreter calls dispatch())
//           jr ra                  (returns to the caller)
//
// Handlers read arguments from a0..a3 and write the result to v0 (and v1 for 64-bit results).
class IopHle {
 public:
  using Handler = std::function<void(IopCpu&, IopMemory&)>;

  static constexpr uint32_t kTrampolineBase = 0x001F0000;
  static constexpr uint32_t kTrampolineSize = 0x1000;
  // syscall; jr ra; nop — the nop is the delay slot, so a slot cannot be 8 bytes.
  static constexpr uint32_t kSlotSize = 12;
  static constexpr uint32_t kMaxHandlers = kTrampolineSize / kSlotSize;

  explicit IopHle(IopMemory& mem) : mem_(mem) {}

  // Register (or replace) a handler and return its trampoline address.
  uint32_t add(IrxLibraryId library, uint16_t function, Handler handler);
  std::optional<uint32_t> resolve(const IrxLibraryId& library, uint16_t function) const;

  // Syscall hook body. Returns false when `pc` is not a trampoline slot, so the caller can report a
  // real (unimplemented) IOP syscall.
  bool dispatch(IopCpu& cpu, uint32_t pc);

  size_t size() const { return entries_.size(); }

 private:
  struct Entry {
    IrxLibraryId library;
    uint16_t function = 0;
    uint32_t slot = 0;
    Handler handler;
  };

  IopMemory& mem_;
  std::vector<Entry> entries_;
};

}  // namespace lantern::iop
