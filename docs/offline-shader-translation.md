# Offline shader translation

`eden-cli --shader-archive-index FILE` translates extracted native archive programs
using the initialized Vulkan renderer's profile, host info, configuration digest and
normal per-title translation-cache file. It implies `--precompile-shaders`. There
is no separate offline cache or arbitrary GPU profile. Disk shader caching and
shader translation caching must both be enabled, with Vulkan selected.

The producer supports vertex B, fragment and geometry. Compute, tessellation,
vertex A, task and mesh are counted as unsupported; their scalar recipes are still
missing. Graphics requiring layer emulation are rejected. The archive environment
retains its existing 256-byte bank-1 limit. No game data belongs in this repository.

## Coordinator commands

On the owner's machine, extract the archives into a new scratch directory outside
the read-only game-data root. The existing Python readers cover BNSH, BFSHA, BAHS,
SARC, CGSB, SHN and BND/DCX effect bundles, including supported compressed members.
BAHS extraction now retains the allocation's bank-1 bytes after the instructions.
The extractor writes `manifest.json` plus one `offline-TITLE_ID.json` per title.
Review its errors: a nonzero exit reports incomplete extraction, even if some
programs were successfully indexed.

```sh
python3 tools/nx2/extract_static_shaders.py \
  --read-only-root /absolute/path/to/extracted \
  --source 0400486024DEC000=/absolute/path/to/extracted/shaders.bfsha \
  --output /absolute/path/to/new-shader-scratch

build-offline/bin/eden-cli --precompile-shaders \
  --shader-archive-index /absolute/path/to/new-shader-scratch/offline-0400486024DEC000.json \
  --shader-workers 4 \
  /absolute/path/to/game.nsp
```

Repeat `--source TITLE_ID=PATH` for more archives, or use the extractor's existing
`--inventory FILE` option. Compressed archives using dictionaries need its existing
`--zstd-dictionary-pack PATH` argument. The title ID in the index must equal the
loaded application's program ID; this prevents accidental cross-title population.

Optionally add `--shader-recipe-cache /absolute/path/to/vulkan_translations.TAG.fork-vN.bin`
to the CLI command. That input must be a format-8 cache with the **same translator,
GPU and settings configuration**. It is opened read-only, including when its tail
is incomplete. It can be the destination cache. Format-7 caches only stored hashes,
so their emission inputs cannot be reconstructed; they are explicitly rejected as
recipe inputs. Run gameplay with this build to collect format-8 recipes first.
The shared command parser exposes the options, but archive production is an
**eden-cli** feature, not a GUI precompile operation.

The CLI uses the existing precompile lifecycle: load the application and GPU,
load disk resources, produce archive modules, then exit. The existing shutdown
workaround briefly starts the loaded cores after precompilation. This is not a
standalone translator executable. Do not run it in the implementation sandbox.

## Input schema

