#!/usr/bin/env python3
"""Attribute outline blend differences from matched scene-linear PFM captures."""

import argparse
import json
import math

from workbench_pfm_compare import read_rgb_pfm


def load(path):
    width, height, pixels = read_rgb_pfm(path)
    return width, height, pixels


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("blender_baseline")
    parser.add_argument("blender_outline")
    parser.add_argument("classicad_baseline")
    parser.add_argument("classicad_outline")
    parser.add_argument("--threshold", type=float, default=0.001)
    args = parser.parse_args()
    captures = [load(path) for path in (
        args.blender_baseline, args.blender_outline,
        args.classicad_baseline, args.classicad_outline,
    )]
    dimensions = {(image[0], image[1]) for image in captures}
    if len(dimensions) != 1:
        raise SystemExit(f"capture dimension mismatch: {sorted(dimensions)}")
    count = captures[0][0] * captures[0][1]
    values = [image[2] for image in captures]

    def mask(baseline, effect):
        return [max(abs(effect[i * 3 + channel] - baseline[i * 3 + channel])
                    for channel in range(3)) > args.threshold
                for i in range(count)]

    blender_mask = mask(values[0], values[1])
    classi_mask = mask(values[2], values[3])
    intersection = sum(a and b for a, b in zip(blender_mask, classi_mask))
    union = sum(a or b for a, b in zip(blender_mask, classi_mask))
    xor = sum(a != b for a, b in zip(blender_mask, classi_mask))
    delta_errors = []
    baseline_errors = []
    output_errors = []
    alphas = [[], []]
    alpha_quarter_errors = [[], []]
    for pixel in range(count):
        start = pixel * 3
        for channel in range(3):
            b_delta = values[1][start + channel] - values[0][start + channel]
            c_delta = values[3][start + channel] - values[2][start + channel]
            delta_errors.append(abs(b_delta - c_delta))
            baseline_errors.append(abs(values[0][start + channel] -
                                       values[2][start + channel]))
            output_errors.append(abs(values[1][start + channel] -
                                     values[3][start + channel]))
        for index, baseline, effect, effect_mask in (
            (0, values[0], values[1], blender_mask),
            (1, values[2], values[3], classi_mask),
        ):
            if not effect_mask[pixel]:
                continue
            denominator = sum(baseline[start + channel] ** 2
                              for channel in range(3))
            if denominator <= 1.0e-8:
                continue
            alpha = sum(
                baseline[start + channel] *
                (baseline[start + channel] - effect[start + channel])
                for channel in range(3)
            ) / denominator
            alphas[index].append(alpha)
            alpha_quarter_errors[index].append(
                abs(alpha * 4.0 - round(alpha * 4.0))
            )

    def summarize_alpha(samples):
        ordered = sorted(samples)
        if not ordered:
            return {"count": 0}
        return {
            "count": len(ordered),
            "mean": sum(ordered) / len(ordered),
            "median": ordered[len(ordered) // 2],
            "p95": ordered[min(len(ordered) - 1,
                               math.ceil(len(ordered) * 0.95) - 1)],
        }

    result = {
        "dimensions": {"width": captures[0][0], "height": captures[0][1]},
        "linear_effect_threshold": args.threshold,
        "effect_masks": {
            "blender_pixels": sum(blender_mask),
            "classiCAD_pixels": sum(classi_mask),
            "xor_pixels": xor,
            "iou": intersection / union if union else 1.0,
        },
        "mean_absolute_rgb_error": {
            "baseline": sum(baseline_errors) / len(baseline_errors),
            "outline_output": sum(output_errors) / len(output_errors),
            "outline_delta": sum(delta_errors) / len(delta_errors),
        },
        "linear_outline_opacity": {
            "blender": summarize_alpha(alphas[0]),
            "classiCAD": summarize_alpha(alphas[1]),
            "mean_distance_to_quarter_steps": [
                sum(errors) / len(errors) if errors else None
                for errors in alpha_quarter_errors
            ],
        },
    }
    print(json.dumps(result, indent=2, sort_keys=True, allow_nan=False))


if __name__ == "__main__":
    main()
