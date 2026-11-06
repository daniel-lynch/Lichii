#!/usr/bin/env python3
"""aot_index.py NUP DICTDIR OUT.bin

Writes the shader pool index the emulator's AOT pool recogniser reads (EDEN_NX2_AOT_INDEX=OUT.bin,
docs/shader-aot-translation.md): every program of every BNSH container in the game's romfs
(Shader/, Layout2Shader/, Effect/ and the bfsha members of the Shader/ and Lib/ SARCs) and of every
sharcb (BAHS) member of those SARCs (the agl/gsys/terrain/app-package engine shader packs), with
its offset from the container's memory pool, padded size, stage slot and a fingerprint of its first
bytes. The output is derived from the game: keep it outside the repository.

A sharcb container is its code pool (header +0x1C, size +0x18): program offsets are relative to it
and the size is the NVN control's padded size (+0x44), as BNSH stage records carry it. That agl
maps the pool contiguously on the GPU is not known from the file; the emulator verifies it the way
it verifies BNSH pools, by the siblings' fingerprints at their expected addresses.

Format (little-endian): "LXAOTIX2", u32 container count; per container: u32 program count, then
per program: u32 pool offset, u32 size, u8 stage slot (bnsh.STAGES index), u64 FNV-1a 64 of the
first min(size, 256) bytes, u32 constant-section offset from the program start (NVN control
section +0x40, 0xffffffff when absent) and u16 its length (+0x3c, at most 0x100). The constant
section is the program's bank-1 constant buffer, bytes of the program pool itself.
"""
import fnmatch, os, struct, sys
sys.dont_write_bytecode = True  # keep __pycache__ out of the worktree
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nup_tool, unzs, sarc, bnsh
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'nx2'))
import shader_containers  # noqa: E402  (parse_sharcb: the BAHS record table)

CTRL_MAGIC = b'\x34\x12\x76\x98'
# parse_sharcb stage names -> bnsh.STAGES slots
SHARCB_SLOTS = {'vertex': 0, 'fragment': 4, 'compute': 5, 'task': 7, 'mesh': 8}

FP_BYTES = 256


def fnv1a64(data):
    h = 0xcbf29ce484222325
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xffffffffffffffff
    return h


def containers(d):
    """(pool offset, [(pool-relative offset, size, stage slot, fingerprint)]) per BNSH in d."""
    by_bnsh = {}
    for p in bnsh.iter_programs(d):
        by_bnsh.setdefault(p['bnsh'], {})[p['code']] = p
    for b, progs in by_bnsh.items():
        pl = bnsh.pool(d, b)
        out = []
        for code, p in sorted(progs.items()):
            rel = code - pl['offset']
            if rel < 0 or rel + p['code_size'] > pl['size']:
                continue  # outside the mapped pool: cannot be found by address
            n = min(p['code_size'], FP_BYTES)
            ctrl = p['control']
            c40, c3c = 0xffffffff, 0
            if d[ctrl:ctrl + 4] == b'\x34\x12\x76\x98' and ctrl + 0x44 <= len(d):
                c3c, c40 = struct.unpack_from('<II', d, ctrl + 0x3c)
                if c40 == 0xffffffff or c3c > 0x100 or rel + c40 + c3c > pl['size']:
                    c40, c3c = 0xffffffff, 0
            out.append((rel, p['code_size'], bnsh.STAGES.index(p['stage']), fnv1a64(d[code:code + n]),
                        c40, c3c))
        if out:
            yield pl['offset'], out


def constants(d, ctrl, rel, pool_size):
    """(offset, size) of the program's bank-1 constant section, from its NVN control."""
    if d[ctrl:ctrl + 4] == CTRL_MAGIC and ctrl + 0x44 <= len(d):
        c3c, c40 = struct.unpack_from('<II', d, ctrl + 0x3c)
        if c40 != 0xffffffff and c3c <= 0x100 and rel + c40 + c3c <= pool_size:
            return c40, c3c
    return 0xffffffff, 0


def sharcb_container(d):
    """[(pool-relative offset, size, stage slot, fingerprint, c40, c3c)] of a sharcb's code pool:
    one per distinct (offset, stage), size = NVN control +0x44."""
    pool_size, pool = struct.unpack_from('<II', d, 0x18)
    out = {}
    for r in shader_containers.parse_sharcb(d):
        rel = r['code_offset'] - pool
        ctrl = r['control_offset']
        size = struct.unpack_from('<I', d, ctrl + 0x44)[0]
        if rel < 0 or not size or rel + size > pool_size:
            continue
        slot = SHARCB_SLOTS[r['stage']]
        n = min(size, FP_BYTES)
        c40, c3c = constants(d, ctrl, rel, pool_size)
        out.setdefault((rel, slot), (rel, size, slot, fnv1a64(d[pool + rel:pool + rel + n]), c40, c3c))
    return [out[k] for k in sorted(out)]


def main():
    nup, dictdir, out_path = sys.argv[1:4]
    dicts = unzs.load_dicts(dictdir)
    n = nup_tool.Nup(nup)
    members = {rel: fi for rel, fi in n.walk(n.packages[0]['romfs_dir'])}

    def load(rel):
        d = n.read(members[rel])
        return unzs.decompress(d, dicts) if rel.endswith('.zs') else d

    result = []
    sharcb = [0, 0]
    for rel in sorted(members):
        if any(fnmatch.fnmatchcase(rel, g) for g in
               ('Shader/*.bfsha.zs', 'Layout2Shader/*.bfsha.zs', 'Effect/*.vsetb.byml.zs')):
            result += [c for _, c in containers(load(rel))]
        elif fnmatch.fnmatchcase(rel, 'Shader/*.sarc.zs') or fnmatch.fnmatchcase(rel, 'Lib/*.release.sarc.zs'):
            outer = load(rel)
            for name, off, size in sarc.entries(outer):
                inner = outer[off:off + size]
                if inner[:4] == b'FSHA':
                    result += [c for _, c in containers(inner)]
                elif inner[:4] == b'BAHS':
                    try:
                        progs = sharcb_container(inner)
                    except shader_containers.ArchiveError as e:
                        print(f'{rel}!{name}: skipped ({e})', file=sys.stderr)
                        continue
                    if progs:
                        result.append(progs)
                        sharcb[0] += 1
                        sharcb[1] += len(progs)
    with open(out_path, 'wb') as f:
        f.write(b'LXAOTIX2' + struct.pack('<I', len(result)))
        for progs in result:
            f.write(struct.pack('<I', len(progs)))
            for rel, size, stage, fp, c40, c3c in progs:
                f.write(struct.pack('<IIBQIH', rel, size, stage, fp, c40, c3c))
    total = sum(len(c) for c in result)
    print(f'{len(result)} containers, {total} programs ({sharcb[0]} sharcb code pools, {sharcb[1]} '
          f'programs) -> {out_path}', file=sys.stderr)


if __name__ == '__main__':
    main()
