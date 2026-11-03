# LANTERN

*Linux Adaptation: Native Translation of the EE, Rendering & Nodes.*

A Linux-native runtime aimed at running a **static recompilation of the PlayStation 2
release of *Silent Hill 2*** with Vulkan rendering, keyboard/mouse and controller support.

> **Status:** early. The platform layer, input mapping, configuration and the Vulkan renderer
> are done and tested. The game runtime (EE recompiler, PS2 HLE, GS→Vulkan) isn't written
> yet, so the window shows a clearly marked **PLACEHOLDER** pattern. See
> [docs/DESIGN.md](docs/DESIGN.md) and [docs/TRACKER.md](docs/TRACKER.md).

**No game content is included or distributed.** You need your own legally obtained disc
image. LANTERN only reads it, and keeps all generated or extracted data under your XDG
user directories.

- Build & run: [BUILD.md](BUILD.md)
- Configuration: [docs/CONFIG.md](docs/CONFIG.md) · Bindings: [docs/INPUT.md](docs/INPUT.md)
- Architecture: [docs/DESIGN.md](docs/DESIGN.md) · Plan: [docs/plans/](docs/plans/)

## Layout

```
src/core       logging, XDG paths
src/input      virtual DualShock 2, bindings, dead zones, hot-plug (SDL-free, unit-tested)
src/config     TOML configuration
src/render     backend-neutral renderer, Null backend, Vulkan backend + shaders
src/platform   SDL3 window, events, gamepads
src/iso        read-only ISO9660 reader
src/ingest     disc identification/extraction + lantern-ingest tool
src/ee         R5900 decoder (binutils-validated), ELF loader, lantern-disasm
src/analysis   function discovery, syscalls, overlays, IOP modules, lantern-analyze
src/app        CLI and main loop
tests/         doctest unit tests + CTest smoke/GPU tests
scripts/       check.sh (lint + build + tests gate)
```
