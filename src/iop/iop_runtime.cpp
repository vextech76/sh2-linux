#include "iop/iop_runtime.h"

#include <cstdio>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>

#include "iop/r3000.h"

namespace lantern::iop {
namespace {
std::vector<uint8_t> read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

constexpr uint32_t kRpcStepBudget = 2000000;
constexpr uint32_t kInitStepBudget = 1500000;
}  // namespace

IopRuntime::IopRuntime() : hle_(mem_), sched_(mem_, interp_), kernel_(hle_, sched_) {
  // Sound-only modules end well below 0xA0000 (the SIF command buffer) and 0x100000 (the fake
  // service buffers). Keep the IOP heap in the free window between them.
  kernel_.heap.next = 0x00100000;
  kernel_.heap.end = 0x001F0000;
  callback_stack_top_ = kernel_.heap.alloc(0x2000) + 0x2000;

  // The probe logs MMIO; the runtime accepts it until the SPU2/DMA models exist.
  mem_.set_mmio([](uint32_t, void* out, unsigned size) { std::memset(out, 0, size); },
                [](uint32_t, const void*, unsigned) {});
  interp_.set_syscall_hook([this](IopCpu& cpu, IopMemory&, uint32_t pc) {
    if (hle_.dispatch(cpu, pc)) return;
    throw Trap(pc, "IOP syscall (no kernel syscall handler)");
  });
  kernel_.install();
}

bool IopRuntime::add_module(const std::filesystem::path& path, std::string* error) {
  auto bytes = read_file(path);
  if (bytes.empty()) {
    if (error) *error = std::format("cannot read {}", path.string());
    return false;
  }
  auto size = irx_image_size(bytes, error);
  if (!size) return false;
  auto base = place_irx(next_base_, *size);
  if (!base) {
    if (error) *error = std::format("{} does not fit in IOP RAM", path.filename().string());
    return false;
  }
  Image image;
  image.label = path.filename().string();
  image.base = *base;
  image.size = *size;
  image.bytes = std::move(bytes);
  images_.push_back(std::move(image));
  next_base_ = *base + *size;
  return true;
}

bool IopRuntime::load_all(std::string* error) {
  // Pass 1: every module's exports (including those of modules loaded later).
  for (const auto& image : images_) {
    auto mod = load_irx(mem_, image.bytes, {.base = image.base, .resolve = {}}, error);
    if (!mod) return false;
    for (const auto& exp : mod->exports) exports_.push_back({exp.library, exp.functions});
  }

  auto resolve = [this](const IrxLibraryId& library, uint16_t function) -> std::optional<uint32_t> {
    if (auto slot = hle_.resolve(library, function)) return slot;
    for (const auto& exp : exports_) {
      if (exp.library.name != library.name || exp.library.version < library.version) continue;
      if (function < exp.functions.size()) return exp.functions[function];
    }
    return std::nullopt;
  };

  // Pass 2: load with imports linked, keeping the module records for diagnostics.
  for (const auto& image : images_) {
    auto mod = load_irx(mem_, image.bytes, {.base = image.base, .resolve = resolve}, error);
    if (!mod) return false;
    modules_.push_back(std::move(*mod));
  }
  return true;
}

bool IopRuntime::load_sound_modules(const std::filesystem::path& extracted, std::string* error) {
  const std::filesystem::path paths[] = {
      extracted / "IOP" / "MODULES" / "LIBSD.IRX",
      extracted / "IOP" / "SOUND" / "SD_CD.IRX",
      extracted / "IOP" / "SOUND" / "SDSTR.IRX",
      extracted / "IOP" / "SOUND" / "SOUNDCD.IRX",
  };
  for (const auto& path : paths)
    if (!add_module(path, error)) return false;
  return load_all(error);
}

bool IopRuntime::start_modules(std::string* error) {
  for (const auto& module : modules_) {
    if (!module.has_entry()) continue;
    const uint32_t stack_size = 0x4000;
    const uint32_t stack = kernel_.heap.alloc(stack_size);
    if (!stack) {
      if (error) *error = "IOP thread-stack heap exhausted";
      return false;
    }
    const uint32_t id = sched_.create_thread(module.entry, 0, 64, stack, stack_size, module.gp);
    if (IopThread* t = sched_.thread(id)) {
      t->cpu.gpr[5] = 0;
      t->cpu.gpr[6] = module.base;  // loadcore's cur_module_addr
      t->cpu.gpr[7] = 0;
    }
    sched_.start_thread(id, 0);
  }

  const SchedulerStats stats = sched_.run(kInitStepBudget);
  if (stats.trapped) {
    if (error)
      *error = std::format("IOP thread {} trapped at 0x{:05x}: {}", stats.trap_thread, stats.trap_pc, stats.trap);
    return false;
  }
  return true;
}

SchedulerStats IopRuntime::run(uint64_t steps) { return sched_.run(steps); }

bool IopRuntime::call_rpc(const RpcServerInfo& server, uint32_t fno, uint32_t send_size, uint32_t recv_size,
                          std::vector<uint8_t>* result, std::string* error) {
  if (!server.function) {
    if (error) *error = "RPC server has no function";
    return false;
  }
  if (!rpc_stack_base_) rpc_stack_base_ = kernel_.heap.alloc(0x4000);
  if (!rpc_stack_base_) {
    if (error) *error = "IOP RPC stack heap exhausted";
    return false;
  }
  result->assign(recv_size, 0);

  // Run the server as a scheduler thread: it may wait on a semaphore that another IOP thread
  // signals, which a bare CPU call cannot model.
  const uint32_t id = sched_.create_thread(server.function, 0, 1, rpc_stack_base_, 0x4000, server.gp);
  if (IopThread* t = sched_.thread(id)) {
    t->cpu.gpr[4] = fno;
    t->cpu.gpr[5] = server.buff;
    t->cpu.gpr[6] = send_size;
  }
  sched_.start_thread(id, 0);

  std::vector<std::pair<uint32_t, uint32_t>> ring;
  sched_.set_observer([&](const IopThread& th, uint32_t pc, uint32_t word) {
    if (th.id != id) return;
    ring.push_back({pc, word});
    if (ring.size() > 40) ring.erase(ring.begin());
  });

  SchedulerStats stats;
  const bool done = sched_.run_until_done(id, kRpcStepBudget, &stats);
  IopThread* t = sched_.thread(id);
  const uint32_t ptr = t ? t->cpu.gpr[2] : 0;
  if (!done) {
    if (error)
      *error = stats.trapped ? std::format("RPC server trapped at 0x{:05x}: {}", stats.trap_pc, stats.trap)
                             : "RPC server did not finish (blocked or out of steps)";
    static int failures = 0;
    if (failures++ < 3 && t) {
      std::fprintf(stderr, "[iop] RPC fail: state=%d pc=0x%05x wait=%u steps=%llu deadlock=%d trapped=%d\n",
                   static_cast<int>(t->state), t->cpu.pc, t->wait_object, static_cast<unsigned long long>(t->steps),
                   stats.deadlock, stats.trapped);
      for (const auto& [pc, word] : ring) {
        const Insn in = decode(word, pc);
        std::fprintf(stderr, "[iop]   0x%05x %08x %s\n", pc, word, in.valid() ? disassemble(in).c_str() : "(invalid)");
      }
    }
    sched_.set_observer({});
    sched_.remove_thread(id);
    return false;
  }
  sched_.set_observer({});
  if (ptr && uint64_t{ptr} + recv_size <= IopMemory::kRamSize) {
    for (uint32_t i = 0; i < recv_size; ++i) (*result)[i] = mem_.read<uint8_t>(ptr + i);
  }
  sched_.remove_thread(id);
  return true;
}

}  // namespace lantern::iop
