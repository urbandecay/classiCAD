"""Verify Blender extraction and render fixtures for NURBS surface materials.

Set CLASSICAD_SURFACE_MATERIAL_BLEND_OUTPUT to save a paired Workbench capture;
the MULTI_SPAN and ALPHA_CUTOUT flags add those fixture variants.
Run with Blender 5.2.2: blender --background --python
tests/blender_surface_material_import_contract.py
"""

import base64
import importlib.util
import math
import os
import pathlib
import struct
import sys
import tempfile
import zlib

import bpy
from mathutils import Vector


root = pathlib.Path(__file__).resolve().parents[1]
extractor_path = root / "src/core/serialization/blender_scene_extract.py"
spec = importlib.util.spec_from_file_location("classiCAD_scene_extract", extractor_path)
extractor = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = extractor
spec.loader.exec_module(extractor)
extractor.SOURCE_PROJECT_NAMESPACE = "surface-material-import-contract"

bpy.ops.object.select_all(action="SELECT")
bpy.ops.object.delete(use_global=False)

alpha_cutout = os.environ.get("CLASSICAD_SURFACE_MATERIAL_ALPHA_CUTOUT") == "1"
if alpha_cutout:
    width, height = 8, 4
    rows = []
    for y in range(4):
        row = bytearray((0,))
        for x in range(8):
            row.extend((40 + x * 20, 70 + y * 20,
                        180 - x * 10, 0 if x < 4 else 255))
        rows.append(bytes(row))

    def png_chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff))

    png_bytes = (b"\x89PNG\r\n\x1a\n" +
                 png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height,
                                                   8, 6, 0, 0, 0)) +
                 png_chunk(b"IDAT", zlib.compress(b"".join(rows))) +
                 png_chunk(b"IEND", b""))
    temp_fd, temp_path = tempfile.mkstemp(suffix=".png")
    with os.fdopen(temp_fd, "wb") as png_file:
        png_file.write(png_bytes)
    image = bpy.data.images.load(temp_path, check_existing=False)
    image.name = "SurfacePackedTexture"
    image.alpha_mode = "STRAIGHT"
    image.pack()
    pathlib.Path(temp_path).unlink()
else:
    image = bpy.data.images.new("SurfacePackedTexture", width=2, height=2)
    image.pixels[:] = [
        0.2, 0.4, 0.6, 0.8,
        0.8, 0.6, 0.4, 1.0,
        0.1, 0.3, 0.5, 0.7,
        0.9, 0.7, 0.5, 0.3,
    ]
    image.alpha_mode = "PREMUL"
image.pack()

material = bpy.data.materials.new("SurfaceTextureMaterial")
material.use_nodes = True
material.diffuse_color = (0.3, 0.5, 0.7, 0.6)
material.roughness = 0.27
material.metallic = 0.41
nodes = material.node_tree.nodes
nodes.clear()
image_node = nodes.new("ShaderNodeTexImage")
image_node.image = image
image_node.show_texture = True
image_node.interpolation = "Closest"
image_node.extension = "EXTEND"

bpy.ops.surface.primitive_nurbs_surface_surface_add(
    enter_editmode=False, location=(0.0, 0.0, 0.0))
surface_object = bpy.context.object
surface_data = surface_object.data
surface_object.name = "SurfaceMaterialObject"
surface_data.name = "SurfaceMaterialPatch"
if os.environ.get("CLASSICAD_SURFACE_MATERIAL_MULTI_SPAN") == "1":
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.curve.select_all(action="SELECT")
    bpy.ops.curve.subdivide(number_cuts=1)
    bpy.ops.object.mode_set(mode="OBJECT")
spline = surface_data.splines[0]
assert spline.point_count_u >= spline.order_u
assert spline.point_count_v >= spline.order_v
multi_span = os.environ.get("CLASSICAD_SURFACE_MATERIAL_MULTI_SPAN") == "1"
if multi_span:
    assert spline.point_count_u > spline.order_u
    assert spline.point_count_v > spline.order_v
