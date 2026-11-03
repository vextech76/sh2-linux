# LANTERN — Design

**LANTERN** — *Linux Adaptation: Native Translation of the EE, Rendering & Nodes.*
A Linux-native runtime for a statically recompiled PlayStation 2 *Silent Hill 2*, rendering
through Vulkan. The project ships **no game content**; the user supplies their own disc image.

Status legend: ✅ implemented & tested · 🟡 partial · ⏳ planned · ❓ research needed

---

## 1. System position

```
 user's disc image (read-only)          LANTERN repository (no game data)
 ┌──────────────────────────┐     ┌────────────────────────────────────────────────┐
 │ /home/…/PS2_Roms/*.iso   │──▶──│ tools/ingest  ISO9660 reader, hash/ID, extract ⏳│
 └──────────────────────────┘     │ tools/recomp  EE ELF → C++ translator        ⏳│
                                  │                                              │
 $XDG_DATA_HOME/lantern/          │ runtime (lantern executable)                 │
 ├─ extracted/  (disc files)  ◀───│  ├─ platform  SDL3 window/events/hot-plug   ✅│
 ├─ recomp/     (generated C++)   │  ├─ input     virtual DualShock 2 mapping   ✅│
 └─ memcards/                     │  ├─ config    TOML settings                 ✅│
 $XDG_CONFIG_HOME/lantern/        │  ├─ render    abstraction + Vulkan + Null   ✅│
 └─ config.toml                   │  ├─ ee/iop/gs/vu/spu2/ipu  PS2 HLE runtime  ⏳│
 $XDG_CACHE_HOME/lantern/         │  └─ app       CLI + main loop               ✅│
 └─ vk_pipeline_cache.bin         └────────────────────────────────────────────────┘
```

**Data rule (enforced):** nothing is ever written next to supplied game data. All writable
state lives under XDG directories; at startup `main.cpp` refuses to run if any writable
directory resolves inside the disc image's directory. Generated recompiled code is a
derivative of the game and lives in `$XDG_DATA_HOME/lantern/recomp/` or `build/`, both
outside version control (`.gitignore` also blocks ISO/ELF/BIOS-like names).

## 2. Language, build, dependencies

| Item | Choice | Why |
|---|---|---|
| Language | C++20 (GCC 16) | PS2 recompiler ecosystem is C/C++; generated code must compile fast |
| Build | CMake ≥ 3.25 + Ninja, `CMakePresets.json` | reproducible; `debug` preset = `-Werror` + ASan/UBSan |
| Window/input | SDL3 (system, Fedora `SDL3-devel`) | Vulkan surfaces, gamepads + hot-plug + mapping DB, audio later |
| Vulkan | system loader + headers (`Vulkan::Vulkan`) | no meta-loader needed at this scale |
| Config | toml++ (system, header-only) | small, typed errors with line/column |
| Tests | doctest (system) + CTest | fast, parallel, one executable per area |
| Shaders | glslc → embedded SPIR-V (`-mfmt=c`) | no runtime shader files to lose |

Rejected: **volk** — Fedora's `volk-devel` is GNU Radio's unrelated DSP library; the Vulkan
meta-loader isn't packaged, and direct loader linking costs nothing measurable here.

## 3. Platform layer (`src/platform`) ✅

`SdlPlatform` owns SDL init, the window, and gamepad handles. It implements
`render::SurfaceSource` (instance extensions, `VkSurfaceKHR` creation, drawable size) so
render code never includes SDL. `pump()` translates SDL events into `InputSystem` calls:

- Keyboard: SDL scancodes (= USB HID usages), key-repeat filtered.
- Mouse: buttons always; relative motion only while captured (F10), so moving the cursor
  over the window never nudges a stick.
- Gamepads: `SDL_EVENT_GAMEPAD_ADDED/REMOVED` open/close devices (initially-connected pads
  arrive as ADDED events too). Enum values are `static_assert`-ed against SDL's.
- Window: pixel-size/restore/maximise → renderer `notify_resize()`; focus loss → release all
  input so nothing sticks down.
- Fullscreen: windowed / borderless desktop / exclusive (closest display mode), F11 toggles.

## 4. Input (`src/input`) ✅ — see also [INPUT.md](INPUT.md)

The game only ever sees a **virtual DualShock 2** (`PadState`: 16 digital buttons, 12
pressure bytes, four 0–255 stick axes, `wire_buttons()` active-low). Host devices map onto it:

