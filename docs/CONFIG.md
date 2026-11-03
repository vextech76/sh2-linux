# Configuration

File: `$XDG_CONFIG_HOME/lantern/config.toml` (default `~/.config/lantern/config.toml`),
created with defaults on first run. Override with `--config PATH`. Regenerate defaults with
`lantern --write-default-config`; inspect the effective config with `lantern --print-config`.

Errors (bad syntax/types/enum values) are logged and the affected value keeps its default.
Warnings: unknown keys, out-of-range numbers (clamped), binding problems.

```toml
log_level = "info"                 # debug | info | warn | error

[graphics]
backend = "vulkan"                 # vulkan | null (headless)
present_mode = "fifo"              # fifo (vsync) | mailbox | immediate; unsupported -> fifo
window_mode = "windowed"           # windowed | borderless | exclusive
width = 1280                       # 320..16384 window size
height = 960                       # 240..16384
resolution_scale = 2               # 1..8, internal GS render scale (used once GS output exists)
frames_in_flight = 2               # 1..3
aspect = "4:3"                     # 4:3 (letterbox) | stretch
validation = "auto"                # auto (debug builds) | on | off
preferred_gpu = ""                 # substring of device name, e.g. "7600M"; see --list-gpus

[game]
iso_path = ""                      # your disc image; opened read-only, never modified

[input]
button_threshold = 0.5             # analog amount that presses a digital pad button
mouse_sensitivity = 0.05           # stick deflection per pixel of captured mouse motion per frame
invert_mouse_y = false

[input.deadzone]
left_inner = 0.15
left_outer = 0.95
right_inner = 0.15
right_outer = 0.95
trigger_inner = 0.05
trigger_outer = 1.0

[input.keyboard_mouse]             # see docs/INPUT.md
cross = ["Space", "mouse_left"]
# ...

[input.gamepad]
cross = ["south"]
# ...
```

Writable locations (all outside game data; startup refuses otherwise):

| Path | Contents |
|---|---|
| `$XDG_CONFIG_HOME/lantern/` | `config.toml` |
| `$XDG_DATA_HOME/lantern/` | `extracted/`, `recomp/`, `memcards/` (later stages) |
| `$XDG_CACHE_HOME/lantern/` | `vk_pipeline_cache.bin` |
