#!/usr/bin/env python3
"""Compare transparent RGBA captures around a shared raster edge."""

import argparse
import json

import numpy as np
from PIL import Image


def load(path):
    image = np.asarray(Image.open(path).convert("RGBA"), dtype=np.int16)
    if image.ndim != 3 or image.shape[2] != 4:
        raise ValueError(f"expected an RGBA image: {path}")
    return image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", help="Blender RGBA PNG capture")
    parser.add_argument("candidate", help="QPainter RGBA PNG capture")
    parser.add_argument("--center", nargs=2, type=int, default=(80, 80),
                        metavar=("X", "Y"))
    parser.add_argument("--radius", type=int, default=1,
                        help="report pixels in this square around the center")
    args = parser.parse_args()
    if args.radius < 0:
        parser.error("--radius must be nonnegative")
    reference = load(args.reference)
    candidate = load(args.candidate)
    if reference.shape != candidate.shape:
        raise SystemExit(
            f"capture dimension mismatch: {reference.shape} != {candidate.shape}"
        )
    height, width = reference.shape[:2]
    x, y = args.center
    if not (0 <= x < width and 0 <= y < height):
        parser.error("--center must lie inside both captures")
    reference_mask = reference[:, :, 3] > 0
    candidate_mask = candidate[:, :, 3] > 0
    shared = reference_mask & candidate_mask
    union = reference_mask | candidate_mask
    error = np.abs(reference - candidate)
    ys = slice(max(0, y - args.radius), min(height, y + args.radius + 1))
    xs = slice(max(0, x - args.radius), min(width, x + args.radius + 1))
    report = {
        "dimensions": {"width": width, "height": height},
        "coverage": {
            "reference_pixels": int(reference_mask.sum()),
            "candidate_pixels": int(candidate_mask.sum()),
            "xor_pixels": int(np.logical_xor(reference_mask,
                                               candidate_mask).sum()),
            "iou": float((reference_mask & candidate_mask).sum() /
                         union.sum()) if union.any() else 1.0,
        },
        "shared_coverage_error": {
            "rgba_mae": error[shared].mean(axis=0).tolist(),
            "rgba_max": error[shared].max(axis=0).tolist(),
            "alpha_mae": float(error[:, :, 3][shared].mean()),
            "alpha_max": int(error[:, :, 3][shared].max()),
        },
        "center_neighborhood": {
            "origin": [x, y],
            "radius": args.radius,
            "reference_rgba": reference[ys, xs].tolist(),
            "candidate_rgba": candidate[ys, xs].tolist(),
        },
    }
    print(json.dumps(report, indent=2, sort_keys=True, allow_nan=False))


if __name__ == "__main__":
    main()