```
Source (Key | MouseButton | MouseAxis± | GamepadButton | GamepadAxis±/trigger)
   └─ BindingTable (per profile: keyboard_mouse, gamepad) ─▶ Target (PadButton | PadAxis± | HostAction)
```

- A source binds to at most one target per profile; `bind()` moves it and returns the
  conflict so UI/config can report it.
- Gamepad sticks: radial dead zone (inner/outer, direction-preserving); triggers: 1-D dead
  zone. Digital pad buttons fire at `button_threshold`; pressure = analog amount × 255.
- Multiple sources per target combine by max; opposing stick halves cancel.
- Hot-plug: first connected pad is active; on removal the next connected pad takes over and
  the removed pad's state vanishes (pad returns to neutral).
- Rebinding: `begin_capture()` swallows the next distinct input (axis needs ≥ 0.6) and
  returns it via `take_captured()`; runtime rebinding takes effect immediately.
- Host actions (fullscreen, mouse capture, quit) fire on press edges only.

`InputSystem` has **no SDL dependency** and is covered by unit tests (mapping, dead zones,
hot-plug, capture, conflicts, focus loss).

## 5. Configuration (`src/config`) ✅

TOML at `$XDG_CONFIG_HOME/lantern/config.toml`, created with defaults on first run.
`parse_config()` never throws: syntax errors (with line/column) and type errors are
**errors** (affected values keep defaults, the app continues loudly); unknown keys,
out-of-range values (clamped) and binding problems are **warnings**. Binding sections
override per target; `[]` unbinds. `serialize_config()` round-trips losslessly (tested).
Writes are atomic (temp file + rename). Keys are documented in [CONFIG.md](CONFIG.md).

## 6. Renderer (`src/render`) 🟡

### 6.1 Abstraction
`render::Renderer` is backend-neutral: `begin_frame() → Ready|Skipped`, draw calls,
`end_frame()`, `notify_resize()`, `set_present_mode()`, `wait_idle()`. Backends: **Vulkan**
and **Null** (headless; CI and logic tests). The factory returns `nullptr` + a user-facing
error. The draw API today is only `draw_placeholder()`; it grows when the GS translator's
needs are concrete (§8.4) — deliberately not speculated ahead of that.

### 6.2 Vulkan backend ✅
- **Instance:** requires Vulkan 1.3. Validation (`VK_LAYER_KHRONOS_validation` + debug-utils
  messenger, also covering instance creation) is `auto` = on in debug builds when installed,
  forceable `on`/`off`. Missing layers produce a clear message with the install command.
  Validation errors are counted; the app exits with code 3 if any occurred (tests rely on it).
- **Device selection:** every device is evaluated (API ≥ 1.3, `VK_KHR_swapchain`,
  `dynamicRendering`, `synchronization2`, graphics+present queue); unsuitable devices are
  listed with reasons if nothing qualifies. Scoring: `preferred_gpu` substring match >
  discrete > integrated > others (CPU/llvmpipe last). `lantern --list-gpus` prints this.
- **Swapchain:** UNORM BGRA/RGBA (GS output is already display-encoded), FIFO by default,
  mailbox/immediate when supported (fallback to FIFO with a warning), `minImageCount + 1`.
  Recreated on `OUT_OF_DATE`, `SUBOPTIMAL`, resize, and present-mode change, passing
  `oldSwapchain`. Zero-sized (minimised) drawables skip frames without recreation.
- **Synchronisation:** 1–3 frames in flight; per-frame command pool, fence, and
  image-available semaphore; **per-swapchain-image** render-finished semaphore (presentation
  may still hold image *i* when frame slots wrap). The fence is reset only after a successful
  acquire, so an early return can't deadlock the next frame. Layout transitions and submits
  use `synchronization2`; rendering uses dynamic rendering (no render passes/framebuffers).
- **Cleanup:** `vkDeviceWaitIdle`, pipeline cache saved, then reverse-order destruction;
  partial init failures are cleaned up by the same path.
- **Aspect:** output is letterboxed to 4:3 by default (`aspect = "stretch"` to fill).
- **Pipeline cache:** persisted to `$XDG_CACHE_HOME/lantern/vk_pipeline_cache.bin`.

### 6.3 Placeholder ✅
A procedural magenta/black checkerboard with the word **PLACEHOLDER** (5×7 bitmap font in the
fragment shader) and the window title *"PLACEHOLDER OUTPUT (no game runtime loaded)"*. It is
the only image shown until real GS output exists. It's not game art and never stands in
for any.

