"""Write Blender's extracted scene-linear RGBA32F texel for a source image.

Usage: blender --background --python tests/workbench_texture_scene_linear_fixture.py -- input.png output.rgba32f
"""

import base64
import pathlib
import struct
import sys

import bpy


argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
if len(argv) != 2:
    raise SystemExit(__doc__)

root = pathlib.Path(__file__).resolve().parents[1]
extractor_path = root / "src/core/serialization/blender_scene_extract.py"
import importlib.util

spec = importlib.util.spec_from_file_location("classicad_scene_extract", extractor_path)
extractor = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = extractor
spec.loader.exec_module(extractor)
extractor.require_pinned_blender()

image = bpy.data.images.load(str(pathlib.Path(argv[0]).resolve()), check_existing=False)
image.colorspace_settings.name = "Gamma 2.2 Encoded AdobeRGB"
image.alpha_mode = "STRAIGHT"
result = extractor.bake_image_source_transform(image, "matched-adobe-texture-fixture")
if result["error"]:
    raise RuntimeError(result["error"])
if result["id"] == "identity" or result["width"] != 1 or result["height"] != 1:
    raise RuntimeError("expected one nonlinear OCIO-converted byte texel")
payload = base64.b64decode(result["payload"], validate=True)
if len(payload) != 16:
    raise RuntimeError("expected exactly one RGBA32F texel")
converted = struct.unpack("<4f", payload)
if max(abs(converted[index] - image.pixels[index]) for index in range(3)) < 1.0e-3:
    raise RuntimeError("Adobe RGB fixture did not exercise a nonlinear source conversion")
pathlib.Path(argv[1]).write_bytes(payload)
print("Blender 5.2.2 scene-linear texture fixture:", result["id"],
      tuple(image.pixels[:]), "->", converted)
