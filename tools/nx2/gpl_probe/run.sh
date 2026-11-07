#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs gpl_probe in separate processes against a private NVIDIA disk cache, so that "cold"
# and "warm" mean what they say and the user's ~/.cache/nvidia is never touched.
#
#   tools/nx2/gpl_probe/run.sh OUT_DIR [WORK]
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=$1
work=${2:-400}
mkdir -p "$out"
python3 -I "$here/gen_shaders.py" "$out/spv" --work "$work"
g++ -std=c++20 -O2 -o "$out/gpl_probe" "$here/gpl_probe.cpp" -lvulkan

export __GL_SHADER_DISK_CACHE=1
export __GL_SHADER_DISK_CACHE_SKIP_CLEANUP=1
probe() { # cache-name label steps...
    local cache=$1 label=$2
    shift 2
    mkdir -p "$out/cache-$cache"
    echo "## $label (cache $cache)"
    __GL_SHADER_DISK_CACHE_PATH="$out/cache-$cache" "$out/gpl_probe" "$out/spv" "$@"
}
fresh() { rm -rf "$out/cache-$1"; }

{
    fresh mono
    probe mono "monolithic, cold" mono mono_blend mono_cull mono_rp_b mono_fs2
    probe mono "monolithic, new process" mono mono_blend mono_cull mono_rp_b mono_fs2

    fresh lib
    probe lib "libraries, cold" lib_vi lib_pre lib_fs lib_fo link lib_fo_blend link_blend \
        lib_fs2 link_fs2 lib_pre_cull link_cull
    probe lib "libraries, new process" lib_vi lib_pre lib_fs lib_fo link lib_fo_blend \
        link_blend lib_fs2 link_fs2 lib_pre_cull link_cull
    # Does the driver cache key a pre-rasterization library on the layout or render pass?
    probe lib "pre-rasterization, other layout / render pass" lib_pre_layout2 lib_pre_rp_b

    fresh dep
    probe dep "in one process: layout / render pass after a cold pre-rasterization" \
        lib_pre lib_pre_layout2 lib_pre_rp_b lib_pre_cull

    fresh mixed
    probe mixed "monolithic first, then libraries (does one warm the other?)" \
        mono lib_pre lib_fs
    GPL_PROBE_RETAIN=1 probe lto "LTO relink (retained libraries)" \
        lib_vi lib_pre lib_fs lib_fo link link_lto
} 2>&1 | tee "$out/probe.txt"
