# Graphics pipeline libraries (prototype, `EDEN_NX2_PIPELINE_LIBRARY=1`)

Status: prototype behind an off-by-default switch. **Not measured in the emulator yet**; the
harness plan is at the end. With the switch unset (or anything but `1`) the Vulkan backend
takes exactly the old path (see "Off means off").

## What it does

Every graphics pipeline that can be split is built from four `VK_EXT_graphics_pipeline_library`
parts and fast-linked without link-time optimization:

| part | contents (from `GraphicsPipeline::MakePipeline`) | key |
|---|---|---|
| vertex input | vertex bindings/attributes/divisors, input assembly | that state |
| pre-rasterization | vertex/tess/geometry or task/mesh stages, tessellation, viewport (+swizzle, depth-clip control), rasterization (+line, conservative, provoking vertex) | SPIR-V content of each stage + that state + layout identity + render pass |
| fragment shader | fragment stage (or none), multisample, depth/stencil | SPIR-V content + that state + layout identity + render pass |
| fragment output | colour blend, multisample | that state + render pass |

Every key also holds the create flags and the dynamic-state list. Parts are cached for the
process in `PipelineLibraryCache` (`vk_pipeline_library.cpp`), shared by all pipelines, and
de-duplicated across threads (a second thread needing a part that is being compiled waits for
it). The linked pipeline replaces `GraphicsPipeline::pipeline`; nothing else in the pipeline
changes (same layout, descriptors, `GraphicsPipelineCacheKey`, disk records).

