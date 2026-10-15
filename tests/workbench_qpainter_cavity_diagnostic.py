#!/usr/bin/env python3
"""Correlate QPainter cavity output errors with CPU/GPU cavity factors."""

import argparse

import numpy as np
from PIL import Image


def read_pfm(path):
    with open(path, "rb") as source:
        magic = source.readline().strip()
        dimensions = source.readline().split()
        scale = float(source.readline())
        if magic != b"Pf" or len(dimensions) != 2:
            raise ValueError(f"{path}: expected scalar PFM")
        width, height = map(int, dimensions)
        payload = source.read()
    if len(payload) != width * height * 4:
        raise ValueError(f"{path}: invalid PFM payload")
    dtype = "<f4" if scale < 0 else ">f4"
    values = np.frombuffer(payload, dtype=dtype).astype(np.float32)
    values = values.reshape(height, width)[::-1] * abs(scale)
    if not np.isfinite(values).all():
        raise ValueError(f"{path}: contains non-finite values")
    return values


def read_rgb(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.int16)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("blender_baseline")
    parser.add_argument("blender_effect")
    parser.add_argument("qpainter_baseline")
    parser.add_argument("qpainter_effect")
    parser.add_argument("gpu_factor")
    parser.add_argument("qpainter_factor")
    parser.add_argument("--threshold", type=int, default=3)
    args = parser.parse_args()

    images = [read_rgb(path) for path in (
        args.blender_baseline, args.blender_effect,
        args.qpainter_baseline, args.qpainter_effect,
    )]
    dimensions = {image.shape for image in images}
    if len(dimensions) != 1:
        raise SystemExit(f"PNG dimensions differ: {sorted(dimensions)}")
    blender_base, blender_effect, cpu_base, cpu_effect = images
    gpu_factor = read_pfm(args.gpu_factor)
    cpu_factor = read_pfm(args.qpainter_factor)
    if gpu_factor.shape != blender_base.shape[:2] or \
            cpu_factor.shape != blender_base.shape[:2]:
        raise SystemExit("factor and PNG dimensions differ")

    blender_delta = blender_effect.astype(np.int16) - blender_base
    cpu_delta = cpu_effect.astype(np.int16) - cpu_base
    blender_mask = np.max(np.abs(blender_delta), axis=2) >= args.threshold
    cpu_mask = np.max(np.abs(cpu_delta), axis=2) >= args.threshold
    mismatch = np.max(np.abs(blender_delta - cpu_delta), axis=2)
    union = blender_mask | cpu_mask
    common_foreground = np.any(blender_base != blender_base[0, 0], axis=2) & \
        np.any(cpu_base != cpu_base[0, 0], axis=2)
    factor_delta = np.abs(gpu_factor - cpu_factor)
    compared = factor_delta[common_foreground]
    if compared.size == 0:
        raise SystemExit("no common foreground pixels")

    iou = (np.logical_and(blender_mask, cpu_mask).sum() /
           union.sum()) if union.any() else 1.0
    print(f"effect masks at {args.threshold}/255: Blender={blender_mask.sum()}, "
          f"QPainter={cpu_mask.sum()}, IoU={iou:.6f}")
    print("GPU vs QPainter factor on common baseline foreground: "
          f"MAE={compared.mean():.6f}, p95={np.percentile(compared, 95):.6f}, "
          f"max={compared.max():.6f}, GPU mean={gpu_factor[common_foreground].mean():.6f}, "
          f"QPainter mean={cpu_factor[common_foreground].mean():.6f}")
    ys, xs = np.nonzero(union)
    ranked = sorted(zip(mismatch[ys, xs], ys, xs), reverse=True)[:10]
    print("largest output-delta mismatches (x,y, RGB error, GPU factor, "
          "QPainter factor, factor error):")
    for error, y, x in ranked:
        print(f"  ({x},{y}) {error}/255 "
              f"{gpu_factor[y, x]:.6f} {cpu_factor[y, x]:.6f} "
              f"{factor_delta[y, x]:.6f}")


if __name__ == "__main__":
    main()