## 7. Asset interfaces 🟡 (Stage 5: reader/ingest ✅, runtime VFS ⏳)

- `iso::Image` — read-only ISO9660 reader over a file handle (PS2 DVDs are ISO9660 + UDF
  bridge; ISO9660 suffices for file access). No writes; opened `O_RDONLY`.
- ✅ `lantern-ingest identify|list|extract` (`src/ingest`). `extract` defaults to the *boot set*
  (SYSTEM.CNF, boot ELF, `*.IRX`, `IOPRP*.IMG`, ~3.7 MB); `--all` copies everything.
- `ingest` — identify the disc: SHA-256 of the image, `SYSTEM.CNF` → boot ELF name and
  serial, region. Records a manifest; extracts to `$XDG_DATA_HOME/lantern/extracted/<serial>/`.
- `vfs::Disc` — runtime file access used by the CDVD HLE: serves sector reads/file opens from
  the original ISO (preferred: no duplication) or the extracted tree.
- Tests use **synthetic** ISO images generated in the test itself — never the real disc.

### 7.1 Findings on the owner's disc (2026-11-03, read-only)
| Item | Value |
|---|---|
| Serial / region | SLUS-20228, NTSC-U, `VER = 1.20`, volume `SILENT_HILL_2__VW047_U1__V1_20` |
| SHA-256 | `c2306f1bfd576469a5e5ebf99877f6ff260404ab78a6d7b71f9b3fe26aefdef5` (4,641,193,984 bytes) |
| Boot ELF | `SLUS_202.28`, 3.0 MB, ELF32 MIPS, flags `5900, eabi64, mips3`, entry `0x00100008` |
| Symbols | effectively none (3-entry symtab, unnamed sections): function discovery must be CFG-based |
| Layout | main image `0x00100000` (file 0x2E6880, mem 0x1E0BC00 incl. BSS) + **~50 zero-file-size overlay segments all at `0x01F0BC00`** |
| Overlays (hypothesis) | `GX/ GY/ GZ/*.BIN` (54 small files); several sizes equal overlay segment sizes. To verify in Stage 6. |
| Data | `DATA/*.MGF` archives (3.1 GB, incl. `MOVIE.MGF` 1.7 GB), `IOP/SOUND/SOUND.DAT` 298 MB, `DATA/DUMMY.DMY` 256 MB padding |
| IOP | `IOPRP.IMG` + 11 IRX: SIO2MAN, PADMAN, MCMAN, MCSERV, LIBSD, SDRDRV, SDSTR, SD_CD, SD_HD, SOUNDCD, SOUNDHD |

Implication: the recompiler must support **several code images sharing one address range**
(overlay-ID-qualified functions, dispatch keyed by the currently loaded overlay).

## 8. Recompilation & PS2 runtime ⏳ / ❓ (Stages 6–9)

The PS2's main CPU is the **Emotion Engine** (MIPS R5900: 64-bit GPRs with 128-bit
MMI/SIMD ops, COP1 FPU with non-IEEE behaviour, COP2 = VU0 macro mode), plus **VU0/VU1**
vector units with their own microcode, **GIF/VIF/DMAC** data paths, the **GS** rasteriser,
the **IOP** (R3000A running IRX modules: pad, CDVD, memcard, sound), **SPU2** audio and the
**IPU** MPEG-2 decoder (FMVs).

1. ✅ **Analysis** (Stage 6, findings in [ANALYSIS.md](ANALYSIS.md)): parse the boot ELF; recover functions (symbols if present, else
   control-flow discovery), syscalls used, IRX modules loaded, VU microcode blobs, jump
   tables. Output: a report + a TOML function/config map consumed by the recompiler.
2. 🟡 **EE recompiler** (Stage 7 — core done, see §8.1): per-function translation of R5900 code to C++ operating on a
   `Cpu` context (128-bit GPRs, HI/LO pairs, FPU with PS2 rounding/clamping), guest memory via
   a 32 MB RDRAM array + MMIO dispatch. Indirect jumps go through a dispatch table;
   un-translated targets trap with the guest PC (never silently skipped). Generated code lands
   outside the repo.
