"""Focused Blender 5.2.2 contract for Workbench's active texture node."""

import importlib.util
import base64
import pathlib
import struct
import sys

import bpy


root = pathlib.Path(__file__).resolve().parents[1]
extractor_path = root / "src/core/serialization/blender_scene_extract.py"
spec = importlib.util.spec_from_file_location("classicad_scene_extract", extractor_path)
extractor = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = extractor
spec.loader.exec_module(extractor)

material = bpy.data.materials.new("ActiveTextureContract")
material.use_nodes = True
nodes = material.node_tree.nodes
nodes.clear()
image_node = nodes.new("ShaderNodeTexImage")
image_node.image = bpy.data.images.new("WorkbenchActiveImage", width=2, height=2)
image_node.show_texture = True
ordinary_active_node = nodes.new("ShaderNodeValue")
nodes.active = ordinary_active_node

references = extractor.image_references(material)
assert len(references) == 1, references
assert references[0]["image_name"] == "WorkbenchActiveImage", references
assert nodes.active == ordinary_active_node

# Byte ACEScg conversion is performed per texel and alpha is preserved.
aces_image = bpy.data.images.new("ACEScgTextureContract", width=2, height=1)
aces_image.colorspace_settings.name = "ACEScg"
aces_image.pixels[:] = [0.18, 0.42, 0.07, 0.25, 0.8, 0.1, 0.3, 0.75]
aces = extractor.bake_image_source_transform(aces_image, "contract-aces")
assert not aces["error"], aces
assert aces["id"] != "identity" and len(aces["id"]) == 64, aces
converted = struct.unpack("<8f", base64.b64decode(aces["payload"]))
source_pixels = aces_image.pixels[:]
assert converted[3] == source_pixels[3] and converted[7] == source_pixels[7], converted
ocio = __import__("PyOpenColorIO")
config = ocio.GetCurrentConfig()
scene_linear = config.getRoleColorSpace(ocio.ROLE_SCENE_LINEAR)
processor = config.getProcessor("ACEScg", scene_linear).getDefaultCPUProcessor()
expected = list(source_pixels[:3])
processor.applyRGB(expected)
assert max(abs(converted[index] - expected[index]) for index in range(3)) < 1.0e-5

# Blender's float-texture path directly copies float buffers, including those
# labeled sRGB, and still applies PREMUL without changing alpha.
float_image = bpy.data.images.new("FloatTextureContract", width=1, height=1,
                                  float_buffer=True)
float_image.colorspace_settings.name = "sRGB"
float_image.alpha_mode = "PREMUL"
float_image.pixels[:] = [1.5, -0.25, 0.5, 0.4]
float_payload = extractor.bake_image_source_transform(float_image,
                                                       "contract-float")
assert float_payload["id"] == "blender-float-buffer", float_payload
float_texel = struct.unpack("<4f", base64.b64decode(float_payload["payload"]))
assert max(abs(actual - expected) for actual, expected in
           zip(float_texel, (0.6, -0.1, 0.2, 0.4))) < 1.0e-6, float_texel

srgb_byte = bpy.data.images.new("SrgbByteTextureContract", width=1, height=1)
srgb_byte.colorspace_settings.name = "sRGB"
srgb_result = extractor.bake_image_source_transform(srgb_byte, "contract-srgb")
assert srgb_result["id"] == "identity" and not srgb_result["payload"], srgb_result
unsupported = extractor.bake_image_source_transform(
    type("UnsupportedByteImage", (), {
        "colorspace_settings": type("ColorSpace", (), {"name": "Unsupported RGB"})(),
        "is_float": False,
    })(), "contract-unsupported")
assert unsupported["error"] and not unsupported["payload"], unsupported

print("active Workbench texture node contract passed")
