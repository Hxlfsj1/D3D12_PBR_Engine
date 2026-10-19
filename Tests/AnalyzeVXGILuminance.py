"""Analyze matched production HDR captures on the CPU; no device or settings changes.

Example: python Tests/AnalyzeVXGILuminance.py --normal front=tmp/front-report.json
         --gi front=tmp/front-gi-report.json --output tmp/vxgi-luminance
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

import numpy as np


ROOT = Path(__file__).resolve().parents[1]
LUMA = np.array([0.2126, 0.7152, 0.0722], dtype=np.float64)
MODE_NAMES = {-1: "base_no_ssgi", 0: "ssgi_only_fallback_disabled", 2: "full_ssgi_plus_vxgi"}
EPSILON = 1e-12


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def resolve(path: str | Path) -> Path:
    path = Path(path)
    return path.resolve() if path.is_absolute() else (ROOT / path).resolve()


def label_path(path: Path) -> str:
    try:
        return path.relative_to(ROOT).as_posix()
    except ValueError:
        return str(path)


def ratio(numerator: float, denominator: float) -> float | None:
    return float(numerator / denominator) if abs(denominator) > EPSILON else None


def distribution(values: np.ndarray) -> dict:
    if not values.size:
        return {"count": 0}
    return {"count": int(values.size), "mean": float(values.mean()),
            "minimum": float(values.min()), "maximum": float(values.max()),
            **{f"p{q:02}": float(np.percentile(values, q)) for q in (1, 10, 20, 50, 80, 90, 99)}}


def load_report(path: Path, gi_only: bool) -> tuple[dict, dict]:
    report = json.loads(path.read_text(encoding="utf-8-sig"))
    if not report.get("completed") or report.get("d3d12_errors") != 0:
        raise ValueError(f"Incomplete report or D3D12 errors: {path}")
    if report.get("gi_only") is not gi_only:
        raise ValueError(f"Expected gi_only={gi_only}: {path}")
    if not report.get("raw_gpu_bytes_unchanged_after_all_captures"):
        raise ValueError(f"Report does not establish an immutable probe cache: {path}")
    if not report.get("raw_validation", {}).get("quality_ready"):
        raise ValueError(f"Probe cache is not fully classified/captured: {path}")
    raw_path = resolve(report["raw_snapshot"])
    raw_hash = sha256(raw_path)
    if raw_hash.lower() != report["raw_snapshot_sha256"].lower():
        raise ValueError(f"Probe RAW hash differs from report: {path}")
    evidence = {"report": label_path(path), "report_sha256": sha256(path),
                "raw_snapshot": label_path(raw_path), "raw_snapshot_sha256": raw_hash,
                "raw_hash_verified": True, "gi_only": gi_only,
                "resolution": report["internal_resolution"],
                "pipeline": report.get("pipeline"), "camera_override": report.get("camera_override"),
                "raw_validation": report["raw_validation"]}
    manifest_path = path.with_name(path.name.replace("-report.json", "-input-hashes.json"))
    inputs = {}
    if manifest_path != path and manifest_path.exists():
        records = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
        inputs = {item["path"].replace("\\", "/").lower(): item["sha256"].lower()
                  for item in records
                  if item["path"].replace("\\", "/").lower().startswith(("settings/", "models/", "hdrs/"))}
        evidence["source_manifest"] = label_path(manifest_path)
        evidence["source_manifest_sha256"] = sha256(manifest_path)
    evidence["recorded_scene_input_hashes"] = inputs
    evidence["source_manifest_available"] = bool(inputs)
    return report, evidence


def load_modes(report: dict, modes: tuple[int, ...]) -> tuple[dict, np.ndarray, np.ndarray, list]:
    width, height = report["internal_resolution"]
    if not isinstance(width, int) or not isinstance(height, int) or width <= 0 or height <= 0:
        raise ValueError("Invalid internal_resolution")
    selected = [entry for entry in report["captures"] if entry["mode"] in modes]
    counts = {mode: sum(entry["mode"] == mode for entry in selected) for mode in modes}
    if any(count != 1 for count in counts.values()):
        raise ValueError(f"Expected one capture per required mode in each named report: {counts}")
    captures = {entry["mode"]: entry for entry in selected}
    first = captures[modes[0]]
    required_matrix = np.asarray(first.get("inverse_view_projection_row_major", []), dtype=np.float64)
    if required_matrix.size != 16 or not np.isfinite(required_matrix).all():
        raise ValueError("CustomCamera inverse_view_projection_row_major is required for geometry masks")
    matrix = required_matrix.reshape(4, 4)
    images, evidence, reference_depth = {}, [], None
    for mode in modes:
        capture = captures[mode]
        for field in ("camera", "sample", "repeat"):
            if capture.get(field) != first.get(field):
                raise ValueError(f"Matched modes differ in {field}")
        other_matrix = np.asarray(capture.get("inverse_view_projection_row_major", []), dtype=np.float64)
        if not np.array_equal(required_matrix, other_matrix):
            raise ValueError("Matched modes have different inverse view-projection matrices")
        hdr_path, depth_path = resolve(capture["linear_hdr"]), resolve(capture["raster_depth"])
        rgba = np.fromfile(hdr_path, dtype="<f4")
        depth = np.fromfile(depth_path, dtype="<f4")
        if rgba.size != width * height * 4 or depth.size != width * height:
            raise ValueError(f"Capture dimensions differ: {hdr_path}")
        if not np.isfinite(rgba).all() or not np.isfinite(depth).all():
            raise ValueError(f"Nonfinite HDR/depth: {hdr_path}")
        if np.any((depth < 0) | (depth > 1)):
            raise ValueError(f"Depth outside DirectX [0,1]: {depth_path}")
        depth = depth.reshape(height, width)
        if reference_depth is not None and not np.array_equal(reference_depth.view(np.uint32), depth.view(np.uint32)):
            raise ValueError("Matched modes have different raster depth bits")
        reference_depth = depth
        rgb = rgba.reshape(height, width, 4)[..., :3].astype(np.float64)
        images[mode] = rgb @ LUMA
        evidence.append({**{key: capture.get(key) for key in ("camera", "mode", "sample", "repeat")},
                         "mode_name": MODE_NAMES[mode], "linear_hdr": label_path(hdr_path),
                         "linear_hdr_sha256": sha256(hdr_path), "raster_depth": label_path(depth_path),
                         "raster_depth_sha256": sha256(depth_path),
                         "inverse_view_projection_row_major": required_matrix.tolist(),
                         "negative_rgb_channels": int((rgb < 0).sum())})
    return images, reference_depth, matrix, evidence


def geometry_masks(depth: np.ndarray, inverse_vp: np.ndarray) -> dict:
    height, width = depth.shape
    foreground = depth < 1.0
    y, x = np.indices(depth.shape, dtype=np.float64)
    ndc = np.stack((2 * (x + .5) / width - 1, 1 - 2 * (y + .5) / height,
                    depth, np.ones(depth.shape)), axis=-1)
    homogeneous = ndc @ inverse_vp
    if not np.isfinite(homogeneous[foreground]).all() or np.any(np.abs(homogeneous[..., 3][foreground]) <= EPSILON):
        raise ValueError("Foreground inverse projection has invalid homogeneous coordinates")
    world_y = np.zeros(depth.shape, dtype=np.float64)
    world_y[foreground] = homogeneous[..., 1][foreground] / homogeneous[..., 3][foreground]
    return {"foreground": foreground, "above_ground_geometry_approximation": foreground & (world_y > .03)}


def rank_group(values: np.ndarray, mask: np.ndarray, upper: bool) -> tuple[np.ndarray, dict]:
    indices = np.flatnonzero(mask)
    count = max(1, int(np.floor(indices.size * .2))) if indices.size else 0
    order = np.argsort(values.ravel()[indices], kind="stable")
    selected = indices[order[-count:] if upper and count else order[:count]]
    group = np.zeros(mask.shape, dtype=bool)
    group.ravel()[selected] = True
    cutoff = float(values.ravel()[selected].min() if upper else values.ravel()[selected].max()) if count else None
    return group, {"count": count, "scope_fraction": ratio(count, indices.size), "cutoff_luminance": cutoff,
                   "scope_pixels_equal_to_cutoff": int(np.sum(values[mask] == cutoff)) if count else 0,
                   "tie_policy": "Stable luminance ranking, then flattened pixel index; exact floor(20% N), minimum one for nonempty masks."}


def change(before: np.ndarray, after: np.ndarray, mask: np.ndarray, scope: np.ndarray) -> dict:
    delta = (after - before)[mask]
    scope_delta = (after - before)[scope]
    positive, negative = np.maximum(delta, 0), np.minimum(delta, 0)
    if not delta.size:
        return {"pixels": 0}
    gain_share = ratio(positive.sum(), np.maximum(scope_delta, 0).sum())
    pixel_share = ratio(delta.size, scope_delta.size)
    return {"pixels": int(delta.size), "mean_delta": float(delta.mean()),
            "sum_delta": float(delta.sum()), "positive_gain_sum": float(positive.sum()),
            "negative_change_sum": float(negative.sum()),
            "positive_pixel_fraction_above_1e-7": float(np.mean(delta > 1e-7)),
            "negative_pixel_fraction_below_minus_1e-7": float(np.mean(delta < -1e-7)),
            "scope_positive_gain_share": gain_share,
            "gain_concentration_vs_area": ratio(gain_share, pixel_share) if gain_share is not None else None,
            "ratio_of_after_to_before_means": ratio(after[mask].mean(), before[mask].mean()),
            "relative_mean_change": ratio(delta.mean(), before[mask].mean()),
            "delta_distribution": distribution(delta)}


def group_metrics(images: dict, group: np.ndarray, scope: np.ndarray, thresholds: list) -> dict:
    result = {"pixels": int(group.sum()),
              "modes": {str(mode): {"luminance": distribution(image[group]),
                                    "low_luminance_fractions": [
                                        {"threshold": threshold, "fraction": float(np.mean(image[group] <= threshold))}
                                        for threshold in thresholds] if group.any() else []}
                        for mode, image in images.items()}}
    result["vxgi_effect_F_minus_S"] = change(images[0], images[2], group, scope)
    if -1 in images:
        result["ssgi_effect_S_minus_B"] = change(images[-1], images[0], group, scope)
        result["full_gi_effect_F_minus_B"] = change(images[-1], images[2], group, scope)
        d_s = result["ssgi_effect_S_minus_B"].get("mean_delta", 0)
        d_v = result["vxgi_effect_F_minus_S"].get("mean_delta", 0)
        result["ratio_of_vxgi_to_ssgi_mean_delta"] = ratio(d_v, d_s)
    return result


def compare_pair(normal_report: dict, gi_report: dict, normal_evidence: dict, gi_evidence: dict) -> None:
    for field in ("raw_snapshot_sha256", "internal_resolution", "pipeline", "samples_per_sequence",
                  "repeat_sequences", "downsample_factor", "debug_shader_policy", "native_debug", "exact_fallback",
                  "camera_override"):
        if normal_report.get(field) != gi_report.get(field):
            raise ValueError(f"Normal/pure-GI reports differ in {field}")
    if normal_evidence["recorded_scene_input_hashes"] != gi_evidence["recorded_scene_input_hashes"]:
        raise ValueError("Normal/pure-GI recorded scene/configuration/model/HDR input hashes differ")
    if normalized_scene(normal_report.get("scene")) != normalized_scene(gi_report.get("scene")):
        raise ValueError("Normal/pure-GI scene geometry or instances differ")


def normalized_scene(scene: dict | None) -> dict | None:
    if scene is None:
        return None
    return {**scene, "bounds": sorted(scene.get("bounds", []), key=lambda item: item["instance_id"])}


def analyze(name: str, normal_path: Path, gi_path: Path | None) -> dict:
    report, evidence = load_report(normal_path, False)
    images, depth, matrix, captures = load_modes(report, (-1, 0, 2))
    masks = geometry_masks(depth, matrix)
    result = {"name": name, "normal": evidence, "normal_captures": captures, "regions": {}}
    gi_images = None
    if gi_path is not None:
        gi_report, gi_evidence = load_report(gi_path, True)
        compare_pair(report, gi_report, evidence, gi_evidence)
        gi_images, gi_depth, gi_matrix, gi_captures = load_modes(gi_report, (0, 2))
        if not np.array_equal(gi_depth.view(np.uint32), depth.view(np.uint32)) or not np.array_equal(gi_matrix, matrix):
            raise ValueError("Normal/pure-GI geometry or camera differs")
        if any(captures[1][field] != gi_captures[0][field] for field in ("camera", "sample", "repeat")):
            raise ValueError("Normal/pure-GI sampling differs")
        result["pure_gi"] = gi_evidence
        result["pure_gi_captures"] = gi_captures
    for region_name, scope in masks.items():
        if not scope.any():
            result["regions"][region_name] = {"pixels": 0, "note": "Empty geometry selection; no numerical conclusion."}
            continue
        baseline_median = float(np.median(images[-1][scope]))
        relative_thresholds = [baseline_median * fraction for fraction in (.01, .05, .10)] if baseline_median > 0 else []
        thresholds = sorted(set([1e-5, 1e-4, 1e-3, 1e-2, *relative_thresholds]))
        region = {"pixels": int(scope.sum()), "image_fraction": float(scope.mean()),
                  "base_median_luminance": baseline_median,
                  "low_luminance_thresholds_absolute_hdr": [1e-5, 1e-4, 1e-3, 1e-2],
                  "low_luminance_thresholds_relative_to_base_median": relative_thresholds,
                  "all": group_metrics(images, scope, scope, thresholds), "groups": {}}
        group_masks = {}
        for source_mode, prefix in ((-1, "base"), (0, "ssgi")):
            for upper, suffix in ((False, "dark20"), (True, "bright20")):
                group, selection = rank_group(images[source_mode], scope, upper)
                group_name = f"{prefix}_{suffix}"
                group_masks[group_name] = group
                region["groups"][group_name] = {"selection": selection,
                                                 **group_metrics(images, group, scope, thresholds)}
        if gi_images is not None:
            region["pure_gi_auxiliary"] = {"all": group_metrics(gi_images, scope, scope, []),
                                            "groups": {key: group_metrics(gi_images, group, scope, [])
                                                       for key, group in group_masks.items()},
                                            "note": "E/pi before receiver material; reuse normal-image pixel groups. Not directly comparable in magnitude to normal HDR deltas."}
        result["regions"][region_name] = region
    return result


def number(value: float | None, percent: bool = False) -> str:
    return "undefined" if value is None else f"{value * 100:.2f}%" if percent else f"{value:.6g}"


def markdown(summary: dict) -> str:
    lines = ["# SSGI / VXGI luminance measurements", "",
             "These are paired linear-HDR measurements, not judgments of physical accuracy. No arbitrary PASS threshold is applied.", "",
             "B = normal rendering with GI disabled; S = normal rendering with SSGI only; F = normal rendering with SSGI plus VXGI fallback. S-B measures SSGI's displayed effect; F-S measures the extra fallback plus the resulting denoiser response. Neither is an isolated pre-filter light transport integral.", "",
             "Y = 0.2126 R + 0.7152 G + 0.0722 B, before preview exposure/tone mapping. Values are in the renderer's arbitrary linear-HDR scale, not cd/m². Means and percentiles describe pixel brightness, not surface-area-weighted or physical energy.", "",
             "The foreground mask uses raster depth < 1. The above-ground mask reconstructs world position at pixel centers and selects world Y > 0.03; in this car-plus-flat-ground scene it approximates vehicle geometry, but is not an object-ID mask. Other elevated geometry would also enter it.", "",
             "Dark/bright groups are fixed 20% pixel rankings from B or S and reused unchanged across all modes. Baseline B groups compare both effects without selecting on either effect. S-dark groups answer what VXGI changes where SSGI leaves the image darkest. Stable pixel-index tie breaking is disclosed in JSON; many equal-valued pixels can make a 20% selection spatially arbitrary.", "",
             "Gain share is the fraction of a region's positive F-S increment occurring inside a group; compare it with the group's pixel fraction. Concentration = gain share / pixel fraction. Signed mean deltas and negative changes are also retained; positive-only shares must not conceal reductions. Relative changes use ratios of region means, never means of near-zero per-pixel ratios. Undefined denominators are null.", "",
             "Low-luminance fractions use fixed absolute thresholds and 1%, 5%, 10% of B's median in the same region, applied identically to every mode. These thresholds are diagnostics, not calibrated visibility limits. Pure-GI E/pi is auxiliary and is before receiving-material modulation, so its magnitude is not interchangeable with normal HDR differences.", ""]
    if "fresh_cache_generation_evidence" in summary:
        cache = summary["fresh_cache_generation_evidence"]
        validation = cache["raw_validation"]
        lines += [f"Fresh cache evidence: `{cache['report']}`. RAW SHA256 `{cache['raw_snapshot_sha256']}`; valid probes {validation['valid']:,}, disabled probes {validation['disabled']:,}, unclassified {validation['unclassified']}; valid minimum history updates {validation['valid_minimum_updates']}. The generated cache and every normal/pure-GI report have matching scene/configuration/model/HDR input hashes.", ""]
    lines += ["These fixed-camera samples establish the behavior in the measured scene and sampling sequence. They do not establish how either method behaves across all scenes, materials, viewing directions, or temporal noise sequences.", ""]
    for dataset in summary["datasets"]:
        lines += [f"## {dataset['name']}", ""]
        for name, region in dataset["regions"].items():
            if not region["pixels"]:
                lines += [f"{name}: empty selection.", ""]
                continue
            overall = region["all"]
            modes = overall["modes"]
            lines += [f"### {name} ({region['pixels']:,} pixels)", "",
                      "| Measurement | B: base | S: SSGI | F: full GI |",
                      "|---|---:|---:|---:|",
                      "| Mean luminance | " + " | ".join(number(modes[str(mode)]["luminance"]["mean"]) for mode in (-1, 0, 2)) + " |",
                      "| P10 luminance | " + " | ".join(number(modes[str(mode)]["luminance"]["p10"]) for mode in (-1, 0, 2)) + " |", "",
                      f"Mean S-B = {number(overall['ssgi_effect_S_minus_B']['mean_delta'])}; mean F-S = {number(overall['vxgi_effect_F_minus_S']['mean_delta'])}; VXGI/SSGI mean increment ratio = {number(overall['ratio_of_vxgi_to_ssgi_mean_delta'])}.", "",
                      "| Fixed pixel group | Mean S-B | Mean F-S | F-S positive gain share | Group pixel share | F vs S mean change |",
                      "|---|---:|---:|---:|---:|---:|"]
            for group_name, group in region["groups"].items():
                effect = group["vxgi_effect_F_minus_S"]
                lines.append(f"| {group_name} | {number(group['ssgi_effect_S_minus_B']['mean_delta'])} | {number(effect['mean_delta'])} | {number(effect['scope_positive_gain_share'], True)} | {number(group['selection']['scope_fraction'], True)} | {number(effect['relative_mean_change'], True)} |")
            lines += ["", "Interpret the whole-region mean and its local groups together: a small whole-image change can coexist with a large dark-region change. A large percentage increase from a tiny baseline does not by itself imply a large absolute contribution. Similar S and F means do not establish similar SSGI and VXGI contributions; compare S-B and F-S instead.", ""]
            lines += ["| Fixed low-luminance threshold (Y <= threshold) | B fraction | S fraction | F fraction |",
                      "|---|---:|---:|---:|"]
            for index, threshold in enumerate(modes["-1"]["low_luminance_fractions"]):
                lines.append(f"| {number(threshold['threshold'])} | " + " | ".join(
                    number(modes[str(mode)]["low_luminance_fractions"][index]["fraction"], True)
                    for mode in (-1, 0, 2)) + " |")
            lines.append("")
            if "pure_gi_auxiliary" in region:
                pure = region["pure_gi_auxiliary"]["all"]
                lines += [f"Auxiliary pure-GI mean E/pi: S={number(pure['modes']['0']['luminance']['mean'])}, F={number(pure['modes']['2']['luminance']['mean'])}; signed F-S={number(pure['vxgi_effect_F_minus_S']['mean_delta'])}.", ""]
        lines += [f"Evidence: `{dataset['normal']['report']}`; report SHA256 `{dataset['normal']['report_sha256']}`. Per-mode image/depth hashes and fixed-pixel statistics are in summary.json.", ""]
    return "\n".join(lines)


def named_reports(arguments: list[str]) -> dict[str, Path]:
    result = {}
    for argument in arguments:
        name, separator, path = argument.partition("=")
        if not separator or not name or not path or name in result:
            raise ValueError(f"Expected a unique NAME=report.json: {argument}")
        result[name] = resolve(path)
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--normal", action="append", required=True, metavar="NAME=REPORT")
    parser.add_argument("--gi", action="append", default=[], metavar="NAME=REPORT")
    parser.add_argument("--cache", type=Path, help="Optional freshly generated complete probe-cache report")
    parser.add_argument("--output", type=Path, default=ROOT / "tmp/vxgi-luminance")
    args = parser.parse_args()
    normal, gi = named_reports(args.normal), named_reports(args.gi)
    if set(gi) - set(normal):
        raise ValueError("Every --gi NAME must also have a --normal NAME")
    datasets = [analyze(name, path, gi.get(name)) for name, path in normal.items()]
    summary = {"schema_version": 1, "scope": "CPU-only matched linear-HDR analysis; no production changes or GPU work.",
               "luminance_weights_linear_rgb": LUMA.tolist(),
               "geometry_mask_contract": "DirectX [0,1] raster depth, pixel-center NDC, row-vector times inverse_view_projection_row_major, world Y > 0.03. Approximate elevated geometry, not object IDs.",
               "relative_denominator_zero_epsilon": EPSILON,
               "conclusions_are_scene_and_camera_dependent": True,
               "datasets": datasets}
    if args.cache:
        cache, cache_evidence = load_report(resolve(args.cache), False)
        validation = cache["raw_validation"]
        target = cache.get("probe_cache_policy", {}).get("target_history_updates")
        if validation.get("imported") is not False or validation.get("cache_classification_complete") is not True:
            raise ValueError("--cache must identify a fresh, fully classified production capture, not an imported cache")
        if validation.get("unclassified") != 0 or target is None or validation.get("valid_minimum_updates", -1) < target:
            raise ValueError("--cache has not reached the configured finite update target")
        for dataset, normal_path in zip(datasets, normal.values()):
            current = json.loads(normal_path.read_text(encoding="utf-8-sig"))
            if cache_evidence["raw_snapshot_sha256"] != dataset["normal"]["raw_snapshot_sha256"]:
                raise ValueError("A normal capture does not use the supplied fresh --cache RAW")
            if cache.get("pipeline") != current.get("pipeline") or normalized_scene(cache.get("scene")) != normalized_scene(current.get("scene")):
                raise ValueError("Fresh --cache and normal capture scene/pipeline differ")
            if cache_evidence["recorded_scene_input_hashes"] != dataset["normal"]["recorded_scene_input_hashes"]:
                raise ValueError("Fresh --cache and normal capture recorded scene input hashes differ")
        summary["fresh_cache_generation_evidence"] = cache_evidence
    output = resolve(args.output)
    output.mkdir(parents=True, exist_ok=True)
    (output / "summary.json").write_text(json.dumps(summary, indent=2, ensure_ascii=False, allow_nan=False) + "\n", encoding="utf-8")
    (output / "report.md").write_text(markdown(summary) + "\n", encoding="utf-8")
    print(f"Analyzed {len(summary['datasets'])} matched camera datasets: {output / 'summary.json'}")


if __name__ == "__main__":
    main()