Code: `pipeline_library.h` (the split, pure and unit tested: `src/tests/video_core/
pipeline_library.cpp`), `vk_pipeline_library.{h,cpp}` (cache, link, statistics),
`vk_graphics_pipeline.cpp` (`MakeLinkedPipeline`, called just before the monolithic
`CreateGraphicsPipeline`), `vk_shader_util.cpp` (records each module's SPIR-V hash),
`vulkan_device.cpp` (enables the extension only when asked).

## The premise, argued with

The field analysis says 60 % of held-draw time (259 pipelines, 10.7 s) was "a known program's
module made on a worker": fragment variants whose descriptor bindings / input varyings follow
the vertex partner, vertex variants for a new vertex-input layout, `force_early_z`. **Those are
new SPIR-V modules, not new fixed-function state.** They come from `MakeRuntimeInfo`
(`vk_pipeline_cache.cpp` ~190-336: `generic_input_types`, `previous_stage_stores`, binding
bases, `force_early_z`, `y_negate`, alpha test, point size, xfb, tessellation modes are baked
into the module). No library scheme makes a new module a link. What libraries change is the
unit the driver compiles:

- monolithic: one compile per exact (vertex, fragment, state); NVIDIA caches per exact pair
  (upstream #411, RTX 4090: cold 2.1-2.5 s; same pair, new state 1-116 ms; a stand-in partner
  does not help);
- libraries: one compile per stage (4090: pre-rasterization 2.6-3.5 s cold, fragment
  0.08-0.19 s cold, either ~1 ms from the driver disk cache in a new process), then a
  0.1-0.3 ms link with any state.

So, honestly, for one session:

1. **A new fragment variant whose vertex partner's library is resident** costs a fragment
   library instead of a full compile: the 0.7-0.95 s field builds would become ~0.1-0.2 s, if
   NVIDIA's ratio on our shaders is like #411's (unmeasured).
2. **A new vertex variant** (vertex-input layout: 49 of the 280 field misses) costs a cold
   pre-rasterization library, which #411 measured as *slower* than a monolithic compile.
   Libraries make that case worse until the driver cache has it.
3. **The pair is usually new as a whole.** 1,660 of 1,764 field pipelines pair stages of the
   same program variation, and a fragment variant made for its real partner is the first
   pipeline that partner appears in, so in a first visit the vertex library is cold too. The
   one-session win is mostly (a) new fixed-function state for a pair already linked (603 of the
   field's pipelines were extra states of 1,161 pairs: attribute formats, render-target
   formats, depth/early-z, cull, blend) and (b) partners whose other stage is resident.

The real lever is **across sessions**: the driver disk cache keeps libraries per stage, so
warming each *module* once (from the translation product, the pipeline-key product or one tour)
covers every pair and state that module meets later, where monolithic warming has to hit the
exact key. Combined with the paired-variant producer (field plan step 3: every module exists
before play), libraries turn "enumerate the pipeline keys" into "enumerate the modules", which
the offline product already does. That is the case the harness has to prove.

### The question that decides the follow-up: does our layout pin libraries to a pair?

Libraries linked without `VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT` need identically
defined layouts. Our layout is one descriptor set holding every stage's bindings
(`DescriptorLayoutBuilder`, `pipeline_helper.h`), so it depends on the partner: a vertex
library built for (V, F1) cannot link with F2 unless F2 has the same descriptors. The prototype
keys the shader libraries on the layout identity, which is correct but means "vertex library
reuse" only happens across fragment variants of the same program and across states — not
across arbitrary partners. #411 avoided this with one descriptor set per stage and
partner-independent binding numbers; that changes SPIR-V emission (`src/shader_recompiler`,
identity-hashed), so it would change the translator tag and invalidate the offline product.
It is deliberately **not** in this prototype. Two measurements decide it:

- does NVIDIA's disk cache key a pre-rasterization library on the layout and render pass?
  (`tools/nx2/gpl_probe`, steps `lib_pre_layout2`, `lib_pre_rp_b`). **Measured: on the
  layout yes (a full 3.65 s recompile for one extra fragment binding), on the render pass no
  (1 ms)** — see "Results" below;
- how many distinct partners a module has in the pipeline-key product (pairs are mostly
  static: 1,660/1,764), which bounds what per-stage sets could add.

## Design decisions

1. **Split from the create info, not from `FixedPipelineState`.** The plan copies the exact
   structures `MakePipeline` builds, so a library holds what the monolithic pipeline would
   have held. Any `pNext` structure, specialization, sample mask or create flag the split does
   not know makes the pipeline monolithic (`Unsupported::UnknownChain` / `CreateFlags`) instead
   of being dropped. Where the `FixedPipelineState` fields land:
   - vertex input: `attributes` (formats/offsets, unless dynamic vertex input), `vertex_strides`
     (unless dynamic stride), `binding_divisors`, `topology` (only its class under dynamic
     topology), primitive restart (static only without EDS2);
   - pre-rasterization: the stages, `patch_control_points`, `viewport_swizzles`,
     `ndc_minus_one_to_one`, `polygon_mode`, cull/front face (static only without EDS1),
     `depth_bias_enable`/`rasterize_enable` (static only without EDS2), depth clamp, line
     smooth/stipple, conservative raster, provoking vertex;
   - fragment shader: the stage, `msaa_mode`, sample shading (setting), alpha to
     coverage/one, depth/stencil (static only without EDS1), depth bounds (always dynamic);
   - fragment output: `attachments` (static only without EDS3 blend), logic op (EDS2/EDS3),
     `msaa_mode`;
   - the render pass (`color_formats`, `depth_format`, `msaa_mode` via `RenderPassKey`) is in
     both shader libraries and the output library;
   - in no library: everything `MakeRuntimeInfo` bakes into modules (above). Those change
     the module, so they change the library's SPIR-V hash.
2. **Static values the dynamic state overrides are normalized** (`Plan::Normalize`): cull mode,
   front face, depth/stencil tests and ops, depth bias enable, rasterizer discard, primitive
   restart, topology within its class, strides, blend enable/equation/write mask, logic op,
   alpha to coverage/one, line mode/stipple, conservative mode, depth bounds, blend constants.
   The driver ignores them at draw time, so pipelines differing only in them share libraries.
   The session keys have EDS1+EDS2+logic op+dynamic vertex input on (one value of the
   dynamic flags), so cull/depth/stencil state no longer splits libraries; blending (no EDS3
   blend in those keys) still splits the cheap output library only.
3. **Libraries are reused by module content.** `BuildShader` records a CityHash128 of each
   module's SPIR-V (only with the switch on); a module the registry never saw makes the
   pipeline monolithic.
4. **Layouts.** The cache owns one layout per layout identity (set-layout flags, bindings,
   partially-bound use, push-constant range), created with the same builder, so a library
   outlives the pipeline that first made it; the link uses the pipeline's own, identically
   defined layout.
5. **Render pass in the key.** The emulator draws inside render passes, and the spec rules for
   mixing a null render pass in shader libraries with a real one in the output library are not
   something to rely on untested, so the shader libraries carry the pipeline's render pass.
   This splits our map by render-target formats; measured on NVIDIA the extra entry costs
   ~1 ms from the disk cache and 0.02 ms in-process (probe step `lib_pre_rp_b`).
6. **No link-time optimization.** `VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT` is never
   set and libraries do not retain LTO info (no memory for it). An optimized relink in the
   background is only worth adding if the harness shows a GPU-time cost of unoptimized
   pipelines; the probe's `link_lto` step gives its CPU cost.
7. **Caching.** No `VkPipelineCache` of our own: libraries go through the same handle as
   monolithic pipelines (null on NVIDIA, `vk_pipeline_cache.cpp` 509-517), so what survives a
   restart is the driver's disk cache. Nothing is added to `vulkan.fork-v53.bin`: with
   preload-all, the boot prebuild of the 8,464-record pipeline-key product builds each key
   through libraries, which fills the part cache for the session. No library eviction yet
   (the count is logged as `resident=`).
8. **Mesh/task** pipelines put task+mesh in the pre-rasterization library and have no vertex
   input part (`EDEN_NX2_PIPELINE_LIBRARY_MESH=0` keeps them monolithic). Compute has no
   libraries and is unchanged.
9. **Fallback.** The device enables the extensions only with the switch on and when
   `graphicsPipelineLibrary`, `graphicsPipelineLibraryFastLinking` and
   `graphicsPipelineLibraryIndependentInterpolationDecoration` are all present (one log line
   either way); otherwise every pipeline is monolithic. Per pipeline: a plan the split refuses,
   a failed library or a failed link builds it monolithic, logged once per kind / for the first
   20 failures, counted in the summary. A failed library key stays failed for the process (a
   transient out-of-memory would keep that module's pipelines monolithic: accepted for a
   prototype).
10. **#321, workers, AOT, reports.** Held-build inline claiming is unchanged; a held draw whose
    build needs a part another worker is compiling waits for that part instead of compiling
    it twice. AOT seeding and the translation cache are untouched. `ReportRuntimePipeline`
    lives in `vk_pipeline_cache.cpp`, which is identity-hashed, so it is **not** changed;
    instead, with the switch on, the per-build `Nx2Perf async_pipelines:` line gains
    `build=linked|monolithic_unsupported|monolithic_fallback libraries_built=<mask>
    libraries_waited=<mask> library_ms= link_ms=` (mask bits: 1 vertex input, 2
    pre-rasterization, 4 fragment shader, 8 fragment output), and `PipelineLibraryCache` logs
    `Nx2Perf pipeline_library: linked= link_ms= link_max_ms= fallback= monolithic: ...
    <part>=built:N/ms/max,reused:,waited:,failed: resident=` every 10 s of builds and at
    shutdown (`(final)`).

## Off means off; the product tag

- Switch unset: the extensions are not loaded and nothing is chained into device creation;
  `GraphicsPipeline` has no library cache, and `MakePipeline` passes the same create info (now
  a named local) to the same `CreateGraphicsPipeline` call; `BuildShader` records nothing; the
  build log line keeps its old format. `CreatePipelineLayout` now takes its push-constant range
  from `PushConstantRange()`, the same values.
- **No identity-hashed file is touched.** `CMakeModules/TranslatorIdentity.cmake` hashes
  `src/shader_recompiler`, the listed `video_core` files (including `vk_pipeline_cache.{cpp,h}`)
  and the `common/` headers they include; this change edits none of them, so the translator
  tag is unchanged and the same `vulkan_translations.<tag>.fork-v53.bin` and
  `vulkan.fork-v53.bin` load in both modes. That is why the "linked" class is reported from
  `vk_graphics_pipeline.cpp` rather than added to `ReportRuntimePipeline`.

## Standalone probe

`tools/nx2/gpl_probe/run.sh OUT_DIR [WORK]` generates synthetic shaders (a heavy vertex
shader, a fragment shader and a fragment variant that differs only in bindings), compiles a
small Vulkan program and runs it in separate processes against a private NVIDIA disk cache
(`__GL_SHADER_DISK_CACHE_PATH`), never the user's: monolithic cold/new-process, the four
libraries cold/new-process, link with new blend, new fragment variant, static cull, whether a
pre-rasterization library hits the disk cache under another layout / render pass, whether a
monolithic compile warms libraries, and an LTO relink.

### Results, 2026-11-06 (RTX 3080 Ti, driver 615.78.08, synthetic shaders, `WORK=400`)

One run, `nice -n 19`. A game session started during it (23:51), so the absolute times are
indicative; the ratios are what matter. ms per step:

| step | cold | new process (warm disk cache) |
|---|---:|---:|
| monolithic V+F | 3,663 | 4.7 |
| monolithic, same pair, new blend | 7.4 | 0.06 |
| monolithic, same pair, static cull / other render-target format | 0.008 / 0.006 | 0.004 |
| monolithic, same V, fragment variant (bindings only) | 7.1 | 0.05 |
| vertex input library / fragment output library | 0.015 / 0.014 | 0.015 / 0.008 |
| pre-rasterization library (V) | 3,667 | 3.3 |
| fragment shader library (F) | 7.5 | 0.05 |
| fast link | 0.24 | 0.36 |
| new output state (blend) + link | 0.016 | 0.008 |
| fragment variant library + link | 6.7 | 0.04 |
| pre-rasterization with static cull | 0.012 | 0.004 |
| pre-rasterization, other render pass (warm cache) | 1.0 | |
| pre-rasterization, **other layout** (one more fragment sampler; warm cache) | **3,651** | |
| same, in one process after a cold pre-rasterization | 3,683 | |
| pre-rasterization after a monolithic V+F in the same cache | 3,648 | |
| LTO relink of the four libraries | 3,534 | |

What it says:

- Device: fast linking, independent interpolation and VK_EXT_mesh_shader all present, so
  the device gate passes. (GPL with mesh stages was not exercised.)
- **The driver keys a pre-rasterization library on the whole pipeline layout**, including
  bindings only the fragment stage uses: a vertex library is reusable only with partners of
  identical descriptor layout. This is the measurement that makes per-stage descriptor sets
  (#411's approach, a translator-identity change) the real follow-up if cross-partner reuse
  is wanted. The render pass is **not** a cost (1 ms warm, 0.017 ms in-process): keying our
  map on it is cheap.
- A cold vertex library cost the same as a cold monolithic pipeline here (3,667 vs 3,663 ms),
  not more as on #411's 4090 — on this shader.
- **NVIDIA already reuses a compiled vertex stage inside monolithic pipelines** when the
  layout and the stage interface match: a new state or a bindings-only fragment variant of a
  known pair cost 0.006-7.4 ms monolithic. Libraries cut those to 0.01-7 ms; the win on
  state variants is real but small in absolute terms (the field's 1,327 "all modules
  cached" pipelines held 3.1 ms each, consistent with this). The field's 0.7-0.95 s builds
  are first compiles of a stage (or a changed interface/layout), which libraries do not
  avoid.
- Libraries and monolithic pipelines do not warm each other's cache entries.
- An LTO relink is a full compile (3.5 s): never on demand.

Not yet measured (next probe steps, when the machine is free): monolithic in a new process
with a fragment variant or state **never seen before** against a warm disk cache (the
cross-session comparison that decides whether libraries beat monolithic warming), and a
fragment variant that reads fewer varyings (an interface change).

## Harness plan (not run)

Binaries: A = dev at the merge base, B = this branch (same translator tag, so the same product
files). Every run gets its own driver cache, `--env __GL_SHADER_DISK_CACHE_PATH=<run>/glcache`,
seeded by copying a cache directory or empty, so A and B never warm each other's cache.

0. Sanity: B with the switch unset against A, one pair: same pipeline count, same held-ms
   classes within noise, no `pipeline_library` lines.
1. Cold (empty driver cache, `--cold-shader-cache` seeded with the pipeline-key product):
   4 interleaved pairs A/B(=1). Expect B no better, possibly worse: cold pre-rasterization
   libraries. Measures the cost side.
2. Warm, same route: driver cache from a previous run of the same binary and mode, 4 pairs.
   Boot preload time, held ms, `driver_ms` p50/p95/max.
3. Warm, other route (the decisive one): warm each binary's driver cache with the tour route,
   then run Daniel's field route, 4 pairs. Monolithic hits only exact keys; libraries hit per
   module.

Counters: held ms by AOT class (`Shader AOT (run time so far)`), `Nx2Perf async_pipelines:
... held_ms=` per pipeline, per-build `driver_ms` and `build=` class, the `pipeline_library`
summary (linked, fallback, built/reused/waited per part with ms and max), frames over 100 ms
and the longest frame (presents lines), MangoHud frame time in the measured windows (GPU cost
of unoptimized links), flash/flicker counts and screenshots (correctness), RSS and the cache
directory size.

Win: in 2/3, class 0 held ms ("all modules cached") near zero and linked `driver_ms` p95 under
a few ms; fewer frames over 100 ms; no frame-time regression beyond pair noise; zero fallbacks;
identical flash/flicker counts.
