"""Draw reproducible, CPU-only luminance comparison sheets from Stage75 captures.

Example:
  python Tests/RenderVXGILuminanceComparison.py --prefix lum025
  python Tests/RenderVXGILuminanceComparison.py \
    --normal exterior=tmp/normal-report.json --pure exterior=tmp/pure-report.json

Only numpy and Pillow are required. This script never creates a GPU device,
changes settings, installs packages, or reads the desktop/OS clipboard.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parents[1]
LUMA = np.array([0.2126, 0.7152, 0.0722], dtype=np.float64)
NORMAL_EXPOSURE = 1.0
PURE_EXPOSURE = 4.0


def resolve(path: str | Path) -> Path:
    value = Path(path)
    return value if value.is_absolute() else ROOT / value


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def display_path(path: Path) -> str:
    try:
        return path.resolve().relative_to(ROOT).as_posix()
    except ValueError:
        return str(path.resolve())


def report_input(value: str) -> tuple[str, Path]:
    name, separator, path = value.partition("=")
    if not separator or not name or not path:
        raise argparse.ArgumentTypeError("Expected NAME=path/to/report.json")
    if any(character not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_" for character in name):
        raise argparse.ArgumentTypeError("NAME must contain only letters, digits, hyphens or underscores")
    return name, resolve(path)


def read_report(path: Path, pure: bool) -> dict:
    report = json.loads(path.read_text(encoding="utf-8-sig"))
    if not report.get("completed") or report.get("gi_only") is not pure:
        raise ValueError(f"Incomplete report or incorrect gi_only: {path}")
    if report.get("window_visible", True) or report.get("present_calls", 1) != 0:
        raise ValueError(f"Expected hidden, no-Present capture: {path}")
    if report.get("d3d12_errors", 1) or report.get("d3d12_unexpected_warnings", 1):
        raise ValueError(f"Unexpected D3D12 diagnostic: {path}")
    if not report.get("raw_gpu_bytes_unchanged_after_all_captures"):
        raise ValueError(f"Frozen RAW was not verified: {path}")
    if not report.get("raw_validation", {}).get("quality_ready"):
        raise ValueError(f"Probe cache is not ready: {path}")
    if sha256(resolve(report["raw_snapshot"])) != report["raw_snapshot_sha256"].upper():
        raise ValueError(f"Probe snapshot hash does not match: {path}")
    return report


def read_capture(capture: dict, resolution: list[int]) -> tuple[np.ndarray, np.ndarray]:
    width, height = resolution
    rgba = np.fromfile(resolve(capture["linear_hdr"]), dtype="<f4")
    depth = np.fromfile(resolve(capture["raster_depth"]), dtype="<f4")
    if rgba.size != width * height * 4 or depth.size != width * height:
        raise ValueError(f"Wrong capture dimensions: {capture['linear_hdr']}")
    rgba, depth = rgba.reshape(height, width, 4), depth.reshape(height, width)
    if not np.isfinite(rgba).all() or not np.isfinite(depth).all():
        raise ValueError("Nonfinite linear HDR or raster depth")
    if np.any((depth < 0) | (depth > 1)):
        raise ValueError("Raster depth is outside [0, 1]")
    return rgba[..., :3].astype(np.float64), depth


def load_view(name: str, normal_path: Path, pure_path: Path) -> dict:
    normal, pure = read_report(normal_path, False), read_report(pure_path, True)
    for key in ("internal_resolution", "raw_snapshot_sha256", "camera_override",
                "samples_per_sequence", "repeat_sequences", "native_configuration",
                "debug_shader_policy", "downsample_factor", "exact_fallback"):
        if normal.get(key) != pure.get(key):
            raise ValueError(f"{name}: normal/pure contract differs: {key}")
    # Source model enumeration comes from an unordered container; compare its
    # identified instances, not incidental iteration order in independent runs.
    def scene_contract(report: dict) -> dict:
        scene = dict(report["scene"])
        scene["bounds"] = sorted(scene.get("bounds", []), key=lambda item: item["instance_id"])
        return scene
    if scene_contract(normal) != scene_contract(pure):
        raise ValueError(f"{name}: normal/pure scene contract differs")
    captures = [{capture["mode"]: capture for capture in report["captures"]}
                for report in (normal, pure)]
    if any(len({capture["camera"] for capture in report["captures"]}) != 1
           for report in (normal, pure)):
        raise ValueError(f"{name}: each report must contain one fixed camera")
    for mapping, modes in ((captures[0], (-1, 0, 2)), (captures[1], (0, 2))):
        if any(mode not in mapping for mode in modes):
            raise ValueError(f"{name}: missing required output modes {modes}")
    arrays, capture_records, depths = {}, {}, []
    for prefix, index, modes in (("normal", 0, (-1, 0, 2)), ("pure", 1, (0, 2))):
        for mode in modes:
            key = f"{prefix}_{mode}"
            capture_records[key] = captures[index][mode]
            arrays[key], depth = read_capture(captures[index][mode], normal["internal_resolution"])
            depths.append(depth)
    if len({record["sample"] for record in capture_records.values()}) != 1:
        raise ValueError(f"{name}: final frame sample differs")
    if any(not np.array_equal(depths[0].view(np.uint32), depth.view(np.uint32))
           for depth in depths[1:]):
        raise ValueError(f"{name}: raster depth differs across modes")
    foreground = depths[0] < 1
    if not foreground.any():
        raise ValueError(f"{name}: no foreground pixels")
    values = arrays["normal_0"] @ LUMA
    indices = np.flatnonzero(foreground)
    # An exact 20% selection with deterministic row-major tie breaking; the same
    # pixels are evaluated in all normal outputs, never reselected after lighting.
    count = int(np.ceil(indices.size * 0.2))
    selected = indices[np.argsort(values.ravel()[indices], kind="stable")[:count]]
    dark = np.zeros(values.size, dtype=bool)
    dark[selected] = True
    dark = dark.reshape(values.shape)
    delta = (arrays["normal_2"] - arrays["normal_0"]) @ LUMA
    return {"name": name, "normal_path": normal_path, "pure_path": pure_path,
            "normal_report": normal, "pure_report": pure, "arrays": arrays,
            "capture_records": capture_records, "foreground": foreground,
            "dark": dark, "dark_threshold": float(values[dark].max()), "delta": delta}


def preview(rgb: np.ndarray, exposure: float) -> Image.Image:
    value = np.maximum(rgb * exposure, 0)
    value /= 1 + value
    value = np.where(value <= 0.0031308, 12.92 * value,
                     1.055 * np.power(value, 1 / 2.4) - 0.055)
    return Image.fromarray(np.uint8(np.clip(np.rint(value * 255), 0, 255)))


def delta_preview(delta: np.ndarray, foreground: np.ndarray, scale: float) -> Image.Image:
    normalized = np.clip(delta / scale, -1, 1)
    neutral = np.array([27, 30, 35], dtype=np.float64)
    positive, negative = np.array([255, 216, 49]), np.array([49, 156, 255])
    endpoint = np.where((normalized >= 0)[..., None], positive, negative)
    weight = np.abs(normalized)[..., None]
    value = neutral * (1 - weight) + endpoint * weight
    value[~foreground] = 0
    return Image.fromarray(np.uint8(np.rint(value)))


def font(size: int) -> ImageFont.ImageFont:
    for path in (Path("C:/Windows/Fonts/arial.ttf"), Path("C:/Windows/Fonts/msyh.ttc")):
        if path.exists():
            return ImageFont.truetype(str(path), size)
    return ImageFont.load_default(size=size)


def fit_text(draw: ImageDraw.ImageDraw, text: str, width: int, size: int = 18) -> ImageFont.ImageFont:
    while size > 11:
        face = font(size)
        if draw.textbbox((0, 0), text, font=face)[2] <= width:
            return face
        size -= 1
    return font(size)


def stats(values: np.ndarray) -> dict:
    return {"count": int(values.size), "mean": float(values.mean()),
            "median": float(np.median(values)), "p10": float(np.quantile(values, .1)),
            "p90": float(np.quantile(values, .9))}


def save_sheet(view: dict, output: Path, delta_scale: float) -> dict:
    arrays = view["arrays"]
    foreground, dark, delta = view["foreground"], view["dark"], view["delta"]
    height, width = foreground.shape
    margin, gap, label_height, header_height = 16, 12, 38, 88
    panel_width, panel_height = max(width, 440), height
    footer_height = max(210, int(height * .45) + 30)
    canvas_width = margin * 2 + 3 * panel_width + gap * 2
    canvas_height = header_height + 2 * (label_height + panel_height + gap) + footer_height
    canvas = Image.new("RGB", (canvas_width, canvas_height), "#171a1f")
    draw = ImageDraw.Draw(canvas)
    draw.text((margin, 10), f"SSGI / VXGI luminance comparison - {view['name']}", fill="white", font=font(25))
    draw.text((margin, 43), "Linear HDR data; fixed exposure within each row. Foreground only for all statistics (raster depth < 1).",
              fill="#c0c7d2", font=font(17))
    draw.text((margin, 65), "Offscreen diagnostic, no DLSS/postprocess/editor/Present; not a main-window screenshot.",
              fill="#c0c7d2", font=font(17))
    normal_images = [preview(arrays[f"normal_{mode}"], NORMAL_EXPOSURE) for mode in (-1, 0, 2)]
    pure_images = [preview(arrays[f"pure_{mode}"], PURE_EXPOSURE) for mode in (0, 2)]
    pure_images.append(delta_preview(delta, foreground, delta_scale))
    labels = [["No SSGI / no VXGI  |  exposure x1", "SSGI only (normal shading)  |  exposure x1",
               "SSGI + VXGI (normal shading)  |  exposure x1"],
              ["Pure SSGI (E/pi, no receiver albedo)  |  exposure x4",
               "Pure complete GI (E/pi, no receiver albedo)  |  exposure x4",
               f"Signed normal luma delta: VXGI on - off  |  +/-{delta_scale:.6g}"]]
    for row, images in enumerate((normal_images, pure_images)):
        y = header_height + row * (label_height + panel_height + gap)
        for column, image in enumerate(images):
            x = margin + column * (panel_width + gap)
            face = fit_text(draw, labels[row][column], panel_width - 8)
            draw.text((x + 4, y + 9), labels[row][column], fill="white", font=face)
            canvas.paste(image, (x + (panel_width - width) // 2, y + label_height))
    footer_y = header_height + 2 * (label_height + panel_height + gap)
    overlay = np.asarray(normal_images[1]).astype(np.float64).copy()
    overlay[dark] = .35 * overlay[dark] + .65 * np.array([237, 72, 221])
    overlay_image = Image.fromarray(np.uint8(np.rint(overlay)))
    thumbnail_height = footer_height - 43
    thumbnail_width = min(int(thumbnail_height * width / height), panel_width)
    overlay_image.thumbnail((thumbnail_width, thumbnail_height), Image.Resampling.LANCZOS)
    canvas.paste(overlay_image, (margin, footer_y + 29))
    draw.text((margin, footer_y + 4), "Fixed darkest 20% mask in magenta", fill="#f397ed", font=font(16))
    x = margin + panel_width + gap
    normal_means = {mode: float((arrays[f"normal_{mode}"] @ LUMA)[foreground].mean()) for mode in (-1, 0, 2)}
    dark_means = {mode: float((arrays[f"normal_{mode}"] @ LUMA)[dark].mean()) for mode in (-1, 0, 2)}
    pure_means = {mode: float((arrays[f"pure_{mode}"] @ LUMA)[foreground].mean()) for mode in (0, 2)}
    lines = [
        "ALL MEANS BELOW USE ORIGINAL LINEAR HDR, BEFORE DISPLAY MAPPING.",
        f"Normal foreground mean: baseline {normal_means[-1]:.6g}, SSGI {normal_means[0]:.6g}, full {normal_means[2]:.6g}",
        f"Fixed dark-region mean: SSGI {dark_means[0]:.6g} -> full {dark_means[2]:.6g} ({dark_means[2] / dark_means[0]:.3g}x)" if dark_means[0] > 0 else
        f"Fixed dark-region mean: SSGI {dark_means[0]:.6g} -> full {dark_means[2]:.6g}; ratio undefined at zero",
        f"Pure GI foreground mean: SSGI {pure_means[0]:.6g} -> full {pure_means[2]:.6g}",
        "Pure E/pi and normal material-shaded images are different quantities; do not compare their means directly.",
        "Delta: yellow = positive; blue = negative; gray = zero; background = black.",
        f"Shared heatmap scale +/-{delta_scale:.6g} linear luma; clipping {np.mean(np.abs(delta[foreground]) > delta_scale):.2%}.",
        "Dark mask selected once from SSGI normal baseline, exact 20% (ties: stable row-major order).",
        "RGB previews: max(HDR * exposure, 0), Reinhard x/(1+x), then sRGB. No per-image auto exposure.",
    ]
    line_height = max(18, min(23, (footer_height - 4) // len(lines)))
    for index, text in enumerate(lines):
        draw.text((x, footer_y + index * line_height), text,
                  fill="white" if index == 0 else "#bdc7d5",
                  font=fit_text(draw, text, canvas_width - x - margin, 16))
    image_path = output / f"{view['name']}-comparison.png"
    canvas.save(image_path)
    return {"name": view["name"], "comparison_image": display_path(image_path),
            "normal_report": display_path(view["normal_path"]), "pure_report": display_path(view["pure_path"]),
            "report_sha256": {"normal": sha256(view["normal_path"]), "pure": sha256(view["pure_path"])},
            "raw_sha256": view["normal_report"]["raw_snapshot_sha256"],
            "raster_depth_bit_identical_across_outputs": True,
            "foreground_pixels": int(foreground.sum()), "dark_mask_pixels": int(dark.sum()),
            "dark_mask_luma_threshold_inclusive": view["dark_threshold"],
            "mask_selection": "Exact ceil(20% of foreground), stable ascending SSGI-normal luma, row-major tie order",
            "normal_foreground_luma": {str(mode): stats((arrays[f"normal_{mode}"] @ LUMA)[foreground]) for mode in (-1, 0, 2)},
            "normal_fixed_dark_luma": {str(mode): stats((arrays[f"normal_{mode}"] @ LUMA)[dark]) for mode in (-1, 0, 2)},
            "pure_foreground_luma": {str(mode): stats((arrays[f"pure_{mode}"] @ LUMA)[foreground]) for mode in (0, 2)},
            "normal_signed_delta_luma": {**stats(delta[foreground]),
                "negative_fraction_below_minus_1e_7": float(np.mean(delta[foreground] < -1e-7)),
                "positive_fraction_above_1e_7": float(np.mean(delta[foreground] > 1e-7)),
                "display_clipped_fraction": float(np.mean(np.abs(delta[foreground]) > delta_scale))},
            "captures": {key: {"linear_hdr": record["linear_hdr"],
                                 "linear_hdr_sha256": sha256(resolve(record["linear_hdr"])),
                                 "raster_depth_sha256": sha256(resolve(record["raster_depth"]))}
                         for key, record in view["capture_records"].items()}}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--normal", action="append", type=report_input, default=[], metavar="NAME=PATH")
    parser.add_argument("--pure", action="append", type=report_input, default=[], metavar="NAME=PATH")
    parser.add_argument("--prefix", help="Resolve tmp/VXGIStage75-PREFIX-normal/pure-NAME-WxH-report.json")
    parser.add_argument("--views", nargs="+", default=["exterior", "front", "cavity"])
    parser.add_argument("--resolution", default="640x400", help="Filename resolution used with --prefix")
    parser.add_argument("--output", type=Path, default=Path("tmp/vxgi-luminance"))
    parser.add_argument("--delta-scale", type=float, help="Shared symmetric linear normal-luma heatmap scale; default pooled absolute-delta p99")
    args = parser.parse_args()
    normal, pure = dict(args.normal), dict(args.pure)
    if args.prefix:
        for name in args.views:
            normal.setdefault(name, ROOT / f"tmp/VXGIStage75-{args.prefix}-normal-{name}-{args.resolution}-report.json")
            pure.setdefault(name, ROOT / f"tmp/VXGIStage75-{args.prefix}-pure-{name}-{args.resolution}-report.json")
    if not normal or normal.keys() != pure.keys():
        parser.error("Provide matching --normal NAME=PATH and --pure NAME=PATH, or --prefix")
    views = [load_view(name, path, pure[name]) for name, path in normal.items()]
    delta_scale = args.delta_scale
    scale_policy = "Explicit --delta-scale shared across all views"
    if delta_scale is None:
        pooled = np.concatenate([np.abs(view["delta"][view["foreground"]]) for view in views])
        delta_scale = max(float(np.quantile(pooled, .99)), 1e-8)
        scale_policy = "Pooled absolute normal-luma delta p99, shared across all views; floor 1e-8"
    if not np.isfinite(delta_scale) or delta_scale <= 0:
        parser.error("--delta-scale must be finite and positive")
    output = resolve(args.output)
    output.mkdir(parents=True, exist_ok=True)
    manifest = {"scope": "CPU-only comparison of hidden Stage75 captures. No new GPU work or main-window screenshots.",
                "luma": "Linear HDR Rec.709: .2126 R + .7152 G + .0722 B; all statistics before display mapping",
                "normal_exposure": NORMAL_EXPOSURE, "pure_exposure": PURE_EXPOSURE,
                "normal_vs_pure_units": "Normal is material-shaded radiance; pure is GI E/pi without receiver albedo. Do not compare their means as the same quantity.",
                "delta_quantity": "Normal full minus normal SSGI-only, signed linear luma. Marginal pipeline contribution, not isolated VXGI irradiance.",
                "delta_scale": delta_scale, "delta_scale_policy": scale_policy,
                "source_script_sha256": sha256(Path(__file__)),
                "views": [save_sheet(view, output, delta_scale) for view in views]}
    manifest_path = output / "render-manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(json.dumps({"manifest": display_path(manifest_path), "delta_scale": delta_scale,
                      "images": [entry["comparison_image"] for entry in manifest["views"]]}, indent=2))


if __name__ == "__main__":
    main()
