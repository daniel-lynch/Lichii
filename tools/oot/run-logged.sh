#!/bin/bash
# Launch the locally built Lichii with the shader dump on and MangoHud frametime logging,
# then keep this run's emulator log next to the frametime CSV.
set -u
OOT="$HOME/Downloads/OOT"
BIN="${BIN:-$HOME/dev/Lichii/build-test/bin/lichii}"  # the dev play binary (build-test is built from the dev worktree); BIN=… overrides
STAMP=$(date +%Y%m%d-%H%M%S)
# Shader dump: off for play (the 2026-11-06 field session wrote 1,342 dump files from the GPU thread);
# OOT_SHADER_DUMP=1 turns it back on for analysis runs.
if [ "${OOT_SHADER_DUMP:-0}" = "1" ]; then export EDEN_NX2_SHADER_DUMP="$OOT/shaders"; fi
# OoT fps: upstream #150's two unswitched commits cost ~10 % here (harness A/B 2026-11-05:
# 35.6 -> 39.4 fps with both off; vegetation unchanged). Default them off for this game;
# OOT_UPSTREAM_DEFAULTS=1 restores upstream behaviour for comparison.
if [ "${OOT_UPSTREAM_DEFAULTS:-0}" != "1" ]; then
  export EDEN_NX2_GFX_STORAGE_SIZE_LIKE_COMPUTE=0 EDEN_NX2_UNIFORM_BANK_REUPLOAD=0 EDEN_NX2_NATIVE_PREFLUSH="${OOT_PREFLUSH:-1}"  # preflush: +3.4 fps median, interleaved A/B 2026-11-06
fi
# Pipeline eviction: upstream destroys graphics pipelines unused for 9000 frames (~2.5 min at 60),
# so re-entering an area rebuilds them on the GPU thread (upstream issue #293 — stutter).
# Off for this game; OOT_EVICT=1 restores upstream behaviour.
if [ "${OOT_EVICT:-0}" != "1" ]; then export EDEN_NX2_PIPELINE_EVICT=0; fi
# OOT_GPU=fast|accurate pins the per-game GPU Mode and reactive flushing before boot
# (fast = GPU Mode Fast + reactive flushing off; accurate = Accurate + flushing on).
# OOT_FLUSH=0|1 overrides the flushing half. Unset = leave the config alone.
# The emulator rewrites its config on exit, so this only runs when no instance is up.
if [ -n "${OOT_GPU:-}" ]; then
  if pgrep -f 'bin/lichii|lichii-linux' >/dev/null; then
    echo "OOT_GPU: an emulator instance is running; config not changed (quit it first)" >&2
  else
    case "$OOT_GPU" in fast) acc=0; fl=false;; accurate) acc=1; fl=true;; *) echo "OOT_GPU must be fast or accurate" >&2; exit 2;; esac
    case "${OOT_FLUSH:-}" in 0) fl=false;; 1) fl=true;; esac
    INI="$HOME/.config/eden/custom/0400486024DEC000.ini"
    python3 -I - "$INI" "$acc" "$fl" <<'PY'
import re,sys
p,acc,fl=sys.argv[1:]; s=open(p).read()
def pin(s,key,val):
    s=re.sub(r'(?m)^'+re.escape(key)+r'\\use_global=.*$', key+r'\\use_global=false', s)
    s=re.sub(r'(?m)^'+re.escape(key)+r'\\default=.*$',    key+r'\\default=false',    s)
    s=re.sub(r'(?m)^'+re.escape(key)+r'=.*$',             key+'='+val,                s)
    return s
s=pin(s,'gpu_accuracy',acc); s=pin(s,'use_reactive_flushing',fl)
open(p,'w').write(s)
print("pinned: gpu_accuracy=%s (%s) use_reactive_flushing=%s"%(acc,'Fast' if acc=='0' else 'Accurate',fl))
PY
  fi
