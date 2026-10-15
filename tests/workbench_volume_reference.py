#!/usr/bin/env python3
"""Render a tiny, deterministic Blender 5.2.2 Workbench volume reference.

Usage: blender --background --python tests/workbench_volume_reference.py -- density.png

The fixture is a single native FLOAT VDB grid with active bounds [-1, 0] on
each axis. Values vary by voxel so interpolation and ray integration are both
visible in the reference.
"""

import argparse
import os
import sys
import tempfile

import bpy
import openvdb


def arguments():
    argv = sys.argv
    argv = argv[argv.index("--") + 1 :] if "--" in argv else []
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", help="Workbench PNG output path")
    parser.add_argument(
        "--vdb-output",
        help="optional path to retain the generated VDB fixture",
    )
    return parser.parse_args(argv)


def build_density_vdb(path):
    grid = openvdb.FloatGrid()
    grid.name = "density"
    grid.transform.postTranslate((0.5, 0.5, 0.5))
    accessor = grid.getAccessor()
    for z in range(-1, 1):
        for y in range(-1, 1):
            for x in range(-1, 1):
                value = 0.2 + 0.2 * (x + 1) + 0.3 * (y + 1) + 0.4 * (z + 1)
                accessor.setValueOn((x, y, z), value)
    openvdb.write(path, grid)
    bounds = grid.evalActiveVoxelBoundingBox()
    if bounds != ((-1, -1, -1), (0, 0, 0)) or grid.activeVoxelCount() != 8:
        raise RuntimeError(
            f"unexpected VDB grid bounds/count: {bounds}, "
            f"{grid.activeVoxelCount()}"
        )


def configure_scene(output_path, vdb_path):
    scene = bpy.context.scene
    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)

    bpy.ops.object.volume_import(filepath=vdb_path, align="WORLD")
    volume = bpy.context.object
    if volume is None or volume.type != "VOLUME":
        raise RuntimeError("VDB import did not create a Blender VOLUME object")
    volume.name = "Density"
    volume.data.display.density = 1.0
    volume.data.display.interpolation_method = "LINEAR"
    volume.data.render.space = "WORLD"
    volume.color = (0.8, 0.12, 0.03, 1.0)

    scene.render.engine = "BLENDER_WORKBENCH"
    scene.render.resolution_x = 640
    scene.render.resolution_y = 480
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGB"
    scene.render.film_transparent = False
    scene.render.dither_intensity = 0.0
    scene.display.render_aa = "OFF"

    shading = scene.display.shading
    shading.light = "FLAT"
    shading.color_type = "SINGLE"
    shading.single_color = (0.8, 0.12, 0.03)
    shading.show_shadows = False
    shading.show_cavity = False
    shading.show_object_outline = False
    shading.show_specular_highlight = False
    shading.show_xray = False
    shading.background_type = "WORLD"
    shading.use_scene_world_render = True

    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0

    world = scene.world or bpy.data.worlds.new("World")
    scene.world = world
    world.color = (0.05, 0.05, 0.05)
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs["Color"].default_value = (
        0.05,
        0.05,
        0.05,
        1.0,
    )

    camera_data = bpy.data.cameras.new("Camera")
    camera = bpy.data.objects.new("Camera", camera_data)
    scene.collection.objects.link(camera)
    camera.location = (0.0, 0.0, 4.0)
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = 3.0
    camera_data.clip_start = 0.1
    camera_data.clip_end = 100.0
    scene.camera = camera
    scene.render.filepath = os.path.abspath(output_path)
    return scene


def main():
    if bpy.app.version != (5, 2, 2):
        raise RuntimeError(
            f"expected Blender 5.2.2, got {bpy.app.version_string}"
        )
    args = arguments()
    retained_vdb = args.vdb_output
    temporary_vdb = None
    if retained_vdb:
        vdb_path = os.path.abspath(retained_vdb)
        os.makedirs(os.path.dirname(vdb_path), exist_ok=True)
    else:
        file_handle, vdb_path = tempfile.mkstemp(suffix=".vdb")
        os.close(file_handle)
        temporary_vdb = vdb_path
    try:
        build_density_vdb(vdb_path)
        scene = configure_scene(args.output, vdb_path)
        bpy.ops.render.render(write_still=True)
        print(
            "Rendered Blender 5.2.2 Workbench volume reference:",
            scene.render.filepath,
            "grid=density bounds=(-1,-1,-1)..(0,0,0) voxels=8",
        )
    finally:
        if temporary_vdb and os.path.exists(temporary_vdb):
            os.unlink(temporary_vdb)


if __name__ == "__main__":
    main()
