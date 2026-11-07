#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Computes the translated-shader cache tag of a source tree without building it.

The tag names the offline translation product, vulkan_translations.<tag>.fork-vN.bin. It is
derived in two steps, both reproduced here:

  1. CMakeModules/TranslatorIdentity.cmake hashes the translator's source into a manifest and
     takes its SHA-256 (TranslatorSourceHash). The lists of directories, files and region files
     are read from the module of the tree being hashed, so the helper follows the tree's rules.
  2. shader_translation_cache.cpp (CurrentCacheIdentity) hashes
     "source|<TranslatorSourceHash>|<compile facts>" with Hasher::AddString (u64 length, then the
     bytes) and CityHash128; the tag is the low half as 16 hex digits.

The build facts (step 1) come from a build directory's translator_identity.facts, and the
compile facts (step 2) from the executable's CompileFacts string; both are the same for two
builds configured alike, so a tag computed for one tree with another build's facts is the tag
that tree gets when built the same way.

Usage:
  translator-tag.py --tree DIR --facts FILE --compile-facts STR    # a working tree
  translator-tag.py --repo DIR --rev REV --facts FILE ...          # a commit, from git objects
  translator-tag.py ... --manifest                                 # print the manifest too
  translator-tag.py --compile-facts-from EXE                       # read the facts from a binary
