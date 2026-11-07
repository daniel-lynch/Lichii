#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic shaders for gpl_probe (no game data): a heavy vertex shader, a light fragment
shader and a second fragment "variant" that differs only in which bindings it reads, like the
fragment variants the emulator makes for a new vertex partner (docs/pipeline-library.md).

    python3 -I gen_shaders.py OUT_DIR [--work N]

Writes vs.vert, fs.frag, fs2.frag and compiles them with glslangValidator to *.spv.
"""
import argparse
import pathlib
import subprocess


def vertex(work: int) -> str:
    lines = [
        "#version 450",
        "layout(location = 0) in vec4 in_pos;",
        "layout(location = 1) in vec4 in_attr;",
        "layout(set = 0, binding = 0, std140) uniform Bones { mat4 bones[64]; vec4 k[64]; };",
        "layout(set = 0, binding = 1, std430) readonly buffer Extra { vec4 extra[]; };",
    ]
    lines += [f"layout(location = {i}) out vec4 v{i};" for i in range(12)]
    lines += ["void main() {",
              "  vec4 p = in_pos; vec4 a = in_attr; int i = int(a.w * 63.0);"]
    # Data-dependent work the compiler cannot fold: indexed loads, divisions, transcendental.
    for n in range(work):
        lines.append(
            f"  p = bones[(i + {n}) & 63] * p + k[(i * {n % 7 + 1}) & 63] * sin(a.x + {n}.0);")
        lines.append(f"  a = a.yzwx * extra[(i + {n * 3}) & 255] + vec4(p.w / (abs(a.x) + 1.0));")
    lines += [f"  v{i} = p * {i + 1}.0 + a.wzyx;" for i in range(12)]
    lines += ["  gl_Position = p;", "}"]
    return "\n".join(lines) + "\n"


def fragment(base: int) -> str:
    lines = ["#version 450"]
    lines += [f"layout(location = {i}) in vec4 v{i};" for i in range(12)]
    lines += [f"layout(set = 0, binding = {base + i}) uniform sampler2D t{i};" for i in range(4)]
    lines += ["layout(location = 0) out vec4 color;", "void main() {", "  vec4 c = vec4(0.0);"]
    lines += [f"  c += texture(t{i % 4}, v{i}.xy) * v{(i + 1) % 12};" for i in range(12)]
    lines += ["  color = c;", "}"]
    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("out", type=pathlib.Path)
    parser.add_argument("--work", type=int, default=400)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    sources = {"vs.vert": vertex(args.work), "fs.frag": fragment(2), "fs2.frag": fragment(6)}
    for name, text in sources.items():
        src = args.out / name
        src.write_text(text)
        subprocess.run(["glslangValidator", "-V", "--target-env", "vulkan1.3", "-o",
                        str(args.out / (src.stem + ".spv")),
                        str(src)], check=True, stdout=subprocess.DEVNULL)


if __name__ == "__main__":
    main()
