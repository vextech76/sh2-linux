# Stage 6 — Static analysis of SLUS-20228 (feasibility report)

Produced by `lantern-analyze` on the owner's disc (2026-11-03). This file records **structure
and counts only**: no game code, disassembly or assets. The full local report and function map
are written to `$XDG_DATA_HOME/lantern/analysis/SLUS-20228/` (not in git).

Reproduce:
```sh
build/release/src/lantern-ingest extract "/path/Silent Hill 2.iso"
build/release/src/lantern-analyze --extracted ~/.local/share/lantern/extracted/SLUS-20228 \
                                  --iso "/path/Silent Hill 2.iso"
```

## 1. Tool validation

The R5900 decoder (`src/ee/r5900.*`) was differentially tested against GNU binutils 2.46
(`objdump -m mips:5900`): **0 disagreements** on validity and mnemonic over a 131,520-word
field sweep, 4,000,000 random words, and all 760,352 words of the boot ELF's main segment.
Deliberate deviations (R5900 manual over GNU): `cvt.w.s` (GNU: `trunc.w.s` — the R5900
always truncates), `sqrt.s fd,ft` operand layout, `vsqrt` with zero field selectors, and
64-bit/double FPU formats that the R5900 lacks. Oracle tests run in CTest (label `oracle`)
when `binutils-mips64-linux-gnu` is installed.

## 2. Boot ELF (`SLUS_202.28`)

| Item | Finding | Evidence |
|---|---|---|
| Format | ELF32 LE MIPS, flags `5900 eabi64 mips3`, entry `0x00100008` | ELF header |
| Symbols | none usable (3-entry symtab, unnamed sections) | section table |
| Main image | `0x00100000`–`0x003E6880` file-backed; 31.5 MB incl. BSS | PT_LOAD |
| Functions found | **7,237** (4,219 via calls/entry, 3,018 via address constants or prologue scan) | recursive descent |
| Coverage | 58.2% of main-image words reached as code; remaining ranges are data in the same segment (.rodata/.data share it) | reachability map |
| Jump tables | 203 resolved (3,181 targets); 11 unresolved `jr`; 166 indirect calls (`jalr`) | GCC switch pattern |
| Invalid encodings on reached paths | 2 (likely data after non-returning calls) | — |

**Implication:** a function-level recompiler is viable. Indirect calls and the 11 unresolved
jumps need a runtime address→function dispatch with a loud trap for unknown targets.

## 3. Overlays — confirmed

52 zero-file-size PT_LOAD segments share base `0x01F0BC00`. The 54 `GX/ GY/ GZ/*.BIN` files
on the disc are those overlays, on three independent lines of evidence:

1. The main image contains the overlay file names as strings (`./gx/*.bin`, `./gy/*.bin`, `./gz/*.bin`).
2. Decoded at `0x01F0BC00`, overlays make 5,912 `jal`s outside themselves; **4,912 land exactly
   on main-image function starts** found independently in §2.
3. 53 of 54 files fit an ELF overlay segment's `memsz` (exact or ≤ 1 KB BSS slack).

The main image reaches overlay code indirectly (217 `lui` references into the overlay region,
no direct `jal`). **Implication:** the recompiler must emit each overlay separately with
overlay-qualified function identities, and the runtime's indirect dispatch must consult the
currently loaded overlay.

## 4. EE kernel usage

67 `syscall` sites, **60 distinct syscalls, all identified** (ps2sdk numbering): threads
(Create/Start/Sleep/Wakeup/Suspend/…), semaphores, INTC/DMAC handlers, alarms, SIF DMA/regs,
GS IMR/CRT, cache flush, `SetupThread/SetupHeap`, `SetSyscall` (2 sites — installs custom
handlers), `Deci2Call` (debug), `KExit`. **Implication:** the HLE kernel surface is finite and
known; `SetSyscall` and `Deci2Call` need explicit handling.

## 5. VU microcode

Heuristic VIF `MPG` scan of the main image: **157 blocks, 26,004 micro-instruction pairs
(~203 KB)**. SH2's rendering is VU1-microcode-driven, so a VU1 implementation is mandatory for
any image output (Stage 8). Overlays and `DATA/*.MGF` were not scanned yet.

## 6. IOP side

`IOPRP.IMG` replaces SCE system modules: SIFCMD, SIFMAN, THREADMAN, IOMAN, MODLOAD, FILEIO,
CDVDMAN, CDVDFSV, LOADFILE, TIMEMANI, ROMDRV, EESYNC.

| Module | Origin | Imports (library: functions used) |
|---|---|---|
| SIO2MAN, PADMAN, MCMAN, MCSERV | SCE standard | intrman, loadcore, sio2man, dmacman, thbase/thevent/thsemap, sifman/sifcmd, vblank, … |
| LIBSD (`Sound_Device_Library` 1.4), SDRDRV | SCE standard | intrman, sifman/sifcmd, thbase/thevent |
| SDSTR, SD_CD, SD_HD | game sound streaming (Konami) | libsd, cdvdman (21), ioman, thread/sema/vblank |
| SOUNDCD / SOUNDHD (`kcet sd drv.`, 78 KB each) | **game-specific sound driver (Konami)** | libsd (14), sd_cd, sdstr (11), timrman, thread/sema |

**Implication — proposed IOP strategy (decision D-6.1):**
- *HLE at the SIF-RPC boundary* for SCE modules with publicly documented protocols
  (padman → `InputSystem`, mcman/mcserv → memory-card files, cdvdfsv/fileio → `vfs::Disc`).
- *Interpret the game's own sound modules* (SOUNDCD/SDSTR/SD_CD and the disc's LIBSD) on a small
  R3000A interpreter with HLE of the IOP kernel libraries they import (the import lists above
  are the complete required surface), and emulate SPU2 at register level. This avoids
  reverse-engineering Konami's private sound-driver protocol and keeps audio faithful.

## 7. Feasibility verdict

| Area | Verdict |
|---|---|
| EE code translation | **Feasible**: clean GCC output, decoder validated, functions/jump tables recoverable |
| Overlays | **Feasible**: structure understood; needs overlay-aware dispatch |
| EE kernel HLE | **Feasible**: 60 known syscalls |
| IOP | **Feasible with hybrid approach** (D-6.1); sound needs R3000A interpreter + SPU2 |
| VU1 + GS → Vulkan | **Largest risk**: ~200 KB microcode; needs VU1 interpreter first, GS translation second |
| FMV (IPU MPEG-2, `MOVIE.MGF` 1.7 GB) | Feasible later (IPU HLE → software MPEG-2 decode) |

## 8. Open questions / next analysis

- Format of `DATA/*.MGF` archives (and whether they contain VU microcode / overlays).
- Exact overlay load protocol (which main-image routine reads `./gx/*.bin`, how entry points are found).
- The 11 unresolved `jr` sites and 166 `jalr` sites: which are virtual-call tables vs. callbacks.
- Export-table parsing for SD_CD/SDSTR reports 0 functions (format variant to investigate).
