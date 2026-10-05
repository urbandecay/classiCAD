#!/usr/bin/env python3
"""Bake Blender's default AgX display transform for classiCAD's viewport."""

import argparse
import math
import struct

import PyOpenColorIO as ocio


LUT_SIZE = 65
SCENE_LINEAR_MAX = 2.0


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("config", help="Blender datafiles/colormanagement/config.ocio")
    parser.add_argument("output", help="Output little-endian RGB float LUT")
    args = parser.parse_args()

    config = ocio.Config.CreateFromFile(args.config)
    processor = config.getProcessor(
        "Linear Rec.709", "sRGB", "AgX", ocio.TRANSFORM_DIR_FORWARD
    ).getDefaultCPUProcessor()

    with open(args.output, "wb") as output:
        for blue in range(LUT_SIZE):
            b = SCENE_LINEAR_MAX * blue / (LUT_SIZE - 1)
            for green in range(LUT_SIZE):
                g = SCENE_LINEAR_MAX * green / (LUT_SIZE - 1)
                for red in range(LUT_SIZE):
                    r = SCENE_LINEAR_MAX * red / (LUT_SIZE - 1)
                    display_rgb = processor.applyRGB([r, g, b])
                    if not all(math.isfinite(channel) for channel in display_rgb):
                        raise RuntimeError("OCIO produced a non-finite LUT value")
                    output.write(struct.pack("<3f", *display_rgb))


if __name__ == "__main__":
    main()
