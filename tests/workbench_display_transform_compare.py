#!/usr/bin/env python3
"""Compare matched Workbench GPU display-transform captures."""

import argparse
import json
import math
import struct

import numpy as np
from PIL import Image


def read_pfm(path):
    with open(path, "rb") as source:
        magic = source.readline().strip()
        dimensions = source.readline().split()
        scale = float(source.readline())
        if magic != b"PF" or len(dimensions) != 2:
            raise ValueError(f"{path}: expected RGB PFM")
        width, height = map(int, dimensions)
        payload = source.read()
    if len(payload) != width * height * 3 * 4:
        raise ValueError(f"{path}: truncated or oversized PFM data")
    pixels = np.frombuffer(
        payload, dtype="<f4" if scale < 0 else ">f4"
    ).astype(np.float32)
    pixels = pixels.reshape(height, width, 3)[::-1]
    pixels *= abs(scale)
    if not np.isfinite(pixels).all():
        raise ValueError(f"{path}: contains non-finite values")
    return pixels


def read_png(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0


def interior_mask(linear, world, threshold):
    foreground = np.max(np.abs(linear - world), axis=2) > threshold
    interior = np.zeros_like(foreground)
    interior[1:-1, 1:-1] = foreground[1:-1, 1:-1]
    for y_offset in (-1, 0, 1):
        for x_offset in (-1, 0, 1):
            if x_offset == 0 and y_offset == 0:
                continue
            neighbor = linear[
                1 + y_offset:linear.shape[0] - 1 + y_offset,
                1 + x_offset:linear.shape[1] - 1 + x_offset,
            ]
            center = linear[1:-1, 1:-1]
            smooth = np.max(np.abs(center - neighbor), axis=2) < 0.025
            interior[1:-1, 1:-1] &= smooth
    return interior


def metrics(reference, candidate, mask):
    error = np.abs(reference - candidate)
    max_channel = np.max(error[mask], axis=1)
    return {
        "common_interior_pixels": int(mask.sum()),
        "mean_rgb_error": [float(value) for value in error[mask].mean(axis=0)],
        "mean_max_channel_error": float(max_channel.mean()),
        "p95_max_channel_error": float(np.percentile(max_channel, 95)),
        "max_channel_error": float(max_channel.max()),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--blender-prefix", required=True)
    parser.add_argument("--classi-prefix", required=True)
    parser.add_argument("--foreground-threshold", type=float, default=0.04)
    args = parser.parse_args()
    states = ("agx", "standard", "hdr-agx")
    world = np.asarray((0.12, 0.07, 0.035), dtype=np.float32)
    linear = {}
    display = {}
    dimensions = set()
    for renderer, prefix in (("blender", args.blender_prefix),
                             ("classiCAD", args.classi_prefix)):
        linear[renderer] = {}
        display[renderer] = {}
        for state in states:
            linear_path = f"{prefix}-{state}.pfm"
            display_path = f"{prefix}-{state}.png"
            linear[renderer][state] = read_pfm(linear_path)
            display[renderer][state] = read_png(display_path)
            if linear[renderer][state].shape != display[renderer][state].shape:
                raise SystemExit(f"{renderer} {state}: PNG/PFM size mismatch")
            dimensions.add(linear[renderer][state].shape[:2])
    if len(dimensions) != 1:
        raise SystemExit(f"capture dimension mismatch: {sorted(dimensions)}")

    base_mask = interior_mask(
        linear["blender"]["agx"], world, args.foreground_threshold
    ) & interior_mask(
        linear["classiCAD"]["agx"], world, args.foreground_threshold
    )
    if not base_mask.any():
        raise SystemExit("no common foreground interior pixels found")

    output = {}
    for state in states:
        output[state] = {
            "scene_linear": metrics(linear["blender"][state],
                                    linear["classiCAD"][state], base_mask),
            "display_srgb": metrics(display["blender"][state],
                                     display["classiCAD"][state], base_mask),
        }
    hdr_input = linear["blender"]["hdr-agx"][base_mask] * 16.0
    hdr_classi_input = linear["classiCAD"]["hdr-agx"][base_mask] * 16.0
    output["hdr_effective_scene_linear_input"] = {
        "exposure_scale": 16.0,
        "blender_max": float(hdr_input.max()),
        "classiCAD_max": float(hdr_classi_input.max()),
        "blender_pixels_over_one": int(np.max(hdr_input, axis=1).__gt__(1.0).sum()),
        "classiCAD_pixels_over_one": int(np.max(hdr_classi_input, axis=1).__gt__(1.0).sum()),
    }
    output["dimensions"] = {
        "width": int(next(iter(dimensions))[1]),
        "height": int(next(iter(dimensions))[0]),
        "common_foreground_interior_pixels": int(base_mask.sum()),
    }
    if not all(math.isfinite(value)
               for state in states
               for value in output[state]["display_srgb"]["mean_rgb_error"]):
        raise SystemExit("non-finite comparison result")
    print(json.dumps(output, indent=2, sort_keys=True, allow_nan=False))


if __name__ == "__main__":
    main()
