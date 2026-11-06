# Offline shader translation

when a shader structure gains a field, update the X list and the count; the build fails otherwise.

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
to the CLI command. That input must be a format-10 translation cache. The producer
imports emission recipes read-only, including from an incomplete tail or a different
translator/GPU/settings configuration. It derives the source stage key using the source
configuration, then translates and canonicalizes with the current destination configuration;
source metadata and SPIR-V are never reused. It can be the destination cache.
Format-9 and earlier inputs are rejected. Use the round-4 product-run cache to capture
runtime variants; untouched offline fallback modules do not count as observed recipes.

`--shader-recipe-count N` selects at most N observed recipes per stage (default 3, allowed range 1–64),
with most-frequent exact-stage recipes first, then class recipes filling remaining slots.
The baseline default is included in addition to this limit. `--shader-recipe-limit N`
caps the total catalog entries across exact and class groups (default 200000).
The 256 MiB import budget counts only admitted serialized recipe tails, excluding
module payloads, entry metadata, and scanned/skipped records.
The reader seeks over fallback modules, compressed SPIR-V, and unused entry metadata;
entry keys/queries associate recipe IDs with source stage keys. Only the stage and
input-load mask are read from metadata when a recipe needs its class key.
Logs and the JSON import report distinguish scanned, eligible, and admitted records,
admitted bytes, bytes skipped by framing, and eligible records skipped by the byte
budget. Logs also report catalog entries and references omitted by the entry cap.
Cross-configuration imports name the source filename's translator tag and the current
tag; renamed/untagged sources report an unknown tag because format 10 stores only the
configuration digest. Runtime open reports other translator tags in the directory once.
The temporary source store is released before emission starts.
Canonical duplicate
variants are emitted only once, and every emission uses fresh IR. Class frequencies
count distinct recorded runtime variants, not repeated gameplay draw invocations.
The producer reports uncompressed emitted SPIR-V bytes as `module_bytes`; `translated`
counts fresh IR translations, which can exceed the number of archive rows when emitting
multiple variants. Complete-variant resume still translates zero programs.

Resume fully reads and decompresses each disk-backed module once per open store,
including its deferred Info, before counting it as complete. A ZSTD frame-header
check alone cannot detect corrupt block data. Validation costs one file open/read
and one decompression per distinct existing module: O(total compressed plus
uncompressed bytes), including 100,000 such operations for 100,000 modules.
Validation is serialized with cache writers and temporarily holds only one decoded
module and its compressed payload; it does not retain decoded modules. Generated
and successfully validated modules subsequently use a presence/state check.
Failures erase the in-memory key so emission can append a replacement; the last
record wins on reload. If emission fails or production is interrupted, a later run
retries the damaged record.

Logs and `--shader-report` include `validated_modules` (successful disk checks),
`invalidated_modules` (failed disk checks), and `replaced_modules` (failed keys
successfully re-emitted in this run). Duplicate input rows do not double-count
these modules. These additive JSON fields keep `schema_version` at 1. Validation
is not a cryptographic integrity check and assumes the product is not externally
modified while its store is open.


Playthrough-hint JSON is not accepted as an emission recipe source: its pipeline keys
need device/runtime-state reconstruction before they can supply emission inputs.
Use a format-10 translation cache recorded during the scene tour instead.
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
loaded metadata and newly emitted compressed modules still lack a global memory
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

Format 9 persists optional full `RuntimeInfo` and incoming `Bindings` with each
variant. Both synchronous and deferred gameplay graphics emission record recipes.
Selection counts distinct stored modules per `(stage, runtime digest, bindings)`;
it does not claim to measure draw frequency. Ties choose the lowest digest pair.
Vertex-B and fragment recipes without legacy varyings canonicalize unused generic input types/stores and input topology before emission and keying. A stage absent from the
recipe input uses its default. Compute recipes are currently unnecessary because
archive compute is unsupported.