fi
# OOT_REJECT=1 applies the issue-101 workaround: rejects the bloom upsample pair and the
# half-res overlay mesh shader (clean Link/foreground, less white glow; no bloom, no river surface).
if [ "${OOT_REJECT:-0}" = "1" ]; then
  export EDEN_NX2_REJECT_HASH=f2b14c43ca0e4300,96dd01c5b0361fcb,c72b6ea746c00433
fi
# OOT_CAPTURE=1 arms a one-frame readback: while playing, run
#   touch ~/Downloads/OOT/framedump/latest/capture.now
# when the artifact is on screen; the dump lands in that directory (checked every 30 frames).
if [ "${OOT_CAPTURE:-0}" = "1" ]; then
  mkdir -p "$OOT/framedump/$STAMP"; ln -sfn "$OOT/framedump/$STAMP" "$OOT/framedump/latest"
  export EDEN_NX2_FRAME_DUMP="$OOT/framedump/$STAMP" EDEN_NX2_FRAME_DUMP_AT=file EDEN_NX2_FRAME_DUMP_SAMPLED=1
  export EDEN_NX2_FRAME_DUMP_COUNT="${OOT_CAPTURE_FRAMES:-1}"   # consecutive frames per trigger (needs the burst patch)
fi
# OOT_AOT=1 turns on shader AOT translation (default off until feat/shader-aot-manifest merges;
# needs a build of that branch, e.g. BIN=~/dev/Lichii-aot2/build-aot2/bin/lichii). Its inputs are
# built from the game's own files into ~/Downloads/OOT/aot/ (see its README.md), never here:
# the pool index, and a directory of static scene manifests the emulator loads and unites.
# OOT_AOT_MANIFEST=<file or dir> overrides the manifest set; OOT_AOT_WORKERS=N sets the
# translation threads (unset = the emulator's default, 2).
if [ "${OOT_AOT:-1}" = "1" ]; then  # default ON since dev 18832d80a5 (AOT merged; -36% frame-blocking cold)
  AOT_INDEX="$OOT/aot/index/${OOT_AOT_INDEX_FILE:-oot-aot-index-sharcb.bin}"  # sharcb index (engine shaders) since AOT branch 3; oot-aot-index.bin = BNSH-only
  if [ -f "$AOT_INDEX" ]; then
    export EDEN_NX2_AOT_TRANSLATE=1 EDEN_NX2_AOT_INDEX="$AOT_INDEX" EDEN_NX2_AOT_SEED=1
    export EDEN_NX2_AOT_MANIFEST="${OOT_AOT_MANIFEST:-$OOT/aot/manifests}"  # full directory: per-scene selection (AOT branch 3) picks the scene the game enters; OOT_AOT_MANIFEST=$OOT/aot/manifests-active restores the global+Kokiri union
    [ -n "${OOT_AOT_WORKERS:-}" ] && export EDEN_NX2_AOT_WORKERS="$OOT_AOT_WORKERS"
    [ -e "$EDEN_NX2_AOT_MANIFEST" ] || echo "OOT_AOT: no manifests at $EDEN_NX2_AOT_MANIFEST (index only; see $OOT/aot/README.md)" >&2
    echo "OOT_AOT: index $AOT_INDEX, manifests $EDEN_NX2_AOT_MANIFEST"
  else
    echo "OOT_AOT: no pool index at $AOT_INDEX, AOT left off. Build it once per game version with" >&2
    echo "  mkdir -p $OOT/aot/index && nice -n 10 python3 -I ~/dev/Lichii-aot2/tools/oot/container/aot_index.py <OoT .nup> $OOT/extract/zsdic $AOT_INDEX" >&2
    echo "  (then the manifests: see $OOT/aot/README.md)" >&2
  fi
fi

