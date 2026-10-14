#!/usr/bin/env python3
"""Compare matched Blender/classiCAD scene-linear effect PFM pairs."""

import argparse
import json
import math
import statistics

from workbench_pfm_compare import read_rgb_pfm


def load(path):
    width, height, pixels = read_rgb_pfm(path)
    return width, height, pixels


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("blender_baseline")
    parser.add_argument("blender_effect")
    parser.add_argument("classicad_baseline")
    parser.add_argument("classicad_effect")
    parser.add_argument("--threshold", type=float, default=0.001)
    args = parser.parse_args()
    captures = [load(path) for path in (
        args.blender_baseline, args.blender_effect,
        args.classicad_baseline, args.classicad_effect,
    )]
    dimensions = {(image[0], image[1]) for image in captures}
    if len(dimensions) != 1:
        raise SystemExit(f"capture dimension mismatch: {sorted(dimensions)}")
    count = captures[0][0] * captures[0][1]
    baseline_blender, effect_blender, baseline_classicad, effect_classicad = (
        image[2] for image in captures
    )
    effect_errors = []
    baseline_errors = []
    output_errors = []
    blender_magnitudes = []
    classicad_magnitudes = []
    blender_mask = []
    classicad_mask = []
    for pixel in range(count):
        start = pixel * 3
        blender_delta = [effect_blender[start + c] - baseline_blender[start + c]
                         for c in range(3)]
        classicad_delta = [effect_classicad[start + c] -
                           baseline_classicad[start + c] for c in range(3)]
        blender_magnitude = max(abs(value) for value in blender_delta)
        classicad_magnitude = max(abs(value) for value in classicad_delta)
        blender_magnitudes.append(blender_magnitude)
        classicad_magnitudes.append(classicad_magnitude)
        blender_mask.append(blender_magnitude > args.threshold)
        classicad_mask.append(classicad_magnitude > args.threshold)
        for channel in range(3):
            effect_errors.append(abs(blender_delta[channel] -
                                     classicad_delta[channel]))
            baseline_errors.append(abs(baseline_blender[start + channel] -
                                       baseline_classicad[start + channel]))
            output_errors.append(abs(effect_blender[start + channel] -
                                     effect_classicad[start + channel]))
    intersection = sum(a and b for a, b in zip(blender_mask, classicad_mask))
    union = sum(a or b for a, b in zip(blender_mask, classicad_mask))
    union_errors = [
        effect_errors[pixel * 3 + channel]
        for pixel in range(count) if blender_mask[pixel] or classicad_mask[pixel]
        for channel in range(3)
    ]
    result = {
        "dimensions": {"width": captures[0][0], "height": captures[0][1]},
        "linear_effect_threshold": args.threshold,
        "effect_masks": {
            "blender_pixels": sum(blender_mask),
            "classiCAD_pixels": sum(classicad_mask),
            "xor_pixels": sum(a != b for a, b in zip(blender_mask,
                                                      classicad_mask)),
            "iou": intersection / union if union else 1.0,
        },
        "mean_max_channel_effect": {
            "blender": statistics.fmean(blender_magnitudes),
            "classiCAD": statistics.fmean(classicad_magnitudes),
        },
        "mean_absolute_rgb_error": {
            "baseline": statistics.fmean(baseline_errors),
            "effect_output": statistics.fmean(output_errors),
            "effect_delta": statistics.fmean(effect_errors),
            "effect_delta_on_mask_union": statistics.fmean(union_errors),
            "effect_delta_union_p95": sorted(union_errors)[
                min(len(union_errors) - 1,
                    math.ceil(len(union_errors) * 0.95) - 1)
            ] if union_errors else 0.0,
        },
    }
    print(json.dumps(result, indent=2, sort_keys=True, allow_nan=False))


if __name__ == "__main__":
    main()
