#!/usr/bin/env python3
"""Bake a Blender OCIO display/view transform for classiCAD's viewport."""

import argparse
import math
import struct

import PyOpenColorIO as ocio


LUT_SIZE = 65
SCENE_LINEAR_MAX = 2.0
SHAPER_SCALE = 16.0


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("config", help="Blender datafiles/colormanagement/config.ocio")
    parser.add_argument("output", help="Output little-endian RGB float LUT")
    parser.add_argument("--display", default="sRGB")
    parser.add_argument("--view", default="AgX")
    args = parser.parse_args()

    config = ocio.Config.CreateFromFile(args.config)
    processor = config.getProcessor(
        "Linear Rec.709", args.display, args.view, ocio.TRANSFORM_DIR_FORWARD
    ).getDefaultCPUProcessor()

    with open(args.output, "wb") as output:
        input_values = [
            (2.0 ** (index / (LUT_SIZE - 1) *
                     math.log2(1.0 + SHAPER_SCALE * SCENE_LINEAR_MAX)) - 1.0)
            / SHAPER_SCALE
            for index in range(LUT_SIZE)
        ]
        for b in input_values:
            for g in input_values:
                for r in input_values:
                    display_rgb = processor.applyRGB([r, g, b])
                    if not all(math.isfinite(channel) for channel in display_rgb):
                        raise RuntimeError("OCIO produced a non-finite LUT value")
                    output.write(struct.pack("<3f", *display_rgb))


if __name__ == "__main__":
    main()
