"""Render the controlled Workbench cube reference in Blender 5.2.2.

Usage: blender --background --python tests/workbench_cube_reference.py -- output.png --aa FXAA --effect BASELINE --linear-output output.exr [--match-classicad-ortho] [--no-specular]
AA accepts OFF, FXAA, or 8 (default: FXAA).
Effect accepts BASELINE, SHADOWS, CAVITY, or OUTLINE (default: BASELINE).
"""

import argparse
import math
import os
import sys

import bpy
from mathutils import Vector


def arguments():
    argv = sys.argv
    argv = argv[argv.index("--") + 1 :] if "--" in argv else []
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", help="PNG output path")
    parser.add_argument(
        "--scene",
        choices=("CUBE", "TRANSPARENT_OVERLAP", "TRANSPARENT_IN_FRONT"),
        default="CUBE",
        help="controlled scene to render (default: CUBE)",
    )
    parser.add_argument(
        "--ortho-eye-distance",
        type=float,
        help=(
            "orthographic camera distance (defaults to 8 for CUBE and 1000 "
            "for TRANSPARENT_OVERLAP)"
        ),
    )
    parser.add_argument("--ortho-scale", type=float, default=5.0)
    parser.add_argument(
        "--view-direction",
        nargs=3,
        type=float,
        metavar=("X", "Y", "Z"),
        help="override the controlled camera direction from the target",
    )
    parser.add_argument(
        "--match-classicad-ortho",
        action="store_true",
        help=(
            "use classiCAD's free orthographic render camera: 1000-unit eye "
            "distance and 0.01/2000 clip range"
        ),
    )
    parser.add_argument("--aa", choices=("OFF", "FXAA", "8"), default="FXAA")
    parser.add_argument(
        "--zero-dither",
        action="store_true",
        help="disable render-output dither for direct display-pixel comparisons",
    )
    parser.add_argument(
        "--effect",
        choices=("BASELINE", "SHADOWS", "CAVITY", "OUTLINE"),
        default="BASELINE",
    )
    parser.add_argument(
        "--lighting",
        choices=("STUDIO", "MATCAP", "FLAT"),
        default="STUDIO",
        help="Workbench lighting mode (default: STUDIO)",
    )
    parser.add_argument(
        "--light-preset",
        default="Default",
        help="bundled Blender Studio Light or MatCap preset (default: Default)",
    )
    parser.add_argument(
        "--world-space-lighting",
        action="store_true",
        help="use world-space Studio Light orientation",
    )
    parser.add_argument(
        "--no-specular",
        action="store_true",
        help="disable Workbench specular highlights for a controlled comparison",
    )
    parser.add_argument(
        "--rotation-degrees",
        type=float,
        default=0.0,
        help="Studio Light Z rotation in degrees (default: 0)",
    )
    parser.add_argument(
        "--color-source",
        choices=("MATERIAL", "OBJECT", "RANDOM"),
        default="MATERIAL",
        help="Workbench object color source (default: MATERIAL)",
    )
    parser.add_argument(
        "--material-color",
        nargs=3,
        type=float,
        metavar=("R", "G", "B"),
        help="set the cube material display RGB in Blender scene-linear values",
    )
    parser.add_argument(
        "--object-color",
        nargs=3,
        type=float,
        metavar=("R", "G", "B"),
        help="set the cube object display RGB in Blender scene-linear values",
    )
    parser.add_argument(
        "--background-source",
        choices=("WORLD", "VIEWPORT"),
        default="WORLD",
        help="Workbench background source (default: WORLD)",
    )
    parser.add_argument("--world-color", nargs=3, type=float, metavar=("R", "G", "B"),
                        default=(0.12, 0.07, 0.035))
    parser.add_argument("--viewport-background-color", nargs=3, type=float,
                        metavar=("R", "G", "B"), default=(0.25, 0.45, 0.65))
    parser.add_argument(
        "--view-transform",
        choices=("AgX", "Standard", "Khronos PBR Neutral", "Filmic"),
        default="AgX",
        help="Blender scene view transform (default: AgX)",
    )
    parser.add_argument("--exposure", type=float, default=0.0,
                        help="Blender scene exposure (default: 0)")
    parser.add_argument("--gamma", type=float, default=1.0,
                        help="Blender scene gamma (default: 1)")
    parser.add_argument(
        "--linear-output",
        help="also write final scene-linear Combined and Z passes to a multilayer OpenEXR",
    )
    parser.add_argument(
        "--linear-pfm-output",
        help="convert the scene-linear Combined pass to an RGB PFM capture",
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

    target = Vector((0.0, 0.0, 0.0))
    direction = (
        Vector(args.view_direction).normalized()
        if args.view_direction is not None
        else (
            Vector((0.0, 0.0, 1.0))
            if args.scene == "TRANSPARENT_IN_FRONT"
            else Vector((4.0, -6.0, 4.0)).normalized()
        )
    )
    camera_data = bpy.data.cameras.new("ReferenceCamera")
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = args.ortho_scale
    camera = bpy.data.objects.new("ReferenceCamera", camera_data)
    bpy.context.collection.objects.link(camera)
    ortho_eye_distance = args.ortho_eye_distance
    if args.match_classicad_ortho:
        ortho_eye_distance = 1000.0
    if ortho_eye_distance is None:
        ortho_eye_distance = (
            1000.0 if args.scene != "CUBE" else 8.0
        )
    if args.match_classicad_ortho:
        camera_data.clip_start = 0.01
        camera_data.clip_end = 2000.0
    elif args.scene == "TRANSPARENT_OVERLAP":
        # classiCAD's free orthographic viewport uses near/far planes at
        # 0.01/2000 for its 1000-unit projection eye. Match the Blender
        # camera's linear depth so weighted-OIT colors compare fairly too.
        camera_data.clip_start = 0.01
        camera_data.clip_end = max(2000.0, ortho_eye_distance * 2.0)
    camera.location = target + direction * ortho_eye_distance
    camera.rotation_euler = (
        (target - camera.location).to_track_quat("-Z", "Y").to_euler()
    )
    if args.scene == "CUBE":
        bpy.ops.mesh.primitive_cube_add(size=2.0, location=(0.0, 0.0, 0.0))
        cube = bpy.context.object
        cube.name = "ReferenceCube"
        if args.material_color is not None:
            material = bpy.data.materials.new("ReferenceCubeMaterial")
            material.diffuse_color = (*args.material_color, 1.0)
            cube.data.materials.append(material)
        if args.object_color is not None:
            cube.color = (*args.object_color, 1.0)
    elif args.scene in ("TRANSPARENT_OVERLAP", "TRANSPARENT_IN_FRONT"):
        # Two camera-facing quads provide stable, known overlap regions without
        # perspective or surface-lighting changes between captures.
        camera_rotation = camera.rotation_euler.to_quaternion()
        camera_right = camera_rotation @ Vector((1.0, 0.0, 0.0))
        camera_up = camera_rotation @ Vector((0.0, 1.0, 0.0))

        def add_transparent_quad(name, center, size, color):
            half = size * 0.5
            vertices = [
                center - camera_right * half - camera_up * half,
                center + camera_right * half - camera_up * half,
                center + camera_right * half + camera_up * half,
                center - camera_right * half + camera_up * half,
            ]
            mesh = bpy.data.meshes.new(name + "Mesh")
            mesh.from_pydata(vertices, [], [(0, 1, 2, 3)])
            mesh.update()
            obj = bpy.data.objects.new(name, mesh)
            bpy.context.collection.objects.link(obj)
            material = bpy.data.materials.new(name + "Material")
            material.diffuse_color = color
            material.blend_method = "BLEND"
            obj.data.materials.append(material)
            return obj

        if args.scene == "TRANSPARENT_IN_FRONT":
            def add_opaque_quad(name, center, size, color, show_in_front=False):
                obj = add_transparent_quad(name, center, size, color)
                obj.data.materials[0].diffuse_color = (*color[:3], 1.0)
                obj.show_in_front = show_in_front
                return obj

            add_opaque_quad(
                "RegularOpaque", target, 3.2, (0.12, 0.62, 0.18, 1.0)
            )
            add_transparent_quad(
                "RegularTransparent",
                target + direction * 0.25 - camera_right * 0.55,
                2.5,
                (0.08, 0.25, 0.95, 0.55),
            )
            add_opaque_quad(
                "InFrontOpaque",
                target - direction * 0.2 + camera_right * 0.35,
                1.15,
                (0.95, 0.72, 0.05, 1.0),
                show_in_front=True,
            )
            in_front_transparent = add_transparent_quad(
                "InFrontTransparent",
                target + direction * 0.4 + camera_right * 0.55,
                1.4,
                (0.95, 0.12, 0.04, 0.48),
            )
            in_front_transparent.show_in_front = True
        else:
            add_transparent_quad(
                "RearTranslucentSurface",
                target - direction * 0.35 - camera_right * 0.25,
                2.4,
                (0.08, 0.25, 0.95, 0.55),
            )
            add_transparent_quad(
                "FrontTranslucentSurface",
                target + direction * 0.35 + camera_right * 0.25,
                2.0,
                (0.95, 0.12, 0.04, 0.48),
            )
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
    if args.zero_dither:
        scene.render.dither_intensity = 0.0
    scene.display.render_aa = args.aa
    shading = scene.display.shading
    shading.light = args.lighting
    light_preset = args.light_preset
    if args.lighting == "MATCAP" and not light_preset.lower().endswith(".exr"):
        light_preset += ".exr"
    elif args.lighting == "STUDIO" and light_preset.lower() != "default" and not light_preset.lower().endswith(".sl"):
        light_preset = light_preset.lower() + ".sl"
    shading.studio_light = light_preset
    shading.show_specular_highlight = not args.no_specular
    shading.use_world_space_lighting = args.world_space_lighting
    shading.studiolight_rotate_z = math.radians(args.rotation_degrees)
    shading.color_type = args.color_source
    shading.show_shadows = args.effect == "SHADOWS"
    shading.shadow_intensity = 0.5
    shading.show_cavity = args.effect == "CAVITY"
    if args.effect == "CAVITY":
        shading.cavity_type = "WORLD"
    shading.cavity_ridge_factor = 1.0
    shading.cavity_valley_factor = 1.0
    scene.display.matcap_ssao_samples = 16
    scene.display.matcap_ssao_distance = 0.2
    scene.display.matcap_ssao_attenuation = 1.0
    shading.object_outline_color = (0.0, 0.0, 0.0)
    shading.show_object_outline = args.effect == "OUTLINE"
    shading.background_type = args.background_source
    scene.world.color = args.world_color
    shading.background_color = args.viewport_background_color
    scene.view_settings.view_transform = args.view_transform
    scene.view_settings.exposure = args.exposure
    scene.view_settings.gamma = args.gamma
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
        if args.linear_pfm_output:
            sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
            from workbench_exr_to_pfm import convert_exr

            depth_output = args.linear_pfm_output + ".depth.pfm"
            convert_exr(args.linear_output, args.linear_pfm_output, depth_output)
            os.remove(depth_output)


if __name__ == "__main__":
    main()
