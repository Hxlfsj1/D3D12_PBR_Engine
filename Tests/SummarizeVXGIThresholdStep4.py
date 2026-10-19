"""Consolidate the frozen-input step-4 ABBA receipts and repeat controls."""
import json
from pathlib import Path

import numpy as np
import SummarizeVXGIThresholdAcceptance as shared

ROOT = Path(__file__).resolve().parents[1]
TMP = ROOT / "tmp"


def report(label, resolution):
    path = TMP / f"VXGIStage75-{label}-{resolution}-report.json"
    value = shared.read_report(path)
    if not value["completed"] or value["d3d12_warnings_errors"] or value["present_calls"] or value["window_visible"]:
        raise ValueError(f"Renderer receipt did not pass: {path}")
    return path, value


def main():
    paths, reports = [], []
    order = (("threshold", "a"), ("exact", "a"), ("exact", "b"), ("threshold", "b"))
    for backend, batch in order:
        path, value = report(f"{backend}-step4-debug-perf-{batch}", "1707x1067")
        if value["exact_fallback"] != (backend == "exact") or not value["native_debug"]:
            raise ValueError("Unexpected fallback or native CPU configuration")
        paths.append(path)
        reports.append(value)
    for path, value in zip(paths[1:], reports[1:]):
        for key in ("pipeline", "scene", "raw_snapshot_sha256", "internal_resolution", "gi_only",
                    "samples_per_sequence", "repeat_sequences", "native_configuration", "debug_shader_policy"):
            if value[key] != reports[0][key]:
                raise ValueError(f"ABBA inputs differ: {key}")
        if shared.source_manifest(path) != shared.source_manifest(paths[0]):
            raise ValueError("ABBA source input manifests differ")
    manifests = []
    for path in paths:
        manifest = path.with_name(path.name.replace("-report.json", "-input-hashes.json"))
        entries = json.loads(manifest.read_text(encoding="utf-8-sig"))
        manifests.append({entry["path"].lower(): entry["sha256"].upper() for entry in entries})
    if any(manifest != manifests[0] for manifest in manifests[1:]):
        raise ValueError("ABBA complete code/input manifests differ")
    pooled, batches = {}, []
    for backend in ("exact", "threshold"):
        chosen = [r for r in reports if r["exact_fallback"] == (backend == "exact")]
        if chosen[0]["native_build_policy"]["executable_sha256"] != chosen[1]["native_build_policy"]["executable_sha256"]:
            raise ValueError("A backend changed its executable between timing batches")
        combined = dict(chosen[0])
        combined["frame_gpu_samples"] = [frame for r in chosen for frame in r["frame_gpu_samples"]]
        pooled[backend] = shared.timing_summary(combined, discard=3)
        for index, value in enumerate(chosen):
            batches.append({"backend": backend, "batch": index, "timings": shared.timing_summary(value, 3)})
    before, after = pooled["exact"][0], pooled["threshold"][0]
    comparison = {}
    for metric in ("trace_gpu_ms", "ssgi_gpu_ms", "renderer_gpu_ms"):
        old, new = before[metric]["median"], after[metric]["median"]
        comparison[metric] = {"reference_median": old, "candidate_median": new,
                              "speedup_ratio": old / new, "median_reduction_fraction": 1 - new / old}

    controls = []
    for backend in ("exact", "threshold"):
        original_path, original = report(f"{backend}-step4-quality", "640x400")
        repeat_path, repeat = report(f"{backend}-step4-screen-repeat", "640x400")
        for key in ("pipeline", "scene", "raw_snapshot_sha256", "native_build_policy", "samples_per_sequence"):
            if original[key] != repeat[key]:
                raise ValueError(f"Screen repeat input differs: {key}")
        a = next(c for c in original["captures"] if c["camera"] == 2 and c["mode"] == 0)
        b = repeat["captures"][0]
        if (b["camera"], b["mode"], b["sample"]) != (2, 0, a["sample"]):
            raise ValueError("Screen repeat used a different camera, mode or frame")
        first, depth_a = shared.load_capture(a, [640, 400])
        second, depth_b = shared.load_capture(b, [640, 400])
        identical = bool(np.array_equal(first.view(np.uint32), second.view(np.uint32)))
        depth_identical = bool(np.array_equal(depth_a.view(np.uint32), depth_b.view(np.uint32)))
        controls.append({"backend": backend, "camera": 2, "mode": 0,
                         "rgba_bit_identical": identical, "depth_bit_identical": depth_identical,
                         "changed_pixels": int(np.any(first != second, axis=2).sum()),
                         "maximum_rgba_difference": float(np.max(np.abs(first - second))),
                         "original_report": shared.relative(original_path), "repeat_report": shared.relative(repeat_path)})
        if not identical or not depth_identical:
            raise ValueError(f"Same-backend screen repeat was not deterministic: {backend}")

    geometry = {}
    for mode in ("Debug", "O2"):
        path = TMP / f"VXGIMeshSDFThresholdAcceptance-GPU-{mode}-report.json"
        value = json.loads(path.read_text(encoding="utf-8-sig"))
        if not value["implementation_invariants_passed"] or value["gpu_warnings"]:
            raise ValueError("Actual-scene threshold implementation checks failed")
        geometry[mode] = {"report": shared.relative(path), "sha256": shared.sha256(path),
                          "outward_instance_balanced": value["quality"]["instance_balanced_outward"]}
    from PIL import Image, ImageDraw
    _, old_quality = report("exact-step4-quality", "640x400")
    _, new_quality = report("threshold-step4-quality", "640x400")
    old_capture = next(c for c in old_quality["captures"] if c["camera"] == 0 and c["mode"] == 2)
    new_capture = next(c for c in new_quality["captures"] if c["camera"] == 0 and c["mode"] == 2)
    old_image, _ = shared.load_capture(old_capture, [640, 400])
    new_image, _ = shared.load_capture(new_capture, [640, 400])
    canvas = Image.new("RGB", (1280, 428), "#16181b")
    draw = ImageDraw.Draw(canvas)
    draw.text((8, 8), "Exact reference | pure GI | exposure x4", fill="white")
    draw.text((648, 8), "Threshold | pure GI | exposure x4 | same RAW", fill="white")
    canvas.paste(shared.preview(old_image[:, :, :3], 4), (0, 28))
    canvas.paste(shared.preview(new_image[:, :, :3], 4), (640, 28))
    image_path = TMP / "VXGI-Threshold-step4-camera0.png"
    canvas.save(image_path)
    output = {"scope": "Native Debug CPU/CRT and production Debug shader policy; frozen RAW; ABBA order; "
                       "2x12 frames per batch, discard first 3 of each sequence. GPU timings exclude probe updates, "
                       "readbacks, UI, DLSS, postprocess and Present; no main-program FPS inference.",
              "order": [f"{backend}-{batch}" for backend, batch in order],
              "complete_code_and_input_manifests_equal": True,
              "reports": [{"path": shared.relative(p), "sha256": shared.sha256(p)} for p in paths],
              "raw_sha256": reports[0]["raw_snapshot_sha256"],
              "pooled_timings": pooled, "batch_timings": batches, "median_comparison": comparison,
              "same_backend_screen_repeat": controls, "actual_scene_geometry": geometry,
              "pure_gi_comparison_image": shared.relative(image_path),
              "quality_status": "Requires review: current threshold accepts nearly every tested outward car ray at its origin; "
                                "successful implementation checks and lower execution time do not certify image quality."}
    path = TMP / "VXGI-Threshold-step4-acceptance.json"
    path.write_text(json.dumps(output, indent=2, ensure_ascii=False, allow_nan=False) + "\n", encoding="utf-8")
    print(json.dumps(comparison, indent=2))
    print(json.dumps(controls, indent=2))
    print(path)


if __name__ == "__main__":
    main()
