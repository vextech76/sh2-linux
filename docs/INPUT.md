# Input bindings

The game sees one virtual **DualShock 2**. Bindings map host inputs onto its buttons/sticks.
What each pad button *does* depends on the game's own control settings; these defaults are
a pad layout, not a claim about in-game actions.

## Targets (left-hand side in config)

| Group | Names |
|---|---|
| Face | `cross` `circle` `square` `triangle` |
| Shoulder | `l1` `r1` `l2` `r2` |
| Stick clicks | `l3` `r3` |
| System | `start` `select` |
| D-pad | `dpad_up` `dpad_down` `dpad_left` `dpad_right` |
| Sticks | `left_stick_up/down/left/right`, `right_stick_up/down/left/right` |
| Host | `toggle_fullscreen` `toggle_mouse_capture` `quit` |

## Sources (right-hand side)

**`[input.keyboard_mouse]`** — keys (case-insensitive): `A`–`Z`, `0`–`9`, `F1`–`F12`,
`Space` `Return` `Escape` `Backspace` `Tab` `Up` `Down` `Left` `Right` `LeftShift`
`RightShift` `LeftCtrl` `RightCtrl` `LeftAlt` `RightAlt` `LeftGui` `RightGui` `Minus`
`Equals` `LeftBracket` `RightBracket` `Backslash` `Semicolon` `Apostrophe` `Grave` `Comma`
`Period` `Slash` `CapsLock` `Insert` `Delete` `Home` `End` `PageUp` `PageDown`
`PrintScreen` `ScrollLock` `Pause` `NumLock` `KP_0`–`KP_9` `KP_Enter` `KP_Plus`
`KP_Minus` `KP_Multiply` `KP_Divide` `KP_Period`, or `KeyNNN` (raw USB HID usage).
Mouse: `mouse_left` `mouse_middle` `mouse_right` `mouse_x1` `mouse_x2`, motion
`mouse_x+` `mouse_x-` `mouse_y+` `mouse_y-` (only while the mouse is captured).

**`[input.gamepad]`** — positional names (south = bottom face button on any brand):
`south` `east` `west` `north` `back` `guide` `start` `left_stick` `right_stick`
`left_shoulder` `right_shoulder` `dpad_up` `dpad_down` `dpad_left` `dpad_right` `misc1`
`left_paddle1` `left_paddle2` `right_paddle1` `right_paddle2` `touchpad`, axes
`left_x+` `left_x-` `left_y+` `left_y-` `right_x±` `right_y±` (`+y` = down),
`left_trigger` `right_trigger`.

## Defaults

| Pad | Keyboard / mouse | Gamepad |
|---|---|---|
| cross | Space, mouse_left | south |
| circle | Q, Backspace | east |
| square | LeftShift | west |
| triangle | F | north |
| l1 / r1 | C / R, mouse_right | left/right_shoulder |
| l2 / r2 | Z / X | left/right_trigger |
| l3 / r3 | LeftCtrl / mouse_middle | left/right_stick |
| start / select | Escape, Return / Tab | start / back |
| d-pad | arrow keys | d-pad |
| left stick | W A S D | left stick |
| right stick | I J K L | right stick |
| toggle_fullscreen | F11 | — |
| toggle_mouse_capture | F10 | — |

Mouse motion is unbound by default; to drive the right stick with the mouse:

```toml
[input.keyboard_mouse]
right_stick_left = ["J", "mouse_x-"]
right_stick_right = ["L", "mouse_x+"]
```

## Rules

- Listing a target replaces its default sources; `[]` unbinds it. Unlisted targets keep defaults.
- One source drives at most one target per profile. Re-using a source moves it and logs a warning.
- Gamepad sources are rejected in `[input.keyboard_mouse]` and vice versa (warning).
- Dead zones (`[input.deadzone]`): sticks are radial (`*_inner` → 0, `*_outer` → full);
  triggers 1-D. Analog sources press digital pad buttons at `input.button_threshold`.
- Hot-plug: the first connected controller is active; unplugging it hands control to the next.
