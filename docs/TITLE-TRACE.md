# Title-screen input trace

How the "title screen ignores Start" question was answered, and what it found. Method only needs
the shipping game and LANTERN's own tooling; no emulator sources were consulted.

## Instrumentation used (all in-tree, off by default)

| Knob | What it does |
|---|---|
| `--frames N` | stop after N guest frames (VBlanks) |
| hot guest PCs | sampled at safe points; printed in the boot report |
| per-thread `pc` | printed per thread in the boot report |
| pad buffer reads | every guest access to the registered pad area is counted; `--watch 1` logs the first ones with the accessing PC and `$ra` |
| `pad-walk` | on the first pad read after frame 900, dumps the guest call chain (stack walk) and the game's input-gate bytes |

## Findings

1. **Thread 1 is not stuck.** Its hot PC is `0x001F9608`, in a loop that writes `4` to `INTC_STAT`
   (0x1000F000) and spins until bit 2 is set again — `sceGsSyncV`, i.e. waiting for the next VBlank.
   Four other threads are ready; no deadlock.
2. **The pad is polled every frame at the title.** The call chain captured at the title is:
   `0x001C6F00 → 0x001902C0 → 0x00190274 → 0x0018FA6C → 0x0018FB04 → 0x002100A0` (libpad internals).
   Guest reads of the pad buffer: ~18k by frame 1200, ~68k by frame 2600 — steady, not a one-off.
3. **The input gate is satisfied.** The game's handler at `0x0018FA90` reads the report, then:

   ```
   18FAFC: jal 0x210050        # padRead -> length in v0
   18FB04: sb  v0, 32(s4)      # ctx[32] = report length
   18FB08: lbu v0, 33(s4)      # gate A
   18FB0C: beq v0, zero, skip  #   must be non-zero
   18FB14: lbu v0, 43(s4)      # gate B
   18FB18: andi v0, 0x10       #   bit 4 must be set
   18FB1C: beq v0, zero, skip
   18FB30: jal 0x2106A0        # real input processing
   18FB34: sb  zero, 33(s4)    # gate A is cleared after processing
   ```

   Sampled at the title (`ctx = *(u32*)(0x2D26E0 + port*16 + slot*4)`):

   | port | length | gate A | gate B | verdict |
   |---|---|---|---|---|
   | 0 (pad) | 20 | 0x01 | 0x11 (bit 4 set) | processed |
   | 1 (empty) | 20 | 0x00 | 0x00 | skipped (correct) |

## Conclusion (superseded — see "Resolution")

Input reaches the game and is processed on every frame at the title screen. The title→menu
transition therefore depended on something downstream of input. The sound system was the leading
hypothesis at the time; it turned out to be wrong (the real sound modules made no difference).

## Resolution

The blocker was in the recompiler runtime's handling of **continuations**, not in any service.

1. With Start pressed, the EE main thread (thread 1) became *dormant* although nothing called
   `ExitThread`/`TerminateThread`: its top-level guest function had *returned*, and the kernel
   trampoline treats a return as thread exit. Without a press, thread 1 kept running and the
   report showed 0 non-standard returns; with a press it showed 7.
2. The return came from the game's quicksort at `0x00174F..0x175060`, which keeps its own return
   stack in `s6`: the continuation at `0x00175048` does `lwu ra,12(s6) … jr ra; addiu s6,s6,16`,
   so it repeatedly "returns" to itself until the stack yields the real caller.
3. `Runtime::jump` used *call* semantics (interpret until `pc == $ra`). At that continuation `$ra`
   already equals `0x175048`, so the interpreter stopped before executing a single instruction and
   reported a return to `0x175048`. Each host frame then saw a mismatch and unwound, until the
   thread's top-level call returned and the trampoline ended the thread.

Fixes (LAN-39):

- `Interpreter::run_until_return` runs a continuation until it returns with `jr $ra` and reports
  where it went; `Runtime::jump` uses it for anything that is not a recompiled function start.
- `Kernel::run_to_sentinel` keeps following continuations at the top level of threads, interrupt
  handlers and custom syscalls until control really returns to the kernel sentinel.

Result: pressing Start at the title now opens the difficulty menu (Action Level / Riddle Level).
A unit test reproduces the manual-return-stack pattern.

## Practical note: rebuild the generated library

`liblantern_game.so` inlines LANTERN's headers (`ee/semantics.h`, `ee/memory.h`). After changing any
of those headers, **rebuild the generated library** (`cmake --build ~/.local/share/lantern/recomp/<serial>/build`)
before running `lantern-boot`; otherwise the host and the game library disagree on object layout and
the run crashes non-deterministically.
