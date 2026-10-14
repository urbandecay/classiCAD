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


def cardinal_outline_mask(object_mask):
    height, width = object_mask.shape
    boundary = np.zeros_like(object_mask)
    for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1)):
        neighbor = np.zeros_like(object_mask)
        y0 = max(0, dy)
        y1 = height + min(0, dy)
        x0 = max(0, dx)
        x1 = width + min(0, dx)
        neighbor[y0:y1, x0:x1] = object_mask[
            y0 - dy:y1 - dy, x0 - dx:x1 - dx
        ]
        boundary |= object_mask != neighbor
    return boundary


def report_outline_mask_attribution(reference_base, reference_effect,
                                    candidate_base, candidate_effect,
                                    threshold):
    def masks(base, effect):
        background = base[0, 0]
        object_mask = np.any(base != background, axis=2)
        observed = np.max(np.abs(effect - base), axis=2) >= threshold
        predicted = cardinal_outline_mask(object_mask)
        return object_mask, observed, predicted

    ref_object, ref_observed, ref_predicted = masks(
        reference_base, reference_effect
    )
    cand_object, cand_observed, cand_predicted = masks(
        candidate_base, candidate_effect
    )
    intersection = ref_object & cand_object
    union = ref_object | cand_object
    silhouette_iou = float(intersection.sum() / union.sum()) if union.any() else 1.0
    print("outline mask attribution:")
    print(f"  baseline object silhouettes: Blender={int(ref_object.sum())}, "
          f"classiCAD={int(cand_object.sum())}, "
          f"XOR={int(np.logical_xor(ref_object, cand_object).sum())}, "
          f"IoU={silhouette_iou:.6f}")
    for name, observed, predicted in (
        ("Blender", ref_observed, ref_predicted),
        ("classiCAD", cand_observed, cand_predicted),
    ):
        xor = int(np.logical_xor(observed, predicted).sum())
        print(f"  {name} observed vs four-cardinal boundary: "
              f"observed={int(observed.sum())}, "
              f"predicted={int(predicted.sum())}, XOR={xor}")
        if xor:
            return False
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("blender_baseline")
    parser.add_argument("blender_effect")
    parser.add_argument("classicad_baseline")
    parser.add_argument("classicad_effect")
    parser.add_argument("--threshold", type=int, default=2,
                        help="changed-pixel channel threshold (default: 2/255)")
    parser.add_argument(
        "--outline-mask-attribution",
        action="store_true",
        help="compare effect masks with four-cardinal boundaries of baseline silhouettes",
    )
    args = parser.parse_args()
    if args.threshold < 1 or args.threshold > 255:
        parser.error("--threshold must be in [1, 255]")
    images = [read_rgb(path) for path in (
        args.blender_baseline, args.blender_effect,
        args.classicad_baseline, args.classicad_effect)]
    summarize("effect-delta comparison", *images, args.threshold)
    if args.outline_mask_attribution and not report_outline_mask_attribution(
        *images, args.threshold
    ):
        raise SystemExit("outline effect mask differs from its baseline cardinal boundary")


if __name__ == "__main__":
    main()
