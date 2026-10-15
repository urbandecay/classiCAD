"""Verify Blender extractor output for a regular NURBS surface texture.

This checks the Blender-to-JSON extraction contract only. The matching C++
material resolver is covered by blender_surface_material_resolution_contracts.

Run with Blender 5.2.2: blender --background --python
tests/blender_surface_material_import_contract.py
"""

import base64
import importlib.util
import math
import os
import pathlib
import sys

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
spline = surface_data.splines[0]
assert spline.point_count_u >= spline.order_u
assert spline.point_count_v >= spline.order_v
spline.material_index = 0
for index, point in enumerate(spline.points):
    point.co = (float(index % spline.point_count_u) -
                (spline.point_count_u - 1) * 0.5,
                float(index // spline.point_count_u) -
                (spline.point_count_v - 1) * 0.5, 0.0, 1.0)
surface_data.materials.append(material)

materials = {}
unsupported = []
patches = extractor.extract_surface_patches(
    surface_object, bpy.context.scene, materials, unsupported)
assert len(patches) == 1, patches
surface = patches[0]["surface"]
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
assert reference["alpha_mode"] == "PREMUL", reference
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
    scene.display.shading.color_type = "MATERIAL"
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
