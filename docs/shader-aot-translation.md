# Shader translation ahead of first use

Ocarina of Time (0400486024DEC000) stutters the first time it draws with a pipeline it has
not built before. Almost all of the GPU-thread cost of such a pipeline is shader translation
(Maxwell decode, IR passes). `docs/shader-translation-cache.md` describes the translated-shader
cache and, under "Programs known before first use", proposes translating the game's programs
before their first draw. This document covers what was built for that, what the measurements
showed, and where the proposal was changed.

Everything is behind the per-game setting **Graphics > Advanced > Translate shaders ahead of
first use** (`use_shader_aot_translation`, default **off**). `EDEN_NX2_AOT_TRANSLATE=1` also
turns it on. The setting remains off by default. Offline products are a separate CLI mode and can
reuse replay-validated entries and defer missing variants without enabling AOT seeding.
Both paths now use the Format 10 writer, shared runtime-info canonicalization and
class-replay hooks; older formats are rebuilt.

## What the switch does

1. **Task and mesh stages are cached.** Before this change the translation cache refused task
   and mesh stages: `ProgramMeta` did not carry `MeshInfo`. OOT draws vegetation with mesh
   pipelines that share one task program. Entries now store `MeshInfo` in a trailing record
   section that is written only for task/mesh entries.
2. **A variant miss is translated on the worker.** When a stage's entry matches but has no SPIR-V
   for this pipeline's runtime info, it used to be translated again on the GPU thread. It is
   now translated by the pipeline worker that emits it, from `ReplayEnvironment`: an owned copy
   of the stage's environment that answers only with the matched entry's live answers and never
   reads guest memory. If the worker's translation differs from the entry, the build fails
   closed and its draws are skipped. Stages with legacy varyings stay on the GPU thread.
3. **Texture handles are replayed by class.** The texture pass reports each handle computation
   to the environment (`BeginTextureHandle` / `EndTextureHandle`). The recorder stores a handle
   as a `TextureHandle` query holding its source recipe (banks, offsets, shifts) instead of
   the raw words. Replay rebuilds the live handle and compares the texture queries made with it
   (type, pixel format, integer-ness). A word that is also consumed by value keeps its exact
   query: driver-constant folding, BRX tables, and the bank-1 tracking read. A recorded handle
   that maps to two live handles fails.
4. **Pool recognition** (diagnostic, needs `EDEN_NX2_AOT_INDEX=<file>`). At each native graphics
   program's first use, its first 256 bytes are looked up in an index of the game's BNSH
   containers. A matching container's other programs are verified by fingerprint at the same
   pool offset. The index is built by `tools/oot/container/aot_index.py` from the user's own
   game files and is game data, so it stays outside the repository.
5. **Seeding** (opt-in, `EDEN_NX2_AOT_SEED=1`, needs the index). Up to four verified vertex
   and pixel siblings per first use get a real environment on the GPU thread. One background
   worker translates each with guessed answers, records what was answered, and stores the
   result as an ordinary entry. Bank-1 words are not guessed: bank 1 is the program's own
   constant section, part of the pool at program + ctrl[+0x40], and is read from GPU memory.
   The draw's class-level replay keeps a seeded entry only if the live answers agree. Bounds:
   32 jobs pending and 4096 entries, reserved at admission. Candidates are admitted before their
   environments are built, and jobs from before a cache reopen store nothing.

## Measurements

Harness: `tools/oot/harness/run.py --headless --duration 300` with
`EDEN_NX2_GFX_STORAGE_SIZE_LIKE_COMPUTE=0 EDEN_NX2_UNIFORM_BANK_REUPLOAD=0`, RTX 3080 Ti. Cold
runs use the new `--cold-shader-cache`: a private copy of the profile with an empty shader
directory. The driver's own cache stays warm. Pipeline numbers come from the
`Runtime pipelines in the last 10 s` lines; "frame-blocking" adds the GPU-thread build time and
the time draws were held for unbuilt pipelines.

