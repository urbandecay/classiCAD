#!/usr/bin/env python3
"""Generate compressed RGB half-float MatCaps from Blender 5.2.2 EXRs."""

from __future__ import annotations

import argparse
import struct
import zlib
from pathlib import Path

import OpenImageIO as oiio
import numpy as np


MAGIC = b"CADMC01\0"
EXPECTED_PRESETS = {
    "basic_bright", "basic_dark", "basic_grey", "basic_side",
    "brush_thumbnail_preview", "ceramic_dark", "ceramic_lightbulb",
    "check_gradient", "check_normal+y", "check_reflection_horizontal",
    "check_reflection_vertical", "check_rim_dark", "check_rim_light",
    "clay_brown", "clay_green", "clay_studio", "clay_warm", "fullmetal",
    "hard_surface_grey", "hard_surface_red", "metal_bronze",
    "metal_carpaint", "pearl", "red_wax", "resin", "toon_dark",
    "toon_light",
}


def read_layer(path: Path) -> tuple[int, int, bytes, bytes]:
    image = oiio.ImageInput.open(str(path))
    if image is None:
        raise RuntimeError(f"cannot open {path}: {oiio.geterror()}")
    try:
        spec = image.spec()
        channels = tuple(spec.channelnames)
        pixels = np.asarray(image.read_image(format=oiio.HALF), dtype="<f2")
        if pixels.shape != (spec.height, spec.width, spec.nchannels):
            raise RuntimeError(f"unexpected pixel shape in {path}: {pixels.shape}")

        if all(f"diffuse.{channel}" in channels for channel in "RGB"):
            diffuse_prefix = "diffuse."
        elif all(channel in channels for channel in "RGB") and not any(
            name.startswith("diffuse.") or name.startswith("specular.")
            for name in channels
        ):
            # Blender's check_normal+y source is a single unlayered RGB image.
            diffuse_prefix = ""
        else:
            raise RuntimeError(f"missing diffuse RGB channels in {path}: {channels}")

        def rgb(prefix: str) -> bytes:
            indices = [channels.index(prefix + channel) for channel in "RGB"]
            return np.ascontiguousarray(pixels[:, :, indices], dtype="<f2").tobytes()

        diffuse = rgb(diffuse_prefix)
        if all(f"specular.{channel}" in channels for channel in "RGB"):
            specular = rgb("specular.")
        else:
            specular = bytes(spec.width * spec.height * 3 * 2)
        return spec.width, spec.height, diffuse, specular
    finally:
        image.close()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--blender-data",
        type=Path,
        default=Path("/snap/blender/7803/5.2/datafiles/studiolights/matcap"),
        help="Blender 5.2.2 matcap EXR directory",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path(__file__).resolve().parent,
        help="destination directory for .qmc files",
    )
    args = parser.parse_args()

    sources = {path.stem: path for path in args.blender_data.glob("*.exr")}
    if set(sources) != EXPECTED_PRESETS:
        missing = sorted(EXPECTED_PRESETS - set(sources))
        unexpected = sorted(set(sources) - EXPECTED_PRESETS)
        raise SystemExit(f"EXR preset mismatch; missing={missing}, unexpected={unexpected}")

    args.output.mkdir(parents=True, exist_ok=True)
    expected_payload_bytes = None
    for name in sorted(EXPECTED_PRESETS):
        width, height, diffuse, specular = read_layer(sources[name])
        if width != 512 or height != 512:
            raise RuntimeError(f"unexpected dimensions for {name}: {width}x{height}")
        payload = diffuse + specular
        if expected_payload_bytes is None:
            expected_payload_bytes = len(payload)
        if len(payload) != expected_payload_bytes:
            raise RuntimeError(f"invalid half-float payload size for {name}")
        encoded = (
            MAGIC
            + struct.pack("<HH", width, height)
            + struct.pack(">I", len(payload))
            + zlib.compress(payload, level=9)
        )
        # Validate the exact qCompress-compatible framing consumed by Qt.
        if zlib.decompress(encoded[16:]) != payload:
            raise RuntimeError(f"compressed payload validation failed for {name}")
        (args.output / f"{name}.qmc").write_bytes(encoded)

    outputs = sorted(args.output.glob("*.qmc"))
    if {path.stem for path in outputs} != EXPECTED_PRESETS:
        raise RuntimeError("output directory does not contain exactly the 27 expected presets")
    print(f"Generated and validated {len(outputs)} MatCaps in {args.output}")


if __name__ == "__main__":
    main()
