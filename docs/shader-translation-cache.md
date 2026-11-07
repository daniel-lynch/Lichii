# Translated shader cache

Ocarina of Time (0400486024DEC000) stutters when it builds pipelines mid-play, even
with a large pipeline cache built at boot. Almost all of that GPU-thread time is shader
translation (Maxwell decode, IR passes, SPIR-V emission). Most of it repeats work the
emulator has already done: the same program built into a pipeline with new fixed state,
or a pipeline rebuilt after `EvictUnusedPipelines` destroyed it. This change keeps
the translated stages in memory and on disk, so those pipelines skip translation. Each
cached stage is checked against the live environment before it is used.

Setting: **Graphics > Advanced > Cache translated shaders**
(`use_shader_translation_cache`, per game, default on). File:
`<shader dir>/<title id>/vulkan_translations.<tag>.fork-v50.bin`, next to
`vulkan.fork-v50.bin`, where `<tag>` names the translator (see Translator identity).

## The problem, from four play sessions

All four sessions were on Linux with an RTX 3080 Ti (driver 615.78), on the nx2 OOT
builds. Runtime numbers come from the existing `Runtime pipelines in the last 10 s`
and `pipeline built on the GPU thread` log lines.

| Session | Pipelines built at boot | Runtime pipelines | GPU-thread time | Worst 10 s window |
|---|---|---|---|---|
| 1, default settings | 38 | 1013 | 9.1 s | 2.5 s (347 pipelines) |
| 2, async shaders on, GPU Fast | 907 | 254 | 3.0 s | 1.57 s (133) |
| 3, same | 1044 | 180 | 2.3 s | 1.58 s (124) |
| 4, same | 1048 | 506 | 5.1 s | 1.59 s (97) |

Session 4 also evicted 434 pipelines, then 255. The 10 s window after the second
eviction built 177 pipelines in 1323 ms, which showed up as objects popping in and out.

"New pipeline for known shaders" never appears in these logs. That does not mean the
runtime pipelines all had new shaders: the line is only written with
`EDEN_NX2_PIPELINE_DIFF` set. To measure reuse directly, I compared each runtime
pipeline's stage hashes against the boot pipeline cache and against the pipelines
built earlier in the same session:

| Session | Runtime stage translations of a program already translated | Logged runtime time in pipelines whose every stage was translated before |
|---|---|---|
| 2 | 387 of 545 (71%) | 1.36 of 3.25 s (42%) |
| 3 | 295 of 375 (79%) | 1.16 of 2.26 s (52%) |
| 4 | 746 of 835 (89%) | 3.65 of 4.90 s (75%) |

## Where a pipeline's time goes

I timed each step of `CreateGraphicsPipeline` with temporary instrumentation (not part
of this change). The workload was `eden-cli --precompile-shaders` (the boot-preload
option from the separate shader-precompile change) building the
1051-pipeline OOT cache (944 graphics, 107 compute) on 8 worker threads with a warm
driver cache. Times are per graphics pipeline.

| Step | Median ms | Mean ms | p95 ms | Share |
|---|---|---|---|---|
| Control flow graph | 0.09 | 0.29 | 0.42 | 0.6% |
| `TranslateProgram` (decode, IR, passes) | 8.79 | 37.11 | 172.26 | 78.0% |
| Runtime info | 0.00 | 0.01 | 0.01 | 0.0% |
| `EmitSPIRV` | 0.84 | 3.68 | 16.57 | 7.7% |
| `vkCreateShaderModule` | 0.04 | 0.17 | 0.14 | 0.4% |
| `GraphicsPipeline` (driver build from its cache) | 1.38 | 6.23 | 25.69 | 13.1% |
| Total | 18.36 | 47.57 | 188.21 | 100% |

- **Translation is 87% of a warm boot.** With a cold driver cache, the driver's
  compile is 93% of the boot and translation 7%. This change does not affect driver
  compiles.
- **Native lowering is not the cost.** I timed `LowerToMaxwell` offline (shader-sweep)
  on the 1080 unique graphics programs of the session shader dumps: median 0.055 ms,
  mean 0.121 ms, p95 0.314 ms. The 109 compute programs: median 0.041 ms. Lowering
  runs once per program in `RefreshStages` and its result is already kept in
  `ShaderInfo::lowered_native`.
- **A program almost always emits the same SPIR-V.** The 991 unique graphics programs
  produced 1007 distinct SPIR-V modules across all 944 pipelines, and 978 programs
  produced exactly one. A module cache keyed by the emitter's real inputs therefore
  hits whenever the program is known.
