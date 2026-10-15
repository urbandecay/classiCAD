#!/usr/bin/env python3
"""Compare scene-linear Combined RGBA for transparent Workbench captures."""

import argparse
import json
import math

import numpy as np
from PIL import Image


def read_pfm(path, channels):
    with open(path, "rb") as source:
        magic = source.readline().strip()
        dimensions = source.readline().split()
        scale = float(source.readline())
        expected_magic = b"PF" if channels == 3 else b"Pf"
        if magic != expected_magic or len(dimensions) != 2:
            raise ValueError(f"{path}: invalid PFM header")
        width, height = map(int, dimensions)
        payload = source.read()
    values = width * height * channels
    if len(payload) != values * 4:
        raise ValueError(f"{path}: invalid PFM payload size")
    dtype = "<f4" if scale < 0 else ">f4"
    pixels = np.frombuffer(payload, dtype=dtype).astype(np.float32)
    shape = (height, width, channels) if channels == 3 else (height, width)
    pixels = pixels.reshape(shape)[::-1]
    pixels *= abs(scale)
    if not np.isfinite(pixels).all():
        raise ValueError(f"{path}: contains non-finite values")
    return pixels


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--blender-color", required=True)
    parser.add_argument("--blender-alpha", required=True)
    parser.add_argument("--blender-png", required=True)
    parser.add_argument("--classi-color", required=True)
    parser.add_argument("--classi-alpha", required=True)
    parser.add_argument("--classi-png", required=True)
    args = parser.parse_args()

    blender_color = read_pfm(args.blender_color, 3)
    blender_alpha = read_pfm(args.blender_alpha, 1)
    classi_color = read_pfm(args.classi_color, 3)
    classi_alpha = read_pfm(args.classi_alpha, 1)
    blender_png = np.asarray(Image.open(args.blender_png).convert("RGBA"))
    classi_png = np.asarray(Image.open(args.classi_png).convert("RGBA"))
    shapes = {blender_color.shape[:2], blender_alpha.shape,
              classi_color.shape[:2], classi_alpha.shape,
              blender_png.shape[:2], classi_png.shape[:2]}
    if len(shapes) != 1:
        raise SystemExit(f"capture dimension mismatch: {sorted(shapes)}")

    common_coverage = (blender_alpha > 0.05) & (classi_alpha > 0.05)
    if not common_coverage.any():
        raise SystemExit("no common partially covered fixture pixels")
    color_error = np.abs(blender_color - classi_color)
    alpha_error = np.abs(blender_alpha - classi_alpha)
    center = (blender_alpha.shape[0] // 2, blender_alpha.shape[1] // 2)
    height, width = blender_alpha.shape
    transparent = {
        "blender_pixels_below_1e-4": int((blender_alpha < 1.0e-4).sum()),
        "classiCAD_pixels_below_1e-4": int((classi_alpha < 1.0e-4).sum()),
        "blender_corner_alpha": float(blender_alpha[5, 5]),
        "classiCAD_corner_alpha": float(classi_alpha[5, 5]),
        "blender_center_overlap_alpha": float(blender_alpha[center]),
        "classiCAD_center_overlap_alpha": float(classi_alpha[center]),
    }
    png_alpha_error = np.abs(
        blender_png[:, :, 3].astype(np.int16) -
        classi_png[:, :, 3].astype(np.int16)
    )
    png_rgb_error = np.abs(
        blender_png[:, :, :3].astype(np.int16) -
        classi_png[:, :, :3].astype(np.int16)
    )
    display_max_channel_error = np.max(png_rgb_error, axis=2)
    scene_linear_p95_max_error = float(np.percentile(
        np.max(color_error[common_coverage], axis=1), 95
    ))
    report = {
        "dimensions": {"width": width, "height": height},
        "common_coverage_pixels": int(common_coverage.sum()),
        "scene_linear_rgb_error_on_common_coverage": {
            "mean_rgb": [float(value)
                         for value in color_error[common_coverage].mean(axis=0)],
            "mean_max_channel": float(
                np.max(color_error[common_coverage], axis=1).mean()
            ),
            "p95_max_channel": scene_linear_p95_max_error,
        },
        "scene_linear_alpha_error": {
            "mean_absolute": float(alpha_error.mean()),
            "mean_on_common_coverage": float(alpha_error[common_coverage].mean()),
            "max_absolute": float(alpha_error.max()),
        },
        "transparent_background_and_overlap": transparent,
        "display_png_alpha_error": {
            "mean_8bit": float(png_alpha_error.mean()),
            "max_8bit": int(png_alpha_error.max()),
            "rgb_mean_8bit": float(png_rgb_error.mean()),
            "rgb_p95_max_channel_8bit": float(
                np.percentile(display_max_channel_error, 95)
            ),
            "rgb_max_channel_8bit": int(png_rgb_error.max()),
            "classiCAD_corner_rgba": classi_png[5, 5].tolist(),
            "blender_corner_rgba": blender_png[5, 5].tolist(),
            "classiCAD_center_rgba": classi_png[center].tolist(),
            "blender_center_rgba": blender_png[center].tolist(),
        },
    }
    if not all(math.isfinite(value)
               for value in report["scene_linear_rgb_error_on_common_coverage"]["mean_rgb"]):
        raise SystemExit("non-finite comparison result")
    if (alpha_error.max() > 1.0e-6 or
            scene_linear_p95_max_error > 0.001 or
            png_alpha_error.max() > 1 or png_rgb_error.max() > 1 or
            blender_alpha[5, 5] > 1.0e-4 or
            classi_alpha[5, 5] > 1.0e-4 or
            not 0.5 < blender_alpha[center] < 1.0 or
            not 0.5 < classi_alpha[center] < 1.0):
        raise SystemExit("transparent-film comparison exceeded its alpha/color tolerances")
    print(json.dumps(report, indent=2, sort_keys=True, allow_nan=False))


if __name__ == "__main__":
    main()
