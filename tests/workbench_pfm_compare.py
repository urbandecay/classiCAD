#!/usr/bin/env python3
"""Compare two RGB PFM captures and report color and foreground-mask errors.

Foreground is defined as a pixel whose largest RGB component is greater than
the supplied threshold in either image. PFM rows are bottom-up by definition;
the tool preserves that order for both images. Use --candidate-top-down for
QPainter fragment captures serialized in Qt's top-down row order.
"""

import argparse
import array
from collections import deque
import json
import math
import os
import statistics
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


def compare(reference, candidate, threshold, edge_threshold=0.01):
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

    report = {
        "dimensions": {"width": ref_width, "height": ref_height},
        "threshold": threshold,
        "edge_coverage_threshold": edge_threshold,
        "color_error": metrics,
        "foreground_mask": {
            "reference_pixels": ref_foreground,
            "candidate_pixels": candidate_foreground,
            "xor_pixels": xor,
            "iou": intersection / union if union else 1.0,
        },
    }
    profile = compare_edge_coverage(reference, candidate, edge_threshold)
    if profile is not None:
        report["edge_coverage"] = profile
    else:
        report["edge_coverage"] = {"available": False,
                                   "reason": "captures need stable foreground and background regions"}
    return report


def flip_rows(capture):
    width, height, pixels = capture
    row_values = width * 3
    flipped = array.array("f")
    for row in range(height - 1, -1, -1):
        start = row * row_values
        flipped.extend(pixels[start : start + row_values])
    return width, height, flipped


def corner_background(width, height, pixels):
    corner_indices = (0, width - 1, (height - 1) * width,
                      height * width - 1)
    return tuple(statistics.median(
        pixels[index * 3 + channel] for index in corner_indices)
        for channel in range(3))


def foreground_mask(width, height, pixels, background, threshold):
    mask = [False] * (width * height)
    for pixel in range(width * height):
        base = pixel * 3
        distance_squared = sum(
            (pixels[base + channel] - background[channel]) ** 2
            for channel in range(3))
        mask[pixel] = distance_squared > threshold * threshold
    return mask


def nearest_seed_map(seed_mask, width, height):
    """Return 8-neighbor distance and owner maps from all true seed pixels."""
    count = width * height
    distance = [-1] * count
    owner = [-1] * count
    queue = deque()
    for index, is_seed in enumerate(seed_mask):
        if is_seed:
            distance[index] = 0
            owner[index] = index
            queue.append(index)

    while queue:
        index = queue.popleft()
        x = index % width
        y = index // width
        next_distance = distance[index] + 1
        for dy in (-1, 0, 1):
            ny = y + dy
            if ny < 0 or ny >= height:
                continue
            for dx in (-1, 0, 1):
                if dx == 0 and dy == 0:
                    continue
                nx = x + dx
                if nx < 0 or nx >= width:
                    continue
                neighbor = ny * width + nx
                if distance[neighbor] >= 0:
                    continue
                distance[neighbor] = next_distance
                owner[neighbor] = owner[index]
                queue.append(neighbor)
    return distance, owner


