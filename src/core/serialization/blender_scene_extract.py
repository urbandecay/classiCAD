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
import struct
import sys
import tempfile
import traceback
import zlib

import bpy


PINNED_BLENDER_VERSION = (5, 2, 2)
BRIDGE_FORMAT = "classiCAD.blender-scene-extract"
BRIDGE_FORMAT_VERSION = 43
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


def data_block_animation_metadata(data_block):
    animation_data = getattr(data_block, "animation_data", None)
    action = getattr(animation_data, "action", None)
    action_slot = getattr(animation_data, "action_slot", None)
    drivers = getattr(animation_data, "drivers", ())
    nla_track_collection = getattr(animation_data, "nla_tracks", ())
    driver_target_names = []
    if animation_data is not None:
        last_added_target = None
        for fcurve in drivers:
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
    for track in list(nla_track_collection)[:1024]:
        nla_tracks.append({
            "name": track.name,
            "action_names": [
                strip.action.name for strip in track.strips
                if strip.action is not None
            ][:1024],
        })
    return {
        "has_animation_data": bool(
            animation_data is not None and
            (action is not None or drivers or nla_track_collection)
        ),
        "action_name": action.name if action is not None else "",
        "action_slot_name": (
            getattr(action_slot, "name_display", "")
            if action_slot is not None else ""
        ),
        "has_drivers": bool(drivers),
        "driver_target_names": driver_target_names[:1024],
        "nla_tracks": nla_tracks,
    }


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
    record = {
        "object_id": stable_id(obj),
        # Workbench Random hashes ID.name, which is obj.name in Blender's
        # Python API. name_full adds a linked-library suffix and would change
        # the Workbench color before the separate library path is mixed in.
        "name": obj.name,
        "data_name": getattr(getattr(obj, "data", None), "name_full", ""),
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
                        ("show_cursor", "viewport_3d_cursor",
                         "Blender's 3D cursor overlay is not represented by classiCAD"),
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
    def visit(node, path):
        collection_id = stable_id(node.collection)
        current_path = path + [collection_id]
        records.append({
            "collection_id": collection_id,
            "path": current_path,
            "exclude": bool(node.exclude),
            "hide_viewport": bool(node.hide_viewport),
            "hide_render": bool(node.hide_render),
            "holdout": bool(node.holdout),
            "indirect_only": bool(node.indirect_only),
        })
        for child in node.children:
            visit(child, current_path)
    visit(view_layer.layer_collection, [])
    return records


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
            pending.extend(collection.children)
        for view_layer in scene.view_layers:
            key = view_layer_key(scene, view_layer)
            for state in layer_collection_records(scene, view_layer):
                states_by_collection.setdefault(state["collection_id"], []).append({
                    "view_layer_key": key,
                    "scene_name": scene.name_full,
                    "view_layer_name": view_layer.name,
                    "path": state["path"],
                    "exclude": state["exclude"],
                    "hide_viewport": state["hide_viewport"],
                    "hide_render": state["hide_render"],
                })
    parent_ids_by_collection = {}
    for parent in collections_by_id.values():
        parent_id = stable_id(parent)
        for child in parent.children:
            parent_ids_by_collection.setdefault(stable_id(child), set()).add(
                parent_id
            )
    for collection in collections_by_id.values():
        collection_id = stable_id(collection)
        output["collections"].append({
            "collection_id": collection_id,
            "name": collection.name_full,
            "library_path": source_library_path(collection),
            "parent_ids": sorted(parent_ids_by_collection.get(collection_id, set())),
            "source_scene_names": sorted(scene_names_by_collection.get(collection_id, set())),
            "hide_viewport": bool(collection.hide_viewport),
            "hide_render": bool(collection.hide_render),
            "hide_select": bool(collection.hide_select),
            "view_layer_states": states_by_collection.get(collection_id, []),
        })
    for scene in bpy.data.scenes:
        # Original mode preserves source mesh data, but Blender still displays
        # objects with their current dependency-graph world transforms.
        depsgraph = depsgraph_for_scene(scene)
        camera_depsgraph = depsgraph
        render = scene.render
        scene_record = {
            "scene_id": stable_id(scene),
            "is_active_scene": scene == active_scene,
            "name": scene.name_full,
            "library_path": source_library_path(scene),
            "view_layer_names": [view_layer.name for view_layer in scene.view_layers],
            "view_layers": [{
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
                "use_pass_combined": bool(view_layer.use_pass_combined),
                "use_pass_z": bool(view_layer.use_pass_z),
                "collection_instances": layer_collection_records(scene, view_layer),
            } for view_layer in scene.view_layers],
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
