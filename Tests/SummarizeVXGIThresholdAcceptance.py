"""Compare frozen-RAW renderer captures without creating a device or changing settings.

Reference means the previous exact triangle-refined consumer, not ground truth.
All reported errors use linear HDR; previews use one fixed exposure for both inputs.
"""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
LUMINANCE = np.array([0.2126, 0.7152, 0.0722], dtype=np.float64)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def relative(path):
    return path.resolve().relative_to(ROOT).as_posix()


def read_report(path):
    report = json.loads(path.read_text(encoding="utf-8-sig"))
    raw_path = ROOT / report["raw_snapshot"]
    raw_hash = sha256(raw_path)
    if raw_hash != report["raw_snapshot_sha256"].upper():
        raise ValueError(f"Frozen RAW no longer matches report: {path}")
    if not report.get("raw_gpu_bytes_unchanged_after_all_captures", False):
        raise ValueError(f"Report did not verify immutable RAW: {path}")
    if not report["raw_validation"]["quality_ready"]:
        raise ValueError(f"RAW is not fully classified and captured: {path}")
    return report


def raw_statistics(path):
    record_type = np.dtype([("sh", "<f4", (4, 4)), ("state", "<u4"),
                            ("updates", "<u4"), ("samples", "<u4"), ("frame", "<u4"),
                            ("backface", "<f4"), ("min_distance", "<f4"),
                            ("classification_rays", "<u4"), ("reason", "<u4")])
    if path.stat().st_size % record_type.itemsize or record_type.itemsize != 96:
        raise ValueError("RAW does not use the production 96-byte probe record")
    records = np.fromfile(path, dtype=record_type)
    if not np.isfinite(records["sh"]).all() or np.any(records["sh"][:, :, 3] != 0):
        raise ValueError("RAW contains invalid SH coefficients/padding")
    if np.any(records["state"] > 2) or np.any(records["reason"] > 4):
        raise ValueError("RAW contains invalid classifications")
    valid = records["state"] == 1
    values, counts = np.unique(records["updates"][valid], return_counts=True)
    return {"record_bytes": record_type.itemsize, "records": int(records.size),
            "valid": int(valid.sum()), "disabled": int((records["state"] == 2).sum()),
            "unclassified": int((records["state"] == 0).sum()),
            "nonzero_sh_records": int(np.any(records["sh"][:, :, :3] != 0, axis=(1, 2)).sum()),
            "maximum_absolute_sh_coefficient": float(np.abs(records["sh"][:, :, :3]).max()),
            "valid_update_distribution": [{"updates": int(value), "probes": int(count)}
                                          for value, count in zip(values, counts)],
            "valid_minimum_updates": int(records["updates"][valid].min()) if valid.any() else 0,
            "valid_maximum_updates": int(records["updates"][valid].max()) if valid.any() else 0}


def source_manifest(path):
    manifest = path.with_name(path.name.replace("-report.json", "-input-hashes.json"))
    records = json.loads(manifest.read_text(encoding="utf-8-sig"))
    selected = {record["path"].lower(): record["sha256"].upper() for record in records
                if record["path"].lower().startswith(("settings/", "models/", "hdrs/"))}
    for required in ("settings/pipeline.json", "settings/scene.json", "settings/lighting.json"):
        if required not in selected:
            raise ValueError(f"Input manifest does not identify {required}")
    return selected


def load_capture(capture, resolution):
    width, height = resolution
    rgba = np.fromfile(ROOT / capture["linear_hdr"], dtype="<f4")
    depth = np.fromfile(ROOT / capture["raster_depth"], dtype="<f4")
    if rgba.size != width * height * 4 or depth.size != width * height:
        raise ValueError(f"Capture dimensions differ: {capture['linear_hdr']}")
    rgba = rgba.reshape(height, width, 4)
    depth = depth.reshape(height, width)
    if not np.isfinite(rgba).all() or not np.isfinite(depth).all():
        raise ValueError("Capture contains nonfinite HDR or depth")
    if np.any((depth < 0) | (depth > 1)):
        raise ValueError("Invalid raster depth")
    return rgba, depth