- Compute: translate median 1.10 ms, mean 20.8 ms. Emit median 0.16 ms.

## Design

Each stage is looked up in two steps.

1. **Entry.** The key is a 128-bit digest of the program's unique hash, its pipeline
   slot, and every environment value the translator reads besides queries:
   - the stage, start address, program header, passthrough mask and mesh registers;
   - the texture bound buffer, local and shared memory sizes, workgroup size;
   - the HLE macro state and the native flags;
   - a configuration digest (below).

   An entry stores the queries the translation made, in order, with their answers:
   constant-buffer words, texture types and formats, viewport transform, storage
   pointer classification, and HLE replacements, including misses. A
   `RecordingEnvironment` wraps the live environment while the CFG and
   `TranslateProgram` run. The entry also stores the translated program without its
   IR: stage, sizes, topology, invocations and `Shader::Info`. On lookup, the queries
   are replayed against the live environment in order, stopping at the first different
   answer. Translation is deterministic, so equal answers mean an equal program. Replay
   asks only questions the translation itself would have asked, so it never touches a
   disabled constant buffer.
2. **Variant.** The key is the entry, a digest of the `RuntimeInfo` computed from the
   entry's metadata, and the incoming descriptor `Bindings`. A variant stores the
   zstd-compressed SPIR-V, the `Shader::Info` after `ConvertLegacyToGeneric` and
   emission, and the outgoing bindings. On a hit, translation and emission are skipped
   and only `vkCreateShaderModule` runs. If the entry hits but no variant matches
   (new state that changes emission), that stage is translated and the new variant
   added.

The configuration digest covers:

- the translator's identity (below): by default a hash of the translator's source and
  build facts, or the commit and executable with `EDEN_NX2_TRANSLATION_CACHE_KEY=exe`;
- every `Shader::Profile` and `HostTranslateInfo` field;
- device model, driver and limits;
- `resolution_info`, `disable_shader_loop_safety_checks`, `renderer_debug` and
  `fix_bloom_effects`;
- `EDEN_NX2_UNDEF_ZERO` and `EDEN_NX2_NO_TENSOR`;
- `VulkanPipelineCacheVersion`, which also appears in the file name.

A file written by another translator or configuration is discarded and rewritten. Records
are appended. A record cut short at the end of the file is dropped and the records
before it are kept.

### Translator identity

A cached stage may only be used by a build whose translator would produce the same output.
Until format 7 the key was the build itself (commit, executable size and modification
time), so every rebuild started a new file, even one that only touched the UI. With daily
rebuilds the first session after each build re-translated hundreds of stages: the warm run
after a rebuild built 108 pipelines in its first 10 s of play, against 0 with a warm cache.

The key is now the translator's identity, computed at build time by
`CMakeModules/TranslatorIdentity.cmake` into the generated
`shader_recompiler/translator_source_hash.h` and read through
`Shader::TranslatorIdentity()` (`src/shader_recompiler/translator_identity.h`). It lives in the
`shader_recompiler` library, which the emulator and the offline tools (shader-sweep) both link,
and it holds nothing about the running process, so two executables built from the same source
with the same facts agree on it. That is what an offline producer of translation caches needs.
It is a SHA-256 over a manifest (`<build>/translator_identity/translator_identity.manifest`)
of:

- every file under `src/shader_recompiler` (frontend, IR passes, SPIR-V backend, native
  lowering, its `CMakeLists.txt`), globbed when the hash runs;
- the video_core files that drive translation or store its result:
  `shader_environment.*`, `shader_archive_environment.*`, `shader_replay_environment.*`,
  `offline_shader_producer.*`, `offline_compute_producer.*`, `shader_translation_cache.*` and
  `renderer_vulkan/pipeline_shader_emission.h`, plus the module itself
  (`TRANSLATOR_IDENTITY_FILES`);
- the marked regions of `shader_cache.*` and `renderer_vulkan/vk_pipeline_cache.*`
  (`TRANSLATOR_IDENTITY_REGION_FILES`, below);
- every `common/...` header those files reach through `#include`, followed transitively;
- build facts: system and processor, compiler id and version, C++ standard, build type,
  `CMAKE_CXX_FLAGS` and the per-configuration flags, LTO, the compile options and definitions
  of the `shader_recompiler` target and directory, and the sirit and SPIRV-Headers packages
  (url and version or hash).

