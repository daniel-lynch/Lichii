#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read-only field audit of x86-64 translation cache v9/v10 files.

No game data is embedded. With --pipeline and --hash-helper, recover stage-key
preimages from the matching pipeline cache and verify them against live digests.
Without a pipeline cache, missing-key components cannot be recovered.

Build the small CityHash/ABI helper with a configured CMake build (use the shared
emulator lock on a machine running the validation harness):
  flock /path/to/.emulator.lock python3 tools/nx2/translation_cache_audit.py \
    --build-helper build-offline --hash-helper /tmp/translation-cache-audit.so
Then pass --pipeline /private/vulkan.fork-v53.bin and the same --hash-helper when
comparing the two translation cache paths. The helper never runs an emulator.
"""
import argparse
import collections
import ctypes
import json
import struct
import shlex
import subprocess
from pathlib import Path

# Serialized field order and x86-64 element sizes, independent of host Python ABI.
SCHEMA = """POD is_task 1
POD is_mesh 1
POD uses_workgroup_id 1
POD uses_local_invocation_id 1
POD uses_invocation_id 1
POD uses_invocation_info 1
POD uses_sample_id 1
POD uses_is_helper_invocation 1
POD uses_subgroup_invocation_id 1
POD uses_subgroup_shuffles 1
POD uses_quad_shuffles 1
POD uses_patches 30
POD interpolation 128
POD loads 64
POD stores 64
POD passthrough 64
MAP legacy_stores_mapping 8
POD loads_indexed_attributes 1
POD stores_frag_color 8
POD stores_sample_mask 1
POD stores_frag_depth 1
POD stores_tess_level_outer 1
POD stores_tess_level_inner 1
POD stores_indexed_attributes 1
POD stores_global_memory 1
POD uses_local_memory 1
POD uses_fp16 1
POD uses_fp64 1
POD uses_fp16_denorms_flush 1
POD uses_fp16_denorms_preserve 1
POD uses_fp32_denorms_flush 1
POD uses_fp32_denorms_preserve 1
POD uses_int8 1
POD uses_int16 1
POD uses_int64 1
POD uses_image_1d 1
POD uses_sampled_1d 1
POD uses_sparse_residency 1
POD uses_demote_to_helper_invocation 1
POD uses_subgroup_vote 1
POD uses_subgroup_mask 1
POD uses_fswzadd 1
POD uses_derivatives 1
POD uses_typeless_image_reads 1
POD uses_typeless_image_writes 1
POD uses_image_buffers 1
POD uses_shared_increment 1
POD uses_shared_decrement 1
POD uses_global_increment 1
POD uses_global_decrement 1
POD uses_atomic_f32_add 1
POD uses_atomic_f16x2_add 1
POD uses_atomic_f16x2_min 1
POD uses_atomic_f16x2_max 1
POD uses_atomic_f32x2_add 1
POD uses_atomic_f32x2_min 1
POD uses_atomic_f32x2_max 1
POD uses_atomic_s32_min 1
POD uses_atomic_s32_max 1
POD uses_int64_bit_atomics 1
POD uses_global_memory 1
POD requires_coherent_global_memory 1
POD requires_coherent_image_memory 1
POD native_global_bounds 1
POD native_global_stats 32
VEC native_indirect_slots 12
POD uses_atomic_image_u32 1
POD uses_shadow_lod 1
POD uses_rescaling_uniform 1
POD uses_cbuf_indirect 1
POD uses_render_area 1
POD used_constant_buffer_types 4
POD used_storage_buffer_types 4
POD used_indirect_cbuf_types 4
POD constant_buffer_mask 4
POD constant_buffer_used_sizes 72
POD nvn_buffer_base 4
POD nvn_buffer_used 8
POD requires_layer_emulation 1
POD emulated_layer 8
POD used_clip_distances 4
VEC constant_buffer_descriptors 8
VEC storage_buffers_descriptors 80
VEC texture_buffer_descriptors 40
VEC image_buffer_descriptors 28
VEC texture_descriptors 44
VECVEC handle_programs 4
VEC image_descriptors 36
"""
SCHEMA = [(kind, name, int(size)) for kind, name, size in
          (line.split() for line in SCHEMA.splitlines())]
STAGES = {0: "vertex", 1: "tess_control", 2: "tess_eval", 3: "geometry",
          4: "fragment", 5: "compute", 6: "vertex_a", 7: "task", 8: "mesh"}

# Field offsets inside the padded descriptor objects in this format.
DESCRIPTORS = {
    "storage_buffers_descriptors": [
        ("cbuf_index", 0, 4), ("cbuf_offset", 4, 4), ("count", 8, 4),
        ("is_written", 12, 1), ("native_indirect", 13, 1), ("<padding>", 14, 2),
        ("indirect_offset", 16, 4), ("write_extent.bounded", 20, 1),
        ("write_extent.guarded", 21, 1), ("write_extent.<padding>", 22, 2),
        ("write_extent.const_end", 24, 4), ("write_extent.local_stride", 28, 12),
        ("write_extent.group_stride", 40, 12), ("write_extent.guard_const", 52, 4),
        ("write_extent.guard_local_stride", 56, 12),
        ("write_extent.guard_group_stride", 68, 12)],
    "texture_descriptors": [("type", 0, 4), ("is_depth", 4, 1),
        ("is_multisample", 5, 1), ("is_integer", 6, 1), ("has_secondary", 7, 1)] +
        [(name, 8 + index * 4, 4) for index, name in enumerate([
            "cbuf_index", "cbuf_offset", "shift_left", "secondary_cbuf_index",
            "secondary_cbuf_offset", "secondary_shift_left", "count", "size_shift",
            "handle_program"])],
}
PADDING = {"texture_buffer_descriptors": [(1, 3)],
           "image_buffer_descriptors": [(7, 1)],
           "image_descriptors": [(11, 1), (29, 3)]}


def value(data):
    if len(data) <= 8:
        return hex(int.from_bytes(data, "little"))
    return [hex(x[0]) for x in struct.iter_unpack("<I", data)] if len(data) % 4 == 0 else data.hex()


def read_info(data, pos):
    fields = {}
    for kind, name, size in SCHEMA:
        if kind == "POD":
            fields[name] = value(data[pos:pos + size])
            pos += size
            continue
        count, = struct.unpack_from("<I", data, pos)
        pos += 4
        fields[name + ".count"] = count
        for i in range(count):
            length = size
            if kind == "VECVEC":
                length, = struct.unpack_from("<I", data, pos)
                pos += 4
                length *= size
            chunk = data[pos:pos + length]
            if len(chunk) != length:
                raise ValueError("truncated Info vector")
            label = f"{name}[{i}]"
            if name in DESCRIPTORS:
                for member, offset, width in DESCRIPTORS[name]:
                    fields[label + "." + member] = value(chunk[offset:offset + width])
            elif name in PADDING:
                canonical = bytearray(chunk)
                for offset, width in PADDING[name]:
                    fields[f"{label}.<padding@{offset}>"] = value(chunk[offset:offset + width])
                    canonical[offset:offset + width] = bytes(width)
                fields[label] = value(canonical)
            else:
                fields[label] = value(chunk)
            pos += length
    return fields, pos


def read_cache(path, provenance, needed_keys=None):
    entries, drops = [], 0
    with path.open("rb") as stream:
        header = stream.read(28)
        if len(header) != 28 or header[:8] != b"LXTRANSC":
            raise ValueError(f"not a translation cache: {path}")
        version, = struct.unpack_from("<I", header, 8)
        if version not in (9, 10):
            raise ValueError(f"unsupported format {version}")
        while frame := stream.read(8):
            magic, length = struct.unpack("<II", frame)
            if magic != 0x52435453 or length < 1:
                raise ValueError("invalid record frame")
            kind = stream.read(1)
            if kind != b"\0":
                drops += kind == b"\2"
                stream.seek(length - 1, 1)
                continue
            data = stream.read(length - 1)
            key = data[:16].hex()
            if bool(data[-1]) != provenance:
                continue
            if needed_keys is not None and key not in needed_keys:
                entries.append(dict(key=key, offline=provenance))
                continue
            count, = struct.unpack_from("<I", data, 16)
            queries = [struct.unpack_from("<BIIII", data, 20 + i * 17) for i in range(count)]
            pos = 20 + count * 17
            stage, = struct.unpack_from("<I", data, pos)
            fields, end = read_info(data, pos + 37)
            if stage in (7, 8):
                # Task and mesh entries end with their MeshInfo: four u32 sizes, then the
                # vertex and primitive line maps (u32 count + u32 elements each).
                end += 16
                for _ in range(2):
                    lines, = struct.unpack_from("<I", data, end)
                    end += 4 + lines * 4
            if end + 1 != len(data):
                raise ValueError("unexpected entry size (ABI mismatch)")
            entries.append(dict(key=key, stage=STAGES.get(stage, str(stage)),
                                info=fields, queries=queries, offline=bool(data[end])))
    return entries, drops, header[12:].hex()


def replay_queries(offline, live):
    answers = collections.defaultdict(set)
    for kind, a, b, answer, c in live:
        answers[kind, a, b, c].add(answer)
    handles = {}
    for kind, a, b, answer, c in offline:
        if kind == 8:
            index, shift, shift2 = a & 255, (a >> 8) & 255, (a >> 16) & 255
            secondary = bool((a >> 24) & 1)
            index2, offset2 = (c >> 16, c & 65535) if secondary else (index, b)
            lhs, rhs = answers[0, index, b, 0], answers[0, index2, offset2, 0]
            if len(lhs) != 1 or len(rhs) != 1:
                return "unobserved_query"
            handle = ((next(iter(lhs)) << shift) | (next(iter(rhs)) << shift2)) & 0xffffffff
            if answer in handles and handles[answer] != handle:
                return "handle_alias_mismatch"
            handles[answer] = handle
            continue
        actual_a = handles.get(a, a) if kind in (1, 2, 3) else a
        observed = answers[kind, actual_a, b, c]
        if not observed:
            return "unobserved_query"
        if observed != {answer}:
            return "texture_class_mismatch" if kind in (1, 2, 3) else "query_mismatch"
    return "match"


def build_helper(build, output):
    root = Path(__file__).resolve().parents[2]
    commands = json.loads((build / "compile_commands.json").read_text())
    command = next(row for row in commands if row["file"].endswith("/shader_translation_cache.cpp"))
    arguments = command.get("arguments") or shlex.split(command["command"])
    includes = [arg for arg in arguments if arg.startswith("-I")]
    compiler = arguments[1] if Path(arguments[0]).name in ("ccache", "sccache") else arguments[0]
    subprocess.run([compiler, "-std=c++20", "-shared", "-fPIC", *includes,
                    str(root / "tools/nx2/translation_cache_audit_hash.cpp"),
                    str(root / "src/common/cityhash.cpp"), "-o", str(output.resolve())],
                   cwd=command["directory"], check=True)


def recover_keys(pipeline, helper_path, config, live_keys, offline_keys):
    helper = ctypes.CDLL(str(helper_path.resolve()))
    helper.hash128.argtypes = [ctypes.c_char_p, ctypes.c_size_t,
                              ctypes.POINTER(ctypes.c_uint64)]
    helper.hash128.restype = None
    helper.graphics_size.restype = ctypes.c_size_t

    def digest(data):
        result = (ctypes.c_uint64 * 2)()
        helper.hash128(data, len(data), result)
        return bytes(result).hex()

    def stage_key(env, shader_hash, slot, hle, gp, texture_bank):
        native = env["native"]
        data = (bytes.fromhex(config) +
                struct.pack("<QIII", shader_hash, slot, env["stage"], env["start"]) +
                env["header"] + gp + env["mesh"] + env["mesh_header"] +
                struct.pack("<BIII", native or texture_bank == 2, texture_bank,
                            env["local"], env["shared"]) + env["workgroup"] +
                bytes([hle, native, native]))
        return digest(data)

    recovered = {}
    with pipeline.open("rb") as stream:
        def read(size):
            data = stream.read(size)
            if len(data) != size:
                raise ValueError("truncated pipeline cache or incompatible ABI")
            return data

        if read(12) != b"yuzucach" + struct.pack("<I", 53):
            raise ValueError("pipeline recovery supports cache version 53 only")
        while frame := stream.read(4):
            count, = struct.unpack("<I", frame)
            if count not in range(1, 7):
                raise ValueError("invalid pipeline environment count")
            environments = []
            for _ in range(count):
                (code_size, types, formats, cbufs, pointers, replacements,
                 local, bank, start, lowest, highest, viewport, stage) = struct.unpack("<6Q7I", read(76))
                code = read(code_size)
                read(types * 8 + formats * 8 + cbufs * 12 + pointers * 9 + replacements * 12)
                env = dict(stage=stage, local=local, bank=bank, start=start,
                           workgroup=bytes(12), shared=0, header=bytes(80),
                           gp=bytes(32), mesh=bytes(12), mesh_header=bytes(80),
                           native=code[-8:] == struct.pack("<Q", 0x50414d4e4544454e))
                if stage == 5:
                    env["workgroup"] = read(12)
                    env["shared"], = struct.unpack("<I", read(4))
                else:
                    env["header"] = read(80)
                    if stage == 3:
                        env["gp"] = read(32)
                    if stage in (7, 8):
                        env["mesh"], env["mesh_header"] = read(12), read(80)
                environments.append(env)
            if environments[0]["stage"] == 5:
                hashes = [struct.unpack_from("<Q", read(24))[0]]
                slots = [5]
            else:
                hashes = struct.unpack_from("<6Q", read(helper.graphics_size()))
                slots = [slot for slot, shader_hash in enumerate(hashes) if shader_hash]
                hashes = [shader_hash for shader_hash in hashes if shader_hash]
            if len(hashes) != len(environments):
                raise ValueError("pipeline stage slots disagree with environments")
            for env, shader_hash, slot in zip(environments, hashes, slots):
                # These inputs are omitted by the pipeline format. Accept an enumerated value
                # only when it reproduces the complete stored live digest exactly.
                masks = [env["gp"]] if env["stage"] == 3 else [bytes(32), b"\xff" * 32]
                for hle in (0, 1):
                    for gp in masks:
                        key = stage_key(env, shader_hash, slot, hle, gp, env["bank"])
                        if key not in live_keys:
                            continue
                        if key in offline_keys:
                            component = "exact"
                        elif env["stage"] == 5:
                            component = "unsupported_compute"
                        elif stage_key(env, shader_hash, slot, hle, gp, 2) in offline_keys:
                            component = "texture_bank"
                        else:
                            component = "unresolved"
                        recovered[key] = dict(component=component,
                                              shader_hash=f"{shader_hash:016x}", slot=slot,
                                              live_texture_bank=env["bank"], hle=bool(hle),
                                              archive_texture_bank=2 if component == "texture_bank" else None,
                                              preimage_verified=True)
    histogram = dict.fromkeys(["exact", "texture_bank", "unsupported_compute",
                               "hash", "header", "other_scalar", "unresolved", "unrecovered"], 0)
    for key in live_keys:
        histogram[recovered[key]["component"] if key in recovered else "unrecovered"] += 1
    return recovered, histogram


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("offline", type=Path, nargs="?")
    parser.add_argument("live", type=Path, nargs="?")
    parser.add_argument("--details", type=Path, help="JSONL: every live entry, exact key and named field differences")
    parser.add_argument("--pipeline", type=Path, help="matching version-53 pipeline cache for verified key recovery")
    parser.add_argument("--hash-helper", type=Path, help="shared library built from translation_cache_audit_hash.cpp")
    parser.add_argument("--build-helper", type=Path, metavar="BUILD_DIRECTORY", help="compile the standalone helper using compile_commands.json, then exit")
    args = parser.parse_args()
    if args.build_helper:
        if not args.hash_helper:
            parser.error("--build-helper requires --hash-helper output path")
        build_helper(args.build_helper, args.hash_helper)
        return
    if not args.offline or not args.live:
        parser.error("offline and live cache paths are required")
    if args.pipeline and not args.hash_helper:
        parser.error("--pipeline requires --hash-helper")
    live, drops, live_config = read_cache(args.live, False)
    offline, _, config = read_cache(args.offline, True, {entry["key"] for entry in live})
    if config != live_config:
        raise ValueError("configuration digests differ; exact-key coverage is not comparable")
    by_key = collections.defaultdict(list)
    for entry in offline:
        if entry["offline"]:
            by_key[entry["key"]].append(entry)
    records = []
    histogram, key_status, field_counts, padding_counts = collections.Counter(), {}, collections.Counter(), collections.Counter()
    for entry in live:
        if entry["offline"]:
            continue
        key, stage = entry["key"], entry["stage"]
        record = dict(key=key, stage=stage, exact_key=key in by_key)
        key_status[key] = f"{'exact' if key in by_key else 'missing'}.{stage}"
        if key not in by_key:
            record["component"] = "unknown: v9/v10 store only the digest; hash/header/scalar preimages absent"
        else:
            candidates = []
            for other in by_key[key]:
                fields = sorted(entry["info"].keys() | other["info"].keys())
                differences = {field: dict(offline=other["info"].get(field), live=entry["info"].get(field))
                               for field in fields if other["info"].get(field) != entry["info"].get(field)}
                padding = {field: values for field, values in differences.items() if "<padding" in field}
                semantic = {field: values for field, values in differences.items() if "<padding" not in field}
                candidates.append((len(semantic), semantic, padding, replay_queries(other["queries"], entry["queries"])))
            _, semantic, padding, replay = min(candidates, key=lambda candidate: candidate[0])
            record.update(query_replay=replay, field_differences=semantic, padding_differences=padding)
            histogram[f"paired_entries.replay_{replay}"] += 1
            histogram[f"paired_entries.semantic_{'different' if semantic else 'equal'}"] += 1
            field_counts.update(semantic.keys())
            padding_counts.update(padding.keys())
        records.append(record)
    histogram.update(key_status.values())
    summary = dict(offline_keys=len(by_key), live_entries=len(records), live_keys=len(key_status),
                   persistent_drop_records=drops, histogram=dict(histogram),
                   semantic_difference_fields=dict(field_counts), padding_difference_fields=dict(padding_counts),
                   missing_key_component_attribution="unavailable: raw key inputs were not recorded")
    if args.pipeline:
        recovered, components = recover_keys(args.pipeline, args.hash_helper, config,
                                              set(key_status), set(by_key))
        summary["key_component_histogram"] = components
        summary["missing_key_component_attribution"] = "verified against complete live and offline digests where recovered"
        for record in records:
            if record["key"] in recovered:
                record.update(recovered[record["key"]])
    if args.details:
        with args.details.open("w") as stream:
            for record in records:
                stream.write(json.dumps(record, sort_keys=True) + "\n")
    print(json.dumps(summary, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
