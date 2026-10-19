"""Summarize production lazy-cache timestamps without treating fence waits as FPS."""

import argparse
import json
import math
import statistics
from collections import defaultdict
from pathlib import Path


def quantile(values, fraction):
    values = sorted(values)
    position = (len(values) - 1) * fraction
    lo, hi = math.floor(position), math.ceil(position)
    return values[lo] * (hi - position) + values[hi] * (position - lo) if lo != hi else values[lo]


def summarize(values):
    values = [float(value) for value in values if isinstance(value, (int, float)) and math.isfinite(value)]
    if not values:
        return None
    return {"count": len(values), "median_ms": statistics.median(values),
            "p95_ms": quantile(values, .95), "max_ms": max(values)}


def metrics(frames, suffixes=("_cpu_ms", "_gpu_ms", "_wall_ms")):
    keys = sorted({key for frame in frames for key in frame if key.endswith(suffixes)})
    return {key: summarize([frame.get(key) for frame in frames
                            if key != "frame_cpu_wall_ms" or not frame.get("image_readback_and_scan")])
            for key in keys}


def pass_ranking(frames):
    samples = defaultdict(list)
    groups = defaultdict(list)
    for frame in frames:
        totals = defaultdict(float)
        for item in frame.get("passes", []):
            name = item["name"]
            value = item["gpu_ms"]
            samples[name].append(value)
            group = "SSGI" if name.startswith("SSGI.") else (
                "HBAO" if name.startswith("HBAO") or name == "ScalarTemporalAO" else name)
            totals[group] += value
        for group, value in totals.items():
            groups[group].append(value)
    ranked = [{"name": name, **summarize(values)} for name, values in samples.items()]
    grouped = [{"name": name, **summarize(values)} for name, values in groups.items()]
    return sorted(ranked, key=lambda item: -item["median_ms"]), sorted(grouped, key=lambda item: -item["median_ms"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("--tail", type=Path)
    parser.add_argument("--discard-steady", type=int, default=24)
    parser.add_argument("--output", type=Path, default=Path("tmp/VXGI-Lazy-performance-summary.json"))
    args = parser.parse_args()
    report = json.loads(args.report.read_text(encoding="utf-8-sig"))
    assert report.get("completed") and report.get("lazy_probes") and report.get("lazy_settle")
    assert report.get("native_debug")
    assert report.get("d3d12_errors", 0) == 0 and report.get("d3d12_unexpected_warnings", 0) == 0
    assert report.get("d3d12_warnings_errors", 0) == report.get("d3d12_known_performance_warnings", 0)
    assert not report.get("window_visible") and report.get("present_calls") == 0
    policy = report["probe_cache_policy"]
    jobs = (report["scene"]["probes"] + policy["probes_per_frame"] - 1) // policy["probes_per_frame"]
    cpu_frames = report["cold_start_frames"]
    gpu_frames = report["frame_gpu_samples"]
    cold = [frame for frame in cpu_frames if frame.get("phase") == "cold_start"]
    steady = [frame for frame in cpu_frames if frame.get("phase") == "lazy_steady"]
    assert len(steady) > args.discard_steady
    retained = steady[args.discard_steady:]
    assert all(frame["update_count"] == 0 for frame in retained), "steady phase scheduled probe work"
    assert all(frame.get("reset_mode", 0) == 0 for frame in retained), "steady phase reset probe cache"
    phases = {"first_frames": cold[:3],
              "classification": [frame for frame in cold if 3 <= frame["sample"] < jobs and not frame.get("image_readback_and_scan")],
              "capture_after_classification": [frame for frame in cold if frame["sample"] >= jobs and not frame.get("image_readback_and_scan")],
              "clean_steady": retained}
    summary = {"source_report": str(args.report), "native_debug": True,
               "internal_resolution": report["internal_resolution"],
               "ssgi_trace_resolution": report["ssgi_trace_resolution"],
               "probe_cache_policy": policy, "classification_budget_frames": jobs,
               "initial_scene_prepare_cpu": report["initial_scene_prepare_cpu"],
               "diagnostics": {key: value for key, value in report.items() if key.startswith("d3d12_")},
               "scope": "Current production event-driven probe cache and real configured scene, hidden NativeDebug raster/SSGI. GPU timestamps are per stage; CPU command recording is separate. Fence-waited diagnostics do not measure main-app FPS. DLSS/postprocess tail, when present, is a separate frozen-input measurement.",
               "cpu_wall_filter": "Image readback/PNG scan frames are excluded only from frame_cpu_wall_ms; pass GPU and CPU command-recording intervals remain valid.",
               "phases": {}}
    for name, frames in phases.items():
        samples = {frame["sample"] for frame in frames}
        phase = "lazy_steady" if name == "clean_steady" else "cold_start"
        gpus = [frame for frame in gpu_frames if frame.get("phase") == phase and frame["sample"] in samples]
        ranking, groups = pass_ranking(gpus)
        summary["phases"][name] = {"retained_frames": len(frames), "metrics": metrics(frames),
                                   "render_pass_ranking": ranking, "render_stage_groups": groups}
    summary["lazy_metadata"] = {key: value for key, value in report.items() if key.startswith("lazy_")}
    summary["raw_validation"] = report["raw_validation"]
    if args.tail:
        tail = json.loads(args.tail.read_text(encoding="utf-8-sig"))
        assert tail.get("completed", tail.get("passed", False))
        assert tail.get("d3d12_warnings_errors") == 0 and not tail.get("window_visible") and tail.get("present_calls") == 0
        ranking, groups = pass_ranking(tail["frames"])
        summary["separate_render_tail"] = {"source_report": str(args.tail), "scope": tail["scope"],
                                            "input_size": tail["input_size"], "output_size": tail["output_size"],
                                            "retained_frames": len(tail["frames"]), "metrics": metrics(tail["frames"]),
                                            "render_pass_ranking": ranking}
    args.output.write_text(json.dumps(summary, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    for name, phase in summary["phases"].items():
        print(f"{name}: {phase['retained_frames']} frames")
        for key in ("probe_record_outer_gpu_ms", "filter_record_outer_gpu_ms", "renderer_gpu_ms", "ssgi_gpu_ms",
                    "state_prepare_cpu_ms", "cache_prepare_cpu_ms", "completed_probe_poll_cpu_ms",
                    "completed_dependency_accept_cpu_ms", "probe_record_cpu_ms", "filter_record_and_delayed_poll_cpu_ms"):
            value = phase["metrics"].get(key)
            if value:
                print(f"  {key}: median={value['median_ms']:.6f}, p95={value['p95_ms']:.6f}, max={value['max_ms']:.6f}")
        for item in phase["render_pass_ranking"][:5]:
            print(f"  {item['name']}: {item['median_ms']:.6f} ms")
    print(f"Saved {args.output}")


if __name__ == "__main__":
    main()
