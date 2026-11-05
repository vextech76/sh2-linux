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

### 8.2 Boot path, HLE kernel, SIF (Stage 7b) 🟡

`lantern-boot` loads the boot ELF into guest RAM, `dlopen`s the recompiled game
(`liblantern_game.so`, built outside git), and runs it on `ps2::Kernel`:

- **Threads** are host coroutines (`ucontext`, 16 MB lazily-committed host stacks) because
  recompiled code runs on the host stack; the guest register file is swapped on each switch
  (VU0 is global hardware and is not). Priority scheduling; switches at syscalls and at
  **loop back-edge safe points** (`rt.tick()` emitted by the recompiler), so busy-wait loops on
  interrupt-set flags make progress. ASan fiber annotations keep sanitizer builds clean.
- **Interrupts/alarms**: VBlank at 59.94 Hz (INTC 2/3 handlers, `SetVSyncFlag`), alarms in HSYNC
  units, DMAC completion handlers; delivered at safe points on a dedicated guest stack in the
  unused kernel area; `i*` syscalls from handlers defer rescheduling.
- **Kernel patches**: SCE libkernel's `SetSyscall` patches into kernel memory are ignored (they
  edit BIOS-internal structures LANTERN doesn't have); user-space handlers are honoured.
- **Hardware**: timers (counting from host time), INTC/DMAC status semantics, GS CSR field bit,
  SIF registers (SMFLG write-1-to-clear, IOP reboot re-raises boot flags). DMA to unemulated
  units completes instantly and is logged as PLACEHOLDER; other registers are stored and listed
  in the boot report.
- **SIF HLE** (`src/iop/sif_hle.*`): modelled IOP RAM for EE→IOP DMA, `INIT_CMD`/`SET_SREG`/
  `RESET`/`RPC_BIND`/`RPC_CALL`/`RPC_RDATA`, replies delivered one packet per SIF0 DMA interrupt
  into the game's receive buffer so its own libsif handler processes them. RPC servers are
  pluggable; unknown servers answer with zeros and are reported as PLACEHOLDER.