| Run | Config | Whole log: pipelines / GPU-thread ms | In game: pipelines / GPU ms / held ms / frame-blocking ms | Worst 10 s | fps med / p5 / ft p99 |
|---|---|---|---|---|---|
| 20261105-224650 | cold, off | 874 / 3843 | 156 / 892 / 155 / 1047 | 47 / 280 | 35.6 / 27.7 / 42.2 |
| 20261105-231202 | cold, on (2b24032436, recognition + query-free seeding) | 879 / 3131 | 176 / 776 / 160 / 936 | 47 / 249 | 35.7 / 27.2 / 42.3 |
| 20261106-002657 | cold, on (5207f8e148, product config) | 902 / 3096 | 176 / 750 / 258 / 1008 | 47 / 244 | 35.3 / 26.9 / 42.9 |
| 20261106-010934 | cold, on (0457b9b2e2, recognition + guessed seeding with bank-1 bytes) | 878 / 2876 | 154 / 645 / 298 / 944 | 64 / 276 | 35.7 / 27.0 / 44.2 |

None of these runs was tainted. Run 20261105-223250 (2fa5aa4ecc, cold, on, recognition only)
was tainted (CPU load 42%); only its counters are used. In run 20261106-001156 the harness
declared in-game before the load burst ended, so its in-game window is not comparable.

Warm cache: in run 20261105-221118 (private warm profile, setting on, verify) the scripted route
built **0** pipelines at run time. Every pipeline came from the boot preload, including 178
task/mesh pipelines with 0 failures. A warm off/on pair therefore cannot show a difference on
this route, and was not run. The warm reference run 20261105-203404 (108 pipelines / 1012 ms
right after the load) was taken while the shared profile's caches were being rewritten by other
builds. At the time the translation file was keyed to the executable, and every build started it
over; since dev 04c181b626 it is keyed to the translator's sources
(`CMakeModules/TranslatorIdentity.cmake`, file `vulkan_translations.<tag>.fork-vN.bin`) and only
a translator change starts it over.

### Correctness

- `EDEN_NX2_TRANSLATION_CACHE_VERIFY=1` with the setting on (run 20261105-221118): 727 cached
  stages, task and mesh included, compared with a fresh translation (SPIR-V, `Shader::Info`
  field by field, bindings). **0 differed.**
- Worker retranslation from entries: 0 differences in every run (157, 138 and 217 stages).
- Class-level replay: "known key, different answers" fell from 150 stages / 277 ms
  (231202) to 0 ordinary stages (002657). The first differing query had been a bank-2
  texture-handle word in 149 of 152 cases.
- Seeded entries are ordinary entries and pass the same replay. In run 20261106-010934:
  - 1428 candidates; 1398 stored with guessed answers; 0 needed a word that cannot be guessed;
  - 946 candidates had their bank-1 section read from the pool;
  - **66 stored entries were used by a later first draw** (first-use hits completed before
    demand);
  - 283 worker retranslations, 0 differed;
  - 6 replay misses on known keys (5 one-to-many handle mappings, 1 storage classification).
  Seeding costs 6.7 s of one background worker per cold session, plus 167 ms of GPU-thread
  time to build candidate environments and 24 ms for recognition. Those 191 ms are not in the
  pipeline columns. Counting them, seeding saved about 30 ms of GPU-thread time over the whole
  session against the product configuration (2876 + 191 vs 3096 ms). Only 66 of 1398 stored
  entries were drawn, because candidates are offered in container order rather than the order
  the scene draws them. The scene manifest (see below) is the lever for that.
- Unit tests `tests [translation_cache]`: 98 assertions in 12 cases. They cover mesh-info
  persistence, the indirect-pointer query, recorder cases (exact-only word, handle-only word,
  shared word, two-word handle, one-to-many mapping) and index parsing.

### Recognition