3. **HLE kernel + IOP** (Stage 7): the 60 EE syscalls the game uses, implemented natively.
   IOP (decision D-6.1): SCE modules HLE'd at the SIF RPC boundary (`padman` ←
   `InputSystem::sample()`, `cdvdfsv`/`fileio` ← `vfs::Disc`, memcard ← `memcards/`); the
   game's own Konami sound modules run on an R3000A interpreter with IOP-kernel HLE and
   register-level SPU2. **The BIOS is not used.** (`scph10000` is not needed and is not read.)
4. **GS/VU1 → Vulkan** (Stage 8): VU1 microcode interpreted (later recompiled) to produce GIF
   packets; a GS translator turns primitives + GS register state into Vulkan draws against an
   emulated local-memory model (4 MB VRAM, swizzled texture formats, CLUTs). This is where the
   renderer abstraction gains texture/target/batch APIs.
5. **SPU2 → SDL audio; IPU → FMV decode** (Stage 9).

### 8.1 Recompiler core (Stage 7) ✅

```
analysis (functions, jump tables) ──▶ emitter ──▶ C++ (one call per instruction into sem::)
                                                         │
ee/semantics.h  ◀── single source of truth ──▶ ee/interpreter (reference + fallback)
```

- **Shared semantics** (`src/ee/semantics.h`, `semantics_vu0.h`): every non-control-flow
  instruction is an inline `sem::OP(Cpu&, Memory&, word, pc)`. Recompiled code passes a constant
  `word`, so field decoding folds away; the interpreter calls the same functions. The op set is
  defined once (`semantics_ops.h`); a test asserts every decodable opcode is either implemented,
  control flow, or explicitly deferred (kernel-mode TLB/BC0/ERET).
- **Coverage on SLUS-20228:** 7,888 functions (main + 54 overlays), 514,628 instructions:
  integer, 64-bit, MMI (complete), FPU (PS2 clamping rules), VU0 macro mode (complete incl. flags,
  clip, Q/I/R, VU0 data memory). Remaining runtime traps: 12 `vcallms` (VU0 micro mode — Stage 8),
  1 `jalr` with a non-`$ra` link, 1 invalid word on a reached path.
- **Control flow:** branch conditions are evaluated before the delay slot; likely branches only run
  the slot when taken; `jal` to a known function is a direct C++ call checked against the expected
  return address (mismatch → loud trap); `jalr`/unknown targets go through `recomp::Runtime`
  (address → function map, overlay-aware, optional interpreter fallback); resolved jump tables
  become `switch` statements with a trapping default.
- **Verification:** 66 synthetic programs (2 hand-written + 64 random covering ALU/MMI/FPU/memory,
  likely branches, loops, calls) are recompiled at build time and must match the interpreter
  bit-for-bit (all GPRs/HI/LO/FPU/memory). A mutation check confirmed the test detects emitter bugs.
- **Real output:** `lantern-recomp` writes the game's C++ + a CMake project to
  `$XDG_DATA_HOME/lantern/recomp/SLUS-20228/` (never in git); it compiles to a ~23 MB static
  library in ~35 s on 16 cores. Generated code must be built with `-ffp-contract=off` so float
  results match the interpreter.
- **Not yet:** the runtime cannot *run* the game — that needs the HLE kernel (threads, interrupts,
  SIF/IOP), DMA/GS/VU1 and the ELF loader/boot path (next stages). Thread switching will require
  host-side coroutines/threads because recompiled code uses the host stack.

## 9. Known limitations / risks

- **No game runs yet.** Stages 1–7 core are complete (platform, input, config, Vulkan, ingest,
  analysis, recompiler); the HLE kernel, IOP, VU1/GS and audio remain.
- FPU/VU float behaviour approximates PS2 rules (clamp instead of inf/NaN, flush denormals); exact
  rounding differences vs. hardware are not yet verified against a reference.
- SH2 relies heavily on VU1 microcode and GS behaviours (fog, effects, framebuffer feedback);
  high accuracy GS translation is the largest technical risk.
- Timing-sensitive code (VSync interrupts, DMA completion) needs an event scheduler; behaviour
  differences may surface as hangs, not crashes.
- Only Vulkan 1.3 GPUs with dynamic rendering + sync2 are supported (Mesa RADV/ANV, NVIDIA
  ≥ 5xx drivers). The developer machine: RX 7600M XT (RADV) — passes all tests.
- Wayland: SDL3 picks X11 or Wayland automatically; tested so far on X11 (Xfce).
- `ASAN_OPTIONS=detect_leaks=0` is set for windowed smoke tests because libX11/SDL/ICD
  drivers leak at exit; unit tests keep leak detection on.
- No license chosen yet (owner decision).
