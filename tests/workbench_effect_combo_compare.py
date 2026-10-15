#!/usr/bin/env python3
"""Compare Blender and classiCAD outline/cavity interaction captures."""

import argparse
import json
import math
import os
import struct

from PIL import Image


EFFECTS = ("baseline", "outline", "cavity", "outline-cavity")


def read_png(path):
    image = Image.open(path).convert("RGB")
    return image.size, list(image.getdata())


def read_pfm(path):
    with open(path, "rb") as capture:
        kind = capture.readline().decode("ascii").strip()
        dimensions = capture.readline().decode("ascii").split()
        scale = float(capture.readline().decode("ascii"))
        if kind != "PF" or len(dimensions) != 2:
            raise ValueError(f"{path}: expected RGB PFM header")
        width, height = map(int, dimensions)
        count = width * height * 3
        payload = capture.read()
        if len(payload) != count * 4:
            raise ValueError(f"{path}: expected {count * 4} data bytes")
        endian = "<" if scale < 0 else ">"
        pixels = list(struct.unpack(f"{endian}{count}f", payload))
        multiplier = abs(scale)
        pixels = [value * multiplier for value in pixels]
        if not all(math.isfinite(value) for value in pixels):
            raise ValueError(f"{path}: contains non-finite values")
        return (width, height), pixels


def difference_mask(before, after, threshold):
    return [max(abs(a - b) for a, b in zip(left, right)) > threshold
            for left, right in zip(before, after)]


def mask_metrics(left, right):
    intersection = sum(a and b for a, b in zip(left, right))
    union = sum(a or b for a, b in zip(left, right))
    return {
        "blender_pixels": sum(left),
        "classiCAD_pixels": sum(right),
        "xor_pixels": sum(a != b for a, b in zip(left, right)),
        "iou": intersection / union if union else 1.0,
    }


def percentile(values, fraction):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1,
                       math.ceil(fraction * len(ordered)) - 1)]


def interaction(pixels):
    baseline, outline, cavity, combined = pixels
    residuals = [
        combined[index] - outline[index] - cavity[index] + baseline[index]
        for index in range(len(baseline))
    ]
    per_pixel = [max(abs(residuals[index + channel]) for channel in range(3))
                 for index in range(0, len(residuals), 3)]
    return {
        "mean_max_channel_residual": sum(per_pixel) / len(per_pixel),
        "p95_max_channel_residual": percentile(per_pixel, 0.95),
        "max_channel_residual": max(per_pixel),
        "pixels_over_0_001": sum(value > 0.001 for value in per_pixel),
    }, residuals


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--blender-prefix", required=True)
    parser.add_argument("--classi-prefix", required=True)
    parser.add_argument("--threshold", type=int, default=2,
                        help="display-pixel effect mask threshold (default: 2)")
    args = parser.parse_args()

    pngs = {}
    pfms = {}
    dimensions = set()
    for renderer, prefix in (("blender", args.blender_prefix),
                             ("classiCAD", args.classi_prefix)):
        pngs[renderer] = {}
        pfms[renderer] = {}
        for effect in EFFECTS:
            png_path = f"{prefix}-{effect}.png"
            pfm_path = f"{prefix}-{effect}.pfm"
            png_size, pngs[renderer][effect] = read_png(png_path)
            pfm_size, pfms[renderer][effect] = read_pfm(pfm_path)
            if png_size != pfm_size:
                raise SystemExit(f"{renderer} {effect}: PNG/PFM size mismatch")
            dimensions.add(png_size)
    if len(dimensions) != 1:
        raise SystemExit(f"capture dimension mismatch: {sorted(dimensions)}")

    masks = {}
    for effect in EFFECTS[1:]:
        blender_mask = difference_mask(
            pngs["blender"]["baseline"], pngs["blender"][effect], args.threshold
        )
        classi_mask = difference_mask(
            pngs["classiCAD"]["baseline"], pngs["classiCAD"][effect], args.threshold
        )
        masks[effect] = mask_metrics(blender_mask, classi_mask)

    blender_interaction, blender_residual = interaction(
        [pfms["blender"][effect] for effect in EFFECTS]
    )
    classi_interaction, classi_residual = interaction(
        [pfms["classiCAD"][effect] for effect in EFFECTS]
    )
    cross_residual = [abs(left - right)
                      for left, right in zip(blender_residual, classi_residual)]
    cross_per_pixel = [max(cross_residual[index + channel] for channel in range(3))
                       for index in range(0, len(cross_residual), 3)]
    report = {
        "dimensions": {"width": next(iter(dimensions))[0],
                       "height": next(iter(dimensions))[1]},
        "display_effect_threshold": args.threshold,
        "effect_masks": masks,
        "scene_linear_interaction_residual": {
            "blender": blender_interaction,
            "classiCAD": classi_interaction,
            "cross_renderer_difference": {
                "mean_max_channel": sum(cross_per_pixel) / len(cross_per_pixel),
                "p95_max_channel": percentile(cross_per_pixel, 0.95),
                "max_channel": max(cross_per_pixel),
            },
        },
        "captures": {
            renderer: {effect: os.path.basename(f"{prefix}-{effect}.png")
                       for effect in EFFECTS}
            for renderer, prefix in (("blender", args.blender_prefix),
                                     ("classiCAD", args.classi_prefix))
        },
    }
    print(json.dumps(report, indent=2, sort_keys=True, allow_nan=False))


if __name__ == "__main__":
    main()
