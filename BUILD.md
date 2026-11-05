# Building and running LANTERN (Linux)

Tested on Fedora 44 (x86_64, GCC 16, Mesa RADV). Other distributions need the equivalent packages.

## 1. Dependencies

```sh
sudo dnf install -y gcc-c++ libasan libubsan cmake ninja-build \
  SDL3-devel vulkan-headers vulkan-loader-devel vulkan-validation-layers glslc \
  tomlplusplus-devel doctest-devel clang-tools-extra
```

Debian/Ubuntu equivalents: `g++ cmake ninja-build libsdl3-dev libvulkan-dev
vulkan-validationlayers glslc libtomlplusplus-dev doctest-dev clang-format`.

You need a GPU and driver with **Vulkan 1.3** (dynamic rendering + synchronization2).
Check it with `vulkaninfo --summary`, or `lantern --list-gpus` once you've built LANTERN.

## 2. Build

```sh
cmake --preset debug          # -Werror, AddressSanitizer + UBSan, validation layers auto-on
cmake --build --preset debug
# or optimised:
cmake --preset release && cmake --build --preset release
```

Output: `build/<preset>/src/lantern`. In-source builds are rejected.

## 3. Test

```sh
ctest --preset debug          # unit + headless smoke tests (no GPU/display needed)
ctest --preset debug-gpu      # Vulkan tests: needs a display; runs with validation on
scripts/check.sh [--gpu]      # full gate: clang-format check, build, tests
```

The tests use their own temporary XDG directories, so your real settings are never touched.

## 4. Run

```sh
build/debug/src/lantern                     # window with the PLACEHOLDER pattern (no game runtime yet)
build/debug/src/lantern --list-gpus         # device suitability report
build/debug/src/lantern --print-config      # effective configuration
build/debug/src/lantern --backend null --frames 60   # headless
build/debug/src/lantern --help
```

Exit codes: 0 ok · 1 startup failure (message explains: no GPU, missing loader, ...) ·
2 usage/config/path-safety error · 3 Vulkan validation errors were reported.

## 5. Ingest your disc (read-only)

```sh
build/release/src/lantern-ingest identify "/path/to/Silent Hill 2.iso" --hash
build/release/src/lantern-ingest extract  "/path/to/Silent Hill 2.iso"   # boot set -> ~/.local/share/lantern/extracted/<serial>/
```

## 6. Static analysis (writes only to ~/.local/share/lantern/analysis/)

```sh
build/release/src/lantern-analyze --extracted ~/.local/share/lantern/extracted/SLUS-20228 --iso "/path/to/Silent Hill 2.iso"
```

## 7. Recompile (writes only to ~/.local/share/lantern/recomp/)

```sh
build/release/src/lantern-recomp --extracted ~/.local/share/lantern/extracted/SLUS-20228 --iso "/path/to/Silent Hill 2.iso"
cmake -S ~/.local/share/lantern/recomp/SLUS-20228 -B ~/.local/share/lantern/recomp/SLUS-20228/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build ~/.local/share/lantern/recomp/SLUS-20228/build   # -> liblantern_game.so (run with lantern-boot)
```

## 8. IOP modules (read-only probe)

```sh
build/release/src/lantern-iop --extracted ~/.local/share/lantern/extracted/SLUS-20228 --inspect
build/release/src/lantern-iop --extracted ~/.local/share/lantern/extracted/SLUS-20228                              --module SOUNDCD --steps 400000 --mmio
```

Loads the game's own `.IRX` modules on the R3000A, resolves inter-module imports, and runs an
initialiser and every thread it starts under the priority scheduler. Findings and the remaining
kernel surface are in `docs/IOP.md`. The probe is read-only and never writes game-derived data.

`lantern-boot` runs the real sound modules by default (LIBSD/SD_CD/SDSTR/SOUNDCD). Pass
`--no-sound-iop` to fall back to the placeholder sound RPC service while debugging.

## 9. Boot (headless, development)

```sh
G=~/.local/share/lantern/recomp/SLUS-20228
cmake -S $G -B $G/build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build $G/build   # liblantern_game.so
build/release/src/lantern-boot --extracted ~/.local/share/lantern/extracted/SLUS-20228 \
                               --iso "/path/to/Silent Hill 2.iso" --seconds 20
```
Prints a boot report: outcome, syscalls, threads, RPC servers used, unmodelled hardware.

Add `--window` to play in a window (keyboard/controller per `~/.config/lantern/config.toml`,
default: arrows = d-pad, Space = Cross, Escape/Return = Start, F11 fullscreen) and `--seconds 0` to run without the watchdog time limit. `--dump-frames DIR` writes PNGs; `--press 600:cross`
scripts input; `--swizzle-variant 0,1` selects the indexed-format in-block orders (see
`src/gs/gs_memory.cpp` and DESIGN.md §8.3).

**After changing `src/ee/*.h`**, rebuild both the host and the generated game library (the library
inlines those headers; mismatched layouts crash unpredictably):

```sh
cmake --build --preset release && cmake --build ~/.local/share/lantern/recomp/SLUS-20228/build
```

Optional decoder oracle: `sudo dnf install binutils-mips64-linux-gnu` enables the `oracle` CTest label.

## Troubleshooting

| Symptom | Fix |
|---|---|
| `validation requested but VK_LAYER_KHRONOS_validation is not installed` | `sudo dnf install vulkan-validation-layers` |
| `no suitable Vulkan GPU found` + per-device reasons | update Mesa/NVIDIA driver; pick another with `preferred_gpu` |
| `SDL_Init failed` | run from a graphical session (X11/Wayland), or use `--backend null` |
| Wrong GPU on hybrid laptop | `preferred_gpu = "7600M"` in config, see `--list-gpus` |