Defaults use zero incoming bindings; float vertex/color types; all previous-stage
stores present; no legacy mapping or per-primitive generics; no depth conversion,
early Z, alpha test, point-size override, transform feedback or dual-source blending.
Other fields are value-initialized. Geometry uses triangle input topology; other
stages keep the value-initialized topology. These are speculative variants, not a
promise to cover the game's pipeline state. Emission performs the same legacy-to-
generic conversion as the renderer. First draw checks query replay and reuses metadata on a match. An exact canonical variant key also skips emission; a variant miss reconstructs live IR and emits on the existing worker path. Reconstructed metadata disagreement drops the entry. Stage-level recipes remain marked as fallback modules.

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
Offline translation: seen=N translated=N resumed=N skipped=N modules=N module_bytes=N elapsed_ms=N
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


## Runtime safety revision (round 3, historical behavior)

Format 9 records offline provenance, module ownership, stage-level fallback recipe
provenance, and persistent metadata rejections. Reproduce the product with this
build: format-8 products and recipe inputs are incompatible. The source identity
also changes, so do not rename an old product to the new cache filename.

Offline metadata never constructs a runtime program. Every offline stage is
translated from the live environment before `MakeRuntimeInfo`, previous-stage
interfaces, layer emulation, or deferred emission. Compare all serialized
`ProgramMeta` fields against that live result and drop a differing entry and all
its modules, including after reopening. This permits continuing with live IR
before any pipeline layout consumes the rejected metadata. Other cached programs
whose retranslation differs after metadata consumption fail the pipeline closed.
This ports the invariant from sibling AOT `f7cdac9675`.

Exact module hits still save SPIR-V emission, but currently pay live translation
cost to establish trustworthy metadata. Eliminating that cost needs an independently
validated runtime recipe probe; it must not restore the original speculative path.
Entries with no modules are rejected and counted on load (including interrupted
producer appends). Runtime entry IDs are distinct from speculative offline IDs.

The shared canonicalizer ports AOT2 `6a2225485d`. Producer resume, producer emission,
synchronous runtime emission and deferred emission all use the same canonical
runtime info. Verification also compares full and canonical SPIR-V/bindings. It
retains loaded and passthrough generic input state, and excludes geometry and
legacy-varying programs from canonicalization.

Per-stage logs distinguish offline variant hits, retranslated variant misses and
metadata rejection; each outcome logs its first 20 occurrences per stage. Aggregate
counts and entry-only rejections are reported with `LogStatistics`.

Performance inspection found that entries and variants were already indexed in
unordered maps at load. Lookup is expected O(1) in total file entries, followed by
query replay only for the same stage key; runtime does not search emission recipes.
The bad run logged 0.040 ms per hit. Later intervals show only 1–24 ms worker
SPIR-V work versus hundreds of ms combined worker emission/driver build. Those
measurements do not establish the cause of the 5-second driver build. Retest with
correct metadata before attributing the regression to cache lookup.

The load now streams framed records instead of allocating a second entire-file
buffer. Append/flush uses a separate writer mutex and no longer blocks lookup on
file I/O. Deferred workers now actually look up exact variants when preceding
stages resolve their incoming bindings; reuse also requires agreement with live
metadata already consumed by the pipeline layout. Compressed modules remain eagerly resident; lazy module loading and a
total memory budget are still needed. Writer calls still wait for other writers.

Coordinator: reproduce using the portable profile and exact producer arguments
recorded in `~/Downloads/OOT/offline-translate/produce-run-2/time-v.txt` (run from
the portable profile directory), with the rebuilt binary and a fresh product:

```sh
cd /home/dlynch/Downloads/OOT/offline-translate/profile-offline
flock /home/dlynch/Downloads/OOT/harness-runs/.emulator.lock \
  nice -n 10 /home/dlynch/dev/Lichii-offline/build-offline/bin/eden-cli \
  -c /home/dlynch/Downloads/OOT/offline-translate/profile-offline/user/config/sdl-offline.ini \
  --precompile-shaders \
  --shader-archive-index /home/dlynch/Downloads/OOT/offline-translate/index-v2/offline-0400486024DEC000.json \
  --shader-workers 4 \
  -g '/home/dlynch/Downloads/OOT/The Legend of Zelda Ocarina of TIme [04004860224DEC000].nup'
```

