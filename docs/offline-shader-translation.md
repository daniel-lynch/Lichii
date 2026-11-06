# Offline shader translation

Status: partial implementation. `ArchiveEnvironment` and class replay are implemented
with synthetic tests; CLI plumbing, archive discovery, scheduling and emission
recipes are not implemented. The command below is a proposed interface, not available.
The environment currently accepts vertex, fragment and geometry programs; compute,
tessellation, task and mesh require additional scalar recipes. Format 7 separates
these records from the sibling AOT branch's format-6 mesh-metadata layout.
No game bytes, manifests or captures belong in this repository.

The producer runs on the player's machine inside `eden-cli --precompile-shaders`,
after installation or a translator change. It must initialize the same Vulkan
profile, host translation info, device and settings as gameplay. It writes ordinary
translation entries and emitted SPIR-V variants through `TranslationCache::Store`,
using `Shader::TranslatorIdentity()` and the existing configuration digest. An entry
without a matching emitted variant does not meet the first-use goal.

Archive discovery must cover BNSH, BFSHA, BAHS and effect bundles, using the layouts
in `tools/nx2` and `tools/oot/container`. An optional external JSON index prioritizes
programs by tier. Its identity is container, BNSH offset, pool offset, byte size,
stage and FNV-1a-64 of the first 256 bytes. Check ranges and fingerprints against
the archive before trusting rows; bank-1 words in a manifest must match the bytes.
Bank 1 comes from program + control[0x40], length control[0x3c]. Do not infer its
extent from the neighboring program or the guest's larger binding window.

An owned archive environment captures native instructions using the runtime capture
rules and lowers at a canonical nonzero base. Compare lowering at a second base
across a 32-bit carry boundary; reject differing code or maps until relocation
records exist. Trap guards and unknown draw-state recipes fail closed. The stage-1
audit in the separate relocation worktree remains independent of this code.

The minimal guess machinery is copied from `feat/shader-aot-translate`, commits
`5207f8e148`, `101ce5d055` and `0457b9b2e2`: Color2D, A8B8G8R8_UNORM, noninteger,
direct storage pointer true, indirect false, viewport 1, no HLE replacement.
Handle words are placeholders only within handle computations. Record their source
recipes and validate their derived classes at the first draw. Every word consumed
by value must be present in bank 1; any other word aborts the candidate. Exact
queries remain exact even when the same word also forms a texture handle.

Emission recipes must specify RuntimeInfo and incoming descriptor Bindings, including
previous-stage interfaces. Reuse the renderer's translation/emission path and normal
variant keys. First draw always replays queries and checks the variant key. A mismatch
falls back to normal translation; this is speculative coverage, not a guarantee that
every possible draw avoids translation.

The coordinator should implement bounded admission before allocating program bytes:
worker count and total memory budget, per-program size limits, and a bounded queue.
The cache's in-memory entries and compressed modules count toward the budget too.
Append entries and variants using the existing writer; after a killed run, discard
an incomplete tail and resume by checking both entry and variant. Never regard a
metadata-only entry as completed. Emit counts for seen, translated, resumed, skipped
by reason, modules emitted, peak reserved memory and elapsed time.

Proposed coordinator command (only after CLI plumbing is implemented):

```sh
build-offline/bin/eden-cli --precompile-shaders \
  --shader-archives /absolute/path/to/extracted/archives \
  --shader-manifest /absolute/path/to/manifest.json \
  --shader-workers 4 --shader-memory-mib 512 \
  /absolute/path/to/game.nsp
```

Expected log contract (counts are placeholders, not measured results):

```text
Translation cache: translator identity source <hash> (... file tag <tag>)
Offline translation: seen=N translated=N resumed=N modules=N elapsed_ms=N
Offline translation: skipped reason=base_dependent count=N
Offline translation: skipped reason=missing_constant count=N
Offline translation: skipped reason=unsupported_stage count=N
Offline translation: skipped reason=unsupported_lowering count=N
```

The coordinator must check a second run emits no already-completed variants, then
interrupt and resume a run, and finally run gameplay with
`EDEN_NX2_TRANSLATION_CACHE_VERIFY=1`. Record first-use entry hits, variant hits,
replay misses and verification differences. Do not run real archives or either
emulator frontend in the implementation sandbox.

## Implementation validation

Clang 22 Release builds of `lichii`, `eden-cli` and `tests` passed under the shared
emulator lock. `tests '[archive_environment],[translation_cache]'` passed 147
assertions in 17 cases, including synthetic translation and SPIR-V variant lookup.
The copied prebuilt SDL3 package was selected with `SDL3_CUSTOM_DIR` and
`YUZU_USE_BUNDLED_SDL3=OFF`: the ON path unconditionally downloads a checksum, which
this sandbox could not fetch. Other requested configuration flags match build-test.
No real-archive or emulator run was performed. Git metadata is outside the sandbox's
writable roots, so these changes could not be committed here.
