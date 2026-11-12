# AGENTS

See `docs/Coding.md` for technical coding guidance.

Keep comments to a reasonable level. Do not overexplain or add/remove comments from unrelated parts of the codebase.

## Game bug reports

Users report game bugs with Lichii logs (`eden_log.txt`, `eden_log.txt.old`). Each `Booting game: <program id>` line starts a session; the build is on the first line (`Lichii Version: ... | nx2-<sha>`). Known program ids:

| Program id | Game |
|---|---|
| 040074A01BF12000 | Donkey Kong Bananza |
| 040077201BE50000 | Kirby Air Riders |
| 04007C6028AA8000 | Yoshi and the Mysterious Book (demo) |
| 0400D4201C6EA000 | Mario Tennis Fever |
| 0400E88025B1A000 | Star Fox |
| 04007EF01F8AE000 | Fire Emblem: Fortune's Weave |
| 0400C3F00006E000 | Mario Kart World |
| 04008E001F098000 | Nintendo Switch Sports Resort |
| 0400A3A01EE4C000 | Pokémon Pokopia |
| 0400A4802437E000 | Drag x Drive Global Jam |
| 040041E0212AE000 | Fast Fusion |
| 04005B902517C000 | Kirby Air Riders Global Test Ride |
| 040057C02159A000 | Nintendo Switch 2 Welcome Tour |
| 040004F027560000 | Orbitals Global Friends Pass |
| 0400380020056000 | Raiders |
| 0400D1D02399A000 | The Duskbloods network test |
| 0400486024DEC000 | The Legend of Zelda: Ocarina of Time |
| 0400245021D2C000 | Yoshi and the Mysterious Book |

Add ids here when a report identifies a new game.

## Before writing a fix

