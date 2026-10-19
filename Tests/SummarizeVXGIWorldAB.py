"""Compare screen-only and cached-world fallback blocks from one production PSO."""

import argparse
import hashlib
import json
import statistics
from pathlib import Path


def quantile(values, q):
    values = sorted(values)
    x = (len(values) - 1) * q
    a = int(x)
    b = min(a + 1, len(values) - 1)
    return values[a] + (values[b] - values[a]) * (x - a)


def stats(values):
    return {"count": len(values), "median_ms": statistics.median(values),
            "p95_ms": quantile(values, .95), "minimum_ms": min(values), "maximum_ms": max(values)}


def value(frame, metric):
    if metric == "trace_gpu_ms":
        return next(item["gpu_ms"] for item in frame["passes"] if item["name"] == "SSGI.Trace")
    return frame[metric]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("--output", type=Path, default=Path("tmp/VXGI-world-ab-summary.json"))
    args = parser.parse_args()
    report = json.loads(args.report.read_text(encoding="utf-8-sig"))
    assert report["completed"] and report["compare_world_modes"]
    assert report["native_debug"] and not report["exact_fallback"]
    assert report["present_calls"] == 0 and not report["window_visible"]
    assert report["d3d12_errors"] == 0 and report["d3d12_unexpected_warnings"] == 0
    assert report["raw_gpu_bytes_unchanged_after_all_captures"]
    raw = Path(report["raw_snapshot"])
    assert hashlib.sha256(raw.read_bytes()).hexdigest().upper() == report["raw_snapshot_sha256"]
    assert report["raw_validation"]["unclassified"] == 0
    assert report["raw_validation"]["valid_minimum_updates"] >= report["probe_cache_policy"]["target_history_updates"]
    order = report["ab_order"]
    assert order == [0, 2, 2, 0] * report["ab_rounds"]
    warmup = report["ab_warmup_frames"]
    count = report["ab_retained_samples_per_block"]
    blocks = []
    for index, mode in enumerate(order):
        frames = [frame for frame in report["frame_gpu_samples"]
                  if frame["phase"] == "world_ab" and frame["repeat"] == index and frame["sample"] >= warmup]
        assert len(frames) == count and {frame["mode"] for frame in frames} == {mode}
        assert [frame["sample"] for frame in frames] == list(range(warmup, warmup + count))
        blocks.append(frames)
    metrics = ["trace_gpu_ms", "ssgi_gpu_ms", "renderer_gpu_ms"]
    if all("renderer_record_cpu_ms" in frame for frames in blocks for frame in frames):
        metrics.append("renderer_record_cpu_ms")
    result = {"source_report": str(args.report), "native_build_policy": report["native_build_policy"],
              "raw_snapshot_sha256": report["raw_snapshot_sha256"], "pipeline": report["pipeline"],
              "internal_resolution": report["internal_resolution"], "trace_resolution": report["ssgi_trace_resolution"],
              "ab_order": order, "warmup_frames_per_block": warmup, "retained_frames_per_block": count,
              "scope": "Same production Trace PSO, same frozen fully-filled probe RAW, scene, camera and STBN frame sequence. Mode0 stops after screen miss; Mode2 invokes world threshold/probe fallback. Probe producer, initialization, DLSS, UI and Present are excluded. These are GPU marginal costs, not main-app FPS.",
              "diagnostics": {key: item for key, item in report.items() if key.startswith("d3d12_")},
              "metrics": {}, "blocks": [], "paired_blocks": []}
    for metric in metrics:
        a = [value(frame, metric) for index, frames in enumerate(blocks) if order[index] == 0 for frame in frames]
        b = [value(frame, metric) for index, frames in enumerate(blocks) if order[index] == 2 for frame in frames]
        delta = statistics.median(b) - statistics.median(a)
        result["metrics"][metric] = {"screen_only": stats(a), "world_fallback": stats(b),
                                     "median_delta_ms": delta, "median_delta_percent": delta / statistics.median(a) * 100}
    for index, frames in enumerate(blocks):
        result["blocks"].append({"index": index, "mode": order[index],
                                 "metrics": {metric: stats([value(frame, metric) for frame in frames]) for metric in metrics}})
    for start in range(0, len(order), 4):
        for ai, bi in [(start, start + 1), (start + 3, start + 2)]:
            assert order[ai] == 0 and order[bi] == 2
            result["paired_blocks"].append({"screen_block": ai, "world_block": bi,
                "metrics": {metric: stats([value(b, metric) - value(a, metric)
                                             for a, b in zip(blocks[ai], blocks[bi])]) for metric in metrics}})
    captures = report["captures"]
    depths = {hashlib.sha256(Path(capture["raster_depth"]).read_bytes()).hexdigest() for capture in captures}
    assert len(depths) == 1, "A/B depth changed"
    result["all_captures_same_depth"] = True
    args.output.write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    for metric, item in result["metrics"].items():
        a, b = item["screen_only"]["median_ms"], item["world_fallback"]["median_ms"]
        print(f"{metric}: {a:.6f} -> {b:.6f} ms; delta {item['median_delta_ms']:+.6f} ms ({item['median_delta_percent']:+.2f}%)")
    for pair in result["paired_blocks"]:
        print(f"Paired blocks {pair['screen_block']}/{pair['world_block']}: "
              f"Trace delta median {pair['metrics']['trace_gpu_ms']['median_ms']:+.6f} ms")
    print(f"Saved {args.output}")


if __name__ == "__main__":
    main()