**IOP services (`src/iop/services.*`)**, protocols per ps2sdk's EE clients and verified on the
wire: CDVDFSV init / S-commands / N-commands (sector reads + streaming from the read-only ISO) /
search-file (both the ps2sdk 292-byte and SCE 296-byte packet layouts) / disk-ready / power-off;
LOADFILE (module loads acknowledged and recorded; `GET_VERSION` reports "2240", the release
SLUS-20228's SCE client checks for); IOP heap (bump allocator over modelled IOP RAM).

**Boot status (SLUS-20228):** crt0 → kernel services → SIF → CDVD → IOP reset (IOPRP) →
9 IOP modules loaded → all `DATA/*.MGF` archives located → main loop: per-frame VIF1 DMA
(~3/frame), scratchpad DMA, GS privileged-register writes, `SetGsCrt` interlaced NTSC; pad
(0x80000100/101), memory card (0x80000400), SDR sound (0x80000701) and the Konami sound driver
(0x15) are bound and answered as PLACEHOLDER. **Next boundary: GIF/VIF/VU1/GS (Stage 8).**

### 8.3 Graphics path (Stage 8a/8b) 🟡

```
EE ──DMA ch1──▶ VIF1 ──DIRECT──▶ GIF PATH2 ─┐
   ──DMA ch2──────────────────▶ GIF PATH3 ─┼─▶ GS registers ─▶ software rasterizer ─▶ local memory (4 MB)
   ──DMA ch8/9 (scratchpad copies)          │                                              │
                     VIF1 UNPACK/MPG ─▶ VU1 memory (interpreter: Stage 8c)                   ▼
                                                                    display readout (PMODE/DISPFBn/DISPLAYn)
```

- **DMAC** (`src/ps2/vif.*`): normal and source-chain modes (refe/cnt/next/ref/refs/call/ret/end,
  2-level call stack, TTE tag words to VIF, IRQ-tag stop), scratchpad transfers; executed
  synchronously on `CHCR.STR`.
- **VIF1**: all codes decoded; DIRECT/DIRECTHL → GIF PATH2; UNPACK (all vn/vl incl. V4-5, CL/WL
  skipping/filling, offset/difference modes, masks, TOPS double buffering) and MPG into VU1
  memory. MSCAL/MSCNT are counted until the VU1 interpreter exists.
- **GIF** (`src/gs/gs.*`): PACKED (all descriptors incl. ADC), REGLIST, IMAGE; packets span DMAs.
- **GS**: general registers, host→local and local→local transfers, CLUT loading (CLD modes,
  CSM1, CSA), FINISH/SIGNAL → CSR + INTC 0, privileged registers, display composition including
  interlaced frame mode.
- **Local memory** (`src/gs/gs_memory.cpp`): the layout is expressed as *page geometry + block
  arrangement + in-block bit order* (offset bit *i* is fed by coordinate bit x*k*/y*k*).
  - Block arrangement and page geometry: documented GS organisation; unchanged from the version
    that renders the game's 32-bit font pixel-exactly.
  - CT32 and CT16 orders were derived from the two swizzle tables that are demonstrably correct on
    screen; a unit test checks the generated tables reproduce them exactly, and the CT16 order is
    exactly the CT32 order with the extra x bit below x0 (the rule used to extend to finer
    formats).
  - PSMT8/PSMT4 in-block order is **not** published in the freely available public documentation
    (ps2tek documents the GS registers, GIF/VIF paths and IOP, but not these tables), so it was
    identified **by measurement against the game itself** (no emulator source used or consulted):
    each candidate order was rendered and compared (`--swizzle-variant T8[,T4]`, dump frames,
    inspect). T8: the order `y3,x3,x0,y0,x1,x2,y1,y2` makes the game's title logo legible while
    the three alternatives produce noise — decisive.
  - **T4 status: derived, not yet verified.** It follows the same rule (extras x4,x3,y3 below the
    base) but no screen has yet exercised a 4-bit texture where candidate orders differ visibly;
    the sweep harness is retained for verification.
- **Software rasterizer** (`src/gs/rasterizer.*`): points, lines, triangles/strips/fans
  (top-left fill rule), sprites; flat/Gouraud; STQ/UV texturing with nearest or bilinear
  (TEX1.MMAG) sampling, four wrap modes, CT/indexed+CLUT formats, TEXA; MODULATE/DECAL/
  HIGHLIGHT(2); fog; alpha test with AFAIL modes; depth test; blending incl. PABE, COLCLAMP, FBA;
  FBMSK. Not yet: mipmapping/minification filters, DATE, dithering, scan-mask, anti-aliasing.
- **First output (2026-11-03):** `lantern-boot --dump-frames DIR` writes PNGs of the displayed
  buffer. SLUS-20228 renders its memory-card warning screen correctly (it reports no card,
  because the memory-card server is still a placeholder). Frames are game output and are never
  committed.
- Observed: at this point the game draws exclusively via VIF1 DIRECT (PATH2) and GIF PATH3; no
  VU1 microcode has been uploaded yet.

### 8.4 Presentation and pad input (Stage 8d, LAN-19/20) ✅

- `Renderer::set_frame_image()` / `draw_frame_image()`: the GS display is uploaded through a
  per-frame staging buffer into a sampled texture (recreated on size change, layout barriers via
  synchronization2) and drawn letterboxed 4:3 with linear upscaling. GPU test
  `gpu_vulkan_image_upload` runs it under validation with a mid-run resize.
- `lantern-boot --window`: SDL3 window + Vulkan, input via `InputSystem` (user config bindings),
  F11 fullscreen; closing the window ends the guest. Presentation happens at guest VBlank, so FIFO
  vsync paces the emulation.
- `PadService` (0x80000100/101, "new" libpad protocol per ps2sdk): INIT/OPEN/... and a per-VBlank
  refresh of the 2x128-byte `pad_data_new` buffers with a DualShock 2 (0x79) report from the
  virtual pad (port 0; port 1 reports disconnected).
- `--press FRAME:button` scripts pad input for automated checks. Verified on SLUS-20228: Left
  selects "Yes" on the memory-card prompt, Cross confirms, the game proceeds to the Konami logo
  and the SILENT HILL 2 title screen.

**Title screen now renders correctly** with the measured PSMT8 order (§8.3): the logo is legible
and the remaining grain is the game's own background effect.

**Recompiler fix for guest continuations:** SH2 sets `$ra` and jumps to create continuations (its
quicksort does). Recompiled calls now *follow* a non-standard return address instead of trapping
(`Runtime::nonstandard_returns` counts them; reported in the boot log). Before the fix the game
trapped at 0x00175188 as soon as it left the title screen.

**`--turbo`** advances one game frame per VBlank for automated checks (~11,000 frames/s). It
changes guest timing enough that boot stalls early in the module-load phase, so paced mode is used
for behaviour checks; the investigation of that divergence is tracked (LAN-26).

### 8.5 Memory card (LAN-22) ✅ (info path) / 🟡 (file data)

Implemented against the documented mcserv RPC protocol (ps2sdk's `iop/memorycard/mcserv` server
and `common/include/libmc-common.h` structures):

- Argument buffer `mcDescParam_t` (48 bytes: fd, port, slot, size, offset, origin, buffer, param).
- Reply `mcRpcStat_t` (12 bytes: result, mcserv_version, mcman_version). INIT (0xFE) reports the
  feature levels of the disc's own modules (MCSERV 2.13, MCMAN 2.23); libmc rejects a card whose
  server reports levels below 0x205/0x206.
- **Card state arrives out-of-band:** GETINFO (0x01, or 0x78 classic) writes an `mcEndParam2_t` /
  `mcEndParam_t` to the EE address passed in `param` — `{type, free, ..., formatted at 144}`. This
  was the reason the game kept saying "not inserted": type 0 in that struct means no card, and it
  had never been written.
- `MemoryCard`: 8 MB card at `$XDG_DATA_HOME/lantern/memcards/card1.ps2`, real superblock/FAT/root
  directory; created and formatted on first use.
- OPEN/CLOSE/SEEK/READ/WRITE/GETDIR/FORMAT are dispatched (file *data* is not modelled yet); every
  unmodelled command is logged once.

**Verified:** SLUS-20228 no longer shows the memory-card prompt: it queries GETINFO 32 times, sees
a formatted PS2 card with 8184 KB free, and boots straight to the Konami logo and title screen.

**Guest-side trace of the title screen (LAN-27):** see `docs/TITLE-TRACE.md` for the method and
the disassembly. Short version: thread 1's hot loop is `sceGsSyncV` (write 4 to INTC_STAT at
0x1000F000, spin until bit 2 sets again — i.e. wait for the next VBlank), the game polls the pad
every frame through libpad, and the input gate in the game's own handler (0x0018FA90) is
**satisfied** on port 0: report length 20, `ctx[33] = 0x01`, `ctx[43] = 0x11` (bit 4 set). So input
is delivered and processed; the title→menu transition is gated downstream of input, and the
remaining dependency is almost certainly the sound system (Stage 9).

