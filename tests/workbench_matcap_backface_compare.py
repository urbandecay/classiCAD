#!/usr/bin/env python3
"""Compare front- and back-facing MatCap probes in paired RGB PFM captures."""

import argparse
import json
import statistics
import sys

from workbench_pfm_compare import read_rgb_pfm


def patch_median(capture, center_x, center_y, radius=15):
    width, height, pixels = capture
    channels = [[], [], []]
    for top_y in range(center_y - radius, center_y + radius + 1):
        pfm_y = height - 1 - top_y
        for x in range(center_x - radius, center_x + radius + 1):
            base = (pfm_y * width + x) * 3
            for channel in range(3):
                channels[channel].append(pixels[base + channel])
    return tuple(statistics.median(channel) for channel in channels)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", help="Blender 5.2.2 RGB PFM capture")
    parser.add_argument("candidate", help="classiCAD RGB PFM capture")
    args = parser.parse_args()

    reference = read_rgb_pfm(args.reference)
    candidate = read_rgb_pfm(args.candidate)
    if reference[:2] != candidate[:2] or reference[:2] != (640, 480):
        parser.error("reference and candidate must both be 640x480 RGB PFMs")

    probes = {}
    success = True
    for name, x in (("front_wound", 190), ("reversed_wound", 450)):
        expected = patch_median(reference, x, 240)
        actual = patch_median(candidate, x, 240)
        errors = tuple(abs(a - b) for a, b in zip(expected, actual))
        probes[name] = {
            "reference_rgb": expected,
            "candidate_rgb": actual,
            "absolute_error_rgb": errors,
            "max_absolute_error": max(errors),
        }
        success &= max(errors) <= 0.002

    reference_separation = abs(
        probes["front_wound"]["reference_rgb"][1]
        - probes["reversed_wound"]["reference_rgb"][1]
    )
    candidate_separation = abs(
        probes["front_wound"]["candidate_rgb"][1]
        - probes["reversed_wound"]["candidate_rgb"][1]
    )
    success &= reference_separation >= 0.3 and candidate_separation >= 0.3
    print(json.dumps({
        "probes": probes,
        "green_channel_separation": {
            "reference": reference_separation,
            "candidate": candidate_separation,
        },
        "passed": bool(success),
    }, indent=2))
    return 0 if success else 1


if __name__ == "__main__":
    sys.exit(main())