`translator_identity.cpp`, compiled as part of the translator, adds its own predefined
macros: compiler and standard library version, `NDEBUG`, `_DEBUG`, `__OPTIMIZE__`, fast-math
and FMA flags and the target instruction set. Debug and Release builds therefore never share a file, and neither do
two configurations of a multi-config generator.

A file's content is hashed, not its time stamp, so touching a file or rebuilding from another
worktree of the same source keeps the identity. Undoing an edit brings the old identity back.
The hash runs whenever one of those files changes (a fraction of a second for about 290
files), and the header is only rewritten when the hash changes, so an unrelated rebuild
recompiles nothing. A file listed by name that no longer exists fails the build, so a rename
has to update the list instead of silently dropping out of the key.

Inputs that vary at run time (profile, host info, device, settings, environment variables) stay
in the configuration digest as before; the identity replaces only the build part.

#### Marked regions

`shader_cache.*` and `vk_pipeline_cache.*` mix the code that decides a translation with code
that cannot change one: pipeline keys and building, eviction, the used-pipeline list, the
driver pipeline cache, AOT scheduling, statistics and logging. Hashing them whole made every
edit of that code a new tag, and so a regeneration of the offline translation product. Only
the code between `// TRANSLATOR-IDENTITY BEGIN[: what]` and `// TRANSLATOR-IDENTITY END` lines
of those files is hashed (manifest line `regions <file> <sha256>`, scheme 2): each region from
its BEGIN token to the end of its END token, followed by a newline, in file order. Inside the
regions:

- `vk_pipeline_cache.cpp`: the translator entry points (`using` declarations), the runtime
  info (`MakeRuntimeInfo` and its helpers), `DeferredStages`, `IndirectPointersMode` (in the
  configuration digest), the profile and host info set up by the constructor, the offline
  product run (`PrecompileArchiveShaders`), `OpenTranslationCache` (configuration digest) and
  `IsTranslationCacheable`, `RecordRecipe`, `EmitWaitingSeed` and `BestRecipe`, `SeedStage`,
  the translation file name in `LoadDiskResources`, both `CreateGraphicsPipeline(pools, ...)`
  and `CreateComputePipeline(pools, ...)` whole, and the compute environment built by
  `CreateComputePipeline(key, shader)`;
- `vk_pipeline_cache.h`: the profile, host info, store and configuration digest members;
- `shader_cache.cpp`: `MeshStateHash`, `RefreshStages`, `SeedPoolCandidates`, `ComputeShader`,
  `GetGraphicsEnvironments` and `MakeShaderInfo` (program hashes, lowered code and the
  environments stages are translated from);
- `shader_cache.h`: `ShaderInfo` and `GraphicsEnvironments`.

Outside them, and so free to change without a new tag: the functions a region calls that
cannot change what a cached stage holds, because they only count, log, schedule or build a
Vulkan object from code that is already final: `BuildStageModule` and `ComputeModuleCode`
(applied alike to cached and fresh SPIR-V; the cache keeps the SPIR-V as emitted),
`LogUntrackedGlobal`, `CountStageOutcome`, `AddRuntimeSpirvTime`, `TryReservePending`,
`ReleasePending`, `ParkSeed`, `EmitWaitingSeeds`, `PumpReadySeeds`, `AotWorkerCount`,
`LoadRecipes` and `SaveRecipes` (a recipe only chooses which variant to emit ahead of time;
every variant is keyed by the runtime info and bindings it was emitted with). Also outside:
invalidation, rejection bookkeeping and lookup in `shader_cache.cpp`, and in
`vk_pipeline_cache.cpp` the pipeline keys, eviction, used pipelines, the dynamic-state features
(they reach a translation only through the runtime info, which is digested by value), boot
loading and the driver pipeline cache.

A region file without a region, an END without a BEGIN, a BEGIN without an END and a nested
BEGIN fail the build. Code a region relies on to decide a translation, its keys or its file
must move into a region with it: code that does and sits outside escapes the identity, and a
build would then load a product that its translator no longer produces. The quoted includes of
a region file are still followed from the whole file.

`tools/oot/translator-tag.py` computes the manifest, source hash and tag of a working tree or
a commit from a build's `translator_identity.facts` and the executable's compile facts,
without building: use it to tell whether an edit needs a new product.

The identity reaches the file in two places, and the file layout does not change
(`FormatVersion` stays 5, so it merges with branches that bump the version for layout
changes):

