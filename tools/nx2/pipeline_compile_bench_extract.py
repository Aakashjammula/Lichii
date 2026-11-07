#!/usr/bin/env python3
"""Extract vertex+fragment pipelines from a Vulkan shader-environment cache for
pipeline_compile_bench: one directory per pipeline with vs.env, fs.env and key.bin.

Records that do not parse are skipped by scanning to the next plausible record, so a cache
with a torn or unknown record still yields the pipelines around it."""

import argparse
from pathlib import Path
import struct
import sys

HEADER = struct.Struct("<6Q7I")
GRAPHICS_KEY_SIZE = 1056
COMPUTE_KEY_SIZE = 24


def parse_env(data, cursor):
    if cursor + HEADER.size > len(data):
        return None
    fields = HEADER.unpack_from(data, cursor)
    code, textures, formats, constants, pointers, replacements = fields[:6]
    stage = fields[12]
    if stage > 8 or code == 0 or code > (16 << 20) or code % 8:
        return None
    if max(textures, formats, constants, pointers, replacements) > 100_000:
        return None
    end = cursor + HEADER.size + code + textures * 8 + formats * 8 + constants * 12
    end += pointers * 9 + replacements * 12
    if stage == 5:
        end += 16
    else:
        end += 80 + (32 if stage == 3 else 0) + (92 if stage in (7, 8) else 0)
    if end > len(data):
        return None
    return end, stage


def parse_entry(data, cursor):
    if cursor + 4 > len(data):
        return None
    count, = struct.unpack_from("<I", data, cursor)
    if not 1 <= count <= 6:
        return None
    pos = cursor + 4
    envs = []
    for _ in range(count):
        parsed = parse_env(data, pos)
        if parsed is None:
            return None
        end, stage = parsed
        envs.append((pos, end, stage))
        pos = end
    compute = envs[0][2] == 5
    if compute and count != 1:
        return None
    key_size = COMPUTE_KEY_SIZE if compute else GRAPHICS_KEY_SIZE
    if pos + key_size > len(data):
        return None
    return envs, (pos, pos + key_size), pos + key_size


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("cache", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--limit", type=int, default=400)
    args = parser.parse_args()
    data = args.cache.read_bytes()
    if data[:8] != b"yuzucach":
        sys.exit("not a shader-environment cache")
    args.output.mkdir(parents=True, exist_ok=False)
    cursor, written, skipped, entries = 12, 0, 0, 0
    while cursor < len(data) and written < args.limit:
        entry = parse_entry(data, cursor)
        if entry is None:
            skipped += 1
            cursor += 1
            continue
        envs, (key_begin, key_end), cursor = entry
        entries += 1
        if [stage for _, _, stage in envs] != [0, 4]:
            continue
        out = args.output / f"{written:04d}"
        out.mkdir()
        (out / "vs.env").write_bytes(data[envs[0][0]:envs[0][1]])
        (out / "fs.env").write_bytes(data[envs[1][0]:envs[1][1]])
        (out / "key.bin").write_bytes(data[key_begin:key_end])
        written += 1
    print(f"{entries} records, {written} vertex+fragment pipelines written, {skipped} bytes skipped")


if __name__ == "__main__":
    main()
