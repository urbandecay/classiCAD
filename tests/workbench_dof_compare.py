#!/usr/bin/env python3
"""Compare the DOF change masks from matched Blender and classiCAD captures."""

import argparse
import json
import os

from PIL import Image


def load_rgb(path):
    image = Image.open(path).convert("RGB")
    return image.size, list(image.getdata())


def effect_mask(baseline, dof, threshold):
    changed = []
    magnitudes = []
    for before, after in zip(baseline, dof):
        magnitude = max(abs(a - b) for a, b in zip(before, after))
        magnitudes.append(magnitude)
        changed.append(magnitude > threshold)
    return changed, magnitudes


def image_delta(left, right):
    values = [max(abs(a - b) for a, b in zip(p, q))
              for p, q in zip(left, right)]
    return {
        "mean_max_channel_delta": sum(values) / len(values),
        "pixels_over_2": sum(value > 2 for value in values),
        "pixels_over_3": sum(value > 3 for value in values),
    }


def mask_metrics(blender_mask, classi_mask):
    intersection = sum(a and b for a, b in zip(blender_mask, classi_mask))
    union = sum(a or b for a, b in zip(blender_mask, classi_mask))
    return {
        "blender_effect_pixels": sum(blender_mask),
        "classiCAD_effect_pixels": sum(classi_mask),
        "effect_mask_xor_pixels": sum(a != b for a, b in zip(blender_mask, classi_mask)),
        "effect_mask_iou": intersection / union if union else 1.0,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("blender_baseline")
    parser.add_argument("blender_dof")
    parser.add_argument("classiCAD_baseline")
    parser.add_argument("classiCAD_dof")
    parser.add_argument("--threshold", type=int, default=2)
    args = parser.parse_args()

    captures = [load_rgb(path) for path in (
        args.blender_baseline, args.blender_dof,
        args.classiCAD_baseline, args.classiCAD_dof,
    )]
    sizes = {capture[0] for capture in captures}
    if len(sizes) != 1:
        raise SystemExit(f"capture dimension mismatch: {sorted(sizes)}")
    blender_mask, blender_delta = effect_mask(
        captures[0][1], captures[1][1], args.threshold
    )
    classi_mask, classi_delta = effect_mask(
        captures[2][1], captures[3][1], args.threshold
    )
    report = {
        "dimensions": {"width": captures[0][0][0], "height": captures[0][0][1]},
        "threshold": args.threshold,
        **mask_metrics(blender_mask, classi_mask),
        "threshold_3_sensitivity": mask_metrics(
            *[effect_mask(captures[index][1], captures[index + 1][1], 3)[0]
              for index in (0, 2)]
        ),
        "mean_per_pixel_max_channel_delta": {
            "blender": sum(blender_delta) / len(blender_delta),
            "classiCAD": sum(classi_delta) / len(classi_delta),
        },
        "raw_output_delta": {
            "baseline_blender_vs_classiCAD": image_delta(
                captures[0][1], captures[2][1]
            ),
            "dof_blender_vs_classiCAD": image_delta(
                captures[1][1], captures[3][1]
            ),
        },
        "captures": [os.path.basename(path) for path in (
            args.blender_baseline, args.blender_dof,
            args.classiCAD_baseline, args.classiCAD_dof,
        )],
    }
    print(json.dumps(report, indent=2, sort_keys=True, allow_nan=False))


if __name__ == "__main__":
    main()