- the configuration digest in the header, as the build identity did before;
- the file name: `vulkan_translations.<tag>.fork-v50.bin`, where `<tag>` is 16 hexadecimal
  digits of a digest of `Shader::TranslatorIdentity()`. Builds with different translators keep
  separate files, so switching between two builds for an A/B does not throw away either
  cache. A file of an older translator, and the `vulkan_translations.fork-v50.bin` of
  earlier builds, are left in place and never read or deleted; remove them by hand to reclaim
  the space (about 24 MB each for OOT).

`EDEN_NX2_TRANSLATION_CACHE_KEY=exe` restores the old key: commit, executable size and
modification time, and the old file name `vulkan_translations.fork-v50.bin`. Unset or
`source` uses the source hash; any other value keeps the source hash and says so in the log.
The log line at boot names the identity in use: `Translation cache: translator identity
source <16 digits of the hash> (293 translator files, file tag <tag>)` or `Translation
cache: translator identity exe (EDEN_NX2_TRANSLATION_CACHE_KEY=exe, commit …)`.

What the identity does not cover:

- Code outside the hashed set that changes translation output without changing any digested
  input. The regions of `vk_pipeline_cache.cpp` are hashed because the steps between
  translation and emission live there; other video_core code feeds translation only through
  environment answers, which are recorded and replayed, and through the profile, host info and
  runtime info, which are digested by value.
- Headers outside `src/common` (boost containers, fmt, the standard library) other than through
  the compiler and standard library version.
- A file the translator compiles that lives outside the hashed directories. A new translator
  directory must be added to `TRANSLATOR_IDENTITY_DIRS`.

The following are never cached and are translated exactly as before:

- VertexA pipelines, task and mesh stages, and layer-emulating pipelines;
- stages whose environment cannot be serialized (code read past its known size, trap
  guards);
- shaders rejected by `HasBrokenPattern`.

Guards against serializing an incomplete `Shader::Info`:

- The serializer, `InfoEqual` and the digests use field lists. At run time a field
  count of `Shader::Info`, `Profile`, `HostTranslateInfo`, `RuntimeInfo`,
  `IR::Program` and `Bindings` is checked against those lists. If a structure gains a
  field, the cache turns itself off with a log warning until the list is updated, and
  a unit test fails.
- A new query on `Shader::Environment` must also be forwarded and recorded by
  `RecordingEnvironment`; `environment.h` says so where the queries are declared.

`EDEN_NX2_TRANSLATION_CACHE_VERIFY=1` translates every cached stage again and
compares the SPIR-V bytes, the `Shader::Info` field by field, and the bindings.

### Rejected options

- **Seeding from the session shader dumps.** Translation needs the environment's
  answers, which the dumps lack. Only 205 of the 1098 unique programs in the
  pipeline cache (19%) make no queries. Also, the dumped programs are the ones the
  pipeline cache already holds with full environments, and those are preloaded at
  boot. Seeding would add nothing that this cache does not already get from the
  first boot.
- **Caching lowered code.** Lowering is under 1% of a pipeline, and its result is
  already kept per program.
- **Translating off the GPU thread when async shaders is on.** The environment reads
  guest state lazily while translating: constant-buffer words for bindless handles,
  TIC entries, storage-pointer classification and the HLE replacement table. These
  must be read as they are at the draw. On a worker they would be read later, which
  produces wrong shaders, and snapshotting every bound constant buffer and the TIC
  table up front is unbounded. It would also leave the work itself, and the boot
  time, unchanged. It could still complement this cache for new programs later.

## Programs known before first use

The game's shader containers (BNSH in `Shader/*.bfsha`, `Effect/*.vsetb.byml`,
Layout2Shader and the agl `sharcb` files) hold every dumped graphics program (1203 of
1203) and 111 of 122 compute programs byte for byte, about 41k unique programs in all.
Each code pool is mapped into GPU memory as one contiguous copy of the on-disk pool, so
a program can be recognised as soon as its pool is mapped, before its first draw.

That makes ahead-of-time **lowering** easy, because lowering needs only the
instructions and the geometry flag. It is not worth doing on its own. Per program,
lowering costs a median of 0.055 ms (mean 0.121 ms; 131 ms for all 1080 dumped graphics
programs). A pipeline's translation costs a median of 8.79 ms in `TranslateProgram`
plus 0.84 ms in `EmitSPIRV`. Pre-lowering would remove less than 1% of the
GPU-thread time.