Run 20261105-231202:
- 796 first uses; 682 were in the index; **613 (77%) had been predicted** (address and bytes
  verified) before their first draw;
- 57 programs were ambiguous (shared by many effect containers);
- 66 pools recognised; 14987 siblings verified, 0 differed, 0 unmapped.
The first version cost 1274 ms of GPU-thread time, mostly from scanning duplicate containers.
Sampling eight programs before a full scan brought it to 24.5 ms (run 20261106-001156).
Building candidate environments for seeding (lowering included) costs another ~330 ms per cold
session, which is why seeding is opt-in.

### What the queries are

From the 1035 entries of a cold session:
- 74% of `CbufValue` reads are texture-handle words. The rest are bank-1 driver words folded as
  constants (exact), BRX tables and bank-1 tracking.
- Texture answers: Color2D 3123, ColorArray2D 21; integer 4 of 4134.
- Storage pointers: direct 92% true, indirect 61% false. The same query got a different answer
  between entries of one program once in 114.
- Bank-1 words never changed between entries of one program (202 / 0).
- The guess "Color2D, float, direct = true, indirect = false, viewport on" fits 96% of
  entries. 24% of entries read an exact bank-1 word, which is now read from the pool.

## Where the design was changed

- **"Answer queries from the GPU state at pool-map time"**: not used. At map time the bound
  constant buffers and texture table belong to another draw. Replay compares exact words, so
  such answers would almost never validate. Seeding uses explicit guesses validated by
  class-level replay, plus exact bank-1 bytes from the pool.
- **"Emit SPIR-V for the most common runtime info"**: unnecessary. The cost the GPU thread paid
  on a variant miss was the retranslation. Moving that to the pipeline worker (item 2) makes an
  entry useful without a matching variant, and needs no recipe statistics.
- **The original 25–58% "programs never drawn before"**: on the current tree and route, the
  warm-cache stutter came mostly from task/mesh stages the cache refused, not from unknown
  programs. In a cold session, most first-use time is genuinely new programs: "new stage key"
  ~700 stages / 1.5–1.7 s per session.
- **A span-based lowering helper** (codex plan): not needed. Siblings get a real
  `GraphicsEnvironment` on the GPU thread, a few per first use, then an owned copy.
- **Query-free seeding first**: done and measured. Only 11 of 2674 candidates make no query,
  and none of them was drawn. Guessing with class-level validation replaced it.

## Costs and limits

- Memory: entries are metadata plus compressed SPIR-V; the translation file reached 21 MB in a
  cold session. Seeding is capped at 4096 entries. The index is ~1 MB (57381 programs).
- The AOT worker thread exists only after the first seeded job.
- `Store::Insert` still flushes the file under the store mutex. Accepted, since AOT stores are
  rare.
- Not covered: compute programs, task/mesh seeding (their pipeline slot depends on draw state),
  sharcb (BAHS) containers, the 57 ambiguous effect programs, and other GPUs.
- Draws held for unbuilt pipelines rise when translation moves to workers (155 → 258 ms in
  game). The in-game frame-blocking total fell only ~4% in the product configuration, while
  GPU-thread time fell 16%.

## Commits (feat/shader-aot-translate, from dev 02dfcd9766)

- `2d38306b76` tools/oot/harness: --cold-shader-cache runs from a private profile copy with an empty shader dir
- `b86c2f57cd` shader_translation_cache: store the mesh info of task and mesh entries
- `765d714618` vk_pipeline_cache: use_shader_aot_translation caches task and mesh stages
- `26b96979a0` shader_translation_cache: record and replay the indirect storage pointer query
- `2fa5aa4ecc` video_core: recognise shader pools at first use (AOT step 1, no translation)
- `db03980d13` video_core: offer recognised pool programs to a renderer for AOT translation
- `2b24032436` vk_pipeline_cache: AOT translation on a worker from owned environments
- `2f4047d016` shader_translation_cache: count replay misses by first differing query kind
- `f7cdac9675` AOT: fixes from the worker review; recognition cost bounded; seeding opt-in
- `5207f8e148` translation cache: class-level replay of texture handles (AOT switch)
- `101ce5d055` AOT: translate recognised programs with guessed answers, validated by class-level replay
- `0457b9b2e2` AOT: answer bank-1 words from the program's own constant section

