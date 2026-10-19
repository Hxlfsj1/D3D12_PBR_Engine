"""Create isolated RAW controls and compare headless linear-HDR captures.

This tool never changes the source cache or renderer.  Constant controls are
fabricated diagnostics, not physically valid probe captures or a proposed fix.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import struct


RECORD_BYTES = 96
Y00 = 0.28209479177387814
PROJECT = Path(__file__).resolve().parents[1]
DEFAULT_RAW = PROJECT / "tmp/vxgi-stage75-performance/resource-init-1707x1067-raw.bin"


def sha256(data: bytes | bytearray) -> str:
    return hashlib.sha256(data).hexdigest()


def inspect(data: bytes | bytearray) -> dict:
    if len(data) == 0 or len(data) % RECORD_BYTES:
        raise ValueError("RAW must contain a nonzero whole number of 96-byte probe records")
    state_counts: dict[str, int] = {}
    reason_counts: dict[str, int] = {}
    valid_updates: dict[str, int] = {}
    nonfinite = zero_sh = negative_direction_possible = 0
    for offset in range(0, len(data), RECORD_BYTES):
        sh = struct.unpack_from("<16f", data, offset)
        state, updates, samples, frame = struct.unpack_from("<4I", data, offset + 64)
        backface, minimum_distance, classify_rays, reason = struct.unpack_from("<2f2I", data, offset + 80)
        state_counts[str(state)] = state_counts.get(str(state), 0) + 1
        reason_counts[str(reason)] = reason_counts.get(str(reason), 0) + 1
        if not all(math.isfinite(x) for x in (*sh, backface, minimum_distance)):
            nonfinite += 1
        rgb_coefficients = [sh[c * 4 + channel] for c in range(4) for channel in range(3)]
        zero_sh += int(all(value == 0 for value in rgb_coefficients))
        if state == 1 and updates > 0:
            valid_updates[str(updates)] = valid_updates.get(str(updates), 0) + 1
            # An L1 directional reconstruction ranges over DC +/- |linear|.
            negative_direction_possible += int(any(
                0.4886025119029199 * math.sqrt(sum(sh[c * 4 + channel] ** 2 for c in (1, 2, 3)))
                > Y00 * sh[channel]
                for channel in range(3)))
    return {
        "records": len(data) // RECORD_BYTES,
        "state_counts": state_counts,
        "reason_counts": reason_counts,
        "valid_update_distribution": valid_updates,
        "nonfinite_records": nonfinite,
        "zero_sh_records": zero_sh,
        "updated_valid_records_with_a_possible_negative_direction": negative_direction_possible,
    }


def make_controls(raw_path: Path, output_dir: Path) -> dict:
    original = raw_path.read_bytes()
    source_stats = inspect(original)
    if source_stats["nonfinite_records"]:
        raise ValueError("Source RAW contains nonfinite records; inspect them before making controls")
    original_hash = sha256(original)
    output_dir.mkdir(parents=True, exist_ok=True)
    report = {
        "scope": "CPU-only fabricated probe-cache controls; no GPU dispatch or production source changes",
        "source": {"path": str(raw_path.resolve()), "sha256": original_hash,
                   "bytes": len(original), "statistics": source_stats},
        "record_layout": {"bytes": RECORD_BYTES, "sh": [0, 64], "state": [64, 68],
                          "update_count": [68, 72], "sample_count": [72, 76], "reason": [92, 96]},
        "byte_range_convention": "half-open byte ranges within each 96-byte record",
        "controls": {},
    }
    constant_sh = struct.pack("<16f", 1.0 / Y00, 1.0 / Y00, 1.0 / Y00, 0.0,
                              *([0.0] * 12))
    for name in ("valid_constant", "all_constant", "l0_only"):
        control = bytearray(original)
        modified_records = modified_bytes = 0
        for offset in range(0, len(control), RECORD_BYTES):
            state, updates = struct.unpack_from("<2I", original, offset + 64)
            if name == "l0_only":
                control[offset + 16:offset + 64] = bytes(48)
            elif name == "all_constant" or (state == 1 and updates > 0):
                control[offset:offset + 64] = constant_sh
                if name == "all_constant":
                    struct.pack_into("<3I", control, offset + 64, 1, 32, 2048)
                    struct.pack_into("<I", control, offset + 92, 0)
            before = original[offset:offset + RECORD_BYTES]
            after = control[offset:offset + RECORD_BYTES]
            changed = sum(a != b for a, b in zip(before, after))
            modified_records += changed > 0
            modified_bytes += changed
        destination = output_dir / (name + "-raw.bin")
        destination.write_bytes(control)
        ranges = ([[16, 64]] if name == "l0_only" else
                  [[0, 64], [64, 76], [92, 96]] if name == "all_constant" else [[0, 64]])
        report["controls"][name] = {
            "path": str(destination.resolve()), "sha256": sha256(control), "bytes": len(control),
            "modified_records": modified_records, "modified_bytes": modified_bytes,
            "allowed_modified_ranges": ranges,
            "selection": "updated valid records only" if name == "valid_constant" else "every allocated record",
            "meaning": {
                "valid_constant": "Keep original query eligibility; replace accepted records by isotropic RGB radiance 1",
                "all_constant": "Force every allocated record query-eligible with isotropic RGB radiance 1; missing cells remain missing",
                "l0_only": "Keep eligibility and original L0; remove directional L1 terms from every record",
            }[name],
            "statistics": inspect(control),
        }
    if sha256(raw_path.read_bytes()) != original_hash:
        raise RuntimeError("Source RAW changed during control creation")
    report["source_verified_unchanged"] = True
    return report


def analyze_captures(args: argparse.Namespace) -> dict:
    import numpy as np

    if not args.width or not args.height:
        raise ValueError("Capture analysis requires --width and --height")
    shape = (args.height, args.width)

    def load_capture(path: Path):
        value = np.fromfile(path, dtype="<f4")
        if value.size != args.width * args.height * 4:
            raise ValueError(f"Capture dimensions do not match {path}")
        return value.reshape((*shape, 4))[:, :, :3].astype(np.float64)

    base = load_capture(args.baseline)
    selection = np.ones(shape, dtype=bool)
    selection_description = "all pixels; background is included unless a mask or depth is provided"
    if args.depth:
        depth = np.fromfile(args.depth, dtype="<f4")
        if depth.size != args.width * args.height:
            raise ValueError("Depth dimensions do not match capture")
        selection &= np.isfinite(depth.reshape(shape)) & (depth.reshape(shape) < 1.0)
        selection_description = "finite foreground depth < 1"
    if args.mask:
        from PIL import Image
        mask = np.asarray(Image.open(args.mask).convert("L"))
        if mask.shape != shape:
            raise ValueError("Mask must match capture dimensions exactly")
        selection &= mask > 0
        selection_description += "; intersected with nonzero PNG mask"
    finite = np.all(np.isfinite(base), axis=2)
    base_luma = base @ np.array([0.2126, 0.7152, 0.0722])
    base_black = selection & finite & (base_luma <= args.black_threshold)
    result = {
        "scope": "Linear HDR final captures; this does not identify individual ray query states",
        "dimensions": [args.width, args.height], "mask": selection_description,
        "selected_pixels": int(selection.sum()), "baseline": str(args.baseline.resolve()),
        "baseline_sha256": sha256(args.baseline.read_bytes()),
        "black_threshold_linear_luminance": args.black_threshold,
        "baseline_black_pixels": int(base_black.sum()), "controls": {},
    }
    for name in ("valid_constant", "all_constant", "l0_only"):
        path = getattr(args, name)
        if path is None:
            continue
        image = load_capture(path)
        image_finite = np.all(np.isfinite(image), axis=2)
        luma = image @ np.array([0.2126, 0.7152, 0.0722])
        paired = selection & finite & image_finite
        formerly_black = base_black & image_finite
        filled = formerly_black & (luma > args.black_threshold)
        delta = luma[paired] - base_luma[paired]
        result["controls"][name] = {
            "path": str(path.resolve()), "sha256": sha256(path.read_bytes()),
            "paired_finite_pixels": int(paired.sum()),
            "selected_nonfinite_pixels": int((selection & ~image_finite).sum()),
            "formerly_black_pixels_filled": int(filled.sum()),
            "formerly_black_fill_fraction": float(filled.sum() / formerly_black.sum()) if formerly_black.any() else None,
            "mean_luminance_in_baseline_black": float(luma[formerly_black].mean()) if formerly_black.any() else None,
            "mean_luminance_delta": float(delta.mean()) if delta.size else None,
            "luminance_delta_quantiles_0_25_50_75_100": np.quantile(delta, [0, .25, .5, .75, 1]).tolist() if delta.size else None,
        }
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--raw", type=Path, default=DEFAULT_RAW)
    parser.add_argument("--output-dir", type=Path, default=PROJECT / "tmp/vxgi-black-blocks")
    parser.add_argument("--baseline", type=Path, help="Optional baseline RGBA32_FLOAT capture")
    parser.add_argument("--valid-constant", type=Path, help="Matching valid_constant control capture")
    parser.add_argument("--all-constant", type=Path, help="Matching all_constant control capture")
    parser.add_argument("--l0-only", type=Path, help="Matching l0_only control capture")
    parser.add_argument("--width", type=int)
    parser.add_argument("--height", type=int)
    parser.add_argument("--depth", type=Path, help="Optional baseline float32 depth; exclude background")
    parser.add_argument("--mask", type=Path, help="Optional exact-size PNG; nonzero pixels are included")
    parser.add_argument("--black-threshold", type=float, default=1e-5)
    args = parser.parse_args()
    report = make_controls(args.raw, args.output_dir)
    if args.baseline:
        report["capture_analysis"] = analyze_captures(args)
    path = args.output_dir / "cpu-controls-report.json"
    path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"report": str(path.resolve()), "source_verified_unchanged": report["source_verified_unchanged"],
                      "controls": {name: value["path"] for name, value in report["controls"].items()}}, indent=2))


if __name__ == "__main__":
    main()
