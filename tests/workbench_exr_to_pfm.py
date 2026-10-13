"""Convert Workbench's Combined and Depth EXR subimages to PFM files.

Run with Blender's bundled OpenImageIO bindings:
  blender --background --python tests/workbench_exr_to_pfm.py -- input.exr color.pfm depth.pfm
"""

import argparse
import sys

import numpy as np
import OpenImageIO as oiio


def arguments():
    argv = sys.argv
    argv = argv[argv.index("--") + 1 :] if "--" in argv else []
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", help="Blender multilayer OpenEXR path")
    parser.add_argument("color_output", help="RGB Combined PFM path")
    parser.add_argument("depth_output", help="scalar view-space Depth PFM path")
    return parser.parse_args(argv)


def write_pfm(path, pixels):
    pixels = np.asarray(pixels, dtype="<f4")[::-1].copy()
    channels = 1 if pixels.ndim == 2 else pixels.shape[2]
    width = pixels.shape[1]
    height = pixels.shape[0]
    magic = "Pf" if channels == 1 else "PF"
    with open(path, "wb") as output:
        output.write(f"{magic}\n{width} {height}\n-1.0\n".encode("ascii"))
        pixels.tofile(output)


def convert_exr(input_path, color_output, depth_output):
    image = oiio.ImageInput.open(input_path)
    if image is None:
        raise RuntimeError(f"Could not open EXR: {input_path}")

    color_spec = image.spec()
    color_pixels = image.read_image(format=oiio.FLOAT)
    color_channels = color_spec.channelnames
    color_indices = [
        next(i for i, name in enumerate(color_channels) if name.endswith("." + channel))
        for channel in ("R", "G", "B")
    ]
    write_pfm(color_output, color_pixels[:, :, color_indices])

    if not image.seek_subimage(1, 0):
        image.close()
        raise RuntimeError("EXR has no second subimage for Workbench Depth")
    depth_pixels = image.read_image(format=oiio.FLOAT)
    if depth_pixels.ndim == 3:
        depth_pixels = depth_pixels[:, :, 0]
    write_pfm(depth_output, depth_pixels)
    image.close()


def main():
    args = arguments()
    convert_exr(args.input, args.color_output, args.depth_output)


if __name__ == "__main__":
    main()
