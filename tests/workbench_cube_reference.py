"""Render the controlled Workbench cube reference in Blender 5.2.2.

Usage: blender --background --python tests/workbench_cube_reference.py -- output.png --aa FXAA --effect BASELINE --linear-output output.exr [--match-classicad-ortho] [--texture-udim-vertical] [--texture-udim-parameter-uvs] [--no-specular]
AA accepts OFF, FXAA (Workbench's one-sample TAA+SMAA enum), or 5/8/11/16/32.
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
        choices=(
            "CUBE",
            "MATERIAL_SLOTS",
            "FOUR_MATERIAL_SLOTS",
            "SHADOW_RECEIVER",
            "TWO_SIDED_MATCAP",
            "NURBS_TEXTURE",
            "MESH_TEXTURE",
            "TRANSPARENT_OVERLAP",
            "TRANSPARENT_IN_FRONT",
        ),
        default="CUBE",
        help="controlled scene to render (default: CUBE)",
    )
    parser.add_argument(
        "--no-shadow-caster",
        action="store_true",
        help="leave only the receiver in the SHADOW_RECEIVER scene",
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
        "--resolution",
        nargs=2,
        type=int,
        metavar=("WIDTH", "HEIGHT"),
        default=(640, 480),
        help="render dimensions (default: 640 480)",
    )
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
    parser.add_argument(
        "--match-classicad-perspective",
        action="store_true",
        help=(
            "match the free perspective viewport at zoom 17.28, with a "
            "25 mm lens, 36 mm sensor, and 0.01/1000 clip range"
        ),
    )
    parser.add_argument(
        "--aa",
        choices=("OFF", "FXAA", "5", "8", "11", "16", "32"),
        default="FXAA",
        help=(
            "Workbench render_aa enum; FXAA selects the one-sample TAA filter "
            "and SMAA resolve in Workbench (default: FXAA)"
        ),
    )
    parser.add_argument(
        "--zero-dither",
        action="store_true",
        help="disable render-output dither for direct display-pixel comparisons",
    )
    parser.add_argument(
        "--transparent-film",
        action="store_true",
        help="render a transparent film background",
    )
    parser.add_argument(
        "--effect",
        choices=("BASELINE", "SHADOWS", "CAVITY", "OUTLINE"),
        default="BASELINE",
    )
    parser.add_argument(
        "--shadow-shift",
        type=float,
        default=0.1,
        help="Workbench shadow shift (default: Blender 5.2.2 default, 0.1)",
    )
    parser.add_argument(
        "--shadow-focus",
        type=float,
        default=0.0,
        help="Workbench shadow focus (default: Blender 5.2.2 default, 0.0)",
    )
    parser.add_argument(
        "--outline-color",
        nargs=3,
        type=float,
        metavar=("R", "G", "B"),
        default=(0.0, 0.0, 0.0),
        help="Workbench object outline color in Blender scene-linear values",
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
        choices=("MATERIAL", "OBJECT", "RANDOM", "TEXTURE", "SINGLE", "VERTEX"),
        default="MATERIAL",
        help="Workbench object color source (default: MATERIAL)",
    )
    parser.add_argument(
        "--attribute-domain",
        choices=("POINT", "CORNER"),
        default="CORNER",
        help="color attribute domain for VERTEX source (default: CORNER)",
    )
    parser.add_argument(
        "--single-color",
        nargs=3,
        type=float,
        metavar=("R", "G", "B"),
        default=(0.8, 0.8, 0.8),
        help="Workbench Single color in Blender scene-linear values",
    )
    parser.add_argument(
        "--texture-image",
        help="external image used by the active Image Texture node for TEXTURE color source",
    )
    parser.add_argument(
        "--texture-image-secondary",
        help="optional second image used by material slot 1 in MESH_TEXTURE",
    )
    parser.add_argument(
        "--texture-udim",
        action="store_true",
        help="load a 1001/1002 UDIM set and span both tiles in a texture fixture",
    )
    parser.add_argument(
        "--texture-udim-vertical",
        action="store_true",
        help="load a 1001/1011 UDIM set and span both V tiles in a texture fixture",
    )
    parser.add_argument(
        "--texture-udim-parameter-uvs",
        action="store_true",
        help=(
            "use a tiled image for NURBS_TEXTURE while keeping its normalized "
            "0..1 parameter UVs"
        ),
    )
    parser.add_argument(
        "--texture-color-space",
        help="set the loaded Image Texture image color space before Workbench sampling",
    )
    parser.add_argument(
        "--texture-alpha-mode",
        choices=("STRAIGHT", "PREMUL", "CHANNEL_PACKED", "NONE"),
        help="set the loaded image alpha interpretation for Workbench cutout tests",
    )
    parser.add_argument(
        "--texture-interpolation",
        choices=("Closest", "Linear", "Cubic", "Smart"),
        default="Closest",
        help="Image Texture interpolation used for TEXTURE color source",
    )
    parser.add_argument(
        "--texture-uv-pattern",
        action="store_true",
        help="vary cube UVs from vertex X/Z for a patterned image comparison",
    )
    parser.add_argument(
        "--material-color",
        nargs=3,
        type=float,
        metavar=("R", "G", "B"),
        help="set the cube material display RGB in Blender scene-linear values",
    )
    parser.add_argument(
        "--material-roughness",
        type=float,
        help="set cube material roughness in [0, 1]",
    )
    parser.add_argument(
        "--material-metallic",
        type=float,
        help="set cube material metallic value in [0, 1]",
    )
    parser.add_argument(
        "--material-alpha",
        type=float,
        default=1.0,
        help="set Workbench material display alpha (default: 1)",
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
    args = parser.parse_args(argv)
    if sum((args.texture_udim, args.texture_udim_vertical,
            args.texture_udim_parameter_uvs)) > 1:
        parser.error("choose only one UDIM mode")
    if args.texture_udim_parameter_uvs and args.scene != "NURBS_TEXTURE":
        parser.error("--texture-udim-parameter-uvs requires NURBS_TEXTURE")
    return args


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
    if any(dimension <= 0 for dimension in args.resolution):
        raise ValueError("render dimensions must be positive")
    if (not math.isfinite(args.material_alpha) or
            not 0.0 <= args.material_alpha <= 1.0):
        raise ValueError("material alpha must be finite and in [0, 1]")
    for name in ("material_roughness", "material_metallic"):
        value = getattr(args, name)
        if value is not None and (not math.isfinite(value) or
                                  not 0.0 <= value <= 1.0):
            raise ValueError(
                f"{name.replace('_', ' ')} must be finite and in [0, 1]"
            )
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
    camera_data.type = "PERSP" if args.match_classicad_perspective else "ORTHO"
    camera_data.ortho_scale = args.ortho_scale
    if args.match_classicad_perspective:
        camera_data.lens = 25.0
        camera_data.sensor_width = 36.0
        camera_data.sensor_fit = "HORIZONTAL"
    camera = bpy.data.objects.new("ReferenceCamera", camera_data)
    bpy.context.collection.objects.link(camera)
    ortho_eye_distance = args.ortho_eye_distance
    if args.match_classicad_perspective:
        ortho_eye_distance = 60.0 / 17.28
    elif args.match_classicad_ortho:
        ortho_eye_distance = 1000.0
    if ortho_eye_distance is None:
        ortho_eye_distance = (
            1000.0 if args.scene != "CUBE" else 8.0
        )
    if args.match_classicad_perspective:
        camera_data.clip_start = 0.01
        camera_data.clip_end = 1000.0
    elif args.match_classicad_ortho:
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
    if args.scene == "FOUR_MATERIAL_SLOTS":
        # Four visible coplanar quads make each material slot independently
        # probeable while keeping the camera and Workbench setup shared with
        # the other controlled mesh references.
        camera_rotation = camera.rotation_euler.to_quaternion()
        camera_right = camera_rotation @ Vector((1.0, 0.0, 0.0))
        camera_up = camera_rotation @ Vector((0.0, 1.0, 0.0))
        vertices = tuple(
            target + camera_right * x + camera_up * y
            for y in (-1.0, 0.0, 1.0)
            for x in (-1.0, 0.0, 1.0)
        )
        faces = (
            (0, 1, 4, 3),
            (1, 2, 5, 4),
            (3, 4, 7, 6),
            (4, 5, 8, 7),
        )
        mesh = bpy.data.meshes.new("FourMaterialSlotsMesh")
        mesh.from_pydata(vertices, (), faces)
        mesh.update()
        grid = bpy.data.objects.new("FourMaterialSlots", mesh)
        bpy.context.collection.objects.link(grid)
        slot_colors = (
            (0.18, 0.28, 0.72),
            (0.72, 0.20, 0.18),
            (0.18, 0.65, 0.25),
            (0.78, 0.68, 0.12),
        )
        for index, color in enumerate(slot_colors):
            material = bpy.data.materials.new(f"FourSlotMaterial{index}")
            material.diffuse_color = (*color, 1.0)
            grid.data.materials.append(material)
        for polygon, slot_index in zip(grid.data.polygons, range(4)):
            polygon.material_index = slot_index
    elif args.scene in ("CUBE", "MATERIAL_SLOTS"):
        bpy.ops.mesh.primitive_cube_add(size=2.0, location=(0.0, 0.0, 0.0))
        cube = bpy.context.object
        cube.name = "ReferenceCube"
        if (args.material_color is not None or
                args.material_roughness is not None or
                args.material_metallic is not None or
                args.color_source == "TEXTURE"):
            material = bpy.data.materials.new("ReferenceCubeMaterial")
            material.diffuse_color = (
                *(args.material_color or (0.8, 0.8, 0.8)), 1.0
            )
            if args.material_roughness is not None:
                material.roughness = args.material_roughness
            if args.material_metallic is not None:
                material.metallic = args.material_metallic
            cube.data.materials.append(material)
        if args.color_source == "TEXTURE":
            if not args.texture_image:
                raise ValueError("TEXTURE color source requires --texture-image")
            material.use_nodes = True
            image_node = material.node_tree.nodes.new("ShaderNodeTexImage")
            image_node.image = bpy.data.images.load(
                os.path.abspath(args.texture_image), check_existing=True
            )
            if args.texture_color_space:
                image_node.image.colorspace_settings.name = args.texture_color_space
            if args.texture_alpha_mode:
                image_node.image.alpha_mode = args.texture_alpha_mode
            image_node.interpolation = args.texture_interpolation
            image_node.show_texture = True
            material.node_tree.nodes.active = image_node
            uv_layer = cube.data.uv_layers.active.data
            if args.texture_uv_pattern:
                for polygon in cube.data.polygons:
                    for loop_index in polygon.loop_indices:
                        vertex = cube.data.vertices[
                            cube.data.loops[loop_index].vertex_index
                        ].co
                        uv_layer[loop_index].uv = (
                            (vertex.x + 1.0) * 0.5,
                            (vertex.z + 1.0) * 0.5,
                        )
            else:
                for loop in uv_layer:
                    loop.uv = (0.5, 0.5)
        if args.color_source == "VERTEX":
            color_attribute = cube.data.color_attributes.new(
                name="Color", type="FLOAT_COLOR", domain=args.attribute_domain
            )
            if args.attribute_domain == "POINT":
                for vertex_index, vertex in enumerate(cube.data.vertices):
                    color_attribute.data[vertex_index].color = (
                        0.1 + 0.35 * (vertex.co.x + 1.0),
                        0.1 + 0.35 * (vertex.co.y + 1.0),
                        0.1 + 0.35 * (vertex.co.z + 1.0),
                        1.0,
                    )
            else:  # CORNER
                for polygon in cube.data.polygons:
                    for loop_index in polygon.loop_indices:
                        vertex = cube.data.vertices[
                            cube.data.loops[loop_index].vertex_index
                        ].co
                        color_attribute.data[loop_index].color = (
                            0.1 + 0.35 * (vertex.x + 1.0),
                            0.1 + 0.35 * (vertex.y + 1.0),
                            0.1 + 0.35 * (vertex.z + 1.0),
                            1.0,
                        )
            color_index = cube.data.color_attributes.find(color_attribute.name)
            cube.data.color_attributes.active_color_index = color_index
            cube.data.color_attributes.render_color_index = color_index
        if args.object_color is not None:
            cube.color = (*args.object_color, 1.0)
        if args.scene == "MATERIAL_SLOTS":
            cube.data.materials.clear()
            material_colors = (
                (0.18, 0.28, 0.72),
                (0.72, 0.20, 0.18),
                (0.18, 0.65, 0.25),
            )
            material_roughness = (0.2, 0.5, 0.8)
            material_metallic = (0.0, 0.5, 1.0)
            for index, color in enumerate(material_colors):
                material = bpy.data.materials.new(
                    f"ReferenceCubeMaterial{index}"
                )
                material.diffuse_color = (*color, 1.0)
                material.roughness = material_roughness[index]
                material.metallic = material_metallic[index]
                cube.data.materials.append(material)
            for polygon in cube.data.polygons:
                normal = polygon.normal
                if normal.z > 0.5:
                    polygon.material_index = 0
                elif normal.y < -0.5:
                    polygon.material_index = 1
                elif normal.x > 0.5:
                    polygon.material_index = 2
                else:
                    polygon.material_index = 0
    elif args.scene == "SHADOW_RECEIVER":
        bpy.ops.mesh.primitive_plane_add(
            size=8.0, location=(0.0, 0.0, -1.0)
        )
        receiver = bpy.context.object
        receiver.name = "ShadowReceiver"
        receiver.color = (0.2, 0.4, 0.8, 1.0)
        if not args.no_shadow_caster:
            bpy.ops.mesh.primitive_cube_add(
                size=2.0, location=(0.0, 0.0, 0.0)
            )
            caster = bpy.context.object
            caster.name = "ShadowCaster"
            caster.color = (0.8, 0.3, 0.2, 1.0)
    elif args.scene == "TWO_SIDED_MATCAP":
        camera_rotation = camera.rotation_euler.to_quaternion()
        camera_right = camera_rotation @ Vector((1.0, 0.0, 0.0))
        camera_up = camera_rotation @ Vector((0.0, 1.0, 0.0))
        camera_outward = (camera.location - target).normalized()
        face_tilt = math.radians(35.0)
        face_up = (camera_up * math.cos(face_tilt) +
                   camera_outward * math.sin(face_tilt))
        half_size = 0.65
        center_offset = 0.78
        vertices = []
        for horizontal_offset in (-center_offset, center_offset):
            center = target + camera_right * horizontal_offset
            vertices.extend((
                center - camera_right * half_size - face_up * half_size,
                center + camera_right * half_size - face_up * half_size,
                center + camera_right * half_size + face_up * half_size,
                center - camera_right * half_size + face_up * half_size,
            ))
        mesh = bpy.data.meshes.new("TwoSidedMatcapMesh")
        mesh.from_pydata(vertices, [], [(0, 1, 2, 3), (4, 7, 6, 5)])
        mesh.update()
        obj = bpy.data.objects.new("TwoSidedMatcap", mesh)
        bpy.context.collection.objects.link(obj)
    elif args.scene == "NURBS_TEXTURE":
        if args.color_source != "TEXTURE" or not args.texture_image:
            raise ValueError("NURBS_TEXTURE requires TEXTURE color source and --texture-image")
        # classiCAD's regular planar NURBS fixture is a degree-one patch over
        # [-1, 1] in both directions. Render its equivalent display proxy with
        # explicit corner UVs so this comparison isolates Workbench sampling.
        mesh = bpy.data.meshes.new("NurbsTextureProxyMesh")
        mesh.from_pydata(
            [(-1.0, -1.0, 0.0), (1.0, -1.0, 0.0),
             (1.0, 1.0, 0.0), (-1.0, 1.0, 0.0)],
            [], [(0, 1, 2, 3)],
        )
        mesh.update()
        proxy = bpy.data.objects.new("NurbsTextureProxy", mesh)
        bpy.context.collection.objects.link(proxy)
        uv_layer = mesh.uv_layers.new(name="UVMap")
        for loop, uv in zip(uv_layer.data, ((0.0, 0.0), (1.0, 0.0),
                                            (1.0, 1.0), (0.0, 1.0))):
            loop.uv = uv
        if args.texture_udim or args.texture_udim_vertical:
            for loop in uv_layer.data:
                if args.texture_udim_vertical:
                    loop.uv.y *= 2.0
                else:
                    loop.uv.x *= 2.0
        material = bpy.data.materials.new("NurbsTextureMaterial")
        material.diffuse_color = (0.8, 0.8, 0.8, args.material_alpha)
        material.use_nodes = True
        image_node = material.node_tree.nodes.new("ShaderNodeTexImage")
        image_node.image = bpy.data.images.load(
            os.path.abspath(args.texture_image), check_existing=True
        )
        if (args.texture_udim or args.texture_udim_vertical or
                args.texture_udim_parameter_uvs):
            image_node.image.source = "TILED"
            udim_tile = 1011 if args.texture_udim_vertical else 1002
            if image_node.image.tiles.get(udim_tile) is None:
                image_node.image.tiles.new(udim_tile)
        if args.texture_color_space:
            image_node.image.colorspace_settings.name = args.texture_color_space
        if args.texture_alpha_mode:
            image_node.image.alpha_mode = args.texture_alpha_mode
        image_node.interpolation = args.texture_interpolation
        image_node.show_texture = True
        material.node_tree.nodes.active = image_node
        mesh.materials.append(material)
    elif args.scene == "MESH_TEXTURE":
        if args.color_source != "TEXTURE" or not args.texture_image:
            raise ValueError("MESH_TEXTURE requires TEXTURE color source and --texture-image")
        # Match classiCAD's explicit two-triangle planar mesh fixture so the
        # full-frame comparison uses the same diagonal and per-corner UVs.
        mesh = bpy.data.meshes.new("MeshTextureProxyMesh")
        mesh.from_pydata(
            [(-1.0, -1.0, 0.0), (1.0, -1.0, 0.0),
             (1.0, 1.0, 0.0), (-1.0, 1.0, 0.0)],
            [], [(0, 1, 2), (0, 2, 3)],
        )
        mesh.update()
        proxy = bpy.data.objects.new("MeshTextureProxy", mesh)
        bpy.context.collection.objects.link(proxy)
        uv_layer = mesh.uv_layers.new(name="UVMap")
        for loop, uv in zip(
            uv_layer.data,
            ((0.0, 0.0), (1.0, 0.0), (1.0, 1.0),
             (0.0, 0.0), (1.0, 1.0), (0.0, 1.0)),
        ):
            loop.uv = uv
        if args.texture_udim or args.texture_udim_vertical:
            for loop in uv_layer.data:
                if args.texture_udim_vertical:
                    loop.uv.y *= 2.0
                else:
                    loop.uv.x *= 2.0
        material = bpy.data.materials.new("MeshTextureMaterial")
        material.diffuse_color = (0.8, 0.8, 0.8, args.material_alpha)
        material.use_nodes = True
        image_node = material.node_tree.nodes.new("ShaderNodeTexImage")
        image_node.image = bpy.data.images.load(
            os.path.abspath(args.texture_image), check_existing=True
        )
        if args.texture_udim or args.texture_udim_vertical:
            image_node.image.source = "TILED"
            udim_tile = 1011 if args.texture_udim_vertical else 1002
            if image_node.image.tiles.get(udim_tile) is None:
                image_node.image.tiles.new(udim_tile)
        if args.texture_color_space:
            image_node.image.colorspace_settings.name = args.texture_color_space
        if args.texture_alpha_mode:
            image_node.image.alpha_mode = args.texture_alpha_mode
        image_node.interpolation = args.texture_interpolation
        image_node.show_texture = True
        material.node_tree.nodes.active = image_node
        mesh.materials.append(material)
        if args.texture_image_secondary:
            secondary_material = bpy.data.materials.new(
                "MeshTextureSecondaryMaterial"
            )
            secondary_material.diffuse_color = (0.8, 0.8, 0.8, 1.0)
            secondary_material.use_nodes = True
            secondary_node = secondary_material.node_tree.nodes.new(
                "ShaderNodeTexImage"
            )
            secondary_node.image = bpy.data.images.load(
                os.path.abspath(args.texture_image_secondary),
                check_existing=True,
            )
            if args.texture_color_space:
                secondary_node.image.colorspace_settings.name = (
                    args.texture_color_space
                )
            if args.texture_alpha_mode:
                secondary_node.image.alpha_mode = args.texture_alpha_mode
            secondary_node.interpolation = args.texture_interpolation
            secondary_node.show_texture = True
            secondary_material.node_tree.nodes.active = secondary_node
            mesh.materials.append(secondary_material)
            mesh.polygons[1].material_index = 1
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
    scene.render.resolution_x, scene.render.resolution_y = args.resolution
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.filepath = args.output
    scene.render.film_transparent = args.transparent_film
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
    shading.single_color = tuple(args.single_color)
    shading.show_shadows = args.effect == "SHADOWS"
    shading.shadow_intensity = 0.5
    scene.display.shadow_shift = args.shadow_shift
    scene.display.shadow_focus = args.shadow_focus
    shading.show_cavity = args.effect == "CAVITY"
    if args.effect == "CAVITY":
        shading.cavity_type = "WORLD"
    shading.cavity_ridge_factor = 1.0
    shading.cavity_valley_factor = 1.0
    scene.display.matcap_ssao_samples = 16
    scene.display.matcap_ssao_distance = 0.2
    scene.display.matcap_ssao_attenuation = 1.0
    shading.object_outline_color = tuple(args.outline_color)
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
