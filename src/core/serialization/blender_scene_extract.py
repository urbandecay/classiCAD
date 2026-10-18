"""Extract Blender 5.2.2 scene meshes for classiCAD's mesh import bridge.

Run in background Blender as:
  blender --background --python blender_scene_extract.py -- \
      extract INPUT.blend OUTPUT.json [original|evaluated]

This emits transport data only. The classiCAD importer must validate every
required mesh record before creating native scene objects.
"""

import base64
import hashlib
import json
import math
import os
import re
import struct
import sys
import tempfile
import traceback
import zlib

import bpy


PINNED_BLENDER_VERSION = (5, 2, 2)
BRIDGE_FORMAT = "classiCAD.blender-scene-extract"
BRIDGE_FORMAT_VERSION = 82
# Keep packed image data from expanding the extractor's JSON transport without
# bound. For UDIMs this is the aggregate decoded byte limit across all tiles.
MAX_PACKED_IMAGE_BYTES = 32 * 1024 * 1024
MAX_SCENE_LINEAR_IMAGE_BYTES = 32 * 1024 * 1024
MAX_SCENE_LINEAR_IMAGE_PIXELS = MAX_SCENE_LINEAR_IMAGE_BYTES // 16
MAX_SCENE_LINEAR_FLOAT_BYTES = 64 * 1024 * 1024
SCENE_LINEAR_FLOAT_PAYLOAD_BYTES = 0
SCENE_LINEAR_FLOAT_PAYLOAD_CACHE = {}
MAX_COLOR_LUTS = 8
MAX_COLOR_LUT_BYTES = 32 * 1024 * 1024
MAX_OUTLINER_HIERARCHY_ITEMS = 1024
MAX_OUTLINER_LIBRARY_ITEMS = 20000
MAX_OUTLINER_ORPHAN_ITEMS = 20000
MAX_OUTLINER_SEQUENCE_ITEMS = 10000
MAX_OUTLINER_SEQUENCE_DEPTH = 128
MAX_OUTLINER_DATA_API_ITEMS = 20000
MAX_OUTLINER_DATA_API_DEPTH = 32
MAX_OUTLINER_DATA_API_BRANCH_ITEMS = 32
MAX_OUTLINER_DATA_API_DRIVER_PATHS = 20000
MAX_OUTLINER_KEYING_SETS = 1024
MAX_OUTLINER_KEYING_SET_PATHS = 20000
SUPPORTED_OBJECT_RELATIVE_KEYING_SET_IDS = frozenset({
    "Available",
    "Location",
    "Rotation",
    "Scaling",
    "BUILTIN_KSI_LocRot",
    "LocRotScale",
    "LocRotScaleCProp",
    "BUILTIN_KSI_LocScale",
    "BUILTIN_KSI_RotScale",
    "BUILTIN_KSI_DeltaLocation",
    "BUILTIN_KSI_DeltaRotation",
    "BUILTIN_KSI_DeltaScale",
    "BUILTIN_KSI_VisualLoc",
    "BUILTIN_KSI_VisualRot",
    "BUILTIN_KSI_VisualScaling",
    "BUILTIN_KSI_VisualLocRot",
    "BUILTIN_KSI_VisualLocScale",
    "BUILTIN_KSI_VisualLocRotScale",
    "BUILTIN_KSI_VisualRotScale",
})
# Blender's RNA Outliner stores collection indices in a signed short.
MAX_OUTLINER_DATA_API_COLLECTION_ITEMS = 32767
MAX_OUTLINER_OVERRIDE_ITEMS = 20000
MAX_OUTLINER_OVERRIDE_PROPERTIES_PER_ID = 512
MAX_OUTLINER_OVERRIDE_DEPTH = 128
MAX_OUTLINER_OVERRIDE_RELATIONS_PER_ID = 2048
MAX_OUTLINER_OVERRIDE_VIEW_LAYER_STATES = 200000
MAX_OUTLINER_VIEW_LAYER_BASE_IDS = 200000
MAX_OUTLINER_ACTION_SLOTS = 20000
MAX_OUTLINER_LIBRARY_ANIMATION_ITEMS = 20000
MAX_OUTLINER_LIBRARY_SPECIALIZED_ITEMS = 20000
COLOR_LUT_SIZE = 65
COLOR_LUT_DOMAIN_MIN = 0.0
COLOR_LUT_DOMAIN_MAX = 16.0
COLOR_LUT_SHAPER = "log2_1p"
COLOR_LUT_SHAPER_SCALE = 64.0
# Volume transport keeps sparse active voxels/tiles compact and bounded.
MAX_VOLUME_GRID_RECORDS = 262144
MAX_VOLUME_GRID_VOXELS = 16 * 1024 * 1024
MAX_VOLUME_GRID_PAYLOAD_BYTES = 16 * 1024 * 1024
MAX_COLLECTION_INSTANCES = 50000
MAX_CAMERA_VIEWPORT_EXTRAS_PER_SCENE = 1024
MAX_COLLECTION_IMAGE_EMPTY_INSTANCES = 256
MAX_COLLECTION_IMAGE_EMPTY_PIXELS = 16 * 1024 * 1024
MAX_COLLECTION_VOLUME_INSTANCES = 4
MAX_COLLECTION_VOLUME_PAYLOAD_BYTES = 32 * 1024 * 1024
MAX_COLLECTION_VOLUME_RECORDS = 524288
MAX_COLLECTION_VOLUME_DENSE_VOXELS = 16 * 1024 * 1024
MAX_GREASE_PENCIL_STROKES_PER_SCENE = 10000
MAX_GREASE_PENCIL_POINTS_PER_SCENE = 100000
MAX_HAIR_CURVES_PER_SCENE = 10000
MAX_HAIR_CURVE_POINTS_PER_SCENE = 100000
MAX_POINT_CLOUD_POINTS_PER_SCENE = 100000
# Mesh JSON expands into several parallel per-vertex/per-corner arrays. Bound
# both a single source mesh and all emitted copies in one scene, including
# collection instances and generated curve/font/metaball meshes.
MAX_MESH_OBJECT_ESTIMATED_BYTES = 32 * 1024 * 1024
MAX_MESH_SCENE_ESTIMATED_BYTES = 128 * 1024 * 1024
MAX_EXTRACTION_JSON_BYTES = 256 * 1024 * 1024
MAX_LOOSE_VERTICES_PER_OBJECT = 10000
MAX_LOOSE_VERTEX_POINTS_PER_SCENE = 50000
SOURCE_PROJECT_NAMESPACE = ""


def bake_color_lut(scene, lut_records, lut_bytes_by_id, unsupported):
    """Bake Blender's selected scene-linear to sRGB display transform."""
    item_id = stable_id(scene)
    color_management = {
        "display_device": scene.display_settings.display_device,
        "view_transform": scene.view_settings.view_transform,
        "look": scene.view_settings.look,
        "exposure": finite_number(scene.view_settings.exposure),
        "gamma": finite_number(scene.view_settings.gamma),
    }
    if color_management["display_device"].lower() != "srgb":
        add_unsupported(unsupported, item_id, "color_management_lut",
                        "only the sRGB display device can be represented by the scene LUT")
        return color_management
    if (color_management["look"].lower() == "none" and
            color_management["view_transform"] in {
                "AgX", "Standard", "Khronos PBR Neutral", "Filmic"
            }):
        # These are already represented by the importer's established paths.
        return color_management
    try:
        import PyOpenColorIO as ocio

        config = ocio.GetCurrentConfig()
        display = color_management["display_device"]
        wanted_transform = color_management["view_transform"]
        view = next((name for name in config.getViews(display)
                     if config.getDisplayViewTransformName(display, name) == wanted_transform), None)
        if view is None:
            # Some Blender configs use the view name as the view-transform name.
            if wanted_transform in config.getViews(display):
                view = wanted_transform
            else:
                raise ValueError("no OCIO sRGB view matches Blender view transform " + wanted_transform)
        transform = ocio.DisplayViewTransform()
        scene_linear = config.getRoleColorSpace(ocio.ROLE_SCENE_LINEAR)
        transform.setSrc(scene_linear)
        transform.setDisplay(display)
        transform.setView(view)
        look = color_management["look"]
        if look and look.lower() != "none":
            look_definition = config.getLook(look)
            if look_definition is None:
                raise ValueError("selected OCIO look is unavailable: " + look)
            # Blender's GetLooksResultColorSpace is the selected look's process
            # space. Keeping both ends of the LookTransform there matches the
            # processor's output color space before the display view transform.
            look_result_space = look_definition.getProcessSpace()
            if not look_result_space:
                raise ValueError("selected OCIO look has no process color space")
            look_transform = ocio.LookTransform()
            look_transform.setSrc(scene_linear)
            look_transform.setDst(look_result_space)
            look_transform.setLooks(look)
            transform.setLooksBypass(True)
            transform.setSrc(look_result_space)
            group = ocio.GroupTransform()
            group.appendTransform(look_transform)
            group.appendTransform(transform)
            processor = config.getProcessor(group).getDefaultCPUProcessor()
        else:
            processor = config.getProcessor(transform).getDefaultCPUProcessor()
        values = bytearray()
        denominator = COLOR_LUT_SIZE - 1
        # Existing consumers index ((blue * size + green) * size + red).
        log_range = math.log2(1.0 + COLOR_LUT_SHAPER_SCALE *
                              (COLOR_LUT_DOMAIN_MAX - COLOR_LUT_DOMAIN_MIN))

        def sample_value(index):
            t = index / denominator
            shaped = math.exp2(t * log_range) - 1.0
            return COLOR_LUT_DOMAIN_MIN + shaped / COLOR_LUT_SHAPER_SCALE

        for b in range(COLOR_LUT_SIZE):
            blue = sample_value(b)
            for g in range(COLOR_LUT_SIZE):
                green = sample_value(g)
                for r in range(COLOR_LUT_SIZE):
                    red = sample_value(r)
                    color = processor.applyRGB([red, green, blue])
                    for component in color:
                        number = finite_number(component)
                        values.extend(struct.pack("<f", number))
        raw = bytes(values)
        lut_id = hashlib.sha256(raw).hexdigest()
        if lut_id not in lut_bytes_by_id:
            if len(lut_records) >= MAX_COLOR_LUTS:
                raise ValueError("scene color LUT limit exceeded (%d unique tables)" % MAX_COLOR_LUTS)
            if sum(lut_bytes_by_id.values()) + len(raw) > MAX_COLOR_LUT_BYTES:
                raise ValueError("scene color LUT byte limit exceeded (%d decoded bytes)" % MAX_COLOR_LUT_BYTES)
            lut_bytes_by_id[lut_id] = len(raw)
            lut_records.append({
                "id": lut_id,
                "size": COLOR_LUT_SIZE,
                "domain_min": COLOR_LUT_DOMAIN_MIN,
                "domain_max": COLOR_LUT_DOMAIN_MAX,
                "shaper": COLOR_LUT_SHAPER,
                "shaper_scale": COLOR_LUT_SHAPER_SCALE,
                "encoding": "rgb-f32-le-base64",
                "data_base64": base64.b64encode(raw).decode("ascii"),
            })
        color_management["lut_id"] = lut_id
    except Exception as error:
        add_unsupported(unsupported, item_id, "color_management_lut",
                        "OCIO sRGB display/view transform could not be baked: " + str(error))
    return color_management


def bake_image_source_transform(image, cache_key="", force_opaque_alpha=False):
    """Convert supported image texels to scene-linear float RGBA before filtering."""
    if cache_key and cache_key in SCENE_LINEAR_FLOAT_PAYLOAD_CACHE:
        return dict(SCENE_LINEAR_FLOAT_PAYLOAD_CACHE[cache_key])
    source = image.colorspace_settings.name
    if source in {"Non-Color", "Linear Rec.709", "sRGB"} and not image.is_float:
        return {"id": "identity", "payload": "", "width": 0, "height": 0, "error": ""}
    try:
        processor = None
        if not image.is_float:
            import PyOpenColorIO as ocio
            config = ocio.GetCurrentConfig()
            scene_linear = config.getRoleColorSpace(ocio.ROLE_SCENE_LINEAR)
            source_space = config.getColorSpace(source)
            if source_space is None:
                raise ValueError("image color space %r is not present in Blender's active OCIO config" % source)
            processor = config.getProcessor(source, scene_linear).getDefaultCPUProcessor()
            transform_id = hashlib.sha256(
                b"OCIO-source-to-scene-linear:" +
                config.getCacheID().encode("utf-8") + b"\0" +
                source.encode("utf-8") + b"\0" +
                scene_linear.encode("utf-8")).hexdigest()
        else:
            # Blender 5.2.2's float image-texture path copies float-buffer
            # texels into its GPU texture without an additional OCIO transform.
            transform_id = "blender-float-buffer"
        width, height = int(image.size[0]), int(image.size[1])
        pixel_count = width * height
        if width <= 0 or height <= 0 or pixel_count > MAX_SCENE_LINEAR_IMAGE_PIXELS:
            raise ValueError("image exceeds the bounded %d-pixel float transport limit" %
                             MAX_SCENE_LINEAR_IMAGE_PIXELS)
        premultiply = getattr(image, "alpha_mode", "STRAIGHT") == "PREMUL"
        source_pixels = image.pixels[:]
        if len(source_pixels) != pixel_count * 4:
            raise ValueError("Blender did not expose a complete RGBA texel array")
        converted = bytearray(pixel_count * 16)
        for pixel_index in range(pixel_count):
            offset = pixel_index * 4
            rgb = [float(source_pixels[offset]), float(source_pixels[offset + 1]),
                   float(source_pixels[offset + 2])]
            alpha = (1.0 if force_opaque_alpha else
                     float(source_pixels[offset + 3]))
            if processor is not None:
                rgb = processor.applyRGB(rgb)
            # Blender premultiplies float image buffers for PREMUL even though
            # that upload path does not run the OCIO processor.
            if premultiply:
                rgb = [component * alpha for component in rgb]
            if not all(math.isfinite(component) for component in rgb + [alpha]):
                raise ValueError("image contains non-finite float texels")
            converted[offset * 4:offset * 4 + 16] = struct.pack(
                "<ffff", rgb[0], rgb[1], rgb[2], alpha)
        payload = bytes(converted)
        if len(payload) > MAX_SCENE_LINEAR_IMAGE_BYTES:
            raise ValueError("image exceeds the 32 MiB float transport limit")
        global SCENE_LINEAR_FLOAT_PAYLOAD_BYTES
        if SCENE_LINEAR_FLOAT_PAYLOAD_BYTES + len(payload) > MAX_SCENE_LINEAR_FLOAT_BYTES:
            raise ValueError("scene-wide scene-linear float texture payload exceeds the 64 MiB transport limit")
        SCENE_LINEAR_FLOAT_PAYLOAD_BYTES += len(payload)
        result = {"id": transform_id,
                  "payload": base64.b64encode(payload).decode("ascii"),
                  "width": width, "height": height, "error": ""}
        if cache_key:
            SCENE_LINEAR_FLOAT_PAYLOAD_CACHE[cache_key] = result
        return result
    except Exception as error:
        return {"id": "", "payload": "", "width": 0, "height": 0,
                "error": "Blender scene-linear image texture conversion failed: " + str(error)}


def require_pinned_blender():
    if tuple(bpy.app.version[:3]) != PINNED_BLENDER_VERSION:
        raise RuntimeError(
            "classiCAD scene extraction requires Blender 5.2.2; found "
            + bpy.app.version_string
        )


def source_library_path(id_block):
    library = getattr(id_block, "library", None)
    return library.filepath if library is not None else ""


def source_library_exists(id_block):
    path = source_library_path(id_block)
    if not path:
        return None
    return os.path.isfile(bpy.path.abspath(path))


def absolute_image_filepath(image):
    """Resolve an image path relative to its owning .blend or linked library."""
    filepath = getattr(image, "filepath", "")
    if not filepath:
        return ""
    return bpy.path.abspath(
        filepath, library=getattr(image, "library", None))


def stable_id(id_block):
    if not SOURCE_PROJECT_NAMESPACE:
        raise RuntimeError("source Blender project namespace was not initialized")
    return (SOURCE_PROJECT_NAMESPACE + "::" + source_library_path(id_block)
            + "::" + id_block.name_full)


def source_project_namespace(filepath):
    if not filepath:
        raise RuntimeError("Blender did not provide the source project filepath")
    normalized_path = os.path.normcase(os.path.realpath(os.path.abspath(filepath)))
    return hashlib.sha256(normalized_path.encode("utf-8")).hexdigest()


def finite_number(value):
    number = float(value)
    if not math.isfinite(number):
        raise ValueError("non-finite numeric value")
    return number


def vector_values(value):
    return [finite_number(component) for component in value]


def matrix_values(matrix):
    return [vector_values(row) for row in matrix]


def add_unsupported(report, item, feature, reason):
    report.append({"item_id": item, "feature": feature, "reason": reason})


def blender_nurbs_knots(point_count, order, cyclic, endpoint, bezier):
    """Mirror Blender 5.2.2 BKE's generated U knot vector."""
    repeat_inner = order - 1 if bezier else 1
    head = order - (1 if cyclic else 0) if endpoint else min(2, repeat_inner)
    tail = 2 * order - 1 if cyclic else (order if endpoint else 0)
    knot_count = point_count + order + (order - 1 if cyclic else 0)
    knots = [0.0] * knot_count
    repeat = head
    current = 0.0
    offset = 1 if endpoint and cyclic else 0
    if offset:
        knots[0] = current
        current += 1.0
    for index in range(offset, knot_count - tail):
        knots[index] = current
        repeat -= 1
        if repeat == 0:
            current += 1.0
            repeat = repeat_inner
    tail_index = knot_count - tail
    for index in range(tail):
        knots[tail_index + index] = current + (knots[index] - knots[0])
    return knots


def image_references(material):
    references = []
    if material is None or not getattr(material, "use_nodes", False):
        return references
    tree = getattr(material, "node_tree", None)
    if tree is None:
        return references
    # Blender stores the Workbench-active texture node independently from the
    # regular active node (RNA exposes NODE_ACTIVE_TEXTURE as show_texture).
    # Do not guess from node-tree order or evaluate a node graph.
    active_node = next((node for node in tree.nodes
                        if getattr(node, "show_texture", False)), None)
    for node in ([active_node] if active_node is not None else []):
        node_type = getattr(node, "type", "")
        if node_type not in {"TEX_IMAGE", "TEX_ENVIRONMENT"}:
            continue
        image = getattr(node, "image", None)
        if image is None:
            continue
        source_transform = (
            {"id": "identity", "payload": "", "width": 0,
             "height": 0, "error": ""}
            if getattr(image, "source", "") == "TILED" else
            bake_image_source_transform(
                image, "image:%s:%s" %
                (image.name_full, source_library_path(image))))
        image_tiles = []
        packed_tile_bytes = 0
        scene_linear_tile_bytes = 0
        packed_tiles = {
            int(getattr(packed, "tile_number", 0)): packed
            for packed in getattr(image, "packed_files", ())
            if int(getattr(packed, "tile_number", 0)) > 0
        }
        if getattr(image, "source", "") == "TILED":
            source_transform = {"id": "identity", "payload": "",
                                "width": 0, "height": 0, "error": ""}
            # Blender Workbench maps floor(UV) to the tile grid (1001 at
            # (0, 0), increasing across X, then Y). Preserve the explicit
            # tile numbers and resolve the conventional <UDIM> path token
            # while the source .blend path context is available.
            filepath_template = absolute_image_filepath(image)
            for tile in getattr(image, "tiles", ()):
                tile_number = int(tile.number)
                tile_path = filepath_template.replace("<UDIM>", str(tile_number))
                packed_tile = packed_tiles.get(tile_number)
                tile_packed_data_base64 = ""
                tile_packed_data_error = ""
                tile_source_transform = {"id": "identity", "payload": "",
                                         "width": 0, "height": 0, "error": ""}
                if packed_tile is not None:
                    try:
                        payload = bytes(packed_tile.packed_file.data)
                        if len(payload) > MAX_PACKED_IMAGE_BYTES:
                            tile_packed_data_error = (
                                "packed UDIM tiles exceed the %d-byte per-image transport limit"
                                % MAX_PACKED_IMAGE_BYTES
                            )
                        elif packed_tile_bytes + len(payload) > MAX_PACKED_IMAGE_BYTES:
                            tile_packed_data_error = (
                                "packed UDIM tiles exceed the %d-byte per-image transport limit"
                                % MAX_PACKED_IMAGE_BYTES
                            )
                        else:
                            packed_tile_bytes += len(payload)
                            tile_packed_data_base64 = base64.b64encode(payload).decode("ascii")
                    except Exception as error:
                        tile_packed_data_error = "could not read packed UDIM tile bytes: " + str(error)
                # Read each tile through Blender so its declared OCIO space is
                # converted per texel before any renderer filters it.
                tile_image = None
                temporary_path = ""
                try:
                    tile_source_bytes = (bytes(packed_tile.packed_file.data)
                                         if packed_tile is not None else b"")
                    if tile_source_bytes:
                        with tempfile.NamedTemporaryFile(delete=False,
                                                         suffix=os.path.splitext(tile_path)[1]) as temp:
                            temp.write(tile_source_bytes)
                            temporary_path = temp.name
                    tile_load_path = temporary_path or tile_path
                    if tile_load_path and os.path.isfile(tile_load_path):
                        tile_image = bpy.data.images.load(tile_load_path,
                                                          check_existing=False)
                        tile_image.colorspace_settings.name = image.colorspace_settings.name
                        tile_image.alpha_mode = image.alpha_mode
                        tile_source_transform = bake_image_source_transform(
                            tile_image, "tile:%s:%s:%d" %
                            (image.name_full, source_library_path(image), tile_number))
                        payload_bytes = len(base64.b64decode(
                            tile_source_transform["payload"])) if tile_source_transform["payload"] else 0
                        if scene_linear_tile_bytes + payload_bytes > MAX_SCENE_LINEAR_IMAGE_BYTES:
                            if payload_bytes:
                                global SCENE_LINEAR_FLOAT_PAYLOAD_BYTES
                                SCENE_LINEAR_FLOAT_PAYLOAD_BYTES -= payload_bytes
                                cache_key = "tile:%s:%s:%d" % (
                                    image.name_full, source_library_path(image), tile_number)
                                SCENE_LINEAR_FLOAT_PAYLOAD_CACHE[cache_key] = {
                                    "id": tile_source_transform["id"],
                                    "payload": "", "width": 0, "height": 0,
                                    "error": "UDIM tiles exceed the 32 MiB per-image float transport limit",
                                }
                            tile_source_transform = {
                                "id": "", "payload": "", "width": 0,
                                "height": 0,
                                "error": "UDIM tiles exceed the 32 MiB per-image float transport limit",
                            }
                        else:
                            scene_linear_tile_bytes += payload_bytes
                except Exception as error:
                    tile_source_transform["error"] = "could not decode UDIM tile for scene-linear conversion: " + str(error)
                finally:
                    if tile_image is not None:
                        bpy.data.images.remove(tile_image)
                    if temporary_path:
                        try:
                            os.unlink(temporary_path)
                        except OSError:
                            pass
                image_tiles.append(
                    {
                        "number": tile_number,
                        "filepath": tile_path,
                        "source_file_exists": bool(
                            tile_path and os.path.isfile(tile_path)
                        ),
                        "packed_data_base64": tile_packed_data_base64,
                        "packed_data_error": tile_packed_data_error,
                        "scene_linear_float_data_base64": tile_source_transform["payload"],
                        "scene_linear_float_width": tile_source_transform["width"],
                        "scene_linear_float_height": tile_source_transform["height"],
                        "source_transform_id": tile_source_transform["id"],
                        "source_transform_error": tile_source_transform["error"],
                    }
                )
        packed_data_base64 = ""
        packed_data_error = ""
        is_tiled = getattr(image, "source", "") == "TILED"
        # Blender exposes per-tile packed payloads for UDIM images. Do not
        # duplicate a tiled image's legacy singular packed_file here.
        packed_file = None if is_tiled else getattr(image, "packed_file", None)
        if packed_file is not None:
            packed_size = int(getattr(packed_file, "size", len(packed_file.data)))
            if packed_size > MAX_PACKED_IMAGE_BYTES:
                packed_data_error = (
                    "packed image exceeds the %d-byte transport limit"
                    % MAX_PACKED_IMAGE_BYTES
                )
            else:
                packed_data = bytes(packed_file.data)
                if len(packed_data) > MAX_PACKED_IMAGE_BYTES:
                    packed_data_error = (
                        "packed image exceeds the %d-byte transport limit"
                        % MAX_PACKED_IMAGE_BYTES
                    )
                    packed_data = b""
            if not packed_data_error:
                packed_data_base64 = base64.b64encode(packed_data).decode("ascii")
        references.append(
            {
                "node_name": node.name,
                "node_type": node_type,
                "image_name": image.name_full,
                "image_library_path": source_library_path(image),
                "image_library_path_exists": source_library_exists(image),
                # Resolve Blender's // path against the source .blend while
                # still in Blender; classiCAD never interprets Blender paths.
                "filepath": absolute_image_filepath(image),
                "packed": packed_file is not None or bool(packed_tiles),
                "packed_data_base64": packed_data_base64,
                "packed_data_error": packed_data_error,
                "source_file_exists": (
                    bool(packed_file is not None or any(
                        tile["source_file_exists"] or tile["packed_data_base64"]
                        for tile in image_tiles)) or
                    (os.path.isfile(absolute_image_filepath(image))
                     if image.filepath else False)
                ),
                "tiles": image_tiles,
                "color_space": image.colorspace_settings.name,
                "source_transform_id": source_transform["id"],
                "scene_linear_float_data_base64": source_transform["payload"],
                "scene_linear_float_width": source_transform["width"],
                "scene_linear_float_height": source_transform["height"],
                "source_transform_error": source_transform["error"],
                "alpha_mode": image.alpha_mode,
                "interpolation": getattr(node, "interpolation", "Linear"),
                # Environment Texture has no extension control. Workbench's
                # default sampler uses Extend; Image Texture defaults Repeat.
                "extension": getattr(
                    node, "extension",
                    "EXTEND" if node_type == "TEX_ENVIRONMENT" else "REPEAT"),
            }
        )
    return references


def material_record(material):
    if material is None:
        return None
    diffuse = material.diffuse_color
    return {
        "material_id": stable_id(material),
        "name": material.name_full,
        "library_path": source_library_path(material),
        "library_path_exists": source_library_exists(material),
        "diffuse_color_linear": vector_values(diffuse),
        "roughness": finite_number(material.roughness),
        "metallic": finite_number(material.metallic),
        "alpha": finite_number(diffuse[3]),
        "image_textures": image_references(material),
    }


def attribute_value(element, data_type):
    if data_type in {"FLOAT_COLOR", "BYTE_COLOR"}:
        value = {"color_linear": vector_values(element.color)}
        if hasattr(element, "color_srgb"):
            value["color_srgb"] = vector_values(element.color_srgb)
        return value
    if data_type in {"FLOAT", "INT", "BOOLEAN", "STRING"}:
        return {"value": element.value}
    if data_type in {"FLOAT_VECTOR", "FLOAT2", "FLOAT3"}:
        if hasattr(element, "vector"):
            return {"vector": vector_values(element.vector)}
        if hasattr(element, "value"):
            return {"value": vector_values(element.value)}
    if data_type == "QUATERNION" and hasattr(element, "value"):
        return {"quaternion": vector_values(element.value)}
    raise NotImplementedError("unsupported Blender attribute data type " + data_type)


def extract_attributes(mesh, item_id, unsupported):
    result = []
    supported_domains = {"POINT", "CORNER", "FACE"}
    for attribute in mesh.attributes:
        domain = attribute.domain
        data_type = attribute.data_type
        record = {
            "name": attribute.name,
            "domain": domain,
            "data_type": data_type,
            "color_space_interpretation": (
                "linear" if data_type in {"FLOAT_COLOR", "BYTE_COLOR"}
                else "not_color"
            ),
        }
        try:
            if domain not in supported_domains:
                raise NotImplementedError("unsupported attribute domain " + domain)
            record["values"] = [
                attribute_value(value, data_type) for value in attribute.data
            ]
        except (NotImplementedError, AttributeError, ValueError) as error:
            record["values"] = None
            add_unsupported(unsupported, item_id,
                            "attribute:" + attribute.name, str(error))
        result.append(record)
    return result


def resolved_render_filepath(render):
    """Return Blender's absolute render path without expanding frame tokens."""
    return bpy.path.abspath(render.filepath) if render.filepath else ""


def visibility_metadata(obj):
    try:
        hidden_in_viewport = bool(obj.hide_get())
    except (RuntimeError, TypeError):
        hidden_in_viewport = bool(obj.hide_viewport)
    return {
        "hide_viewport": bool(obj.hide_viewport),
        "hide_render": bool(obj.hide_render),
        "hidden_in_viewport": hidden_in_viewport,
        "hide_select": bool(obj.hide_select),
        "hide_surface_pick": bool(obj.hide_surface_pick),
    }


def data_block_animation_metadata(data_block, remaining_rows=None):
    animation_data = getattr(data_block, "animation_data", None)
    action = getattr(animation_data, "action", None)
    action_slot = getattr(animation_data, "action_slot", None)
    drivers = getattr(animation_data, "drivers", ())
    nla_track_collection = getattr(animation_data, "nla_tracks", ())
    driver_target_names = []
    result = {
        "has_animation_data": False,
        "action_name": "",
        "action_slot_name": "",
        "has_drivers": False,
        "driver_target_names": driver_target_names,
        "nla_tracks": [],
        "truncated": False,
    }
    if animation_data is None or not (action is not None or drivers or
                                      nla_track_collection):
        return result

    local_rows = [MAX_OUTLINER_HIERARCHY_ITEMS]
    budgets = [local_rows]
    if remaining_rows is not None:
        budgets.append(remaining_rows)
    truncated = [False]

    def reserve_rows(count=1):
        if any(budget[0] < count for budget in budgets):
            truncated[0] = True
            return False
        for budget in budgets:
            budget[0] -= count
        return True

    if not reserve_rows():
        result["truncated"] = True
        return result
    result["has_animation_data"] = True

    if action is not None:
        if reserve_rows():
            result["action_name"] = action.name
            if action_slot is not None and reserve_rows():
                result["action_slot_name"] = getattr(
                    action_slot, "name_display", "")

    if drivers:
        if reserve_rows():
            result["has_drivers"] = True
            last_added_target = None
            for fcurve_index, fcurve in enumerate(drivers):
                if fcurve_index >= MAX_OUTLINER_HIERARCHY_ITEMS:
                    truncated[0] = True
                    break
                driver = fcurve.driver
                if driver is None or not driver.variables:
                    continue
                stop_targets = False
                for variable in driver.variables:
                    for target in variable.targets:
                        target_id = target.id
                        if target_id is None or target_id == last_added_target:
                            continue
                        if not reserve_rows():
                            stop_targets = True
                            break
                        driver_target_names.append(target_id.name)
                        last_added_target = target_id
                    if stop_targets:
                        break
                if stop_targets:
                    break

    nla_tracks = []
    for track_index, track in enumerate(nla_track_collection):
        if track_index >= MAX_OUTLINER_HIERARCHY_ITEMS:
            truncated[0] = True
            break
        if track_index == 0:
            if not reserve_rows(2):
                break
        elif not reserve_rows():
            break
        action_names = []
        for strip_index, strip in enumerate(track.strips):
            if strip_index >= MAX_OUTLINER_HIERARCHY_ITEMS:
                truncated[0] = True
                break
            if strip.action is not None:
                if not reserve_rows():
                    break
                action_names.append(strip.action.name)
        nla_tracks.append({"name": track.name, "action_names": action_names})
    result["nla_tracks"] = nla_tracks
    result["driver_target_names"] = driver_target_names
    result["truncated"] = truncated[0]
    return result


def outliner_name_tree(items, include_constraints=False, remaining_rows=None,
                       truncated_out=None):
    """Bounded Blender name/parent tree used by display-only Outliner rows."""
    local_remaining = [MAX_OUTLINER_HIERARCHY_ITEMS]
    budgets = [local_remaining]
    if remaining_rows is not None:
        budgets.append(remaining_rows)
    truncated = [False]

    def reserve_rows(count=1):
        if any(budget[0] < count for budget in budgets):
            truncated[0] = True
            return False
        for budget in budgets:
            budget[0] -= count
        return True

    def convert(item, depth):
        if depth >= 128:
            truncated[0] = True
            return None
        if not reserve_rows():
            return None
        result = {"name": str(item.name), "children": []}
        if include_constraints:
            constraints = []
            for index, constraint in enumerate(getattr(item, "constraints", ())):
                if index >= MAX_OUTLINER_HIERARCHY_ITEMS:
                    truncated[0] = True
                    break
                if not reserve_rows(2 if index == 0 else 1):
                    break
                constraints.append(str(constraint.name))
            result["constraints"] = constraints
        for child in getattr(item, "children", ()):
            row = convert(child, depth + 1)
            if row is not None:
                result["children"].append(row)
            if any(budget[0] <= 0 for budget in budgets):
                truncated[0] = True
                break
        return result

    rows = []
    for item in items:
        if getattr(item, "parent", None) is not None:
            continue
        row = convert(item, 0)
        if row is not None:
            rows.append(row)
        if any(budget[0] <= 0 for budget in budgets):
            truncated[0] = True
            break
    if truncated_out is not None:
        truncated_out[:] = [truncated[0]]
    return rows


