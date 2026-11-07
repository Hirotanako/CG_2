"""Rebuild the deterministic tiled R16 height map used by TerrainSystem.

This is the saved custom generator project for the assignment. Adjacent files
contain the same border samples because all tiles are sliced from one 257x257
field and each 65x65 slice overlaps its neighbour by one sample.
"""
from __future__ import annotations

import json
import math
import random
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "SecondSem_CG" / "assets" / "terrain"


def smooth(t: float) -> float:
    return t * t * (3.0 - 2.0 * t)


def value_noise(x: float, y: float, seed: int) -> float:
    x0, y0 = math.floor(x), math.floor(y)
    tx, ty = smooth(x - x0), smooth(y - y0)

    def sample(ix: int, iy: int) -> float:
        rng = random.Random(seed ^ (ix * 0x45D9F3B) ^ (iy * 0x119DE1F3))
        return rng.random() * 2.0 - 1.0

    a = sample(x0, y0) * (1.0 - tx) + sample(x0 + 1, y0) * tx
    b = sample(x0, y0 + 1) * (1.0 - tx) + sample(x0 + 1, y0 + 1) * tx
    return a * (1.0 - ty) + b * ty


def generate(config: dict) -> list[int]:
    size = config["resolution"]
    seed = config["seed"]
    result: list[float] = []
    for y in range(size):
        v = y / (size - 1)
        for x in range(size):
            u = x / (size - 1)
            amplitude, frequency, fbm = 1.0, 1.35, 0.0
            for octave in range(6):
                n = value_noise(u * frequency, v * frequency, seed + octave * 1013)
                fbm += n * amplitude
                amplitude *= 0.52
                frequency *= 2.03
            ridge = 1.0 - abs(value_noise(u * 7.2, v * 7.2, seed ^ 0xA53C))
            radius = math.hypot(u - 0.52, v - 0.46)
            basin = max(0.0, 1.0 - radius * 2.1)
            edge = min(1.0, min(u, v, 1.0 - u, 1.0 - v) / 0.18)
            result.append((0.52 + fbm * 0.23 + ridge * ridge * 0.26 - basin * 0.19) * smooth(edge))
    lo, hi = min(result), max(result)
    return [round(max(0.0, min(1.0, (h - lo) / (hi - lo))) * 65535.0) for h in result]


def main() -> None:
    config = json.loads((OUT / "generator_project.json").read_text(encoding="utf-8"))
    heights = generate(config)
    size = config["resolution"]
    tile_count = config["tiles_per_axis"]
    tile_size = config["tile_samples_with_shared_border"]
    stride = tile_size - 1
    for ty in range(tile_count):
        for tx in range(tile_count):
            samples = [
                heights[(ty * stride + y) * size + tx * stride + x]
                for y in range(tile_size)
                for x in range(tile_size)
            ]
            (OUT / f"tile_{ty:02d}_{tx:02d}.r16").write_bytes(struct.pack(f"<{len(samples)}H", *samples))

    # Generator-side seam assertion: a vertical/horizontal shared edge must be
    # byte-for-byte identical before the files are accepted as an asset set.
    def read_tile(ty: int, tx: int) -> tuple[int, ...]:
        raw = (OUT / f"tile_{ty:02d}_{tx:02d}.r16").read_bytes()
        return struct.unpack(f"<{tile_size * tile_size}H", raw)

    for ty in range(tile_count):
        for tx in range(tile_count - 1):
            left, right = read_tile(ty, tx), read_tile(ty, tx + 1)
            assert [left[y * tile_size + tile_size - 1] for y in range(tile_size)] == [right[y * tile_size] for y in range(tile_size)]
    for ty in range(tile_count - 1):
        for tx in range(tile_count):
            top, bottom = read_tile(ty, tx), read_tile(ty + 1, tx)
            assert list(top[-tile_size:]) == list(bottom[:tile_size])
    print(
        f"Generated and seam-checked {tile_count * tile_count} overlapping R16 tiles "
        f"({min(heights)}..{max(heights)}) in {OUT}"
    )


if __name__ == "__main__":
    main()
