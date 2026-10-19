"""Summarize production-frame diagnostics without treating CPU/GPU spans as additive.

Example:
  python Tests/AnalyzeStartupFrameDiagnostics.py tmp/control-frames.json \
      tmp/toggle-frames.json --output-prefix tmp/startup-comparison
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import statistics
from typing import Any


def number(value: Any) -> float | None:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    value = float(value)
    return value if math.isfinite(value) else None


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lower = math.floor(position)
    upper = math.ceil(position)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def distribution(values: list[float]) -> dict[str, float | int]:
    if not values:
        return {"count": 0}
    return {
        "count": len(values),
        "mean": statistics.fmean(values),
        "p50": percentile(values, 0.5),
        "p90": percentile(values, 0.9),
        "p99": percentile(values, 0.99),
        "min": min(values),
        "max": max(values),
    }


def time_s(row: dict[str, Any]) -> float:
    return row["_time_s"]


def summarize(rows: list[dict[str, Any]]) -> dict[str, Any]:
    wall = [value for row in rows if (value := number(row.get("wall_ms"))) is not None and value > 0]
    result: dict[str, Any] = {
        "frames": len(rows),
        "first_frame": rows[0].get("frame") if rows else None,
        "last_frame": rows[-1].get("frame") if rows else None,
        "first_time_s": time_s(rows[0]) if rows else None,
        "last_time_s": time_s(rows[-1]) if rows else None,
        "modes": sorted({str(row.get("mode")) for row in rows}),
        "wall_ms": distribution(wall),
        # Throughput is reciprocal mean interval, not median instantaneous FPS.
        "fps_from_intervals": 1000.0 * len(wall) / sum(wall) if wall else None,
        "gpu_pending_frames": sum(bool(row.get("metadata", {}).get("diagnostics_gpu_pending")) for row in rows),
        "gpu_not_submitted_frames": sum(bool(row.get("metadata", {}).get("diagnostics_gpu_not_submitted")) for row in rows),
    }
    for domain in ("cpu_ms", "gpu_ms"):
        values_by_name: dict[str, list[float]] = {}
        for row in rows:
            metadata = row.get("metadata", {})
            if domain == "gpu_ms" and (metadata.get("diagnostics_gpu_pending") or metadata.get("diagnostics_gpu_not_submitted")):
                continue
            for name, raw in row.get(domain, {}).items():
                value = number(raw)
                if value is not None and value >= 0:
                    values_by_name.setdefault(name, []).append(value)
        result[domain] = {name: distribution(values) for name, values in sorted(values_by_name.items())}
    return result


def differences(before: dict[str, Any], after: dict[str, Any]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    before_wall = before.get("wall_ms", {}).get("p50")
    after_wall = after.get("wall_ms", {}).get("p50")
    if before_wall is not None and after_wall is not None:
        result["wall_p50_delta_ms"] = after_wall - before_wall
    for domain in ("cpu_ms", "gpu_ms"):
        changes = []
        for name in before[domain].keys() & after[domain].keys():
            first = before[domain][name]["p50"]
            last = after[domain][name]["p50"]
            changes.append({
                "name": name, "before_p50_ms": first, "after_p50_ms": last,
                "delta_ms": last - first,
                "delta_percent": (last / first - 1.0) * 100.0 if first else None,
            })
        result[domain] = sorted(changes, key=lambda change: abs(change["delta_ms"]), reverse=True)
    return result


def load_run(path: Path, settle_seconds: float, window_seconds: float) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    content = path.read_bytes()
    document = json.loads(content.decode("utf-8-sig"))
    raw_rows = document if isinstance(document, list) else document.get("frames", [])
    if not raw_rows:
        raise ValueError(f"{path}: no frame rows")
    rows = []
    cumulative_seconds = 0.0
    inferred_times = 0
    for raw in raw_rows:
        row = dict(raw)
        metadata = row.get("metadata", {})
        # The application's T handler runs inside the UI, after BeginFrame.
        # Its final metadata describes the commands actually submitted this frame.
        if metadata.get("actual_mode") is not None:
            row["mode"] = metadata["actual_mode"]
        elapsed = number(metadata.get("time_s", row.get("time_s")))
        if elapsed is None:
            elapsed = cumulative_seconds
            inferred_times += 1
        row["_time_s"] = elapsed
        cumulative_seconds = elapsed + max(number(row.get("wall_ms")) or 0.0, 0.0) / 1000.0
        rows.append(row)
    rows.sort(key=lambda row: (time_s(row), row.get("frame", 0)))
    phases: list[dict[str, Any]] = []
    phase_rows: list[list[dict[str, Any]]] = []
    previous_key = None
    for row in rows:
        stage = row.get("metadata", {}).get("stage", row.get("stage", ""))
        key = (str(stage), row.get("mode"))
        if key != previous_key:
            phase_rows.append([])
            phases.append({"index": len(phases), "stage": str(stage), "mode": row.get("mode")})
            previous_key = key
        phase_rows[-1].append(row)
    for phase, samples in zip(phases, phase_rows):
        phase["all"] = summarize(samples)
        retained = [row for row in samples if time_s(row) >= time_s(samples[0]) + settle_seconds]
        phase["retained"] = summarize(retained)
        phase["retained_by_frame_parity"] = {
            str(parity): summarize([row for row in retained if isinstance(row.get("frame"), int)
                and row["frame"] % 2 == parity]) for parity in (0, 1)
        }
        phase["settle_seconds"] = settle_seconds
    windows = []
    buckets: dict[int, list[dict[str, Any]]] = {}
    for row in rows:
        buckets.setdefault(math.floor(time_s(row) / window_seconds), []).append(row)
    for bucket, samples in sorted(buckets.items()):
        windows.append({"start_s": bucket * window_seconds, "end_s": (bucket + 1) * window_seconds, **summarize(samples)})
    comparisons = []
    for after in range(1, len(phases)):
        earlier = [index for index in range(after) if phases[index]["mode"] == phases[after]["mode"]]
        if earlier:
            before = earlier[0]
            comparisons.append({"before_phase": before, "after_phase": after,
                **differences(phases[before]["retained"], phases[after]["retained"])})
    metadata_changes = {}
    for key in sorted({key for row in rows for key in row.get("metadata", {})} - {"time_s", "stage"}):
        changes = []
        previous = object()
        change_count = 0
        for row in rows:
            value = row.get("metadata", {}).get(key)
            if value != previous:
                change_count += 1
                if len(changes) < 16:
                    changes.append({"frame": row.get("frame"), "time_s": time_s(row), "value": value})
                previous = value
        metadata_changes[key] = {"first_values_or_changes": changes,
            "change_count": change_count, "truncated": change_count > len(changes)}
    return {
        "path": str(path), "sha256": hashlib.sha256(content).hexdigest(),
        "label": path.stem, "times_inferred_from_wall_intervals": inferred_times,
        "run_metadata": {key: value for key, value in document.items() if key not in {"frames", "statistics"}}
            if isinstance(document, dict) else {},
        "statistics": document.get("statistics", {}) if isinstance(document, dict) else {},
        "all": summarize(rows), "phases": phases, "windows": windows,
        "same_mode_phase_comparisons": comparisons, "metadata_changes": metadata_changes,
    }, rows


def matched_elapsed(first: dict[str, Any], first_rows: list[dict[str, Any]],
                    second: dict[str, Any], second_rows: list[dict[str, Any]],
                    window_seconds: float) -> dict[str, Any]:
    first_buckets: dict[int, list[dict[str, Any]]] = {}
    second_buckets: dict[int, list[dict[str, Any]]] = {}
    for target, rows in ((first_buckets, first_rows), (second_buckets, second_rows)):
        for row in rows:
            target.setdefault(math.floor(time_s(row) / window_seconds), []).append(row)
    windows = []
    for bucket in sorted(first_buckets.keys() & second_buckets.keys()):
        a = summarize(first_buckets[bucket])
        b = summarize(second_buckets[bucket])
        windows.append({"start_s": bucket * window_seconds, "end_s": (bucket + 1) * window_seconds,
            "same_modes": a["modes"] == b["modes"] and len(a["modes"]) == 1,
            "a": a, "b": b, "b_minus_a": differences(a, b)})
    # For a control/toggle pair, compare the control at the actual first/restored
    # elapsed ranges. This distinguishes a mode round trip from elapsed-time drift.
    controls = [(first, first_rows), (second, second_rows)]
    toggle = next(((run, rows) for run, rows in controls if len(run["all"]["modes"]) > 1
        and run["same_mode_phase_comparisons"]), None)
    control = next(((run, rows) for run, rows in controls if len(run["all"]["modes"]) == 1
        and toggle and run["phases"][0]["mode"] == toggle[0]["phases"][0]["mode"]), None)
    adjusted = []
    if control and toggle:
        control_run, control_rows = control
        toggle_run, toggle_rows = toggle
        for comparison in toggle_run["same_mode_phase_comparisons"]:
            matched_control = []
            matched_toggle = []
            ranges = []
            for index in (comparison["before_phase"], comparison["after_phase"]):
                phase = toggle_run["phases"][index]["retained"]
                start = phase["first_time_s"]
                end = phase["last_time_s"]
                if start is None or end is None:
                    break
                control_samples = [row for row in control_rows if start <= time_s(row) <= end]
                toggle_samples = [row for row in toggle_rows if start <= time_s(row) <= end]
                if not control_samples or not toggle_samples:
                    break
                matched_control.append(summarize(control_samples))
                matched_toggle.append(summarize(toggle_samples))
                ranges.append([start, end])
            if len(matched_control) == 2:
                baseline = differences(*matched_control)
                observed = differences(*matched_toggle)
                corrected = {}
                for domain in ("cpu_ms", "gpu_ms"):
                    baseline_by_name = {entry["name"]: entry for entry in baseline[domain]}
                    corrected[domain] = sorted([
                        {"name": entry["name"], "toggle_delta_ms": entry["delta_ms"],
                         "control_delta_ms": baseline_by_name[entry["name"]]["delta_ms"],
                         "adjusted_delta_ms": entry["delta_ms"] - baseline_by_name[entry["name"]]["delta_ms"]}
                        for entry in observed[domain] if entry["name"] in baseline_by_name
                    ], key=lambda entry: abs(entry["adjusted_delta_ms"]), reverse=True)
                if "wall_p50_delta_ms" in baseline and "wall_p50_delta_ms" in observed:
                    corrected["wall_p50_adjusted_delta_ms"] = observed["wall_p50_delta_ms"] - baseline["wall_p50_delta_ms"]
                adjusted.append({"control": control_run["label"], "toggle": toggle_run["label"],
                    "elapsed_ranges_s": ranges, "control_before_after": baseline,
                    "toggle_before_after": observed, "difference_in_differences": corrected})
    return {"a": first["label"], "b": second["label"], "windows": windows,
        "elapsed_adjusted_round_trips": adjusted}


def markdown(report: dict[str, Any]) -> str:
    lines = ["# Production startup frame diagnostics", "", report["timing_contract"], ""]
    for run in report["runs"]:
        lines += [f"## {run['label']}", "", "| Phase | Mode | Frames retained | Elapsed s | FPS from intervals | Wall p50 / p90 ms |",
            "| --- | ---: | ---: | --- | ---: | --- |"]
        for phase in run["phases"]:
            summary = phase["retained"]
            wall = summary["wall_ms"]
            if not summary["frames"]:
                lines.append(f"| {phase['index']}: {phase['stage']} | {phase['mode']} | 0 | — | — | — |")
                continue
            fps = summary["fps_from_intervals"]
            fps_text = f"{fps:.2f}" if fps is not None else "—"
            lines.append(f"| {phase['index']}: {phase['stage']} | {phase['mode']} | {summary['frames']} | "
                f"{summary['first_time_s']:.3f}–{summary['last_time_s']:.3f} | {fps_text} | "
                f"{wall.get('p50', 0):.4f} / {wall.get('p90', 0):.4f} |")
        for comparison in run["same_mode_phase_comparisons"]:
            lines += ["", f"Same-mode phase {comparison['before_phase']} → {comparison['after_phase']} (after minus before):"]
            for domain in ("cpu_ms", "gpu_ms"):
                for entry in comparison[domain][:5]:
                    lines.append(f"- {domain} {entry['name']}: {entry['before_p50_ms']:.4f} → "
                        f"{entry['after_p50_ms']:.4f} ms ({entry['delta_ms']:+.4f} ms).")
        for phase in run["phases"]:
            parity = phase["retained_by_frame_parity"]
            even = parity["0"]["gpu_ms"].get("SSGI.Trace", {})
            odd = parity["1"]["gpu_ms"].get("SSGI.Trace", {})
            if even.get("count") and odd.get("count") and abs(even["mean"] - odd["mean"]) > 0.05:
                lines += ["", f"Phase {phase['index']} Trace GPU mean by frame parity: "
                    f"even {even['mean']:.4f} ms ({even['count']} frames), "
                    f"odd {odd['mean']:.4f} ms ({odd['count']} frames)."]
        lines += ["", f"GPU pending: {run['all']['gpu_pending_frames']}; not submitted: {run['all']['gpu_not_submitted_frames']}.", ""]
    for pair in report["matched_elapsed_comparisons"]:
        lines += [f"## Matched elapsed: {pair['a']} / {pair['b']}", "",
            f"{len(pair['windows'])} shared elapsed windows; detailed per-window CPU/GPU and mode checks are in JSON.", ""]
        for adjusted in pair["elapsed_adjusted_round_trips"]:
            lines.append(f"Control-adjusted round trip for {adjusted['toggle']}:")
            for domain in ("cpu_ms", "gpu_ms"):
                for entry in adjusted["difference_in_differences"][domain][:5]:
                    lines.append(f"- {domain} {entry['name']}: toggle {entry['toggle_delta_ms']:+.4f}, "
                        f"control {entry['control_delta_ms']:+.4f}, adjusted {entry['adjusted_delta_ms']:+.4f} ms.")
            lines.append("")
    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", nargs="+", type=Path)
    parser.add_argument("--output-prefix", type=Path, default=Path("tmp/startup-frame-comparison"))
    parser.add_argument("--settle-seconds", type=float, default=0.25,
        help="Keep all samples, but exclude this interval at each phase start from retained comparisons.")
    parser.add_argument("--window-seconds", type=float, default=1.0)
    args = parser.parse_args()
    if not math.isfinite(args.settle_seconds) or args.settle_seconds < 0 or not math.isfinite(args.window_seconds) or args.window_seconds <= 0:
        parser.error("settle-seconds must be finite/nonnegative and window-seconds finite/positive")
    loaded = [load_run(path, args.settle_seconds, args.window_seconds) for path in args.inputs]
    report = {
        "schema_version": 1,
        "timing_contract": "CPU spans and GPU spans are separate and may overlap/nest; do not sum them. "
            "FPS is frame count divided by the sum of positive reported wall intervals. Hidden Present/occlusion "
            "and an attached debugger can change throughput; inspect run metadata before interpreting FPS. "
            "GPU-pending/not-submitted rows retain CPU evidence but are excluded from GPU summaries. "
            "Elapsed matching controls time drift across independent runs; it does not establish identical GPU clocks or system load.",
        "settle_seconds": args.settle_seconds, "window_seconds": args.window_seconds,
        "runs": [run for run, _ in loaded],
        "matched_elapsed_comparisons": [matched_elapsed(*loaded[a], *loaded[b], args.window_seconds)
            for a in range(len(loaded)) for b in range(a + 1, len(loaded))],
    }
    args.output_prefix.parent.mkdir(parents=True, exist_ok=True)
    json_path = Path(str(args.output_prefix) + ".json")
    markdown_path = Path(str(args.output_prefix) + ".md")
    json_path.write_text(json.dumps(report, indent=2, ensure_ascii=False, allow_nan=False) + "\n", encoding="utf-8")
    markdown_path.write_text(markdown(report), encoding="utf-8")
    print(json_path)
    print(markdown_path)


if __name__ == "__main__":
    main()
