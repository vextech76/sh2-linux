#include "iop/hle.h"

#include <format>
#include <stdexcept>

namespace lantern::iop {

uint32_t IopHle::add(IrxLibraryId library, uint16_t function, Handler handler) {
  for (auto& e : entries_) {
    if (e.library == library && e.function == function) {
      e.handler = std::move(handler);
      return e.slot;
    }
  }
  if (entries_.size() >= kMaxHandlers) throw std::runtime_error("IopHle: trampoline page exhausted");
  const uint32_t slot = kTrampolineBase + static_cast<uint32_t>(entries_.size()) * kSlotSize;
  const uint32_t index = static_cast<uint32_t>(entries_.size());
  // syscall with its 20-bit code field carrying the handler index; jr ra and its delay slot.
  mem_.write<uint32_t>(slot, 0x0000000Cu | (index << 6));
  mem_.write<uint32_t>(slot + 4, 0x03E00008u);
  mem_.write<uint32_t>(slot + 8, 0u);
  entries_.push_back({std::move(library), function, slot, std::move(handler)});
  return slot;
}

std::optional<uint32_t> IopHle::resolve(const IrxLibraryId& library, uint16_t function) const {
  for (const auto& e : entries_)
    if (e.library == library && e.function == function) return e.slot;
  return std::nullopt;
}

bool IopHle::dispatch(IopCpu& cpu, uint32_t pc) {
  if (pc < kTrampolineBase || pc >= kTrampolineBase + entries_.size() * kSlotSize) return false;
  const uint32_t index = (pc - kTrampolineBase) / kSlotSize;
  if ((pc - kTrampolineBase) % kSlotSize != 0 || index >= entries_.size()) return false;
  const uint32_t expected = 0x0000000Cu | (index << 6);
  if (mem_.read<uint32_t>(pc) != expected)
    throw std::runtime_error(std::format("IopHle: corrupt trampoline slot at 0x{:08x}", pc));
  // Copy before invoking: a handler may register another one, reallocating the vector.
  Handler handler = entries_[index].handler;
  handler(cpu, mem_);
  return true;
}

}  // namespace lantern::iop