# OOT_PRODUCT: offline shader product (a translation cache written by eden-cli --precompile-shaders).
# Every vulkan_translations.*.bin under $OOT/offline-translate/product-live/ is copied (reflink when
# the filesystem allows, else a real copy) into the live shader cache dir if a file of that name is
# not there yet; the runtime uses the one whose translator tag matches this build and logs the tags
# it ignores. OOT_PRODUCT=0 skips the step.
if [ "${OOT_PRODUCT:-1}" = "1" ]; then
  # OOT_PRODUCT_DIR=<dir> installs from a staged product directory instead (candidate builds).
  PRODUCT_DIR="${OOT_PRODUCT_DIR:-$OOT/offline-translate/product-live}"
  LIVE_SHADER_DIR="$HOME/.cache/eden/shader/0400486024dec000"
  if [ -d "$PRODUCT_DIR" ] && [ -d "$LIVE_SHADER_DIR" ]; then
    for f in "$PRODUCT_DIR"/vulkan_translations.*.bin; do
      [ -f "$f" ] || continue
      b=$(basename "$f")
      # A product can be re-produced under the same name (same build, more recipes): the
      # producer writes <name>.sha256 beside it and the install records it in <name>.installed;
      # a changed sha replaces the live copy (its runtime records are already in the new product).
      want=$(cat "$f.sha256" 2>/dev/null | cut -c1-64)
      have=$(cat "$LIVE_SHADER_DIR/$b.installed" 2>/dev/null | cut -c1-64)
      if [ ! -e "$LIVE_SHADER_DIR/$b" ]; then
        cp --reflink=auto "$f" "$LIVE_SHADER_DIR/$b" && echo "offline shader product installed: $b" >&2 \
          && [ -n "$want" ] && echo "$want" > "$LIVE_SHADER_DIR/$b.installed"
      elif [ -n "$want" ] && [ "$want" != "$have" ]; then
        cp --reflink=auto "$f" "$LIVE_SHADER_DIR/$b.new" && mv -f "$LIVE_SHADER_DIR/$b.new" "$LIVE_SHADER_DIR/$b" \
          && echo "$want" > "$LIVE_SHADER_DIR/$b.installed" && echo "offline shader product updated: $b" >&2
      fi
    done
  fi
fi
# Pipeline cache version: the emulator opens <shader dir>/<title>/vulkan.fork-v<N>.bin, N its build's
# VulkanPipelineCacheVersion (src/video_core/pipeline_cache_files.h: 53 up to dev c4507a3910, 54 since
# upstream #432), and DELETES that file at boot when its header holds another version. So every step
# below works on version N's files only, and the tool refuses to write a file whose name and header
# disagree. N comes from the binary itself: builds since the nx2 a10e5f5569 merge carry the string
# "lichii-pipeline-cache=vulkan.fork-v<N>.bin" (VulkanPipelineCacheMarker, logged at boot), so a
# copied or renamed binary still answers; a binary without it is an older build: 53.
# OOT_PIPELINE_VERSION=N overrides. A wrong N costs that launch its warm cache, never a file: the
# emulator only ever opens (and deletes) its own version's name, and the files of every other
# version are left as they are.
PC_VERSION="${OOT_PIPELINE_VERSION:-}"
if [ -z "$PC_VERSION" ]; then
  PC_VERSION=$(grep -ao 'lichii-pipeline-cache=vulkan\.fork-v[0-9][0-9]*\.bin' "$BIN" 2>/dev/null | head -1 | sed 's/.*fork-v\([0-9]*\)\.bin$/\1/')
  [ -n "$PC_VERSION" ] || PC_VERSION=53