def edge_coverage(reference, candidate, edge_threshold, band_width=2):
    """Estimate fractional silhouette coverage in a local flat-color model.

    PFM has no object ID or depth attachment, so this estimates coverage by
    projecting each edge RGB sample between its nearest stable background and
    foreground samples. Foreground is Euclidean RGB distance from the median
    corner background greater than ``edge_threshold``. Corners must contain
    background, and both stable interior regions must remain wider than the
    band. The 8-neighbor distance is a Chebyshev pixel-step distance. This is
    intended for matched, hard-edged fixtures with locally stable face colors;
    the existing mask and RGB metrics remain authoritative summaries.
    """
    ref_width, ref_height, ref_pixels = reference
    cand_width, cand_height, cand_pixels = candidate
    if (ref_width, ref_height) != (cand_width, cand_height):
        raise ValueError("dimension mismatch in edge coverage comparison")
    width, height = ref_width, ref_height
    count = width * height
    if count < 9:
        return None

    ref_background = corner_background(width, height, ref_pixels)
    cand_background = corner_background(width, height, cand_pixels)
    ref_mask = foreground_mask(width, height, ref_pixels,
                               ref_background, edge_threshold)
    cand_mask = foreground_mask(width, height, cand_pixels,
                                cand_background, edge_threshold)
    if not any(ref_mask) or all(ref_mask) or not any(cand_mask) or all(cand_mask):
        return None

    def coverage_context(pixels, mask):
        background_distance, _ = nearest_seed_map(
            [not value for value in mask], width, height)
        foreground_distance, _ = nearest_seed_map(mask, width, height)
        stable_foreground = [mask[index] and background_distance[index] > band_width
                             for index in range(count)]
        stable_background = [not mask[index] and foreground_distance[index] > band_width
                             for index in range(count)]
        _, foreground_owner = nearest_seed_map(stable_foreground, width, height)
        _, background_owner = nearest_seed_map(stable_background, width, height)
        if not any(stable_foreground) or not any(stable_background):
            return None

        coverage = [0.0] * count
        for index in range(count):
            foreground_index = foreground_owner[index]
            background_index = background_owner[index]
            if foreground_index < 0 or background_index < 0:
                return None
            base = index * 3
            foreground_base = foreground_index * 3
            background_base = background_index * 3
            direction = [pixels[foreground_base + channel] -
                         pixels[background_base + channel] for channel in range(3)]
            denominator = sum(value * value for value in direction)
            if denominator <= 1.0e-20:
                return None
            numerator = sum(
                (pixels[base + channel] - pixels[background_base + channel]) *
                direction[channel] for channel in range(3))
            coverage[index] = min(1.0, max(0.0, numerator / denominator))
        return coverage, background_distance, foreground_distance

    ref_context = coverage_context(ref_pixels, ref_mask)
    cand_context = coverage_context(cand_pixels, cand_mask)
    if ref_context is None or cand_context is None:
        return None
    ref_coverage, ref_to_background, ref_to_foreground = ref_context
    cand_coverage, cand_to_background, cand_to_foreground = cand_context

    band_indices = []
    for index in range(count):
        ref_edge_distance = (ref_to_background[index] if ref_mask[index]
                             else ref_to_foreground[index])
        cand_edge_distance = (cand_to_background[index] if cand_mask[index]
                              else cand_to_foreground[index])
        if min(ref_edge_distance, cand_edge_distance) <= band_width:
            band_indices.append(index)
    if not band_indices:
        return None

    errors = [abs(ref_coverage[index] - cand_coverage[index])
              for index in band_indices]
    sorted_errors = sorted(errors)
    p90_index = min(len(sorted_errors) - 1,
                    math.ceil(0.90 * len(sorted_errors)) - 1)
    partial_low = 0.05
    partial_high = 0.95
    ref_partial = sum(partial_low < ref_coverage[index] < partial_high
                      for index in band_indices)
    cand_partial = sum(partial_low < cand_coverage[index] < partial_high
                       for index in band_indices)
    return {
        "available": True,
        "method": "nearest stable RGB projection; 8-neighbor Chebyshev distance",
        "threshold_rgb_distance": edge_threshold,
        "band_width_pixels": band_width,
        "band_pixels": len(band_indices),
        "coverage_mae": sum(errors) / len(errors),
        "coverage_p90_abs_error": sorted_errors[p90_index],
        "coverage_bias": sum(cand_coverage[index] - ref_coverage[index]
                              for index in band_indices) / len(band_indices),
        "reference_partial_coverage_fraction": ref_partial / len(band_indices),
        "candidate_partial_coverage_fraction": cand_partial / len(band_indices),
    }


def compare_edge_coverage(reference, candidate, edge_threshold, band_width=2):
    return edge_coverage(reference, candidate, edge_threshold, band_width)


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
        "--edge-threshold",
        type=float,
        default=0.01,
        help=("edge-profile threshold on Euclidean RGB distance from each "
              "image's median corner background (default: 0.01)"),
    )
    parser.add_argument(
        "--candidate-top-down",
        action="store_true",
        help="flip candidate rows from Qt top-down order into PFM bottom-up order",
    )
    parser.add_argument(
        "--json-output",
        metavar="PATH",
        help="also write the concise report as JSON to PATH",
    )
    args = parser.parse_args(argv)
    if not math.isfinite(args.threshold) or args.threshold < 0.0:
        parser.error("--threshold must be finite and nonnegative")
    if not math.isfinite(args.edge_threshold) or args.edge_threshold < 0.0:
        parser.error("--edge-threshold must be finite and nonnegative")
    return args


def main(argv=None):
    args = parse_args(argv)
    try:
        candidate = read_rgb_pfm(args.candidate)
        if args.candidate_top_down:
            candidate = flip_rows(candidate)
        report = compare(
            read_rgb_pfm(args.reference),
            candidate,
            args.threshold,
            args.edge_threshold,
        )
        if args.candidate_top_down:
            report["candidate_row_order"] = "top-down; flipped to PFM bottom-up"
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
