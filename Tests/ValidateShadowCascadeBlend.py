"""Validate native HDR captures against independently reconstructed floor depth."""
import json
from pathlib import Path
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
STAGE = ROOT / "tmp/shadow-cascade-blend/runtime"
runs = json.loads((STAGE / "runs.json").read_text(encoding="utf-8-sig"))
checks = []
splits = np.array([5., 15., 50., 100.])
ends = np.array([5.49, 16., 53.5, 100.])
for run in runs:
    meta_path = STAGE / run["capture"].replace("\\", "/")
    metadata = json.loads(meta_path.read_text(encoding="utf-8-sig"))
    output = next(o for o in metadata["outputs"] if not o["preview"])
    assert output["format_dxgi"] == 10
    width, height = output["width"], output["height"]
    pixels = np.fromfile(meta_path.parent / output["file"], dtype=np.float16).reshape(height, width, 4).astype(float)
    assert np.isfinite(pixels).all(), run["name"]
    assert pixels[:, :, :3].max() > .001, run["name"]
    assert run["errors"] == run["warnings"] == 0
    result = {"name": run["name"], "finite": True, "capture": run["capture"]}
    if run["diagnostic"]:
        camera = metadata["camera"]
        eye = np.array(camera["eye"])
        forward = np.array(camera["forward"]); forward /= np.linalg.norm(forward)
        up = np.array(camera["up"]); right = np.cross(up, forward); right /= np.linalg.norm(right)
        up = np.cross(forward, right)
        tangent = np.tan(np.radians(camera["vertical_fov_degrees"]) / 2)
        x = (2 * (np.arange(width) + .5) / width - 1)[None, :]
        y = (1 - 2 * (np.arange(height) + .5) / height)[:, None]
        rays = forward + x[:, :, None] * tangent * width / height * right + y[:, :, None] * tangent * up
        depth = -eye[1] / rays[:, :, 1]  # Floor y=0, independent of the shader's cascade selection.
        positions = eye + depth[:, :, None] * rays
        index = (depth[:, :, None] > splits[:3]).sum(axis=2)
        previous = np.maximum(index - 1, 0)
        band = (index > 0) & (depth < ends[previous])
        weight = np.clip((depth - splits[previous]) / (ends[previous] - splits[previous] + 1e-30), 0, 1)
        expected = np.where(band, pixels[:, :, 1] * (1 - weight) + pixels[:, :, 2] * weight, pixels[:, :, 2])
        expected = expected + (1 - expected) * np.clip((depth - 90) / 10, 0, 1)
        # Reject sky/out-of-floor pixels and locations potentially covered by the box.
        floor = (np.abs(positions[:, :, 0]) < 19.9) & (np.abs(positions[:, :, 2]) < 19.9) & (positions[:, :, 2] > 2.2)
        band_floor = band & floor
        assert band_floor.sum() > 100, (run["name"], "No meaningful transition samples")
        assert (pixels[:, :, 3][band_floor] == 1).all(), (run["name"], "Invalid overlap coverage")
        valid = (pixels[:, :, 3] == 1) & floor
        error = np.abs(pixels[:, :, 0] - expected)
        assert error[valid].max() < .0025, (run["name"], float(error[valid].max()))
        differing = band_floor & (np.abs(pixels[:, :, 1] - pixels[:, :, 2]) > .01)
        assert differing.sum() > 100, (run["name"], "Transition test has no distinct PCSS inputs")
        result.update(transition_pixels=int(band_floor.sum()), tested_pixels=int(valid.sum()),
                      distinct_input_pixels=int(differing.sum()),
                      max_blend_error=float(error[valid].max()),
                      transition_cascades=sorted(set(index[band_floor].tolist())))
    checks.append(result)
report = {"passed": True, "runs": len(checks), "checks": checks,
          "pixel_tolerance": .0025, "interpretation": "Actual shadow vs independent depth-based lerp of separately sampled PCSS results."}
(STAGE / "validation.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
print(json.dumps({"passed": True, "runs": len(checks), "diagnostic_checks": [c for c in checks if "max_blend_error" in c]}, indent=2))
