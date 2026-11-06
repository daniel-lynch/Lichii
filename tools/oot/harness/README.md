# Ocarina of Time test harness

Unattended, repeatable runs of Ocarina of Time on Lichii: boot, title, load the newest save,
walk forward, then turn the camera through four ~90° headings with a screenshot and an fps sample
at each, then a clean shutdown and a summary.

    python3 -I tools/oot/harness/run.py --headless --duration 300
    python3 -I tools/oot/harness/run.py --binary ~/dev/Lichii/build/bin/lichii --headless   # A/B
    python3 -I tools/oot/harness/run.py --env EDEN_NX2_COMPUTE_WRITE_MARK=0 --headless       # knob A/B
    python3 -I tools/oot/harness/analyze.py ~/Downloads/OOT/harness-runs/<stamp>            # re-summarise

Drop `--headless` to watch it in a nested gamescope window on the desktop. Exit status: 0 full
run, 2 a step failed (the run stops there, with a `FAIL-*` / `last-state` screenshot), 3 refused
to start (another emulator instance, or under 40 GiB available).

## What it never touches

The emulator runs with its working directory in `~/Downloads/OOT/harness-profile`, whose `user/`
directory puts the Yuzu-lineage path code in portable mode. `portable.py` builds it by **copying**
`~/.config/eden` (qt-config.ini + the per-game ini), `~/.local/share/eden` (NAND, save) and
`~/.cache/eden/shader` (warm pipeline caches); `--refresh-profile` recopies them. Every run
re-copies the seed config and save into `user/` and patches only the copies: absolute paths
re-pointed into the profile, player 0 mapped to the virtual pad, no stop confirmation, no update
check, and the per-game pins (GPU Mode Fast, reactive flushing off, async shaders, preload all
pipelines, no eviction; `--pin key=value` adds more). The run checks mtimes of the live files
before and after and records any change in `run.json` (`live_files_changed`).

`--cold-shader-cache` runs from a private copy of the profile in `<run>/profile` whose
`user/cache/shader` is empty: no pipeline cache and no translation cache, so every pipeline is
built at run time. The shared profile is only read. The driver's own cache (`~/.cache/nvidia`)
stays warm. `--cold-shader-seed FILE` (repeatable) copies one file into that empty directory, under its
parent directory's name (the title id): for example only the shader AOT recipe file of an earlier
run, to measure a session that starts with the recipes but no translations.

## How it works

- **Input**: `pad.py` creates an Xbox-layout uinput gamepad (vendor 1209, product 0e0d) before
  launch. SDL3 numbers its buttons/axes in evdev code order, so the harness writes the matching
  `engine:sdl,guid:03000000091200000d0e000001000000,port:0,...` params for player 0 (A = button 1
  east, B = 0 south, ...). Background events are on (`SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS`),
  so focus does not matter.
- **Display / screenshots**: gamescope (`--backend headless` with `--headless`), the emulator via
  Xwayland, screenshots with `gamescopectl screenshot` against the run's own gamescope socket
  (read from the emulator's environment). The screenshot backend is a class in `run.py`
  (`SHOT_BACKENDS`); the debug-server channel from upstream PR #111 can be added beside it.
- **fps**: MangoHud as gamescope's `--mangoapp` (the in-process MangoHud layer crashes this
  emulator at exit and stalls shutdown ~50 s; mangoapp does not). Logging is started and stopped
  with `mangohudctl set log_session` at the in-game window, so `mangohud.csv` holds only the
  measured part. The emulator log's per-frame `NX2 deferred writes frame=` lines give guest fps and
  the deferred-write counters for the same windows; `nvidia-smi dmon -s tu -o DT` gives PCIe/SM.
- **Screen states** (`states.py`): reference crops in `refs/` (title logo, the "Load" banner, the
  Load/Cancel dialog) compared as 64x32 grayscale thumbnails; in-game is the heart HUD (red pixels
  top-left). `python3 -I states.py <png>...` prints the classification and scores for screenshots;
  `states.py make-refs title=... load_list=... load_confirm=...` rebuilds references.
- **Camera turns**: 6 right-stick pulses of 0.2 s with 0.6 s gaps = ~90° (24 = a full turn,
  measured with `--probe-steps 24`). Shorter gaps over-turn: with 0.3 s the headings drifted about
  90° every two cycles. Each cycle after the first logs a rotation-closure score (thumbnail MAD of
  the q0 view against cycle 0's; ~37 for visually matching views, since fog, leaves and NPCs move,
  ~55 when drifted). The per-visit rows in summary.md show whether a heading's fps is stable.
- **Shutdown**: SIGTERM to the emulator (SIGINT is `_exit(1)` in this frontend), wait for exit and
  check the log for "Emulation cleanup completed"; SIGKILL after `--shutdown-timeout`. Core dumps
  of the emulator are disabled unless `--keep-cores`.

