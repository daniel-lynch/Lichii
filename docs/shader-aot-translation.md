# Shader translation ahead of first use

Ocarina of Time (0400486024DEC000) stutters the first time it draws with a pipeline it has
not built before. Almost all of the GPU-thread cost of such a pipeline is shader translation
(Maxwell decode, IR passes). `docs/shader-translation-cache.md` describes the translated-shader
cache and, under "Programs known before first use", proposes translating the game's programs
before their first draw. This document covers what was built for that, what the measurements
showed, and where the proposal was changed.

Everything is behind the per-game setting **Graphics > Advanced > Translate shaders ahead of
first use** (`use_shader_aot_translation`, default **off**). `EDEN_NX2_AOT_TRANSLATE=1` also
turns it on. With the setting off, translation, recording and replay behave exactly as before.
The cache file format version did change (5 → 6), so existing files are rebuilt once.

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

## Per-scene manifests, engine shader packs, persisted recipes (feat/shader-aot-scenes)

Branch 3 closes the three gaps measured on branch 2: the union of all 81 manifests diluted
priority, the global manifest's engine rows matched nothing, and seeds guessed before their
class's first recipe missed. Everything stays behind the existing switches.

### What was added

- **Per-scene manifest selection.** When `EDEN_NX2_AOT_MANIFEST` names a directory that holds
  `manifest-_global.json`, the global manifest is loaded at the first native program and
  `manifest-<Scene>.json` when the game enters that scene.
  - The RomFS builder wraps the scene files while the switch is on: the placement files
    `Banc/<Dir>/<Scene>_Static|_Dynamic.bcett.byml.zs` and
    `Banc/GameBancParam/<Scene>.game__banc__GameBancParam.bgyml` (240 files). Each read posts
    the scene name (`Common::ShaderAotScene`). The pool index polls it on every draw.
  - Which read names the scene was measured with `EDEN_NX2_AOT_SCENE_TRACE=1`, which logs every
    Banc file read. The game reads the save's placement files at boot, behind the title. On
    entry it reads only the scene's GameBancParam. 7 s later it reads Hyrule Field's placement
    files, which is a neighbour prefetch. The first rule ("the last placement read") therefore
    selected Hyrule Field for a Kokiri Forest session (runs 20261106-063022 and 063753). Now a
    GameBancParam read commits the scene, and a placement read names it only before the first
    entry.
  - A new scene drops the queued candidates and replaces the previous scene's tiers; global
    tiers stay. The new scene's programs are then offered from every pool recognised so far, in
    tier order, after their fingerprints are verified again. Seeds already stored stay.
  - A scene without a manifest keeps the current rows. The union of every manifest, as before,
    is used when no scene file was hooked, with `EDEN_NX2_AOT_SCENES=0`, or when no scene is
    entered within `EDEN_NX2_AOT_SCENE_WAIT` seconds (default 120).
- **sharcb (BAHS) code pools in the index.** `tools/oot/container/aot_index.py` also indexes
  the sharcb members of the Shader/ and Lib/ SARCs: the agl, gsys and terrain engine packs and
  the application package.
  - Each code pool (header +0x1C) is one container.
  - Program offsets are relative to that pool, the size is NVN control +0x44, and the stage
    comes from the BAHS record table (`shader_containers.parse_sharcb`).
  - The LXAOTIX2 format is unchanged, and the recogniser needed no change.
  - That agl maps the pool contiguously was not known from the file. The run verifies it:
    siblings are checked by fingerprint, as for BNSH pools.
- **Recipe histogram persistence.**
  - The per-class emission recipes are written next to the translation file, as
    `vulkan_translations.<tag>.recipes.fork-vN.bin`. They are written every 30 s when they
    changed and at shutdown, and read when the translation file opens.
  - The file is refused for another translation configuration. Counts from earlier sessions
    weigh half.
  - A recipe from the file is used like one seen this session. A wrong guess is still never
    used, because the variant key carries the runtime info and the bindings.
- **Re-emission takes pending slots.** Seeds re-emitted once their class's recipe is seen used
  to be queued all at once, bounded only by the 2048 parked seeds. They now wait in a ready list
  and take one of the 32 pending slots each. Every finished AOT job queues the next.
- **Harness.**
  - `--cold-shader-seed FILE` copies a file into the empty shader directory of a cold run.
  - `--gdb` now resumes the guest only after the process is suspended (or 10 s). A `c` sent
    earlier was lost, and the title never showed (runs 20261106-065922 and 072438).

### Index regeneration

    mkdir -p ~/Downloads/OOT/aot/index
    nice -n 10 python3 -I tools/oot/container/aot_index.py <game .nup> <zstd dictionary dir> \
      ~/Downloads/OOT/aot/index/oot-aot-index-sharcb.bin