The indirect-query fix also exists on its own as `63b50fa209` on
`fix/translation-cache-indirect-query`, cut from dev, for merging ahead of the AOT work.

## Manifest-driven seeding (feat/shader-aot-manifest)

Branch 2 fixes the two measured reasons why seeding moved GPU-thread time without lowering the
frame-blocking total: candidates were offered in container order (66 of 1398 seeds drawn), and
a seeded entry carried no SPIR-V, so its first draw still waited for a retranslation and an
emission on the pipeline worker. Everything stays behind the existing switches.

### What was added

- **Scene manifests** (`EDEN_NX2_AOT_MANIFEST=<manifest-<Scene>.json or a directory of them>`).
  These are static per-scene lists built from the game's own files, kept outside the repository.
  Each row names a stage program by the index key: fingerprint (FNV-1a 64 of the first
  min(size, 256) bytes), pool offset, padded size and stage slot. Each row also has a pass tier.
  The key was checked against the index offline: every row of the measured scene's manifest
  matches an indexed program exactly.
  - Every manifest in the directory is read on a loader thread and applied once.
  - A program named by several manifests keeps its lowest tier and is queued once.
  - First uses seen while the files load are recognised after they are applied.
  - A recognised pool offers its manifest programs, tier 1 to 3. Other siblings are not
    offered when a manifest is loaded (`EDEN_NX2_AOT_OTHERS=1` offers them after the
    manifest programs).
  - A full queue gives way to manifest programs.
  - Manifest programs are fed to the workers from any draw while the workers have room, not
    only at a new program's first use.
  - The manifest's bank-1 words are only compared with the pool bytes, which are
    authoritative.
- **Workers**: `EDEN_NX2_AOT_WORKERS` (default 2, at most half the host threads), with the same
  ownership rules as before.
- **Seeded modules** (`EDEN_NX2_AOT_EMIT=0` turns them off).
  - Run-time vertex-B + fragment pipelines record each stage's emission recipe (runtime info,
    bindings in) by class: the stage and its loaded varyings.
  - A seed also stores the module for its class's most common recipe. A seed that only found
    a stage-level recipe keeps its owned environment (at most 2048) and is translated and
    emitted again once its class's first recipe is seen.
  - With AOT on, vertex-B and fragment variants are keyed by a canonical runtime info. It clears
    what their SPIR-V does not read: the type and previous-stage stores of generic inputs the
    stage neither loads nor passes through, and the input topology (read for geometry only).
  - A wrong guess is never used: the variant key carries the runtime info and the bindings.
  - A module is used only when its info equals the entry's metadata.
  - A deferred stage now looks its module up on the worker once the earlier stage's bindings
    are known.
- **Publication**:
  - Store::Insert checks the store generation under the store mutex.
  - InsertVariant accepts only an entry of the current store.
  - Shutdown drains the pipeline workers before the AOT workers.
  - With the setting off, the texture pass makes no handle-tracking calls.
- **Statistics**:
  - candidates, seeds, first uses and draw hits by tier;
  - first-use full hits (entry and module from the cache) against modules emitted at the draw;
  - why a seeded program missed;
  - run-time pipelines and held time by how their modules were made;
  - job bytes, and process resident and peak memory.

### Measurements

Cold runs, same harness arguments as above. "Burst" is the 10-s report that holds the scene
load (about 660 pipelines). "After" is the reports after it, up to the end of the in-game
window. The old "in game" column started inside the burst in some runs, so it is not used here.
Cells are pipelines / GPU-thread ms / held ms = frame-blocking.