fi
case "$PC_VERSION" in ''|0*|*[!0-9]*) echo "pipeline keys: cache version '$PC_VERSION' is not a version number; using 53" >&2; PC_VERSION=53;; esac
echo "pipeline keys: $(basename "$BIN") uses cache version v$PC_VERSION" >&2
# OOT_PIPELINES: offline pipeline-key product ($OOT/offline-translate/pipeline-live/vulkan.fork-v<N>.bin
# + its .sha256, N as above): pipeline-tools/pipeline_cache_merge.py appends the product's records whose key the
# live pipeline cache lacks, before the emulator starts, so those pipelines are built at boot instead
# of mid-play. The live cache's records stay a byte-identical prefix, with one exception: a record
# cut short at its end (a crash mid-append) is dropped, because the emulator deletes the WHOLE cache
# when it finds one; the backup (<live>.bak-*, 3 kept) keeps those bytes. Runs when the product's
# sha differs from the one in <live>.merged, or the live cache was deleted and recreated since
# (inode changed); otherwise only `repair` runs (about a second: it reads the cache), which drops a
# cut-short tail a crashed session left -- with the same checks and a backup. Session lock: every
# launch holds <live>.session.lock on fd 9, which the emulator inherits -- exclusive for the merge,
# then shared while the game runs -- so a merge never runs under an emulator another launch
# started (that launch skips it). The tool refuses rather than
# risk the cache -- 3: an emulator, another session or another merge is using it; 2: live cache
# unusable (or a version mismatch); 4: product failed validation -- and every refusal only skips the
# merge: the game launches anyway. No product of version N: nothing merged (repair still runs).
# OOT_PIPELINES=0 skips the carry-over, the merge and the repair (the session lock is still taken).
# Carry-over to a new version (53 -> 54: the record format is the same, only the header's version
# changed). The old file vulkan.fork-v<N-1>.bin is only ever read -- never changed, renamed or
# removed: it is the backup, and still the cache of older binaries. It is carried whenever it is not
# the file <live>.carried-v<N-1> recorded (inode, size, mtime) at the last carry, so a carry that
# did not happen (busy, refused, OOT_PIPELINES=0, a v<N> binary started some other way -- which
# then makes its own vulkan.fork-v<N>.bin, from a stale v52 file if one is there) is retried at the
# next launch, and records a v<N-1> binary added since are carried too:
#   - no vulkan.fork-v<N>.bin yet: `convert --live` writes it = the old file's whole records under
#     the new header, verified, installed only while the name is still free (RENAME_NOREPLACE),
#     with merge's locks and emulator checks plus a lease on the old file;
#   - vulkan.fork-v<N>.bin exists: `convert` writes the old file's records as a temporary v<N>
#     product (.carry.vulkan.fork-v<N>.bin + .sha256, removed afterwards) and `merge` appends the
#     ones v<N> lacks, with all of merge's guarantees (v<N>'s records stay a byte-identical prefix,
#     backup first, atomic exchange).
# Then the product merge above runs on the new file, as usual (and takes its own backup). Also
# carried, as the emulator does for v52: vulkan_used.fork-v<N-1>.bin (the recently used pipeline
# list that picks what is built at boot; its format has no version), copied when v<N> has none.
LIVE_PC="$HOME/.cache/eden/shader/0400486024dec000/vulkan.fork-v$PC_VERSION.bin"
LIVE_PREV="${LIVE_PC%/*}/vulkan.fork-v$((PC_VERSION - 1)).bin"
if mkdir -p "${LIVE_PC%/*}" 2>/dev/null && exec 9>>"$LIVE_PC.session.lock"; then
  if flock -n -x 9; then
    PC_TOOL="$OOT/offline-translate/pipeline-tools/pipeline_cache_merge.py"
    PC_PRODUCT="$OOT/offline-translate/pipeline-live/vulkan.fork-v$PC_VERSION.bin"
    CARRY_STAMP="$LIVE_PC.carried-v$((PC_VERSION - 1))"
    prev_id=$(stat -c '%i %s %Y' "$LIVE_PREV" 2>/dev/null)
    if [ "${OOT_PIPELINES:-1}" = "1" ] && [ -s "$LIVE_PREV" ] && [ ! -L "$LIVE_PREV" ] \
        && [ "$prev_id" != "$(cat "$CARRY_STAMP" 2>/dev/null)" ]; then
      if [ ! -e "$LIVE_PC" ] && [ ! -L "$LIVE_PC" ]; then
        echo "pipeline keys: first v$PC_VERSION launch; carrying the v$((PC_VERSION - 1)) cache over" >&2
        timeout -s INT -k 10 300 python3 -I "$PC_TOOL" convert --live --to "$PC_VERSION" "$LIVE_PREV" "$LIVE_PC" --session-lock
        crc=$?
      else
        echo "pipeline keys: adding the v$((PC_VERSION - 1)) cache's records to the v$PC_VERSION cache (not carried since it changed)" >&2
        CARRY="${LIVE_PC%/*}/.carry.vulkan.fork-v$PC_VERSION.bin"
        rm -f "$CARRY" "$CARRY.sha256"
        timeout -s INT -k 10 300 python3 -I "$PC_TOOL" convert --to "$PC_VERSION" "$LIVE_PREV" "$CARRY" \
          && timeout -s INT -k 10 300 python3 -I "$PC_TOOL" merge --version "$PC_VERSION" --live "$LIVE_PC" --product "$CARRY" --session-lock
        crc=$?
        rm -f "$CARRY" "$CARRY.sha256"
      fi
      case $crc in
        0) echo "$prev_id" > "$CARRY_STAMP.tmp" && mv -f "$CARRY_STAMP.tmp" "$CARRY_STAMP";;
        3) echo "pipeline keys: emulator busy, v$((PC_VERSION - 1)) cache not carried over (retried next launch)" >&2;;
        *) echo "pipeline keys: v$((PC_VERSION - 1)) cache not carried over (exit $crc, reason above; retried next launch); launching anyway" >&2;;
      esac
    fi
    USED_PC="${LIVE_PC%/*}/vulkan_used.fork-v$PC_VERSION.bin" USED_PREV="${LIVE_PC%/*}/vulkan_used.fork-v$((PC_VERSION - 1)).bin"
    if [ "${OOT_PIPELINES:-1}" = "1" ] && [ ! -e "$USED_PC" ] && [ ! -L "$USED_PC" ] && [ -s "$USED_PREV" ] && [ ! -L "$USED_PREV" ]; then
      cp "$USED_PREV" "$USED_PC.carry.$$" 2>/dev/null && mv -n "$USED_PC.carry.$$" "$USED_PC" \
        && echo "pipeline keys: used-pipeline list carried over to v$PC_VERSION" >&2
      rm -f "$USED_PC.carry.$$"
    fi
    # One-time drop of stale #410 records. Since upstream #410 (b8d81e2097) the emulator fails to build
    # some pipelines a cache recorded before it (FileEnvironment::IsCbufStorageBufferPointer, "Uncached
    # storage pointer classification") and logs them as failed at every boot. The list
    # ($OOT/offline-translate/pipeline-live/drop-keys-410.txt, g:/c:/cp: lines; OOT_DROP_KEYS=<file>
    # overrides) names them; `filter --drop-keys` writes the cache without them (and without duplicate,
    # invalid or cut-short records) to a temp file beside it, `check` verifies that file, and it
    # replaces the live file only when the live file is unchanged since its backup
    # (<live>.bak-drop410-<stamp>, kept) and no emulator runs. Inside the exclusive session lock, like
    # the merge. <live>.dropped-410 records it, so it runs once per cache file (a refusal is retried
    # next launch). A later carry from a changed v<N-1> file can bring such records back; the product
    # never does (it is built without them). OOT_DROP_410=0 skips the step.
    DROP_KEYS="${OOT_DROP_KEYS:-$OOT/offline-translate/pipeline-live/drop-keys-410.txt}" DROP_STAMP="$LIVE_PC.dropped-410"
    if [ "${OOT_PIPELINES:-1}" = "1" ] && [ "${OOT_DROP_410:-1}" = "1" ] && [ -f "$DROP_KEYS" ] \
        && [ -s "$LIVE_PC" ] && [ ! -L "$LIVE_PC" ] && [ ! -e "$DROP_STAMP" ]; then
      DROP_TMP="${LIVE_PC%/*}/.drop410.vulkan.fork-v$PC_VERSION.bin" DROP_BAK="$LIVE_PC.bak-drop410-$STAMP"
      DROP_LOG="$OOT/logs/drop410-$STAMP.log"
      rm -f "$DROP_TMP" "$DROP_TMP.sha256"
      before=$(stat -c '%i %s %Y' "$LIVE_PC")
      if pgrep -x 'lichii|lichii-linux' >/dev/null; then
        echo "pipeline keys: an emulator is running; stale #410 records not dropped (retried next launch)" >&2
      elif cp --reflink=auto "$LIVE_PC" "$DROP_BAK" \
          && timeout -s INT -k 10 300 python3 -I "$PC_TOOL" filter --version "$PC_VERSION" --allow-missing \
               --drop-keys "$DROP_KEYS" "$DROP_TMP" "$LIVE_PC" > "$DROP_LOG" 2>&1 \
          && timeout -s INT -k 10 300 python3 -I "$PC_TOOL" check --version "$PC_VERSION" "$DROP_TMP" >> "$DROP_LOG" 2>&1 \
          && [ "$(stat -c '%i %s %Y' "$LIVE_PC")" = "$before" ] && ! pgrep -x 'lichii|lichii-linux' >/dev/null \
          && mv -f "$DROP_TMP" "$LIVE_PC"; then
        echo "$before -> $(stat -c '%i %s %Y' "$LIVE_PC") $(basename "$DROP_KEYS") $(sha256sum < "$DROP_KEYS" | cut -c1-16)" > "$DROP_STAMP"
        grep -v '^  not found:' "$DROP_LOG" | head -2 | sed 's/^/pipeline keys: #410 drop: /' >&2
        echo "pipeline keys: stale #410 records dropped from the v$PC_VERSION cache (backup $DROP_BAK)" >&2
      else
        grep -v '^  not found:' "$DROP_LOG" 2>/dev/null | head -5 >&2
        rm -f "$DROP_BAK"  # the live file was not replaced: its backup is not needed
        echo "pipeline keys: stale #410 records not dropped (reason above; retried next launch); launching anyway" >&2
      fi
      rm -f "$DROP_TMP" "$DROP_TMP.sha256"
    fi
    rc=none
    if [ "${OOT_PIPELINES:-1}" = "1" ] && [ ! -f "$PC_PRODUCT" ]; then
      echo "pipeline keys: no v$PC_VERSION product ($PC_PRODUCT); nothing merged" >&2
    fi
    if [ "${OOT_PIPELINES:-1}" = "1" ] && [ -f "$PC_PRODUCT" ]; then
      want=$(cut -c1-64 "$PC_PRODUCT.sha256" 2>/dev/null)
      have=$(cut -c1-64 "$LIVE_PC.merged" 2>/dev/null)
      ino=$(stat -c %i "$LIVE_PC" 2>/dev/null)
      have_ino=$(sed -n 's/.* ino=//p' "$LIVE_PC.merged" 2>/dev/null)
      if [ -z "$want" ] || [ "$want" != "$have" ] || [ "$ino" != "$have_ino" ]; then
        timeout -s INT -k 10 300 python3 -I "$PC_TOOL" merge --version "$PC_VERSION" --live "$LIVE_PC" --product "$PC_PRODUCT" --session-lock
        rc=$?
        case $rc in
          0) echo "$want ino=$(stat -c %i "$LIVE_PC" 2>/dev/null)" > "$LIVE_PC.merged.tmp" \
               && mv -f "$LIVE_PC.merged.tmp" "$LIVE_PC.merged";;
          3) echo "pipeline keys: emulator busy, skipped" >&2;;
          *) echo "pipeline keys: not merged (exit $rc, reason above); launching anyway" >&2;;
        esac
      fi
    fi
    # No merge ran (or it refused the product / an unreadable live cache): still repair a
    # cut-short tail a crashed session left, which the emulator would answer by deleting the cache.
    if [ "${OOT_PIPELINES:-1}" = "1" ] && [ "$rc" != 0 ] && [ "$rc" != 3 ] && [ -s "$LIVE_PC" ]; then
      timeout -s INT -k 10 120 python3 -I "$PC_TOOL" repair --version "$PC_VERSION" --live "$LIVE_PC" --session-lock \
        | grep -v '^live .*records$\|^nothing to repair' >&2 9>&-  # 9>&-: the filter holds no session fd
    fi
  elif [ "${OOT_PIPELINES:-1}" = "1" ]; then
    echo "pipeline keys: emulator busy, skipped (another session holds the cache)" >&2
  fi
  flock -s -w 60 9 || echo "pipeline keys: session lock not taken; launching anyway" >&2
