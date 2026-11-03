---
name: LANTERN — Silent Hill 2 (PS2) Linux static-recompilation port
overview: >
  Build a Linux-native runtime (SDL3 + Vulkan) and the tooling to ingest a user-supplied
  SH2 PS2 disc image, statically recompile its EE executable to C++, and run it on HLE
  replacements of the PS2 kernel, IOP modules, GS/VU1, SPU2 and IPU. No game data in the repo.
compass_phase: final
todos:
  - id: s1-scaffold
    content: Repo, CMake presets, docs, tracker, check gate
    status: completed
  - id: s2-platform-config
    content: SDL3 platform layer, XDG paths, TOML config with validation + tests
    status: completed
  - id: s3-vulkan
    content: Renderer abstraction, Null + Vulkan 1.3 backend (validation, swapchain recreation, sync), PLACEHOLDER
    status: completed
  - id: s4-input
    content: Virtual DS2, bindings, dead zones, rebinding/capture, hot-plug + tests
    status: completed
  - id: s5-ingest
    content: Read-only ISO9660 reader, disc identification (SYSTEM.CNF, hash), extraction to XDG data
    status: completed
  - id: s6-analysis
    content: EE ELF parser, function discovery, syscall/IRX/VU-microcode inventory, feasibility report
    status: completed
  - id: s7-recomp-runtime
    content: R5900 -> C++ translator, memory map, HLE kernel/SIF, pad/cdvd/memcard IOP HLE
    status: pending
  - id: s8-gs-vu1
    content: VU1 interpreter, GIF/GS translator onto the renderer abstraction
    status: pending
  - id: s9-audio-fmv
    content: SPU2 -> SDL audio, IPU MPEG-2 FMV
    status: pending
isProject: true
---

# LANTERN — master plan

## RECON (findings, 2026-11-02)
- Workspace had **no** existing SH2/Silent Hill project — greenfield; nothing replaced.
- Supplied data: `/home/vincent/PS2_Roms/Silent Hill 2.iso` (4.64 GB DVD image),
  `scph10000.zip` + unpacked `scph10000/` (Japanese v1.00 BIOS). Only names/sizes inspected.
- Host: Fedora 44 Xfce/X11, RX 7600M XT (RADV, Vulkan 1.4) + 780M iGPU, GCC 16.
- Note: the original *Silent Hill* (1999) is a PS1 title; the PS2 target is *Silent Hill 2*.

## FRAME
The user wants SH2 PS2 running natively on Linux with Vulkan, KB/M + controllers, configurable,
maintainable and tested. The constraining problem isn't the Linux layer; it's executing EE
code built for very different hardware (R5900 + VU0/VU1 + GS + IOP) without shipping or
inventing game content.

## TARGET
A `lantern` executable that, given the user's ISO path, identifies the disc, uses a
recompiled EE program generated locally from that disc, and runs it with HLE system
services, presenting GS output through Vulkan and reading input from the virtual DS2.

## APPROACH (chosen) and alternatives
| Option | Verdict |
|---|---|
| **Static recompilation + HLE runtime** (chosen) | Native performance, clean host integration, no BIOS. Highest up-front effort. |
| Embed an emulator (PCSX2 core) | Fast route to playable, but GPL-3 obligations, not a "port", and PCSX2 already exists. Revisit only if recomp stalls. |
| Full matching decompilation | Most maintainable result but years of human effort; can follow on from recomp output. |
| Clean-room engine reimplementation | Needs complete format/behaviour knowledge; much later, if ever. |

## Architecture overview
See `docs/DESIGN.md` §1. Runtime libraries are layered core → input → config → render → platform → app;
PS2 subsystems (ee, iop, gs, vu, spu2, ipu) slot between platform and app and talk to render/input only
through their abstractions.

## Scope boundaries
In: Linux x86_64, Vulkan 1.3, SDL3, NTSC-U/PAL SH2 PS2 disc(s) the user owns.
Out (for now): other OSes, other games, netplay, enhancement mods, shipping any game-derived files,
BIOS LLE.

## Integration map
- `/home/vincent/PS2_Roms` — read-only input; never written.
- XDG config/data/cache dirs — all writable state.
- System packages (dnf) — toolchain + SDL3/Vulkan/toml++/doctest.

## Risks and trade-offs
- VU1 microcode + GS effects accuracy (largest risk) → start with an interpreter and compare
  against reference behaviour; recompile VU later.
- Indirect control flow / self-modifying or overlay code in the EE ELF → runtime trap with PC
  and an "add function" workflow, never silent skips.
- Timing (VSync/DMA interrupts) → event scheduler; hangs are likely early.
- Legal: generated code is a derivative work → stays out of git by construction.

## Decision log
- **C++20/CMake/SDL3** — approved by owner 2026-11-02.
- **No volk** — Fedora `volk-devel` is GNU Radio VOLK (name clash); link the loader directly.
- **Vulkan 1.3 baseline** (dynamic rendering + sync2) — simpler, correct sync; all target GPUs qualify.
- **UNORM swapchain** — GS output is display-encoded; avoid double gamma.
- **Per-image render-finished semaphores** — avoids reuse hazards flagged by modern validation.
- **Input via virtual DS2 only** — the game reads pad buffers; one mapping point, testable without SDL.
- **No BIOS** — HLE kernel; avoids needing/reading copyrighted firmware at runtime.
- **D-6.1 Hybrid IOP** (Stage 6 finding) — HLE SCE modules at SIF-RPC level; interpret the
  game's own Konami sound modules (R3000A + IOP-kernel HLE + SPU2) instead of reverse-engineering
  their private protocol. See docs/ANALYSIS.md §6.
- **Overlay-aware recompilation** — 54 overlays share 0x01F0BC00; functions are identified by
  (overlay, address) and indirect dispatch consults the loaded overlay.
