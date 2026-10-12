"""Render the controlled Workbench cube reference in Blender 5.2.2.

Usage: blender --background --python tests/workbench_cube_reference.py -- output.png --aa FXAA
AA accepts OFF, FXAA, or 8 (default: FXAA).
"""

import argparse
import sys

import bpy
from mathutils import Vector


def arguments():
    argv = sys.argv
    argv = argv[argv.index("--") + 1 :] if "--" in argv else []
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", help="PNG output path")
    parser.add_argument("--aa", choices=("OFF", "FXAA", "8"), default="FXAA")
    return parser.parse_args(argv)


def main():
    args = arguments()
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)

    bpy.ops.mesh.primitive_cube_add(size=2.0, location=(0.0, 0.0, 0.0))
    cube = bpy.context.object
    cube.name = "ReferenceCube"

    target = Vector((0.0, 0.0, 0.0))
    direction = Vector((4.0, -6.0, 4.0)).normalized()
    camera_data = bpy.data.cameras.new("ReferenceCamera")
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = 5.0
    camera = bpy.data.objects.new("ReferenceCamera", camera_data)
    bpy.context.collection.objects.link(camera)
    camera.location = target + direction * 8.0
    camera.rotation_euler = (target - camera.location).to_track_quat("-Z", "Y").to_euler()
    bpy.context.scene.camera = camera

    scene = bpy.context.scene
    scene.render.engine = "BLENDER_WORKBENCH"
    scene.render.resolution_x = 640
    scene.render.resolution_y = 480
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.filepath = args.output
    scene.render.film_transparent = False
    scene.display.render_aa = args.aa

    shading = scene.display.shading
    shading.light = "STUDIO"
    shading.studio_light = "Default"
    shading.color_type = "MATERIAL"
    shading.show_shadows = False
    shading.show_cavity = False
    shading.show_object_outline = False
    shading.background_type = "WORLD"
    scene.world.color = (0.0232, 0.0232, 0.0232)

    scene.view_settings.view_transform = "AgX"
    bpy.ops.render.render(write_still=True)


if __name__ == "__main__":
    main()