UTF-8 JSON, version 1, maximum 64 MiB (`MaxIndexBytes`; larger fails with "index too
large") and 100,000 rows. The extractor writes it compact. Paths are relative to the
index directory unless absolute. Nested/compressed members are materialized by
Python into owned allocation files; offsets never refer into compressed bytes.
For direct uncompressed allocations, container and control may name the same file.

```json
{
  "schema_version": 1,
  "title_id": "0400486024DEC000",
  "programs": [{
    "container": "programs/TITLE/PROGRAM-vertex.bin",
    "offset": 0,
    "size": 512,
    "stage": "vertex",
    "control": "controls/CONTROL.bin",
    "control_offset": 0,
    "constants_offset": 256,
    "constants_size": 4,
    "fingerprint": "0123456789abcdef",
    "tier": 0,
    "bank1_words": [305419896]
  }]
}
```

The example fingerprint is illustrative. Use the generated index. `fingerprint`
is FNV-1a-64 over the first `min(256, size)` bytes. `constants_offset` is relative
to the program; it and `constants_size` must match control words at 0x40 and 0x3c.
The reader consumes 0x58 control bytes. Optional `bank1_words` must cover the entire
constant section and match its little-endian bytes exactly. `tier` defaults to 0;
lower tiers are admitted first, with stable ordering within a tier. A vertex, fragment or
geometry row whose constant section exceeds 256 bytes is parsed and then skipped as
`bank1_too_large`; it does not fail the index. Stage names
are `vertex`, `tess-control`, `tess-eval`, `geometry`, `fragment`, `compute`,
`vertex-a`, `task`, `mesh`. Unknown stage strings or malformed index rows fail the
index; unsupported known stages are counted without reading their allocations.
The extractor omits empty programs and controls shorter than 0x58, which cannot
supply this schema's native control metadata.

The parser rejects negative, floating, overflowing and oversized ranges. Allocation
reads are capped at 16 MiB and checked against the file length before allocation.
The worker verifies fingerprints, constants and control/header agreement before
translation. The fingerprint is a stale-index check, not a cryptographic signature.

## Translation, emission and resumption

Workers default to `max(1, hardware_concurrency / 2)`, capped at 64 and the number
of rows. `--shader-workers N` accepts 1..64. An atomic row cursor admits at most one
allocation per worker, with independent IR pools; there is no unbounded work queue.
Striped stage locks prevent duplicate candidates racing through lookup and append.
This bounds concurrent allocations, **not total process memory**: IR expansion,
loaded cache entries and retained compressed modules still lack a global memory
budget and eviction policy. The earlier proposed `--shader-memory-mib` flag is not
implemented.

The owned archive environment captures instructions with runtime capture rules,
lowers at two canonical bases across a 32-bit carry, and rejects differing code or
maps. It rejects trap guards and incomplete programs. Exact reads must come from
bank 1. Unknown handle words may be guessed only inside tracked handle expressions;
queries replay their derived classes at first draw. Guesses are Color2D,
A8B8G8R8_UNORM, noninteger, direct storage pointer true, indirect false, viewport 1,
and no HLE replacement. Copied class replay came from `feat/shader-aot-translate`
commits `5207f8e148`, `101ce5d055`, `0457b9b2e2`.

Format 8 persists optional full `RuntimeInfo` and incoming `Bindings` with each
variant. Both synchronous and deferred gameplay graphics emission record recipes.
Selection counts distinct stored modules per `(stage, runtime digest, bindings)`;
it does not claim to measure draw frequency. Ties choose the lowest digest pair.
The normal entry-dependent variant key remains unchanged. A stage absent from the
recipe input uses its default. Compute recipes are currently unnecessary because
archive compute is unsupported.

Defaults use zero incoming bindings; float vertex/color types; all previous-stage
stores present; no legacy mapping or per-primitive generics; no depth conversion,
early Z, alpha test, point-size override, transform feedback or dual-source blending.
Other fields are value-initialized. Geometry uses triangle input topology; other
stages keep the value-initialized topology. These are speculative variants, not a
promise to cover the game's pipeline state. Emission performs the same legacy-to-
generic conversion as the renderer. First draw still checks query replay and the
normal variant key, falling back on mismatch.

The producer uses `TranslationCache::Store` and `Shader::TranslatorIdentity()` via
the normal Vulkan configuration digest. New producer sources are included in that
identity. Only successful emission inserts an entry and SPIR-V variant; resumption
requires both matching entry queries and a decompressible matching module. A
metadata-only entry is translated again. The existing append writer flushes each
framed record. On reopening, it preserves complete records and removes an
incomplete tail before appending. Failed repair/append-open no longer falls back
to truncating a valid cache. Recipe reads never repair the source. CLI production
fails if the destination is not persistent or writing fails. This is process-kill
recovery, not a power-loss/fsync guarantee; do not run multiple processes writing
the same title cache concurrently.

The producer logs per-reason skip counts, followed by:

```text
Offline translation: seen=N translated=N resumed=N skipped=N modules=N elapsed_ms=N
```

Reasons include `missing_constant` (a value-consumed word outside exact bank 1),
`unsupported_stage`, `unsupported_lowering`, `base_dependent`, `invalid_range`,
`fingerprint_mismatch`, `constants_mismatch`, `input_io`, and the fallback
`lowering_or_emission_rejected`. Debug logging includes the row index and exception.
Malformed whole indices, incompatible recipe files and persistence failures fail
the CLI; individual rejected programs are reported and the run continues.

## Validation and remaining checks

Synthetic tests cover strict JSON parsing and stable tiers, full recipe round-trip
and most-frequent selection, duplicate worker candidates, complete-variant resume,
metadata-only recovery, and every byte truncation of an emitted module record.
Python shader/container tests pass: 35 tests, including BAHS constant retention
and extraction-to-index generation. Both the full and final incremental builds of
`lichii`, `eden-cli` and `tests` passed using:

```sh
flock /home/dlynch/Downloads/OOT/harness-runs/.emulator.lock \
  nice -n 19 ninja -C build-offline lichii eden-cli tests
```

From `build-offline`, `bin/tests '[translation_cache],[video_core]'` completed:
408 cases, 405 passed, two known failures in `nx2_video_render.cpp` (lines 151 and
210), and one skipped Vulkan test (`EDEN_TEST_VULKAN` unset). All new tests passed.
There were 62,836,751 assertions, with only the two known assertions failing.
Logs: `/tmp/offline-build-final.log`, `/tmp/offline-tests.log`, and
`/tmp/offline-python-tests.log`. No emulator or CLI was executed; no user Eden
configuration/cache was accessed. Commit instructions are in
`/tmp/offline-translation-commits/commit-plan2.sh` with messages 04 and 05; no commit
was attempted.

Real-archive coverage, interrupt/resume of a real CLI process, and gameplay first-use
hits remain unmeasured. On the owner's machine, repeat the command and check that
already-complete variants emit no modules; interrupt and resume a run, then launch
with `EDEN_NX2_TRANSLATION_CACHE_VERIFY=1`. Record entry/variant hits, replay misses
and verification differences. Open questions are the real first-use coverage of
these recipes, scalar recipes for the unsupported stages, programs with more than
256 bank-1 bytes, and a total memory budget covering the cache and IR.
