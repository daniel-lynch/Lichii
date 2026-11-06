#!/bin/bash
# Launch the locally built Lichii with the shader dump on and MangoHud frametime logging,
# then keep this run's emulator log next to the frametime CSV.
set -u
OOT="$HOME/Downloads/OOT"
BIN="${BIN:-$HOME/dev/Lichii/build/bin/lichii}"
STAMP=$(date +%Y%m%d-%H%M%S)
export EDEN_NX2_SHADER_DUMP="$OOT/shaders"
# OoT fps: upstream #150's two unswitched commits cost ~10 % here (harness A/B 2026-11-05:
# 35.6 -> 39.4 fps with both off; vegetation unchanged). Default them off for this game;
# OOT_UPSTREAM_DEFAULTS=1 restores upstream behaviour for comparison.
if [ "${OOT_UPSTREAM_DEFAULTS:-0}" != "1" ]; then
  export EDEN_NX2_GFX_STORAGE_SIZE_LIKE_COMPUTE=0 EDEN_NX2_UNIFORM_BANK_REUPLOAD=0 EDEN_NX2_NATIVE_PREFLUSH=1  # preflush: +3.4 fps median, interleaved A/B 2026-11-06
fi
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
  AOT_INDEX="$OOT/aot/index/oot-aot-index.bin"
  if [ -f "$AOT_INDEX" ]; then
    export EDEN_NX2_AOT_TRANSLATE=1 EDEN_NX2_AOT_INDEX="$AOT_INDEX" EDEN_NX2_AOT_SEED=1
    export EDEN_NX2_AOT_MANIFEST="${OOT_AOT_MANIFEST:-$OOT/aot/manifests}"
    [ -n "${OOT_AOT_WORKERS:-}" ] && export EDEN_NX2_AOT_WORKERS="$OOT_AOT_WORKERS"
    [ -e "$EDEN_NX2_AOT_MANIFEST" ] || echo "OOT_AOT: no manifests at $EDEN_NX2_AOT_MANIFEST (index only; see $OOT/aot/README.md)" >&2
    echo "OOT_AOT: index $AOT_INDEX, manifests $EDEN_NX2_AOT_MANIFEST"
  else
    echo "OOT_AOT: no pool index at $AOT_INDEX, AOT left off. Build it once per game version with" >&2
    echo "  mkdir -p $OOT/aot/index && nice -n 10 python3 -I ~/dev/Lichii-aot2/tools/oot/container/aot_index.py <OoT .nup> $OOT/extract/zsdic $AOT_INDEX" >&2
    echo "  (then the manifests: see $OOT/aot/README.md)" >&2
  fi
fi
# MangoHud: overlay + CSV of fps/frametime, starts 5 s after launch, runs until exit.
export MANGOHUD=1
export MANGOHUD_CONFIG="fps,frametime,gpu_stats,cpu_stats,output_folder=$OOT/logs,autostart_log=5,log_duration=0,log_interval=100"
# PCIe + GPU load sampler (1 s) for the whole session, saved beside the logs.
nvidia-smi dmon -s tu -d 1 > "$OOT/logs/pcie-$STAMP.txt" 2>/dev/null &
SAMPLER=$!
"$BIN" "$@"
kill $SAMPLER 2>/dev/null
cp "$HOME/.local/share/eden/log/eden_log.txt" "$OOT/logs/eden_log-$STAMP.txt" 2>/dev/null
echo "log saved: $OOT/logs/eden_log-$STAMP.txt"
echo "frametime csv: $(ls -t "$OOT"/logs/*.csv 2>/dev/null | head -1)"
echo "shader dumps: $(ls "$OOT/shaders" | wc -l) files"
