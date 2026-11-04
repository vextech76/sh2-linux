# Tracker

Local DOCTRINE tracker. Status: `todo` · `in_progress` · `review` · `done` · `blocked`.
Parent epic **LAN-0**: SH2 Linux port (plan: `docs/plans/lantern-sh2-port_4ba54e2e.plan.md`).

| ID | Title | Status | Commit | Notes |
|---|---|---|---|---|
| LAN-1 | Scaffold: CMake presets, docs, gate script | done | see git log | |
| LAN-2 | Core: logging, XDG paths | done | | |
| LAN-3 | Config: TOML parse/serialise/validate | done | | 11 test cases |
| LAN-4 | Input: virtual DS2, bindings, dead zones, hot-plug, capture | done | | 25 test cases |
| LAN-5 | Render: abstraction + Null + Vulkan 1.3 + PLACEHOLDER | done | | GPU tests incl. swapchain recreation under validation |
| LAN-6 | Platform: SDL3 window/events/gamepads, fullscreen, mouse capture | done | | |
| LAN-7 | Ingest: read-only ISO9660 reader + disc ID + extraction | done | | SLUS-20228 identified; boot set extracted to XDG data |
| LAN-8 | Analysis: EE ELF parse, function/syscall/IRX/VU inventory | done | | See docs/ANALYSIS.md; overlays confirmed |
| LAN-9 | Recompiler: R5900 → C++ | done | | 514,628 insns; 14 runtime-trap sites; differential tests |
| LAN-10 | HLE kernel + SIF + IOP pad/cdvd/memcard | done | | kernel, SIF, CDVD, LOADFILE, IOP heap; boot reaches the render loop |
| LAN-11 | VU1 + GS → Vulkan | in_progress | | DMAC/VIF1/GIF/GS + software rasterizer done; first frame rendered |
| LAN-12 | SPU2 audio, IPU FMV | todo | | |
| LAN-22 | Memory card model + mcman service | in_progress | | card file/format done; presence check unsatisfied |
| LAN-25 | Follow guest continuations in recompiled calls | done | | found by reaching the title screen |
| LAN-26 | Turbo mode stalls boot (timing divergence) | todo | | paced mode works; investigate before using turbo for behaviour |
| LAN-19 | padman HLE -> InputSystem | done | | |
| LAN-21 | Local-memory layout derived from documented structure; PSMT8 order measured | done | | |
| LAN-20 | GS display in the Vulkan window | done | | |
| LAN-13 | Choose a license | blocked | | Owner decision |
| LAN-14 | Remove wrongly installed `volk-devel` (GNU Radio) | todo | | Owner action: `sudo dnf remove volk-devel` |
| LAN-15 | MGF archive format analysis | todo | | |
| LAN-16 | R3000A interpreter + IOP kernel-library HLE (for game sound modules) | todo | | Decision D-6.1 |
| LAN-17 | SPU2 register-level emulation | todo | | |

## [NEXT]
- LAN-24: verify the T4 in-block order on a screen that exercises 4-bit textures
  (`--swizzle-variant 0,N`), then promote it from "derived" to "measured" in DESIGN.md. T8 was
  measured on the title screen (logo legible; alternatives produce noise).
- LAN-22: mcserv HLE (0x80000400) backed by $XDG_DATA_HOME/lantern/memcards.
- LAN-23: run the game from the main `lantern` executable (session refactor of lantern-boot).
- LAN-11 cont.: VU1 micro-mode interpreter (+ XGKICK PATH1) when the game first uploads microcode.
- LAN-19: padman (0x80000100/101) wired to InputSystem; mcserv (0x80000400) on XDG memcards.