def distribution(values):
    values = np.asarray(values, dtype=np.float64)
    if not values.size:
        return {"count": 0}
    return {"count": int(values.size), "mean": float(values.mean()),
            "median": float(np.median(values)), "p95": float(np.percentile(values, 95)),
            "maximum": float(values.max()), "minimum": float(values.min())}


def difference(source, candidate, mask):
    delta = candidate[:, :, :3].astype(np.float64) - source[:, :, :3]
    selected = delta[mask]
    absolute = np.abs(selected)
    source_luma = source[:, :, :3].astype(np.float64)[mask] @ LUMINANCE
    candidate_luma = candidate[:, :, :3].astype(np.float64)[mask] @ LUMINANCE
    return {"foreground_pixels": int(mask.sum()),
            "foreground_changed_pixels": int(np.any(absolute > 1e-7, axis=1).sum()),
            "foreground_changed_fraction": float(np.any(absolute > 1e-7, axis=1).mean()),
            "rgb_mean_absolute_difference": float(absolute.mean()),
            "rgb_rmse": float(np.sqrt(np.mean(selected ** 2))),
            "rgb_maximum_absolute_difference": float(absolute.max()),
            "mean_luminance_delta": float((candidate_luma - source_luma).mean()),
            "reference_foreground_luminance": distribution(source_luma),
            "candidate_foreground_luminance": distribution(candidate_luma),
            "reference_foreground_nonzero_fraction": float((source_luma > 1e-7).mean()),
            "candidate_foreground_nonzero_fraction": float((candidate_luma > 1e-7).mean()),
            "reference_negative_rgb_channels": int((source[:, :, :3] < -1e-7).sum()),
            "candidate_negative_rgb_channels": int((candidate[:, :, :3] < -1e-7).sum()),
            "reference_sky_maximum_absolute_rgb": float(np.abs(source[:, :, :3][~mask]).max())
            if (~mask).any() else 0.0,
            "candidate_sky_maximum_absolute_rgb": float(np.abs(candidate[:, :, :3][~mask]).max())
            if (~mask).any() else 0.0}


def preview(rgb, exposure):
    mapped = np.maximum(rgb.astype(np.float64) * exposure, 0)
    mapped /= 1 + mapped
    mapped = np.where(mapped <= 0.0031308, 12.92 * mapped,
                      1.055 * np.power(mapped, 1 / 2.4) - 0.055)
    return Image.fromarray(np.uint8(np.clip(np.rint(mapped * 255), 0, 255)))


def sheet(rows, output, exposure, difference_exposure):
    if not rows:
        return None
    height, width = rows[0][1].shape[:2]
    header, footer = 28, 28
    canvas = Image.new("RGB", (width * 3, header + len(rows) * (height + footer)), "#16181b")
    draw = ImageDraw.Draw(canvas)
    labels = [f"Exact reference, exposure x{exposure:g}",
              f"Threshold, exposure x{exposure:g}",
              f"Absolute RGB delta, exposure x{difference_exposure:g}"]
    for column, label in enumerate(labels):
        draw.text((column * width + 8, 8), label, fill="white")
    for index, (label, source, candidate) in enumerate(rows):
        y = header + index * (height + footer)
        for column, image in enumerate((preview(source[:, :, :3], exposure),
                                        preview(candidate[:, :, :3], exposure),
                                        preview(np.abs(candidate[:, :, :3] - source[:, :, :3]),
                                                difference_exposure))):
            canvas.paste(image, (column * width, y))
        draw.text((8, y + height + 7), label, fill="white")
    canvas.save(output)
    return relative(output)