Lichii is a fork of [Eglantine](https://github.com/Djihads80/Eglantine). Do not guess, and do not rewrite a fix upstream already has.

1. `git remote add upstream https://github.com/Djihads80/Eglantine.git && git fetch upstream`.
2. Check `upstream/nx2`, open and closed PRs, unmerged branches, and issues for the game and symptom (`gh pr list -R Djihads80/Eglantine --state all`, `gh issue list -R Djihads80/Eglantine --state all`, `gh pr diff <n>`). Read the diffs, not only the titles.
3. If nx2 is behind `upstream/nx2`, merge upstream first. Port an existing upstream fix before writing a new one, and cite its PR or commit.
4. Write a new fix only when nothing upstream addresses the root cause the log shows.

## Live debug server

Every debug package on the PC launches with `EDEN_NX2_DEBUG_SERVER=127.0.0.1:6545` (in the env argument of `build-fix.sh`; `EDEN_NX2_DEBUG_DIR=<dir>` moves its dumps). Ask the running game from bash on the PC with native Windows Python; MSYS `python` rejects `C:/` paths.

```sh
PY=/c/Users/Zortos/AppData/Local/Programs/Python/Python314/python.exe
$PY -I C:/lx/src/tools/nx2/debug_mcp.py --server 127.0.0.1:6545 --call '{"method":"status"}'
$PY -I C:/lx/src/tools/nx2/debug_mcp.py --server 127.0.0.1:6545 --call '{"method":"screenshot"}' --out C:/Users/Zortos/.capy/work/lichii-debug/shot.png
```

Methods: `status`, `frame_times` (`seconds`, `refresh_hz`), `screenshot`, `log_tail` (`after`, `contains`), `log_filter`, `capture_frame` (the frame dump below, taken now; `sampled`, `index`, `count` for a burst into `frame-<k>/`), `capture_image`, `reject_shaders` (`EDEN_NX2_REJECT_HASH` at runtime; `"reject": false` restores). Without `--call` the script is a stdio MCP server. See `docs/nx2/debug-mcp.md`.

- Try a live experiment (screenshot, `reject_shaders` A/B, `capture_frame`, `log_tail`) before asking the owner for a new build or shortcut.
- Use full 16-hex-digit hashes; shorter ones are refused.
- Restore every hash you reject before you finish.
- Never start or stop the emulator. If nothing listens, ask the owner to launch the game.
- Keep sessions light while the owner plays: one request at a time, no capture loops, `sampled` only when you need pass inputs.

## RenderDoc and the live server

Use both for graphics investigations. The live server provides emulator state, native shader hashes, logs and reversible shader A/B tests. [RenderDoc](https://github.com/baldurk/renderdoc) inspects the translated Vulkan frame: draw and dispatch state, resource contents, pixel history and shader debugging where supported. Prefer it over extending our dumps when it already exposes the missing evidence.

- Use the live server to identify the failing scene and programs, then capture one affected frame with RenderDoc. Find the first incorrect output and inspect its inputs, bindings, blend/depth state and resource dependencies. Record the build, settings, event IDs and shader hashes so the two views can be correlated.
- The existing integration is `src/core/tools/renderdoc.cpp`, gated by `enable_renderdoc_hotkey`. It requires RenderDoc to be loaded into the process; enabling the hotkey alone does not load it. Ask the owner to launch the capture-enabled package. Never start, stop or inject into their running emulator without their approval.
- [renderdoc-mcp](https://github.com/Linkingooo/renderdoc-mcp) is a third-party replay adapter, not part of RenderDoc. Pin and inspect its version, use a compatible RenderDoc Python module, and verify a real capture opens before relying on it. If an extension, pixel-history query or shader debug operation is unsupported, report that limit rather than treating missing data as a clean result.
- Keep `.rdc` captures on the owner's PC: they can contain game shaders, textures and buffers. Do not upload them to public issues or services. Share only the specific permitted evidence needed for the investigation.
- Capture and replay can alter timing or hide races. Compare with an uncaptured live run before ruling out a synchronization bug. Distinguish measured GPU counters from adapter estimates; use CPU/thread and frame traces for emulator performance.
- A shader replacement that reduces fog or exposure can be a useful visual comparison, but is not proof of an emulator fix. Trace the original pipeline's first incorrect result before shipping a change. Matching shader arithmetic alone does not validate bindings, synchronization or input data.

## Debugging tools

Use these before guessing. All are off unless the variable is set; pass them as the env argument of `build-fix.sh`. Read them from the log at `*:Info` unless noted.

| Symptom | Tool | Output |
|---|---|---|
| Wrong image (white, black, missing objects, smearing) | `EDEN_NX2_FRAME_DUMP=<dir>` with `EDEN_NX2_FRAME_DUMP_AT=<seconds>`, `<frame>f` or `file` (captures once `<dir>/capture.now` exists; default 60); add `EDEN_NX2_FRAME_DUMP_SAMPLED=1` for pass inputs; `EDEN_NX2_FRAME_DUMP_COUNT=N` (≤ 8) captures N consecutive frames into `<dir>/frame-<k>/` (`file` mode re-arms after each burst) | One frame's pass colour targets, dispatch storage images (3D included) and present source; with `_SAMPLED`, every texture each pass samples and each compute shader samples before its first dispatch (`kind: sampled`, with stage (5 = compute), TIC index, host sampler state and raw TSC words, 3D and depth included, block-compressed as raw blocks without a `.png`), the uniform and storage buffers of every draw and dispatch once per shader and binding (`kind: buffer`, bytes in `buffer_<hash>.bin.zst`, from the host buffer when the GPU wrote the range) each dispatch's group counts (`kind: launch`) every draw or dispatch dropped for a rejected program (`kind: skipped`, with program address, rt0 and reason) and the samples each draw passed (`kind: draw`, from an occlusion query, with mesh and indirect flags, first element, element and instance counts; draws made while the game counts samples are not probed), as `.png` and `.raw.zst`, plus `manifest.jsonl` with shader hashes, formats, addresses and per-channel stats. `python3 -I tools/nx2/frame_dump_index.py <dir>` builds `index.html`. Find the first pass whose output is wrong, then inspect its shaders. |
| Rejected or miscompiled shaders | `EDEN_NX2_SHADER_DUMP=<dir>` | Native programs with `.header.bin` sidecars and `failed-compute-*.env` for failed compute pipelines. Replay offline with `tools/nx2/shader_sweep.py` (see `tools/nx2/shader-sweep.md`). |
| A newly lowered program changes the image | `EDEN_NX2_REJECT_HASH=<hash>,...` (native program keys from shader dump file names, or shader hashes from the frame dump) | Those programs are rejected as if unsupported; the log shows `rejected by EDEN_NX2_REJECT_HASH` per hit. Bisect by halving the list. The key covers the captured code, so recompute it when the capture rules change. |
| Draw and render-state questions | `EDEN_NX2_RENDER_TRACE=1`, filter `*:Info HW.GPU:Debug Render.Vulkan:Debug Shader:Debug` | Per-draw state, rejected stages and zero-format texture descriptors. |
| Task/mesh draws with no task groups (missing foliage) | `EDEN_NX2_MESH_COUNT_TRACE=1`; `EDEN_NX2_MESH_COUNT_START=<hex>` picks the macro start whose count writes log their inputs (default `24b`); `EDEN_NX2_MESH_COUNT_TRACE_DIR=<dir>` for the code dump (default `/tmp`) | `NX2 mesh count zero seq=N` per zero written to the task count (`vertex_buffer.count`) with mesh control on: macro or direct write, task and mesh program addresses, and the engine's last macro DMA read (source, first words, `dirty`/`flushed`, collected CPU writes the flush applied over the source); `NX2 mesh count probe seq=N` with the back's GPU-modified state and host buffer words at that source; `NX2 mesh count draw` for zero-group draws with shader hashes; `NX2 mesh count summary` every second (adds DMA reads over a report / a pending report / one the back had not run, count writes by macro start `start=z<zero>/nz<nonzero>/off<mesh control off>`, query reports requested/written/discarded/unsupported type). 200 lines per session. On the split front macros then run in the interpreter, not the JIT. For the traced start, 40 zero and 40 nonzero count writes log `NX2 mesh count macro n=N kind=zero|nonzero`: parameters, register reads (`addr:value`), data RAM reads with their writer (`d<dma serial>@<start>` DMA read, `w<call>@<start>` DWRITE, `self`, `?`), the call's DWRITEs, the emit index, and the parameters' pushbuffer VA, GP entry path (`submitted`/`safe`/`copy`/`slow`/`prefetch`), GPU-written page and whether guest memory there still holds the submitted words. `NX2 mesh count dma flow serial=N` (60 over a report, 20 others) at the end of the reading macro: the words, the data RAM writes and methods after the read, the front's latest `report_semaphore` op over the source (decoded, `reached_back`) and the query cache's report there (`status=pending|written|immediate|discarded_*`, requested and effective type, the value it wrote or `will_write`); `NX2 mesh count report resolved` when a report a DMA read found pending is written. Every report: the first 4 per slot (400 in all) log `NX2 mesh count report request id=N` (GPU and device address, requested and effective type, flags, payload, `path=immediate|fence|sync_operation`, `fallback_payload1` when no streamer takes the type) and `NX2 mesh count report write id=N` (`write_seq` = write order, status, value, `later_request_written_first` when a later report was written before it, e.g. a fence report ahead of a ZPass completion). ZPassPixelCount64: `NX2 mesh count zpass no-counter` when a report finds no host query running (final value 0), `NX2 mesh count zpass enable site=draw|draw_texture|clear` on `zpass_pixel_count_enable` transitions (with `host_query_active`, frame, call index), `NX2 mesh count zpass host-result` with the summed host result per report (200 lines each). The first zero count from a macro writes `mme_code.bin`, `mme_data_ram.bin` and `mme_positions.txt`; `python3 -I tools/nx2/mme_dis.py mme_code.bin --positions mme_positions.txt --start 0x24b` disassembles (`--all`, `--count`, `--data-ram mme_data_ram.bin --dram 0:0x40`). |
| Native global memory a shader cannot bind (writes or loads dropped) | `EDEN_NX2_UNTRACKED_GLOBAL_LOG=1` (also on with `EDEN_NX2_MESH_COUNT_TRACE=1`); A/B with `EDEN_NX2_DYN_CBUF_POINTERS=1`, `EDEN_NX2_INDIRECT_POINTERS=1` (both off by default since the merge of upstream #150, whose dynamic-slot and table-entry bindings are the default) | One line per native program at pipeline build: `NX2 untracked global: hash=<hash> stage=<stage> writes=W loads=L reason=dyn-cbuf(idx=c<n>):w/l,loaded-ptr:w/l,other:w/l dyn-bound:w/l dyn-slots=N dyn-dropped=N indirect-bound:lN indirect-records=N indirect-dropped=N indirect-ssbos=N ssbos=N dyn_cbuf_pointers=0|1 indirect_pointers=0|1`. `dyn-cbuf`: the address comes from a constant buffer read at a dynamic offset (a pointer table); `loaded-ptr`: from a value loaded from memory and still unbound; `dyn-bound`: accesses bound through every pointer slot of such a table (`dyn-slots` of them, `dyn-dropped` over the descriptor limit). `indirect-bound`: loads through a pointer loaded from a record (`*(cbuf[slot] + deref)`), bound per dispatch to read-only bindings (`indirect-ssbos` of them, filled from `indirect-records` records in order; `indirect-dropped` records over the cap of 32). `EDEN_NX2_DYN_CBUF_POINTERS=1` / `EDEN_NX2_INDIRECT_POINTERS=1` turn those bindings on; `EDEN_NX2_INDIRECT_POINTERS=guest` reads GPU-written records from guest memory instead of the host copy (no wait). With `EDEN_NX2_DEFERRED_WRITE_LOG`, the per-frame `NX2 deferred writes` line ends with `native_indirect(bound/dropped/host_reads)`. `shader-sweep` reports the same counts as `native_global` in its JSON (plus `indirect_records`). |
| Video playback | `EDEN_NX2_VDEC_TRACE=1`, `EDEN_NX2_VDEC_DUMP=<dir>` | Decoder submit, decode and composite trace; decoded frames. |
| Which functions a thread spends its time in (Windows) | `EDEN_NX2_SAMPLE_PROFILE=<hz>` with `EDEN_NX2_SAMPLE_DELAY=<s>` or `file` (starts once `sample.now` exists in the log directory), `EDEN_NX2_SAMPLE_SECONDS=<s>` (default 20), `EDEN_NX2_SAMPLE_THREADS` (default `GPU,GPUBack,VulkanWorker`; `CPUCore_*` adds the guest cores) | `nx2_samples_<pid>.bin` in the log directory. `python3 -I tools/nx2/sample_report.py <file> --exe lichii.exe [--svg out.svg]` prints self and inclusive time by function and the top call paths per thread (lichii.pdb next to the exe). Turns the GPU_PROFILE zone timers off. |
| Performance and frame pacing | `EDEN_NX2_GPU_PROFILE=1`, `EDEN_DKB_FRAME_TRACE=1` | GPU thread, back thread and Vulkan worker time per second by zone, GPU waits by the zone they happen in, waits for the back by site, and spin mutex contention; per-present interval, CPU and fence timing. The profile also logs `Async read-back per second` every 10 s: guest reads of GPU-written memory, their waits, and macro DMA reads that waited for the split's back or drained it. |
| HostTiming CPU or timer lateness (late guest timers, audio or vsync jitter) | `EDEN_NX2_HOSTTIMING_STATS=1`, or live with the debug server's `host_timing {"stats": true}` | One `HostTiming wait` line per second: waits, waits woken by newly scheduled events, OS sleep and `WaitFor` time (ms/s), how late timed waits ended and how late events fired (p50/p99/max µs), and the thread's CPU %. |
| Shader stutter or not, boot and cache costs | `EDEN_NX2_STUTTER_TRACE=1` | One `Nx2Stutter` line per frame presented over 33 ms after the last, with its top cause (gfx/compute pipeline built on the GPU thread, pipeline wait, shader capture, texture, buffer, query/fence, present, other) and each GPU thread's ms; `Nx2StutterSummary` every 10 s (frame-time histogram, builds, cache lookups, cache file size); `Nx2Boot` boot phases with commit and peak commit. Frames over `EDEN_NX2_STUTTER_DETAIL_MS` (default 50) also get `Nx2StutterDetail`: each GPU thread's eight largest zones (ms/calls) and the rare sites that ran (device memory allocate/free, staging, buffer and image create/delete/GC, synchronous downloads, `Scheduler::Finish`), with each run over 2 ms by thread; the summary adds the sites per 10 s. The category totals in the summary are every frame's steady cost, not hitch time. `python3 -I tools/nx2/cache_metrics.py parse <log>` turns a log into JSON, `compare` A/B runs; `tools/nx2/cache_bench.py` runs the cold/warm matrix on the PC. |
| Command corruption or pop-in | `EDEN_NX2_PB_CHECK=1`; A/B with `EDEN_NX2_PB_SNAPSHOT=0` or `EDEN_NX2_FETCH_DRAIN=1` | How many pushbuffer entries change between submit and parse. |
| Commands read from memory instead of their submit-time copy (frames ahead, #392) | `EDEN_NX2_LIVE_READ_CHECK=1` | Every 10000 such entries, `Entries parsed from memory (<reason>): N checked, M differ` per reason, plus the first four differing entries per reason. GPU-written pages differ by design (the GPU rewrote them after submit), so compare against a frames-ahead-0 run. |
| Split GPU thread races | `EDEN_NX2_SPLIT_CHECK=1` | Reports scheduler use from the wrong thread. |
| Wrong bytes where the guest reads GPU-written buffer memory | `EDEN_NX2_READ_CHECK=<n>` (1 checks every read download) | Compares 1 in n read downloads with a synchronous flush's copy. Every 10 s a `Read download check` line: bytes that differ, by who wrote them to guest memory, and GPU-modified bytes no download brought (both must stay 0). The first 16 differences get their own line. |
| Wrong macro parameters read by DMA on the split's front | `EDEN_NX2_DMA_READ_VERIFY=1` | A macro DMA read made without draining the split drains it anyway and is compared. Every 10 s a `Macro DMA read checks` line: reads, those made without a drain and those that read other words than after a drain (must stay 0), with the first 16 differing reads. |
| Upscaler and DLSS contracts | `EDEN_NX2_DLSS_CONTRACT_TRACE=1` | Inputs and outputs of replaced upscaler networks. |
| Service calls | `EDEN_NX2_IPC_TRACE=1` | IPC command trace. |
| Read stalls (loading hitches, NUP vs other containers) | `EDEN_NX2_FS_READ_TRACE=1` | About once a second while the guest reads, one `Guest storage reads` line (`IStorage`, the RomFS) and one `Guest file reads` line (`IFile`): count, MiB, p50/p99/max latency and reads over 1 ms and 16 ms, plus totals and read time since boot. Times cover the backend read (NUP decompression included), not the Switch 1 SD-card throttle. |
| Unmapped device reads (`Unmapped Device ReadBlock`) | `EDEN_NX2_READBLOCK_TRACE=1` | Every read of unmapped device memory, one line per read with its first unmapped address, unmapped byte count and caller (`file:line`). Without it only the first 16 are logged. |
| Device loss (`Device loss occurred!`) | Always on | The `Device fault` lines after it: what VK_EXT_device_fault reports (fault addresses and their type, vendor fault codes). `not supported by the driver` means the driver has no such data. |
| Driver crash, stall or slow pipeline builds | `EDEN_NX2_PIPELINE_BUILD_TRACE=<file>` | A flushed `BEGIN`/`END` line per driver pipeline build with the pipeline and stage hashes; `END` adds the wall time (`ms=`). The last `BEGIN` without an `END` names the shaders the driver crashed or is stuck on. Times include waiting on other builds, so compare runs from the same cleared driver cache rather than single builds. |
| Host crash (log just stops) | Windows minidump in `%LOCALAPPDATA%\CrashDumps\lichii.exe.<pid>.dmp` | Read the exception and stack with the `minidump` Python package, then symbolize with `llvm-symbolizer --obj=lichii.exe --inlines --relative-address <offset>` next to the build's `lichii.pdb`. |

## How work is split

- One subagent per game, all running in parallel.
- Small research (upstream PR and issue surveys, log reading): a cheap model, DeepSeek V4.1 Flash at max reasoning when its endpoint is available.
- Hard fixes (emulation, shaders, GPU): GPT-6 Astra at high reasoning.
- When a GPT-6 Astra subagent can't find the cause, switch that subagent to Claude Opus 5.5 at high reasoning.
- The main thread (Opus) orchestrates and reviews.

## Builds

- Cloud machines build with clang and Ninja for compile and unit-test checks.
- The build the owner tests runs on the owner's Windows PC (DESKTOP-4UD89UT, Ryzen 9 5900X), built with clang using 12 jobs (`cmake --build <dir> --parallel 12`).
- On the PC the toolchain is clang-cl (LLVM 22) with the VS 2022 SDK, Qt 6.8.3 in `C:/lx/qt`, Ninja and sccache. Source is `C:/lx/src`, the persistent build dir is `C:/lx/build`, and `C:/lx/configure.sh` holds the configure flags. Build and package a fix with one command: `bash C:/lx/build-fix.sh <branch> <build-name> "<Game> - <fix>" ["ENV=VAL ..."] ["log filter"]`.
- Every fix gets its own debug build on that PC in `C:\Users\Zortos\.capy\work\lichii-debug\<build>\` and a desktop shortcut `Lichii Debug - <Game> - <fix>` that launches it with the owner's existing user data and keeps the log next to the build, so the owner can run it and send the log back.
- Bundle a round's candidate fixes into one build behind env switches, so the owner tests one package and A/Bs it live through the debug server instead of one build per idea.

## Toggles and merging

- Experiment switches (env flags that pick between candidate fixes) never merge to nx2. A proven fix ships as unconditional behaviour, and a disproven one is dropped.
- Diagnostic tools (frame dump, `EDEN_NX2_REJECT_HASH`, traces, the debug server) stay in nx2, off unless their variable is set.
