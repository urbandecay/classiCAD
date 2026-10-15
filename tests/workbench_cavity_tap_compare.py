#!/usr/bin/env python3
"""Compare selected QPainter cavity taps with diagnostic GPU tap maps."""

import argparse
import csv

import numpy as np


def read_pfm(path, channels):
    with open(path, "rb") as source:
        magic = source.readline().strip()
        dimensions = source.readline().split()
        scale = float(source.readline())
        expected_magic = b"Pf" if channels == 1 else b"PF"
        if magic != expected_magic or len(dimensions) != 2:
            raise ValueError(f"{path}: unexpected PFM format")
        width, height = map(int, dimensions)
        payload = source.read()
    if len(payload) != width * height * channels * 4:
        raise ValueError(f"{path}: invalid PFM payload")
    dtype = "<f4" if scale < 0 else ">f4"
    values = np.frombuffer(payload, dtype=dtype).astype(np.float32)
    shape = (height, width) if channels == 1 else (height, width, channels)
    values = values.reshape(shape)[::-1] * abs(scale)
    if not np.isfinite(values).all():
        raise ValueError(f"{path}: contains non-finite values")
    return values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("gpu_prefix", help="prefix used for GPU cavity captures")
    parser.add_argument("qpainter_taps_csv")
    args = parser.parse_args()

    with open(args.qpainter_taps_csv, newline="") as source:
        rows = list(csv.DictReader(source))
    if not rows:
        raise SystemExit("QPainter tap capture contains no receiver samples")

    width = height = None
    gpu_amounts = []
    gpu_sample_y = []
    for tap in range(16):
        amounts = read_pfm(
            f"{args.gpu_prefix}-tap-{tap:02}-amounts.pfm", 3)
        sample_y = read_pfm(
            f"{args.gpu_prefix}-tap-{tap:02}-factor.pfm", 1)
        if amounts.shape[:2] != sample_y.shape:
            raise SystemExit(f"GPU tap {tap}: channel dimensions differ")
        if width is None:
            height, width = sample_y.shape
        if sample_y.shape != (height, width):
            raise SystemExit(f"GPU tap {tap}: dimensions differ")
        gpu_amounts.append(amounts)
        gpu_sample_y.append(sample_y)

    coordinate_errors = []
    contribution_errors = []
    for row in rows:
        x = int(row["receiver_x"])
        y = int(row["receiver_y"])
        tap = int(row["tap"])
        if tap < 0 or tap >= len(gpu_amounts) or not (0 <= x < width and
                                                       0 <= y < height):
            raise SystemExit(f"invalid receiver/tap in CSV row: {row}")
        gpu = gpu_amounts[tap][y, x]
        gpu_sample_x = int(np.floor(gpu[2] * width))
        gpu_sample_y_pixel = height - 1 - int(np.floor(
            gpu_sample_y[tap][y, x] * height))
        cpu_sample_x = int(row["sample_x"])
        cpu_sample_y_pixel = int(row["sample_y"])
        coordinate_error = max(abs(gpu_sample_x - cpu_sample_x),
                               abs(gpu_sample_y_pixel - cpu_sample_y_pixel))
        coordinate_errors.append(coordinate_error)
        cpu_cavity = float(row["cavity_contribution"])
        cpu_edge = float(row["edge_contribution"])
        contribution_error = max(abs(float(gpu[0]) - cpu_cavity),
                                 abs(float(gpu[1]) - cpu_edge))
        contribution_errors.append(contribution_error)
        print(f"receiver=({x},{y}) tap={tap:02d} "
              f"GPU sample=({gpu_sample_x},{gpu_sample_y_pixel}) "
              f"QPainter sample=({cpu_sample_x},{cpu_sample_y_pixel}) "
              f"pixel_delta={coordinate_error} "
              f"GPU contributions=({gpu[0]:.7g},{gpu[1]:.7g}) "
              f"QPainter=({cpu_cavity:.7g},{cpu_edge:.7g}) "
              f"max_contribution_delta={contribution_error:.7g}")

    print("aggregate: exact sample-pixel matches="
          f"{sum(error == 0 for error in coordinate_errors)}/"
          f"{len(coordinate_errors)}, mean/max Chebyshev pixel error="
          f"{np.mean(coordinate_errors):.4f}/{max(coordinate_errors)}, "
          "contribution MAE/max="
          f"{np.mean(contribution_errors):.7g}/"
          f"{max(contribution_errors):.7g}")
    if max(coordinate_errors) > 1:
        raise SystemExit("GPU/QPainter tap coordinates differ by more than one pixel")


if __name__ == "__main__":
    main()
