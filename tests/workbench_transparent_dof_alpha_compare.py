#!/usr/bin/env python3
"""Compare transparent-film alpha behavior through Workbench DOF."""

import argparse
import json

from PIL import Image


def alpha(path):
    image = Image.open(path).convert("RGBA")
    return image.size, list(image.getchannel("A").getdata())


def metrics(before, after):
    delta = [abs(a - b) for a, b in zip(before, after)]
    before_mask = [value > 1 for value in before]
    after_mask = [value > 1 for value in after]
    intersection = sum(a and b for a, b in zip(before_mask, after_mask))
    union = sum(a or b for a, b in zip(before_mask, after_mask))
    return {
        "changed_pixels_over_1": sum(value > 1 for value in delta),
        "maximum_alpha_delta": max(delta, default=0),
        "coverage_xor_pixels": sum(a != b for a, b in zip(before_mask, after_mask)),
        "coverage_iou": intersection / union if union else 1.0,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("blender_baseline")
    parser.add_argument("blender_dof")
    parser.add_argument("classiCAD_baseline")
    parser.add_argument("classiCAD_dof")
    args = parser.parse_args()
    captures = [alpha(path) for path in (
        args.blender_baseline,
        args.blender_dof,
        args.classiCAD_baseline,
        args.classiCAD_dof,
    )]
    sizes = {item[0] for item in captures}
    if len(sizes) != 1:
        raise SystemExit(f"capture dimension mismatch: {sorted(sizes)}")
    blender_pair = metrics(captures[0][1], captures[1][1])
    classi_pair = metrics(captures[2][1], captures[3][1])
    cross_errors = [
        abs(blender - classi)
        for blender, classi in zip(captures[1][1], captures[3][1])
    ]
    report = {
        "dimensions": {"width": captures[0][0][0], "height": captures[0][0][1]},
        "blender_dof_alpha_change": blender_pair,
        "classiCAD_dof_alpha_change": classi_pair,
        "effect_alpha_cross_renderer": {
            "maximum_delta": max(cross_errors, default=0),
            "mean_delta": sum(cross_errors) / len(cross_errors) if cross_errors else 0.0,
            "pixels_over_1": sum(value > 1 for value in cross_errors),
        },
    }
    print(json.dumps(report, indent=2, sort_keys=True))
    if (blender_pair["maximum_alpha_delta"] > 1 or
            classi_pair["maximum_alpha_delta"] > 1 or
            report["effect_alpha_cross_renderer"]["maximum_delta"] > 1):
        raise SystemExit("transparent-film DOF alpha exceeds one display code")


if __name__ == "__main__":
    main()
