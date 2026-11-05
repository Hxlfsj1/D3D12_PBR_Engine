"""Validate exported screen-probe geometry and constant-radiance captures.

Consumes the engine's metadata.json and adjacent raw resources; no renderer mocks.
"""
import argparse
import json
from pathlib import Path

import numpy as np


def load_capture(path):
    path = Path(path)
    metadata = json.loads(path.read_text(encoding="utf-8-sig"))

    def texture(suffix):
        output = next(o for o in metadata["outputs"] if o["file"].endswith(suffix))
        dtype, channels = {2: ("<f4", 4), 10: ("<f2", 4), 42: ("<u4", 1)}[output["format_dxgi"]]
        values = np.fromfile(path.parent / output["file"], dtype=dtype)
        return values.reshape(output["height"], output["width"], channels)

    return metadata, texture


def check_geometry(path):
    metadata, texture = load_capture(path)
    settings = metadata["ssgi"]["sampling"]["screen_probes"]
    position = texture("-screen-probe-positions.raw")
    normal = texture("-screen-probe-normal-state.raw").astype(np.float32)
    receiver = texture("-screen-probe-receivers.raw")[..., 0]
    gi = texture("-gi-filtered.raw")
    directions = texture("-gi-full-world-owners.raw")
    filtered = texture("-screen-probe-filtered-directions.raw")
    for array in (position, normal, gi, directions, filtered):
        assert np.isfinite(array).all(), f"Nonfinite output in {path}"
    assert np.isin(position[..., 3], [0, 1, 2]).all()
    valid = position[..., 3] > 0
    assert np.array_equal(valid, receiver > 0)
    assert np.all(np.abs(np.linalg.norm(normal[valid, :3], axis=-1) - 1) < 0.005)
    assert np.all((normal[valid, 3] >= 1) & (normal[valid, 3] <= settings["history_updates"]))
    height, width = gi.shape[:2]
    step = settings["cell_width_internal_pixels"]
    assert position.shape[:2] == ((height + step - 1) // step, (width + step - 1) // step)
    assert np.all(receiver <= width * height)
    # The selected receiver must belong to its own downsample cell, including
    # partial cells at the right/bottom edges of odd-sized render targets.
    py, px = np.indices(receiver.shape)
    packed = receiver.astype(np.int64) - 1
    assert np.all((packed[valid] % width) // step == px[valid])
    assert np.all((packed[valid] // width) // step == py[valid])
    assert directions.shape[:2] == (position.shape[0] * 8, position.shape[1] * 8)
    assert directions.shape == filtered.shape
    assert np.all(directions[..., :3] >= 0) and np.all(filtered[..., :3] >= 0)
    fresh = int(np.count_nonzero(position[..., 3] == 2))
    return {"capture": str(path), "grid": settings["grid"], "valid": int(valid.sum()),
            "fresh": fresh, "held": int(np.count_nonzero(position[..., 3] == 1)),
            "configured_fresh_rays": fresh * settings["rays_per_fresh_probe"]}


def check_constant(path, value):
    _, texture = load_capture(path)
    gi = texture("-gi-filtered.raw")[..., :3].astype(np.float32)
    assert np.isfinite(gi).all()
    error = float(np.max(np.abs(gi - value)))
    assert error < 0.005 if value else error == 0
    return {"capture": str(path), "expected": value, "max_absolute_error": error}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--geometry", nargs="*", default=[])
    parser.add_argument("--white")
    parser.add_argument("--black")
    parser.add_argument("--output")
    args = parser.parse_args()
    results = {"geometry": [check_geometry(p) for p in args.geometry]}
    if args.white:
        results["white"] = check_constant(args.white, 1)
    if args.black:
        results["black"] = check_constant(args.black, 0)
    text = json.dumps(results, indent=2)
    if args.output:
        Path(args.output).write_text(text, encoding="utf-8")
    print(text)