8 s and 1.2 GB peak RSS. Result: 2907 containers and 64031 programs, against 2894 and 57381
before. The difference is 13 sharcb code pools holding 6650 programs. All 204 engine rows of
`manifest-_global.json` now match an indexed program; 0 did before.

### Measurements

These are cold runs with the harness arguments above plus `EDEN_NX2_NATIVE_PREFLUSH=1`, all
on the feat/shader-aot-scenes binary. The base moved since branch 2: fps rose from about 39 to
about 46, so the comparison is the new "base" run, not 051402. Cells are pipelines /
GPU-thread ms / held ms = frame-blocking. "Burst" and "After" are split as in branch 2. None of
the runs was tainted.

| Run | Config | Whole log | Burst | After | Seeds stored / drawn | Full hits / emitted at the draw | Uncovered pipelines / ms held | fps med / p5 |
|---|---|---|---|---|---|---|---|---|
| 20261106-070421 | off | 882 / 3427 / 362 = 3790 | 2566 | 1060 | - | - | - | 44.6 / 28.5 |
| 20261106-065647 | base: global + Kokiri united (`EDEN_NX2_AOT_SCENES=0`), BNSH index | 894 / 1897 / 538 = 2435 | 1703 | 599 | 1002 / 267 | 111 / 156 | 358 / 297 | 46.5 / 29.7 |
| 20261106-063022 | all 81 manifests, selection by the last placement read (picked Hyrule Field) | 886 / 2178 / 607 = 2785 | 1766 | 882 | 1359 / 184 | 79 / 105 | 396 / 444 | 49.8 / 30.5 |
| 20261106-065902 | all 81 manifests, selection on entry (step 1) | 901 / 1846 / 585 = 2431 | 1713 | 579 | 1024 / 280 | 122 / 158 | 356 / 356 | 46.4 / 28.4 |
| 20261106-072033 | + sharcb index (step 2) | 868 / 1927 / 883 = 2810 | 1725 | 940 | 1050 / 363 | 160 / 203 | 290 / 595 | 47.2 / 30.0 |
| 20261106-073407 | + only 072033's recipe file in the empty shader dir (step 3) | 889 / 1881 / 335 = 2216 | 1571 | 509 | 1086 / 376 | 317 / 59 | 287 / 204 | 47.6 / 28.8 |

- **Step 1.** The full directory with selection matches the global + Kokiri union: 2431
  against 2435 frame-blocking, and 280 against 267 seeds drawn. Branch 2's union of all 81 files
  was 2918, and the first rule here gave 2785.
  - Manifest load: the global file is read in 2.1–2.5 ms on the loader thread and applied in
    about 4 ms. Kokiri's is read in 20–28 ms and applied in 0.4–0.6 ms.
  - Peak resident: 12.86 GB, against 12.78 GB for base.
- **Step 2.**
  - 531 of 531 global rows match (327 before), and every first use is now found in the index
    (800 of 800).
  - The sharcb pools verify at their expected addresses: 73 pools recognised against 66, and
    siblings verified 19822 against 14987 (1 differed). So agl maps the code pool contiguously.
  - Uncovered pipelines fell from 356 to 290, and seeds drawn rose from 280 to 363.
  - Run 072033's frame-blocking rose anyway, from held time after the burst (562 ms; one
    window has 333 ms over 28 pipelines). That run also has 5 pipelines whose worker emission
    plus driver build took over 200 ms; sel has none. These look like first-time driver compiles
    of SPIR-V the driver cache had not seen. Run 073407 used the same index and had none of them,
    with 158 ms held after the burst.
- **Step 3.** With only the recipe file, the session starts with 320 recipes of 250 classes. 602
  seeds used a recipe from the file before this session saw it.
  - 820 seeds were emitted with their class's recipe, against 240, and 195 parked, against 737.
  - Full hits at the first draw rose from 160 to 317, and stages emitted at the draw fell from
    203 to 59.
  - Held time on seeded stages fell from 276 to 105 ms. Frame-blocking is 2216: 9 % below base
    and 42 % below off.

Scene change, using the tour kit's Kokiri Forest → Hyrule Field change. The GameBancParam read
at 135.4 s applied Hyrule Field's manifest in 2.0 ms (34.9 ms read). It dropped 0 queued
candidates and offered 1173 programs from 59 recognised pools. Market's placement prefetch was
ignored.

| Run | Config | Hyrule load window (10 s) | Walk in Hyrule Field (5 windows) | Seeds stored / drawn | fps med / p5 |
|---|---|---|---|---|---|
| 20261106-074925 | global + Kokiri united, sharcb index | 438 / 1167 / 220 = 1387 | held 906 ms | 1088 / 316 | 46.3 / 38.5 |
| 20261106-072835 | all manifests, selection, sharcb index | 450 / 749 / 335 = 1084 | held 5876 ms | 2071 / 533 | 47.6 / 36.6 |

- With the scene's manifest, the load window's GPU-thread time fell by 418 ms (−36 %), and its
  frame-blocking by 303 ms (−22 %).