spline.material_index = 0
for index, point in enumerate(spline.points):
    point.co = (3.0 * (index % spline.point_count_u) /
                (spline.point_count_u - 1) - 1.5,
                3.0 * (index // spline.point_count_u) /
                (spline.point_count_v - 1) - 1.5, 0.0, 1.0)
surface_data.materials.append(material)

materials = {}
unsupported = []
patches = extractor.extract_surface_patches(
    surface_object, bpy.context.scene, materials, unsupported)
assert len(patches) == 1, patches
surface = patches[0]["surface"]
if multi_span:
    assert surface["control_vertex_count_u"] > surface["order_u"], surface
    assert surface["control_vertex_count_v"] > surface["order_v"], surface
    assert len(set(surface["knots_u"])) > 1, surface
    assert len(set(surface["knots_v"])) > 1, surface
    for axis in ("u", "v"):
        order = surface["order_" + axis]
        point_count = surface["control_vertex_count_" + axis]
        full_knots = extractor.blender_nurbs_knots(
            point_count, order, False, False, False)
        active_spans = sum(
            full_knots[index] < full_knots[index + 1]
            for index in range(order - 1, point_count))
        assert active_spans > 1, (axis, full_knots, active_spans)
    bpy.context.view_layer.update()
    evaluated = surface_object.evaluated_get(
        bpy.context.evaluated_depsgraph_get())
    evaluated_mesh = evaluated.to_mesh()
    try:
        uv_layer = evaluated_mesh.uv_layers.active
        assert uv_layer is not None
        for component in (0, 1):
            values = [loop.uv[component] for loop in uv_layer.data]
            assert math.isclose(min(values), 0.0, abs_tol=1.0e-6)
            assert math.isclose(max(values), 1.0, abs_tol=1.0e-6)
    finally:
        evaluated.to_mesh_clear()
material_id = surface["material_id"]
assert material_id in materials, material_id
record = materials[material_id]
assert record["name"] == "SurfaceTextureMaterial", record
for actual, expected in zip(record["diffuse_color_linear"], (0.3, 0.5, 0.7, 0.6)):
    assert math.isclose(actual, expected, rel_tol=0.0, abs_tol=1.0e-6), record
assert math.isclose(record["roughness"], 0.27, rel_tol=0.0, abs_tol=1.0e-6), record
assert math.isclose(record["metallic"], 0.41, rel_tol=0.0, abs_tol=1.0e-6), record
assert math.isclose(record["alpha"], 0.6, rel_tol=0.0, abs_tol=1.0e-6), record
references = materials[material_id]["image_textures"]
assert len(references) == 1, references
reference = references[0]
assert not reference["tiles"], reference
assert reference["packed"] and reference["packed_data_base64"], reference
assert base64.b64decode(reference["packed_data_base64"]) == bytes(
    image.packed_file.data), "packed regular image bytes must be preserved"
assert reference["alpha_mode"] == ("STRAIGHT" if alpha_cutout else "PREMUL"), reference
if alpha_cutout:
    packed_png = base64.b64decode(reference["packed_data_base64"])
    assert packed_png[:8] == b"\x89PNG\r\n\x1a\n", packed_png[:8]
    assert packed_png[25] == 6, "the packed cutout image must retain its RGBA channel"
    alpha_values = [image.pixels[index * 4 + 3]
                    for index in range(width * height)]
    assert min(alpha_values) == 0.0 and max(alpha_values) == 1.0
assert reference["interpolation"] == "Closest", reference
assert reference["extension"] == "EXTEND", reference
assert reference["source_transform_id"] == "identity", reference

blend_output = os.environ.get("CLASSICAD_SURFACE_MATERIAL_BLEND_OUTPUT")
if blend_output:
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_WORKBENCH"
    scene.render.resolution_x = 640
    scene.render.resolution_y = 480
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.film_transparent = False
    scene.display.render_aa = "OFF"
    scene.display.shading.light = "STUDIO"
    scene.display.shading.studio_light = "Default"
    scene.display.shading.color_type = os.environ.get(
        "CLASSICAD_SURFACE_MATERIAL_COLOR_MODE", "MATERIAL").upper()
    scene.display.shading.show_specular_highlight = True
    scene.display.shading.show_shadows = False
    scene.display.shading.show_cavity = False
    scene.display.shading.show_object_outline = False
    scene.display.shading.background_type = "WORLD"
    scene.world.color = (0.0219421387, 0.0219421387, 0.0219421387)
    scene.view_settings.view_transform = "AgX"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0

    direction = Vector((4.0, -6.0, 4.0)).normalized()
    camera_data = bpy.data.cameras.new("SurfaceMaterialReferenceCamera")
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = 5.0
    camera_data.clip_start = 0.01
    camera_data.clip_end = 2000.0
    camera = bpy.data.objects.new("SurfaceMaterialReferenceCamera", camera_data)
    bpy.context.collection.objects.link(camera)
    camera.location = direction * 1000.0
    camera.rotation_euler = (-camera.location).to_track_quat("-Z", "Y").to_euler()
    scene.camera = camera

    prefix = pathlib.Path(blend_output).with_suffix("")
    prefix.parent.mkdir(parents=True, exist_ok=True)
    scene.render.filepath = str(prefix) + ".png"
    bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(blend_output))
    bpy.ops.render.render(write_still=True)

    sys.path.insert(0, str(root / "tests"))
    from workbench_cube_reference import make_linear_output
    from workbench_exr_to_pfm import convert_exr

    linear_path = str(prefix) + ".exr"
    make_linear_output(scene, linear_path)
    bpy.ops.render.render(write_still=False)
    convert_exr(linear_path, str(prefix) + ".pfm",
                str(prefix) + ".depth.pfm")
    pathlib.Path(str(prefix) + ".depth.pfm").unlink(missing_ok=True)

print("NURBS surface regular texture material import contract passed")
