# AGENTS.md — LANTERN (sh2-linux)

Governed by Agentic Engineering (`../agentic_engineering/CLAUDE.md`): COMPASS for planning,
DOCTRINE for every change. No operator UI yet (IRON SIGHT applies if a settings/debug UI is added).

## Project Overview

LANTERN (*Linux Adaptation: Native Translation of the EE, Rendering & Nodes*) is a C++20
Linux runtime for a static recompilation of PS2 *Silent Hill 2*: SDL3 platform layer, Vulkan
renderer behind an abstraction, virtual-DualShock-2 input mapping, TOML config. Design:
`docs/DESIGN.md`. Master plan: `docs/plans/`.

## Tool bindings

| DOCTRINE role | Binding |
|---|---|
| Tracker | `docs/TRACKER.md` (local; IDs `LAN-n`) |
| Lint | `clang-format --dry-run --Werror` (via `scripts/check.sh`) + `-Werror` build |
| Test suite | `ctest --preset debug` (parallel); GPU: `ctest --preset debug-gpu` |
| Full gate | `scripts/check.sh --gpu` |
| Deploy | none (local builds only); remote `origin` = github.com/vextech76/sh2-linux (private) |
| Commit format | `type(scope): description (LAN-n)` |

## Hard rules

1. **Game data is protected.** `/home/vincent/PS2_Roms/` is read-only input. Never write
   there, never commit anything extracted, decompiled, or recompiled from it. Generated
   outputs go to `$XDG_DATA_HOME/lantern/` or `build/`.
2. **The BIOS is not used** by design (HLE kernel). Don't read it without asking the owner.
3. **Placeholders are always visibly marked** (the PLACEHOLDER shader, window title, logs).
   Never invent game content or substitute unrelated assets.
4. Ask the owner before deleting files, choosing or changing a license, or adding large dependencies.
5. `src/input` must stay SDL-free (unit-testable). Render code never includes SDL.
