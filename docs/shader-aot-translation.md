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
builds. The translation file is keyed to the executable, and every build starts it over.

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

## Next

- Scene manifest (separate branch): offer only the programs the current scene's materials use,
  tier 1 first, instead of every sibling in container order. Only 66 of 1398 seeded entries
  were drawn.
- Frame-blocking: draws held for unbuilt pipelines (258–298 ms in game) are ~75% of the worker
  translation and emission time. The workers are not yet buying much parallelism; starting
  earlier (seeding) or emitting seeded variants ahead of time is the lever.
- The translation file is keyed to the executable, so every new build starts cold. A
  translator identity made from the sources would keep it warm across builds, but needs care.
