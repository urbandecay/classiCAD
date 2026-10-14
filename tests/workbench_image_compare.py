#!/usr/bin/env python3
"""Compare matched Blender and classiCAD display PNGs across the full frame."""

import argparse
import json

import numpy as np
from PIL import Image, ImageFilter


def load_rgb(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.float32)


def color_metrics(reference, candidate, mask):
    values = np.abs(reference[mask] - candidate[mask])
    if values.size == 0:
        raise ValueError("comparison region contains no pixels")
    return {
        "pixels": int(mask.sum()),
        "channel_mae": float(values.mean()),
        "channel_p95": float(np.percentile(values, 95.0)),
        "channel_max": float(values.max()),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", help="Blender display PNG")
    parser.add_argument("candidate", help="classiCAD display PNG")
    parser.add_argument("--foreground-threshold", type=float, default=5.0)
    parser.add_argument("--silhouette-interior-radius", type=int, default=4)
    args = parser.parse_args()

    reference = load_rgb(args.reference)
    candidate = load_rgb(args.candidate)
    if reference.shape != candidate.shape or reference.ndim != 3:
        raise ValueError(
            f"capture dimension mismatch: {reference.shape} vs {candidate.shape}"
        )
    height, width, channels = reference.shape
    if channels != 3 or width < 3 or height < 3:
        raise ValueError("captures must be RGB images at least 3x3 pixels")

    corners = np.stack((reference[0, 0], reference[0, -1],
                        reference[-1, 0], reference[-1, -1]))
    candidate_corners = np.stack((candidate[0, 0], candidate[0, -1],
                                  candidate[-1, 0], candidate[-1, -1]))
    reference_background = np.median(corners, axis=0)
    candidate_background = np.median(candidate_corners, axis=0)
    reference_mask = (
        np.max(np.abs(reference - reference_background), axis=2) >
        args.foreground_threshold
    )
    candidate_mask = (
        np.max(np.abs(candidate - candidate_background), axis=2) >
        args.foreground_threshold
    )
    union = np.count_nonzero(reference_mask | candidate_mask)
    intersection = np.count_nonzero(reference_mask & candidate_mask)
    radius = max(0, args.silhouette_interior_radius)
    if radius:
        size = radius * 2 + 1
        interior = np.asarray(
            Image.fromarray(reference_mask.astype(np.uint8) * 255)
            .filter(ImageFilter.MinFilter(size))
        ) > 0
    else:
        interior = reference_mask
    edge_band = reference_mask & ~interior

    report = {
        "dimensions": {"width": width, "height": height},
        "foreground_threshold": args.foreground_threshold,
        "reference_background_rgb": reference_background.tolist(),
        "candidate_background_rgb": candidate_background.tolist(),
        "foreground_mask": {
            "reference_pixels": int(reference_mask.sum()),
            "candidate_pixels": int(candidate_mask.sum()),
            "xor_pixels": int(np.count_nonzero(reference_mask ^ candidate_mask)),
            "iou": float(intersection / union) if union else 1.0,
        },
        "color_error": {
            "full_frame": color_metrics(
                reference, candidate,
                np.ones((height, width), dtype=bool),
            ),
            "common_foreground": color_metrics(
                reference, candidate, reference_mask & candidate_mask,
            ),
            "reference_interior_away_from_silhouette": color_metrics(
                reference, candidate, interior,
            ),
            "reference_silhouette_band": color_metrics(
                reference, candidate, edge_band,
            ),
        },
    }
    print(json.dumps(report, indent=2, sort_keys=True, allow_nan=False))


if __name__ == "__main__":
    main()
