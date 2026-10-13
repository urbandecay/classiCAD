"""Render the controlled Workbench cube reference in Blender 5.2.2.

Usage: blender --background --python tests/workbench_cube_reference.py -- output.png --aa FXAA --effect BASELINE --linear-output output.exr
AA accepts OFF, FXAA, or 8 (default: FXAA).
Effect accepts BASELINE, SHADOWS, CAVITY, or OUTLINE (default: BASELINE).
"""

import argparse
import os
import sys

import bpy
from mathutils import Vector


def arguments():
    argv = sys.argv
    argv = argv[argv.index("--") + 1 :] if "--" in argv else []
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", help="PNG output path")
    parser.add_argument("--aa", choices=("OFF", "FXAA", "8"), default="FXAA")
    parser.add_argument(
        "--effect",
        choices=("BASELINE", "SHADOWS", "CAVITY", "OUTLINE"),
        default="BASELINE",
    )
    parser.add_argument(
        "--linear-output",
        help="also write final scene-linear Combined and Z passes to a multilayer OpenEXR",
    )
    parser.add_argument(
        "--jitter-pixels",
        nargs=2,
        type=float,
        metavar=("X", "Y"),
        help=(
            "apply a Blender projection-space subpixel offset "
            "(positive Y is projection-up; diagnostic only)"
        ),
    )
    parser.add_argument(
        "--jitter-metadata",
        help="render one AA-off Blender reference for every offset in a classiCAD capture metadata file",
    )
    return parser.parse_args(argv)


def make_linear_output(scene, output_path):
    # The background Workbench render result is not exposed as a populated
    # bpy.data.images datablock, so route the post-AA render passes through a
    # 32-bit multilayer EXR File Output node instead.
    scene.view_layers[0].use_pass_z = True
    composite = bpy.data.node_groups.new(
        "Workbench Linear Capture", "CompositorNodeTree"
    )
    composite.interface.new_socket(
        name="Image", in_out="OUTPUT", socket_type="NodeSocketColor"
    )
    render_layers = composite.nodes.new("CompositorNodeRLayers")
    group_output = composite.nodes.new("NodeGroupOutput")
    file_output = composite.nodes.new("CompositorNodeOutputFile")
    output_directory, output_name = os.path.split(os.path.abspath(output_path))
    output_stem, _ = os.path.splitext(output_name)
    file_output.directory = output_directory
    file_output.file_name = output_stem
    file_output.format.file_format = "OPEN_EXR_MULTILAYER"
    file_output.format.color_depth = "32"
    file_output.save_as_render = False
    file_output.file_output_items.new("RGBA", "Combined")
    file_output.file_output_items.new("FLOAT", "Depth")
    composite.links.new(
        render_layers.outputs["Image"], group_output.inputs["Image"]
    )
    composite.links.new(
        render_layers.outputs["Image"], file_output.inputs["Combined"]
    )
    composite.links.new(
        render_layers.outputs["Depth"], file_output.inputs["Depth"]
    )
    scene.compositing_node_group = composite
    scene.render.use_compositing = True
    return file_output


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
    if args.jitter_pixels is not None:
        # Reproduce Blender's projection-space offset with an equivalent camera
        # translation. Positive Y follows Blender's projection-up convention.
        screen_x, screen_y = args.jitter_pixels
        units_per_pixel = camera_data.ortho_scale / 640.0
        camera_rotation = camera.rotation_euler.to_quaternion()
        camera_right = camera_rotation @ Vector((1.0, 0.0, 0.0))
        camera_up = camera_rotation @ Vector((0.0, 1.0, 0.0))
        camera.location += (
            -camera_right * screen_x * units_per_pixel
            - camera_up * screen_y * units_per_pixel
        )
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
    shading.show_shadows = args.effect == "SHADOWS"
    shading.shadow_intensity = 0.5
    shading.show_cavity = args.effect == "CAVITY"
    shading.cavity_ridge_factor = 1.0
    shading.cavity_valley_factor = 1.0
    shading.show_object_outline = args.effect == "OUTLINE"
    shading.background_type = "WORLD"
    scene.world.color = (0.0232, 0.0232, 0.0232)

    scene.view_settings.view_transform = "AgX"
    if args.jitter_metadata:
        if not args.linear_output:
            raise ValueError("--jitter-metadata requires --linear-output")
        scene.display.render_aa = "OFF"
        file_output = make_linear_output(scene, args.linear_output)
        output_dir, output_name = os.path.split(os.path.abspath(args.output))
        output_stem, _ = os.path.splitext(output_name)
        linear_dir, linear_name = os.path.split(
            os.path.abspath(args.linear_output)
        )
        linear_stem, _ = os.path.splitext(linear_name)
        base_camera_location = camera.location.copy()
        camera_rotation = camera.rotation_euler.to_quaternion()
        camera_right = camera_rotation @ Vector((1.0, 0.0, 0.0))
        camera_up = camera_rotation @ Vector((0.0, 1.0, 0.0))
        units_per_pixel = camera_data.ortho_scale / scene.render.resolution_x
        offsets = []
        with open(args.jitter_metadata, "r", encoding="utf-8") as metadata:
            for line in metadata:
                fields = line.split()
                if not fields or fields[0].startswith("#"):
                    continue
                if len(fields) != 3:
                    raise ValueError(
                        f"invalid jitter metadata row: expected 3 fields, got {len(fields)}"
                    )
                offsets.append((int(fields[0]), float(fields[1]), float(fields[2])))
        if not offsets:
            raise ValueError("--jitter-metadata contains no sample offsets")

        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from workbench_exr_to_pfm import convert_exr

        for sample_index, screen_x, screen_y in offsets:
            camera.location = (
                base_camera_location
                - camera_right * screen_x * units_per_pixel
                - camera_up * screen_y * units_per_pixel
            )
            output_base = f"{output_stem}-sample-{sample_index}"
            linear_base = f"{linear_stem}-sample-{sample_index}"
            scene.render.filepath = os.path.join(output_dir, output_base + ".png")
            file_output.directory = linear_dir
            file_output.file_name = linear_base
            bpy.ops.render.render(write_still=True)
            convert_exr(
                os.path.join(linear_dir, linear_base + ".exr"),
                os.path.join(linear_dir, linear_base + "-color.pfm"),
                os.path.join(linear_dir, linear_base + "-depth.pfm"),
            )
        return

    bpy.ops.render.render(write_still=True)

    if args.linear_output:
        make_linear_output(scene, args.linear_output)
        bpy.ops.render.render(write_still=False)


if __name__ == "__main__":
    main()
