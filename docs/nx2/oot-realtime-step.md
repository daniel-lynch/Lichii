# Ocarina of Time: real-time step (opt-in)

Title `0400486024DEC000`, main build id `E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348` (the build the
built-in Max Resolution patch pins). Off by default.

## Why

The game advances a fixed 1/60 s per presented frame: one update per present, no accumulator,
no catch-up, and nothing in the game rescales the step when the present rate drops. On hardware
that only shows up as brief half-speed hitches; under emulation at ~39 presents/s it means the
whole game (animation, physics, timers, cutscenes) runs at 39/60 = 0.65x real time, while
streamed music keeps real time.

## What it does

With `EDEN_NX2_REALTIME_STEP=1` (or the setting `oot_realtime_step`, `[Core]`, per game; the
environment variable wins when set, `0` forces it off), every nx2:vi2 present
(`INx2Vi2Session::PresentClientBuffer`, after vblank pacing and before the reply) feeds
`Service::Nx2RealtimeStep::StepController` (src/core/hle/service/nx2_realtime_step.h):

- present interval EMA, time constant 0.5 s; each sample clamped to [1/60, 1/20] s so a load
  stall can't drag the estimate below 20 fps;
- fps = 1 / EMA, clamped to [20, 60]; f = 60 / fps, clamped to [1, 3];
- 20 presents of warm-up before the first write; then a write only when f moves by more than
  2 % from the last value written, or when it reaches a clamp (1 or 3) exactly; f within 2 % of 1
  (above ~58.8 presents/s) counts as 1, so a held 60 with vblank jitter writes nothing.

The time object is the pointer at `main + 0x4E94B20` (read on every write; skipped while null).
Before writing, the object must still hold its constructor constants +0x14 = 1.0f and
+0x1c = 60 (a guard against a wrong pointer). Then, through the process's `Memory::WriteBlock` /
`Write32` (the same guest-memory path as the cheat engine's writes):

| offset | written | game's meaning |
|---|---|---|
| +0x00..+0x0c | {f, 1/f, f/60, 60/f} | main-thread block (getter 710111f450 reads +0x00/+0x08) |
| +0x10 | f | frames-per-step multiplier (136 direct readers) |
| +0x18 | f/60 | step in seconds (40 direct readers) |

+0x14 and +0x1c are left alone. f/60 is computed as f × 0.016666668f, so f = 1 restores the
constructor's bits exactly (0x3C888889).

Logs: one Info line on engagement (title, main base, object, first f), a Debug line on each
change, an Info line on restore. A Warning if the title matches but the build id does not.

Restore: when the switch reads off at a present (the setting changed at runtime) the f = 1
block is written back on that present; the vi2 session destructor does the same if the process
it wrote is still the running application. The game never rewrites these fields, so without a
restore a runtime off would keep the last f.

### Never below f = 1, and why that expects the frame limiter

f is never below 1.0 (f = max(1.0, 60 / fps), via the fps clamp at 60 and the f clamp at 1). Above
60 presents/s the answer is the emulator's 60 fps frame limiter / vblank pacing, not a sub-1 step:
the feature expects the limiter on. With the limiter off and the guest presenting faster than 60,
f stays 1.0 and the game runs fast, exactly as it would on hardware without vsync.

### A bridge, not a fix

This compensates for dips below 60. Once the dips are fixed and 60 is held, the measured rate is
60, f is 1.0, nothing is written (the hysteresis keeps the constructor's values) and the feature
is a no-op.

## Why the present path, not the cheat engine tick

- The cheat engine's tick is a 12 Hz CoreTiming event (`CHEAT_ENGINE_NS` = 83 ms) that knows
  nothing about presents; the EMA needs present timestamps. The present handler sees every
  present.
- The cheat engine only exists when a cheat list was loaded for the build; this feature must not
  depend on cheats being installed.
- The present handler runs on the game's own present thread with the owning process at hand,
  and writes before the reply, so the next frame steps by the new f. The writes are data stores
  through the same `Core::Memory::Memory` the cheat engine uses; no instruction-cache
  invalidation is needed (data pages).

## Why poke fields rather than patch code

- Getter 710111f450 only covers the +0x00/+0x08 readers. +0x10 and +0x18 are read directly by
  ~176 sites, so patching the getter alone would leave most of the game at 1/60.
- The per-core writer 710071a090 (and its inlined copies in six job loops) derives the per-core
  blocks from +0x10 × hit-stop scales; with +0x10 = f it already does the right thing, so it
  needs no patch.
- Fields need no code-size or branch room and are gated by a value check; nothing in the game
  writes them after construction (static scan: every reference to the cell and its GOT entry).

## Input repeat factor: not written (and why)

`DAT_7104d41fc8` (main + 0x4D41FC8, the button hold/repeat factor) is recomputed by the frame
entry 7103b4ea90 at the start of every frame as 60 / (controller target fps), before calc and
the button routines run. A write at the present is overwritten before any reader sees it, so
the "second switch" is not implemented. Consequence: with the controller frozen at 60 the factor
is 1.0 and menu/hold repeat counts presents, i.e. runs at ~0.65x real time at 39 fps while the
rest of the game runs real time. Making it real time would need a code patch at the writer
(7103b4eaf0) to multiply by f; not done.

## Recommended companions

Keep the Force-60 and "freeze controller" cheats on. They hold the present interval at 1 and the
controller target at 60, so the game's fps controller doesn't change the interval underneath
the measurement (and the input factor stays 1.0 instead of 2.0).

## Risks

- **Physics.** The physics world (Havok/phive) steps f/60 s per frame: ~1/39 s at 39 fps instead
  of 1/60. Larger steps can change contact resolution, tunnelling through thin geometry and
  spring/damping stability. The engine has frame-rate-aware code (safe-delta-time fields,
  `powf(k, f)` damping), so it was built for f ≠ 1, but this game never ran it that way on
  hardware. f is capped at 3 (20 fps).
- **Frame counters compared with `== N`.** Event code that counts frames (event frame numbers,
  fades) may add f per frame and test for equality; with non-integer f such a test can be
  skipped. Not observed yet; the symptom would be an event or fade that never ends.
- **Hit-stop stacks.** Per-core blocks (+0x20 + core × 0x30) are re-derived from +0x10 on the
  next hit-stop push; a pop (710071c900) may restore a block saved before f changed, so a worker
  thread could run one hit-stop with the previous f. Only while a per-core override is active
  (+0x1c4 mask).
- **Torn frame.** The block is written from the present thread while the main thread may be in
  calc; one frame can read a mix of the old and new f (≤ 2 % apart by the hysteresis).
- **Input repeat** stays frame-based (above).
- **Wrong build.** Offsets are gated by title + main build id + the object's constructor
  constants; any other build is left alone (Warning logged).