def outliner_grease_pencil_data_layer_tree(data, legacy=False,
                                           remaining_rows=None,
                                           truncated_out=None):
    """Capture bounded Blender Grease Pencil layer/group rows from an ID."""
    local_remaining = [MAX_OUTLINER_HIERARCHY_ITEMS]
    budgets = [local_remaining]
    if remaining_rows is not None:
        budgets.append(remaining_rows)
    truncated = [False]

    def reserve_row():
        if any(budget[0] <= 0 for budget in budgets):
            truncated[0] = True
            return False
        for budget in budgets:
            budget[0] -= 1
        return True

    rows = []
    try:
        if legacy:
            layers = getattr(data, "layers", None)
            if layers is not None:
                layer_count = len(layers)
                first_index = max(
                    0, layer_count - MAX_OUTLINER_HIERARCHY_ITEMS)
                if layer_count > MAX_OUTLINER_HIERARCHY_ITEMS:
                    truncated[0] = True
                for index in range(layer_count - 1, first_index - 1, -1):
                    if not reserve_row():
                        break
                    rows.append({
                        "name": str(layers[index].name),
                        "is_group": False,
                        "children": [],
                    })
        else:
            def convert(node, depth):
                if depth >= 128:
                    truncated[0] = True
                    return None
                if not reserve_row():
                    return None
                is_group = (
                    getattr(getattr(node, "bl_rna", None), "identifier", "") ==
                    "GreasePencilLayerGroup"
                )
                children = []
                if is_group:
                    source_children = getattr(node, "children", ())
                    child_count = len(source_children)
                    if child_count > MAX_OUTLINER_HIERARCHY_ITEMS:
                        truncated[0] = True
                    first_index = max(
                        0, child_count - MAX_OUTLINER_HIERARCHY_ITEMS)
                    for index in range(child_count - 1, first_index - 1, -1):
                        child_row = convert(source_children[index], depth + 1)
                        if child_row is not None:
                            children.append(child_row)
                        if any(budget[0] <= 0 for budget in budgets):
                            truncated[0] = True
                            break
                return {
                    "name": str(node.name),
                    "is_group": is_group,
                    "children": children,
                }

            root_nodes = getattr(data, "root_nodes", ())
            root_count = len(root_nodes)
            if root_count > MAX_OUTLINER_HIERARCHY_ITEMS:
                truncated[0] = True
            first_index = max(0, root_count - MAX_OUTLINER_HIERARCHY_ITEMS)
            for index in range(root_count - 1, first_index - 1, -1):
                row = convert(root_nodes[index], 0)
                if row is not None:
                    rows.append(row)
                if any(budget[0] <= 0 for budget in budgets):
                    truncated[0] = True
                    break
    except (AttributeError, ReferenceError, RuntimeError, TypeError):
        truncated[0] = True

    if truncated_out is not None:
        truncated_out[:] = [truncated[0]]
    return rows


def outliner_grease_pencil_layer_tree(obj):
    """Capture the Blender 5.2 Grease Pencil data-block's layer tree."""
    data = getattr(obj, "data", None)
    if data is None:
        return []
    if obj.type == "GPENCIL":
        return outliner_grease_pencil_data_layer_tree(data, legacy=True)
    if obj.type == "GREASEPENCIL":
        return outliner_grease_pencil_data_layer_tree(data)
    return []


def outliner_library_object_tree(obj, parent_budgets=None, truncated=None,
                                 id_group_names_by_type=None):
    """Capture Blender Object ID's direct children in source order."""
    local_remaining = [MAX_OUTLINER_HIERARCHY_ITEMS]
    budgets = list(parent_budgets or []) + [local_remaining]
    if truncated is None:
        truncated = [False]
    rows = []

    def reserve_row():
        if any(budget[0] <= 0 for budget in budgets):
            truncated[0] = True
            return False
        for budget in budgets:
            budget[0] -= 1
        return True

    def id_group_name_for_id(id_block):
        if id_group_names_by_type is None or id_block is None:
            return ""
        id_type = getattr(getattr(id_block, "bl_rna", None), "identifier", "")
        return str(id_group_names_by_type.get(id_type, ""))

    def add_leaf(name, kind, id_group_name=""):
        if not name or not reserve_row():
            return False
        node = {"name": str(name), "kind": kind, "children": []}
        if kind in {"DATA_BLOCK", "MATERIAL"}:
            node["id_group_name"] = str(id_group_name)
        rows.append(node)
        return True

    def add_group_to(parent_rows, name, kind, entries, child_kind):
        group = None
        try:
            for entry in entries:
                entry_name = str(entry.name)
                if not entry_name:
                    continue
                if group is None:
                    if any(budget[0] < 2 for budget in budgets):
                        truncated[0] = True
                        return
                    if not reserve_row():
                        return
                    group = {"name": name, "kind": kind, "children": []}
                    parent_rows.append(group)
                if not reserve_row():
                    return
                group["children"].append({
                    "name": entry_name, "kind": child_kind, "children": []
                })
        except (AttributeError, ReferenceError, RuntimeError, TypeError):
            truncated[0] = True

    def add_group(name, kind, entries, child_kind):
        add_group_to(rows, name, kind, entries, child_kind)

    try:
        pose = getattr(obj, "pose", None)
        if pose is not None and reserve_row():
            pose_node = {"name": "Pose", "kind": "POSE", "children": []}
            rows.append(pose_node)
            if getattr(obj, "mode", "") == "POSE":
                pose_bone_records = []
                pose_bones = getattr(pose, "bones", ())
                try:
                    for bone in pose_bones:
                        if not reserve_row():
                            break
                        name = str(bone.name)
                        parent = getattr(bone, "parent", None)
                        bone_node = {
                            "name": name, "kind": "POSE_BONE", "children": []
                        }
                        add_group_to(
                            bone_node["children"], "Constraints", "CONSTRAINTS",
                            getattr(bone, "constraints", ()), "CONSTRAINT")
                        pose_node["children"].append(bone_node)
                        pose_bone_records.append({
                            "name": name,
                            "parent": str(parent.name) if parent is not None else "",
                            "node": bone_node,
                        })
                except (AttributeError, ReferenceError, RuntimeError, TypeError):
                    truncated[0] = True

                records_by_name = {
                    record["name"]: record for record in pose_bone_records
                }
                for record in pose_bone_records:
                    depth = 0
                    parent_name = record["parent"]
                    ancestry = {record["name"]}
                    while parent_name in records_by_name:
                        if parent_name in ancestry:
                            truncated[0] = True
                            depth = 125
                            break
                        ancestry.add(parent_name)
                        depth += 1
                        parent_name = records_by_name[parent_name]["parent"]
                    if depth >= 124:
                        truncated[0] = True
                        continue
                    parent_record = records_by_name.get(record["parent"])
                    if parent_record is not None:
                        pose_node["children"].remove(record["node"])
                        parent_record["node"]["children"].append(record["node"])
        data = getattr(obj, "data", None)
        if data is not None:
            add_leaf(getattr(data, "name", ""), "DATA_BLOCK",
                     id_group_name_for_id(data))

        try:
            for slot in getattr(obj, "material_slots", ()):
                material = getattr(slot, "material", None)
                if material is not None and not add_leaf(
                        getattr(material, "name", ""), "MATERIAL",
                        id_group_name_for_id(material)):
                    break
        except (AttributeError, ReferenceError, RuntimeError, TypeError):
            truncated[0] = True

        add_group("Constraints", "CONSTRAINTS",
                  getattr(obj, "constraints", ()), "CONSTRAINT")

        def object_modifiers():
            for property_name in ("modifiers", "grease_pencil_modifiers"):
                try:
                    for modifier in getattr(obj, property_name, ()):
                        yield modifier
                except (AttributeError, ReferenceError, RuntimeError, TypeError):
                    truncated[0] = True

        add_group("Modifiers", "MODIFIERS", object_modifiers(), "MODIFIER")

        effects = getattr(obj, "shader_effects", None)
        if effects is None:
            effects = getattr(obj, "shader_fx", ())
        add_group("Effects", "EFFECTS", effects, "EFFECT")

        if getattr(obj, "type", "") in {"MESH", "LATTICE", "GREASEPENCIL"}:
            add_group("Vertex Groups", "VERTEX_GROUPS",
                      getattr(obj, "vertex_groups", ()), "VERTEX_GROUP")

        instance_collection = getattr(obj, "instance_collection", None)
        if (instance_collection is not None and
                getattr(obj, "instance_type", "") == "COLLECTION"):
            add_leaf(getattr(instance_collection, "name", ""),
                     "COLLECTION_INSTANCE")
    except (AttributeError, ReferenceError, RuntimeError, TypeError):
        truncated[0] = True

    return rows


def outliner_library_collection_children(collection, remaining_rows,
                                         truncated_out=None,
                                         id_group_names_by_type=None,
                                         additional_budgets=None):
    """Capture Collection ID children in Blender's collection-then-object order."""
    local_remaining = [MAX_OUTLINER_HIERARCHY_ITEMS]
    budgets = [local_remaining]
    if remaining_rows is not None:
        budgets.append(remaining_rows)
    budgets.extend(additional_budgets or [])
    truncated = [False]

    def id_group_name_for_id(id_block):
        if id_group_names_by_type is None or id_block is None:
            return ""
        id_type = getattr(getattr(id_block, "bl_rna", None), "identifier", "")
        return str(id_group_names_by_type.get(id_type, ""))

    def reserve_row():
        if any(budget[0] <= 0 for budget in budgets):
            truncated[0] = True
            return False
        for budget in budgets:
            budget[0] -= 1
        return True

    def convert_collection(child, depth, ancestry):
        if depth >= 128:
            truncated[0] = True
            return None
        child_id = stable_id(child)
        if child_id in ancestry:
            truncated[0] = True
            return None
        if not reserve_row():
            return None
        next_ancestry = ancestry | {child_id}
        node = {"name": str(child.name), "kind": "COLLECTION", "children": []}
        try:
            nested_collections = child.children
            for nested in nested_collections:
                nested_node = convert_collection(
                    nested, depth + 1, next_ancestry)
                if nested_node is not None:
                    node["children"].append(nested_node)
                if any(budget[0] <= 0 for budget in budgets):
                    truncated[0] = True
                    break
            if any(budget[0] <= 0 for budget in budgets):
                return node
            for obj in child.objects:
                if not reserve_row():
                    break
                object_truncated = [False]
                object_node = {
                    "name": str(obj.name), "kind": "OBJECT",
                    "id_group_name": id_group_name_for_id(obj),
                    "children": outliner_library_object_tree(
                        obj, budgets, object_truncated,
                        id_group_names_by_type),
                }
                node["children"].append(object_node)
                if object_truncated[0]:
                    truncated[0] = True
        except (AttributeError, ReferenceError, RuntimeError, TypeError):
            truncated[0] = True
        return node

    children = []
    ancestry = {stable_id(collection)}
    try:
        for child in collection.children:
            node = convert_collection(child, 0, ancestry)
            if node is not None:
                children.append(node)
            if any(budget[0] <= 0 for budget in budgets):
                truncated[0] = True
                break
        if not any(budget[0] <= 0 for budget in budgets):
            for obj in collection.objects:
                if not reserve_row():
                    break
                object_truncated = [False]
                object_node = {
                    "name": str(obj.name), "kind": "OBJECT",
                    "id_group_name": id_group_name_for_id(obj),
                    "children": outliner_library_object_tree(
                        obj, budgets, object_truncated,
                        id_group_names_by_type),
                }
                children.append(object_node)
                if object_truncated[0]:
                    truncated[0] = True
    except (AttributeError, ReferenceError, RuntimeError, TypeError):
        truncated[0] = True

    if truncated_out is not None:
        truncated_out[:] = [truncated[0]]
    return children


def outliner_library_scene_tree(scene, parent_budgets,
                                id_group_names_by_type, truncated_out=None):
    """Capture Scene ID children in Blender's view-layer/world/collection/object order."""
    remaining = [MAX_OUTLINER_HIERARCHY_ITEMS]
    budgets = [remaining] + list(parent_budgets or [])
    truncated = [False]

    def reserve_row():
        if any(budget[0] <= 0 for budget in budgets):
            truncated[0] = True
            return False
        for budget in budgets:
            budget[0] -= 1
        return True

    def id_group_name_for_id(id_block):
        if id_group_names_by_type is None or id_block is None:
            return ""
        id_type = getattr(getattr(id_block, "bl_rna", None), "identifier", "")
        return str(id_group_names_by_type.get(id_type, ""))

    result = {
        "has_view_layers_group": False,
        "view_layer_names": [],
        "world_name": "",
        "world_group_name": "",
        "has_scene_collection_group": False,
        "collection_tree": [],
        "has_objects_group": False,
        "object_tree": [],
    }
    if reserve_row():
        result["has_view_layers_group"] = True
        for view_layer in scene.view_layers:
            if not reserve_row():
                break
            result["view_layer_names"].append(str(view_layer.name))

    world = getattr(scene, "world", None)
    if world is not None and reserve_row():
        result["world_name"] = str(world.name)
        result["world_group_name"] = id_group_name_for_id(world)

    master_collection = getattr(scene, "master_collection", None)
    if master_collection is None:
        master_collection = getattr(scene, "collection", None)
    if master_collection is not None and reserve_row():
        result["has_scene_collection_group"] = True
        if reserve_row():
            collection_truncated = []
            children = outliner_library_collection_children(
                master_collection, remaining, collection_truncated,
                id_group_names_by_type, budgets[1:])
            result["collection_tree"].append({
                "name": str(master_collection.name),
                "kind": "COLLECTION",
                "children": children,
            })
            if collection_truncated == [True]:
                truncated[0] = True

    if reserve_row():
        result["has_objects_group"] = True
        records = []
        records_by_id = {}
        for obj in scene.objects:
            if not reserve_row():
                break
            object_id = stable_id(obj)
            object_truncated = []
            node = {
                "name": str(obj.name),
                "kind": "OBJECT",
                "id_group_name": id_group_name_for_id(obj),
                "object_contents": outliner_library_object_tree(
                    obj, budgets, object_truncated,
                    id_group_names_by_type),
                "children": [],
            }
            if object_truncated == [True]:
                truncated[0] = True
            parent = getattr(obj, "parent", None)
            parent_id = stable_id(parent) if parent is not None else ""
            record = {"id": object_id, "parent_id": parent_id, "node": node}
            records.append(record)
            records_by_id[object_id] = record

        roots = []
        for record in records:
            parent_id = record["parent_id"]
            parent_record = records_by_id.get(parent_id)
            if parent_record is None:
                roots.append(record["node"])
                continue
            cursor = parent_id
            ancestry = {record["id"]}
            depth = 0
            cyclic = False
            while cursor in records_by_id:
                if cursor in ancestry or depth >= 128:
                    cyclic = True
                    break
                ancestry.add(cursor)
                cursor = records_by_id[cursor]["parent_id"]
                depth += 1
            if cyclic:
                truncated[0] = True
                roots.append(record["node"])
            else:
                parent_record["node"]["children"].append(record["node"])
        result["object_tree"] = roots

    if truncated_out is not None:
        truncated_out[:] = [truncated[0]]
    return result


def outliner_library_tree():
    """Capture Blender's linked-library/type grouping for Libraries mode."""
    current_file = {
        "id": "CURRENT_FILE",
        "name": "Current File",
        "filepath": "",
        "parent_library_id": "",
        "is_current_file": True,
        "data_blocks": [],
        "truncated": False,
    }
    libraries = [current_file]
    records_by_id = {current_file["id"]: current_file}
    for library in list(bpy.data.libraries)[:MAX_OUTLINER_HIERARCHY_ITEMS - 1]:
        library_id = stable_id(library)
        parent = getattr(library, "parent", None)
        record = {
            "id": library_id,
            "name": str(library.filepath),
            "filepath": str(library.filepath),
            "parent_library_id": stable_id(parent) if parent is not None else "",
            "is_current_file": False,
            "data_blocks": [],
            "truncated": False,
        }
        libraries.append(record)
        records_by_id[library_id] = record

    groups_by_library = {}
    seen_ids = set()
    row_count = 0
    action_slot_count = 0
    library_animation_rows_remaining = [MAX_OUTLINER_LIBRARY_ANIMATION_ITEMS]
    library_specialized_rows_remaining = [MAX_OUTLINER_LIBRARY_SPECIALIZED_ITEMS]
    id_group_names_by_type = {}
    for property_rna in bpy.data.bl_rna.properties:
        if getattr(property_rna, "type", "") != "COLLECTION":
            continue
        fixed_type = getattr(property_rna, "fixed_type", None)
        id_type = str(getattr(fixed_type, "identifier", ""))
        group_name = str(getattr(property_rna, "name", ""))
        if id_type and group_name:
            id_group_names_by_type.setdefault(id_type, group_name)
    for property_rna in bpy.data.bl_rna.properties:
        if getattr(property_rna, "type", "") != "COLLECTION":
            continue
        try:
            collection = getattr(bpy.data, property_rna.identifier)
        except (AttributeError, RuntimeError, TypeError):
            continue
        for id_block in collection:
            if not isinstance(id_block, bpy.types.ID):
                continue
            id_key = stable_id(id_block)
            if id_key in seen_ids:
                continue
            seen_ids.add(id_key)
            library = getattr(id_block, "library", None)
            library_id = stable_id(library) if library is not None else "CURRENT_FILE"
            record = records_by_id.get(library_id)
            if record is None:
                continue
            if row_count >= MAX_OUTLINER_LIBRARY_ITEMS:
                record["truncated"] = True
                continue
            row_count += 1
            groups = groups_by_library.setdefault(library_id, {})
            group_name = str(property_rna.name)
            group = groups.get(group_name)
            if group is None:
                group = {
                    "name": group_name,
                    "names": [],
                    "animations": [],
                    "action_slots": [],
                    "data_block_subtrees": [],
                }
                groups[group_name] = group
                record["data_blocks"].append(group)
            # Blender's tree_element_id.cc displays ID.name + 2, which maps to
            # RNA ID.name (without name_full's linked-library suffix).
            data_block_name = str(id_block.name)
            group["names"].append(data_block_name)
            if id_block.bl_rna.identifier not in {"Action", "Collection"}:
                animation = data_block_animation_metadata(
                    id_block, library_animation_rows_remaining)
                if animation["truncated"]:
                    record["truncated"] = True
                if animation["has_animation_data"]:
                    group["animations"].append({
                        "data_block_name": data_block_name,
                        "animation": animation,
                    })
            id_type = id_block.bl_rna.identifier
            if (id_type in {"Mesh", "Curve", "MetaBall", "Armature", "Collection", "Object", "Texture", "FreestyleLineStyle", "Scene"}
                    or "GreasePencil" in id_type):
                subtree = {
                    "data_block_name": data_block_name,
                    "shape_key_data_name": "",
                    "shape_key_names": [],
                    "material_names": [],
                    "armature_bone_tree": [],
                    "bone_collection_tree": [],
                    "grease_pencil_layer_tree": [],
                    "collection_tree": [],
                    "object_tree": [],
                    "texture_image_name": "",
                    "line_style_texture_names": [],
                    "scene_tree": {},
                }
                local_rows_remaining = [MAX_OUTLINER_HIERARCHY_ITEMS]
                subtree_truncated = [False]

                def reserve_subtree_row():
                    if (local_rows_remaining[0] <= 0 or
                            library_specialized_rows_remaining[0] <= 0):
                        subtree_truncated[0] = True
                        return False
                    local_rows_remaining[0] -= 1
                    library_specialized_rows_remaining[0] -= 1
                    return True

                if id_type == "Mesh":
                    shape_keys = getattr(id_block, "shape_keys", None)
                    if shape_keys is not None:
                        if reserve_subtree_row():
                            subtree["shape_key_data_name"] = str(shape_keys.name)
                            try:
                                for index, key_block in enumerate(shape_keys.key_blocks):
                                    if index >= MAX_OUTLINER_HIERARCHY_ITEMS:
                                        subtree_truncated[0] = True
                                        break
                                    if not reserve_subtree_row():
                                        break
                                    subtree["shape_key_names"].append(
                                        str(key_block.name))
                            except (AttributeError, ReferenceError, RuntimeError, TypeError):
                                subtree_truncated[0] = True

                try:
                    materials = getattr(id_block, "materials", ())
                    for index, material in enumerate(materials):
                        if index >= MAX_OUTLINER_HIERARCHY_ITEMS:
                            subtree_truncated[0] = True
                            break
                        if material is None:
                            continue
                        if not reserve_subtree_row():
                            break
                        subtree["material_names"].append(str(material.name))
                except (AttributeError, ReferenceError, RuntimeError, TypeError):
                    subtree_truncated[0] = True

                if id_type == "Armature":
                    bone_tree_truncated = []
                    collection_tree_truncated = []
                    try:
                        subtree["armature_bone_tree"] = outliner_name_tree(
                            getattr(id_block, "bones", ()),
                            remaining_rows=library_specialized_rows_remaining,
                            truncated_out=bone_tree_truncated)
                        subtree["bone_collection_tree"] = outliner_name_tree(
                            getattr(id_block, "collections", ()),
                            remaining_rows=library_specialized_rows_remaining,
                            truncated_out=collection_tree_truncated)
                    except (AttributeError, ReferenceError, RuntimeError, TypeError):
                        subtree_truncated[0] = True
                    if (bone_tree_truncated == [True] or
                            collection_tree_truncated == [True]):
                        subtree_truncated[0] = True

                if "GreasePencil" in id_type:
                    grease_pencil_tree_truncated = []
                    subtree["grease_pencil_layer_tree"] = (
                        outliner_grease_pencil_data_layer_tree(
                            id_block,
                            legacy=not hasattr(id_block, "root_nodes"),
                            remaining_rows=library_specialized_rows_remaining,
                            truncated_out=grease_pencil_tree_truncated))
                    if grease_pencil_tree_truncated == [True]:
                        subtree_truncated[0] = True

                if id_type == "Collection":
                    collection_tree_truncated = []
                    subtree["collection_tree"] = (
                        outliner_library_collection_children(
                            id_block, library_specialized_rows_remaining,
                            collection_tree_truncated,
                            id_group_names_by_type))
                    if collection_tree_truncated == [True]:
                        subtree_truncated[0] = True

                if id_type == "Object":
                    object_tree_truncated = []
                    subtree["object_tree"] = outliner_library_object_tree(
                        id_block,
                        [library_specialized_rows_remaining],
                        object_tree_truncated,
                        id_group_names_by_type)
                    if object_tree_truncated == [True]:
                        subtree_truncated[0] = True

                if id_type == "Texture":
                    image = getattr(id_block, "image", None)
                    if image is not None:
                        if reserve_subtree_row():
                            subtree["texture_image_name"] = str(image.name)

                if id_type == "FreestyleLineStyle":
                    try:
                        for texture_slot in getattr(id_block, "texture_slots", ()):
                            if texture_slot is None:
                                continue
                            texture = getattr(texture_slot, "texture", None)
                            if texture is None:
                                continue
                            if not reserve_subtree_row():
                                break
                            subtree["line_style_texture_names"].append(
                                str(texture.name))
                    except (AttributeError, ReferenceError, RuntimeError, TypeError):
                        subtree_truncated[0] = True

                if id_type == "Scene":
                    scene_tree_truncated = []
                    subtree["scene_tree"] = outliner_library_scene_tree(
                        id_block,
                        [local_rows_remaining,
                         library_specialized_rows_remaining],
                        id_group_names_by_type,
                        scene_tree_truncated)
                    if scene_tree_truncated == [True]:
                        subtree_truncated[0] = True

                if subtree_truncated[0]:
                    record["truncated"] = True
                if (subtree["shape_key_data_name"] or
                        subtree["material_names"] or
                        subtree["armature_bone_tree"] or
                        subtree["bone_collection_tree"] or
                        subtree["grease_pencil_layer_tree"] or
                        subtree["collection_tree"] or
                        subtree["object_tree"] or
                        subtree["texture_image_name"] or
                        subtree["line_style_texture_names"] or
                        subtree["scene_tree"]):
                    group["data_block_subtrees"].append(subtree)
            if id_block.bl_rna.identifier == "Action":
                slots = []
                try:
                    for slot in getattr(id_block, "slots", ()):
                        if action_slot_count >= MAX_OUTLINER_ACTION_SLOTS:
                            record["truncated"] = True
                            break
                        try:
                            identifier = str(getattr(slot, "identifier", ""))[:256]
                            display_name = str(
                                getattr(slot, "name_display", "") or
                                (identifier[2:] if len(identifier) > 2 else identifier))
                        except (AttributeError, ReferenceError, RuntimeError, TypeError):
                            record["truncated"] = True
                            continue
                        slots.append({
                            "name": display_name[:4096],
                            "identifier": identifier,
                        })
                        action_slot_count += 1
                except (AttributeError, ReferenceError, RuntimeError, TypeError):
                    record["truncated"] = True
                group["action_slots"].append({
                    "action_name": data_block_name,
                    "slots": slots,
                })

    return libraries


def outliner_orphan_groups():
    """Capture Blender's zero-user ID rows for Orphan Data mode."""
    groups = []
    groups_by_name = {}
    seen_ids = set()
    row_count = 0
    never_unused_types = {"Library", "Scene", "WorkSpace"}
    for property_rna in bpy.data.bl_rna.properties:
        if getattr(property_rna, "type", "") != "COLLECTION":
            continue
        try:
            collection = getattr(bpy.data, property_rna.identifier)
        except (AttributeError, RuntimeError, TypeError):
            continue
        for id_block in collection:
            if not isinstance(id_block, bpy.types.ID):
                continue
            id_key = stable_id(id_block)
            if id_key in seen_ids:
                continue
            seen_ids.add(id_key)
            if (id_block.bl_rna.identifier in never_unused_types or
                    int(id_block.users) > 0):
                continue
            group_name = str(property_rna.name)
            group = groups_by_name.get(group_name)
            if group is None:
                group = {"name": group_name, "names": [], "truncated": False}
                groups_by_name[group_name] = group
                groups.append(group)
            if row_count >= MAX_OUTLINER_ORPHAN_ITEMS:
                group["truncated"] = True
                continue
            row_count += 1
            group["names"].append(str(id_block.name_full))
    return groups


