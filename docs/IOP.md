# IOP: loading and running the game's modules

Stage 9 runs the game's own IOP programs on an emulated R3000A instead of reimplementing their
contents. This document records the IRX format as the shipped modules actually use it, the loader
and HLE that are implemented, and exactly what the probe has reached so far. All measurements come
from the user's own extracted disc data; nothing here is game code.

## Tool

```
lantern-iop --extracted ~/.local/share/lantern/extracted/SLUS-20228 --inspect
lantern-iop --extracted ~/.local/share/lantern/extracted/SLUS-20228 --module SOUNDCD --mmio
```

`--inspect` loads every `.IRX` under the extracted directory at a 64 KiB aligned base, applies
relocations, resolves inter-module imports, and prints module identity, imports, exports and
unresolved functions. `--romdir` also loads the IRX modules packed in `IOP/MODULES/IOPRP.IMG`
(SIFCMD, THREADMAN, IOMAN, MODLOAD, CDVDMAN, …); they are ordinary IRX images inside a ROMDIR
table. `--module NAME` runs a module's initialiser (and the thread bodies its
`StartThread` calls make runnable) on the R3000A interpreter. `--mmio` prints the register accesses
that no model answered. The tool is read-only with respect to disc data.

## IRX format as used here

An IRX is a 32-bit little-endian MIPS ELF (`e_type = 0xFF80`) with a `.iopmod` section
(`sh_type = 0x70000080`):

| Offset | Field |
|---|---|
| 0x00 | module-id pointer (a link-time address inside `.data`) |
| 0x04 | entry point, **relative to the module base** |
| 0x08 | gp, relative to the module base |
| 0x0C/0x10/0x14 | text / data / bss sizes |
| 0x18 | version, major in the high byte |
| 0x1A | NUL-terminated module name |

Findings that shaped the loader (`src/iop/irx.cpp`):

- **Every module has an initialiser, including at offset 0.** `loadcore` calls the entry point
  unconditionally (`entrypoint_ret = EntryPoint(0, 0, cur_module_addr, 0)`), and several shipped
  sound modules (`SD_CD`, `SDSTR`, `SOUNDCD`, `SOUNDHD`) put it at the start of `.text`.
- **Relocations use only `R_MIPS_32`, `R_MIPS_26`, `R_MIPS_HI16` and `R_MIPS_LO16`, all against
  symbol 0.** Symbol 0 means "this module": the link-time address is already in the instruction or
  data word, so only the base address is added. Named symbols are supported too.
- **`r_offset` is the link-time virtual address**, already including the target section's `sh_addr`,
  not an offset within the section.
- **Export-table entries carry `R_MIPS_32` relocations**, so after relocation they are absolute
  addresses. The loader verifies every export lies inside the module instead of guessing whether to
  add the base again.
- **Import stubs are `jr ra; addiu zero,zero,N`**, terminated by two zero words. Patching an
  import means writing `j handler; nop` over the 8-byte stub. The loader does exactly that for
  every function a resolver answers and reports the rest as unresolved; it never invents a return
  value.
- Bases are required to be 64 KiB aligned. That keeps every `%lo16` value unchanged under
  relocation, which the loader checks, and matches the fact that the modules themselves are linked
  from zero.

The loader copies `PT_LOAD`, zeroes `.bss`, applies relocations, scans imports/exports, then patches
stubs. It rejects an ELF that does not fit in 2 MiB, has no `.iopmod`, or contains an unknown
relocation type.

## HLE dispatch

`src/iop/hle.h` owns a trampoline page at `0x001F0000` (below the stack at `0x001FF000`). Each
handler gets a 12-byte slot:

```
syscall      ; 20-bit code field = handler index
jr ra
nop          ; the delay slot: 8-byte slots corrupt the return value
```

The interpreter's syscall hook calls `IopHle::dispatch`, which runs the host handler with the guest
`a0..a3` and lets it set `v0`/`v1`. This makes handlers ordinary C++ lambdas, and a module sees the
same calling convention as a real IOP library. An unresolved import is replaced with `break` by the
probe, so the trap names the exact library and function number.

