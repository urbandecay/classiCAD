"""Focused Blender 5.2.2 contract for Workbench's active texture node."""

import importlib.util
import binascii
import base64
import pathlib
import struct
import sys
import tempfile
import zlib

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
expected = processor.applyRGB(expected)
assert max(abs(converted[index] - expected[index]) for index in range(3)) < 1.0e-5
assert max(abs(converted[index] - source_pixels[index]) for index in range(3)) > 1.0e-3

# Nonlinear OCIO source spaces use Blender's active processor before sampling.
adobe_image = bpy.data.images.new("AdobeRGBTextureContract", width=1, height=1)
adobe_image.colorspace_settings.name = "Gamma 2.2 Encoded AdobeRGB"
adobe_image.alpha_mode = "PREMUL"
adobe_image.pixels[:] = [0.18, 0.42, 0.07, 0.25]
adobe = extractor.bake_image_source_transform(adobe_image, "contract-adobe-rgb")
assert not adobe["error"], adobe
assert adobe["id"] != "identity" and len(adobe["id"]) == 64, adobe
adobe_texel = struct.unpack("<4f", base64.b64decode(adobe["payload"]))
adobe_source = adobe_image.pixels[:]
adobe_processor = config.getProcessor(
    adobe_image.colorspace_settings.name, scene_linear
).getDefaultCPUProcessor()
adobe_expected = adobe_processor.applyRGB(list(adobe_source[:3]))
adobe_expected = [component * adobe_source[3] for component in adobe_expected]
assert max(abs(actual - expected) for actual, expected in
           zip(adobe_texel, (*adobe_expected, adobe_source[3]))) < 1.0e-5
assert max(abs(adobe_texel[index] - adobe_source[index]) for index in range(3)) > 1.0e-3
assert adobe["id"] != aces["id"]
# This configured source includes a nonlinear transfer curve, so a matrix-only
# approximation would not reproduce its processor output.
adobe_basis = [adobe_processor.applyRGB(axis[:]) for axis in
               ([1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0])]
adobe_probe = [0.3, 0.7, 0.2]
adobe_actual_probe = adobe_processor.applyRGB(adobe_probe[:])
adobe_matrix_probe = [sum(adobe_probe[column] * adobe_basis[column][row]
                          for column in range(3)) for row in range(3)]
assert max(abs(adobe_actual_probe[row] - adobe_matrix_probe[row])
           for row in range(3)) > 1.0e-3

# Tiled byte images use the same arbitrary OCIO processor per tile.
def png_chunk(kind, payload):
    return (struct.pack(">I", len(payload)) + kind + payload +
            struct.pack(">I", binascii.crc32(kind + payload) & 0xffffffff))


def one_pixel_png(red, green, blue, alpha):
    header = struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", header) +
            png_chunk(b"IDAT", zlib.compress(bytes((0, red, green, blue, alpha)))) +
            png_chunk(b"IEND", b""))


with tempfile.TemporaryDirectory(prefix="classiCAD_ocio_udim_contract_") as directory:
    template = pathlib.Path(directory) / "source.<UDIM>.png"
    tile_sources = {
        1001: one_pixel_png(170, 90, 30, 128),
        1002: one_pixel_png(35, 140, 225, 200),
    }
    for number, encoded_png in tile_sources.items():
        pathlib.Path(str(template).replace("<UDIM>", str(number))).write_bytes(encoded_png)
    first_tile_path = str(template).replace("<UDIM>", "1001")
    tiled_image = bpy.data.images.load(first_tile_path, check_existing=False)
    tiled_image.source = "TILED"
    tiled_image.tiles.new(1002)
    tiled_image.filepath = str(template)
    tiled_image.filepath_raw = str(template)
    tiled_image.colorspace_settings.name = "Gamma 2.2 Encoded AdobeRGB"
    tiled_image.alpha_mode = "PREMUL"
    tiled_material = bpy.data.materials.new("NonlinearOCIOUDIMContract")
    tiled_material.use_nodes = True
    tiled_material.node_tree.nodes.clear()
    tiled_node = tiled_material.node_tree.nodes.new("ShaderNodeTexImage")
    tiled_node.image = tiled_image
    tiled_node.show_texture = True
    tiled_references = extractor.image_references(tiled_material)
    assert len(tiled_references) == 1, tiled_references
    tiled_reference = tiled_references[0]
    assert len(tiled_reference["tiles"]) == 2, tiled_reference
    assert [tile["number"] for tile in tiled_reference["tiles"]] == [1001, 1002]
    assert all(tile["source_transform_id"] != "identity" and
               tile["scene_linear_float_width"] == 1 and
               tile["scene_linear_float_height"] == 1 and
               tile["scene_linear_float_data_base64"]
               for tile in tiled_reference["tiles"]), tiled_reference
    for tile in tiled_reference["tiles"]:
        source_path = tile["filepath"]
        source_image = bpy.data.images.load(source_path, check_existing=False)
        source_image.colorspace_settings.name = tiled_image.colorspace_settings.name
        source_image.alpha_mode = tiled_image.alpha_mode
        expected_payload = extractor.bake_image_source_transform(
            source_image, "verify-tile-%d" % tile["number"])
        actual = struct.unpack(
            "<4f", base64.b64decode(tile["scene_linear_float_data_base64"]))
        expected = struct.unpack("<4f", base64.b64decode(expected_payload["payload"]))
        assert max(abs(a - b) for a, b in zip(actual, expected)) < 1.0e-6
        assert tile["source_transform_id"] == expected_payload["id"]
        bpy.data.images.remove(source_image)

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
