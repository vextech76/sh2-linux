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

## Threads and virtual time

`iop_threads.*` is a cooperative priority scheduler. Each `IopThread` owns a complete `IopCpu`
state and a private stack; a handler blocks by throwing `ThreadSwitch` after the scheduler advances
the thread past its trampoline `syscall`, so it resumes at `jr ra`. `SleepThread`, `WaitSema`,
`DelayThread` and the vblank waits block; `SignalSema` hands a token directly to a waiter instead
of incrementing and waking; `WakeupThread` releases a sleeping thread. Virtual time is the
36.864 MHz IOP clock (one cycle per instruction), advanced to the next timed wake when every thread
is blocked. `now_us`, `USec2SysClock` and `GetSystemTime` all use that clock. A 4096-instruction
quantum rotates ready threads at the same priority.

Still missing from this layer: hardware-timer callbacks do not fire, and no interrupt context is
delivered. Those are the next step.

## SPU2

`spu2.*` implements the measured SPU2 block: two cores 0x400 apart, 24 voices each, the voice
parameter and SSA/LSAX/NAX register arrays, key on/off, ENDX/STATX, core and master volumes,
transfer-address/data, and a 2 MiB sample RAM. Voice playback decodes PS-ADPCM (16-byte blocks,
shift/filter and the four prediction coefficient pairs), advances at the pitch-derived rate,
loops at LSAX and raises IOP IRQ 9 when a voice ends. The IOP DMA channels for core 0/1
(`0x1F8010C0`, `0x1F801500`) copy blocks between IOP RAM and SPU2 RAM and raise IRQ `0x24`/`0x28`
when the transfer completes.

`intrman.RegisterIntrHandler` now records real handlers; `IopKernel::raise_irq` schedules the
registered guest handler as a high-priority callback. This is still thread context, not an
interrupt vector, but it lets the sound driver's completion handlers run and signal semaphores.

At the title screen no voice is keyed on yet, so a 1,500-frame run reports `audio: 1,200,000
frames mixed, peak 0`. The mixer is exercised by unit tests with synthetic ADPCM blocks.

## Real sound modules in the boot runtime

`iop_runtime.*` packages the RAM/interpreter/scheduler/kernel-HLE into one object. `lantern-boot`
now loads `LIBSD.IRX`, `SD_CD.IRX`, `SDSTR.IRX` and `SOUNDCD.IRX`, starts their initialisers and
runs their threads between EE frames (100k IOP instructions per frame). `SifHle` shares the
runtime's RAM and, for a SID registered by a real module, sends the RPC call to that module's server
instead of the placeholder. The sound driver registers SID `0x15` with function `0x478` and buffer
`0x56700`; in a 2,100-frame run the bridge served **1,970 real RPC calls with zero failures**.

Thbase alarms and timrman handlers are now real: they are scheduled on the IOP virtual clock and run
as high-priority guest threads (`IopScheduler::schedule_callback`), one at a time. That is what
unblocked the sound driver's RPC server, which waits on a semaphore its alarm callback signals.
The callbacks still run in thread context, not an interrupt context; no IOP interrupt vector is
delivered yet.

After the title screen's Start press the EE main thread terminates, but the game does not load
`MENU.MGF`: the remaining threads wait on semaphores and the display stays on the title. The next
candidate is the SPU2/DMA path (playback completion) rather than SIF, since the sound RPC bridge is
now answering successfully.

## Probe results

`--inspect` currently loads **11 modules, 441 imported functions, 6085 `j`/`jal` targets**; after
inter-module resolution and the handlers below, **109 imported functions remain unresolved**. The
remaining set is the kernel surface that still has to be implemented (sifman/sifcmd RPC data paths,
cdvdman, ioman, secrman, sysclib, thevent, thsemap priorities, …).

Initialisers and threads reached on the interpreter:

| Module | Result |
|---|---|
| LIBSD 1.4 | initialiser returns |
| MCMAN 2.23 | initialiser traps at `secrman.4` |
| MCSERV 2.13 | initialiser returns; thread 2 blocks on semaphore 0 (waiting for an EE command, reported as a deadlock) |
| PADMAN 4.2 | initialiser returns; threads 1 and 2 return |
| SIO2MAN 2.4 | initialiser traps at `thevent.4` |
| SD_CD 0.1 | initialiser returns |
| SDSTR 2.5 | initialiser returns |
| SOUNDCD 1.0 | initialiser returns; nine driver threads exist and the scheduler runs them concurrently |

The Konami sound driver (`SOUNDCD`, module `kcet sd drv.`) is the important path. Its initialiser
creates a priority-47 thread; that thread sets up SPU2 (writes to `0x1F9000xx`, allocates a 32-bit
timer, hooks `timrman.SetTimerHandler`), initialises SIF/RPC, waits for a vblank and creates more
workers. After a 1,000,000-instruction run the scheduler reports three threads finished, one
sleeping, one waiting for a vblank, one running (the priority-47 mixing thread), one ready and
three dormant. Nothing traps: the driver is doing its own work and waiting for vblank/SIF, not
stuck on an unimplemented import. The 56 seconds of virtual time are mostly idle waiting; that is
expected until the EE side sends RPC requests.

Measured hardware surface from the same run (`--mmio`): the DMA registers (`0x1F8010F0`,
`0x1F801014`, `0x1F801404/40C/414`, `0x1F801570`) and the SPU2 block. The SPU2 writes include the
per-voice registers at `0x1F900000`+`voice*0x10` (`0xF000` volumes, `0x8006` pitch, `0x0000`
adsr), the reverb/effect registers at `0x1F900198`, `0x1F9001A0`, `0x1F900344`, `0x1F900580`,
`0x1F900760`, and the core/master registers at `0x1F900788` and `0x1F9007C0`. Those are the
registers the SPU2 model must implement first; the access log is the evidence, not a spec.

## What is real and what is a probe shim

Real behaviour in `lantern-iop` and `lantern-boot`: IRX copying/relocation, import/export parsing
and stub patching, entry-point calling convention, `loadcore` library registration (returns
`KE_OK`), a bump `sysmem` allocator, the priority scheduler with blocking waits and a virtual
clock, semaphore counts and hand-off, hardware-timer allocation/register state, thbase alarms and
timrman callbacks (as guest threads), vblank wakeups, the 36.864 MHz clock conversions, the SIF RPC
bridge to real IOP servers, the SPU2 register/voice/ADPCM/DMA core, intrman handlers on IRQ lines,
and the R3000A execution itself.

Probe-only shims, marked here so they are not mistaken for final semantics:

- IOP interrupts are not delivered on a real vector. Timer/alarm and SPU2/DMA handlers run as guest
  threads; the CPU does not push an interrupt frame.
- SIF transfers complete immediately; the EE-side command parser still owns packet marshalling and
  the boot path still uses the HLE services for pad, memory card and CDVD.
- The SPU2 model does not implement reverb, noise, pitch modulation or SPDIF; unmapped I/O still
  returns zero. Audio is mixed but not yet routed to a host audio device.
- `WaitVblankStart/End/WaitVblank/WaitNonVblank` all wake at the next frame.

## Next steps

1. Route the mixed PCM to SDL3 audio and add an `--no-audio` fallback.
2. Add a real IOP interrupt context for SPU2/DMA/vblank (the handlers currently run as threads).
3. Finish the title→menu investigation: with SPU2 working, re-check whether the EE loader thread
   advances to `MENU.MGF`; otherwise pursue VU1/IPU (both still placeholders).
4. Grow the kernel HLE from the remaining unresolved imports, keeping the public function numbering
   (ps2sdk headers) as the interface reference.
