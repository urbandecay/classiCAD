"""Compare classiCAD's bundled AgX LUT with Blender 5.2.2's OCIO processor.

Run with:
  blender --background --python tests/verify_agx_lut.py
"""

import math
import pathlib
import struct

import PyOpenColorIO as ocio


ROOT = pathlib.Path(__file__).resolve().parents[1]
LUT_PATH = ROOT / "resources/blender/colormanagement/agx_srgb_default_97.bin"
SIZE = 97
DOMAIN_MAX = 16.0
SHAPER_SCALE = 64.0

raw = LUT_PATH.read_bytes()
values = struct.unpack("<%df" % (len(raw) // 4), raw)
assert len(values) == SIZE ** 3 * 3

config = ocio.GetCurrentConfig()
display = "sRGB"
view = "AgX"
view_name = next(
    (name for name in config.getViews(display)
     if config.getDisplayViewTransformName(display, name) == view), view
)
transform = ocio.DisplayViewTransform()
transform.setSrc(config.getRoleColorSpace(ocio.ROLE_SCENE_LINEAR))
transform.setDisplay(display)
transform.setView(view_name)
processor = config.getProcessor(transform).getDefaultCPUProcessor()


def sample(red, green, blue):
    def coordinate(value):
        bounded = min(max(value, 0.0), DOMAIN_MAX)
        return (math.log2(1.0 + SHAPER_SCALE * bounded) /
                math.log2(1.0 + SHAPER_SCALE * DOMAIN_MAX) * (SIZE - 1))

    coordinates = [coordinate(red), coordinate(green), coordinate(blue)]
    lower = [int(value) for value in coordinates]
    upper = [min(index + 1, SIZE - 1) for index in lower]
    fractions = [value - index for value, index in zip(coordinates, lower)]

    def texel(r, g, b):
        offset = ((b * SIZE + g) * SIZE + r) * 3
        return values[offset:offset + 3]

    def lerp(a, b, t):
        return tuple(x * (1.0 - t) + y * t for x, y in zip(a, b))

    z0, z1 = lower[2], upper[2]
    y0, y1 = lower[1], upper[1]
    x0, x1 = lower[0], upper[0]
    tx, ty, tz = fractions
    low = lerp(lerp(texel(x0, y0, z0), texel(x1, y0, z0), tx),
               lerp(texel(x0, y1, z0), texel(x1, y1, z0), tx), ty)
    high = lerp(lerp(texel(x0, y0, z1), texel(x1, y0, z1), tx),
                lerp(texel(x0, y1, z1), texel(x1, y1, z1), tx), ty)
    return lerp(low, high, tz)


samples = (
    (0.18, 0.18, 0.18),
    (0.18, 0.09, 0.045),
    (0.5, 0.5, 0.5),
    (1.0, 0.5, 0.25),
    (2.88, 2.88, 2.88),
    (8.0, 4.0, 2.0),
    (16.0, 16.0, 16.0),
)
maximum_error = 0.0
for color in samples:
    expected = processor.applyRGB(list(color))
    actual = sample(*color)
    error = max(abs(a - b) for a, b in zip(actual, expected))
    maximum_error = max(maximum_error, error)
    print("input=%s max_abs_error=%.9g" % (color, error))

assert maximum_error <= 0.001, maximum_error
print("AgX LUT comparison passed; maximum error %.9g" % maximum_error)