- The walk windows held 5.9 s against 0.9 s. In run 072835, 4 pipelines spent over 500 ms in
  worker emission plus driver build (2563, 2568, 742 and 732 ms), against 1 in run 074925.
  These are probably the same first-sight driver compiles as in run 072033. That is not
  established: it needs a repeat of 072835 on the now-warm driver cache.
- Whole-log frame-blocking is therefore not comparable between the two tour runs (9611 against
  5453).

Correctness and limits:
- Worker retranslations and replay are unchanged. Bank-1 words agreed with the pool 487–523
  times per run and differed 0 times.
- The SIGSEGV seen at shutdown in some runs (063753, 072033) is the core teardown-order fault
  shared by every branch. The runs reached their 300 s and their statistics are read from the
  periodic lines.
- Unit tests: `[translation_cache]` has 177 assertions in 18 cases, including scene tier
  replacement, the entry/prefetch rule and the scene file names. `[video_core]` has only the 2
  known nx2_video_render failures.
- Not measured:
  - a return to an earlier scene (A → B → A), where the game might not read GameBancParam
    again;
  - a scene entered without a manifest (its rows stay as they were).

### Commits (feat/shader-aot-scenes, from dev 859b6ef80c)

- `af5978f1cc` AOT: select scene manifests by the scene the game reads
- `a9bbdcd202` tools/oot/container/aot_index: index sharcb (BAHS) code pools
- `3afe6a88d7` AOT: keep the recipe histogram with the translation file; re-emission takes pending slots
- `3009389c59` tools/oot/harness: --cold-shader-seed copies chosen files into the empty shader dir
- `d30e337226` AOT scenes: the scene is the one the game enters; placement reads are prefetches after that
- `c014adbc3a` tools/oot/harness: --gdb resumes the guest only once the process is suspended
- `9de4d57112` tools/oot/harness: wait at most 10 s for the suspension line (the log is flushed late)

### Review fixes

A read-only review of the branch found seven defects. Each is fixed in its own commit:

1. Scene hints are scoped to the emulation session. `Core::System::Load` begins a session
   before the game's RomFS is built. That clears the scene, the entry flag and the hooked-file
   count, including the zero-hook case, and moves the generation on. A wrapper that outlives
   its session carries an old token and posts nothing.
2. The recipe file is written through a temporary file that is created exclusively, under a
   name unique to the writer. It is flushed, synced and closed, then renamed.
3. Fresh seeds and re-emissions reserve their pending slot with the same atomic check-and-take.
   The slot is released in a scoped way.
4. A candidate is marked done only once it is seeded or can never be seeded. Candidates there
   was no room for go back to the front of their queues.
5. The size of the recipe file is checked against a maximum derived from the format before it
   is read, and the file is parsed as a stream. A class repeated in the file, or a recipe
   repeated within a class, makes the whole file rejected.
6. A save that fails stays pending: the histogram carries a revision, and only a successful
   write records the revision of its snapshot.
7. A full class replaces its weakest recipe that was only loaded from the file, else a recipe
   seen only once this session. A new dominant recipe therefore gets in.

A scene is also posted only after a read that returned bytes.

A second review left two of these partial, and both are now finished:
- A full class whose recipes have all been seen more than once keeps up to four turned-away
  recipes with a tally. One replaces the weakest resident once its tally is higher.
- A candidate whose address is unmapped when it is taken is offered again after 2 s, 4 s, and so
  on, at most eight times.

Runs on the fixed binary, before these two follow-ups, in the step-2 configuration:

| Run | Config | Whole log | Burst | After | Seeds stored / drawn | Full hits / emitted at the draw | fps med / p5 |
|---|---|---|---|---|---|---|---|
| 20261106-095432 | step 2 (the recipe file of 072033 was refused, see below) | 872 / 1852 / 664 = 2515 | 1766 | 591 | 1316 / 417 | 184 / 233 | 43.9 / 24.4 |
| 20261106-120248 | + only 095432's recipe file (step 3) | 901 / 1755 / 349 = 2104 | 1472 | 487 | 1321 / 414 | 341 / 73 | 44.6 / 25.3 |

- Run 120248 loaded 318 recipes of 249 classes, and 668 seeds used one of them. Its 2104 ms is
  below the 2216 of run 073407, the step-3 run before the fixes.
- The translation configuration digest includes the translator's identity, so a recipe file is
  refused after a translator change, like the translation file. A run with the new translator
  therefore had to write the file first: run 095432, with the same configuration as 072033.
- Run 095432 crashed at shutdown (the known teardown fault), so its recipe file holds what it had
  written by its last 30-s save.

Open assumption: returning to an earlier scene (A → B → A) relies on the game reading A's
GameBancParam again when it re-enters A. No trace has shown a return yet; the tour run made a
single change. If the game does not read it again, B's rows stay selected until the next entry.
