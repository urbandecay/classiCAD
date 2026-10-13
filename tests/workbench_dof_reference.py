"""Render matched Blender 5.2.2 Workbench baseline and DOF captures.

Usage: blender --background --python tests/workbench_dof_reference.py -- output-prefix

Writes ``<prefix>-baseline.png`` and ``<prefix>-dof.png`` at 640x480. The
camera, cube, Workbench settings, and optics match the focused classiCAD GPU
DOF capture in ``viewport_render_contracts.cpp``.
"""

import argparse
import os
import sys

import bpy
from mathutils import Vector


def main():
    argv = sys.argv[sys.argv.index("--") + 1 :] if "--" in sys.argv else []
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output_prefix")
    args = parser.parse_args(argv)

    scene = bpy.context.scene
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)

    scene.render.engine = "BLENDER_WORKBENCH"
    scene.render.resolution_x = 640
    scene.render.resolution_y = 480
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.film_transparent = False
    # classiCAD's QImage capture does not add Blender render dithering; keep
    # the pixel comparison focused on Workbench DOF output.
    scene.render.dither_intensity = 0.0
    scene.display.render_aa = "OFF"
    scene.view_settings.view_transform = "AgX"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0

    shading = scene.display.shading
    shading.light = "STUDIO"
    shading.color_type = "MATERIAL"
    shading.background_type = "WORLD"
    shading.show_shadows = False
    shading.show_cavity = False
    shading.show_specular_highlight = False
    shading.show_object_outline = False
    shading.use_dof = False
    scene.world.color = (0.022, 0.022, 0.022)

    bpy.ops.mesh.primitive_cube_add(size=2.0, location=(0.0, 0.0, 0.0))
    cube = bpy.context.object
    cube.name = "ReferenceCube"
    material = bpy.data.materials.new("ReferenceCubeMaterial")
    material.diffuse_color = (0.8, 0.8, 0.8, 1.0)
    cube.data.materials.append(material)

    target = Vector((0.0, 0.0, 0.0))
    direction = Vector((4.0, -6.0, 4.0)).normalized()
    # ViewportTransform starts at a 60-unit reference distance; its 17.28
    # zoom maps to 60/17.28 for this matched perspective camera.
    camera_location = target + direction * (60.0 / 17.28)
    camera_data = bpy.data.cameras.new("ReferenceCamera")
    camera = bpy.data.objects.new("ReferenceCamera", camera_data)
    scene.collection.objects.link(camera)
    camera.location = camera_location
    camera.rotation_euler = (target - camera_location).to_track_quat("-Z", "Y").to_euler()
    camera_data.type = "PERSP"
    camera_data.lens = 25.0
    camera_data.sensor_fit = "HORIZONTAL"
    camera_data.sensor_width = 36.0
    camera_data.sensor_height = 24.0
    camera_data.clip_start = 0.01
    camera_data.clip_end = 34.7222222
    camera_data.dof.focus_distance = camera_location.length
    camera_data.dof.aperture_fstop = 0.1
    camera_data.dof.aperture_blades = 6
    camera_data.dof.aperture_rotation = 0.2
    camera_data.dof.aperture_ratio = 1.2
    scene.camera = camera

    output_prefix = os.path.abspath(args.output_prefix)
    os.makedirs(os.path.dirname(output_prefix), exist_ok=True)
    for enabled, suffix in ((False, "baseline"), (True, "dof")):
        shading.use_dof = enabled
        camera_data.dof.use_dof = enabled
        scene.render.filepath = f"{output_prefix}-{suffix}.png"
        bpy.ops.render.render(write_still=True)


if __name__ == "__main__":
    main()