"""

import argparse
import hashlib
import os
import re
import subprocess
import sys

MASK = (1 << 64) - 1
K0 = 0xC3A5C85C97CB3127
K1 = 0xB492B66FBE98F273
K2 = 0x9AE16A3B2F90404F


# CityHash128 as src/common/cityhash.cpp implements it (little-endian host).
def _fetch64(data, pos):
    return int.from_bytes(data[pos:pos + 8], "little")


def _fetch32(data, pos):
    return int.from_bytes(data[pos:pos + 4], "little")


def _rotate(val, shift):
    return val if shift == 0 else ((val >> shift) | (val << (64 - shift))) & MASK


def _shift_mix(val):
    return val ^ (val >> 47)


def _hash128to64(lo, hi):
    mul = 0x9DDFEA08EB382D69
    a = ((lo ^ hi) * mul) & MASK
    a ^= a >> 47
    b = ((hi ^ a) * mul) & MASK
    b ^= b >> 47
    return (b * mul) & MASK


def _hash_len16_mul(u, v, mul):
    a = ((u ^ v) * mul) & MASK
    a ^= a >> 47
    b = ((v ^ a) * mul) & MASK
    b ^= b >> 47
    return (b * mul) & MASK


def _hash_len0to16(data, pos, length):
    if length >= 8:
        mul = (K2 + length * 2) & MASK
        a = (_fetch64(data, pos) + K2) & MASK
        b = _fetch64(data, pos + length - 8)
        c = (_rotate(b, 37) * mul + a) & MASK
        d = ((_rotate(a, 25) + b) * mul) & MASK
        return _hash_len16_mul(c, d, mul)
    if length >= 4:
        mul = (K2 + length * 2) & MASK
        a = _fetch32(data, pos)
        return _hash_len16_mul((length + (a << 3)) & MASK, _fetch32(data, pos + length - 4), mul)
    if length > 0:
        a = data[pos]
        b = data[pos + (length >> 1)]
        c = data[pos + length - 1]
        y = (a + (b << 8)) & 0xFFFFFFFF
        z = (length + (c << 2)) & 0xFFFFFFFF
        return (_shift_mix(((y * K2) ^ (z * K0)) & MASK) * K2) & MASK
    return K2


def _weak_hash_len32_with_seeds(data, pos, a, b):
    w = _fetch64(data, pos)
    x = _fetch64(data, pos + 8)
    y = _fetch64(data, pos + 16)
    z = _fetch64(data, pos + 24)
    a = (a + w) & MASK
    b = _rotate((b + a + z) & MASK, 21)
    c = a
    a = (a + x) & MASK
    a = (a + y) & MASK
    b = (b + _rotate(a, 44)) & MASK
    return (a + z) & MASK, (b + c) & MASK


def _city_murmur(data, pos, length, seed_lo, seed_hi):
    a, b, c, d = seed_lo, seed_hi, 0, 0
    remaining = length - 16
    if remaining <= 0:
        a = (_shift_mix((a * K1) & MASK) * K1) & MASK
        c = (b * K1 + _hash_len0to16(data, pos, length)) & MASK
        d = _shift_mix((a + (_fetch64(data, pos) if length >= 8 else c)) & MASK)
    else:
        c = _hash128to64((_fetch64(data, pos + length - 8) + K1) & MASK, a)
        d = _hash128to64((b + length) & MASK, (c + _fetch64(data, pos + length - 16)) & MASK)
        a = (a + d) & MASK
        while True:
            a ^= (_shift_mix((_fetch64(data, pos) * K1) & MASK) * K1) & MASK
            a = (a * K1) & MASK
            b ^= a
            c ^= (_shift_mix((_fetch64(data, pos + 8) * K1) & MASK) * K1) & MASK
            c = (c * K1) & MASK
            d ^= c
            pos += 16
            remaining -= 16
            if remaining <= 0:
                break
    a = _hash128to64(a, c)
    b = _hash128to64(d, b)
    return a ^ b, _hash128to64(b, a)


def _city_hash128_with_seed(data, pos, length, seed_lo, seed_hi):
    if length < 128:
        return _city_murmur(data, pos, length, seed_lo, seed_hi)
    x, y = seed_lo, seed_hi
    z = (length * K1) & MASK
    v0 = (_rotate(y ^ K1, 49) * K1 + _fetch64(data, pos)) & MASK
    v1 = (_rotate(v0, 42) * K1 + _fetch64(data, pos + 8)) & MASK
    w0 = (_rotate((y + z) & MASK, 35) * K1 + x) & MASK
    w1 = (_rotate((x + _fetch64(data, pos + 88)) & MASK, 53) * K1) & MASK
    while True:
        for _ in range(2):
            x = (_rotate((x + y + v0 + _fetch64(data, pos + 8)) & MASK, 37) * K1) & MASK
            y = (_rotate((y + v1 + _fetch64(data, pos + 48)) & MASK, 42) * K1) & MASK
            x ^= w1
            y = (y + v0 + _fetch64(data, pos + 40)) & MASK
            z = (_rotate((z + w0) & MASK, 33) * K1) & MASK
            v0, v1 = _weak_hash_len32_with_seeds(data, pos, (v1 * K1) & MASK, (x + w0) & MASK)
            w0, w1 = _weak_hash_len32_with_seeds(data, pos + 32, (z + w1) & MASK,
                                                 (y + _fetch64(data, pos + 16)) & MASK)
            z, x = x, z
            pos += 64
        length -= 128
        if length < 128:
            break
    x = (x + _rotate((v0 + z) & MASK, 49) * K0) & MASK
    y = (y * K0 + _rotate(w1, 37)) & MASK
    z = (z * K0 + _rotate(w0, 27)) & MASK
    w0 = (w0 * 9) & MASK
    v0 = (v0 * K0) & MASK
    tail_done = 0
    while tail_done < length:
        tail_done += 32
        y = (_rotate((x + y) & MASK, 42) * K0 + v1) & MASK
        w0 = (w0 + _fetch64(data, pos + length - tail_done + 16)) & MASK
        x = (x * K0 + w0) & MASK
        z = (z + w1 + _fetch64(data, pos + length - tail_done)) & MASK
        w1 = (w1 + v0) & MASK
        v0, v1 = _weak_hash_len32_with_seeds(data, pos + length - tail_done, (v0 + z) & MASK, v1)
        v0 = (v0 * K0) & MASK
    x = _hash128to64(x, v0)
    y = _hash128to64((y + z) & MASK, w0)
    return ((_hash128to64((x + v1) & MASK, w1) + y) & MASK,
            _hash128to64((x + w1) & MASK, (y + v1) & MASK))


def city_hash128(data):
    if len(data) >= 16:
        return _city_hash128_with_seed(data, 16, len(data) - 16, _fetch64(data, 0),
                                       (_fetch64(data, 8) + K0) & MASK)
    return _city_hash128_with_seed(data, 0, len(data), K0, K1)


def file_tag(source_hash, compile_facts):
    """CurrentCacheIdentity: Hasher::AddString("source|<hash>|<facts>"), CityHash128, low half."""
    value = ("source|" + source_hash + "|" + compile_facts).encode()
    lo, _ = city_hash128(len(value).to_bytes(8, "little") + value)
    return "{:016x}".format(lo)


# Source access: a working tree or a commit's git objects.
class Tree:
    def __init__(self, root):
        self.root = root

    def exists(self, path):
        return os.path.exists(os.path.join(self.root, path))

    def is_dir(self, path):
        return os.path.isdir(os.path.join(self.root, path))

    def read(self, path):
        with open(os.path.join(self.root, path), "rb") as file:
            return file.read()

    def walk_files(self, directory):
        # file(GLOB_RECURSE ... LIST_DIRECTORIES false): files, symlinked directories not followed.
        found = []
        for base, dirs, files in os.walk(os.path.join(self.root, directory)):
            dirs[:] = [d for d in dirs if not os.path.islink(os.path.join(base, d))]
            for name in files:
                found.append(os.path.relpath(os.path.join(base, name), self.root))
        return found


class GitTree:
    def __init__(self, repo, rev):
        self.repo = repo
        listing = subprocess.run(["git", "-C", repo, "ls-tree", "-r", "-z", rev],
                                 check=True, capture_output=True).stdout
        self.blobs = {}
        self.dirs = set()
        for record in listing.split(b"\0"):
            if not record:
                continue
            meta, path = record.split(b"\t", 1)
            mode, kind, sha = meta.split()
            path = path.decode()
            if kind == b"blob":
                self.blobs[path] = sha.decode()
                parts = path.split("/")
                for i in range(1, len(parts)):
                    self.dirs.add("/".join(parts[:i]))
        self.cache = {}

    def exists(self, path):
        return path in self.blobs or path in self.dirs

    def is_dir(self, path):
        return path in self.dirs

    def read(self, path):
        if path not in self.cache:
            self.cache[path] = subprocess.run(
                ["git", "-C", self.repo, "cat-file", "blob", self.blobs[path]],
                check=True, capture_output=True).stdout
        return self.cache[path]

    def walk_files(self, directory):
        prefix = directory.rstrip("/") + "/"
        return [p for p in self.blobs if p.startswith(prefix)]


MODULE = "CMakeModules/TranslatorIdentity.cmake"


def _cmake_list(text, name):
    match = re.search(r"set\(" + name + r"\b(.*?)\)", text, re.S)
    if not match:
        return None
    body = re.sub(r"#[^\n]*", "", match.group(1))
    return body.split()


def read_rules(module_text):
    text = module_text.decode()
    rules = {
        "scheme": (_cmake_list(text, "TRANSLATOR_IDENTITY_SCHEME") or ["1"])[0],
        "dirs": _cmake_list(text, "TRANSLATOR_IDENTITY_DIRS") or [],
        "files": _cmake_list(text, "TRANSLATOR_IDENTITY_FILES") or [],
        "region_files": _cmake_list(text, "TRANSLATOR_IDENTITY_REGION_FILES") or [],
        "prefix": "common/",
    }
    prefix = _cmake_list(text, "TRANSLATOR_IDENTITY_INCLUDE_PREFIX")
    if prefix:
        rules["prefix"] = prefix[0].strip('"')
    return rules


BEGIN = b"TRANSLATOR-IDENTITY BEGIN"
END = b"TRANSLATOR-IDENTITY END"


class RegionError(Exception):
    pass


def marked_regions(content, path):
    """The bytes TranslatorIdentity.cmake hashes for a region file: each region from its BEGIN
    token to the end of its END token, followed by a newline, in file order."""
    rest = content
    out = b""
    count = 0
    while True:
        b = rest.find(BEGIN)
        e = rest.find(END)
        if b == -1:
            if e != -1:
                raise RegionError(path + ": TRANSLATOR-IDENTITY END without BEGIN")
            break
        if e == -1:
            raise RegionError(path + ": TRANSLATOR-IDENTITY BEGIN without END")
        if e < b:
            raise RegionError(path + ": TRANSLATOR-IDENTITY END without BEGIN")
        nested = rest.find(BEGIN, b + len(BEGIN))
        if nested != -1 and nested < e:
            raise RegionError(path + ": nested TRANSLATOR-IDENTITY BEGIN")
        out += rest[b:e + len(END)] + b"\n"
        rest = rest[e + len(END):]
        count += 1
    if count == 0:
        raise RegionError(path + ": no TRANSLATOR-IDENTITY regions")
    return out


def manifest(tree, facts, rules=None, region_override=None):
    """The manifest TranslatorIdentity.cmake writes, for `tree` under `rules` (default: the
    tree's own module). `region_override(path, content)` may supply a region file's hashed bytes
    (for trees that predate the markers)."""
    if rules is None:
        rules = read_rules(tree.read(MODULE))
    files = []
    for directory in rules["dirs"]:
        if not tree.is_dir(directory):
            raise RuntimeError(directory + " does not exist")
        files += tree.walk_files(directory)
    region_set = set(rules["region_files"])
    for path in rules["files"] + rules["region_files"]:
        if not tree.exists(path):
            raise RuntimeError(path + " does not exist")
        files.append(path)
    exclude = re.compile(r"(/__pycache__/|\.pyc$|~$|\.swp$|/\.)")
    files = [f for f in files if not exclude.search(f)]

    include_line = re.compile(rb'^[ \t]*#[ \t]*include[ \t]*"' + re.escape(rules["prefix"].encode()))
    include_name = re.compile(rb'"(' + re.escape(rules["prefix"].encode()) + rb'[^"]+)"')
    pending = list(files)
    seen = set(files)
    while pending:
        path = pending.pop(0)
        for line in tree.read(path).split(b"\n"):
            if include_line.match(line):
                match = include_name.search(line)
                if match:
                    header = "src/" + match.group(1).decode()
                    if tree.exists(header) and header not in seen:
                        seen.add(header)
                        pending.append(header)
                        files.append(header)
    files = sorted(set(files))

    text = "scheme {}\n".format(rules["scheme"])
    for path in files:
        content = tree.read(path)
        if path in region_set:
            regions = (region_override(path, content) if region_override
                       else marked_regions(content, path))
            text += "regions {} {}\n".format(path, hashlib.sha256(regions).hexdigest())
        else:
            text += "file {} {}\n".format(path, hashlib.sha256(content).hexdigest())
    text += facts
    return text, len(files)


def compile_facts_from(executable):
    data = open(executable, "rb").read()
    match = re.search(rb"version=[\x20-\x7e]*?;x86_64;|version=[\x20-\x7e]*?;arm64;", data)
    if not match:
        raise RuntimeError("no CompileFacts string in " + executable)
    return match.group(0).decode()


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tree", help="working tree to hash")
    parser.add_argument("--repo", help="repository, with --rev")
    parser.add_argument("--rev", help="commit to hash from git objects")
    parser.add_argument("--facts", help="a build's translator_identity/translator_identity.facts")
    parser.add_argument("--compile-facts", help="the executable's CompileFacts string")
    parser.add_argument("--compile-facts-from", help="read CompileFacts from this executable")
    parser.add_argument("--manifest", action="store_true", help="print the manifest")
    args = parser.parse_args()

    compile_facts = args.compile_facts
    if args.compile_facts_from:
        compile_facts = compile_facts_from(args.compile_facts_from)
        if not args.tree and not args.rev:
            print(compile_facts)
            return 0
    if not args.facts or compile_facts is None:
        parser.error("--facts and --compile-facts (or --compile-facts-from) are needed")
    if args.rev:
        tree = GitTree(args.repo or ".", args.rev)
    else:
        tree = Tree(args.tree or ".")
    facts = open(args.facts, encoding="utf-8", newline="").read()
    text, count = manifest(tree, facts)
    source_hash = hashlib.sha256(text.encode()).hexdigest()
    if args.manifest:
        sys.stdout.write(text)
    print("source {} ({} files)".format(source_hash, count))
    print("tag {}".format(file_tag(source_hash, compile_facts)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
