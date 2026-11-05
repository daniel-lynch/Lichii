# Execution coverage of a game's main module

"What code runs when I open a chest?" There is no decompilation of the Switch 2 games we look at,
only a Ghidra database of each `main` NSO. Execution coverage answers the question by diff: take a
snapshot of the code that has run, do the thing once, take another, and list what is new as
Ghidra addresses. With a function list exported from Ghidra the list becomes function names.

## Use

```
EDEN_NX2_COVERAGE=1 EDEN_NX2_DEBUG_SERVER=127.0.0.1:6545 lichii ...

python3 -I tools/nx2/coverage_diff.py ctl reset          # optional: forget everything so far
python3 -I tools/nx2/coverage_diff.py snap before.json   # in front of the chest
#   ... open the chest ...
python3 -I tools/nx2/coverage_diff.py snap after.json
python3 -I tools/nx2/coverage_diff.py diff before.json after.json --functions functions.tsv
```

`--functions` takes a tab-separated `address<TAB>name[<TAB>size]` file with Ghidra addresses (hex,
with or without `0x`; lines that do not parse, such as a header, are skipped). It is game-derived:
keep it outside this repository; the tool only reads it. Without it the report still lists the
ranges. Addresses in the report are Ghidra addresses, `live - module base + 0x7100000000`
(`--image-base` changes the last term); the live address is printed next to each range for gdb.
Snapshots store offsets from the module base, so two snapshots from different boots (different
ASLR bases) diff correctly.

The report has one line per newly hit range (start, end, bytes, live start, the function holding
the start, and how many more functions the range runs into), then every function the new ranges
touch, in address order.

Diffing removes everything that already ran before, so the cleanest map comes from a "before"
snapshot taken as late as possible: stand in front of the chest, wait a few seconds so the idle
loop's code is in the map, snapshot, open, snapshot. Code that also runs on other occasions (the
chest's first frame on screen, a sound) shows up too; a second "open another chest" diff against
the first "after" snapshot narrows it.

In the OoT harness (`tools/oot/harness/run.py`), `--env EDEN_NX2_COVERAGE=1 --env
EDEN_NX2_DEBUG_SERVER=127.0.0.1:<port>` takes a snapshot at the title step and at the in-game
step and writes `coverage-title.json`, `coverage-ingame.json` and
`coverage-title-to-ingame.txt` into the run folder; `--coverage-functions TSV` names it.

Agents can use the same tools through the MCP adapter (`tools/nx2/debug_mcp.py`):
`coverage_start`, `coverage_reset`, `coverage_stop` and `coverage_snapshot` (which writes to an
`out` file and returns only the summary, as a snapshot can be a megabyte of JSON).

## Debug-server methods

| method | params | does |
|---|---|---|
| `coverage_start` | | clears the map and resumes recording |
| `coverage_reset` | | same as `coverage_start` |
| `coverage_stop` | | freezes the map: later snapshots return it as of now |
| `coverage_snapshot` | `encoding`: `"ranges"` (default) or `"bitmap"`; `extents` (default true) | the map |

Every method answers with `module` (`"main"`), `base` (the live load address, hex string),
`size`, `granule` (4), `stopped` and `compiled_blocks`. A snapshot adds `hit_blocks` and either

- `ranges`: a flat `[offset, length, offset, length, ...]` list in bytes from `base`, sorted and
  merged, plus `range_count` and `covered_bytes`. With `extents` each hit block counts as its
  whole compiled extent; without, as its first instruction only; or
- `bitmap_base64`: the raw bits; bit `i` of byte `b` is the block starting at offset
  `(b * 8 + i) * 4`.

Recording runs from boot when `EDEN_NX2_COVERAGE=1`; `coverage_start`/`reset` only clears. The
methods fail with a message when the switch is off or no `main` has been loaded.

## How it works

- `src/core/loader/nso.cpp` calls `Core::ExecCoverage::OnModuleLoaded` for every module it places;
  only the application process's `main` gets a map (`src/core/arm/exec_coverage.cpp`): one bit
  per 4-byte instruction slot over the module image (0x4ea0000 bytes of OoT image -> 2.5 MiB),
  plus a table of the extent of every compiled block.
- `ArmDynarmic64::MakeJit` gives the application's JITs a `Dynarmic::A64::BlockCoverage`
  (`src/dynarmic/src/dynarmic/interface/A64/config.h`). For every block it compiles, dynarmic's x64
  backend (`A64EmitX64::Emit`) asks it for the bitmap byte and mask of the block's start PC. A
  block inside `main` gets a prologue right after the block's frame setup, which is also where
  linked blocks, the fast-dispatch table and the return stack buffer jump in:

  ```
  mov  rax, <byte address>
  test byte [rax], mask
  jnz  marked
  lock or byte [rax], mask
  marked:
  ```

  After the first entry a block pays one L1 load and a predicted-taken branch, and never writes
  the shared cache line again, so the cores do not fight over it. Blocks outside `main` (the SDK,
  other modules, other processes) get nothing.
- `coverage_stop` copies the bits aside and then sets every live bit, so the prologues stop
  writing; `coverage_start` clears them again.
- A recompile (cache invalidation, a full code cache) asks again, and the table keeps the longest
  extent seen for a start.

Off (the default), `Enabled()` is false, no map is allocated, `block_coverage` stays null and the
emitter emits exactly what it did before: zero cost.

Limits:

- x64 hosts only; the arm64 backend ignores `block_coverage`.
- Granularity is the dynarmic block. A block that is entered counts as fully run, which is wrong
  only when a guest exception (a fault) cuts it short. The extent can run into the next function
  when a function falls through without a branch; the report counts those functions as touched.
- The diff is coverage, not counts: code that ran before the "before" snapshot is invisible in the
  diff even if the event runs it again. Take the "before" snapshot as late as possible.

## Why not something cheaper

- **Sampling PCs through the gdb stub.** The stub parks every core per stop, so a sampler gets a
  few hundred stops a second at a large perturbation, and it is statistical: a handler that runs
  once for a few microseconds (an item pickup) is missed almost surely. Coverage needs every
  entry, once.
- **dynarmic counters.** dynarmic keeps none; its per-block bookkeeping is the perf map (host
  JIT symbols) and the block range table used for invalidation, which record what was compiled,
  not what ran. The compiled set nearly answers "what ran once", since a block is compiled on its
  first entry, but it cannot be reset: a block compiled at boot is not compiled again when the
  event runs it, so it cannot answer "what ran just now".
- **Host sampling (`EDEN_NX2_SAMPLE_PROFILE`).** It sees JIT code only as `[jit]`, without guest
  PCs, and is statistical like the gdb sampler.