def timing_summary(report, discard):
    result = []
    frames = report["frame_gpu_samples"]
    for camera, mode in sorted({(f["camera"], f["mode"]) for f in frames}):
        selected = [f for f in frames if f["camera"] == camera and f["mode"] == mode
                    and f["sample"] >= discard]
        result.append({"camera": camera, "mode": mode,
                       "renderer_gpu_ms": distribution([f["renderer_gpu_ms"] for f in selected]),
                       "ssgi_gpu_ms": distribution([f["ssgi_gpu_ms"] for f in selected]),
                       "trace_gpu_ms": distribution([p["gpu_ms"] for f in selected
                                                     for p in f["passes"] if p["name"] == "SSGI.Trace"])})
    return result


def compare(reference_path, candidate_path, output, discard, mode, difference_exposure,
            require_screen_bit_identity=False):
    reference = read_report(reference_path)
    candidate = read_report(candidate_path)
    for key in ("raw_snapshot_sha256", "internal_resolution", "gi_only", "pipeline", "scene"):
        if reference[key] != candidate[key]:
            raise ValueError(f"Reference/candidate contract differs: {key}")
    for key in ("samples_per_sequence", "repeat_sequences", "debug_shader_policy", "native_debug", "native_configuration"):
        if reference.get(key) != candidate.get(key):
            raise ValueError(f"Reference/candidate sampling/build policy differs: {key}")
    self_check = reference_path == candidate_path
    if not self_check and reference.get("exact_fallback") is not True:
        raise ValueError("Reference capture did not explicitly identify the exact fallback")
    if not self_check and candidate.get("exact_fallback") is not False:
        raise ValueError("Candidate capture did not explicitly identify the threshold fallback")
    reference_manifest = source_manifest(reference_path)
    if reference_manifest != source_manifest(candidate_path):
        raise ValueError("Scene/configuration/HDR/model input manifest differs")
    resolution = reference["internal_resolution"]
    reference_captures = {(c["camera"], c["mode"]): c for c in reference["captures"]}
    candidate_captures = {(c["camera"], c["mode"]): c for c in candidate["captures"]}
    if reference_captures.keys() != candidate_captures.keys():
        raise ValueError("Reference/candidate camera and mode sets differ")
    shared = sorted(reference_captures.keys() & candidate_captures.keys())
    if not shared:
        raise ValueError("No matching captured cameras and modes")
    result = {"contract": "Same frozen RAW, production inputs, finite ray radius, camera, sample and raster depth. Linear HDR deltas compare two consumer algorithms; exact reference is not ground truth. Timings exclude probe updates, UI, Present and readbacks and cannot be converted to main-program FPS.",
              "reference_report": relative(reference_path),
              "candidate_report": relative(candidate_path),
              "self_check_only": self_check,
              "identical_source_input_hashes": reference_manifest,
              "reference_report_sha256": sha256(reference_path),
              "candidate_report_sha256": sha256(candidate_path),
              "raw_snapshot_sha256": reference["raw_snapshot_sha256"],
              "independent_raw_statistics": raw_statistics(ROOT / candidate["raw_snapshot"]),
              "raw_validation": candidate["raw_validation"],
              "pipeline": candidate["pipeline"], "gi_only": candidate["gi_only"],
              "internal_resolution": resolution, "comparisons": [],
              "reference_timings": timing_summary(reference, discard),
              "candidate_timings": timing_summary(candidate, discard),
              "discard_initial_frames_per_sequence": discard,
              "screen_only_comparisons": []}
    rows = []
    cached = {}
    for camera, capture_mode in shared:
        first, second = reference_captures[camera, capture_mode], candidate_captures[camera, capture_mode]
        if first["sample"] != second["sample"]:
            raise ValueError("Final captured sample differs")
        source, source_depth = load_capture(first, resolution)
        image, image_depth = load_capture(second, resolution)
        if not np.array_equal(source_depth.view(np.uint32), image_depth.view(np.uint32)):
            raise ValueError("Raster depth differs between reference and candidate")
        entry = {"camera": camera, "mode": capture_mode, "sample": first["sample"],
                 "depth_bit_identical": True, "rgba_bit_identical": bool(np.array_equal(source.view(np.uint32), image.view(np.uint32))),
                 "reference_image": first["image"], "candidate_image": second["image"],
                 "reference_linear_hdr_sha256": sha256(ROOT / first["linear_hdr"]),
                 "candidate_linear_hdr_sha256": sha256(ROOT / second["linear_hdr"]),
                 "raster_depth_sha256": sha256(ROOT / first["raster_depth"]),
                 **difference(source, image, image_depth < 1)}
        if require_screen_bit_identity and capture_mode <= 0 and not entry["rgba_bit_identical"]:
            raise ValueError("GI off or screen-only changed despite identical source inputs")
        result["comparisons"].append(entry)
        cached[camera, capture_mode] = (source, image, image_depth)
        if capture_mode == mode:
            rows.append((f"camera {camera}, mode {capture_mode}, sample {first['sample']}; reference is not ground truth", source, image))
    for (camera, capture_mode), (source, image, depth) in cached.items():
        if capture_mode != mode or (camera, 0) not in cached:
            continue
        reference_screen, candidate_screen, _ = cached[camera, 0]
        for label, screen, hybrid in (("reference", reference_screen, source),
                                       ("candidate", candidate_screen, image)):
            result["screen_only_comparisons"].append({"camera": camera, "consumer": label,
                                                       "baseline_mode": 0, "candidate_mode": capture_mode,
                                                       **difference(screen, hybrid, depth < 1)})
    output.parent.mkdir(parents=True, exist_ok=True)
    result["screen_only_bit_identity_proof"] = {
        "cameras": [entry["camera"] for entry in result["comparisons"] if entry["mode"] == 0],
        "all_captured_screen_only_rgba_bit_identical": all(
            entry["rgba_bit_identical"] for entry in result["comparisons"] if entry["mode"] == 0),
        "available": any(entry["mode"] == 0 for entry in result["comparisons"])}
    reference_timings = {(entry["camera"], entry["mode"]): entry for entry in result["reference_timings"]}
    result["timing_ratios"] = []
    for entry in result["candidate_timings"]:
        previous = reference_timings.get((entry["camera"], entry["mode"]))
        if previous is None:
            continue
        result["timing_ratios"].append({"camera": entry["camera"], "mode": entry["mode"],
                                        "candidate_over_reference_median": {
                                            key: entry[key]["median"] / previous[key]["median"]
                                            for key in ("trace_gpu_ms", "ssgi_gpu_ms", "renderer_gpu_ms")
                                            if previous[key].get("median", 0) > 0}})
    result["comparison_image"] = sheet(rows, output.with_suffix(".png"),
                                        4 if candidate["gi_only"] else 1, difference_exposure)
    output.write_text(json.dumps(result, indent=2, ensure_ascii=False, allow_nan=False) + "\n", encoding="utf-8")
    for entry in result["comparisons"]:
        print(f"camera {entry['camera']} mode {entry['mode']}: changed {entry['foreground_changed_fraction']:.3%}, MAE {entry['rgb_mean_absolute_difference']:.8f}, luma delta {entry['mean_luminance_delta']:.8f}")
    print(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", required=True, type=Path)
    parser.add_argument("--candidate", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--discard", type=int, default=3)
    parser.add_argument("--mode", type=int, default=2)
    parser.add_argument("--difference-exposure", type=float, default=20)
    parser.add_argument("--require-screen-bit-identity", action="store_true",
                        help="Fail on any GI-off/screen-only bit change instead of reporting it")
    args = parser.parse_args()
    compare(args.reference.resolve(), args.candidate.resolve(), args.output.resolve(),
            args.discard, args.mode, args.difference_exposure, args.require_screen_bit_identity)


if __name__ == "__main__":
    main()
