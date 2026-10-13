#!/usr/bin/env python3
"""Compare two RGB PFM captures and report color and foreground-mask errors.

Foreground is defined as a pixel whose largest RGB component is greater than
the supplied threshold in either image. PFM rows are bottom-up by definition;
the tool preserves that order for both images, so paired captures compare in
the same coordinate convention.
"""

import argparse
import array
import json
import math
import os
import struct
import sys


def read_line(stream, description):
    line = stream.readline()
    if not line:
        raise ValueError(f"missing {description}")
    return line.strip()


def read_rgb_pfm(path):
    with open(path, "rb") as source:
        magic = read_line(source, "PFM magic")
        if magic != b"PF":
            raise ValueError(f"{path}: expected RGB PFM magic 'PF', got {magic!r}")

        dimension_line = read_line(source, "PFM dimensions")
        while dimension_line.startswith(b"#"):
            dimension_line = read_line(source, "PFM dimensions")
        fields = dimension_line.split()
        if len(fields) != 2:
            raise ValueError(f"{path}: malformed PFM dimensions {dimension_line!r}")
        try:
            width, height = (int(field) for field in fields)
        except ValueError as error:
            raise ValueError(f"{path}: dimensions must be integers") from error
        if width <= 0 or height <= 0:
            raise ValueError(f"{path}: dimensions must be positive")

        scale_line = read_line(source, "PFM scale")
        try:
            scale = float(scale_line)
        except ValueError as error:
            raise ValueError(f"{path}: malformed PFM scale {scale_line!r}") from error
        if not math.isfinite(scale) or scale == 0.0:
            raise ValueError(f"{path}: PFM scale must be finite and nonzero")

        expected_values = width * height * 3
        payload = source.read()
        expected_bytes = expected_values * 4
        if len(payload) != expected_bytes:
            raise ValueError(
                f"{path}: expected {expected_bytes} pixel bytes, got {len(payload)}"
            )

    little_endian = scale < 0.0
    pixels = array.array("f")
    pixels.frombytes(payload)
    if pixels.itemsize != 4:
        raise RuntimeError("this Python platform does not use 32-bit array floats")
    host_little_endian = struct.pack("=I", 1)[0] == 1
    if little_endian != host_little_endian:
        pixels.byteswap()

    multiplier = abs(scale)
    if multiplier != 1.0:
        for index, value in enumerate(pixels):
            pixels[index] = value * multiplier
    if any(not math.isfinite(value) for value in pixels):
        raise ValueError(f"{path}: PFM contains non-finite RGB samples")
    return width, height, pixels


def compare(reference, candidate, threshold):
    ref_width, ref_height, ref_pixels = reference
    cand_width, cand_height, cand_pixels = candidate
    if (ref_width, ref_height) != (cand_width, cand_height):
        raise ValueError(
            "dimension mismatch: "
            f"reference is {ref_width}x{ref_height}, candidate is "
            f"{cand_width}x{cand_height}"
        )

    channel_abs = [0.0, 0.0, 0.0]
    channel_squared = [0.0, 0.0, 0.0]
    channel_max = [0.0, 0.0, 0.0]
    finite_count = [0, 0, 0]
    ref_foreground = 0
    candidate_foreground = 0
    intersection = 0
    union = 0
    xor = 0

    for pixel in range(ref_width * ref_height):
        base = pixel * 3
        ref_rgb = ref_pixels[base : base + 3]
        cand_rgb = cand_pixels[base : base + 3]
        for channel in range(3):
            ref_value = ref_rgb[channel]
            cand_value = cand_rgb[channel]
            if math.isfinite(ref_value) and math.isfinite(cand_value):
                error = abs(ref_value - cand_value)
                channel_abs[channel] += error
                channel_squared[channel] += error * error
                channel_max[channel] = max(channel_max[channel], error)
                finite_count[channel] += 1

        ref_mask = max(ref_rgb) > threshold
        candidate_mask = max(cand_rgb) > threshold
        ref_foreground += ref_mask
        candidate_foreground += candidate_mask
        intersection += ref_mask and candidate_mask
        union += ref_mask or candidate_mask
        xor += ref_mask != candidate_mask

    if any(count == 0 for count in finite_count):
        raise ValueError("captures contain no finite paired RGB samples")
    metrics = {}
    names = ("red", "green", "blue")
    for channel, name in enumerate(names):
        count = finite_count[channel]
        metrics[name] = {
            "mae": channel_abs[channel] / count,
            "rmse": math.sqrt(channel_squared[channel] / count),
            "max_error": channel_max[channel],
        }

    return {
        "dimensions": {"width": ref_width, "height": ref_height},
        "threshold": threshold,
        "color_error": metrics,
        "foreground_mask": {
            "reference_pixels": ref_foreground,
            "candidate_pixels": candidate_foreground,
            "xor_pixels": xor,
            "iou": intersection / union if union else 1.0,
        },
    }


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", help="reference RGB PFM capture")
    parser.add_argument("candidate", help="candidate RGB PFM capture")
    parser.add_argument(
        "--threshold",
        type=float,
        default=0.1,
        help="foreground threshold on the largest RGB component (default: 0.1)",
    )
    parser.add_argument(
        "--json-output",
        metavar="PATH",
        help="also write the concise report as JSON to PATH",
    )
    args = parser.parse_args(argv)
    if not math.isfinite(args.threshold):
        parser.error("--threshold must be finite")
    return args


def main(argv=None):
    args = parse_args(argv)
    try:
        report = compare(
            read_rgb_pfm(args.reference),
            read_rgb_pfm(args.candidate),
            args.threshold,
        )
        report["reference"] = os.path.basename(args.reference)
        report["candidate"] = os.path.basename(args.candidate)
        rendered = json.dumps(report, indent=2, sort_keys=True, allow_nan=False)
        print(rendered)
        if args.json_output:
            with open(args.json_output, "w", encoding="utf-8") as output:
                output.write(rendered + "\n")
    except (OSError, ValueError, RuntimeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