Preserve a copy of the new product before gameplay changes it. Run producer again
to check resume. For each gameplay comparison, use equivalent isolated portable
profiles and reset their shader/driver caches to the intended starting state:
new product, empty-cache control, then new product with verification. The harness
takes the lock itself. From the repository, the product validation command is:

```sh
python3 -I tools/oot/harness/run.py \
  --binary /home/dlynch/dev/Lichii-offline/build-offline/bin/lichii \
  --profile /home/dlynch/Downloads/OOT/offline-translate/profile-offline \
  --headless --duration 300 \
  --env EDEN_NX2_GFX_STORAGE_SIZE_LIKE_COMPUTE=0 \
  --env EDEN_NX2_UNIFORM_BANK_REUPLOAD=0
```

For verification add `--env EDEN_NX2_TRANSLATION_CACHE_VERIFY=1`; use the cold
profile for the empty-cache control. Compare actual scene screenshots (HUD-only
white frames can pass the harness's hearts detector), boot identity/digest,
per-stage outcomes, verification differences, time to gameplay, GPU-thread build
cost, worker emission and driver cost, and held-draw time. No gameplay result is
claimed for this revision until these coordinator runs complete.


Round-3 synthetic test result: `bin/tests '[translation_cache],[video_core]'`
completed with 413 cases: 410 passed, the two known `nx2_video_render.cpp`
assertions at lines 151 and 210 failed, and one Vulkan test was skipped. There
were 62,836,859 assertions, with only the two known failures. Log:
`/tmp/offline-tests3-final.log`. All new cache tests passed. Both the initial and
final incremental locked builds of `lichii`, `eden-cli` and `tests` passed. Build
logs: `/tmp/offline-build3.log` and `/tmp/offline-build3-final.log`. No emulator or eden-cli execution
was performed for validation; no user Eden configuration or cache was accessed.
The six incremental patches/messages (06–11) and the executable commit plan are
in `/tmp/offline-translation-commits/commit-plan3.sh`. No commit was attempted.

## Round 4 memory and remaining work

Loaded SPIR-V payloads are indexed by file offset and read on variant lookup, rather than retaining
all compressed modules in RAM. Entry queries, program metadata, variant metadata, and newly emitted
modules remain resident. Loading still uses one temporary record buffer; this is not a fully lazy
entry store. A read-only inventory of product3 found 609,645,979 bytes (581.4 MiB)
of compressed SPIR-V across 36,546 variants; these payloads are no longer retained on load.
This is an allocation inventory, not a measured reduction in peak RSS. Keep the cache file in place and unmodified while a process uses it.

The round-3 product run peaked at 13.7 GB versus 11.9 GB control for a 720 MB product file. No new
runtime memory measurement has been made by the patch author. Re-measure peak RSS using the same
300-second scene. The next memory item is offset-backed entry and variant metadata with a bounded
hot-entry cache; measure the remaining metadata cost before implementing that larger change.

A matching offline module now reuses metadata after query replay without translation or emission.
A different variant still needs Maxwell translation to reconstruct IR, which is not persisted by
this format; emission runs on the existing worker path. The counters label this as a variant miss,
not an emission-only hit. Persisted IR (or a safely reusable in-memory IR cache) is a separate item.

## Round 4 coordinator revalidation

Rebuild products and recipe caches with format 10; do not reuse format-9 files under a renamed tag.
The serialized storage write-extent padding is now deterministic, and the format/config digest
change deliberately prevents trusting old products. Use fresh isolated portable profiles with the
same settings/save as round 3, never the default user config/cache. Preserve product4 before any
runtime run adds entries or drops.

1. Collect new recipes if desired, then repeat the round-3 producer arguments with the rebuilt
   binary and a fresh product4 profile. Repeat production against the completed destination;
   require `translated=0`, `modules=0`, and identical file size and SHA-256. Check replay-compatible
   supported rows are resumed; unsupported/skipped rows can still be counted as skipped.
2. In a separate fresh producer profile, send SIGTERM to the producer PID after records begin.
   Require exit 143 and the record-boundary cancellation log. Resume that destination and compare
   its module inventory with an uninterrupted run. SIGINT must exit 130. In-flight workers may
   finish one record each; cancellation does not discard completed records.
3. Repeat the product, empty-cache control and product+verify 300-second harness runs, each with
   its own equivalent isolated profile. From this repository run the following for each profile:

   ```sh
   python3 -I tools/oot/harness/run.py \
     --binary /home/dlynch/dev/Lichii-offline/build-offline/bin/lichii \
     --profile /absolute/path/to/isolated-round4-profile --headless --duration 300 \
     --env EDEN_NX2_GFX_STORAGE_SIZE_LIKE_COMPUTE=0 \
     --env EDEN_NX2_UNIFORM_BANK_REUPLOAD=0 \
     --env EDEN_NX2_NATIVE_PREFLUSH=1
   ```

   Add `--env EDEN_NX2_TRANSLATION_CACHE_VERIFY=1` for verify. Require correct geometry in actual
   screenshots and zero real fresh-translation mismatches; canonical difference diagnostics are
   allowed. Record full hits, variant misses, replay misses, metadata drops, GPU-thread and worker
   times, and peak RSS. Compare with 3,421/3,691 ms and 13.7/11.9 GB from round 3. An emission-only
   counter of zero is expected with this metadata/SPIR-V-only format.

## Round 4 paired-data findings

The differing Info bytes are padding in `storage_buffers_descriptors[].write_extent`, after
`bounded` and `guarded`, not `constant_buffer_mask`. Format 10 writes deterministic descriptor
representations. All 331 replay-compatible paired entries have equal semantic Info. Twenty-one
other entry pairs fail handle-alias replay, and six pairs across five keys fail texture-class
replay (2D versus array-2D); these remain misses. The actual live entries are in
`runs3-aside/verify`, while the product run retained 277 persistent drops.

The audit recovers all 679 live stage digests from the saved pipeline environments before
classifying missing keys. Of these, 303 exist exactly offline (47 vertex, 256 fragment).
Changing only texture cbuf bank 0 to the old archive default 2 finds offline counterparts for
all 265 missing graphics keys (245 vertex, 20 fragment). The remaining 111 are unsupported
compute stages. Hash, header, other scalar and unresolved mismatch counts are zero in this
sample. The archive now starts with bank 0, matching the native graphics state; lowered bound
texture instructions still select their encoded bank, including bank 2.

The read-only audit tool reports named Info fields, replay failures and each live key. Build its
small CityHash/ABI helper under the shared lock, then reproduce the round-3 audit:

```sh
flock /home/dlynch/Downloads/OOT/harness-runs/.emulator.lock nice -n 19 \
  python3 -I tools/nx2/translation_cache_audit.py \
  --build-helper build-offline --hash-helper /tmp/translation-cache-audit.so
python3 -I tools/nx2/translation_cache_audit.py \
  /home/dlynch/Downloads/OOT/offline-translate/product3/vulkan_translations.cdfc0fe46a82146c.fork-v53.bin \
  /home/dlynch/Downloads/OOT/offline-translate/runs3-aside/verify/vulkan_translations.cdfc0fe46a82146c.fork-v53.bin \
  --pipeline /home/dlynch/Downloads/OOT/offline-translate/runs3-aside/verify/vulkan.fork-v53.bin \
  --hash-helper /tmp/translation-cache-audit.so --details /tmp/cache-audit4.jsonl
```

The tool supports x86-64 translation formats 9/10 and pipeline format 53. Without a matching
pipeline file it reports exact-key coverage and metadata differences, but cannot recover missing
key components from digests alone. Private inputs and detailed reports stay outside the repository.

Round-4 validation: both locked builds of `lichii`, `eden-cli`, and `tests` succeeded. The final
`bin/tests '[translation_cache],[video_core]'` run had 418 cases: 415 passed, the two known
`nx2_video_render.cpp` assertions at lines 151/210 failed, and one Vulkan case was skipped.
All new tests passed; 62,836,884 assertions included only the two known failures. Logs and six
reviewable patches/messages are in `/tmp/offline-translation-commits/`; the unexecuted commit
script is `commit-plan4.sh`. Its cached patch sequence was verified in an isolated index against
HEAD `120d0078ee`. The audit helper was built under the shared lock and reproduced the histogram.
No emulator or eden-cli was run, no default user config/cache was touched, and no commits were made.


## Pass 5 integration and validation

The dev AOT implementation (18832d80a5, dev head 859b6ef80c) is unified with the
Format 10 producer. One canonicalizer and recipe-class function serve runtime,
seeding and offline production. Replay-valid entries can supply metadata before any
module exists; worker lookup resolves bindings before reconstructing IR for a missing
variant. Full module hits avoid reconstruction. A missing module still requires a
worker translation from captured answers: Format 10 does not serialize IR. Legacy
varying conversion and non-deferred builds retain their synchronous path.

Offline metadata disagreement persists a drop record, including when discovered on
a worker. Canonical verify and asynchronous reference emission use fresh programs;
no comparison emits a second module from an already-emitted program. AOT seeding
retains its existing switches and default-off setting. CLI teardown is outside this
change's scope.

For round-5 validation, rebuild a fresh product using round-4's gameplay cache with
`--shader-recipe-cache FILE --shader-recipe-count 3`, then repeat production and
confirm `translated=0`. Use separate portable profiles for product, empty-cache
control and `EDEN_NX2_TRANSLATION_CACHE_VERIFY=1`; keep settings and the Kokiri route
identical. Record module bytes, variant misses/full hits, replay misses, metadata
drops, GPU-thread translation count/time, memory overhead and verify mismatches.
The expected coverage improvement is a target, not a measured result. The coordinator
must still verify NVIDIA pipeline creation and rendering on the real game.

The Pass 5 locked Ninja build completed for `lichii`, `eden-cli` and `tests`.
`bin/tests '[translation_cache],[video_core]'` ran 440 cases: 437 passed, the two
known `nx2_video_render.cpp` failures (151 and 210), and one skipped Vulkan test;
62,848,335 of 62,848,337 assertions passed. No emulator was run. Build/test logs,
patches and the owner-run commit/revalidation scripts are under
`/tmp/offline-translation-commits/` (`build-plan5-retry.log`, `tests-plan5.log`,
`commit-plan5.sh`, `revalidate-plan5.sh`). The real Git metadata was read-only in
this session, so conflict resolution used temporary Git metadata; the commit
script performs the real merge before installing the reviewed resolution.


## Pass 10 variant residency

`EDEN_NX2_TRANSLATION_RESIDENT_MIB` sets the per-store variant residency budget
(default 512 MiB; zero disables retention of disk-backed payloads). Runtime
insertions retain decoded `Shader::Info` and compressed SPIR-V, including after
append. Loaded variants retain neither until the first successful lookup, which
promotes both. Repeated resident lookups copy Info and decompress in memory;
they do not open or read the cache file.

Oldest admissions are evicted first when the budget is exceeded; hits do not
refresh their age. Accounting includes the payload object and container capacity
(conservatively counting inline small-vector capacity too), not total process
RSS or allocator overhead. In-flight reader snapshots can temporarily retain
an evicted payload. Memory-only variants and failed appends retain their sole
usable copy even above budget. Entries, translator IR and worker allocations
are outside this budget. Lookups release the store mutex before disk reads or
decompression; append still serializes writers, without holding the reader's
store mutex. The on-disk format is unchanged. These cache source files participate in the
translator source hash, so rebuilding produces a new identity tag.