def outliner_library_overrides_tree():
    """Capture Blender's Library Overrides Properties and Hierarchies trees."""
    def identity(value):
        try:
            return int(value.as_pointer())
        except (AttributeError, ReferenceError, RuntimeError, TypeError):
            return id(value)

    def display_value(value):
        if value is None:
            return ""
        if isinstance(value, (str, bool, int, float)):
            return str(value)[:4096]
        if isinstance(value, bpy.types.ID):
            return str(value.name_full)[:4096]
        try:
            values = list(value)
        except (ReferenceError, RuntimeError, TypeError):
            values = None
        if values is not None:
            if len(values) > 16:
                values = values[:16]
                suffix = ", …"
            else:
                suffix = ""
            if all(isinstance(item, (bool, int, float)) for item in values):
                return (", ".join(str(item) for item in values) + suffix)[:4096]
        try:
            return str(value)[:4096]
        except (ReferenceError, RuntimeError, TypeError):
            return ""

    def property_path_segments(id_block, path):
        """Resolve Blender RNA names along a stored override property path."""
        segments = []
        current = id_block
        last_property = None
        token_pattern = re.compile(
            r'(?P<property>[A-Za-z_][A-Za-z0-9_]*)|'
            r'\[(?P<index>[0-9]+)\]|'
            r'\[(?P<key>"(?:\\.|[^"\\])*")\]'
        )
        for token in token_pattern.finditer(str(path)):
            property_name = token.group("property")
            if property_name is not None:
                try:
                    rna = getattr(current, "bl_rna", None)
                    last_property = (
                        rna.properties.get(property_name)
                        if rna is not None else None
                    )
                    label = (str(last_property.name) if last_property is not None
                             else property_name.replace("_", " ").title())
                    value = getattr(current, property_name)
                except (AttributeError, ReferenceError, RuntimeError, TypeError):
                    label = property_name.replace("_", " ").title()
                    value = None
                    last_property = None
                segments.append({
                    "key": "property:" + property_name,
                    "name": label[:4096],
                    "kind": "PROPERTY",
                })
                current = value
                continue

            try:
                if token.group("index") is not None:
                    item_index = int(token.group("index"))
                    item = current[item_index]
                    item_key = str(item_index)
                else:
                    quoted_key = token.group("key")[1:-1]
                    item_key = quoted_key.replace(r'\"', '"').replace(r"\\", "\\")
                    getter = getattr(current, "get", None)
                    item = getter(item_key) if getter is not None else current[item_key]
                item_name = str(getattr(item, "name", "") or item_key)
                item_rna = getattr(item, "bl_rna", None)
                if not getattr(item, "name", "") and item_rna is not None:
                    item_name = str(getattr(item_rna, "name", item_name))
            except (AttributeError, IndexError, KeyError, ReferenceError,
                    RuntimeError, TypeError, ValueError):
                item = None
                item_name = item_key
            segments.append({
                "key": "item:" + item_key,
                "name": item_name[:4096],
                "kind": "ITEM",
            })
            current = item
            last_property = None
        return segments, last_property

    def is_path_animated(id_block, path):
        animation_data = getattr(id_block, "animation_data", None)
        if animation_data is None:
            return False
        curves = list(getattr(animation_data, "drivers", ()))
        action = getattr(animation_data, "action", None)
        if action is not None:
            curves.extend(list(getattr(action, "fcurves", ())))
        return any(str(getattr(curve, "data_path", "")) == path
                   for curve in curves)

    def is_system_override_property(id_block, path, override_property,
                                    final_property, resolved_value):
        if final_property is None:
            return False
        property_type = str(getattr(final_property, "type", ""))
        if property_type not in {"POINTER", "COLLECTION"}:
            return False
        points_to_id = isinstance(resolved_value, bpy.types.ID)
        if not points_to_id:
            try:
                fixed_type = final_property.fixed_type
                points_to_id = (
                    fixed_type == bpy.types.ID or
                    (isinstance(fixed_type, type) and
                     issubclass(fixed_type, bpy.types.ID))
                )
            except (AttributeError, TypeError):
                points_to_id = False
        if not points_to_id and property_type == "COLLECTION":
            try:
                points_to_id = any(
                    isinstance(item, bpy.types.ID)
                    for item in list(resolved_value)[:1]
                )
            except (ReferenceError, RuntimeError, TypeError):
                points_to_id = False
        if not points_to_id:
            return False

        operations = list(getattr(override_property, "operations", ()))
        if not operations or not all(
                "IDPOINTER_MATCH_REFERENCE" in
                set(getattr(operation, "flag", ()))
                for operation in operations):
            return False
        return not is_path_animated(id_block, path)

    def override_properties(id_block, override_info):
        rows = []
        try:
            source_properties = list(getattr(override_info, "properties", ()))
        except (ReferenceError, RuntimeError, TypeError):
            source_properties = []
        for source_property in source_properties[:MAX_OUTLINER_OVERRIDE_PROPERTIES_PER_ID]:
            try:
                path = str(source_property.rna_path)
            except (AttributeError, ReferenceError, RuntimeError, TypeError):
                continue
            segments, final_property = property_path_segments(id_block, path)
            try:
                value = id_block.path_resolve(path)
                valid = True
            except (AttributeError, ReferenceError, RuntimeError, TypeError, ValueError):
                value = None
                valid = False

            operations = []
            for operation in list(getattr(source_property, "operations", ()))[:64]:
                try:
                    operation_type = str(operation.operation)
                    label = str(getattr(operation, "label", "") or "")
                except (AttributeError, ReferenceError, RuntimeError, TypeError):
                    continue
                labels = {
                    "INSERT_AFTER": "Added through override",
                    "INSERT_BEFORE": "Added through override",
                    "NOOP": "Protected from override",
                    "ADD": "Additive override",
                    "SUBTRACT": "Subtractive override",
                    "MULTIPLY": "Multiplicative override",
                    "CUSTOM": "Custom override",
                }
                operations.append({
                    "type": operation_type[:128],
                    "label": (label or labels.get(operation_type, ""))[:512],
                })
            system_property = is_system_override_property(
                id_block, path, source_property, final_property, value)
            rows.append({
                "path": path[:4096],
                "name": (segments[-1]["name"] if segments
                         else path.rsplit(".", 1)[-1])[:4096],
                "valid": valid,
                "system": system_property,
                "value": display_value(value),
                "segments": segments[:MAX_OUTLINER_OVERRIDE_DEPTH],
                "operations": operations,
            })
        return rows

    id_records = []
    by_pointer = {}
    seen_ids = set()
    truncated = [False]
    for collection_property in bpy.data.bl_rna.properties:
        if getattr(collection_property, "type", "") != "COLLECTION":
            continue
        try:
            collection = getattr(bpy.data, collection_property.identifier)
        except (AttributeError, RuntimeError, TypeError):
            continue
        for id_block in collection:
            if not isinstance(id_block, bpy.types.ID):
                continue
            pointer = identity(id_block)
            if pointer in seen_ids:
                continue
            seen_ids.add(pointer)
            override_info = getattr(id_block, "override_library", None)
            if override_info is None or getattr(id_block, "library", None) is not None:
                continue
            reference = getattr(override_info, "reference", None)
            if reference is None:
                continue
            if len(id_records) >= MAX_OUTLINER_OVERRIDE_ITEMS:
                truncated[0] = True
                continue
            hierarchy_root = getattr(override_info, "hierarchy_root", None)
            if hierarchy_root is None:
                hierarchy_root = id_block
            record = {
                "id": stable_id(id_block),
                "name": str(id_block.name_full)[:4096],
                "type": str(collection_property.name)[:256],
                "rna_type": str(id_block.bl_rna.identifier)[:256],
                "library_path": source_library_path(id_block)[:4096],
                "reference_name": (
                    str(reference.name_full)[:4096] if reference is not None else ""
                ),
                "reference_library_path": (
                    source_library_path(reference)[:4096]
                    if reference is not None else ""
                ),
                "root_id": stable_id(hierarchy_root),
                "root_pointer": identity(hierarchy_root),
                "is_root": identity(hierarchy_root) == pointer,
                "is_system_id": bool(getattr(override_info, "is_system_override", False)),
                "properties": override_properties(id_block, override_info),
                "references": [],
            }
            id_records.append(record)
            by_pointer[pointer] = (id_block, record)

    def referenced_ids(id_block):
        """Read bounded ID pointers from Blender RNA to mirror Main relations."""
        pending = [(id_block, 0)]
        visited_structs = {identity(id_block)}
        result = []
        while pending and len(result) < MAX_OUTLINER_OVERRIDE_RELATIONS_PER_ID:
            value, depth = pending.pop()
            if (isinstance(value, bpy.types.ID) and
                    identity(value) != identity(id_block)):
                result.append(value)
                continue
            if depth >= 16:
                continue
            rna = getattr(value, "bl_rna", None)
            if rna is None:
                continue
            try:
                properties = list(rna.properties)
            except (AttributeError, ReferenceError, RuntimeError, TypeError):
                continue
            for prop in properties:
                identifier = str(getattr(prop, "identifier", ""))
                if identifier in {"rna_type", "library", "override_library"}:
                    continue
                if (value is id_block and identifier == "parent" and
                        getattr(id_block, "bl_rna", None) is not None and
                        id_block.bl_rna.identifier == "Object"):
                    continue
                prop_type = str(getattr(prop, "type", ""))
                if prop_type not in {"POINTER", "COLLECTION"}:
                    continue
                try:
                    child = getattr(value, identifier)
                except (AttributeError, ReferenceError, RuntimeError, TypeError):
                    continue
                if child is None:
                    continue
                if prop_type == "COLLECTION":
                    try:
                        values = list(child)
                    except (ReferenceError, RuntimeError, TypeError):
                        continue
                    pending.extend((item, depth + 1)
                                   for item in values[:256])
                else:
                    child_pointer = identity(child)
                    if child_pointer not in visited_structs:
                        visited_structs.add(child_pointer)
                        pending.append((child, depth + 1))
                if len(pending) + len(result) >= MAX_OUTLINER_OVERRIDE_RELATIONS_PER_ID:
                    truncated[0] = True
                    break
        unique = []
        pointers = set()
        for value in result:
            pointer = identity(value)
            if pointer not in pointers:
                pointers.add(pointer)
                unique.append(value)
        return unique

    for id_block, record in by_pointer.values():
        record["references"] = [identity(value) for value in referenced_ids(id_block)]

    properties_groups = []
    properties_groups_by_type = {}
    for record in id_records:
        group = properties_groups_by_type.get(record["type"])
        if group is None:
            group = {"name": record["type"], "items": []}
            properties_groups_by_type[record["type"]] = group
            properties_groups.append(group)
        group["items"].append({
            key: value for key, value in record.items()
            if key not in {"references", "root_pointer"}
        })

    children_by_pointer = {pointer: [] for pointer in by_pointer}
    for parent_pointer, (parent_id, parent_record) in by_pointer.items():
        root_pointer = int(parent_record["root_pointer"])
        sibling_seen = set()
        for child_pointer in parent_record["references"]:
            child_entry = by_pointer.get(int(child_pointer))
            if child_entry is None:
                continue
            if int(child_entry[1]["root_pointer"]) != root_pointer:
                continue
            if child_pointer == parent_pointer or child_pointer in sibling_seen:
                continue
            sibling_seen.add(child_pointer)
            children_by_pointer[parent_pointer].append(int(child_pointer))
        if getattr(parent_id, "bl_rna", None) is not None and \
                parent_id.bl_rna.identifier == "Object":
            for child_pointer, (child_id, child_record) in by_pointer.items():
                if (int(child_record["root_pointer"]) == root_pointer and
                        getattr(child_id, "parent", None) is parent_id and
                        child_pointer not in sibling_seen):
                    sibling_seen.add(child_pointer)
                    children_by_pointer[parent_pointer].append(child_pointer)

    hierarchy_groups = []
    hierarchy_groups_by_library = {}
    hierarchy_rows_remaining = [MAX_OUTLINER_OVERRIDE_ITEMS]

    override_object_pointers = {
        pointer for pointer, (id_block, _record) in by_pointer.items()
        if isinstance(id_block, bpy.types.Object)
    }
    object_view_layer_states = {
        pointer: {"states": {}, "complete": True}
        for pointer in override_object_pointers
    }
    remaining_view_layer_states = [MAX_OUTLINER_OVERRIDE_VIEW_LAYER_STATES]

    def selectable_objects_in_view_layer(scene, view_layer):
        """Mirror the collection-derived part of Blender's BASE_SELECTABLE."""
        selectable_by_pointer = {}

        def visit(layer_collection, inherited_viewport_hidden,
                  inherited_select_hidden):
            if layer_collection.exclude:
                return
            collection = layer_collection.collection
            is_master_collection = (
                identity(collection) == identity(scene.collection)
            )
            viewport_hidden = inherited_viewport_hidden
            select_hidden = inherited_select_hidden
            if not is_master_collection:
                viewport_hidden = viewport_hidden or bool(collection.hide_viewport)
                select_hidden = select_hidden or bool(collection.hide_select)
            path_selectable = not viewport_hidden and not select_hidden
            for obj in collection.objects:
                pointer = identity(obj)
                if pointer in override_object_pointers:
                    selectable_by_pointer[pointer] = (
                        selectable_by_pointer.get(pointer, False) or
                        path_selectable
                    )
            for child in layer_collection.children:
                visit(child, viewport_hidden, select_hidden)

        visit(view_layer.layer_collection, False, False)
        return selectable_by_pointer

    for scene in bpy.data.scenes:
        for view_layer in scene.view_layers:
            selectable_by_pointer = selectable_objects_in_view_layer(
                scene, view_layer)
            try:
                layer_objects = list(view_layer.objects)
                active_object = view_layer.objects.active
            except (AttributeError, ReferenceError, RuntimeError, TypeError):
                for pointer in override_object_pointers:
                    object_view_layer_states[pointer]["complete"] = False
                continue
            layer_key = view_layer_key(scene, view_layer)
            for id_block in layer_objects:
                pointer = identity(id_block)
                state_record = object_view_layer_states.get(pointer)
                if state_record is None:
                    continue
                if remaining_view_layer_states[0] <= 0:
                    state_record["complete"] = False
                    continue
                try:
                    # The extractor runs Blender with --background, so this
                    # resolves to BASE_ENABLED_AND_VISIBLE_IN_DEFAULT_VIEWPORT,
                    # the exact visibility flag used by the Outliner filter.
                    visible = bool(id_block.visible_get(view_layer=view_layer))
                    selected = bool(id_block.select_get(view_layer=view_layer))
                    hidden = bool(id_block.hide_get(view_layer=view_layer))
                    selectable = (
                        selectable_by_pointer.get(pointer, False) and
                        not bool(id_block.hide_viewport) and
                        not bool(id_block.hide_select) and not hidden
                    )
                except (AttributeError, ReferenceError, RuntimeError, TypeError):
                    state_record["complete"] = False
                    continue
                state_record["states"][layer_key] = {
                    "visible": visible,
                    "selected": selected,
                    "active": (
                        active_object is not None and
                        identity(active_object) == pointer
                    ),
                    "selectable": selectable,
                }
                remaining_view_layer_states[0] -= 1

    def hierarchy_node(pointer, root_pointer, depth, ancestry):
        if hierarchy_rows_remaining[0] <= 0 or depth >= MAX_OUTLINER_OVERRIDE_DEPTH:
            truncated[0] = True
            return None
        entry = by_pointer.get(pointer)
        if entry is None or pointer in ancestry:
            return None
        id_block, record = entry
        hierarchy_rows_remaining[0] -= 1
        node = {
            "id": record["id"],
            "name": record["name"],
            "type": record["type"],
            "rna_type": record["rna_type"],
            "object_type": (
                str(id_block.type)
                if isinstance(id_block, bpy.types.Object) else ""
            ),
            "root_id": record["root_id"],
            "children": [],
        }
        if isinstance(id_block, bpy.types.Object):
            state_record = object_view_layer_states.get(pointer, {
                "states": {}, "complete": False,
            })
            node["view_layer_states"] = state_record["states"]
            node["view_layer_states_complete"] = state_record["complete"]
        next_ancestry = ancestry | {pointer}
        sibling_seen = set()
        for child_pointer in children_by_pointer.get(pointer, ()):
            if child_pointer in sibling_seen:
                continue
            sibling_seen.add(child_pointer)
            child_entry = by_pointer.get(child_pointer)
            if child_entry is None or int(child_entry[1]["root_pointer"]) != root_pointer:
                continue
            child = hierarchy_node(child_pointer, root_pointer, depth + 1,
                                   next_ancestry)
            if child is not None:
                node["children"].append(child)
        return node

    for pointer, (id_block, record) in by_pointer.items():
        if not record["is_root"]:
            continue
        library_path = source_library_path(id_block)
        library = getattr(id_block, "library", None)
        if library is not None:
            library_key = stable_id(library)
            library_name = str(library.filepath or library.name_full)
            current_file = False
        else:
            library_key = "CURRENT_FILE"
            library_name = "Current File"
            current_file = True
        group_record = hierarchy_groups_by_library.get(library_key)
        if group_record is None:
            group_record = {
                "id": library_key,
                "name": library_name[:4096],
                "current_file": current_file,
                "groups": [],
                "truncated": False,
            }
            hierarchy_groups_by_library[library_key] = group_record
            hierarchy_groups.append(group_record)
        type_group = next((group for group in group_record["groups"]
                           if group["name"] == record["type"]), None)
        if type_group is None:
            type_group = {"name": record["type"], "items": []}
            group_record["groups"].append(type_group)
        root_node = hierarchy_node(pointer, pointer, 0, set())
        if root_node is not None:
            type_group["items"].append(root_node)
        if hierarchy_rows_remaining[0] <= 0:
            group_record["truncated"] = True

    if not any(group["current_file"] for group in hierarchy_groups):
        hierarchy_groups.insert(0, {
            "id": "CURRENT_FILE",
            "name": "Current File",
            "current_file": True,
            "groups": [],
            "truncated": False,
        })
    hierarchy_groups = [
        group for group in hierarchy_groups
        if group["current_file"] or group["groups"]
    ]
    return {
        "properties": {
            "groups": properties_groups,
            "truncated": truncated[0],
        },
        "hierarchies": hierarchy_groups,
        "truncated": truncated[0] or hierarchy_rows_remaining[0] <= 0,
        "has_libraries": bool(bpy.data.libraries),
    }


def outliner_sequencer_tree(active_window, fallback_scene):
    """Capture Blender's current workspace Sequencer Outliner tree."""
    workspace = getattr(active_window, "workspace", None)
    scene = (getattr(workspace, "sequencer_scene", None)
             if workspace is not None else fallback_scene)
    record = {
        "workspace_name": str(workspace.name) if workspace is not None else "",
        "scene_id": stable_id(scene) if scene is not None else "",
        "scene_name": str(scene.name) if scene is not None else "",
        "strips": [],
        "truncated": False,
    }
    sequence_editor = getattr(scene, "sequence_editor", None)
    if sequence_editor is None:
        return record

    meta_stack = list(getattr(sequence_editor, "meta_stack", ()))
    current_strips = (meta_stack[-1].strips if meta_stack
                      else sequence_editor.strips)
    active_strip = getattr(sequence_editor, "active_strip", None)
    remaining = [MAX_OUTLINER_SEQUENCE_ITEMS]
    truncated = [False]
    effect_types = {
        "CROSS", "ADD", "SUBTRACT", "ALPHA_OVER", "ALPHA_UNDER",
        "GAMMA_CROSS", "MULTIPLY", "OVERDROP", "WIPE", "GLOW",
        "TRANSFORM", "SPEED",
        "MULTICAM", "COMPOSITOR", "ADJUSTMENT", "GAUSSIAN_BLUR",
        "COLORMIX", "COLOR", "TEXT",
    }

    def media_path(strip):
        strip_type = str(getattr(strip, "type", ""))
        if strip_type == "IMAGE":
            try:
                elements = getattr(strip, "elements", ())
                if len(elements) > 0:
                    directory = str(getattr(strip, "directory", "") or "")
                    filename = str(getattr(elements[0], "filename", "") or "")
                    return os.path.join(directory, filename) if directory else filename
            except (AttributeError, RuntimeError, TypeError):
                return ""
            return ""
        if strip_type != "MOVIE":
            # Blender only groups rows with StripData::stripdata, which is
            # present for image and movie strips. Sound paths belong to a
            # separate Sound data-block and must not create duplicate groups.
            return ""
        try:
            filepath = str(getattr(strip, "filepath", "") or "")
        except (AttributeError, RuntimeError, TypeError):
            filepath = ""
        return filepath

    def strip_record(strip, depth, source_id):
        if remaining[0] <= 0:
            truncated[0] = True
            return None
        remaining[0] -= 1
        strip_type = str(getattr(strip, "type", ""))
        is_meta = strip_type == "META"
        is_effect = strip_type in effect_types
        path = media_path(strip)
        try:
            directory = str(getattr(strip, "directory", "") or "")
        except (AttributeError, RuntimeError, TypeError):
            directory = ""
        if not directory and path:
            directory = os.path.dirname(path)
        item = {
            "kind": "STRIP",
            "name": str(getattr(strip, "name", "")),
            "type": strip_type,
            "media_filename": bpy.path.basename(path) if path else "",
            "data_directory": directory or "Strip None",
            "is_meta": is_meta,
            "is_effect": is_effect,
            "selected": bool(getattr(strip, "select", False)),
            "active": strip == active_strip,
            "source_id": source_id,
            "children": [],
        }
        if is_meta:
            if depth >= MAX_OUTLINER_SEQUENCE_DEPTH:
                if len(getattr(strip, "strips", ())) > 0:
                    truncated[0] = True
            else:
                children = strip.strips
                for child_index, child in enumerate(children):
                    child_record = strip_record(
                        child, depth + 1,
                        source_id + "/child:" + str(child_index))
                    if child_record is not None:
                        item["children"].append(child_record)
                    if remaining[0] <= 0 and child_index + 1 < len(children):
                        truncated[0] = True
                        break
        return item

    top_level = []
    top_level_truncated = False
    for strip in current_strips:
        if len(top_level) >= MAX_OUTLINER_SEQUENCE_ITEMS:
            top_level_truncated = True
            break
        top_level.append(strip)
    truncated[0] = top_level_truncated
    media_filenames = []
    indices_by_filename = {}
    for index, strip in enumerate(top_level):
        path = media_path(strip)
        filename = bpy.path.basename(path) if path else ""
        media_filenames.append(filename)
        if filename:
            indices_by_filename.setdefault(filename, []).append(index)
    rows = []
    for index, strip in enumerate(top_level):
        filename = media_filenames[index]
        if filename:
            matching_indices = indices_by_filename[filename]
            if matching_indices[0] != index:
                continue
            if len(matching_indices) > 1:
                duplicate_row = {
                    "kind": "DUPLICATE",
                    "name": filename,
                    "type": "",
                    "media_filename": filename,
                    "data_directory": "",
                    "is_meta": False,
                    "is_effect": False,
                    "selected": False,
                    "active": False,
                    "children": [],
                }
                for matching_index in matching_indices:
                    candidate = top_level[matching_index]
                    child_record = strip_record(
                        candidate, 0, "strip:" + str(matching_index))
                    if child_record is not None:
                        duplicate_row["children"].append(child_record)
                    if (remaining[0] <= 0 and
                            matching_index != matching_indices[-1]):
                        truncated[0] = True
                        break
                rows.append(duplicate_row)
                if remaining[0] <= 0:
                    if matching_indices[-1] + 1 < len(top_level):
                        truncated[0] = True
                    break
                continue
        strip_item = strip_record(strip, 0, "strip:" + str(index))
        if strip_item is not None:
            rows.append(strip_item)
        if remaining[0] <= 0:
            if index + 1 < len(top_level):
                truncated[0] = True
            break
    record["strips"] = rows
    record["truncated"] = truncated[0]
    return record


