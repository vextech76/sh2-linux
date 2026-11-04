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
| LAN-11 | VU1 + GS → Vulkan | todo | | |
| LAN-12 | SPU2 audio, IPU FMV | todo | | |
| LAN-13 | Choose a license | blocked | | Owner decision |
| LAN-14 | Remove wrongly installed `volk-devel` (GNU Radio) | todo | | Owner action: `sudo dnf remove volk-devel` |
| LAN-15 | MGF archive format analysis | todo | | |
| LAN-16 | R3000A interpreter + IOP kernel-library HLE (for game sound modules) | todo | | Decision D-6.1 |
| LAN-17 | SPU2 register-level emulation | todo | | |

## [NEXT]
- LAN-11 (Stage 8): DMA controller (VIF1/GIF/SPR channels, chain mode) -> VIF1 unpack ->
  VU1 micro-mode interpreter -> GIF packets -> GS state machine + local memory -> Vulkan
  presentation of the display buffer. Start with SPR DMA (memory-to-memory) and GIF PATH3
  image transfers so the first frame buffer can be shown.
- LAN-19: padman (0x80000100/101) wired to InputSystem; mcserv (0x80000400) on XDG memcards.