## Probe results

`--inspect` currently loads **11 modules, 441 imported functions, 6085 `j`/`jal` targets**; after
inter-module resolution and the handlers below, **109 imported functions remain unresolved**. The
remaining set is the kernel surface that still has to be implemented (sifman/sifcmd RPC data paths,
cdvdman, ioman, secrman, sysclib, thevent, thsemap priorities, …).

Initialisers and thread bodies reached on the interpreter:

| Module | Initialiser | Thread bodies |
|---|---|---|
| LIBSD 1.4 | returns | — |
| MCMAN 2.23 | stops at `secrman.4` | — |
| MCSERV 2.13 | returns | thread 1 returns |
| PADMAN 4.2 | returns | threads 1 and 2 return |
| SIO2MAN 2.4 | stops at `thevent.4` | — |
| SD_CD 0.1 | returns | — |
| SDSTR 2.5 | returns | — |
| SOUNDCD 1.0 | returns | thread 1 returns after ~231k instructions; thread 2 starts and runs SPU2 code |

The Konami sound driver (`SOUNDCD`, module `kcet sd drv.`) is the important path: its initialiser
creates a priority-47 thread, that thread sets up SPU2 (writes to `0x1F9000xx`, allocates a 32-bit
timer, hooks `timrman.SetTimerHandler`, initialises SIF/RPC, waits a vblank, creates a
priority-29 worker), and the worker reads SPU2 sample data through the driver's own tables. A
1,000,000-instruction run stays inside that worker without trapping; it is doing real mixing work,
not spinning on an unimplemented call.

Measured hardware surface from the same run (`--mmio`): the DMA registers (`0x1F8010F0`,
`0x1F801014`, `0x1F801404/40C/414`, `0x1F801570`) and the SPU2 block. The SPU2 writes include the
per-voice registers at `0x1F900000`+`voice*0x10` (`0xF000` volumes, `0x8006` pitch, `0x0000`
adsr), the reverb/effect registers at `0x1F900198`, `0x1F9001A0`, `0x1F900344`, `0x1F900580`,
`0x1F900760`, and the core/master registers at `0x1F900788` and `0x1F9007C0`. Those are the
registers the SPU2 model must implement first; the access log is the evidence, not a spec.

## What is real and what is a probe shim

Real behaviour in `lantern-iop`: IRX copying/relocation, import/export parsing and stub patching,
entry-point calling convention, `loadcore` library registration (returns `KE_OK`), a bump
`sysmem` allocator, semaphore counts, hardware-timer allocation and register state, the
36.864 MHz `USec2SysClock` conversion, and the R3000A execution itself.

Probe-only shims, marked here so they are not mistaken for final semantics:

- Threads are recorded and each body is run once after the initialiser; there is no scheduler,
  blocking or priority preemption.
- `WaitSema` never blocks (a would-block wait is logged and treated as acquired).
- SIF is reported already initialised; `sceSifSetDma` completes immediately and `sceSifRpcLoop`
  returns instead of waiting for EE requests.
- Vblank waits complete immediately; hardware timers keep state but never fire their handlers; no
  IOP interrupts are delivered.
- Unmapped I/O reads return zero and writes are logged; the SPU2 and DMA registers have no
  behaviour yet.

## Next steps

1. Replace the probe thread table with an IOP scheduler (ready/waiting/delay queues, priorities,
   `SleepThread`/`DelayThread`/`WaitSema` blocking) and deliver timer, vblank and DMA interrupts.
2. Model the IOP DMA controller and wire SIF DMA to the existing EE-side SIF path so RPC calls
   actually cross between the CPUs; then `SOUNDCD`'s RPC server can receive the game's requests.
3. Implement the measured SPU2 register block and voice mixing, then feed its output to the audio
   backend.
4. Grow the kernel HLE from the 109 unresolved imports, keeping the public function numbering
   (ps2sdk headers) as the interface reference.
