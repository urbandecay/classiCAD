#!/usr/bin/env python3
"""Compare Blender/classiCAD effect deltas relative to matched baselines."""

import argparse

import numpy as np
from PIL import Image


def read_rgb(path):
    image = np.asarray(Image.open(path).convert("RGB"), dtype=np.int16)
    if image.shape != (480, 640, 3):
        raise ValueError(f"expected 640x480 RGB capture: {path}")
    return image


def summarize(name, reference_base, reference_effect,
             candidate_base, candidate_effect, threshold):
    reference_delta = reference_effect - reference_base
    candidate_delta = candidate_effect - candidate_base
    reference_mask = np.max(np.abs(reference_delta), axis=2) >= threshold
    candidate_mask = np.max(np.abs(candidate_delta), axis=2) >= threshold
    union = reference_mask | candidate_mask
    intersection = reference_mask & candidate_mask
    error = np.abs(reference_delta.astype(np.int32) -
                    candidate_delta.astype(np.int32))
    magnitudes_ref = np.max(np.abs(reference_delta), axis=2)[reference_mask]
    magnitudes_candidate = np.max(np.abs(candidate_delta), axis=2)[candidate_mask]
    union_error = error[union]
    all_error = error.reshape(-1)
    iou = float(intersection.sum() / union.sum()) if union.any() else 1.0
    print(f"{name}: threshold={threshold}/255")
    print(f"  effect pixels: Blender={int(reference_mask.sum())}, "
          f"classiCAD={int(candidate_mask.sum())}, "
          f"XOR={int(np.logical_xor(reference_mask, candidate_mask).sum())}, "
          f"IoU={iou:.6f}")
    print(f"  delta magnitude max-channel /255: "
          f"Blender mean={magnitudes_ref.mean():.4f}, "
          f"p95={np.percentile(magnitudes_ref, 95):.2f}, "
          f"max={magnitudes_ref.max()}; classiCAD mean="
          f"{magnitudes_candidate.mean():.4f}, "
          f"p95={np.percentile(magnitudes_candidate, 95):.2f}, "
          f"max={magnitudes_candidate.max()}")
    print(f"  signed RGB-delta error /255: whole-image MAE="
          f"{all_error.mean():.5f}, union MAE={union_error.mean():.5f}, "
          f"union p95={np.percentile(union_error, 95):.2f}, "
          f"union max={union_error.max()}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("blender_baseline")
    parser.add_argument("blender_effect")
    parser.add_argument("classicad_baseline")
    parser.add_argument("classicad_effect")
    parser.add_argument("--threshold", type=int, default=2,
                        help="changed-pixel channel threshold (default: 2/255)")
    args = parser.parse_args()
    if args.threshold < 1 or args.threshold > 255:
        parser.error("--threshold must be in [1, 255]")
    images = [read_rgb(path) for path in (
        args.blender_baseline, args.blender_effect,
        args.classicad_baseline, args.classicad_effect)]
    summarize("effect-delta comparison", *images, args.threshold)


if __name__ == "__main__":
    main()
