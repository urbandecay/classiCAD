"""Compare matched Blender/classiCAD polygon-cube captures.

Capture both at 640x480 with the same camera direction and orthographic scale.
Face-interior probes cover all three camera-visible cube faces; full-frame
blank checks catch failed captures before any comparison is reported.
"""

import argparse

import numpy as np
from PIL import Image


PROBE_GROUPS = {
    "top": [(x, y) for y in (80, 120, 160) for x in (260, 320, 380)],
    "front": [(x, y) for y in (220, 260, 330) for x in (170, 230, 290)],
    "right": [(x, y) for y in (240, 275, 330) for x in (400, 420, 460)],
}
PROBES = [point for group in PROBE_GROUPS.values() for point in group]


def read_pfm(path):
    with open(path, "rb") as source:
        magic = source.readline().strip()
        width, height = map(int, source.readline().split())
        scale = float(source.readline())
        channels = 3 if magic == b"PF" else 1
        dtype = "<f4" if scale < 0 else ">f4"
        values = np.fromfile(source, dtype=dtype)
    expected = width * height * channels
    if magic != b"PF" or values.size != expected:
        raise ValueError(f"invalid RGB PFM: {path}")
    image = values.reshape(height, width, channels)[::-1]
    if image.shape[:2] != (480, 640):
        raise ValueError(f"expected 640x480 linear capture: {path}")
    if not np.isfinite(image).all() or float(image.max()) <= 1e-6:
        raise ValueError(f"blank or invalid scene-linear capture: {path}")
    return image


def read_display(path):
    image = np.asarray(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0
    if image.shape[:2] != (480, 640):
        raise ValueError(f"expected 640x480 display capture: {path}")
    if float(image.max()) <= 1.0 / 255.0:
        raise ValueError(f"blank display capture: {path}")
    return image


def probes(image):
    return np.stack([image[y, x] for x, y in PROBES])


def report_face_errors(reference, candidate, label):
    offset = 0
    for face, points in PROBE_GROUPS.items():
        count = len(points)
        error = np.abs(reference[offset:offset + count] -
                       candidate[offset:offset + count])
        scale = 255.0 if label == "display" else 1.0
        suffix = "/255" if label == "display" else ""
        print(f"{label} {face}: MAE={error.mean() * scale:.6f}{suffix}, "
              f"max={error.max() * scale:.6f}{suffix}")
        offset += count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("blender_display")
    parser.add_argument("classicad_display")
    parser.add_argument("blender_linear")
    parser.add_argument("classicad_linear")
    parser.add_argument("--max-linear-error", type=float, default=0.015)
    parser.add_argument("--max-display-error", type=float, default=3.0 / 255.0)
    args = parser.parse_args()

    blender_linear = probes(read_pfm(args.blender_linear))
    classicad_linear = probes(read_pfm(args.classicad_linear))
    blender_display = probes(read_display(args.blender_display))
    classicad_display = probes(read_display(args.classicad_display))
    linear_error = np.abs(blender_linear - classicad_linear)
    display_error = np.abs(blender_display - classicad_display)
    print(f"nonblank matched captures; face-interior probes={len(PROBES)}")
    print(f"scene-linear RGB: MAE={linear_error.mean():.7f}, max={linear_error.max():.7f}")
    print(f"display RGB: MAE={display_error.mean() * 255.0:.4f}/255, "
          f"max={display_error.max() * 255.0:.4f}/255")
    report_face_errors(blender_linear, classicad_linear, "scene-linear")
    report_face_errors(blender_display, classicad_display, "display")
    if (linear_error.max() > args.max_linear_error or
            display_error.max() > args.max_display_error):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
