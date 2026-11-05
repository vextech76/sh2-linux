#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "iop/hle.h"
#include "iop/iop_cpu.h"
#include "iop/iop_interp.h"
#include "iop/iop_kernel.h"
#include "iop/iop_threads.h"
#include "iop/irx.h"
#include "iop/spu2.h"

namespace lantern::iop {

// A small, reusable IOP: shared RAM, interpreter, scheduler, kernel HLE and the game's modules.
// The development probe and the boot runtime both use it; the boot runtime currently loads the
// sound set (LIBSD, SD_CD, SDSTR, SOUNDCD) and forwards SIF RPC calls to its registered servers.
class IopRuntime {
 public:
  IopRuntime();

  IopMemory& mem() { return mem_; }
  const IopMemory& mem() const { return mem_; }
  IopKernel& kernel() { return kernel_; }
  const IopKernel& kernel() const { return kernel_; }
  Spu2& spu2() { return spu2_; }
  void mix_audio(int16_t* out, uint32_t frames) { spu2_.mix(out, frames); }

  bool add_module(const std::filesystem::path& path, std::string* error);
  bool load_sound_modules(const std::filesystem::path& extracted, std::string* error);
  // Run the module initialisers; their threads stay in the scheduler.
  bool start_modules(std::string* error);

  SchedulerStats run(uint64_t steps);
  uint64_t now_us() const { return sched_.now_us(); }
  const RpcServerInfo* find_rpc(uint32_t sid) const { return kernel_.find_rpc(sid); }
  const std::vector<LoadedModule>& modules() const { return modules_; }

  // Call a guest RPC server function synchronously. The arguments must already be in IOP RAM at
  // `server.buff`; `recv_size` bytes are copied from the pointer returned in v0.
  bool call_rpc(const RpcServerInfo& server, uint32_t fno, uint32_t send_size, uint32_t recv_size,
                std::vector<uint8_t>* result, std::string* error);

 private:
  struct Image {
    std::string label;
    std::vector<uint8_t> bytes;
    uint32_t base = 0;
    uint32_t size = 0;
  };
  struct ExportRef {
    IrxLibraryId library;
    std::vector<uint32_t> functions;
  };

  bool load_all(std::string* error);
  void mmio_read(uint32_t addr, void* out, unsigned size);
  void mmio_write(uint32_t addr, const void* in, unsigned size);
  void start_spu_dma(int core);

  IopMemory mem_;
  Interpreter interp_;
  IopHle hle_;
  IopScheduler sched_;
  IopKernel kernel_;
  Spu2 spu2_;
  struct SpuDma {
    uint32_t madr = 0, bcr = 0, chcr = 0;
  };
  SpuDma spu_dma_[2]{};
  std::vector<Image> images_;
  std::vector<LoadedModule> modules_;
  std::vector<ExportRef> exports_;
  uint32_t next_base_ = 0x00010000;
  uint32_t callback_stack_top_ = 0;
  uint32_t rpc_stack_base_ = 0;
};

}  // namespace lantern::iop