| Run | Config | Whole log | Burst | After | fps med / p5 |
|---|---|---|---|---|---|
| 20261106-042612 | off (6a2225485d) | 868 / 3553 / 338 = 3891 | 2771 | 948 | 38.0 / 28.8 |
| 20261106-034543 | on, 1 manifest, 2 workers (6a2225485d) | 871 / 2156 / 724 = 2880 | 1966 | 741 | 37.1 / 27.9 |
| 20261106-044617 | the same, 1 worker | 877 / 2262 / 716 = 2978 | 1994 | 803 | 37.7 / 28.1 |
| 20261106-043616 | + re-emission (ee8f66013e), 2 workers | 867 / 2191 / 671 = 2862 | 1952 | 739 | 36.3 / 24.3 |
| 20261106-052024 | off, merged with dev (a457bde01f) | 868 / 3398 / 473 = 3871 | 2487 | 1216 | 41.9 / 27.5 |
| 20261106-051402 | on, global + scene manifest, 2 workers (a457bde01f) | 879 / 1920 / 543 = 2463 | 1747 | 576 | 39.4 / 26.0 |
| 20261106-054640 | the same, 4 workers | 880 / 1925 / 530 = 2455 | 1754 | 565 | 41.4 / 27.5 |
| 20261106-055301 | the same, all 81 manifests | 894 / 2423 / 495 = 2918 | 2161 | 621 | 41.2 / 27.2 |

For comparison, on the same split: the previous seeding (20261106-010934) gives 2374 burst /
944 after, and the earlier off reference (20261105-224650) gives 2965 / 1047. None of the runs
was tainted. fps differences are within the single-run spread (about ±2–3 fps).

Run 20261106-051402 (the merge candidate), counts:
- Manifests: 2 files, 1672 rows of tiers 1–3, 1468 matched indexed programs. The rest are
  engine programs in containers the index does not cover. Read in 22 ms on the loader thread,
  applied in 5 ms. Bank-1 words agreed with the pool 509 times and differed 0 times.
- Seeds: 1031 candidates, 996 stored, **276 used by a first draw** (tier 1/2/3: 211/24/41).
  - 110 first-use full hits; 166 seeded stages were emitted at the draw.
  - Seeded programs whose first draw missed: 42 had another stage key, 16 had different
    answers.
  - 6 draws found their seed still running.
- Seeded modules: 180 with the class's recipe, 737 with a stage-level recipe, then 562 emitted
  again once their class was seen.
- Worker time: 3.8 s translation + 3.0 s modules. GPU thread: 3.2 ms admission + 87 ms candidate
  environments + 23 ms recognition. Job bytes peak 54 MB; process peak resident 12.8 GB.
- Run-time pipelines / ms held:

  | How the modules were made | Pipelines | ms held |
  |---|---|---|
  | All from the cache | 302 | 3.0 |
  | A seeded stage's module made on a worker | 110 | 246.7 |
  | A known program's module made on a worker | 10 | 7.7 |
  | A stage translated at the draw (not covered) | 345 | 285.9 |

  About half of the remaining held time is coverage, half seeded stages whose recipe guess
  missed.
- Workers: 1, 2 and 4 workers give the same held time (716 / 724 ms with 1 / 2 on one build;
  530 / 543 ms with 4 / 2 on the merged one). Held time is the pipeline worker's retranslation
  and emission at the first draw, not a queue on the AOT worker. Two workers draw more seeds
  than one (247 against 203), four no more than two. The default stays 2.
- All 81 manifests at once dilute priority. 2045 seeds were stored and 154 drawn; the candidate
  queue peaked at its 4096 cap. The 81 files (75 MB) were read in 804 ms on the loader thread;
  peak resident was 13.1 GB against 12.8 GB with two. One scene's manifest plus the global one
  is what the launcher should pass until manifests are chosen per scene.

### Earlier start (pool map time)