Ahead-of-time **translation** is what would help, and it is what this cache is built
to accept. Translation needs the live environment's answers: constant-buffer words
for bindless handles, texture descriptor types, and storage pointer classification.
Emission also needs the pipeline's runtime info. An entry made in advance is
therefore only a guess, and replaying its queries is what makes the guess safe to
use:

1. When a pool is mapped, a worker translates its new programs, answering queries from
   the GPU state at that moment.
2. The results go into this cache as ordinary entries, and the SPIR-V is emitted for
   the runtime info most common for that stage.
3. At the first draw, the entry's queries are replayed. A stale guess fails replay and
   is translated as today. A correct guess skips translation, and a variant miss
   costs only the stage's own translation.

This is not implemented. It addresses the part this cache cannot: programs the game
has never drawn before, which were 25% to 58% of runtime pipeline time in the
sessions above. The 19% of programs that make no queries could be seeded with no
guessing at all. Their variant still depends on runtime info, though, so they too
would be emitted for the most common state and checked by the variant key.

## Results

These are `eden-cli --precompile-shaders` runs in a portable profile with the same
1051-pipeline cache, a warm driver cache and 8 threads. Log lines are quoted.

**First boot, empty file.** Programs shared between pipelines already hit within the
session:
`Translation cache (boot): 527 stages from the cache in 539.2 ms (1.023 ms each), 1468 translated in 43688.1 ms (29.760 ms each)`.
File written: 24 MB, 1309 entries and 1404 modules.

**Second boot:**
`Translation cache: 1309 stage entries and 1404 modules read from vulkan_translations.fork-v50.bin in 35.8 ms`,
`Translation cache (boot): 1995 stages from the cache in 219.6 ms (0.110 ms each), 0 translated`.

**Boot preload of 1051 pipelines.** The "Total Pipeline Count" to "Precompile
finished" interval, with the setting toggled between alternating runs:

| Run | Setting off | Setting on |
|---|---|---|
| 1 | 2.13 s | 0.13 s |
| 2 | 2.02 s | 0.18 s |

**Correctness.** With `EDEN_NX2_TRANSLATION_CACHE_VERIFY=1` against the populated file:
`Translation cache: 1995 cached stages compared with a fresh translation, 0 differed`.
That covers 1888 graphics stages and 107 compute stages, comparing SPIR-V bytes,
`Shader::Info` field by field, and bindings.

**Rebuild after eviction.** With temporary instrumentation, each preloaded graphics
pipeline was built a second time from the same key and environments, in the run-time
mode where the driver build goes to a worker. That is what the GPU thread pays to
bring back an evicted pipeline, apart from building the environments.

| | Median | Mean | p95 | Max | Total, 944 pipelines |
|---|---|---|---|---|---|
| Setting off | 12.18 ms | 44.49 ms | 199.2 ms | 526.9 ms | 42.0 s |
| Setting on | 0.43 ms | 2.56 ms | 13.8 ms | 48.1 ms | 2.4 s |

**Unit tests.** `tests [translation_cache]`: 6 cases, 54 assertions. They cover:

- field coverage;
- `Shader::Info` round trip, including more descriptors than a `small_vector` holds
  inline;
- record/replay, including stopping at the first mismatch before a dependent read;
- stage keys;
- the runtime-info digest;
- persistence, a cut-short tail record, and discarding a file from another
  configuration.

## Costs and limits

- Memory is about the size of the file: compressed modules plus infos, 24 MB for
  OOT.
- A stage translated at run time is also compressed and appended to the file on the
  GPU thread, which costs a fraction of a millisecond next to the translation itself.
- A cached stage costs 0.1 ms (replay, decompression, info copy) plus
  `vkCreateShaderModule`.
- A build that changes the translator, its build flags or the compiler starts a new file,
  and its first boot translates as before. Other rebuilds keep the file (see Translator
  identity). Files of older translators are not pruned.
- The file is append-only and never pruned, like `vulkan.fork-v50.bin`.
- Programs never seen before are still translated on the GPU thread. In the sessions
  above, that is 25% to 58% of the runtime pipeline time.
- Not yet measured: runtime GPU-thread time in real play, other GPUs and drivers, and
  the Windows and macOS builds. The source identity needs no executable, so every platform
  now keeps the file on disk. With `EDEN_NX2_TRANSLATION_CACHE_KEY=exe`, Windows reads the
  executable with `GetModuleFileNameW` and other non-Linux platforms keep the cache in memory
  only.