fi
# MangoHud: overlay + CSV of fps/frametime, starts 5 s after launch, runs until exit.
# MangoHud: OOT_MANGOHUD=0 disables the overlay and the frametime log entirely. The NVIDIA GPU
# sampler (gpu_stats) aborts the process inside libMangoHud.so on this host since 2026-11-06
# (NVIDIA::get_samples_and_copy -> free(): invalid pointer), so it is off; OOT_MANGOHUD_GPU_STATS=1
# turns it back on once MangoHud is fixed.
# NVIDIA driver shader cache: large and never cleaned up, so pipelines compiled once stay warm
# across sessions (the driver cache is where the 0.7-0.9 s runtime builds end up). OOT_GL_CACHE=0 leaves the driver defaults.
if [ "${OOT_GL_CACHE:-1}" = "1" ]; then export __GL_SHADER_DISK_CACHE_SIZE=8589934592 __GL_SHADER_DISK_CACHE_SKIP_CLEANUP=1; fi
export MANGOHUD="${OOT_MANGOHUD:-0}"  # default off since 2026-11-06: the in-process layer aborts lichii at GUI start on this host
# Stereo for this game: the host advertises 6 channels, the game then renders 5.1 and dialogue
# (centre channel) lands ~4.5 dB low in our downmix. Field-verified 2026-11-07: forcing stereo
# brings the voice lines back. OOT_AUDIO_STEREO=0 restores the 5.1 path.
if [ "${OOT_AUDIO_STEREO:-1}" = "1" ]; then export EDEN_NX2_AUDIO_STEREO=1; fi
MH_CFG="fps,frametime,cpu_stats,output_folder=$OOT/logs,autostart_log=5,log_duration=0,log_interval=100"
[ "${OOT_MANGOHUD_GPU_STATS:-0}" = "1" ] && MH_CFG="$MH_CFG,gpu_stats"
export MANGOHUD_CONFIG="$MH_CFG"
# PCIe + GPU load sampler (1 s) for the whole session, saved beside the logs.
nvidia-smi dmon -s tu -d 1 > "$OOT/logs/pcie-$STAMP.txt" 2>/dev/null 9>&- &  # 9>&-: only the game holds the session lock
SAMPLER=$!
# Field test in progress: every harness/build agent yields to this marker, so nothing else uses the
# GPU or the emulator lock while the game is up. Removed when the game exits (trap covers signals).
FIELD_MARKER="$OOT/harness-runs/.aot-priority"
touch "$FIELD_MARKER" 2>/dev/null
trap 'rm -f "$FIELD_MARKER"' EXIT
"$BIN" "$@"
kill $SAMPLER 2>/dev/null
cp "$HOME/.local/share/eden/log/eden_log.txt" "$OOT/logs/eden_log-$STAMP.txt" 2>/dev/null
echo "log saved: $OOT/logs/eden_log-$STAMP.txt"
echo "frametime csv: $(ls -t "$OOT"/logs/*.csv 2>/dev/null | head -1)"
echo "shader dumps: $(ls "$OOT/shaders" | wc -l) files"