Not added. The measured pool is recognised at its first program's use about 12 s after the load
starts and 15 s before the in-game window. Only 2–6 draws found their seed still running, and 20
to 40 manifest first uses found it still queued. The loss is in recipe guesses and coverage, not
in the start time. A map-time trigger would need three things this branch does not have:
- a queue from the address-space map path (guest thread) to the GPU thread;
- a GPU-thread point that runs without draws;
- evidence that the bytes are in place at map time.
Lowering does not depend on the pool's address, so nothing else blocks it.

### Correctness

- Seeded entries pass the same class-level replay as any entry: no path takes a seeded module
  without its entry being found by live replay. A guessed float 2D texture fails replay against
  an integer or a different-format live texture.
- Worker retranslations from entries: 0 differed in every run.
- Verify run 20261106-060204 (`EDEN_NX2_TRANSLATION_CACHE_VERIFY=1`, merge candidate):
  - 1400 vertex/fragment stages were emitted with both the canonical and the full runtime
    info, and **0 differed**.
  - Every cached stage, seeded modules included, was compared with a fresh translation:
    **0 differences** were reported over 1109 entry hits.
  - 293 seeds were used by a first draw.
  - The two earlier verify runs (20261106-045241, 20261106-053935) crashed on the title
    screen. The verify-only cross-check had emitted the same IR twice, and EmitSPIRV writes ids
    into the IR, so the second module was invalid. Fixed in 251ea65244.
  - The verify run's own shutdown was forced after 61 s ("emulation thread failed to stop").
    The off run 20261106-052024 crashed at shutdown (SIGSEGV), with AOT off. Other branches'
    recent runs show both, and the AOT-on runs on the same binary shut down cleanly, but the
    cause is not established.
- Unit tests: `[translation_cache]` 143 assertions in 15 cases, including manifest parsing and
  tiers, and refusals across a store reopen. `[video_core]`: only the 2 known
  nx2_video_render failures.

### Commits (feat/shader-aot-manifest)

From feat/shader-aot-translate eb6472c44b, oldest first:

- `bfec9c3089` AOT: scene-manifest priority, configurable workers, seeded SPIR-V variants
- `6a2225485d` AOT: canonical runtime info for vertex/fragment variants; manifest-only seeding
- `ee8f66013e` AOT: re-emit stage-guessed seeds once their class's recipe is seen
- `13e7c2e2d8` Merge dev (eabd20fa53, upstream #194 reconciled) into feat/shader-aot-manifest
- `18cb5e5c35` AOT: read scene manifests on a loader thread and apply them once
- `64980cf629` AOT: a seed parks only while its class has no recipe
- `eba3a53c9c` translation cache: publication checks the store's generation under its mutex
- `24da59555d` AOT: drain the pipeline workers before the AOT workers at shutdown
- `26995796c8` shader_recompiler: texture-handle hooks only for environments that track them
- `4532d5c900` AOT: modules fail closed on metadata that differs from their entry
- `88db798ff4` tests: translation cache refuses publications from before a reopen
- `a457bde01f` AOT statistics: run-time pipelines and held time by how their modules were made; process memory
- `827fd4192b` tests: restore the closing brace lost in the dev merge
- `251ea65244` AOT verify: emit the full-runtime-info module from a fresh translation

The translation-cache FormatVersion is 9 after the merge with dev (this branch had 6, dev 5;
9 is above the offline producer branch's 8).

## Next

- Per-scene manifest selection: always the global manifest, plus the scene's own when its banc
  files are read (or the scene inferred from the first recognised scene container). A slim
  manifest format, or lazy parsing.
- Index sharcb (BAHS) containers: the global manifest's engine programs, present in every scene,
  are the largest uncovered block.
- Recipes: about half of the held time left is seeded stages whose recipe guess missed (the
  fragment's bindings in follow the paired vertex program). Persisting the recipe histogram
  with the translation file would make warm sessions guess from the start.
