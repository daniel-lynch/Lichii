#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Hash a co-op kit. --load accepts a title's load directory, or a load root with one title."""
import argparse
import hashlib
import os
from pathlib import Path
import re

SIDECARS = ('coop-ux.json', 'coop-anim.json', 'coop-equip.json', 'coop-ledger.json')


def fingerprint(path):
    digest, size = hashlib.sha256(), 0
    with path.open('rb') as source:
        while block := source.read(1024 * 1024):
            digest.update(block)
            size += len(block)
    return size, digest.hexdigest()


def regular_files(directory, ancestors=()):
    """Follow links, but fail on a directory cycle rather than silently omit kit files."""
    real = directory.resolve(strict=True)
    if real in ancestors:
        raise ValueError('mod directory symlink cycle')
    for path in directory.iterdir():
        if path.is_dir():
            yield from regular_files(path, (*ancestors, real))
        elif path.is_file():
            yield path
        elif path.is_symlink():
            raise ValueError(f'broken mod symlink: {path}')


def canonical_text(keys, program_dir):
    keys, program_dir = Path(keys), Path(program_dir)
    size, digest = fingerprint(keys)
    lines = ['oot-coop-mod-hash 1', f'keys {size} {digest}']
    for name in SIDECARS:
        path = keys.parent / name
        if not os.path.lexists(path):
            lines.append(f'sidecar {name} absent')
        else:
            size, digest = fingerprint(path)
            lines.append(f'sidecar {name} {size} {digest}')
    records = []
    if program_dir.exists():
        for mod in program_dir.iterdir():
            if not mod.is_dir():
                continue
            for tree in ('romfs', 'exefs'):
                root = mod / tree
                if not os.path.lexists(root):
                    continue
                for path in regular_files(root):
                    relative = path.relative_to(mod).as_posix()
                    if any(c in relative for c in '\r\n'):
                        raise ValueError('newline in mod path')
                    size, digest = fingerprint(path)
                    records.append((relative.encode('utf-8'), digest, size, relative))
    records.sort(key=lambda value: (value[0], value[1]))
    lines.extend(f'mod {relative} {size} {digest}' for _, digest, size, relative in records)
    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--keys', type=Path, required=True)
    parser.add_argument('--load', type=Path, required=True)
    parser.add_argument('--program-id', help='16 uppercase hex; selects a title under a shared load root')
    parser.add_argument('--text', action='store_true')
    args = parser.parse_args()
    try:
        directory = args.load
        if args.program_id:
            if not re.fullmatch('[0-9A-F]{16}', args.program_id):
                raise ValueError('invalid program id')
            directory /= args.program_id
        elif not re.fullmatch('[0-9A-F]{16}', directory.name):
            titles = [p for p in directory.iterdir() if p.is_dir() and re.fullmatch('[0-9A-F]{16}', p.name)]
            if len(titles) != 1:
                raise ValueError('--load must select one title; use --program-id for a shared load root')
            directory = titles[0]
        text = canonical_text(args.keys, directory)
        print(text if args.text else hashlib.sha256(text.encode('utf-8')).hexdigest(), end='' if args.text else '\n')
    except (OSError, ValueError) as exc:
        parser.exit(1, f'mod hash failed: {exc}\n')


if __name__ == '__main__':
    main()