### 9.1 IOP CPU core (Stage 9, first step) ✅

Decision D-6.1 needs an R3000A to run the game's own sound modules. Implemented so far:

- `iop/r3000.*`: MIPS I decoder and disassembler for the IOP (no 64-bit, MMI, FPU or coprocessor
  data transfers — the IOP has none of these). Reserved fields must be zero, which keeps data from
  decoding as instructions. Verified against **68 golden encodings assembled by GNU binutils**
  (`-march=r3000`, disassembled with `-m mips:3000`), including branch/jump text at the real
  addresses.
- `iop/iop_cpu.h`: 32-bit register file, HI/LO, COP0, 2 MB RAM with kuseg/kseg0/kseg1 mapping,
  1 KB scratchpad, MMIO hook.
- `iop/iop_interp.*`: interpreter with **load delay slots** (the instruction after a load still
  sees the old value; back-to-back loads keep only the last — hardware behaviour), branch delay
  slots, link written before the slot runs, MIPS I DIV/DIVU-by-zero results, address-error traps
  and an explicit trap for anything unimplemented. A syscall hook is in place for the IOP kernel.
- Unit tests: golden encodings, load-delay behaviour (including the back-to-back case), delay slot
  plus `jal` link ordering, HI/LO arithmetic, traps, and the absence of EE-only instructions.