## Flash and flicker (on by default)

Screenshots cannot catch a single-frame white flash or texture flicker, so every run also counts
them on the emulator side. The harness starts the debug server (`EDEN_NX2_DEBUG_SERVER=
127.0.0.1:<--debug-port, 6545>`, loopback only; `EDEN_NX2_DEBUG_DIR=<run>/debug`) and, once in
game, sends `flash_monitor` (`debugsrv.py` is the small JSON-lines client). From then on every
present is downsampled on the GPU to 64x36 and read back asynchronously
(`src/video_core/nx2_flicker_meter.cpp`, design and cost in docs/nx2/debug-mcp.md):

- **flashes**: frames whose centre-half mean luma (0-255) is at least `--flash-jump` (6) above
  BOTH neighbouring frames, i.e. a one-frame brightening (the PR #121 "+6 over both neighbours"
  test).
- **flicker**: non-flash frames where one of 48 tiles (8x6 samples each) has its mean or
  deviation spike or dip by at least `--tile-jump` (12) against BOTH neighbours. Camera turns
  change tiles monotonically and do not count; a flickering texture does.

`flash_stats {"reset":true}` is called at the start and end of every window, so each row of
summary.md has a `flashes` and a `flicker` column (heading rows sum their visits; the in-game row
is everything since the monitor started, camera turns included). The "Flash / flicker monitor"
section has the totals, the largest jumps, dropped probes and the measured CPU/GPU cost per
present. The first flash arms one frame dump into
`<run>/debug/flash-monitor-<session>/first-flash/` (read it with tools/oot/framedump/); the dump
stalls the window it lands in for a few seconds, so `--no-flash-capture` when fps matters more.
Measured cost: about 0.012 ms CPU and 0.011 ms GPU per present (runs 20261105-180556/-181234).
`--no-flash` runs without the debug server; a binary without `flash_monitor` (before
feat/harness-flash-detect) leaves the columns at `-` and logs a failed `flash-monitor` step.

## gdb stub and SVC trace (opt-in)

    python3 -I tools/oot/harness/run.py --headless --gdb                          # stub + in-game probes
    python3 -I tools/oot/harness/run.py --headless --gdb --svc-trace 4800:4870    # + SVC window

- `--gdb` sets `use_gdbstub=true` / `gdbstub_port` (`--gdb-port`, default 6543) in the profile's
  `user/config/qt-config.ini` only; without `--gdb` the profile pins `use_gdbstub=false`, since the
  stub holds the guest at boot until a client continues it. At launch the harness waits for the
  log's "Starting server on port" line and continues the guest; in game it runs `threads`,
  `delta` (20 s), `pcs` (60 s) and `count-frames` (60 frames) into `threads.txt`, `delta.txt/.csv`,
  `pcs.txt/.csv`, `count-frames.txt`, each as a `gdb-*` phase so summary.md has guest fps over it,
  then a "gdb stub" section (delta distinct values, top-10 main-thread PCs as Ghidra addresses,
  mean kick/join per frame). The heading measurement runs afterwards, unperturbed by stops.
- The probe scripts are a **private kit outside this repository** (`--kit`, default
  `~/Downloads/OOT/ghidra/runtime`: `rsp_probe.py`, `svc_trace_parse.py`), run with `python3 -I`.
  Without it the guest is still continued (a built-in two-line client in `gdbkit.py`) and the
  measurements are skipped with a message.
- **The stub listens on 0.0.0.0** (`src/core/debugger/debugger.cpp:146`), unauthenticated, while
  the run is up. The preflight refuses if the port is already bound (it bind-tests rather than
  connects, because connecting to a live stub pauses that emulator).
- `--svc-trace FIRST:LAST` adds `EDEN_NX2_SVC_TRACE=FIRST:LAST` to the run's environment and, after
  shutdown, runs the kit's `svc_trace_parse.py` on `eden_log.txt` (with `--threads threads.txt`
  when `--gdb` captured the names) into `svc-trace.md`.

## Output: `~/Downloads/OOT/harness-runs/<stamp>/`

`screenshots/` (title, file-select, load-confirm, in-game, spawn, after-walk, heading<q>-c<cycle>),
`mangohud.csv` (+ mangoapp's own summary), `pcie.txt`, `eden_log.txt`, `gamescope.txt`,
`harness.log`, `run.json` (steps, phase windows, clock offsets), `summary.json`, `summary.md`
(per-phase and per-heading: MangoHud fps median/p5/p95, frametime p95/p99, guest fps,
native_dispatches, native_marked_KB, fence_copy_MB, pool collected vs applied, PCIe rx p95, SM %,
flashes, flicker), `debug/` (the debug server's dump dir: the first-flash frame dump).

Rules it enforces: one emulator instance (refuses if `bin/lichii` is running; `--wait-idle N`
waits), MemAvailable ≥ 40 GiB, Python niced.
