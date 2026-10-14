#!/usr/bin/env python3
"""Compare cast-shadow deltas after removing unoccluded light changes."""

import argparse
import json

import numpy as np

from workbench_pfm_compare import read_rgb_pfm


def load(path):
    width, height, pixels = read_rgb_pfm(path)
    return np.asarray(pixels, dtype=np.float32).reshape(height, width, 3)


def receiver_mask(image):
    # The controlled fixture assigns the receiver blue and the caster red.
    return ((image[:, :, 2] > image[:, :, 0] * 1.35) &
            (image[:, :, 1] > image[:, :, 0] * 1.15) &
            (image[:, :, 2] > 0.04))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("blender_baseline")
    parser.add_argument("blender_effect")
    parser.add_argument("blender_receiver_only_baseline")
    parser.add_argument("blender_receiver_only_effect")
    parser.add_argument("classicad_baseline")
    parser.add_argument("classicad_effect")
    parser.add_argument("classicad_receiver_only_baseline")
    parser.add_argument("classicad_receiver_only_effect")
    parser.add_argument("--threshold", type=float, default=0.001)
    args = parser.parse_args()

    captures = [load(path) for path in (
        args.blender_baseline, args.blender_effect,
        args.blender_receiver_only_baseline,
        args.blender_receiver_only_effect,
        args.classicad_baseline, args.classicad_effect,
        args.classicad_receiver_only_baseline,
        args.classicad_receiver_only_effect,
    )]
    if len({image.shape for image in captures}) != 1:
        raise SystemExit("matched scene-linear captures must have equal dimensions")
    (blender_base, blender_effect, blender_empty_base, blender_empty_effect,
     classiCAD_base, classiCAD_effect, classiCAD_empty_base,
     classiCAD_empty_effect) = captures

    blender_receiver = receiver_mask(blender_base)
    classiCAD_receiver = receiver_mask(classiCAD_base)
    shared_receiver = blender_receiver & classiCAD_receiver
    if not blender_receiver.any() or not classiCAD_receiver.any() or not shared_receiver.any():
        raise SystemExit("could not identify the blue receiver in both baselines")

    # With shadows enabled Blender also changes the base light response. The
    # receiver-only pair measures that change; subtract it to isolate the
    # additional contribution from the caster's shadow volume.
    blender_delta = (blender_effect - blender_base) - (
        blender_empty_effect - blender_empty_base
    )
    classiCAD_delta = (classiCAD_effect - classiCAD_base) - (
        classiCAD_empty_effect - classiCAD_empty_base
    )
    blender_magnitude = np.max(np.abs(blender_delta), axis=2)
    classiCAD_magnitude = np.max(np.abs(classiCAD_delta), axis=2)
    blender_shadow = blender_receiver & (blender_magnitude > args.threshold)
    classiCAD_shadow = classiCAD_receiver & (classiCAD_magnitude > args.threshold)
    shared_union = blender_shadow | classiCAD_shadow
    shared_intersection = blender_shadow & classiCAD_shadow

    def summary(mask, magnitude, delta):
        selected = magnitude[mask]
        return {
            "pixels": int(mask.sum()),
            "mean_max_channel_effect": float(selected.mean()) if selected.size else 0.0,
            "p95_max_channel_effect": float(np.percentile(selected, 95)) if selected.size else 0.0,
            "max_max_channel_effect": float(selected.max()) if selected.size else 0.0,
            "mean_rgb_delta": [float(value) for value in delta[mask].mean(axis=0)]
            if selected.size else [0.0, 0.0, 0.0],
        }

    def changed_summary(mask, magnitude, delta):
        return summary(mask & (magnitude > args.threshold), magnitude, delta)

    receiver_union = blender_receiver | classiCAD_receiver
    receiver_iou = float(shared_receiver.sum() / receiver_union.sum())
    shadow_union_count = int(shared_union.sum())
    shadow_iou = float(shared_intersection.sum() / shadow_union_count) \
        if shadow_union_count else 1.0
    common_delta_error = np.abs(blender_delta - classiCAD_delta)[shared_receiver]
    union_delta_error = np.abs(blender_delta - classiCAD_delta)[
        shared_receiver & shared_union
    ]
    report = {
        "dimensions": {
            "width": int(blender_base.shape[1]),
            "height": int(blender_base.shape[0]),
        },
        "threshold": args.threshold,
        "caster_only_delta": True,
        "receiver_masks": {
            "blender_pixels": int(blender_receiver.sum()),
            "classiCAD_pixels": int(classiCAD_receiver.sum()),
            "iou": receiver_iou,
        },
        "effect_masks_inside_receiver": {
            "blender_pixels": int(blender_shadow.sum()),
            "classiCAD_pixels": int(classiCAD_shadow.sum()),
            "iou": shadow_iou,
        },
        "blender_receiver_effect": summary(
            blender_receiver, blender_magnitude, blender_delta
        ),
        "blender_cast_shadow_only": changed_summary(
            blender_receiver, blender_magnitude, blender_delta
        ),
        "classiCAD_receiver_effect": summary(
            classiCAD_receiver, classiCAD_magnitude, classiCAD_delta
        ),
        "classiCAD_cast_shadow_only": changed_summary(
            classiCAD_receiver, classiCAD_magnitude, classiCAD_delta
        ),
        "effect_delta_absolute_rgb_error": {
            "mean_on_shared_receiver": float(common_delta_error.mean()),
            "mean_on_effect_mask_union": float(union_delta_error.mean())
            if union_delta_error.size else 0.0,
        },
    }
    print(json.dumps(report, indent=2, sort_keys=True, allow_nan=False))


if __name__ == "__main__":
    main()