### 9.2 IRX loading, HLE dispatch and the module probe ✅

- `iop/irx.*` loads PS2 IOP modules into the 2 MiB IOP RAM: `PT_LOAD` copy, `.bss` zeroing,
  `R_MIPS_32/26/HI16/LO16` relocations, import/export table discovery, and patching of each import
  stub with `j handler; nop`. Measurements from the shipped modules (all relocations against
  symbol 0, `r_offset` already virtual, export entries relocated by `R_MIPS_32`, entry point 0
  is a real initialiser) are recorded in `docs/IOP.md`.
- `iop/hle.*` dispatches host handlers through a 12-byte trampoline slot (`syscall; jr ra; nop`),
  so HLE functions use the normal IOP calling convention without inventing guest code.
- `lantern-iop` loads every `.IRX` under the extracted directory, resolves inter-module imports
  (LIBSD, sd_cd, sdstr, …), reports the remaining kernel surface, and runs module initialisers and
  the thread bodies they start on the R3000A.
- Result: the Konami sound driver (`SOUNDCD`) initialises, creates its priority-47 thread, sets up
  SPU2 and SIF/RPC, and starts a priority-29 worker that performs real SPU2 sample work. The
  measured register surface is in `docs/IOP.md`.

### 9.3 IOP scheduler ✅

- `iop/iop_threads.*`: priority scheduler over complete per-thread `IopCpu` states, with blocking
  `SleepThread`/`WaitSema`/`DelayThread`/vblank waits, a virtual 36.864 MHz clock, a 4096-instruction
  quantum and stable thread storage (handlers may create threads mid-step).
- `lantern-iop` now runs the module initialiser and every started thread concurrently. MCSERV's
  wait on semaphore 0 is reported as a deadlock, which is the truthful result without an EE SIF
  command; SOUNDCD runs its full nine-thread set (finished, sleeping, vblank, running, ready and
  dormant threads at once).
- Unit tests cover delays, priorities, semaphore hand-off, vblank wakeups, traps and dormant-thread
  deadlock.

### 9.4 Real IOP runtime and sound RPC bridge ✅

- `iop_runtime.*` packages RAM, interpreter, scheduler, kernel HLE and the game's modules. Boot
  loads LIBSD/SD_CD/SDSTR/SOUNDCD, runs their initialisers and threads between EE frames, and
  shares RAM with `SifHle`.
- `SifHle` forwards RPC calls for SIDs registered by real modules to their guest server functions;
  thbase alarms and timrman handlers are scheduled on the virtual clock and run as guest threads.
  A 2,100-frame run served 1,970 real sound RPC calls with zero failures.
- After Start the EE main thread terminates, but the game does not yet load `MENU.MGF`; the display
  remains on the title. The remaining suspects are SPU2/DMA completion and VU1/IPU.

### 9.5 SPU2 core ✅

- `spu2.*`: two cores, 24 voices each, register file, key on/off, ENDX/STATX, master volumes,
  transfer address/data, 2 MiB sample RAM, PS-ADPCM decoding, loop/end handling and a 48 kHz
  stereo mixer.
- IOP DMA channels 0/1 copy blocks between IOP RAM and SPU2 RAM and raise the SPU2 DMA IRQs;
  `intrman` handlers are real and run on those IRQ lines (as guest callbacks, not a CPU vector).
- Unit tests cover register readback, ADPCM playback, key-on/off ENDX + IRQ, loop end and the
  transfer data port.

Not yet: routing PCM to a host audio device, a real interrupt context, VU1/IPU execution, and the
remaining title→menu blocker (the EE loader still waits on semaphores after Start). LAN-38..LAN-40.

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