def outliner_data_api_tree(focus_root_property=None, focus_node_path=None):
    """Capture Blender's RNA Data API tree with bounded recursion."""
    remaining = [MAX_OUTLINER_DATA_API_ITEMS - 1]
    truncated = [False]
    pending_structs = []
    driver_paths = []
    seen_driver_ids = set()
    remaining_driver_paths = [MAX_OUTLINER_DATA_API_DRIVER_PATHS]
    driver_paths_truncated = [False]
    root = {
        "kind": "STRUCT",
        "key": "main",
        "name": "RNA",
        "type": str(getattr(getattr(bpy.data, "bl_rna", None),
                             "identifier", "BlendData")),
        "value": "",
        "children": [],
    }

    def array_component_name(subtype, index):
        """Match Blender RNA_property_array_item_char() labels."""
        if index < 4:
            if subtype in ("QUATERNION", "AXISANGLE"):
                return ("W", "X", "Y", "Z")[index]
            if subtype in {
                    "TRANSLATION", "DIRECTION", "XYZ", "XYZ_LENGTH",
                    "EULER", "VELOCITY", "ACCELERATION", "COORDS",
                    "PIXEL"}:
                return ("X", "Y", "Z", "W")[index]
            if subtype in ("COLOR", "COLOR_GAMMA"):
                return ("R", "G", "B", "A")[index]
        return str(index + 1)

    def struct_name(value):
        try:
            name = getattr(value, "name", None)
            if name:
                return str(name)[:4096]
            rna = getattr(value, "bl_rna", None)
            return str(getattr(rna, "name", "(empty)"))[:4096]
        except (AttributeError, ReferenceError, RuntimeError, TypeError):
            return "(empty)"

    def identity(value):
        try:
            return int(value.as_pointer())
        except (AttributeError, ReferenceError, RuntimeError, TypeError):
            return id(value)

    def reserve_node(kind, key, name, type_name, value_text="",
                     branch_remaining=None, item_remaining=None):
        if remaining[0] <= 0:
            truncated[0] = True
            return None
        if branch_remaining is not None and branch_remaining[0] <= 0:
            truncated[0] = True
            return None
        if item_remaining is not None:
            for item_budget in item_remaining:
                if item_budget[0] <= 0:
                    truncated[0] = True
                    return None
        remaining[0] -= 1
        if branch_remaining is not None:
            branch_remaining[0] -= 1
        if item_remaining is not None:
            for item_budget in item_remaining:
                item_budget[0] -= 1
        return {
            "kind": kind,
            "key": str(key)[:512],
            "name": str(name)[:4096],
            "type": str(type_name)[:256],
            "value": str(value_text)[:4096],
            "children": [],
        }

    def properties_of(value):
        try:
            rna = getattr(value, "bl_rna", None)
            properties = getattr(rna, "properties", ())
            return list(properties)
        except (AttributeError, ReferenceError, RuntimeError, TypeError):
            return []

    def animation_path_metadata(owner, prop, identifier, is_array,
                                array_index=0):
        try:
            if not bool(getattr(prop, "is_animatable", False)):
                return None
            id_block = owner if isinstance(owner, bpy.types.ID) else getattr(
                owner, "id_data", None)
            if not isinstance(id_block, bpy.types.ID):
                return None

            # Blender's Outliner operators call RNA_property_anim_editable(),
            # which includes the owner-specific editable callback and library
            # override checks. Property.is_readonly only describes the static
            # RNA flag; query the live owner to include those dynamic checks.
            if bool(owner.is_property_readonly(identifier)):
                return None

            # RNA_property_anim_editable also rejects a property when its
            # owning ID already uses a linked or overridden Action.
            animation_data = id_block.animation_data
            action = animation_data.action if animation_data is not None else None
            if action is not None and (
                    not bool(action.is_editable) or
                    getattr(action, "override_library", None) is not None):
                return None

            owner_path = "" if owner is id_block else str(
                owner.path_from_id())
            data_path = str(owner.path_from_id(identifier))
            if not data_path or len(data_path) > 4096:
                return None
            return {
                "id_type": str(id_block.id_type)[:256],
                "id_name": str(id_block.name_full)[:4096],
                "library_path": source_library_path(id_block)[:4096],
                "data_path": data_path,
                "owner_path": owner_path,
                "property_identifier": identifier,
                "custom_property": False,
                "array_index": int(array_index),
                "use_entire_array": bool(is_array),
            }
        except (AttributeError, ReferenceError, RuntimeError, TypeError,
                ValueError):
            return None

    def custom_property_value_type(value):
        try:
            from idprop.types import IDPropertyArray, IDPropertyGroup
        except ImportError:
            IDPropertyArray = ()
            IDPropertyGroup = ()
        if isinstance(value, IDPropertyGroup):
            return "GROUP"
        if isinstance(value, IDPropertyArray):
            return "ARRAY"
        if isinstance(value, bool):
            return "BOOLEAN"
        if isinstance(value, int):
            return "INT"
        if isinstance(value, float):
            return "FLOAT"
        if isinstance(value, str):
            return "STRING"
        return "OTHER"

    def custom_property_entries(owner):
        """Yield the custom properties the Data API tree can display."""
        try:
            keys = iter(owner.keys())
            rna_properties = getattr(getattr(owner, "bl_rna", None),
                                     "properties", ())
            find_rna_property = getattr(rna_properties, "get", None)
        except (AttributeError, ReferenceError, RuntimeError, TypeError):
            return
        try:
            for raw_key in keys:
                try:
                    identifier = str(raw_key)
                    if (not identifier or identifier == "_RNA_UI" or
                            len(identifier) > 4096):
                        continue
                    property_rna = (find_rna_property(identifier)
                                    if callable(find_rna_property) else None)
                    # Runtime RNA properties backed by ID properties are
                    # already shown by the regular RNA property walk.
                    if property_rna is not None and bool(getattr(
                            property_rna, "is_runtime", False)):
                        continue
                    property_value = owner[raw_key]
                except (AttributeError, KeyError, ReferenceError, RuntimeError,
                        TypeError, ValueError):
                    continue
                yield (raw_key, identifier, property_value,
                       custom_property_value_type(property_value))
        except (AttributeError, ReferenceError, RuntimeError, TypeError,
                ValueError):
            return

    def has_data_api_children(value):
        for prop in properties_of(value):
            try:
                if (str(prop.identifier) != "rna_type" and not bool(
                        getattr(prop, "is_hidden", False))):
                    return True
            except (AttributeError, ReferenceError, RuntimeError, TypeError):
                continue
        return next(custom_property_entries(value), None) is not None

    def custom_property_node_budget(owner, depth, ancestry, limit):
        """Count visible custom rows recursively, capped by the row budget."""
        if limit <= 0:
            return 0
        rows = 0
        for _raw_key, _identifier, property_value, value_type in \
                custom_property_entries(owner):
            if rows >= limit:
                break
            rows += 1
            if value_type == "ARRAY":
                try:
                    rows = min(limit, rows + len(property_value))
                except (AttributeError, OverflowError, ReferenceError,
                        RuntimeError, TypeError, ValueError):
                    pass
            elif value_type == "GROUP":
                group_id = id(property_value)
                if (depth + 1 < MAX_OUTLINER_DATA_API_DEPTH and
                        group_id not in ancestry and rows < limit):
                    rows += custom_property_node_budget(
                        property_value, depth + 1,
                        ancestry | {group_id}, limit - rows)
            if rows >= limit:
                break
        return rows

    def custom_override_path_is_animatable(id_block, data_path):
        override_info = getattr(id_block, "override_library", None)
        if override_info is None:
            return True
        if bool(getattr(override_info, "is_system_override", False)):
            return False
        try:
            override_properties = list(override_info.properties)[
                :MAX_OUTLINER_OVERRIDE_PROPERTIES_PER_ID]
            for source_property in override_properties:
                if str(source_property.rna_path) != data_path:
                    continue
                operations = list(source_property.operations)[:64]
                return bool(operations) and all(
                    str(operation.operation) == "REPLACE" and
                    "LOCKED" not in set(getattr(operation, "flag", ()))
                    for operation in operations)
        except (AttributeError, ReferenceError, RuntimeError, TypeError,
                ValueError):
            return False
        return False

    def custom_property_animation_path(id_block, owner, owner_path,
                                       identifier, data_path, value,
                                       array_index=0):
        try:
            if not isinstance(id_block, bpy.types.ID) or not bool(
                    getattr(id_block, "is_editable", False)):
                return None
            value_type = custom_property_value_type(value)
            is_array = value_type == "ARRAY"
            if not is_array and value_type not in {"BOOLEAN", "INT", "FLOAT"}:
                return None
            if is_array:
                if len(value) <= 0 or value.typecode not in {"b", "i", "f", "d"}:
                    return None
            if not custom_override_path_is_animatable(id_block, data_path):
                return None
            readonly_check = getattr(owner, "is_property_readonly", None)
            if callable(readonly_check):
                try:
                    if bool(readonly_check(identifier)):
                        return None
                except (AttributeError, ReferenceError, RuntimeError,
                        TypeError, ValueError):
                    return None

            animation_data = id_block.animation_data
            action = animation_data.action if animation_data is not None else None
            if action is not None and (
                    not bool(action.is_editable) or
                    getattr(action, "override_library", None) is not None):
                return None
            if not data_path or len(data_path) > 4096:
                return None
            return {
                "id_type": str(id_block.id_type)[:256],
                "id_name": str(id_block.name_full)[:4096],
                "library_path": source_library_path(id_block)[:4096],
                "data_path": data_path,
                "owner_path": owner_path,
                "property_identifier": identifier,
                "custom_property": True,
                "array_index": int(array_index),
                "use_entire_array": bool(is_array),
            }
        except (AttributeError, ReferenceError, RuntimeError, TypeError,
                ValueError):
            return None

    def add_custom_property_nodes(parent, owner, id_block, owner_path,
                                  depth, ancestry, branch_remaining,
                                  item_remaining=None):
        for raw_key, identifier, property_value, value_type in \
                custom_property_entries(owner):
            if remaining[0] <= 0:
                truncated[0] = True
                parent["truncated"] = True
                break
            try:
                escaped_key = bpy.utils.escape_identifier(identifier)
                segment = '["%s"]' % escaped_key
                data_path = owner_path + segment
                display_name = identifier
                if not isinstance(owner, type(None)) and hasattr(
                        owner, "id_properties_ui"):
                    try:
                        display_name = str(owner.id_properties_ui(
                            identifier).as_dict().get("name") or identifier)
                    except (AttributeError, ReferenceError, RuntimeError,
                            TypeError, ValueError):
                        display_name = identifier
            except (AttributeError, KeyError, ReferenceError, RuntimeError,
                    TypeError, ValueError):
                continue

            property_node = reserve_node(
                "PROPERTY", "idprop:" + identifier, display_name,
                "IDPROPERTY_" + value_type,
                branch_remaining=branch_remaining,
                item_remaining=item_remaining)
            if property_node is None:
                parent["truncated"] = True
                break
            property_node["has_children"] = False
            if value_type in {"BOOLEAN", "INT", "FLOAT", "ARRAY"}:
                path_record = custom_property_animation_path(
                    id_block, owner, owner_path, identifier, data_path,
                    property_value)
                if path_record is not None:
                    property_node["animation_path"] = path_record

            if value_type == "GROUP":
                property_node["has_children"] = bool(property_value)
                if focus_node_path is not None and property_node["has_children"]:
                    property_node["deferred"] = True
                elif depth + 1 >= MAX_OUTLINER_DATA_API_DEPTH:
                    if len(property_value):
                        truncated[0] = True
                        property_node["truncated"] = True
                elif id(property_value) in ancestry:
                    truncated[0] = True
                    property_node["truncated"] = True
                else:
                    add_custom_property_nodes(
                        property_node, property_value, id_block, data_path,
                        depth + 1, ancestry | {id(property_value)},
                        branch_remaining, item_remaining)
            elif value_type == "ARRAY":
                property_node["has_children"] = len(property_value) > 0
                if focus_node_path is not None and property_node["has_children"]:
                    property_node["deferred"] = True
                    parent["children"].append(property_node)
                    continue
                try:
                    array_values = enumerate(property_value)
                    for index, _element in array_values:
                        array_node = reserve_node(
                            "ARRAY_ELEM", str(index), str(index + 1),
                            "IDPROPERTY_ELEMENT",
                            branch_remaining=branch_remaining,
                            item_remaining=item_remaining)
                        if array_node is None:
                            break
                        whole_array_path = property_node.get("animation_path")
                        if whole_array_path is not None:
                            element_path = dict(whole_array_path)
                            element_path["array_index"] = index
                            element_path["use_entire_array"] = False
                            array_node["animation_path"] = element_path
                        property_node["children"].append(array_node)
                    if len(property_node["children"]) < len(property_value):
                        property_node["truncated"] = True
                except (AttributeError, OverflowError, ReferenceError,
                        RuntimeError, TypeError, ValueError):
                    truncated[0] = True
                    property_node["truncated"] = True
            parent["children"].append(property_node)

    def capture_driver_paths(value):
        if (focus_root_property is not None or
                focus_node_path is not None):
            return
        if not isinstance(value, bpy.types.ID):
            return
        pointer = identity(value)
        if pointer in seen_driver_ids:
            return
        seen_driver_ids.add(pointer)
        try:
            animation_data = value.animation_data
            source_drivers = animation_data.drivers if animation_data else ()
        except (AttributeError, ReferenceError, RuntimeError, TypeError):
            return
        try:
            for fcurve in source_drivers:
                if remaining_driver_paths[0] <= 0:
                    driver_paths_truncated[0] = True
                    break
                try:
                    driver = fcurve.driver
                    data_path = str(fcurve.data_path)
                    array_index = int(fcurve.array_index)
                    if driver is None or not data_path or len(data_path) > 4096:
                        continue
                    driver_paths.append({
                        "id_type": str(value.id_type)[:256],
                        "id_name": str(value.name_full)[:4096],
                        "library_path": source_library_path(value)[:4096],
                        "data_path": data_path,
                        "array_index": array_index,
                    })
                    remaining_driver_paths[0] -= 1
                except (AttributeError, ReferenceError, RuntimeError,
                        TypeError, ValueError):
                    continue
        except (AttributeError, ReferenceError, RuntimeError, TypeError):
            return

    def resolve_focus_node(path):
        if not isinstance(path, list) or not path or len(path) > 64:
            raise RuntimeError("invalid Blender Data API node path")
        current = bpy.data
        current_kind = "STRUCT"
        target = None
        target_owner = None
        target_property = None
        for segment in path:
            if not isinstance(segment, dict):
                raise RuntimeError("invalid Blender Data API node path")
            kind = segment.get("kind")
            key = segment.get("key")
            if (kind not in {"PROPERTY", "STRUCT", "ARRAY_ELEM"} or
                    not isinstance(key, str) or not key or len(key) > 512):
                raise RuntimeError("invalid Blender Data API node path")
            if kind == "PROPERTY":
                if current_kind not in {"STRUCT", "PROPERTY"}:
                    raise RuntimeError("invalid Blender Data API property path")
                target_owner = current
                target_property = None
                if key.startswith("idprop:"):
                    current = current[key[len("idprop:"):]]
                else:
                    try:
                        rna_properties = getattr(
                            getattr(current, "bl_rna", None), "properties", ())
                        target_property = rna_properties.get(key)
                        if (target_property is None or
                                bool(getattr(target_property, "is_hidden", False))):
                            raise RuntimeError(
                                "Blender Data API property is unavailable: " + key)
                        current = getattr(current, key)
                    except (AttributeError, ReferenceError, RuntimeError,
                            TypeError) as error:
                        raise RuntimeError(
                            "could not resolve Blender Data API property: " + key) from error
                current_kind = "PROPERTY"
            elif kind == "STRUCT":
                if current_kind != "PROPERTY":
                    raise RuntimeError("invalid Blender Data API struct path")
                if key != "pointer":
                    try:
                        current = current[int(key)]
                    except (IndexError, KeyError, TypeError, ValueError) as error:
                        raise RuntimeError(
                            "could not resolve Blender Data API collection item") from error
                current_kind = "STRUCT"
                target_property = None
            else:
                if current_kind != "PROPERTY":
                    raise RuntimeError("invalid Blender Data API array path")
                try:
                    current = current[int(key)]
                except (IndexError, KeyError, TypeError, ValueError) as error:
                    raise RuntimeError(
                        "could not resolve Blender Data API array element") from error
                current_kind = "ARRAY_ELEM"
                target_property = None
            target = (kind, key, current, target_owner, target_property)
        return target

    def add_struct(value, key, depth, ancestry, branch_remaining,
                   item_remaining=None):
        rna = getattr(value, "bl_rna", None)
        type_name = str(getattr(rna, "identifier", ""))
        node = reserve_node("STRUCT", key, struct_name(value), type_name,
                            branch_remaining=branch_remaining,
                            item_remaining=item_remaining)
        if node is None:
            return None
        node["has_children"] = has_data_api_children(value)
        if focus_node_path is not None and node["has_children"]:
            node["deferred"] = True
        pointer = identity(value)
        capture_driver_paths(value)
        if pointer in ancestry:
            if node["has_children"]:
                truncated[0] = True
                node["truncated"] = True
            return node
        if depth >= MAX_OUTLINER_DATA_API_DEPTH:
            if node["has_children"]:
                truncated[0] = True
                node["truncated"] = True
            return node

        pending_structs.append(
            (node, value, depth, ancestry | {pointer}, branch_remaining,
             item_remaining))
        return node

    def populate_struct(node, value, depth, ancestry, branch_remaining,
                        item_remaining=None):
        direct_truncated = False
        properties = properties_of(value)
        unreadable_value = object()
        visible_properties = []
        property_values = {}
        collection_sizes = {}
        child_branches = []
        available_budget = min(
            [remaining[0], branch_remaining[0]] +
            [item_budget[0] for item_budget in (item_remaining or [])])
        direct_node_budget = custom_property_node_budget(
            value, depth, ancestry, available_budget)
        for prop in properties:
            try:
                identifier = str(prop.identifier)
                if identifier == "rna_type" or bool(
                        getattr(prop, "is_hidden", False)):
                    continue
                prop_type = str(prop.type)
                prop_name = str(prop.name)
                is_array = bool(getattr(prop, "is_array", False))
            except (AttributeError, ReferenceError, RuntimeError, TypeError):
                continue

            visible_properties.append((prop, identifier, prop_type, prop_name))
            direct_node_budget += 1
            if prop_type in ("POINTER", "COLLECTION") or (
                    prop_type in ("BOOLEAN", "INT", "FLOAT") and is_array):
                try:
                    property_value = getattr(value, identifier)
                    property_values[identifier] = property_value
                except (AttributeError, ReferenceError, RuntimeError,
                        TypeError):
                    property_values[identifier] = unreadable_value
                    continue

                if prop_type == "POINTER":
                    if property_value is not None:
                        child_branches.append((identifier, "POINTER", 1))
                elif prop_type == "COLLECTION":
                    try:
                        collection_size = len(property_value)
                    except (AttributeError, ReferenceError, RuntimeError,
                            TypeError):
                        property_values[identifier] = unreadable_value
                        continue
                    collection_sizes[identifier] = collection_size
                    if collection_size > 0:
                        child_branches.append((
                            identifier, "COLLECTION", collection_size))
                else:
                    try:
                        array_values = list(property_value)
                    except (AttributeError, ReferenceError, RuntimeError,
                            TypeError):
                        property_values[identifier] = unreadable_value
                    else:
                        property_values[identifier] = array_values
                        direct_node_budget += len(array_values)

        child_budget = max(0, available_budget - direct_node_budget)
        child_family_limits = {}
        if child_branches:
            family_share, family_remainder = divmod(
                child_budget, len(child_branches))
            for family_index, (identifier, _kind, _size) in enumerate(
                    child_branches):
                child_family_limits[identifier] = (
                    family_share +
                    (1 if family_index < family_remainder else 0))

        for prop, identifier, prop_type, prop_name in visible_properties:
            if remaining[0] <= 0:
                truncated[0] = True
                direct_truncated = True
                break
            property_node = reserve_node(
                "PROPERTY", identifier, prop_name, prop_type,
                branch_remaining=branch_remaining,
                item_remaining=item_remaining)
            if property_node is None:
                direct_truncated = True
                break
            property_node["has_children"] = False
            is_array = bool(getattr(prop, "is_array", False))
            property_animation_path = animation_path_metadata(
                value, prop, identifier, is_array)
            if property_animation_path is not None:
                property_node["animation_path"] = property_animation_path
            if identifier in property_values:
                property_value = property_values[identifier]
            else:
                try:
                    property_value = getattr(value, identifier)
                except (AttributeError, ReferenceError, RuntimeError,
                        TypeError):
                    property_value = unreadable_value
            if property_value is unreadable_value:
                node["children"].append(property_node)
                continue

            if prop_type == "POINTER":
                if property_value is not None:
                    property_node["has_children"] = True
                    if focus_node_path is not None:
                        property_node["deferred"] = True
                    else:
                        pointer_budget = child_family_limits.get(identifier, 0)
                        if pointer_budget <= 0:
                            truncated[0] = True
                            property_node["truncated"] = True
                        else:
                            pointer_item_remaining = list(item_remaining or [])
                            pointer_item_remaining.append([pointer_budget])
                            child = add_struct(property_value, "pointer",
                                               depth + 1, ancestry,
                                               branch_remaining,
                                               pointer_item_remaining)
                            if child is not None:
                                property_node["children"].append(child)
            elif prop_type == "COLLECTION":
                try:
                    collection_size = collection_sizes[identifier]
                    iterator = iter(property_value)
                except (AttributeError, KeyError, ReferenceError, RuntimeError,
                        TypeError):
                    continue
                property_node["has_children"] = collection_size > 0
                if collection_size <= 0:
                    node["children"].append(property_node)
                    continue
                if focus_node_path is not None:
                    property_node["deferred"] = True
                    node["children"].append(property_node)
                    continue
                collection_budget = branch_remaining[0]
                if item_remaining is not None:
                    collection_budget = min(
                        collection_budget,
                        *(item_budget[0] for item_budget in item_remaining))
                collection_budget = min(
                    collection_budget,
                    child_family_limits.get(identifier, 0))
                if collection_budget <= 0:
                    truncated[0] = True
                    property_node["truncated"] = True
                    node["children"].append(property_node)
                    continue
                collection_item_limit = min(
                    collection_size, MAX_OUTLINER_DATA_API_COLLECTION_ITEMS,
                    MAX_OUTLINER_DATA_API_BRANCH_ITEMS,
                    max(1, collection_budget // 2))
                if collection_size > collection_item_limit:
                    truncated[0] = True
                    property_node["truncated"] = True
                per_child_limit = max(
                    1, collection_budget // max(1, collection_item_limit))
                for index, element in enumerate(iterator):
                    if (index >= collection_size or
                            index >= collection_item_limit or
                            remaining[0] <= 0 or
                            branch_remaining[0] <= 0 or
                            (item_remaining is not None and any(
                                item_budget[0] <= 0
                                for item_budget in item_remaining))):
                        truncated[0] = True
                        break
                    child_item_remaining = list(item_remaining or [])
                    child_item_remaining.append([per_child_limit])
                    child = add_struct(element, str(index), depth + 1,
                                       ancestry, branch_remaining,
                                       child_item_remaining)
                    if child is not None:
                        property_node["children"].append(child)
                if len(property_node["children"]) < collection_size:
                    property_node["truncated"] = True
            elif (prop_type in ("BOOLEAN", "INT", "FLOAT") and
                  bool(getattr(prop, "is_array", False))):
                values = property_value
                subtype = str(getattr(prop, "subtype", ""))
                property_node["has_children"] = bool(values)
                if focus_node_path is not None and values:
                    property_node["deferred"] = True
                    node["children"].append(property_node)
                    continue
                for index, element in enumerate(values):
                    array_key = str(index)
                    array_name = array_component_name(subtype, index)
                    array_node = reserve_node(
                        "ARRAY_ELEM", array_key, array_name, prop_type,
                        branch_remaining=branch_remaining,
                        item_remaining=item_remaining)
                    if array_node is None:
                        break
                    if property_animation_path is not None:
                        element_path = dict(property_animation_path)
                        element_path["array_index"] = index
                        element_path["use_entire_array"] = False
                        array_node["animation_path"] = element_path
                    property_node["children"].append(array_node)
                if len(property_node["children"]) < len(values):
                    property_node["truncated"] = True
            node["children"].append(property_node)

        id_block = value if isinstance(value, bpy.types.ID) else getattr(
            value, "id_data", None)
        if isinstance(id_block, bpy.types.ID):
            try:
                custom_owner_path = "" if value is id_block else str(
                    value.path_from_id())
            except (AttributeError, ReferenceError, RuntimeError, TypeError,
                    ValueError):
                custom_owner_path = ""
            add_custom_property_nodes(
                node, value, id_block, custom_owner_path, depth, ancestry,
                branch_remaining, item_remaining)
        if direct_truncated:
            node["truncated"] = True
        node["has_children"] = bool(node["children"]) or bool(
            node.get("truncated"))

    ancestry = {identity(bpy.data)}
    root_properties = []
    property_owner = bpy.data
    focused_target = None
    if focus_node_path is not None:
        focused_target = resolve_focus_node(focus_node_path)
        (focused_kind, focused_key, focused_value, focused_owner,
         focused_property) = focused_target
        if focused_kind == "PROPERTY":
            if focused_property is None:
                raise RuntimeError(
                    "custom Blender Data API properties cannot be refreshed independently")
            property_owner = focused_owner
            root_properties.append(focused_property)
        elif focused_kind != "STRUCT":
            raise RuntimeError(
                "this Blender Data API node has no expandable children")
    else:
        for prop in properties_of(bpy.data):
            try:
                identifier = str(prop.identifier)
                if identifier == "rna_type" or bool(
                        getattr(prop, "is_hidden", False)):
                    continue
            except (AttributeError, ReferenceError, RuntimeError, TypeError):
                continue
            if (focus_root_property is not None and
                    identifier != focus_root_property):
                continue
            root_properties.append(prop)

    root_property_count = len(root_properties)
    focused_property_node = None
    for root_property_index, prop in enumerate(root_properties):
        if remaining[0] <= 0:
            truncated[0] = True
            break
        try:
            identifier = str(prop.identifier)
            if identifier == "rna_type" or bool(
                    getattr(prop, "is_hidden", False)):
                continue
            prop_type = str(prop.type)
            prop_name = str(prop.name)
        except (AttributeError, ReferenceError, RuntimeError, TypeError):
            continue
        property_node = reserve_node("PROPERTY", identifier, prop_name,
                                     prop_type)
        if property_node is None:
            break
        if (focus_root_property is not None or
                (focused_target is not None and
                 focused_target[0] == "PROPERTY")):
            focused_property_node = property_node
        property_node["has_children"] = False
        remaining_root_properties = root_property_count - root_property_index
        future_root_properties = remaining_root_properties - 1
        # Reserve a row for every later bpy.data property, then split the
        # remaining capacity across root sections. Unused capacity carries
        # forward because each later section recalculates from `remaining`.
        branch_limit = max(
            0, (remaining[0] - future_root_properties) //
            max(1, remaining_root_properties))
        branch_remaining = [branch_limit]
        is_array = bool(getattr(prop, "is_array", False))
        property_animation_path = animation_path_metadata(
            property_owner, prop, identifier, is_array)
        if property_animation_path is not None:
            property_node["animation_path"] = property_animation_path
        try:
            property_value = getattr(property_owner, identifier)
        except (AttributeError, ReferenceError, RuntimeError, TypeError):
            root["children"].append(property_node)
            continue
        if prop_type == "POINTER":
            if property_value is not None:
                property_node["has_children"] = True
                child = add_struct(property_value, "pointer", 1,
                                   ancestry | {identity(property_owner)},
                                   branch_remaining)
                if child is not None:
                    property_node["children"].append(child)
        elif prop_type == "COLLECTION":
            try:
                collection_size = len(property_value)
                iterator = iter(property_value)
            except (AttributeError, ReferenceError, RuntimeError, TypeError):
                iterator = iter(())
                collection_size = 0
            property_node["has_children"] = collection_size > 0
            collection_item_limit = (
                MAX_OUTLINER_DATA_API_COLLECTION_ITEMS
                if (focus_root_property is not None or
                    (focused_target is not None and
                     focused_target[0] == "PROPERTY"))
                else MAX_OUTLINER_DATA_API_BRANCH_ITEMS)
            section_collection_limit = min(
                collection_size, MAX_OUTLINER_DATA_API_COLLECTION_ITEMS,
                collection_item_limit,
                max(1, branch_remaining[0]) if focus_node_path is not None
                else max(1, branch_remaining[0] // 32))
            if collection_size > section_collection_limit:
                truncated[0] = True
                property_node["truncated"] = True
            per_item_limit = (1 if focus_node_path is not None else max(
                1, branch_remaining[0] // max(1, section_collection_limit)))
            for index, element in enumerate(iterator):
                if (index >= collection_size or
                        index >= section_collection_limit or
                        remaining[0] <= 0 or
                        branch_remaining[0] <= 0):
                    truncated[0] = True
                    break
                item_remaining = [[per_item_limit]]
                child = add_struct(element, str(index), 1, ancestry,
                                   branch_remaining, item_remaining)
                if child is not None:
                    property_node["children"].append(child)
            if len(property_node["children"]) < collection_size:
                property_node["truncated"] = True
        elif (prop_type in ("BOOLEAN", "INT", "FLOAT") and
              bool(getattr(prop, "is_array", False))):
            try:
                values = list(property_value)
            except (AttributeError, ReferenceError, RuntimeError, TypeError):
                values = []
            property_node["has_children"] = bool(values)
            subtype = str(getattr(prop, "subtype", ""))
            for index, element in enumerate(values):
                array_name = array_component_name(subtype, index)
                array_node = reserve_node(
                    "ARRAY_ELEM", str(index), array_name, prop_type,
                    branch_remaining=branch_remaining)
                if array_node is None:
                    break
                if property_animation_path is not None:
                    element_path = dict(property_animation_path)
                    element_path["array_index"] = index
                    element_path["use_entire_array"] = False
                    array_node["animation_path"] = element_path
                property_node["children"].append(array_node)
            if len(property_node["children"]) < len(values):
                property_node["truncated"] = True
        root["children"].append(property_node)

    if focused_target is not None and focused_target[0] == "STRUCT":
        (_focused_kind, focused_key, focused_value, _focused_owner,
         _focused_property) = focused_target
        target_rna = getattr(focused_value, "bl_rna", None)
        target_node = reserve_node(
            "STRUCT", focused_key, struct_name(focused_value),
            str(getattr(target_rna, "identifier", "")))
        if target_node is not None:
            root["children"].append(target_node)
            target_node["has_children"] = has_data_api_children(focused_value)
            populate_struct(
                target_node, focused_value, 1,
                ancestry | {identity(focused_value)}, [remaining[0]])

    if focus_node_path is None:
        pending_index = 0
        while pending_index < len(pending_structs):
            if remaining[0] <= 0:
                truncated[0] = True
                break
            (node, value, depth, ancestry, branch_remaining,
             item_remaining) = pending_structs[pending_index]
            pending_index += 1
            populate_struct(node, value, depth, ancestry, branch_remaining,
                            item_remaining)

    if focused_property_node is not None:
        focused_property_node["truncated"] = truncated[0]

    def has_data_collection(name):
        try:
            return len(getattr(bpy.data, name)) > 0
        except (AttributeError, ReferenceError, RuntimeError, TypeError):
            return False

    others_collections = (
        "curves", "metaballs", "hair_curves", "pointclouds", "volumes",
        "lightprobes", "lattices", "fonts", "speakers",
    )

    return {
        "root": root,
        "truncated": truncated[0],
        "driver_paths": driver_paths,
        "driver_paths_truncated": (
            driver_paths_truncated[0] or truncated[0]),
        "object_type_filter_availability": {
            "MESH": has_data_collection("meshes"),
            "ARMATURE": has_data_collection("armatures"),
            "LIGHT": has_data_collection("lights"),
            "CAMERA": has_data_collection("cameras"),
            "GREASE_PENCIL": has_data_collection("grease_pencils"),
            "EMPTY": True,
            "OTHERS": any(has_data_collection(name)
                          for name in others_collections),
        },
    }


def outliner_scene_keying_sets(scene):
    """Capture scene keying-set paths used by the Data API Outliner."""
    records = []
    active_key = ""
    truncated = False
    try:
        collection = scene.keying_sets
        active = collection.active
        keying_set_count = len(collection)
    except (AttributeError, ReferenceError, RuntimeError, TypeError):
        return records, active_key, truncated

    active_is_scene_set = active is not None and any(
        keying_set == active for keying_set in collection)
    scene_set_limit = (MAX_OUTLINER_KEYING_SETS
                       if active_is_scene_set or active is None
                       else MAX_OUTLINER_KEYING_SETS - 1)
    remaining_paths = MAX_OUTLINER_KEYING_SET_PATHS
    for index, keying_set in enumerate(collection):
        if index >= scene_set_limit:
            truncated = True
            break
        try:
            name = str(keying_set.bl_label)[:4096]
            key = "keying-set:%d:%s" % (index, name)
            is_absolute = bool(keying_set.is_path_absolute)
            source_paths = keying_set.paths
            type_info = keying_set.type_info
            relative_keying_set_type_id = (
                str(type_info.bl_idname) if not is_absolute and
                type_info is not None else "")
            relative_keyframe_supported = bool(
                relative_keying_set_type_id in
                SUPPORTED_OBJECT_RELATIVE_KEYING_SET_IDS and
                callable(type_info.poll) and callable(type_info.iterator))
        except (AttributeError, ReferenceError, RuntimeError, TypeError):
            continue
        paths = []
        for source_path in source_paths:
            if remaining_paths <= 0:
                truncated = True
                break
            try:
                id_block = source_path.id
                paths.append({
                    "id_type": str(source_path.id_type)[:256],
                    "id_name": (str(id_block.name_full)[:4096]
                                if isinstance(id_block, bpy.types.ID) else ""),
                    "library_path": (
                        source_library_path(id_block)[:4096]
                        if isinstance(id_block, bpy.types.ID) else ""),
                    "data_path": str(source_path.data_path)[:4096],
                    "array_index": int(source_path.array_index),
                    "use_entire_array": bool(
                        source_path.use_entire_array),
                    "group": str(source_path.group)[:4096],
                    "group_method": str(source_path.group_method)[:128],
                })
                remaining_paths -= 1
            except (AttributeError, ReferenceError, RuntimeError, TypeError,
                    ValueError):
                continue
        records.append({
            "key": key,
            "name": name,
            "is_absolute": is_absolute,
            "is_builtin": False,
            "relative_keyframe_supported": relative_keyframe_supported,
            "paths": paths,
        })
        if keying_set == active:
            active_key = key
    if keying_set_count > scene_set_limit:
        truncated = True

    if active is not None and not any(
            keying_set == active for keying_set in collection):
        try:
            is_absolute = bool(active.is_path_absolute)
            type_info = active.type_info
            type_id = (str(type_info.bl_idname)
                       if not is_absolute and type_info is not None else "")
            if type_id:
                name = str(active.bl_label)[:4096]
                active_key = "keying-set-info:" + type_id
                records.append({
                    "key": active_key,
                    "name": name,
                    "is_absolute": False,
                    "is_builtin": True,
                    "relative_keyframe_supported": bool(
                        type_id in SUPPORTED_OBJECT_RELATIVE_KEYING_SET_IDS and
                        callable(type_info.poll) and
                        callable(type_info.iterator)),
                    "paths": [],
                })
        except (AttributeError, ReferenceError, RuntimeError, TypeError):
            pass
    return records, active_key, truncated


def object_metadata(obj, scene, depsgraph=None):
    parent = obj.parent
    library_path = source_library_path(obj)
    animation_data = getattr(obj, "animation_data", None)
    action_slot = getattr(animation_data, "action_slot", None)
    driver_target_names = []
    if animation_data is not None:
        last_added_target = None
        for fcurve in animation_data.drivers:
            driver = fcurve.driver
            if driver is None or not driver.variables:
                continue
            for variable in driver.variables:
                for target in variable.targets:
                    target_id = target.id
                    if target_id is not None and target_id != last_added_target:
                        driver_target_names.append(target_id.name)
                        last_added_target = target_id
    nla_tracks = []
    if animation_data is not None:
        for track in list(animation_data.nla_tracks)[:1024]:
            nla_tracks.append({
                "name": track.name,
                "action_names": [
                    strip.action.name for strip in track.strips
                    if strip.action is not None
                ][:1024],
            })
    modifier_entries = []
    for modifier in list(obj.modifiers)[:1024]:
        linked_id = None
        linked_type = ""
        if modifier.type in ("LATTICE", "CURVE", "ARMATURE", "HOOK"):
            linked_id = getattr(modifier, "object", None)
            linked_type = "object"
        elif modifier.type == "NODES":
            linked_id = getattr(modifier, "node_group", None)
            linked_type = "node_tree"
        elif modifier.type == "PARTICLE_SYSTEM":
            linked_id = getattr(modifier, "particle_system", None)
            linked_type = "particle_system"
        modifier_entries.append({
            "name": modifier.name,
            "linked_name": linked_id.name if linked_id is not None else "",
            "linked_type": linked_type if linked_id is not None else "",
        })
    shader_effect_entries = []
    for effect in list(getattr(obj, "shader_effects", ()))[:
            MAX_OUTLINER_HIERARCHY_ITEMS]:
        linked_id = (
            getattr(effect, "object", None)
            if getattr(effect, "type", "") == "FX_SWIRL" else None
        )
        shader_effect_entries.append({
            "name": effect.name,
            "linked_name": linked_id.name if linked_id is not None else "",
            "linked_type": "object" if linked_id is not None else "",
        })
    grease_pencil_layer_tree = outliner_grease_pencil_layer_tree(obj)
    record = {
        "object_id": stable_id(obj),
        # Workbench Random hashes ID.name, which is obj.name in Blender's
        # Python API. name_full adds a linked-library suffix and would change
        # the Workbench color before the separate library path is mixed in.
        "name": obj.name,
        "data_name": getattr(getattr(obj, "data", None), "name_full", ""),
        "data_type": getattr(
            getattr(getattr(obj, "data", None), "bl_rna", None),
            "identifier", ""),
        "data_animation": data_block_animation_metadata(
            getattr(obj, "data", None)),
        "data_material_names": [
            material.name for material in
            getattr(getattr(obj, "data", None), "materials", ())
            if material is not None
        ][:1024],
        "data_material_animations": [
            data_block_animation_metadata(material) for material in
            getattr(getattr(obj, "data", None), "materials", ())
            if material is not None
        ][:1024],
        "shape_key_data_name": (
            obj.data.shape_keys.name
            if obj.type == "MESH" and obj.data is not None and
            obj.data.shape_keys is not None else ""
        ),
        "shape_key_names": [
            key.name for key in obj.data.shape_keys.key_blocks
        ][:1024] if obj.type == "MESH" and obj.data is not None and
            obj.data.shape_keys is not None else [],
        "material_slot_names": [
            slot.material.name for slot in obj.material_slots
            if slot.material is not None
        ][:1024],
        "material_slot_animations": [
            data_block_animation_metadata(slot.material)
            for slot in obj.material_slots if slot.material is not None
        ][:1024],
        "modifier_names": [modifier.name for modifier in obj.modifiers][:1024],
        "modifier_entries": modifier_entries,
        "shader_effect_entries": shader_effect_entries,
        "grease_pencil_layer_tree": grease_pencil_layer_tree,
        "constraint_names": [
            constraint.name for constraint in obj.constraints
        ][:1024],
        "vertex_group_names": [
            group.name for group in getattr(obj, "vertex_groups", ())
        ][:1024] if obj.type in ("MESH", "LATTICE", "GREASE_PENCIL") else [],
        "has_animation_data": bool(
            animation_data is not None and (
                animation_data.action is not None or animation_data.drivers or
                animation_data.nla_tracks
            )
        ),
        "animation_action_name": (
            animation_data.action.name
            if animation_data is not None and animation_data.action is not None
            else ""
        ),
        "animation_action_slot_name": (
            getattr(action_slot, "name_display", "")
            if action_slot is not None else ""
        ),
        "has_drivers": bool(
            animation_data is not None and animation_data.drivers
        ),
        "driver_target_names": driver_target_names[:1024],
        "nla_tracks": nla_tracks,
        "library_path": library_path,
        "asset_marked": getattr(obj, "asset_data", None) is not None,
        # Blender resolves a linked ID's relative filepath before Workbench
        # hashes it; preserve that render identity beside the source path.
        "workbench_random_library_path": (
            bpy.path.abspath(library_path) if library_path else ""
        ),
        "library_path_exists": source_library_exists(obj),
        "scene_name": scene.name_full,
        "parent_id": stable_id(parent) if parent is not None else "",
        "collection_ids": sorted(stable_id(collection) for collection in obj.users_collection),
        "visibility": visibility_metadata(obj),
        "showInFront": bool(obj.show_in_front),
        "show_wire": bool(getattr(obj, "show_wire", False)),
        "show_all_edges": bool(getattr(obj, "show_all_edges", False)),
        "wire_display": str(getattr(obj, "display_type", "")) == "WIRE",
        "bounds_display_type": (
            str(getattr(obj, "display_bounds_type", "BOX"))
            if str(getattr(obj, "display_type", "")) == "BOUNDS" else ""
        ),
        "display_color_linear": vector_values(obj.color),
        "classiCAD_geometry_type": obj.get("classiCAD_geometry_type", ""),
        "classiCAD_proxy_geometry": obj.get("classiCAD_proxy_geometry", ""),
        "classiCAD_data_block": obj.get("classiCAD_data_block", ""),
    }
    if str(getattr(obj, "display_type", "")) == "BOUNDS":
        bounds_obj = obj.evaluated_get(depsgraph) if depsgraph is not None else obj
        corners = [tuple(float(component) for component in corner)
                   for corner in bounds_obj.bound_box]
        if len(corners) == 8 and all(
                math.isfinite(component)
                for corner in corners for component in corner):
            record["bounds_min_local"] = [
                min(corner[axis] for corner in corners) for axis in range(3)]
            record["bounds_max_local"] = [
                max(corner[axis] for corner in corners) for axis in range(3)]
            record["bounds_transform_world"] = matrix_values(
                bounds_obj.matrix_world)
    if obj.type == "ARMATURE" and obj.data is not None:
        is_pose_mode = str(getattr(obj, "mode", "")) == "POSE"
        is_edit_mode = str(getattr(obj, "mode", "")) == "EDIT"
        record["has_pose"] = obj.pose is not None
        record["show_pose_channels"] = bool(
            obj.pose is not None and is_pose_mode and not is_edit_mode)
        if is_edit_mode:
            bones = getattr(obj.data, "edit_bones", ())
        else:
            bones = getattr(obj.data, "bones", ())
        record["armature_bone_tree"] = (
            [] if is_pose_mode else outliner_name_tree(bones))
        record["pose_bone_tree"] = (
            outliner_name_tree(obj.pose.bones, include_constraints=True)
            if record["show_pose_channels"] else [])
        record["bone_collection_tree"] = outliner_name_tree(
            getattr(obj.data, "collections", ()))
    return record


def extract_image_empty(obj, scene, packed_payload_budget,
                        transform_override=None, depsgraph=None):
    """Extract a still Image Empty into the existing Picture geometry path."""
    evaluated = obj.evaluated_get(depsgraph) if depsgraph is not None else obj
    if (obj.type != "EMPTY" or
            str(getattr(evaluated, "empty_display_type", "")) != "IMAGE"):
        return None, ""
    image = getattr(obj, "data", None)
    if image is None:
        return None, "Image Empty has no image data"
    image_source = str(getattr(image, "source", ""))
    if image_source not in {"FILE", "GENERATED"}:
        return None, "only still file-backed or generated Image Empties are supported"
    width, height = (int(value) for value in image.size[:2])
    if width <= 0 or height <= 0 or width * height > 16_777_216:
        return None, "Image Empty has missing or oversized image dimensions"
    filepath = (bpy.path.abspath(
        image.filepath, library=getattr(image, "library", None))
        if image_source == "FILE" else "")
    source_exists = bool(filepath and os.path.isfile(filepath))
    image_alpha_mode = str(getattr(image, "alpha_mode", "STRAIGHT"))
    force_opaque_alpha = image_alpha_mode == "NONE"
    if force_opaque_alpha:
        # Re-encode the decoded pixels so the ordinary QImage path also sees
        # opaque alpha; the original file or packed bytes may contain alpha.
        filepath = ""
        source_exists = False
    packed_data = b""
    packed_file = (None if force_opaque_alpha else
                   getattr(image, "packed_file", None))
    if packed_file is not None:
        packed_size = int(getattr(packed_file, "size", len(packed_file.data)))
        if (packed_size <= MAX_PACKED_IMAGE_BYTES and
                packed_payload_budget["bytes"] + packed_size <=
                MAX_PACKED_IMAGE_BYTES):
            packed_data = bytes(packed_file.data)
            if len(packed_data) <= MAX_PACKED_IMAGE_BYTES:
                packed_payload_budget["bytes"] += len(packed_data)
            else:
                packed_data = b""
    if image_source == "GENERATED" or force_opaque_alpha:
        try:
            pixels = image.pixels[:]
            expected_components = width * height * 4
            if len(pixels) != expected_components:
                raise ValueError("Blender did not expose a complete RGBA pixel array")
            png_rows = bytearray()
            for y in range(height):
                png_rows.append(0)
                # Blender's pixel buffer starts at the lower-left; PNG rows
                # start at the upper-left.
                row_offset = (height - 1 - y) * width * 4
                for component_index, component in enumerate(
                        pixels[row_offset:row_offset + width * 4]):
                    if force_opaque_alpha and component_index % 4 == 3:
                        png_rows.append(255)
                        continue
                    value = float(component)
                    if not math.isfinite(value):
                        raise ValueError("generated image has non-finite pixels")
                    png_rows.append(max(0, min(255, int(value * 255.0 + 0.5))))

            def png_chunk(kind, payload):
                chunk = kind + payload
                return (struct.pack(">I", len(payload)) + chunk +
                        struct.pack(">I", zlib.crc32(chunk) & 0xffffffff))

            packed_data = (b"\x89PNG\r\n\x1a\n" +
                           png_chunk(b"IHDR", struct.pack(
                               ">2I5B", width, height, 8, 6, 0, 0, 0)) +
                           png_chunk(b"IDAT", zlib.compress(bytes(png_rows))) +
                           png_chunk(b"IEND", b""))
        except Exception as error:
            return None, "Image Empty pixels could not be encoded: " + str(error)
        if (len(packed_data) > MAX_PACKED_IMAGE_BYTES or
                packed_payload_budget["bytes"] + len(packed_data) >
                MAX_PACKED_IMAGE_BYTES):
            return None, "Image Empty exceeds the aggregate 32 MiB transport limit after pixel normalization"
        packed_payload_budget["bytes"] += len(packed_data)
    if not packed_data and not source_exists:
        if packed_file is not None:
            return None, "packed Image Empty exceeds or exhausts the 32 MiB aggregate transport limit and has no available source path"
        return None, "Image Empty image is neither packed nor available at its resolved path"

    from mathutils import Vector
    pixel_aspect = getattr(image, "display_aspect", (1.0, 1.0))
    aspect_x = max(1.0e-12, width * float(pixel_aspect[0]))
    aspect_y = max(1.0e-12, height * float(pixel_aspect[1]))
    if aspect_x >= aspect_y:
        aspect_x, aspect_y = 1.0, aspect_y / aspect_x
    else:
        aspect_x, aspect_y = aspect_x / aspect_y, 1.0
    matrix = (transform_override if transform_override is not None
              else evaluated.matrix_world)
    display_size = float(evaluated.empty_display_size)
    half_x = (matrix.to_3x3() @ Vector((1.0, 0.0, 0.0))) * (
        aspect_x * display_size * 0.5)
    half_y = (matrix.to_3x3() @ Vector((0.0, 1.0, 0.0))) * (
        aspect_y * display_size * 0.5)
    offset = evaluated.empty_image_offset
    center = matrix.translation + half_x * (2.0 * float(offset[0]) + 1.0) + \
        half_y * (2.0 * float(offset[1]) + 1.0)
    corners = [center - half_x + half_y,
               center + half_x + half_y,
               center + half_x - half_y,
               center - half_x - half_y]
    source_transform = bake_image_source_transform(
        image, "image:%s:%s" %
        (image.name_full, source_library_path(image)) +
        (":alpha-none" if force_opaque_alpha else ""),
        force_opaque_alpha=force_opaque_alpha)
    item = object_metadata(obj, scene, depsgraph)
    item.update({
        "object_type": "EMPTY_IMAGE",
        "picture_corners_world": [vector_values(corner) for corner in corners],
        "picture_image_path": filepath if source_exists else "",
        "picture_image_packed_data_base64": (
            base64.b64encode(packed_data).decode("ascii") if packed_data else ""),
        "picture_color_linear": [finite_number(value) for value in evaluated.color],
        "picture_use_alpha_blend": bool(getattr(
            evaluated, "use_empty_image_alpha", False)),
        "picture_use_view_transform": bool(getattr(
            image, "use_view_as_render", False)),
        "picture_premultiplied_alpha": str(
            image_alpha_mode) == "PREMUL",
        "picture_color_space": image.colorspace_settings.name,
        "picture_source_transform_id": source_transform["id"],
        "picture_scene_linear_float_data_base64": source_transform["payload"],
        "picture_scene_linear_float_width": source_transform["width"],
        "picture_scene_linear_float_height": source_transform["height"],
        "picture_source_transform_error": source_transform["error"],
        "empty_image_depth": str(getattr(evaluated, "empty_image_depth", "DEFAULT")),
        "show_empty_image_perspective": bool(
            getattr(evaluated, "show_empty_image_perspective", True)),
        "show_empty_image_orthographic": bool(
            getattr(evaluated, "show_empty_image_orthographic", True)),
        "show_empty_image_only_axis_aligned": bool(
            getattr(evaluated, "show_empty_image_only_axis_aligned", False)),
        "empty_image_side": str(
            getattr(evaluated, "empty_image_side", "DOUBLE_SIDED")),
    })
    return item, ""


def extract_camera_background_images(scene, packed_payload_budget,
                                     unsupported_features):
    """Import still images displayed over the active scene camera frame."""
    camera = scene.camera
    if camera is None or not bool(getattr(camera.data,
                                           "show_background_images", False)):
        return []
    evaluated = camera.evaluated_get(depsgraph_for_scene(scene))
    try:
        from mathutils import Matrix, Vector
        frame = evaluated.data.view_frame(scene=scene)
    except Exception as error:
        add_unsupported(unsupported_features, stable_id(camera),
                        "camera_background_images",
                        "camera frame could not be evaluated: " + str(error))
        return []
    if len(frame) != 4:
        return []
    min_x = min(float(point.x) for point in frame)
    max_x = max(float(point.x) for point in frame)
    min_y = min(float(point.y) for point in frame)
    max_y = max(float(point.y) for point in frame)
    frame_width = max_x - min_x
    frame_height = max_y - min_y
    if (not math.isfinite(frame_width) or not math.isfinite(frame_height) or
            frame_width <= 0.0 or frame_height <= 0.0):
        return []
    frame_center_x = (min_x + max_x) * 0.5
    frame_center_y = (min_y + max_y) * 0.5
    records = []
    camera_id = stable_id(camera)
    view_layer_hidden = {
        view_layer_key(scene, view_layer): False
        for view_layer in scene.view_layers
    }
    default_layer_key = (view_layer_key(scene, scene.view_layers[0])
                         if len(scene.view_layers) else "")
    for index, background in enumerate(camera.data.background_images):
        if not bool(getattr(background, "show_background_image", True)):
            continue
        image = getattr(background, "image", None)
        if image is None:
            add_unsupported(
                unsupported_features, camera_id,
                "camera_background_image",
                "camera background image %d has no image data" % (index + 1))
            continue
        image_size = getattr(image, "size", (0, 0))
        if len(image_size) < 2 or int(image_size[0]) <= 0 or int(image_size[1]) <= 0:
            add_unsupported(
                unsupported_features, camera_id,
                "camera_background_image",
                "camera background image %d is not loaded" % (index + 1))
            continue
        display_aspect = getattr(image, "display_aspect", (1.0, 1.0))
        image_width = float(image_size[0]) * max(1.0e-12, float(display_aspect[0]))
        image_height = float(image_size[1]) * max(1.0e-12, float(display_aspect[1]))
        image_aspect = image_width / image_height
        frame_method = str(getattr(background, "frame_method", "FIT"))
        if frame_method == "STRETCH":
            display_width, display_height = frame_width, frame_height
        elif frame_method == "CROP":
            if image_aspect >= frame_width / frame_height:
                display_height = frame_height
                display_width = display_height * image_aspect
            else:
                display_width = frame_width
                display_height = display_width / image_aspect
        else:
            if image_aspect >= frame_width / frame_height:
                display_width = frame_width
                display_height = display_width / image_aspect
            else:
                display_height = frame_height
                display_width = display_height * image_aspect
        scale = max(0.0, float(getattr(background, "scale", 1.0)))
        display_width *= scale
        display_height *= scale
        rotation = float(getattr(background, "rotation", 0.0))
        if not math.isfinite(rotation):
            rotation = 0.0
        # Blender's camera background matrix keeps the camera frame depth,
        # uses the stored offset directly, and applies aspect corrections
        # after the orthographic scale adjustment.
        offset = getattr(background, "offset", (0.0, 0.0))
        camera_aspect = frame_width / frame_height
        offset_x = float(offset[0])
        offset_y = float(offset[1])
        if str(getattr(evaluated.data, "type", "PERSP")) == "ORTHO":
            ortho_scale = float(getattr(evaluated.data, "ortho_scale", 1.0))
            offset_x *= ortho_scale
            offset_y *= ortho_scale
        offset_x *= min(1.0, camera_aspect)
        offset_y /= (max(1.0, camera_aspect) *
                     (image_aspect / camera_aspect))
        center_x = frame_center_x + offset_x
        center_y = frame_center_y + offset_y
        frame_depth = max(1.0e-9, abs(float(frame[0].z)))
        clip_start = max(0.0, float(getattr(evaluated.data,
                                             "clip_start", 0.1)))
        clip_end = float(getattr(evaluated.data, "clip_end", 1000.0))
        camera_depth = frame_depth
        if (math.isfinite(clip_end) and clip_end * 0.999 >
                clip_start * 1.001):
            camera_depth = max(frame_depth, clip_end * 0.999)
        perspective = str(getattr(evaluated.data, "type", "PERSP")) != "ORTHO"
        camera_frame_scale = camera_depth / frame_depth if perspective else 1.0
        display_width *= camera_frame_scale
        display_height *= camera_frame_scale
        center_x *= camera_frame_scale
        center_y *= camera_frame_scale
        cosine = math.cos(rotation)
        sine = math.sin(rotation)
        half_width = display_width * 0.5
        half_height = display_height * 0.5
        local_corners = [(-half_width, half_height),
                         (half_width, half_height),
                         (half_width, -half_height),
                         (-half_width, -half_height)]
        flip_x = bool(getattr(background, "use_flip_x", False))
        flip_y = bool(getattr(background, "use_flip_y", False))
        # Keep perspective framing while moving the image card near the far
        # clip plane, so the depth-tested BACK variant stays behind visible
        # geometry in classiCAD's scene compositor.
        camera_matrix = evaluated.matrix_world @ Matrix.Translation(
            Vector((0.0, 0.0, -camera_depth)))
        corners_world = []
        for x, y in local_corners:
            if flip_x:
                x = -x
            if flip_y:
                y = -y
            # Blender rotates by -rotation around camera local Z.
            rotated_x = x * cosine + y * sine + center_x
            rotated_y = -x * sine + y * cosine + center_y
            corners_world.append(camera_matrix @ Vector((rotated_x,
                                                          rotated_y,
                                                          0.0)))
        proxy_name = "%s Camera Background %d" % (camera.name_full, index + 1)
        proxy = bpy.data.objects.new(proxy_name, None)
        try:
            proxy.empty_display_type = "IMAGE"
            proxy.data = image
            proxy.empty_display_size = 1.0
            proxy.empty_image_offset = (0.0, 0.0)
            proxy.matrix_world = camera_matrix
            # Blender keeps camera BACK images infinitesimally transparent so
            # they cannot occlude a transparent FRONT image composited later.
            alpha = min(0.999999, max(0.0, float(
                getattr(background, "alpha", 1.0))))
            proxy.color = (1.0, 1.0, 1.0, alpha)
            proxy.use_empty_image_alpha = True
            display_depth = str(getattr(background, "display_depth", "BACK"))
            # A depth-tested image at the camera frame models Blender's BACK
            # overlay against scene depth. FRONT remains an in-front overlay.
            picture_depth = "DEFAULT" if display_depth == "BACK" else "FRONT"
            proxy.empty_image_depth = picture_depth
            proxy.show_empty_image_perspective = True
            proxy.show_empty_image_orthographic = True
            proxy.show_empty_image_only_axis_aligned = False
            item, image_error = extract_image_empty(
                proxy, scene, packed_payload_budget)
        finally:
            bpy.data.objects.remove(proxy, do_unlink=True)
        if item is None:
            add_unsupported(
                unsupported_features, camera_id,
                "camera_background_image",
                "camera background image %d could not be imported: %s" %
                (index + 1, image_error or "unsupported image"))
            continue
        derived_id = hashlib.sha256(
            (camera_id + "::camera-background::" + str(index)).encode("utf-8")
        ).hexdigest()
        item["object_id"] = "classiCAD::camera-background::" + derived_id
        item["source_object_id"] = camera_id
        item["source_object_name"] = camera.name_full
        item["name"] = proxy_name
        item["parent_id"] = camera_id
        item["collection_ids"] = [stable_id(collection)
                                  for collection in camera.users_collection]
        item["picture_corners_world"] = [vector_values(point)
                                          for point in corners_world]
        camera_world_inverse = evaluated.matrix_world.inverted_safe()
        item["picture_camera_local_corners"] = [
            vector_values(camera_world_inverse @ point)
            for point in corners_world]
        item["picture_camera_source_lens_millimeters"] = finite_number(
            getattr(evaluated.data, "lens", 50.0))
        item["picture_camera_view_only"] = True
        item["picture_use_alpha_blend"] = True
        item["empty_image_depth"] = picture_depth
        item["view_layer_hidden"] = view_layer_hidden
        item["default_view_layer_key"] = default_layer_key
        records.append(item)
    return records


def loose_edge_curve_item(obj, scene, source_item, edges_world, edge_indices):
    """Build a curve companion without reusing the Blender mesh identity."""
    source_id = source_item["object_id"]
    derived_id = hashlib.sha256(source_id.encode("utf-8")).hexdigest()
    item = object_metadata(obj, scene)
    item["object_id"] = "classiCAD::derived::loose-edges::" + derived_id
    item["source_object_id"] = source_id
    item["source_object_name"] = obj.name_full
    item["name"] = obj.name_full + " (Loose Edges)"
    item["object_type"] = "CURVE"
    item["classiCAD_loose_mesh_edge_companion"] = True
    item["classiCAD_loose_mesh_edge_source_indices"] = [
        int(index) for index in edge_indices]
    item["splines"] = [
        {"type": "POLY", "control_points_world": edge}
        for edge in edges_world
    ]
    item["view_layer_hidden"] = source_item["view_layer_hidden"]
    item["default_view_layer_key"] = source_item["default_view_layer_key"]
    return item


def loose_vertex_point_items(source_item, points_world):
    """Build bounded Point records for mesh vertices with no incident edge."""
    source_id = source_item["object_id"]
    records = []
    metadata_fields = (
        "library_path", "workbench_random_library_path", "library_path_exists",
        "scene_name", "parent_id", "collection_ids", "visibility",
        "showInFront", "show_wire", "show_all_edges", "wire_display",
        "display_color_linear", "view_layer_hidden",
        "default_view_layer_key")
    for vertex_index, world_point in points_world:
        derived_id = hashlib.sha256(
            (source_id + "::loose-vertex::" + str(vertex_index))
            .encode("utf-8")).hexdigest()
        item = {field: source_item[field] for field in metadata_fields
                if field in source_item}
        item.update({
            "object_id": "classiCAD::derived::loose-vertex::" + derived_id,
            "source_object_id": source_id,
            "source_object_name": source_item["name"],
            # Keep Blender's source ID name so Workbench Random uses the same
            # color for its mesh and derived loose-vertex marker.
            "name": source_item["name"],
            "source_vertex_index": vertex_index,
            "object_type": "POINT",
            "point_world": world_point,
        })
        records.append(item)
    return records


def scene_camera_record(scene, depsgraph=None):
    camera = scene.camera
    if camera is None:
        return None
    evaluated = camera.evaluated_get(depsgraph) if depsgraph is not None else camera
    data = evaluated.data
    dof_focus_distance = finite_number(data.dof.focus_distance)
    focus_object = data.dof.focus_object
    if focus_object is not None:
        focus_evaluated = (focus_object.evaluated_get(depsgraph)
                           if depsgraph is not None else focus_object)
        camera_position = evaluated.matrix_world.translation
        focus_position = focus_evaluated.matrix_world.translation
        camera_backward = evaluated.matrix_world.to_3x3().col[2].normalized()
        # Match BKE_camera_object_dof_distance(): measure the focus target's
        # distance along the camera's local +Z axis, not Euclidean distance.
        dof_focus_distance = max(
            abs(camera_backward.dot(camera_position - focus_position)), 1.0e-5)
    return {
        "object_id": stable_id(camera),
        "name": camera.name_full,
        "object_type": "CAMERA",
        "matrix_world": matrix_values(evaluated.matrix_world),
        "camera_type": data.type,
        "show_passepartout": bool(getattr(data, "show_passepartout", False)),
        "passepartout_alpha": finite_number(
            getattr(data, "passepartout_alpha", 0.5)),
        "lens_millimeters": finite_number(data.lens),
        "sensor_fit": data.sensor_fit,
        "sensor_width_millimeters": finite_number(data.sensor_width),
        "sensor_height_millimeters": finite_number(data.sensor_height),
        "clip_start": finite_number(data.clip_start),
        "clip_end": finite_number(data.clip_end),
        "shift_x": finite_number(data.shift_x),
        "shift_y": finite_number(data.shift_y),
        "orthographic_scale": finite_number(data.ortho_scale),
        "dof_enabled": bool(data.dof.use_dof),
        "dof_focus_distance": finite_number(dof_focus_distance),
        "dof_f_stop": finite_number(data.dof.aperture_fstop),
        "dof_aperture_blades": int(data.dof.aperture_blades),
        "dof_aperture_rotation": finite_number(data.dof.aperture_rotation),
        "dof_aperture_ratio": finite_number(data.dof.aperture_ratio),
    }


def camera_viewport_extra_record(obj, scene, depsgraph):
    """Capture the depth-tested wire guide Blender draws for a camera object."""
    from mathutils import Vector

    evaluated = obj.evaluated_get(depsgraph) if depsgraph is not None else obj
    camera = evaluated.data
    display_size = finite_number(getattr(camera, "display_size", 1.0))
    if display_size <= 0.0:
        return None
    frame = camera.view_frame(scene=scene)
    if len(frame) != 4:
        return None

    matrix = evaluated.matrix_world
    basis = matrix.to_3x3()
    axes = []
    axis_scales = []
    for axis_index in range(3):
        axis = Vector(tuple(basis[row][axis_index] for row in range(3)))
        if axis.length <= 1.0e-12:
            return None
        axis_scales.append(float(axis.length))
        axes.append(axis.normalized())
    origin = matrix.translation.copy()

    is_orthographic = camera.type == "ORTHO"
    if is_orthographic:
        # Blender keeps an orthographic camera's frame at ortho_scale. The
        # display size sets only the camera body's depth in the object view.
        draw_size = 0.5 * finite_number(camera.ortho_scale)
        local_frame = [
            Vector((finite_number(point.x), finite_number(point.y), -display_size))
            for point in frame
        ]
        camera_view_frame = [
            Vector((finite_number(point.x), finite_number(point.y),
                    -(finite_number(camera.clip_start) + 0.1)))
            for point in frame
        ]
    else:
        # BKE_camera_view_frame_ex uses half the display size, adjusted by the
        # reciprocal object scale, for perspective camera guides.
        average_inverse_scale = sum(1.0 / scale for scale in axis_scales) / 3.0
        if not math.isfinite(average_inverse_scale) or average_inverse_scale <= 1.0e-12:
            return None
        draw_size = (display_size * 0.5) / average_inverse_scale
        # camera.view_frame() calls BKE_camera_view_frame_ex with drawsize=1,
        # whose perspective frame is already half-sized. The overlay call
        # passes camera.display_size, so its frame needs twice r_drawsize.
        frame_scale = 2.0 * draw_size
        local_frame = [
            Vector((finite_number(point.x) * frame_scale,
                    finite_number(point.y) * frame_scale,
                    finite_number(point.z) * frame_scale))
            for point in frame
        ]
        camera_view_frame = []
        for point in frame:
            depth = abs(float(point.z))
            if not math.isfinite(depth) or depth <= 1.0e-12:
                return None
            # Blender clips the active camera's frame to clip_start + 0.1.
            clip_scale = (finite_number(camera.clip_start) + 0.1) / depth
            camera_view_frame.append(Vector((
                finite_number(point.x) * clip_scale,
                finite_number(point.y) * clip_scale,
                finite_number(point.z) * clip_scale,
            )))

    def world_point(local_point):
        return origin + axes[0] * local_point.x + axes[1] * local_point.y + axes[2] * local_point.z

    local_frame_segments = []
    local_segments = []
    for index in range(4):
        local_frame_segments.append(
            (local_frame[index], local_frame[(index + 1) % 4]))
        local_segments.append(
            (local_frame[index], local_frame[(index + 1) % 4]))
        local_segments.append((Vector((0.0, 0.0, 0.0)), local_frame[index]))

    # Blender's camera overlay adds its recognizable triangle above the frame
    # when the camera is drawn as a scene object rather than as the active
    # camera-view border.
    center_x = sum(point.x for point in local_frame) * 0.25
    frame_top = max(point.y for point in local_frame)
    frame_depth = sum(point.z for point in local_frame) * 0.25
    triangle_size = 0.7 * draw_size
    triangle_base = frame_top + 0.1 * draw_size
    triangle = (
        Vector((center_x - triangle_size, triangle_base, frame_depth)),
        Vector((center_x + triangle_size, triangle_base, frame_depth)),
        Vector((center_x, triangle_base + 2.0 * triangle_size, frame_depth)),
    )
    local_segments.extend(((triangle[0], triangle[1]),
                           (triangle[1], triangle[2]),
                           (triangle[2], triangle[0])))

    def world_line_vertices_for_segments(segments):
        vertices = []
        for first, second in segments:
            vertices.append(vector_values(world_point(first)))
            vertices.append(vector_values(world_point(second)))
        return vertices

    camera_view_frame_segments = [
        (camera_view_frame[index], camera_view_frame[(index + 1) % 4])
        for index in range(4)
    ]
    camera_view_line_vertices = world_line_vertices_for_segments(
        camera_view_frame_segments)
    world_line_vertices = world_line_vertices_for_segments(local_segments)

    visible_view_layer_keys = []
    visible_default = True
    for index, view_layer in enumerate(scene.view_layers):
        try:
            visible = bool(obj.visible_get(view_layer=view_layer))
        except Exception:
            visible = not object_hidden_in_layer(obj, view_layer)
        if index == 0:
            visible_default = visible
        if visible:
            visible_view_layer_keys.append(view_layer_key(scene, view_layer))

    return {
        "object_id": stable_id(obj),
        "kind": "CAMERA",
        "is_active_camera": scene.camera == obj,
        "visible_default": visible_default,
        "visible_view_layer_keys": visible_view_layer_keys,
        "camera_view_line_vertices": camera_view_line_vertices,
        "world_line_vertices": world_line_vertices,
    }


def workbench_shading_record(scene, shading=None):
    """Capture Workbench Solid inputs, optionally from a saved 3D view."""
    if shading is None:
        shading = scene.display.shading
    matcap_flip_x = getattr(shading, "matcap_flip_x", None)
    world_space_lighting = getattr(shading, "use_world_space_lighting", None)
    if world_space_lighting is None:
        world_space_lighting = not bool(shading.use_studiolight_view_rotation)
    selected_studio_light = getattr(shading, "selected_studio_light", None)
    record = {
        # Blender stores viewport AA separately from render AA. Keep it with
        # the imported viewport shading record so opening a .blend selects
        # the saved viewport quality rather than classiCAD's local preference.
        "viewport_aa": str(scene.display.viewport_aa),
        "light": str(shading.light),
        "studio_light": str(getattr(shading, "studio_light", "")),
        "selected_studio_light": str(getattr(selected_studio_light, "name", "")),
        "color_type": str(shading.color_type),
        "single_color": vector_values(shading.single_color),
        "background_type": str(shading.background_type),
        "background_color": vector_values(shading.background_color),
        "show_shadows": bool(shading.show_shadows),
        "shadow_intensity": finite_number(shading.shadow_intensity),
        "show_cavity": bool(shading.show_cavity),
        "cavity_type": str(shading.cavity_type),
        "cavity_ridge_factor": finite_number(shading.cavity_ridge_factor),
        "cavity_valley_factor": finite_number(shading.cavity_valley_factor),
        "curvature_ridge_factor": finite_number(shading.curvature_ridge_factor),
        "curvature_valley_factor": finite_number(shading.curvature_valley_factor),
        "show_object_outline": bool(shading.show_object_outline),
        "object_outline_color": vector_values(shading.object_outline_color),
        "show_specular_highlight": bool(shading.show_specular_highlight),
        "use_world_space_lighting": bool(world_space_lighting),
        "show_backface_culling": bool(shading.show_backface_culling),
        "show_xray": bool(shading.show_xray),
        "show_xray_wireframe": bool(shading.show_xray_wireframe),
        "xray_alpha": finite_number(shading.xray_alpha),
        "xray_alpha_wireframe": finite_number(shading.xray_alpha_wireframe),
        "matcap_flip_x": (bool(matcap_flip_x)
                           if matcap_flip_x is not None else None),
        "use_dof": bool(shading.use_dof),
        "light_direction": vector_values(scene.display.light_direction),
        "shadow_shift": finite_number(scene.display.shadow_shift),
        "shadow_focus": finite_number(scene.display.shadow_focus),
        "cavity_distance": finite_number(scene.display.matcap_ssao_distance),
        "cavity_attenuation": finite_number(scene.display.matcap_ssao_attenuation),
        "cavity_samples": int(scene.display.matcap_ssao_samples),
        "use_scene_lights_render": bool(shading.use_scene_lights_render),
        "use_scene_world_render": bool(shading.use_scene_world_render),
        "studiolight_background_alpha": finite_number(shading.studiolight_background_alpha),
        "studiolight_background_blur": finite_number(shading.studiolight_background_blur),
        "studiolight_intensity": finite_number(shading.studiolight_intensity),
        "studiolight_rotate_z": finite_number(shading.studiolight_rotate_z),
        "unsupported": [],
    }
    unsupported = []
    if record["use_scene_lights_render"]:
        unsupported.append("use_scene_lights_render")
    if record["use_scene_world_render"]:
        unsupported.append("use_scene_world_render")
    # Blender 5.2.2's Workbench Solid renderer does not consume these
    # Eevee Lookdev background controls. Keep their raw values in the record
    # for future renderer support without reporting them as Workbench gaps.
    if matcap_flip_x is None:
        unsupported.append("matcap_flip_x (not exposed by Blender 5.2.2 scene shading RNA)")
    record["unsupported"] = unsupported
    return record


def _saved_view_screens(scene=None):
    screens = [screen for screen in bpy.data.screens
               if not bool(getattr(screen, "is_temporary", False))]
    screens.sort(key=lambda screen: screen.name_full)
    window = getattr(bpy.context, "window", None)
    active_screen = getattr(window, "screen", None)
    if ((scene is None or getattr(window, "scene", None) == scene) and
            active_screen in screens):
        screens.remove(active_screen)
        screens.insert(0, active_screen)
    return screens


def _saved_view_areas(screen):
    areas = sorted(screen.areas, key=lambda area: (-area.y, area.x))
    active_area = getattr(bpy.context, "area", None)
    if (active_area is not None and active_area.type == "VIEW_3D" and
            active_area in areas):
        areas.remove(active_area)
        areas.insert(0, active_area)
    return areas


def saved_viewport_shading_record(scene, unsupported_features):
    """Return the deterministically selected saved VIEW_3D shading record.

    Prefer the active window screen when that window is editing this scene.
    Otherwise screens are ordered by name; areas are ordered from top to
    bottom, then left to right. Temporary screens are ignored so this stays
    tied to a persistent saved layout rather than a transient popup view.
    """
    for screen in _saved_view_screens(scene):
        areas = _saved_view_areas(screen)
        for area in areas:
            if area.type == "VIEW_3D":
                space = area.spaces.active
                record = workbench_shading_record(scene, space.shading)
                record["viewport_shading_type"] = str(space.shading.type)
                overlay = space.overlay
                record["show_overlays"] = bool(overlay.show_overlays)
                record["show_extras"] = bool(overlay.show_extras)
                record["show_cursor"] = bool(
                    getattr(overlay, "show_cursor", True))
                record["show_camera_passepartout"] = bool(
                    getattr(overlay, "show_camera_passepartout", True))
                record["show_floor"] = bool(overlay.show_floor)
                record["show_ortho_grid"] = bool(overlay.show_ortho_grid)
                record["show_axis_x"] = bool(overlay.show_axis_x)
                record["show_axis_y"] = bool(overlay.show_axis_y)
                record["show_axis_z"] = bool(overlay.show_axis_z)
                record["show_outline_selected"] = bool(
                    overlay.show_outline_selected)
                record["show_wireframes"] = bool(overlay.show_wireframes)
                record["wireframe_color_type"] = str(
                    getattr(space.shading, "wireframe_color_type", "THEME"))
                record["wireframe_opacity"] = finite_number(
                    overlay.wireframe_opacity)
                record["wireframe_threshold"] = finite_number(
                    overlay.wireframe_threshold)
                if overlay.show_overlays:
                    unsupported_overlays = (
                        ("show_text", "viewport_text_labels",
                         "Blender viewport text labels are not displayed by classiCAD"),
                        ("show_stats", "viewport_statistics",
                         "Blender viewport statistics are not displayed by classiCAD"),
                        ("show_camera_guides", "viewport_camera_guides",
                         "Blender camera guide overlays are not displayed by classiCAD"),
                        ("show_relationship_lines", "viewport_relationship_lines",
                         "Blender parent relationship lines are not displayed by classiCAD"),
                        ("show_origins", "viewport_object_origins",
                         "Blender object origin markers are not displayed by classiCAD"),
                        ("show_bones", "viewport_bones",
                         "Blender armature bone overlays are not represented by classiCAD"),
                        ("show_motion_paths", "viewport_motion_paths",
                         "Blender motion path overlays are not represented by classiCAD"),
                        ("show_annotation", "viewport_annotations",
                         "Blender annotation overlays are not represented by classiCAD"),
                    )
                    for property_name, feature_name, reason in unsupported_overlays:
                        if bool(getattr(overlay, property_name, False)):
                            add_unsupported(unsupported_features,
                                            stable_id(scene),
                                            feature_name,
                                            reason)
                themes = getattr(bpy.context.preferences, "themes", ())
                if themes:
                    view_theme = getattr(themes[0], "view_3d", None)
                    if view_theme is not None:
                        for record_key, theme_key in (
                                ("theme_wire_color", "wire"),
                                ("theme_wire_background_color", "background"),
                                ("theme_active_object_color", "object_active"),
                                ("theme_selected_object_color", "object_selected"),
                                ("theme_transform_color", "transform")):
                            color = getattr(view_theme, theme_key, None)
                            if color is not None:
                                record[record_key] = vector_values(color)
                return record
    return None


def saved_viewport_view_record(scene):
    """Return the first saved free or scene-camera VIEW_3D view."""
    if scene is None:
        return None
    for screen in _saved_view_screens(scene):
        areas = _saved_view_areas(screen)
        for area in areas:
            if area.type != "VIEW_3D":
                continue
            space = area.spaces.active
            region = space.region_3d
            perspective = str(region.view_perspective)
            if perspective == "CAMERA":
                # This first camera-view port uses the scene camera record
                # already transported by the importer. A space-specific
                # camera needs its own camera-data record and is left for a
                # later extension.
                camera = getattr(space, "camera", None) or scene.camera
                if camera is None or camera != scene.camera:
                    continue
                offset = getattr(region, "view_camera_offset", (0.0, 0.0))
                try:
                    offset_x, offset_y = float(offset[0]), float(offset[1])
                    camera_zoom = float(region.view_camera_zoom)
                except (TypeError, ValueError, IndexError, AttributeError):
                    continue
                if (not math.isfinite(camera_zoom) or
                        not -30.0 <= camera_zoom <= 600.0 or
                        not math.isfinite(offset_x) or
                        not math.isfinite(offset_y) or
                        not -1.0 <= offset_x <= 1.0 or
                        not -1.0 <= offset_y <= 1.0):
                    continue
                return {
                    "view_perspective": "CAMERA",
                    "camera_zoom": camera_zoom,
                    "camera_offset_x": offset_x,
                    "camera_offset_y": offset_y,
                }
            if perspective not in {"PERSP", "ORTHO"}:
                continue
            rotation = region.view_rotation
            location = region.view_location
            return {
                "view_perspective": perspective,
                "view_location": vector_values(location),
                "view_rotation_wxyz": [
                    finite_number(rotation.w), finite_number(rotation.x),
                    finite_number(rotation.y), finite_number(rotation.z),
                ],
                "view_distance": finite_number(region.view_distance),
                "view_lens": finite_number(space.lens),
                "clip_start": finite_number(space.clip_start),
                "clip_end": finite_number(space.clip_end),
            }
    return None


def view_layer_key(scene, view_layer):
    return stable_id(scene) + "::view-layer::" + view_layer.name


def layer_collection_records(scene, view_layer):
    records = []
    active_layer_collection = view_layer.active_layer_collection
    nodes = []
    def visit(node, path, has_excluded_ancestor=False):
        collection_id = stable_id(node.collection)
        current_path = path + [collection_id]
        excluded = bool(node.exclude)
        records.append({
            "collection_id": collection_id,
            "path": current_path,
            "active": node == active_layer_collection,
            "exclude": excluded,
            "hide_viewport": bool(node.hide_viewport),
            "hide_render": bool(node.hide_render),
            "holdout": bool(node.holdout),
            "indirect_only": bool(node.indirect_only),
        })
        nodes.append((node, current_path, has_excluded_ancestor))
        for child in node.children:
            visit(child, current_path,
                  has_excluded_ancestor or excluded)
    visit(view_layer.layer_collection, [])

    excluded_roots = [
        (node, path) for node, path, has_excluded_ancestor in nodes
        if bool(node.exclude) and not has_excluded_ancestor
    ]
    previously_excluded_by_path = {}
    if excluded_roots:
        try:
            active_object = view_layer.objects.active
        except (AttributeError, ReferenceError, RuntimeError, TypeError):
            active_object = None
        selected_object_pointers = set()
        try:
            for obj in view_layer.objects:
                try:
                    if obj.select_get(view_layer=view_layer):
                        selected_object_pointers.add(obj.as_pointer())
                except (AttributeError, ReferenceError, RuntimeError, TypeError):
                    continue
        except (AttributeError, ReferenceError, RuntimeError, TypeError):
            pass

        try:
            for root, root_path in excluded_roots:
                try:
                    root.exclude = False
                    pending = [
                        (child, root_path) for child in root.children
                    ]
                    while pending:
                        node, parent_path = pending.pop()
                        current_path = parent_path + [
                            stable_id(node.collection)
                        ]
                        previously_excluded_by_path[tuple(current_path)] = (
                            bool(node.exclude)
                        )
                        pending.extend(
                            (child, current_path) for child in node.children
                        )
                finally:
                    # Blender's recursive update saves the just-restored
                    # descendant flags back into PREVIOUSLY_EXCLUDED while
                    # leaving the source view layer's visible state intact.
                    root.exclude = True
        finally:
            try:
                if active_layer_collection is not None:
                    view_layer.active_layer_collection = active_layer_collection
            except (AttributeError, ReferenceError, RuntimeError, TypeError):
                pass
            try:
                for obj in view_layer.objects:
                    try:
                        obj.select_set(
                            obj.as_pointer() in selected_object_pointers,
                            view_layer=view_layer,
                        )
                    except (AttributeError, ReferenceError, RuntimeError,
                            TypeError):
                        continue
            except (AttributeError, ReferenceError, RuntimeError, TypeError):
                pass
            if active_object is not None:
                try:
                    view_layer.objects.active = active_object
                except (AttributeError, ReferenceError, RuntimeError, TypeError):
                    pass

    for record in records:
        record["previously_excluded"] = previously_excluded_by_path.get(
            tuple(record["path"]), False
        )
    return records


def view_layer_record(scene, view_layer, active_scene, active_view_layer,
                      remaining_base_ids):
    collection_instances = layer_collection_records(scene, view_layer)
    active_collection_path = next(
        (record["path"] for record in collection_instances
         if record["active"]),
        [],
    )
    base_object_ids = []
    base_object_ids_complete = True
    try:
        for obj in view_layer.objects:
            if remaining_base_ids[0] <= 0:
                base_object_ids_complete = False
                break
            base_object_ids.append(stable_id(obj))
            remaining_base_ids[0] -= 1
    except (AttributeError, ReferenceError, RuntimeError, TypeError):
        base_object_ids_complete = False
    return {
        "name": view_layer.name,
        "key": view_layer_key(scene, view_layer),
        "is_active_view_layer": (
            scene == active_scene and view_layer == active_view_layer
        ),
        "selected_object_ids": sorted(
            stable_id(obj) for obj in scene.objects
            if obj.select_get(view_layer=view_layer)
        ),
        "active_object_id": (
            stable_id(view_layer.objects.active)
            if view_layer.objects.active is not None else ""
        ),
        "base_object_ids": base_object_ids,
        "base_object_ids_complete": base_object_ids_complete,
        "use_pass_combined": bool(view_layer.use_pass_combined),
        "use_pass_z": bool(view_layer.use_pass_z),
        "active_collection_path": active_collection_path,
        "collection_instances": collection_instances,
    }


def object_hidden_in_layer(obj, view_layer):
    try:
        return bool(obj.hide_get(view_layer=view_layer))
    except (RuntimeError, TypeError):
        return bool(obj.hide_get())


def mesh_payload_estimate(mesh, material_slot_count=0):
    vertex_count = len(mesh.vertices)
    polygon_count = len(mesh.polygons)
    loop_count = len(mesh.loops)
    edge_count = len(mesh.edges)
    uv_layer_count = len(mesh.uv_layers)
    attribute_value_count = sum(len(attribute.data)
                                for attribute in mesh.attributes)
    estimated_bytes = (
        vertex_count * 64 + loop_count * 192 + polygon_count * 80 +
        edge_count * 24 + loop_count * uv_layer_count * 64 +
        attribute_value_count * 128 + material_slot_count * 1024)
    return vertex_count, polygon_count, loop_count, edge_count, estimated_bytes


def check_mesh_payload(mesh, budget, item_id, reserve=False,
                       material_slot_count=0):
    counts = mesh_payload_estimate(mesh, material_slot_count)
    estimated_bytes = counts[4]
    if (estimated_bytes > MAX_MESH_OBJECT_ESTIMATED_BYTES or
            (reserve and budget["estimated_bytes"] + estimated_bytes >
             MAX_MESH_SCENE_ESTIMATED_BYTES)):
        budget["limit_exceeded"] = True
        raise ValueError(
            "mesh payload limit exceeded (estimated %d bytes for this mesh; "
            "%d of %d scene bytes already reserved)" %
            (estimated_bytes, budget["estimated_bytes"],
             MAX_MESH_SCENE_ESTIMATED_BYTES))
    if reserve:
        budget["estimated_bytes"] += estimated_bytes
    return counts


def extract_mesh_object(obj, scene, mode, depsgraph, materials, unsupported,
                        transform_override=None, evaluated_object_override=None,
                        mesh_payload_budget=None):
    item_id = stable_id(obj)
    item = object_metadata(obj, scene, depsgraph)
    item["object_type"] = obj.type
    item["import_mode"] = mode
    if obj.parent is not None:
        add_unsupported(unsupported, item_id, "parent_hierarchy",
                        "the object's world transform is preserved, but its Blender parent relationship is not represented")
    if item["library_path_exists"] is False:
        add_unsupported(unsupported, item_id, "linked_library",
                        "linked Blender library file is missing: " + item["library_path"])
    source_mesh = getattr(obj, "data", None)
    if (obj.type == "MESH" and source_mesh is not None and
            mesh_payload_budget is not None):
        # Reject oversized source geometry before requesting an evaluated
        # mesh, which may otherwise allocate substantially more data.
        check_mesh_payload(source_mesh, mesh_payload_budget, item_id,
                           material_slot_count=len(obj.material_slots))
    if (getattr(source_mesh, "shape_keys", None) is not None and
            len(source_mesh.shape_keys.key_blocks) > 1):
        if mode == "evaluated":
            reason = ("shape key animation and alternate key states are not represented; "
                      "only the current evaluated shape was imported")
        else:
            reason = ("shape keys are not represented; only the mesh's base geometry "
                      "was imported")
        add_unsupported(unsupported, item_id, "shape_keys", reason)
    if mode == "evaluated":
        evaluated_obj = (evaluated_object_override
                         if evaluated_object_override is not None
                         else obj.evaluated_get(depsgraph))
        transform = (transform_override if transform_override is not None
                     else evaluated_obj.matrix_world)
    else:
        evaluated_obj = None
        transform = (transform_override if transform_override is not None
                     else obj.evaluated_get(depsgraph).matrix_world
                     if depsgraph is not None else obj.matrix_world)
    try:
        mesh = (evaluated_obj.to_mesh(preserve_all_data_layers=True,
                                      depsgraph=depsgraph)
                if mode == "evaluated" else obj.data)
        if mesh is None:
            raise ValueError("Blender did not provide mesh data")
        source_obj = evaluated_obj if evaluated_obj is not None else obj
        if mesh_payload_budget is not None:
            check_mesh_payload(
                mesh, mesh_payload_budget, item_id, reserve=True,
                material_slot_count=len(source_obj.material_slots))
        item["transform_world"] = matrix_values(transform)
        item["mesh_name"] = mesh.name_full
        item["mesh_library_path"] = source_library_path(mesh)
        item["vertices_local"] = [vector_values(vertex.co) for vertex in mesh.vertices]
        item["polygons"] = [
            [int(vertex_index) for vertex_index in polygon.vertices]
            for polygon in mesh.polygons
        ]
        polygon_edges = set()
        polygon_vertices = set()
        for polygon in mesh.polygons:
            vertices = [int(vertex_index) for vertex_index in polygon.vertices]
            polygon_vertices.update(vertices)
            if len(vertices) > 1:
                for index, vertex_index in enumerate(vertices):
                    next_vertex_index = vertices[(index + 1) % len(vertices)]
                    polygon_edges.add(tuple(sorted((vertex_index,
                                                    next_vertex_index))))
        mesh_edges = [
            tuple(sorted((int(edge.vertices[0]), int(edge.vertices[1]))))
            for edge in mesh.edges
        ]
        loose_edges = [
            (int(edge.index), int(edge.vertices[0]), int(edge.vertices[1]))
            for edge in mesh.edges
            if tuple(sorted((int(edge.vertices[0]), int(edge.vertices[1]))))
            not in polygon_edges
        ]
        edge_vertices = {vertex_index for edge in mesh_edges for vertex_index in edge}
        isolated_vertex_indices = [
            int(vertex.index) for vertex in mesh.vertices
            if int(vertex.index) not in polygon_vertices and
            int(vertex.index) not in edge_vertices
        ]
        if isolated_vertex_indices and obj.type == "MESH" and not item[
                "bounds_display_type"] and mesh_payload_budget is not None:
            isolated_count = len(isolated_vertex_indices)
            if (isolated_count > MAX_LOOSE_VERTICES_PER_OBJECT or
                    mesh_payload_budget["loose_vertex_point_count"] +
                    isolated_count > MAX_LOOSE_VERTEX_POINTS_PER_SCENE):
                mesh_payload_budget["loose_vertex_limit_exceeded"] = True
                item["_loose_vertices_omitted"] = isolated_count
            else:
                mesh_payload_budget["loose_vertex_point_count"] += isolated_count
                item["_loose_vertices_world"] = [
                    (vertex_index,
                     vector_values(transform @ mesh.vertices[vertex_index].co))
                    for vertex_index in isolated_vertex_indices
                ]
        # PolygonMesh3D stores faces only. Preserve true loose edges as a
        # companion CURVE transport item, using world points so evaluated
        # object transforms are baked exactly once by the existing curve
        # parser. The scene loop removes this private field before emitting
        # the mesh record.
        if loose_edges and obj.type == "MESH":
            item["_loose_edges_world"] = [
                [
                    vector_values(transform @ mesh.vertices[first].co),
                    vector_values(transform @ mesh.vertices[second].co),
                ]
                for _edge_index, first, second in loose_edges
            ]
            item["_loose_edge_source_indices"] = [
                edge_index for edge_index, _first, _second in loose_edges]
        item["polygon_material_indices"] = [
            int(polygon.material_index) for polygon in mesh.polygons
        ]
        item["loop_vertex_indices"] = [int(loop.vertex_index) for loop in mesh.loops]
        mesh.calc_loop_triangles()
        face_triangles = [[] for _polygon in mesh.polygons]
        for triangle in mesh.loop_triangles:
            polygon = mesh.polygons[triangle.polygon_index]
            face_triangles[triangle.polygon_index].append([
                int(loop_index - polygon.loop_start)
                for loop_index in triangle.loops
            ])
        item["polygon_loop_triangles"] = face_triangles

        slots = []
        for slot_index, slot in enumerate(source_obj.material_slots):
            material = slot.material
            record = material_record(material)
            if record is not None:
                materials[record["material_id"]] = record
                if record["library_path_exists"] is False:
                    add_unsupported(
                        unsupported, item_id,
                        "material_library:" + record["name"],
                        "linked Blender material library file is missing: "
                        + record["library_path"],
                    )
                for image in record["image_textures"]:
                    if (image["image_library_path_exists"] is False and
                            not image["packed_data_base64"] and not any(
                                tile["packed_data_base64"] for tile in image["tiles"])):
                        add_unsupported(
                            unsupported, item_id,
                            "image_library:" + image["image_name"],
                            "linked Blender image library file is missing: "
                            + image["image_library_path"],
                        )
                    if image["packed_data_error"]:
                        add_unsupported(
                            unsupported, item_id,
                            "image_texture:" + image["image_name"],
                            image["packed_data_error"],
                        )
                    if image["source_transform_error"]:
                        add_unsupported(
                            unsupported, item_id,
                            "image_color_space:" + image["image_name"],
                            image["source_transform_error"],
                        )
                    for tile in image["tiles"]:
                        if tile["source_transform_error"]:
                            add_unsupported(
                                unsupported, item_id,
                                "image_color_space_tile:%s:%d" %
                                (image["image_name"], tile["number"]),
                                tile["source_transform_error"],
                            )
                        if tile["packed_data_error"]:
                            add_unsupported(
                                unsupported, item_id,
                                "image_texture_tile:%s:%d" %
                                (image["image_name"], tile["number"]),
                                tile["packed_data_error"],
                            )
                    if not image["source_file_exists"]:
                        add_unsupported(
                            unsupported, item_id,
                            "image_texture:" + image["image_name"],
                            "external image is missing or has no filepath: "
                            + image["filepath"],
                        )
            slots.append({
                "slot_index": slot_index,
                "material_id": record["material_id"] if record else "",
                "link_mode": slot.link,
            })
        item["material_slots"] = slots

        item["uv_layers"] = [
            {
                "name": layer.name,
                "active": bool(layer.active),
                "active_render": bool(layer.active_render),
                "domain": "CORNER",
                "values": [vector_values(loop.uv) for loop in layer.data],
            }
            for layer in mesh.uv_layers
        ]
        item["active_uv_layer"] = (
            mesh.uv_layers.active.name if mesh.uv_layers.active else ""
        )
        item["default_uv_layer"] = (
            mesh.uv_layers.active_render.name if mesh.uv_layers.active_render else ""
        )
        item["color_attributes"] = extract_attributes(mesh, item_id, unsupported)
        active_color = getattr(mesh, "color_attributes", None)
        item["active_color_attribute"] = (
            active_color.active_color.name
            if active_color is not None and active_color.active_color else ""
        )
        default_color = getattr(active_color, "default_color", None)
        item["default_color_attribute"] = (
            default_color.name if default_color is not None else ""
        )
        item["custom_split_normals_present"] = bool(
            getattr(mesh, "has_custom_normals", False)
        )
        # Blender exposes the evaluated split normal per loop even when the
        # mesh has no separately authored custom-normal layer. Retaining this
        # array preserves smooth/sharp seams in the imported display mesh.
        item["corner_normals"] = [
            vector_values(loop.normal) for loop in mesh.loops
        ]
        if mode == "evaluated":
            item["evaluated_geometry_baked"] = True
            add_unsupported(unsupported, item_id, "modifier_editability",
                            "evaluated mesh is baked and no longer parametrically editable")
        else:
            item["evaluated_geometry_baked"] = False
            for modifier in obj.modifiers:
                add_unsupported(unsupported, item_id,
                                "modifier:" + modifier.name,
                                "original mode keeps source mesh; modifier was not applied")
        for slot in source_obj.material_slots:
            if slot.material is not None and slot.material.use_nodes:
                add_unsupported(unsupported, item_id,
                                "shader_nodes:" + slot.material.name_full,
                                "node graphs are not converted; Workbench display values and image references are extracted")
        return item
    finally:
        if evaluated_obj is not None:
            evaluated_obj.to_mesh_clear()


def extract_scalar_volume_grid(obj, scene, depsgraph,
                               transform_override=None):
    """Extract one bounded scalar VolumeGrid through Geometry Nodes fields.

    Blender exposes grid metadata but not voxel arrays through bpy. Grid To
    Points exposes active voxel/tile indices and values as fields; storing
    those fields on a temporary realized mesh makes them readable through the
    supported evaluated-mesh attribute API.
    """
    evaluated_source_object = obj.evaluated_get(depsgraph)
    evaluated_geometry = evaluated_source_object.evaluated_geometry()
    evaluated_volume = evaluated_geometry.volume
    volume = evaluated_volume if evaluated_volume is not None else obj.data
    grids = list(volume.grids)
    if not grids:
        raise ValueError("volume has no readable grids")
    float_grids = [grid for grid in grids
                   if grid.data_type == "FLOAT" and grid.channels == 1]
    if not float_grids:
        raise ValueError("volume has no single-channel FLOAT grid")
    named_density = [grid for grid in float_grids if grid.name == "density"]
    if named_density:
        grid = named_density[0]
    elif len(float_grids) == 1:
        grid = float_grids[0]
    else:
        raise ValueError("volume has multiple FLOAT grids and no unique density grid")

    if not grid.is_loaded:
        load_result = grid.load()
        if load_result is False or not grid.is_loaded:
            raise ValueError("volume grid could not be loaded")

    node_group = None
    helper_mesh = None
    helper_object = None
    evaluated_object = None
    evaluated_mesh = None
    try:
        node_group = bpy.data.node_groups.new(
            "classiCAD Volume Extraction", "GeometryNodeTree")
        node_group.interface.new_socket(
            name="Geometry", in_out="OUTPUT", socket_type="NodeSocketGeometry")
        nodes = node_group.nodes
        object_info = nodes.new("GeometryNodeObjectInfo")
        object_info.inputs["Object"].default_value = obj
        object_info.transform_space = "ORIGINAL"
        named_grid = nodes.new("GeometryNodeGetNamedGrid")
        named_grid.data_type = "FLOAT"
        named_grid.inputs["Name"].default_value = grid.name
        named_grid.inputs["Remove"].default_value = False
        grid_points = nodes.new("GeometryNodeGridToPoints")
        grid_points.data_type = "FLOAT"
        grid_info = nodes.new("GeometryNodeGridInfo")
        grid_info.data_type = "FLOAT"
        points_to_vertices = nodes.new("GeometryNodePointsToVertices")
        output = nodes.new("NodeGroupOutput")
        node_group.links.new(object_info.outputs["Geometry"],
                             named_grid.inputs["Volume"])
        node_group.links.new(named_grid.outputs["Grid"], grid_points.inputs["Grid"])
        node_group.links.new(named_grid.outputs["Grid"], grid_info.inputs["Grid"])
        node_group.links.new(grid_points.outputs["Points"],
                             points_to_vertices.inputs["Points"])

        geometry_socket = points_to_vertices.outputs["Mesh"]
        captured_fields = [
            ("value", "FLOAT", grid_points.outputs["Value"]),
            ("x", "INT", grid_points.outputs["X"]),
            ("y", "INT", grid_points.outputs["Y"]),
            ("z", "INT", grid_points.outputs["Z"]),
            ("is_tile", "BOOLEAN", grid_points.outputs["Is Tile"]),
            ("extent", "INT", grid_points.outputs["Extent"]),
            ("background", "FLOAT", grid_info.outputs["Background Value"]),
        ]
        for attribute_name, data_type, field_socket in captured_fields:
            store = nodes.new("GeometryNodeStoreNamedAttribute")
            store.domain = "POINT"
            store.data_type = data_type
            store.inputs["Name"].default_value = "_classiCAD_volume_" + attribute_name
            node_group.links.new(geometry_socket, store.inputs["Geometry"])
            node_group.links.new(field_socket, store.inputs["Value"])
            geometry_socket = store.outputs["Geometry"]
        node_group.links.new(geometry_socket, output.inputs["Geometry"])

        helper_mesh = bpy.data.meshes.new("classiCAD Volume Extraction Mesh")
        helper_object = bpy.data.objects.new("classiCAD Volume Extraction", helper_mesh)
        scene.collection.objects.link(helper_object)
        modifier = helper_object.modifiers.new("classiCAD Volume Extraction", "NODES")
        modifier.node_group = node_group
        bpy.context.view_layer.update()
        evaluated_object = helper_object.evaluated_get(depsgraph)
        evaluated_mesh = evaluated_object.to_mesh(
            preserve_all_data_layers=True, depsgraph=depsgraph)
        if evaluated_mesh is None:
            raise ValueError("Geometry Nodes did not realize the volume grid")
        if len(evaluated_mesh.vertices) <= 0:
            raise ValueError("volume grid has no active voxels")
        if len(evaluated_mesh.vertices) > MAX_VOLUME_GRID_RECORDS:
            raise ValueError("volume grid exceeds the active record cap (%d)" %
                             MAX_VOLUME_GRID_RECORDS)

        def attribute_data(name):
            attribute = evaluated_mesh.attributes.get("_classiCAD_volume_" + name)
            if attribute is None or len(attribute.data) != len(evaluated_mesh.vertices):
                raise ValueError("volume grid extraction is missing attribute " + name)
            return attribute.data

        values = attribute_data("value")
        xs = attribute_data("x")
        ys = attribute_data("y")
        zs = attribute_data("z")
        tile_flags = attribute_data("is_tile")
        extents = attribute_data("extent")
        background_values = attribute_data("background")
        background = float(background_values[0].value)
        if not math.isfinite(background):
            raise ValueError("volume grid background value is non-finite")

        records = []
        encoded_record_bytes = 0
        expanded_voxel_count = 0
        minimum = [None, None, None]
        maximum = [None, None, None]
        for index in range(len(evaluated_mesh.vertices)):
            x = int(xs[index].value)
            y = int(ys[index].value)
            z = int(zs[index].value)
            extent = int(extents[index].value)
            is_tile = bool(tile_flags[index].value)
            value = float(values[index].value)
            if extent < 1 or (not is_tile and extent != 1):
                raise ValueError("volume grid has an invalid voxel tile extent")
            if not math.isfinite(value):
                raise ValueError("volume grid contains a non-finite voxel value")
            expanded_voxel_count += extent * extent * extent
            if expanded_voxel_count > MAX_VOLUME_GRID_VOXELS:
                raise ValueError("volume grid exceeds the expanded voxel cap (%d)" %
                                 MAX_VOLUME_GRID_VOXELS)
            coordinates = (x, y, z)
            for axis in range(3):
                lower = coordinates[axis]
                upper = lower + extent - 1
                minimum[axis] = lower if minimum[axis] is None else min(minimum[axis], lower)
                maximum[axis] = upper if maximum[axis] is None else max(maximum[axis], upper)
            record = [x, y, z, extent, is_tile, value]
            encoded_record_bytes += len(json.dumps(
                record, separators=(",", ":"), allow_nan=False).encode("utf-8"))
            if encoded_record_bytes + len(records) > MAX_VOLUME_GRID_PAYLOAD_BYTES:
                raise ValueError("volume grid payload exceeds the byte cap (%d)" %
                                 MAX_VOLUME_GRID_PAYLOAD_BYTES)
            records.append(record)

        active_bounds = minimum + maximum
        dimensions = [maximum[axis] - minimum[axis] + 1 for axis in range(3)]
        dense_voxel_count = dimensions[0] * dimensions[1] * dimensions[2]
        if dense_voxel_count > MAX_VOLUME_GRID_VOXELS:
            raise ValueError("volume grid dense bounds exceed the voxel cap (%d)" %
                             MAX_VOLUME_GRID_VOXELS)
        grid_payload = {
            "name": grid.name,
            "data_type": grid.data_type,
            "channels": int(grid.channels),
            "grid_to_object": matrix_values(grid.matrix_object),
            "object_to_world": matrix_values(
                transform_override if transform_override is not None
                else obj.matrix_world),
            "background": background,
            "active_bounds": active_bounds,
            "expanded_voxel_count": expanded_voxel_count,
            "dense_voxel_count": dense_voxel_count,
            "active_records": records,
        }
        # Estimate the final compact JSON size without serializing the full
        # potentially large record array a second time.
        payload_header = dict(grid_payload)
        payload_header["active_records"] = []
        payload_bytes = len(json.dumps(
            payload_header, separators=(",", ":"), allow_nan=False).encode("utf-8"))
        payload_bytes += encoded_record_bytes + max(0, len(records) - 1)
        if payload_bytes > MAX_VOLUME_GRID_PAYLOAD_BYTES:
            raise ValueError("volume grid payload exceeds the byte cap (%d)" %
                             MAX_VOLUME_GRID_PAYLOAD_BYTES)

        return {
            "volume_grid": grid_payload,
            "_transport_metrics": {
                "payload_bytes": payload_bytes,
                "active_record_count": len(records),
                "dense_voxel_count": dense_voxel_count,
            },
            "volume_display": {
                "density": finite_number(obj.data.display.density),
                "render_space": str(obj.data.render.space),
                "interpolation_method": str(obj.data.display.interpolation_method),
                "use_slice": bool(obj.data.display.use_slice),
                "slice_axis": str(obj.data.display.slice_axis),
                "slice_depth": finite_number(obj.data.display.slice_depth),
            },
        }
    finally:
        if evaluated_object is not None and evaluated_mesh is not None:
            evaluated_object.to_mesh_clear()
        if helper_object is not None:
            bpy.data.objects.remove(helper_object, do_unlink=True)
        if helper_mesh is not None and helper_mesh.users == 0:
            bpy.data.meshes.remove(helper_mesh)
        if node_group is not None and node_group.users == 0:
            bpy.data.node_groups.remove(node_group)


def unsupported_object_record(obj, scene, depsgraph=None):
    record = object_metadata(obj, scene, depsgraph)
    record["object_type"] = obj.type
    if obj.type == "EMPTY" and obj.instance_collection is not None:
        record["reason"] = "collection instance is metadata only; no geometry was fabricated"
        record["instance_collection"] = obj.instance_collection.name_full
        record["instance_collection_name"] = obj.instance_collection.name
        record["instance_collection_id"] = stable_id(obj.instance_collection)
    elif obj.type == "EMPTY" and str(
            getattr(obj, "empty_display_type", "")) == "IMAGE":
        record["reason"] = "Image Empty image is not supported as a still file-backed picture"
        image = getattr(obj, "data", None)
        if image is not None:
            record["image_name"] = image.name_full
            record["image_path"] = bpy.path.abspath(
                image.filepath, library=getattr(image, "library", None))
    elif obj.type == "EMPTY":
        record["reason"] = (
            "Empty display icons are viewport overlays; only still Image Empties have a supported classiCAD representation")
    elif obj.type == "CURVE":
        record["reason"] = "curve has no supported POLY, BEZIER, or NURBS spline"
    elif obj.type == "SURFACE":
        record["reason"] = "surface has no supported NURBS patch"
    elif obj.type == "FONT":
        record["reason"] = "FONT objects require evaluated glyph conversion; source text is not represented"
    elif obj.type == "META":
        record["reason"] = "METABALL objects require evaluated surface conversion; original metaball data is not represented"
    elif obj.type == "VOLUME":
        record["reason"] = "volume has no supported single-channel FLOAT grid"
    elif obj.type == "GREASEPENCIL":
        record["reason"] = "Grease Pencil has no supported current-frame stroke centerlines"
    elif obj.type == "CURVES":
        record["reason"] = "Hair Curves has no supported strand centerlines"
    elif obj.type == "POINTCLOUD":
        record["reason"] = (
            "Point Cloud has no supported evaluated point-based component")
    else:
        record["reason"] = "no matching classiCAD scene-object geometry type"
    return record


def extract_point_cloud_item(obj, scene, depsgraph,
                             transform_override=None,
                             evaluated_object_override=None,
                             max_points=MAX_POINT_CLOUD_POINTS_PER_SCENE):
    """Extract one evaluated Blender point cloud as a bounded grouped object."""
    from mathutils import Vector

    evaluated_object = (evaluated_object_override or
                        obj.evaluated_get(depsgraph))
    evaluated_geometry = evaluated_object.evaluated_geometry()
    point_cloud = (getattr(evaluated_geometry, "pointcloud", None)
                   if evaluated_geometry is not None else None)
    if point_cloud is None:
        point_cloud = getattr(evaluated_object, "data", None)
    if point_cloud is None or getattr(point_cloud, "type", "POINTS") != "POINTS":
        raise ValueError("evaluated geometry is not a point-based Point Cloud")
    points = point_cloud.points
    point_count = len(points)
    if point_count <= 0:
        raise ValueError("point cloud has no points")
    if point_count > max_points:
        raise ValueError("point cloud point limit exceeded")

    positions = [0.0] * (point_count * 3)
    radii = [0.0] * point_count
    points.foreach_get("co", positions)
    points.foreach_get("radius", radii)
    transform = (transform_override if transform_override is not None
                 else evaluated_object.matrix_world)
    axis_scales = [float(value) for value in transform.to_scale()]
    if any(not math.isfinite(scale) for scale in axis_scales):
        raise ValueError("point cloud object transform is invalid")
    # Blender 5.2.2's Workbench point-cloud shader scales the sphere radius by
    # length(to_scale(object_to_world)) / sqrt(3), including nonuniform scales.
    radius_scale = math.sqrt(sum(scale * scale for scale in axis_scales) / 3.0)
    if not math.isfinite(radius_scale):
        raise ValueError("point cloud object transform is invalid")

    color_attribute = point_cloud.attributes.get("color")
    colors = None
    if (color_attribute is not None and
            color_attribute.domain == "POINT" and
            color_attribute.data_type in {"FLOAT_COLOR", "BYTE_COLOR"} and
            len(color_attribute.data) == point_count):
        raw_colors = [0.0] * (point_count * 4)
        color_attribute.data.foreach_get("color", raw_colors)
        colors = [
            raw_colors[index * 4:index * 4 + 4]
            for index in range(point_count)
        ]

    item = object_metadata(obj, scene, depsgraph)
    item["object_type"] = "POINTCLOUD"
    item["pointcloud_type"] = "POINTS"
    item["points"] = []
    for index in range(point_count):
        position = Vector(positions[index * 3:index * 3 + 3])
        world = transform @ position
        # Blender's signed radius field still controls a nonnegative point
        # extent in the Workbench point primitive.
        radius = abs(float(radii[index])) * radius_scale
        if (not all(math.isfinite(float(value)) for value in world) or
                not math.isfinite(radius) or radius < 0.0):
            raise ValueError("point cloud contains a non-finite position or radius")
        point_record = {
            "position_world": [float(world.x), float(world.y), float(world.z)],
            "radius_world": radius,
            "source_index": index,
        }
        if colors is not None:
            point_record["color_linear"] = colors[index]
        item["points"].append(point_record)
    item["_transport_metrics"] = {"point_count": point_count}
    return item


def extract_poly_curve_object(obj, scene, unsupported, depsgraph=None,
                              transform_override=None):
    """Extract supported source splines in world XYZ."""
    from mathutils import Vector

    item_id = stable_id(obj)
    item = object_metadata(obj, scene, depsgraph)
    item["object_type"] = "CURVE"
    item["splines"] = []
    transform = (transform_override if transform_override is not None
                 else (obj.evaluated_get(depsgraph).matrix_world
                       if depsgraph is not None else obj.matrix_world))
    if obj.parent is not None:
        add_unsupported(unsupported, item_id, "parent_hierarchy",
                        "the object's world-space spline geometry is preserved, but its Blender parent relationship is not represented")
    if item["library_path_exists"] is False:
        add_unsupported(unsupported, item_id, "linked_library",
                        "linked Blender library file is missing: " + item["library_path"])
    curve_data = obj.data
    if float(curve_data.bevel_depth) > 0.0 or float(curve_data.extrude) > 0.0:
        add_unsupported(
            unsupported, item_id, "curve_surface_geometry",
            "bevel or extrude surface geometry is omitted; only source spline centerlines are imported",
        )
    if getattr(curve_data, "bevel_object", None) is not None:
        add_unsupported(
            unsupported, item_id, "curve_bevel_object",
            "bevel-object surface geometry is omitted; only source spline centerlines are imported",
        )
    if (curve_data.dimensions == "2D" and
            getattr(curve_data, "fill_mode", "NONE") != "NONE"):
        add_unsupported(
            unsupported, item_id, "curve_2d_fill",
            "2D curve fill surfaces are omitted; only source spline centerlines are imported",
        )
    for spline_index, spline in enumerate(obj.data.splines):
        if spline.type not in {"POLY", "BEZIER", "NURBS"}:
            add_unsupported(
                unsupported, item_id,
                "spline:%d:%s" % (spline_index, spline.type),
                "unsupported Blender spline type",
            )
            continue
        if spline.type == "POLY":
            points = []
            for point in spline.points:
                world = transform @ Vector((point.co[0], point.co[1], point.co[2]))
                points.append(vector_values(world))
            if spline.use_cyclic_u and points and points[-1] != points[0]:
                points.append(list(points[0]))
            if len(points) < 2:
                add_unsupported(unsupported, item_id,
                                "spline:%d:POLY" % spline_index,
                                "POLY spline has fewer than two control points")
                continue
            item["splines"].append({
                "type": "POLY",
                "resolution_u": max(1, int(spline.resolution_u)),
                "control_points_world": points,
            })
            continue

        if spline.type == "NURBS":
            source_points = list(spline.points)
            order = int(spline.order_u)
            if len(source_points) < order or order < 2:
                add_unsupported(unsupported, item_id,
                                "spline:%d:NURBS" % spline_index,
                                "NURBS spline has fewer control points than its order")
                continue
            cyclic = bool(spline.use_cyclic_u)
            weights = []
            points = []
            for point in source_points:
                world = transform @ Vector((point.co[0], point.co[1], point.co[2]))
                points.append(vector_values(world))
                weights.append(finite_number(point.co[3]))
            if cyclic:
                for index in range(order - 1):
                    points.append(list(points[index]))
                    weights.append(weights[index])
            full_knots = blender_nurbs_knots(
                len(source_points), order, cyclic,
                bool(spline.use_endpoint_u), bool(spline.use_bezier_u))
            if len(full_knots) != len(points) + order:
                raise ValueError("Blender generated an inconsistent NURBS knot vector")
            item["splines"].append({
                "type": "NURBS",
                "resolution_u": max(1, int(spline.resolution_u)),
                "degree": order - 1,
                "control_points_world": points,
                "weights": weights,
                "rational": any(weight != 1.0 for weight in weights),
                "knots": full_knots[1:-1],
            })
            continue

        bezier_points = list(spline.bezier_points)
        count = len(bezier_points)
        segment_count = count if spline.use_cyclic_u else max(0, count - 1)
        spans = []
        for segment_index in range(segment_count):
            start = bezier_points[segment_index]
            end = bezier_points[(segment_index + 1) % count]
            local_points = (start.co, start.handle_right, end.handle_left, end.co)
            span = []
            for point in local_points:
                world = transform @ Vector((point[0], point[1], point[2]))
                span.append(vector_values(world))
            spans.append(span)
        if not spans:
            add_unsupported(unsupported, item_id,
                            "spline:%d:BEZIER" % spline_index,
                            "open BEZIER spline has fewer than two control points")
            continue
        item["splines"].append({
            "type": "BEZIER",
            "resolution_u": max(1, int(spline.resolution_u)),
            "spans_world": spans,
        })
    for modifier in obj.modifiers:
        add_unsupported(unsupported, item_id, "modifier:" + modifier.name,
                        "exact spline import preserves source control points; modifier was not applied")
    return item


def curve_has_surface_output(obj):
    curve_data = obj.data
    return (
        float(curve_data.bevel_depth) > 0.0 or
        float(curve_data.extrude) > 0.0 or
        getattr(curve_data, "bevel_object", None) is not None or
        (curve_data.dimensions == "2D" and
         getattr(curve_data, "fill_mode", "NONE") != "NONE") or
        bool(obj.modifiers)
    )


def extract_grease_pencil_curve_item(obj, scene, unsupported,
                                     transform_override=None,
                                     evaluated_obj=None,
                                     depsgraph=None,
                                     max_strokes=MAX_GREASE_PENCIL_STROKES_PER_SCENE,
                                     max_points=MAX_GREASE_PENCIL_POINTS_PER_SCENE):
    """Import current-frame Grease Pencil stroke centerlines as POLY curves."""
    from mathutils import Vector

    item_id = stable_id(obj)
    item = object_metadata(obj, scene, depsgraph)
    item["object_type"] = "CURVE"
    item["source_object_type"] = "GREASEPENCIL"
    item["splines"] = []
    source = evaluated_obj or obj
    object_transform = (transform_override if transform_override is not None
                        else source.matrix_world)
    point_count = 0
    stroke_count = 0

    for layer in source.data.layers:
        hidden_by_group = False
        group = getattr(layer, "parent_group", None)
        while group is not None:
            if bool(getattr(group, "hide", False)):
                hidden_by_group = True
                break
            group = getattr(group, "parent_group", None)
        if (hidden_by_group or bool(getattr(layer, "hide", False)) or
                float(getattr(layer, "opacity", 1.0)) <= 0.0):
            continue
        if getattr(layer, "parent", None) is not None:
            add_unsupported(
                unsupported, item_id, "grease_pencil_layer_parent:" + layer.name,
                "a Grease Pencil layer with an object or bone parent was skipped because its parent transform is not represented",
            )
            continue
        frame = layer.current_frame()
        drawing = getattr(frame, "drawing", None) if frame is not None else None
        if drawing is None or getattr(drawing, "type", "DRAWING") != "DRAWING":
            continue
        layer_transform = object_transform @ layer.matrix_local
        for stroke in drawing.strokes:
            points = stroke.points
            if len(points) < 2:
                # A one-point Grease Pencil dot has no curve counterpart.
                continue
            if stroke_count >= max_strokes:
                raise ValueError("Grease Pencil stroke limit exceeded")
            if point_count + len(points) > max_points:
                raise ValueError("Grease Pencil point limit exceeded")
            world_points = []
            for point in points:
                world = layer_transform @ Vector(point.position)
                world_points.append(vector_values(world))
            if bool(stroke.cyclic) and world_points[-1] != world_points[0]:
                world_points.append(list(world_points[0]))
            item["splines"].append({
                "type": "POLY",
                "control_points_world": world_points,
            })
            stroke_count += 1
            point_count += len(points)

    item["_transport_metrics"] = {
        "stroke_count": stroke_count,
        "point_count": point_count,
    }
    add_unsupported(
        unsupported, item_id, "grease_pencil_centerline",
        "current-frame Grease Pencil strokes were imported as POLY centerlines; one-point dots, stroke width, opacity, color, fills, layer effects, modifiers, and animation were not retained",
    )
    return item


def extract_hair_curve_item(obj, scene, unsupported, transform_override=None,
                            depsgraph=None,
                            max_curves=MAX_HAIR_CURVES_PER_SCENE,
                            max_points=MAX_HAIR_CURVE_POINTS_PER_SCENE):
    """Import Blender Hair Curves control paths as ordinary POLY curves."""
    from mathutils import Vector

    item_id = stable_id(obj)
    item = object_metadata(obj, scene, depsgraph)
    item["object_type"] = "CURVE"
    item["source_object_type"] = "CURVES"
    item["splines"] = []
    transform = (transform_override if transform_override is not None
                 else (obj.evaluated_get(depsgraph).matrix_world
                       if depsgraph is not None else obj.matrix_world))
    data = obj.data
    curve_count = len(data.curves)
    point_count = len(data.points)
    if curve_count > max_curves:
        raise ValueError("Hair Curves strand limit exceeded")
    if point_count > max_points:
        raise ValueError("Hair Curves point limit exceeded")
    offsets = [int(value.value) for value in data.curve_offset_data]
    if (len(offsets) != curve_count + 1 or not offsets or offsets[0] != 0 or
            offsets[-1] != point_count or
            any(first > second for first, second in zip(offsets, offsets[1:]))):
        raise ValueError("Hair Curves has inconsistent point offsets")

    for curve_index in range(curve_count):
        first = offsets[curve_index]
        end = offsets[curve_index + 1]
        if end - first < 2:
            continue
        points = []
        for point_index in range(first, end):
            world = transform @ Vector(data.points[point_index].position)
            points.append(vector_values(world))
        item["splines"].append({
            "type": "POLY",
            "control_points_world": points,
        })
    item["_transport_metrics"] = {
        "curve_count": curve_count,
        "point_count": point_count,
    }
    if item["splines"]:
        add_unsupported(
            unsupported, item_id, "hair_curve_centerlines",
            "Hair Curves control points were imported as straight POLY centerlines; one-point strands, curve interpolation, strand radius, profile, color, material effects, and modifier results were not retained",
        )
    return item


def curve_surface_item_id(source_item_id):
    digest = hashlib.sha256(
        (source_item_id + "::evaluated-curve-surface").encode("utf-8")
    ).hexdigest()
    return "classiCAD::derived::curve-surface::" + digest


def source_curve_features_after_surface_bake(features):
    baked_features = {"curve_surface_geometry", "curve_bevel_object",
                      "curve_2d_fill"}
    return [feature for feature in features
            if feature.get("feature") not in baked_features and
            not feature.get("feature", "").startswith("modifier:")]


def extract_curve_surface_mesh(obj, scene, source_item_id, materials,
                               unsupported, depsgraph=None,
                               transform_override=None,
                               mesh_payload_budget=None):
    """Bake visible curve surfaces as a separate display-mesh item."""
    if not curve_has_surface_output(obj):
        return None
    mesh_features = []
    try:
        evaluated_depsgraph = depsgraph or depsgraph_for_scene(scene)
        evaluated_obj = obj.evaluated_get(evaluated_depsgraph)
        item = extract_mesh_object(
            obj, scene, "evaluated", evaluated_depsgraph, materials,
            mesh_features, transform_override=transform_override,
            evaluated_object_override=evaluated_obj,
            mesh_payload_budget=mesh_payload_budget)
    except Exception as error:
        add_unsupported(
            unsupported, source_item_id, "curve_surface_proxy",
            "evaluated curve surface could not be baked: " + str(error))
        return None
    if not item.get("polygons"):
        return None

    item["object_type"] = "MESH"
    item["object_id"] = curve_surface_item_id(source_item_id)
    item["source_object_id"] = source_item_id
    item["source_object_name"] = obj.name_full
    item["source_object_type"] = "CURVE"
    item["name"] = obj.name_full + " (Evaluated Curve Surface)"
    item["evaluated_curve_surface_proxy"] = True
    add_unsupported(
        mesh_features, item["object_id"], "curve_surface_baked",
        "the visible curve surface was imported as a baked mesh; bevel, extrusion, fill, and modifier settings are not editable",
    )
    unsupported.extend(mesh_features)
    return item


def extract_surface_patches(obj, scene, materials, unsupported, depsgraph=None,
                            transform_override=None):
    """Extract each Blender SURFACE NURBS patch as a separate item."""
    from mathutils import Vector

    source_id = stable_id(obj)
    transform = (transform_override if transform_override is not None
                 else (obj.evaluated_get(depsgraph).matrix_world
                       if depsgraph is not None else obj.matrix_world))
    if obj.parent is not None:
        add_unsupported(unsupported, source_id, "parent_hierarchy",
                        "the surface world-space control net is preserved, but its Blender parent relationship is not represented")
    if source_library_exists(obj) is False:
        add_unsupported(unsupported, source_id, "linked_library",
                        "linked Blender library file is missing: " + source_library_path(obj))
    material_ids = []
    for slot in obj.material_slots:
        record = material_record(slot.material)
        if record is not None:
            materials[record["material_id"]] = record
            material_ids.append(record["material_id"])
            if record["library_path_exists"] is False:
                add_unsupported(
                    unsupported, source_id, "material_library:" + record["name"],
                    "linked Blender material library file is missing: " +
                    record["library_path"])
            for image in record["image_textures"]:
                if (image["image_library_path_exists"] is False and
                        not image["packed_data_base64"] and not any(
                            tile["packed_data_base64"] for tile in image["tiles"])):
                    add_unsupported(
                        unsupported, source_id,
                        "image_library:" + image["image_name"],
                        "linked Blender image library file is missing: " +
                        image["image_library_path"],
                    )
                if image["packed_data_error"]:
                    add_unsupported(
                        unsupported, source_id,
                        "image_texture:" + image["image_name"],
                        image["packed_data_error"],
                    )
                if image["source_transform_error"]:
                    add_unsupported(
                        unsupported, source_id,
                        "image_color_space:" + image["image_name"],
                        image["source_transform_error"],
                    )
                for tile in image["tiles"]:
                    if tile["source_transform_error"]:
                        add_unsupported(
                            unsupported, source_id,
                            "image_color_space_tile:%s:%d" %
                            (image["image_name"], tile["number"]),
                            tile["source_transform_error"],
                        )
                    if tile["packed_data_error"]:
                        add_unsupported(
                            unsupported, source_id,
                            "image_texture_tile:%s:%d" %
                            (image["image_name"], tile["number"]),
                            tile["packed_data_error"],
                        )
                if not image["source_file_exists"]:
                    add_unsupported(
                        unsupported, source_id,
                        "image_texture:" + image["image_name"],
                        "external image is missing or has no filepath: " +
                        image["filepath"],
                    )
        else:
            material_ids.append("")
    patches = []
    for spline_index, spline in enumerate(obj.data.splines):
        if spline.type != "NURBS":
            add_unsupported(unsupported, source_id,
                            "surface_spline:%d:%s" % (spline_index, spline.type),
                            "only NURBS surface patches have an exact surface representation")
            continue
        count_u = int(spline.point_count_u)
        count_v = int(spline.point_count_v)
        order_u = int(spline.order_u)
        order_v = int(spline.order_v)
        if (order_u < 2 or order_v < 2 or count_u < order_u or
                count_v < order_v or len(spline.points) != count_u * count_v):
            add_unsupported(unsupported, source_id,
                            "surface_spline:%d:NURBS" % spline_index,
                            "surface control net is smaller than its U/V orders")
            continue

        cyclic_u = bool(spline.use_cyclic_u)
        cyclic_v = bool(spline.use_cyclic_v)
        expanded_u = count_u + (order_u - 1 if cyclic_u else 0)
        expanded_v = count_v + (order_v - 1 if cyclic_v else 0)
        world_points = []
        weights = []
        # Blender's BPoint array is U-fast; classiCAD's surface net is V-fast.
        for u_index in range(expanded_u):
            source_u = u_index % count_u
            for v_index in range(expanded_v):
                source_v = v_index % count_v
                point = spline.points[source_u + source_v * count_u]
                world = transform @ Vector(
                    (point.co[0], point.co[1], point.co[2]))
                world_points.append(vector_values(world))
                weights.append(finite_number(point.co[3]))

        full_knots_u = blender_nurbs_knots(
            count_u, order_u, cyclic_u,
            bool(spline.use_endpoint_u), bool(spline.use_bezier_u))
        full_knots_v = blender_nurbs_knots(
            count_v, order_v, cyclic_v,
            bool(spline.use_endpoint_v), bool(spline.use_bezier_v))
        if (len(full_knots_u) != expanded_u + order_u or
                len(full_knots_v) != expanded_v + order_v):
            raise ValueError("Blender generated inconsistent surface knot vectors")

        item = object_metadata(obj, scene, depsgraph)
        item["object_id"] = "%s::surface:%d" % (source_id, spline_index)
        item["source_object_id"] = source_id
        item["object_type"] = "SURFACE"
        item["surface"] = {
            "degree_u": order_u - 1,
            "degree_v": order_v - 1,
            "order_u": order_u,
            "order_v": order_v,
            "control_vertex_count_u": expanded_u,
            "control_vertex_count_v": expanded_v,
            # Blender evaluates each legacy surface spline with independent
            # U/V preview resolutions per control-net segment.
            "display_grid_u": max(1, count_u * max(1, int(spline.resolution_u))),
            "display_grid_v": max(1, count_v * max(1, int(spline.resolution_v))),
            "control_points_world": world_points,
            "weights": weights,
            "rational": any(weight != 1.0 for weight in weights),
            "knots_u": full_knots_u[1:-1],
            "knots_v": full_knots_v[1:-1],
            "material_id": (
                material_ids[spline.material_index]
                if 0 <= spline.material_index < len(material_ids) else ""
            ),
        }
        if obj.modifiers:
            for modifier in obj.modifiers:
                add_unsupported(
                    unsupported, item["object_id"], "modifier:" + modifier.name,
                    "exact surface import preserves source patches; modifier was not applied")
        patches.append(item)
    # Blender draws legacy SURFACE objects from evaluated edge paths. Evaluate
    # each source spline separately so its display lines stay with its exact
    # imported NURBS patch.
    if patches and depsgraph is not None and not obj.modifiers:
        for item in patches:
            spline_index = int(item["object_id"].rsplit(":", 1)[1])
            temporary_data = obj.data.copy()
            temporary_object = None
            try:
                try:
                    for index in reversed(range(len(temporary_data.splines))):
                        if index != spline_index:
                            temporary_data.splines.remove(
                                temporary_data.splines[index])
                    temporary_object = bpy.data.objects.new(
                        "__classiCAD_surface_wire_eval__", temporary_data)
                    scene.collection.objects.link(temporary_object)
                    temporary_object.matrix_world = transform.copy()
                    with bpy.context.temp_override(
                            scene=scene, view_layer=scene.view_layers[0]):
                        bpy.context.view_layer.update()
                    evaluated = temporary_object.evaluated_get(depsgraph)
                    evaluated_mesh = evaluated.to_mesh()
                    try:
                        if len(evaluated_mesh.edges) <= 200000:
                            wire_vertices = []
                            for edge in evaluated_mesh.edges:
                                first = evaluated.matrix_world @ evaluated_mesh.vertices[
                                    edge.vertices[0]].co
                                second = evaluated.matrix_world @ evaluated_mesh.vertices[
                                    edge.vertices[1]].co
                                wire_vertices.append(vector_values(first))
                                wire_vertices.append(vector_values(second))
                            item["surface_wire_vertices_world"] = wire_vertices
                    finally:
                        evaluated.to_mesh_clear()
                except Exception as error:
                    add_unsupported(
                        unsupported, item["object_id"], "surface_wire",
                        "Blender evaluated wire paths could not be captured: " +
                        str(error))
            finally:
                if temporary_object is not None:
                    bpy.data.objects.remove(temporary_object, do_unlink=True)
                if temporary_data.users == 0:
                    bpy.data.curves.remove(temporary_data)
                with bpy.context.temp_override(
                        scene=scene, view_layer=scene.view_layers[0]):
                    bpy.context.view_layer.update()
    return patches


def depsgraph_for_scene(scene):
    if not scene.view_layers:
        raise RuntimeError("scene has no view layer for evaluated mesh extraction")
    view_layer = scene.view_layers[0]
    with bpy.context.temp_override(scene=scene, view_layer=view_layer):
        return bpy.context.evaluated_depsgraph_get()


def extract_scene(input_path, mode):
    global SCENE_LINEAR_FLOAT_PAYLOAD_BYTES
    SCENE_LINEAR_FLOAT_PAYLOAD_BYTES = 0
    SCENE_LINEAR_FLOAT_PAYLOAD_CACHE.clear()
    global SOURCE_PROJECT_NAMESPACE
    require_pinned_blender()
    if mode not in {"original", "evaluated"}:
        raise ValueError("mode must be 'original' or 'evaluated'")
    bpy.ops.wm.open_mainfile(filepath=os.path.abspath(input_path))
    require_pinned_blender()
    source_filepath = bpy.data.filepath
    SOURCE_PROJECT_NAMESPACE = source_project_namespace(source_filepath)
    active_window = getattr(bpy.context, "window", None)
    active_scene = active_window.scene if active_window is not None else None
    if active_scene not in bpy.data.scenes:
        active_scene = bpy.data.scenes[0] if bpy.data.scenes else None
    active_view_layer = (
        active_window.view_layer
        if active_window is not None and active_window.scene == active_scene
        else None
    )
    if active_scene is None or active_view_layer not in active_scene.view_layers:
        active_view_layer = (
            active_scene.view_layers[0]
            if active_scene is not None and active_scene.view_layers else None
        )

    output = {
        "format": BRIDGE_FORMAT,
        "format_version": BRIDGE_FORMAT_VERSION,
        "source": {
            "filepath": source_filepath,
            "project_namespace": SOURCE_PROJECT_NAMESPACE,
            "blender_version": bpy.app.version_string,
            "blender_version_tuple": list(bpy.app.version[:3]),
            "import_mode": mode,
        },
        "active_scene_id": stable_id(active_scene) if active_scene is not None else "",
        "active_view_layer_key": (
            view_layer_key(active_scene, active_view_layer)
            if active_scene is not None and active_view_layer is not None else ""
        ),
        "saved_viewport_view": saved_viewport_view_record(active_scene),
        "outliner_libraries": outliner_library_tree(),
        "outliner_orphans": outliner_orphan_groups(),
        "outliner_sequencer": outliner_sequencer_tree(
            active_window, active_scene),
        "outliner_data_api": outliner_data_api_tree(),
        "outliner_library_overrides": outliner_library_overrides_tree(),
        "scenes": [],
        "collections": [],
        "items": [],
        "unsupported_items": [],
        "unsupported_features": [],
        "materials": {},
        "color_luts": [],
        "summary": {},
    }
    lut_bytes_by_id = {}
    object_count = 0
    mesh_count = 0
    surface_patch_count = 0
    font_mesh_count = 0
    volume_grid_count = 0
    packed_empty_image_payload_budget = {"bytes": 0}
    remaining_view_layer_base_ids = [MAX_OUTLINER_VIEW_LAYER_BASE_IDS]
    collections_by_id = {stable_id(collection): collection
                         for collection in bpy.data.collections}
    scene_names_by_collection = {}
    states_by_collection = {}
    for scene in bpy.data.scenes:
        pending = [scene.collection]
        visited = set()
        while pending:
            collection = pending.pop()
            collection_id = stable_id(collection)
            if collection_id in visited:
                continue
            visited.add(collection_id)
            collections_by_id[collection_id] = collection
            scene_names_by_collection.setdefault(collection_id, set()).add(scene.name_full)
            pending.extend(reversed(list(collection.children)))
        for view_layer in scene.view_layers:
            key = view_layer_key(scene, view_layer)
            for state in layer_collection_records(scene, view_layer):
                states_by_collection.setdefault(state["collection_id"], []).append({
                    "view_layer_key": key,
                    "scene_name": scene.name_full,
                    "view_layer_name": view_layer.name,
                    "path": state["path"],
                    "exclude": state["exclude"],
                    "previously_excluded": state["previously_excluded"],
                    "hide_viewport": state["hide_viewport"],
                    "hide_render": state["hide_render"],
                    "holdout": state["holdout"],
                    "indirect_only": state["indirect_only"],
                })
    parent_ids_by_collection = {}
    parent_orders_by_collection = {}
    master_collection_ids = set()
    for scene in bpy.data.scenes:
        master_collection_ids.add(stable_id(scene.collection))
    for parent in collections_by_id.values():
        parent_id = stable_id(parent)
        for child_order, child in enumerate(parent.children):
            parent_ids_by_collection.setdefault(stable_id(child), set()).add(
                parent_id
            )
            parent_orders_by_collection.setdefault(
                stable_id(child), []).append({
                    "parent_id": parent_id,
                    "order": child_order,
                })
    for collection in collections_by_id.values():
        collection_id = stable_id(collection)
        output["collections"].append({
            "collection_id": collection_id,
            "name": collection.name_full,
            "color_tag": str(collection.color_tag),
            "is_master": collection_id in master_collection_ids,
            "library_path": source_library_path(collection),
            "asset_marked": getattr(collection, "asset_data", None) is not None,
            "parent_ids": sorted(parent_ids_by_collection.get(collection_id, set())),
            "parent_orders": parent_orders_by_collection.get(collection_id, []),
            "source_scene_names": sorted(scene_names_by_collection.get(collection_id, set())),
            "hide_viewport": bool(collection.hide_viewport),
            "hide_render": bool(collection.hide_render),
            "hide_select": bool(collection.hide_select),
            "instance_offset": [float(value) for value in collection.instance_offset],
            "view_layer_states": states_by_collection.get(collection_id, []),
        })
    for scene in bpy.data.scenes:
        # Original mode preserves source mesh data, but Blender still displays
        # objects with their current dependency-graph world transforms.
        depsgraph = depsgraph_for_scene(scene)
        camera_depsgraph = depsgraph
        render = scene.render
        (keying_sets, active_keying_set_key,
         keying_sets_truncated) = outliner_scene_keying_sets(scene)
        scene_record = {
            "scene_id": stable_id(scene),
            "is_active_scene": scene == active_scene,
            "name": scene.name_full,
            "cursor_location": vector_values(scene.cursor.location),
            "keying_sets": keying_sets,
            "active_keying_set_key": active_keying_set_key,
            "keying_sets_truncated": keying_sets_truncated,
            "library_path": source_library_path(scene),
            "world_name": scene.world.name_full if scene.world else "",
            "world_animation_data": data_block_animation_metadata(scene.world),
            "scene_animation_data": data_block_animation_metadata(scene),
            "view_layer_names": [view_layer.name for view_layer in scene.view_layers],
            "view_layers": [
                view_layer_record(scene, view_layer, active_scene,
                                  active_view_layer,
                                  remaining_view_layer_base_ids)
                for view_layer in scene.view_layers
            ],
            "collection_ids": sorted(
                collection_id for collection_id, scene_names in scene_names_by_collection.items()
                if scene.name_full in scene_names
            ),
            "object_ids": [],
            "viewport_extras": [],
            # Camera and focus-object transforms are evaluated scene state,
            # even when mesh import is explicitly set to preserve original
            # (unevaluated) geometry.
            "active_camera": scene_camera_record(scene, camera_depsgraph),
            "workbench_shading": workbench_shading_record(scene),
            "workbench_viewport_shading": saved_viewport_shading_record(
                scene, output["unsupported_features"]),
            "color_management": bake_color_lut(
                scene, output["color_luts"], lut_bytes_by_id,
                output["unsupported_features"]),
            "render_output": {
                # Resolve Blender's project-relative `//` path at import time.
                # Keep frame tokens intact; classiCAD does not expand them.
                "filepath": resolved_render_filepath(render),
                "use_file_extension": bool(render.use_file_extension),
                "resolution_x": int(render.resolution_x),
                "resolution_y": int(render.resolution_y),
                "resolution_percentage": int(render.resolution_percentage),
                "use_border": bool(render.use_border),
                "use_crop_to_border": bool(render.use_crop_to_border),
                "border_min_x": finite_number(render.border_min_x),
                "border_min_y": finite_number(render.border_min_y),
                "border_max_x": finite_number(render.border_max_x),
                "border_max_y": finite_number(render.border_max_y),
                "pixel_aspect_x": finite_number(render.pixel_aspect_x),
                "pixel_aspect_y": finite_number(render.pixel_aspect_y),
                "dither_intensity": finite_number(
                    getattr(render, "dither_intensity", 1.0)),
                # Blender backs RenderSettings.film_transparent with the
                # R_ALPHAPREMUL render alpha-mode bit used by Workbench.
                "film_transparent": bool(render.film_transparent),
                # World.color is scene-linear and is the opaque Workbench
                # render background when shading.background_type is not
                # VIEWPORT.
                "world_color_scene_linear": (
                    vector_values(scene.world.color) if scene.world else None
                ),
                "background_type": str(scene.display.shading.background_type),
                "render_aa": str(scene.display.render_aa),
                "file_format": render.image_settings.file_format,
                "exr_codec": str(getattr(render.image_settings,
                                          "exr_codec", "ZIP")),
                "color_mode": render.image_settings.color_mode,
                "color_depth": render.image_settings.color_depth,
                "quality": render.image_settings.quality,
                "compression": render.image_settings.compression,
                "tiff_codec": render.image_settings.tiff_codec,
            },
        }
        grease_pencil_stroke_count = 0
        grease_pencil_point_count = 0
        grease_pencil_limit_exceeded = False
        hair_curve_count = 0
        hair_curve_point_count = 0
        hair_curve_limit_exceeded = False
        point_cloud_point_count = 0
        point_cloud_limit_exceeded = False
        mesh_payload_budget = {
            "estimated_bytes": 0,
            "limit_exceeded": False,
            "loose_vertex_point_count": 0,
            "loose_vertex_limit_exceeded": False,
        }
        viewport_extra_count = 0
        viewport_extra_limit_reported = False
        for obj in scene.objects:
            object_count += 1
            object_id = stable_id(obj)
            scene_record["object_ids"].append(object_id)
            if obj.type == "CAMERA":
                if viewport_extra_count < MAX_CAMERA_VIEWPORT_EXTRAS_PER_SCENE:
                    try:
                        extra = camera_viewport_extra_record(
                            obj, scene, camera_depsgraph)
                    except Exception as error:
                        extra = None
                        add_unsupported(
                            output["unsupported_features"], object_id,
                            "camera_viewport_extra",
                            "camera guide could not be evaluated: " + str(error))
                    if extra is not None:
                        scene_record["viewport_extras"].append(extra)
                        viewport_extra_count += 1
                elif not viewport_extra_limit_reported:
                    viewport_extra_limit_reported = True
                    add_unsupported(
                        output["unsupported_features"], stable_id(scene),
                        "camera_viewport_extra_limit",
                        "camera guides beyond the per-scene limit of %d were omitted" %
                        MAX_CAMERA_VIEWPORT_EXTRAS_PER_SCENE)
            if obj.type == "EMPTY" and obj.instance_collection is not None:
                add_unsupported(
                    output["unsupported_features"], object_id,
                    "collection_instance_hierarchy",
                    "collection instances are expanded into supported mesh, curve, and surface copies; their hierarchy, instance selection, and per-instance Random variation are not represented",
                )
                unsupported_item = unsupported_object_record(
                    obj, scene, depsgraph)
                unsupported_item["reason"] = (
                    "collection instance is shown in the Outliner only; copied geometry is imported separately")
                output["unsupported_items"].append(unsupported_item)
                continue
            if obj.type == "EMPTY":
                item, image_error = extract_image_empty(
                    obj, scene, packed_empty_image_payload_budget,
                    depsgraph=depsgraph)
                if item is not None:
                    item["view_layer_hidden"] = {
                        view_layer_key(scene, view_layer): object_hidden_in_layer(obj, view_layer)
                        for view_layer in scene.view_layers
                    }
                    item["default_view_layer_key"] = (
                        view_layer_key(scene, scene.view_layers[0])
                        if len(scene.view_layers) else ""
                    )
                    output["items"].append(item)
                else:
                    unsupported_item = unsupported_object_record(obj, scene, depsgraph)
                    if image_error:
                        unsupported_item["reason"] = image_error
                    output["unsupported_items"].append(unsupported_item)
                continue
            if obj.type == "GREASEPENCIL":
                try:
                    grease_depsgraph = depsgraph or depsgraph_for_scene(scene)
                    item = extract_grease_pencil_curve_item(
                        obj, scene, output["unsupported_features"],
                        evaluated_obj=obj.evaluated_get(grease_depsgraph),
                        depsgraph=grease_depsgraph,
                        max_strokes=(MAX_GREASE_PENCIL_STROKES_PER_SCENE -
                                     grease_pencil_stroke_count),
                        max_points=(MAX_GREASE_PENCIL_POINTS_PER_SCENE -
                                    grease_pencil_point_count))
                    metrics = item.pop("_transport_metrics")
                    if not item["splines"]:
                        unsupported_item = unsupported_object_record(
                            obj, scene, depsgraph)
                        unsupported_item["reason"] = (
                            "Grease Pencil has no visible multi-point stroke in the current frame")
                        output["unsupported_items"].append(unsupported_item)
                        continue
                    item["view_layer_hidden"] = {
                        view_layer_key(scene, view_layer): object_hidden_in_layer(obj, view_layer)
                        for view_layer in scene.view_layers
                    }
                    item["default_view_layer_key"] = (
                        view_layer_key(scene, scene.view_layers[0])
                        if len(scene.view_layers) else ""
                    )
                    output["items"].append(item)
                    grease_pencil_stroke_count += metrics["stroke_count"]
                    grease_pencil_point_count += metrics["point_count"]
                except Exception as error:
                    if "limit exceeded" in str(error):
                        grease_pencil_limit_exceeded = True
                    output["unsupported_items"].append({
                        **object_metadata(obj, scene, depsgraph),
                        "object_type": obj.type,
                        "reason": "Grease Pencil extraction failed; whole object skipped: " + str(error),
                    })
                continue
            if obj.type == "CURVES":
                try:
                    hair_depsgraph = depsgraph or depsgraph_for_scene(scene)
                    item = extract_hair_curve_item(
                        obj, scene, output["unsupported_features"],
                        transform_override=obj.evaluated_get(
                            hair_depsgraph).matrix_world,
                        depsgraph=hair_depsgraph,
                        max_curves=(MAX_HAIR_CURVES_PER_SCENE -
                                    hair_curve_count),
                        max_points=(MAX_HAIR_CURVE_POINTS_PER_SCENE -
                                    hair_curve_point_count))
                    metrics = item.pop("_transport_metrics")
                    hair_curve_count += metrics["curve_count"]
                    hair_curve_point_count += metrics["point_count"]
                    if not item["splines"]:
                        unsupported_item = unsupported_object_record(
                            obj, scene, depsgraph)
                        unsupported_item["reason"] = (
                            "Hair Curves has no multi-point strand in its source geometry")
                        output["unsupported_items"].append(unsupported_item)
                        continue
                    item["view_layer_hidden"] = {
                        view_layer_key(scene, view_layer): object_hidden_in_layer(obj, view_layer)
                        for view_layer in scene.view_layers
                    }
                    item["default_view_layer_key"] = (
                        view_layer_key(scene, scene.view_layers[0])
                        if len(scene.view_layers) else ""
                    )
                    output["items"].append(item)
                except Exception as error:
                    if "limit exceeded" in str(error):
                        hair_curve_limit_exceeded = True
                    output["unsupported_items"].append({
                        **object_metadata(obj, scene, depsgraph),
                        "object_type": obj.type,
                        "reason": "Hair Curves extraction failed; whole object skipped: " + str(error),
                    })
                continue
            if obj.type == "POINTCLOUD":
                try:
                    point_cloud_depsgraph = depsgraph or depsgraph_for_scene(scene)
                    item = extract_point_cloud_item(
                        obj, scene, point_cloud_depsgraph,
                        evaluated_object_override=obj.evaluated_get(
                            point_cloud_depsgraph),
                        max_points=(MAX_POINT_CLOUD_POINTS_PER_SCENE -
                                    point_cloud_point_count))
                    metrics = item.pop("_transport_metrics")
                    point_cloud_point_count += metrics["point_count"]
                    item["view_layer_hidden"] = {
                        view_layer_key(scene, view_layer):
                        object_hidden_in_layer(obj, view_layer)
                        for view_layer in scene.view_layers
                    }
                    item["default_view_layer_key"] = (
                        view_layer_key(scene, scene.view_layers[0])
                        if len(scene.view_layers) else ""
                    )
                    output["items"].append(item)
                except Exception as error:
                    if "limit exceeded" in str(error):
                        point_cloud_limit_exceeded = True
                    output["unsupported_items"].append({
                        **object_metadata(obj, scene, depsgraph),
                        "object_type": obj.type,
                        "reason": "Point Cloud extraction failed; whole object skipped: " + str(error),
                    })
                continue
            if obj.type == "CURVE":
                curve_features = []
                item = extract_poly_curve_object(
                    obj, scene, curve_features, depsgraph)
                if item["splines"]:
                    item["view_layer_hidden"] = {
                        view_layer_key(scene, view_layer): object_hidden_in_layer(obj, view_layer)
                        for view_layer in scene.view_layers
                    }
                    item["default_view_layer_key"] = (
                        view_layer_key(scene, scene.view_layers[0])
                        if len(scene.view_layers) else ""
                    )
                    output["items"].append(item)
                    surface_item = extract_curve_surface_mesh(
                        obj, scene, item["object_id"], output["materials"],
                        output["unsupported_features"], depsgraph,
                        mesh_payload_budget=mesh_payload_budget)
                    if surface_item is not None:
                        surface_item["view_layer_hidden"] = item[
                            "view_layer_hidden"]
                        surface_item["default_view_layer_key"] = item[
                            "default_view_layer_key"]
                        output["items"].append(surface_item)
                        output["unsupported_features"].extend(
                            source_curve_features_after_surface_bake(
                                curve_features))
                    else:
                        output["unsupported_features"].extend(curve_features)
                else:
                    output["unsupported_features"].extend(curve_features)
                    output["unsupported_items"].append(
                        unsupported_object_record(obj, scene, depsgraph))
                continue
            if obj.type == "SURFACE":
                try:
                    patches = extract_surface_patches(
                        obj, scene, output["materials"],
                        output["unsupported_features"], depsgraph)
                    for item in patches:
                        item["view_layer_hidden"] = {
                            view_layer_key(scene, view_layer): object_hidden_in_layer(obj, view_layer)
                            for view_layer in scene.view_layers
                        }
                        item["default_view_layer_key"] = (
                            view_layer_key(scene, scene.view_layers[0])
                            if len(scene.view_layers) else ""
                        )
                        output["items"].append(item)
                    surface_patch_count += len(patches)
                    if not patches:
                        output["unsupported_items"].append(
                            unsupported_object_record(obj, scene, depsgraph))
                except Exception as error:
                    output["unsupported_items"].append({
                        **object_metadata(obj, scene, depsgraph),
                        "object_type": obj.type,
                        "reason": "surface extraction failed; whole object skipped: " + str(error),
                    })
                continue
            if obj.type == "FONT":
                try:
                    # Text has no original mesh representation to preserve.
                    # Import its currently visible glyphs in either mesh mode
                    # so choosing original meshes does not make text vanish.
                    evaluated_depsgraph = depsgraph or depsgraph_for_scene(scene)
                    item = extract_mesh_object(
                        obj, scene, "evaluated", evaluated_depsgraph, output["materials"],
                        output["unsupported_features"],
                        mesh_payload_budget=mesh_payload_budget)
                    item["object_type"] = "MESH"
                    item["source_object_type"] = "FONT"
                    add_unsupported(
                        output["unsupported_features"], item["object_id"],
                        "font_to_mesh",
                        "evaluated glyph geometry was imported as a mesh; editable text, font settings, and text layout were not retained")
                    item["view_layer_hidden"] = {
                        view_layer_key(scene, view_layer): object_hidden_in_layer(obj, view_layer)
                        for view_layer in scene.view_layers
                    }
                    item["default_view_layer_key"] = (
                        view_layer_key(scene, scene.view_layers[0])
                        if len(scene.view_layers) else ""
                    )
                    output["items"].append(item)
                    mesh_count += 1
                    font_mesh_count += 1
                except Exception as error:
                    output["unsupported_items"].append({
                        **object_metadata(obj, scene, depsgraph),
                        "object_type": obj.type,
                        "reason": "font mesh extraction failed; whole object skipped: " + str(error),
                    })
                continue
            if obj.type == "META":
                try:
                    # Metaballs have no original polygon mesh to preserve.
                    # Always import their current evaluated surface so an
                    # original-mesh choice does not make visible geometry vanish.
                    evaluated_depsgraph = depsgraph or depsgraph_for_scene(scene)
                    item = extract_mesh_object(
                        obj, scene, "evaluated", evaluated_depsgraph, output["materials"],
                        output["unsupported_features"],
                        mesh_payload_budget=mesh_payload_budget)
                    item["object_type"] = "MESH"
                    item["source_object_type"] = "META"
                    add_unsupported(
                        output["unsupported_features"], item["object_id"],
                        "metaball_to_mesh",
                        "evaluated metaball geometry was imported as a baked mesh; editable metaball data and threshold settings were not retained")
                    item["view_layer_hidden"] = {
                        view_layer_key(scene, view_layer): object_hidden_in_layer(obj, view_layer)
                        for view_layer in scene.view_layers
                    }
                    item["default_view_layer_key"] = (
                        view_layer_key(scene, scene.view_layers[0])
                        if len(scene.view_layers) else ""
                    )
                    output["items"].append(item)
                    mesh_count += 1
                except Exception as error:
                    output["unsupported_items"].append({
                        **object_metadata(obj, scene, depsgraph),
                        "object_type": obj.type,
                        "reason": "metaball mesh extraction failed; whole object skipped: " + str(error),
                    })
                continue
            if obj.type == "VOLUME":
                try:
                    volume_depsgraph = depsgraph or depsgraph_for_scene(scene)
                    item = object_metadata(obj, scene, volume_depsgraph)
                    item["object_type"] = "VOLUME"
                    item["import_mode"] = mode
                    volume_data = extract_scalar_volume_grid(
                        obj, scene, volume_depsgraph,
                        transform_override=obj.evaluated_get(
                            volume_depsgraph).matrix_world)
                    volume_data.pop("_transport_metrics", None)
                    item.update(volume_data)
                    item["view_layer_hidden"] = {
                        view_layer_key(scene, view_layer): object_hidden_in_layer(obj, view_layer)
                        for view_layer in scene.view_layers
                    }
                    item["default_view_layer_key"] = (
                        view_layer_key(scene, scene.view_layers[0])
                        if len(scene.view_layers) else ""
                    )
                    output["items"].append(item)
                    volume_grid_count += 1
                except Exception as error:
                    output["unsupported_items"].append({
                        **object_metadata(obj, scene, depsgraph),
                        "object_type": obj.type,
                        "reason": "volume extraction failed; whole object skipped: " + str(error),
                    })
                continue
            if obj.type != "MESH":
                output["unsupported_items"].append(
                    unsupported_object_record(obj, scene, depsgraph))
                continue
            try:
                item = extract_mesh_object(obj, scene, mode, depsgraph,
                                           output["materials"],
                                           output["unsupported_features"],
                                           mesh_payload_budget=mesh_payload_budget)
                item["view_layer_hidden"] = {
                    view_layer_key(scene, view_layer): object_hidden_in_layer(obj, view_layer)
                    for view_layer in scene.view_layers
                }
                item["default_view_layer_key"] = (
                    view_layer_key(scene, scene.view_layers[0])
                    if len(scene.view_layers) else ""
                )
                loose_edges_world = item.pop("_loose_edges_world", [])
                loose_edge_source_indices = item.pop(
                    "_loose_edge_source_indices", [])
                loose_vertices_world = item.pop("_loose_vertices_world", [])
                omitted_vertices = item.pop("_loose_vertices_omitted", 0)
                loose_edges_item = (
                    loose_edge_curve_item(obj, scene, item, loose_edges_world,
                                          loose_edge_source_indices)
                    if loose_edges_world and not item["bounds_display_type"] else None
                )
                output["items"].append(item)
                mesh_count += 1
                if loose_edges_item is not None:
                    output["items"].append(loose_edges_item)
                output["items"].extend(
                    loose_vertex_point_items(item, loose_vertices_world))
                if omitted_vertices:
                    add_unsupported(
                        output["unsupported_features"], item["object_id"],
                        "loose_vertex_point_limit",
                        "%d isolated mesh vertices were omitted because the per-object limit is %d or the per-scene limit is %d" %
                        (omitted_vertices, MAX_LOOSE_VERTICES_PER_OBJECT,
                         MAX_LOOSE_VERTEX_POINTS_PER_SCENE))
            except Exception as error:
                output["unsupported_items"].append({
                    **object_metadata(obj, scene, depsgraph),
                    "object_type": obj.type,
                    "reason": "mesh extraction failed; whole object skipped: " + str(error),
                })
        instance_depsgraph = depsgraph or depsgraph_for_scene(scene)
        if instance_depsgraph is not None:
            emitted_instance_keys = set()
            unsupported_instance_types = set()
            instance_records = []
            instance_limit_exceeded = False
            image_instance_count = 0
            image_instance_pixels = 0
            image_instance_limit_exceeded = False
            volume_instance_count = 0
            volume_instance_limit_exceeded = False
            volume_instance_payload_bytes = 0
            volume_instance_active_records = 0
            volume_instance_dense_voxels = 0

            # Surface wire extraction updates Blender's dependency graph while
            # evaluating a temporary object, so snapshot instance identities
            # and matrices before extracting any instance geometry.
            for instance in instance_depsgraph.object_instances:
                if not instance.is_instance:
                    continue
                evaluated_obj = instance.object
                source_obj = getattr(evaluated_obj, "original", evaluated_obj)
                parent = instance.parent
                parent_source = getattr(parent, "original", parent)
                if parent_source is None:
                    continue
                if (parent_source.type != "EMPTY" or
                        parent_source.instance_collection is None):
                    unsupported_instance_types.add(
                        "non-collection-generated-" + source_obj.type)
                    continue
                is_image_empty = (
                    source_obj.type == "EMPTY" and
                    str(getattr(evaluated_obj, "empty_display_type", "")) == "IMAGE")
                if source_obj.type not in {
                        "MESH", "CURVE", "SURFACE", "FONT", "META",
                        "VOLUME", "GREASEPENCIL", "CURVES", "POINTCLOUD"} and not is_image_empty:
                    unsupported_instance_types.add(source_obj.type)
                    continue
                if is_image_empty:
                    image = getattr(source_obj, "data", None)
                    image_size = getattr(image, "size", (0, 0))
                    width, height = (int(image_size[0]), int(image_size[1]))
                    image_pixels = max(0, width) * max(0, height)
                    if (image_instance_count >=
                            MAX_COLLECTION_IMAGE_EMPTY_INSTANCES or
                            image_instance_pixels + image_pixels >
                            MAX_COLLECTION_IMAGE_EMPTY_PIXELS):
                        image_instance_limit_exceeded = True
                        continue
                    image_instance_count += 1
                    image_instance_pixels += image_pixels
                if source_obj.type == "VOLUME":
                    if volume_instance_count >= MAX_COLLECTION_VOLUME_INSTANCES:
                        volume_instance_limit_exceeded = True
                        continue
                    volume_instance_count += 1
                if len(instance_records) >= MAX_COLLECTION_INSTANCES:
                    instance_limit_exceeded = True
                    break
                instance_records.append((
                    source_obj, parent_source, instance.matrix_world.copy(),
                    tuple(int(value) for value in instance.persistent_id),
                    is_image_empty))

            def assign_instance_metadata(item, instance_id, source_obj,
                                         parent_source, instance_transform):
                item["object_id"] = instance_id
                item["parent_id"] = stable_id(parent_source)
                if item.get("bounds_display_type"):
                    item["bounds_transform_world"] = matrix_values(
                        instance_transform)
                item["collection_ids"] = sorted(
                    stable_id(collection)
                    for collection in parent_source.users_collection)
                item["view_layer_hidden"] = {
                    view_layer_key(scene, view_layer): (
                        object_hidden_in_layer(source_obj, view_layer) or
                        object_hidden_in_layer(parent_source, view_layer))
                    for view_layer in scene.view_layers
                }
                item["default_view_layer_key"] = (
                    view_layer_key(scene, scene.view_layers[0])
                    if len(scene.view_layers) else ""
                )

            for (source_obj, parent_source, instance_transform, persistent_id,
                 is_image_empty) in instance_records:
                source_id = stable_id(source_obj)
                parent_id = stable_id(parent_source)
                instance_key = (source_id, parent_id, persistent_id)
                if instance_key in emitted_instance_keys:
                    continue
                emitted_instance_keys.add(instance_key)
                instance_suffix = ":".join(str(value) for value in persistent_id)
                instance_id = "classiCAD::instance::" + hashlib.sha256(
                    (stable_id(scene) + "::" + source_id + "::" + parent_id +
                     "::" + instance_suffix)
                    .encode("utf-8")).hexdigest()
                try:
                    if is_image_empty:
                        item, image_error = extract_image_empty(
                            source_obj, scene, packed_empty_image_payload_budget,
                            transform_override=instance_transform,
                            depsgraph=instance_depsgraph)
                        if item is None:
                            unsupported_item = unsupported_object_record(
                                source_obj, scene, instance_depsgraph)
                            unsupported_item["object_id"] = instance_id
                            if image_error:
                                unsupported_item["reason"] = image_error
                            output["unsupported_items"].append(unsupported_item)
                        else:
                            assign_instance_metadata(
                                item, instance_id, source_obj, parent_source,
                                instance_transform)
                            output["items"].append(item)
                    elif source_obj.type == "VOLUME":
                        if (volume_instance_payload_bytes >=
                                MAX_COLLECTION_VOLUME_PAYLOAD_BYTES or
                                volume_instance_active_records >=
                                MAX_COLLECTION_VOLUME_RECORDS or
                                volume_instance_dense_voxels >=
                                MAX_COLLECTION_VOLUME_DENSE_VOXELS):
                            volume_instance_limit_exceeded = True
                            unsupported_item = unsupported_object_record(
                                source_obj, scene, instance_depsgraph)
                            unsupported_item["object_id"] = instance_id
                            unsupported_item["reason"] = (
                                "collection Volume instance aggregate extraction limit was reached")
                            output["unsupported_items"].append(unsupported_item)
                            continue
                        item = object_metadata(
                            source_obj, scene, instance_depsgraph)
                        item["object_type"] = "VOLUME"
                        item["import_mode"] = mode
                        volume_data = extract_scalar_volume_grid(
                            source_obj, scene, instance_depsgraph,
                            transform_override=instance_transform)
                        metrics = volume_data.pop("_transport_metrics")
                        if (
                                volume_instance_payload_bytes +
                                metrics["payload_bytes"] >
                                MAX_COLLECTION_VOLUME_PAYLOAD_BYTES or
                                volume_instance_active_records +
                                metrics["active_record_count"] >
                                MAX_COLLECTION_VOLUME_RECORDS or
                                volume_instance_dense_voxels +
                                metrics["dense_voxel_count"] >
                                MAX_COLLECTION_VOLUME_DENSE_VOXELS):
                            volume_instance_limit_exceeded = True
                            unsupported_item = unsupported_object_record(
                                source_obj, scene, instance_depsgraph)
                            unsupported_item["object_id"] = instance_id
                            unsupported_item["reason"] = (
                                "collection Volume instance exceeded the remaining per-scene payload, record, or dense-voxel budget")
                            output["unsupported_items"].append(unsupported_item)
                            continue
                        item.update(volume_data)
                        assign_instance_metadata(
                            item, instance_id, source_obj, parent_source,
                            instance_transform)
                        output["items"].append(item)
                        volume_instance_payload_bytes += metrics["payload_bytes"]
                        volume_instance_active_records += metrics[
                            "active_record_count"]
                        volume_instance_dense_voxels += metrics[
                            "dense_voxel_count"]
                        volume_grid_count += 1
                    elif source_obj.type in {"FONT", "META"}:
                        item_features = []
                        evaluated_object = source_obj.evaluated_get(
                            instance_depsgraph)
                        item = extract_mesh_object(
                            source_obj, scene, "evaluated", instance_depsgraph,
                            output["materials"], item_features,
                            transform_override=instance_transform,
                            evaluated_object_override=evaluated_object,
                            mesh_payload_budget=mesh_payload_budget)
                        item["object_type"] = "MESH"
                        item["source_object_type"] = source_obj.type
                        assign_instance_metadata(
                            item, instance_id, source_obj, parent_source,
                            instance_transform)
                        feature_name = (
                            "font_to_mesh" if source_obj.type == "FONT"
                            else "metaball_to_mesh")
                        reason = (
                            "evaluated glyph geometry was imported as a mesh; editable text, font settings, and text layout were not retained"
                            if source_obj.type == "FONT" else
                            "evaluated metaball geometry was imported as a baked mesh; editable metaball data and threshold settings were not retained")
                        add_unsupported(
                            item_features, instance_id, feature_name, reason)
                        output["unsupported_features"].extend(item_features)
                        output["items"].append(item)
                        if source_obj.type == "FONT":
                            font_mesh_count += 1
                        mesh_count += 1
                    elif source_obj.type == "GREASEPENCIL":
                        item_features = []
                        item = extract_grease_pencil_curve_item(
                            source_obj, scene, item_features,
                            transform_override=instance_transform,
                            evaluated_obj=source_obj.evaluated_get(
                                instance_depsgraph),
                            depsgraph=instance_depsgraph,
                            max_strokes=(MAX_GREASE_PENCIL_STROKES_PER_SCENE -
                                         grease_pencil_stroke_count),
                            max_points=(MAX_GREASE_PENCIL_POINTS_PER_SCENE -
                                        grease_pencil_point_count))
                        metrics = item.pop("_transport_metrics")
                        if not item["splines"]:
                            unsupported_item = unsupported_object_record(
                                source_obj, scene, instance_depsgraph)
                            unsupported_item["object_id"] = instance_id
                            unsupported_item["reason"] = (
                                "Grease Pencil instance has no visible multi-point stroke in the current frame")
                            output["unsupported_items"].append(unsupported_item)
                            continue
                        assign_instance_metadata(
                            item, instance_id, source_obj, parent_source,
                            instance_transform)
                        output["items"].append(item)
                        output["unsupported_features"].extend(item_features)
                        grease_pencil_stroke_count += metrics["stroke_count"]
                        grease_pencil_point_count += metrics["point_count"]
                    elif source_obj.type == "CURVES":
                        item_features = []
                        item = extract_hair_curve_item(
                            source_obj, scene, item_features,
                            transform_override=instance_transform,
                            depsgraph=instance_depsgraph,
                            max_curves=(MAX_HAIR_CURVES_PER_SCENE -
                                        hair_curve_count),
                            max_points=(MAX_HAIR_CURVE_POINTS_PER_SCENE -
                                        hair_curve_point_count))
                        metrics = item.pop("_transport_metrics")
                        hair_curve_count += metrics["curve_count"]
                        hair_curve_point_count += metrics["point_count"]
                        if not item["splines"]:
                            unsupported_item = unsupported_object_record(
                                source_obj, scene, instance_depsgraph)
                            unsupported_item["object_id"] = instance_id
                            unsupported_item["reason"] = (
                                "Hair Curves instance has no multi-point strand in its source geometry")
                            output["unsupported_items"].append(unsupported_item)
                            continue
                        assign_instance_metadata(
                            item, instance_id, source_obj, parent_source,
                            instance_transform)
                        output["items"].append(item)
                        output["unsupported_features"].extend(item_features)
                    elif source_obj.type == "POINTCLOUD":
                        item = extract_point_cloud_item(
                            source_obj, scene, instance_depsgraph,
                            transform_override=instance_transform,
                            evaluated_object_override=source_obj.evaluated_get(
                                instance_depsgraph),
                            max_points=(MAX_POINT_CLOUD_POINTS_PER_SCENE -
                                        point_cloud_point_count))
                        metrics = item.pop("_transport_metrics")
                        if point_cloud_point_count + metrics["point_count"] > \
                                MAX_POINT_CLOUD_POINTS_PER_SCENE:
                            point_cloud_limit_exceeded = True
                            unsupported_item = unsupported_object_record(
                                source_obj, scene, instance_depsgraph)
                            unsupported_item["object_id"] = instance_id
                            unsupported_item["reason"] = (
                                "collection Point Cloud instance aggregate point limit was reached")
                            output["unsupported_items"].append(unsupported_item)
                            continue
                        assign_instance_metadata(
                            item, instance_id, source_obj, parent_source,
                            instance_transform)
                        output["items"].append(item)
                        point_cloud_point_count += metrics["point_count"]
                    elif source_obj.type == "MESH":
                        item = extract_mesh_object(
                            source_obj, scene, mode, instance_depsgraph,
                            output["materials"], output["unsupported_features"],
                            transform_override=instance_transform,
                            evaluated_object_override=(
                                source_obj.evaluated_get(instance_depsgraph)
                                if mode == "evaluated" else None),
                            mesh_payload_budget=mesh_payload_budget,
                        )
                        assign_instance_metadata(
                            item, instance_id, source_obj, parent_source,
                            instance_transform)
                        loose_edges_world = item.pop("_loose_edges_world", [])
                        loose_edge_source_indices = item.pop(
                            "_loose_edge_source_indices", [])
                        loose_vertices_world = item.pop(
                            "_loose_vertices_world", [])
                        omitted_vertices = item.pop(
                            "_loose_vertices_omitted", 0)
                        loose_edges_item = (
                            loose_edge_curve_item(
                                source_obj, scene, item, loose_edges_world,
                                loose_edge_source_indices)
                            if loose_edges_world and
                            not item["bounds_display_type"] else None
                        )
                        output["items"].append(item)
                        if loose_edges_item is not None:
                            output["items"].append(loose_edges_item)
                        output["items"].extend(
                            loose_vertex_point_items(item, loose_vertices_world))
                        if omitted_vertices:
                            add_unsupported(
                                output["unsupported_features"], instance_id,
                                "loose_vertex_point_limit",
                                "%d isolated mesh vertices were omitted because the per-object limit is %d or the per-scene limit is %d" %
                                (omitted_vertices,
                                 MAX_LOOSE_VERTICES_PER_OBJECT,
                                 MAX_LOOSE_VERTEX_POINTS_PER_SCENE))
                        mesh_count += 1
                    elif source_obj.type == "CURVE":
                        curve_features = []
                        item = extract_poly_curve_object(
                            source_obj, scene, curve_features,
                            instance_depsgraph,
                            transform_override=instance_transform)
                        assign_instance_metadata(
                            item, instance_id, source_obj, parent_source,
                            instance_transform)
                        output["items"].append(item)
                        surface_item = extract_curve_surface_mesh(
                            source_obj, scene, instance_id, output["materials"],
                            output["unsupported_features"], instance_depsgraph,
                            transform_override=instance_transform,
                            mesh_payload_budget=mesh_payload_budget)
                        if surface_item is not None:
                            assign_instance_metadata(
                                surface_item, instance_id, source_obj,
                                parent_source, instance_transform)
                            surface_item["object_id"] = curve_surface_item_id(
                                instance_id)
                            surface_item["source_object_id"] = instance_id
                            output["items"].append(surface_item)
                            output["unsupported_features"].extend(
                                source_curve_features_after_surface_bake(
                                    curve_features))
                        else:
                            output["unsupported_features"].extend(curve_features)
                    else:
                        patches = extract_surface_patches(
                            source_obj, scene, output["materials"],
                            output["unsupported_features"], instance_depsgraph,
                            transform_override=instance_transform)
                        for patch_index, item in enumerate(patches):
                            patch_id = "%s::surface:%d" % (
                                instance_id, patch_index)
                            assign_instance_metadata(
                                item, patch_id, source_obj, parent_source,
                                instance_transform)
                            item["source_object_id"] = instance_id
                            output["items"].append(item)
                            scene_record["object_ids"].append(patch_id)
                        surface_patch_count += len(patches)
                    if source_obj.type != "SURFACE":
                        scene_record["object_ids"].append(instance_id)
                    object_count += 1
                except Exception as error:
                    if (source_obj.type == "GREASEPENCIL" and
                            "limit exceeded" in str(error)):
                        grease_pencil_limit_exceeded = True
                    if (source_obj.type == "CURVES" and
                            "limit exceeded" in str(error)):
                        hair_curve_limit_exceeded = True
                    if (source_obj.type == "POINTCLOUD" and
                            "limit exceeded" in str(error)):
                        point_cloud_limit_exceeded = True
                    output["unsupported_items"].append({
                        **object_metadata(source_obj, scene, depsgraph),
                        "object_id": instance_id,
                        "object_type": source_obj.type,
                        "reason": "collection geometry instance extraction failed; instance skipped: " + str(error),
                    })
            if instance_limit_exceeded:
                add_unsupported(
                    output["unsupported_features"], stable_id(scene),
                    "collection_instance_limit",
                    "supported collection instances exceeded the extraction limit of %d; remaining instances were skipped" %
                    MAX_COLLECTION_INSTANCES,
                )
            if image_instance_limit_exceeded:
                add_unsupported(
                    output["unsupported_features"], stable_id(scene),
                    "collection_image_empty_instance_limit",
                    "static Image Empty collection instances exceeded the per-scene limit of %d instances or %d decoded pixels; remaining image instances were skipped" %
                    (MAX_COLLECTION_IMAGE_EMPTY_INSTANCES,
                     MAX_COLLECTION_IMAGE_EMPTY_PIXELS),
                )
            if volume_instance_limit_exceeded:
                add_unsupported(
                    output["unsupported_features"], stable_id(scene),
                    "collection_volume_instance_limit",
                    "collection Volume instances exceeded the per-scene limits of %d items, %d payload bytes, %d active records, or %d dense voxels; remaining instances were skipped" %
                    (MAX_COLLECTION_VOLUME_INSTANCES,
                     MAX_COLLECTION_VOLUME_PAYLOAD_BYTES,
                     MAX_COLLECTION_VOLUME_RECORDS,
                     MAX_COLLECTION_VOLUME_DENSE_VOXELS),
                )
            if unsupported_instance_types:
                add_unsupported(
                    output["unsupported_features"], stable_id(scene),
                    "non_mesh_collection_instances",
                    "collection or generated instances with unsupported geometry were skipped: " +
                    ", ".join(sorted(unsupported_instance_types)),
                )
        if grease_pencil_limit_exceeded:
            add_unsupported(
                output["unsupported_features"], stable_id(scene),
                "grease_pencil_scene_limit",
                "Grease Pencil extraction exceeded the per-scene limit of %d strokes or %d points; remaining objects were skipped" %
                (MAX_GREASE_PENCIL_STROKES_PER_SCENE,
                 MAX_GREASE_PENCIL_POINTS_PER_SCENE),
            )
        if hair_curve_limit_exceeded:
            add_unsupported(
                output["unsupported_features"], stable_id(scene),
                "hair_curve_scene_limit",
                "Hair Curves extraction exceeded the per-scene limit of %d strands or %d points; remaining objects were skipped" %
                (MAX_HAIR_CURVES_PER_SCENE,
                 MAX_HAIR_CURVE_POINTS_PER_SCENE),
            )
        if point_cloud_limit_exceeded:
            add_unsupported(
                output["unsupported_features"], stable_id(scene),
                "point_cloud_scene_limit",
                "Point Cloud extraction exceeded the per-scene limit of %d points; remaining objects and instances were skipped" %
                MAX_POINT_CLOUD_POINTS_PER_SCENE,
            )
        if mesh_payload_budget["limit_exceeded"]:
            add_unsupported(
                output["unsupported_features"], stable_id(scene),
                "mesh_payload_scene_limit",
                "mesh extraction exceeded the per-object limit of %d estimated bytes or the per-scene limit of %d estimated bytes; remaining oversized meshes and instances were skipped" %
                (MAX_MESH_OBJECT_ESTIMATED_BYTES,
                 MAX_MESH_SCENE_ESTIMATED_BYTES),
            )
        if mesh_payload_budget["loose_vertex_limit_exceeded"]:
            add_unsupported(
                output["unsupported_features"], stable_id(scene),
                "loose_vertex_scene_limit",
                "isolated mesh vertices exceeded the per-object limit of %d Point items or the per-scene limit of %d; over-limit point sets were omitted" %
                (MAX_LOOSE_VERTICES_PER_OBJECT,
                 MAX_LOOSE_VERTEX_POINTS_PER_SCENE),
            )
        for camera_background in extract_camera_background_images(
                scene, packed_empty_image_payload_budget,
                output["unsupported_features"]):
            output["items"].append(camera_background)
            scene_record["object_ids"].append(camera_background["object_id"])
        output["scenes"].append(scene_record)
    output["summary"] = {
        "scene_count": len(output["scenes"]),
        "source_object_occurrences": object_count,
        "mesh_items_extracted": mesh_count,
        "surface_patches_extracted": surface_patch_count,
        "font_objects_meshed": font_mesh_count,
        "volume_grids_extracted": volume_grid_count,
        "unsupported_item_occurrences": len(output["unsupported_items"]),
        "unsupported_feature_count": len(output["unsupported_features"]),
        "material_count": len(output["materials"]),
    }
    return output


def write_json_atomic(path, document):
    output_path = os.path.abspath(path)
    output_dir = os.path.dirname(output_path)
    os.makedirs(output_dir, exist_ok=True)
    fd, temporary_path = tempfile.mkstemp(prefix=".classicad-scene-",
                                         suffix=".json", dir=output_dir)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            json.dump(document, stream, ensure_ascii=False, allow_nan=False,
                      separators=(",", ":"))
        if os.path.getsize(temporary_path) > MAX_EXTRACTION_JSON_BYTES:
            raise ValueError(
                "Blender scene extraction exceeded the %d byte JSON transport limit" %
                MAX_EXTRACTION_JSON_BYTES)
        os.replace(temporary_path, output_path)
    except Exception:
        try:
            os.unlink(temporary_path)
        except OSError:
            pass
        raise


def main():
    require_pinned_blender()
    if "--" not in sys.argv:
        raise RuntimeError("missing arguments after Blender's -- separator")
    arguments = sys.argv[sys.argv.index("--") + 1:]
    if len(arguments) == 5 and arguments[0] in {
            "data_api_section", "data_api_node"}:
        input_path, output_path, focus_argument, expected_sha256 = arguments[1:]
        if (not focus_argument or len(focus_argument) > 64 * 1024 or
                len(expected_sha256) != 64 or
                any(character not in "0123456789abcdefABCDEF"
                    for character in expected_sha256)):
            raise RuntimeError(
                "invalid Blender Data API focus path or source hash")
        if arguments[0] == "data_api_section":
            if len(focus_argument) > 512:
                raise RuntimeError("invalid Blender Data API section identifier")
            focus_node_path = [{"kind": "PROPERTY", "key": focus_argument}]
        else:
            focus_node_path = json.loads(focus_argument)

        input_path = os.path.abspath(input_path)

        def source_sha256():
            digest = hashlib.sha256()
            with open(input_path, "rb") as source_file:
                for chunk in iter(lambda: source_file.read(1024 * 1024), b""):
                    digest.update(chunk)
            return digest.hexdigest()

        expected_sha256 = expected_sha256.lower()
        if source_sha256() != expected_sha256:
            raise RuntimeError(
                "The Blender file changed after it was imported; Data API refresh was cancelled")
        bpy.ops.wm.open_mainfile(filepath=input_path)
        require_pinned_blender()
        section = outliner_data_api_tree(focus_node_path=focus_node_path)
        if len(section["root"]["children"]) != 1:
            raise RuntimeError(
                "Blender Data API node is unavailable")
        if source_sha256() != expected_sha256:
            raise RuntimeError(
                "The Blender file changed while its Data API section was loading")
        write_json_atomic(output_path, section)
        return
    if len(arguments) not in {3, 4} or arguments[0] != "extract":
        raise RuntimeError(
            "usage: blender --background --python blender_scene_extract.py -- "
            "extract INPUT.blend OUTPUT.json [original|evaluated]"
        )
    mode = arguments[3].lower() if len(arguments) == 4 else "original"
    document = extract_scene(arguments[1], mode)
    write_json_atomic(arguments[2], document)


if __name__ == "__main__":
    try:
        main()
    except Exception:
        traceback.print_exc()
        sys.exit(2)
