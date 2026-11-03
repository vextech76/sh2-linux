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
| LAN-10 | HLE kernel + SIF + IOP pad/cdvd/memcard | in_progress | | kernel + SIF done; boot reaches LOADFILE/CDVD |
| LAN-11 | VU1 + GS → Vulkan | todo | | |
| LAN-12 | SPU2 audio, IPU FMV | todo | | |
| LAN-13 | Choose a license | blocked | | Owner decision |
| LAN-14 | Remove wrongly installed `volk-devel` (GNU Radio) | todo | | Owner action: `sudo dnf remove volk-devel` |
| LAN-15 | MGF archive format analysis | todo | | |
| LAN-16 | R3000A interpreter + IOP kernel-library HLE (for game sound modules) | todo | | Decision D-6.1 |
| LAN-17 | SPU2 register-level emulation | todo | | |

## [NEXT]
- LAN-18: IOP HLE servers: CDVDFSV (0x80000592/593/595/596/597/59A: init, S/N commands,
  sector reads from the read-only ISO, search file, disk ready/sync), LOADFILE (0x80000006:
  module load acknowledgements per loaded IRX), then padman, mcserv, and sound placeholder.
