"""Offline HDR/timestamp analysis; this never creates a device or changes settings."""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]


def stats(values):
    values = np.asarray(list(values), dtype=np.float64)
    values = values[np.isfinite(values)]
    if not values.size:
        return None
    return {"count": int(values.size), "median": float(np.median(values)),
            "p95": float(np.percentile(values, 95)), "maximum": float(values.max()),
            "mean": float(values.mean())}


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load_capture(capture, width, height):
    path = ROOT / capture["linear_hdr"]
    rgba = np.fromfile(path, dtype="<f4")
    if rgba.size != width * height * 4:
        raise ValueError(f"Incorrect HDR size: {path}")
    rgba = rgba.reshape(height, width, 4)
    depth = np.fromfile(ROOT / capture["raster_depth"], dtype="<f4")
    if depth.size != width * height:
        raise ValueError("Incorrect depth size")
    return rgba, depth.reshape(height, width) < 1


def summarize_report(path, discard):
    report = json.loads(path.read_text(encoding="utf-8-sig"))
    width, height = report["internal_resolution"]
    result = {"report": str(path.relative_to(ROOT)), "report_sha256": sha256(path),
              "label": report["label"], "resolution": [width, height],
              "gi_only": report["gi_only"], "pipeline": report["pipeline"],
              "native_debug": report.get("native_debug"),
              "debug_shader_policy": report["debug_shader_policy"],
              "raw_snapshot_sha256": report["raw_snapshot_sha256"],
              "discard_initial_frames_per_sequence": discard,
              "timings": [], "images": [], "same_report_mode_comparisons": []}
    frames = report["frame_gpu_samples"]
    for camera, mode in sorted({(f["camera"], f["mode"]) for f in frames}):
        selected = [f for f in frames if f["camera"] == camera and f["mode"] == mode
                    and f["sample"] >= discard]
        names = sorted({p["name"] for f in selected for p in f["passes"]})
        entry = {"camera": camera, "mode": mode,
                 "renderer_gpu_ms": stats(f["renderer_gpu_ms"] for f in selected),
                 "ssgi_gpu_ms": stats(f["ssgi_gpu_ms"] for f in selected),
                 "passes": {name: stats(p["gpu_ms"] for f in selected for p in f["passes"]
                                        if p["name"] == name) for name in names}}
        result["timings"].append(entry)
    arrays = {}
    for capture in report["captures"]:
        rgba, mask = load_capture(capture, width, height)
        rgb = rgba[:, :, :3].astype(np.float64)
        foreground = rgb[mask]
        finite = np.isfinite(rgba)
        if not finite.all():
            raise ValueError(f"Nonfinite HDR: {capture['linear_hdr']}")
        luminance = foreground @ np.array([0.2126, 0.7152, 0.0722])
        image = {key: capture[key] for key in ("camera", "mode", "sample", "image", "linear_hdr")}
        image.update({"sha256": sha256(ROOT / capture["linear_hdr"]),
                      "foreground_pixels": int(mask.sum()),
                      "nonfinite_channels": int((~finite).sum()),
                      "negative_rgb_channels": int((rgb < -1e-7).sum()),
                      "foreground_luminance": stats(luminance),
                      "foreground_nonzero_fraction": float(np.mean(luminance > 1e-7))
                      if luminance.size else None,
                      "maximum_rgb": float(rgb.max()), "minimum_rgb": float(rgb.min())})
        result["images"].append(image)
        arrays[capture["camera"], capture["mode"], capture["sample"]] = (capture, rgb, mask)
    for (camera, mode, sample), (candidate, rgb, mask) in arrays.items():
        if mode <= 0 or (camera, 0, sample) not in arrays:
            continue
        baseline, source, source_mask = arrays[camera, 0, sample]
        if not np.array_equal(mask, source_mask):
            raise ValueError("Mode comparison has different depth masks")
        difference = rgb[mask] - source[mask]
        absolute = np.abs(difference)
        delta_luminance = difference @ np.array([0.2126, 0.7152, 0.0722])
        result["same_report_mode_comparisons"].append({
            "camera": camera, "baseline_mode": 0, "candidate_mode": mode,
            "sample": sample, "foreground_pixels": int(mask.sum()),
            "mean_rgb_absolute_error": float(absolute.mean()) if absolute.size else None,
            "rgb_rmse": float(np.sqrt(np.mean(difference ** 2))) if difference.size else None,
            "maximum_rgb_absolute_error": float(absolute.max()) if absolute.size else None,
            "foreground_changed_fraction": float(np.mean(np.any(absolute > 1e-7, axis=1)))
            if absolute.size else None,
            "mean_luminance_delta": float(delta_luminance.mean()) if delta_luminance.size else None,
            "contract": "Same frozen RAW/camera/frame/depth. Difference measures added world fallback and denoiser response; no ground truth."})
    for key in ("raw_statistics", "raw_validation", "probe_warmup_batches", "memory_and_cache",
                "initial_scene_prepare_cpu", "mesh_instance_upload_verification"):
        if key in report:
            result[key] = report[key]
    warm = report.get("probe_warmup_samples", [])
    if warm:
        result["warmup_timing_fields"] = sorted(warm[0])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reports", nargs="+", type=Path)
    parser.add_argument("--discard", type=int, default=2)
    parser.add_argument("--output", type=Path, default=ROOT / "tmp/VXGIMeshSDFAcceptance-summary.json")
    args = parser.parse_args()
    results = [summarize_report(p.resolve(), args.discard) for p in args.reports]
    output = {"contract": "Offline linear HDR/depth and existing D3D12 timestamp analysis. No FPS extrapolation or quality ground truth.",
              "reports": results}
    args.output.write_text(json.dumps(output, indent=2, ensure_ascii=False, allow_nan=False) + "\n", encoding="utf-8")
    for result in results:
        print(result["label"])
        for entry in result["timings"]:
            trace = entry["passes"].get("SSGI.Trace")
            print(f"  camera{entry['camera']} mode{entry['mode']}: Trace={trace}, renderer={entry['renderer_gpu_ms']}")
    print(args.output)


if __name__ == "__main__":
    main()
