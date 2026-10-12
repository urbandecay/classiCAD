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
import sys
import tempfile
import traceback

import bpy


PINNED_BLENDER_VERSION = (5, 2, 2)
BRIDGE_FORMAT = "classiCAD.blender-scene-extract"
BRIDGE_FORMAT_VERSION = 10
# Keep a single packed image from expanding the extractor's JSON transport
# without bound. The limit applies to the decoded image bytes.
MAX_PACKED_IMAGE_BYTES = 32 * 1024 * 1024
SOURCE_PROJECT_NAMESPACE = ""


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
    # Workbench's Texture color source uses the active Image Texture Node.
    # Do not guess from node-tree iteration order or evaluate a node graph.
    active_node = getattr(tree.nodes, "active", None)
    for node in ([active_node] if active_node is not None else []):
        if getattr(node, "type", "") != "TEX_IMAGE":
            continue
        image = getattr(node, "image", None)
        if image is None:
            continue
        packed_data_base64 = ""
        packed_data_error = ""
        packed_file = getattr(image, "packed_file", None)
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
                "image_name": image.name_full,
                "image_library_path": source_library_path(image),
                # Resolve Blender's // path against the source .blend while
                # still in Blender; classiCAD never interprets Blender paths.
                "filepath": (bpy.path.abspath(image.filepath)
                             if image.filepath else ""),
                "packed": packed_file is not None,
                "packed_data_base64": packed_data_base64,
                "packed_data_error": packed_data_error,
                "source_file_exists": (
                    True if image.packed_file is not None else
                    (os.path.isfile(bpy.path.abspath(image.filepath))
                     if image.filepath else False)
                ),
                "color_space": image.colorspace_settings.name,
                "alpha_mode": image.alpha_mode,
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
    }


def object_metadata(obj, scene):
    parent = obj.parent
    return {
        "object_id": stable_id(obj),
        "name": obj.name_full,
        "library_path": source_library_path(obj),
        "library_path_exists": source_library_exists(obj),
        "scene_name": scene.name_full,
        "parent_id": stable_id(parent) if parent is not None else "",
        "collection_ids": sorted(stable_id(collection) for collection in obj.users_collection),
        "visibility": visibility_metadata(obj),
        "display_color_linear": vector_values(obj.color),
        "classiCAD_geometry_type": obj.get("classiCAD_geometry_type", ""),
        "classiCAD_proxy_geometry": obj.get("classiCAD_proxy_geometry", ""),
        "classiCAD_data_block": obj.get("classiCAD_data_block", ""),
    }


def scene_camera_record(scene, depsgraph=None):
    camera = scene.camera
    if camera is None:
        return None
    evaluated = camera.evaluated_get(depsgraph) if depsgraph is not None else camera
    data = evaluated.data
    return {
        "object_id": stable_id(camera),
        "name": camera.name_full,
        "object_type": "CAMERA",
        "matrix_world": matrix_values(evaluated.matrix_world),
        "camera_type": data.type,
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
        "dof_focus_distance": finite_number(data.dof.focus_distance),
        "dof_f_stop": finite_number(data.dof.aperture_fstop),
    }


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


def extract_mesh_object(obj, scene, mode, depsgraph, materials, unsupported):
    item_id = stable_id(obj)
    item = object_metadata(obj, scene)
    item["object_type"] = obj.type
    item["import_mode"] = mode
    if item["library_path_exists"] is False:
        add_unsupported(unsupported, item_id, "linked_library",
                        "linked Blender library file is missing: " + item["library_path"])
    if mode == "evaluated":
        evaluated_obj = obj.evaluated_get(depsgraph)
        mesh = evaluated_obj.to_mesh(preserve_all_data_layers=True,
                                     depsgraph=depsgraph)
        transform = evaluated_obj.matrix_world
    else:
        evaluated_obj = None
        mesh = obj.data
        transform = obj.matrix_world
    if mesh is None:
        raise ValueError("Blender did not provide mesh data")
    try:
        item["transform_world"] = matrix_values(transform)
        item["mesh_name"] = mesh.name_full
        item["mesh_library_path"] = source_library_path(mesh)
        item["vertices_local"] = [vector_values(vertex.co) for vertex in mesh.vertices]
        item["polygons"] = [
            [int(vertex_index) for vertex_index in polygon.vertices]
            for polygon in mesh.polygons
        ]
        item["polygon_material_indices"] = [
            int(polygon.material_index) for polygon in mesh.polygons
        ]
        item["loop_vertex_indices"] = [int(loop.vertex_index) for loop in mesh.loops]

        source_obj = evaluated_obj if evaluated_obj is not None else obj
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
                    if image["packed_data_error"]:
                        add_unsupported(
                            unsupported, item_id,
                            "image_texture:" + image["image_name"],
                            image["packed_data_error"],
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
        item["color_attributes"] = extract_attributes(mesh, item_id, unsupported)
        active_color = getattr(mesh, "color_attributes", None)
        item["active_color_attribute"] = (
            active_color.active_color.name
            if active_color is not None and active_color.active_color else ""
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


def unsupported_object_record(obj, scene):
    record = object_metadata(obj, scene)
    record["object_type"] = obj.type
    if obj.type == "EMPTY" and obj.instance_collection is not None:
        record["reason"] = "collection instance is metadata only; no geometry was fabricated"
        record["instance_collection"] = obj.instance_collection.name_full
    elif obj.type == "CURVE":
        record["reason"] = "curve has no supported POLY, BEZIER, or NURBS spline"
    elif obj.type == "SURFACE":
        record["reason"] = "surface has no supported NURBS patch"
    elif obj.type == "FONT":
        record["reason"] = "Blender FONT objects are not imported"
    else:
        record["reason"] = "no matching classiCAD scene-object geometry type"
    return record


def extract_poly_curve_object(obj, scene, unsupported):
    """Extract supported source splines in world XYZ."""
    from mathutils import Vector

    item_id = stable_id(obj)
    item = object_metadata(obj, scene)
    item["object_type"] = "CURVE"
    item["splines"] = []
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
                world = obj.matrix_world @ Vector((point.co[0], point.co[1], point.co[2]))
                points.append(vector_values(world))
            if spline.use_cyclic_u and points and points[-1] != points[0]:
                points.append(list(points[0]))
            if len(points) < 2:
                add_unsupported(unsupported, item_id,
                                "spline:%d:POLY" % spline_index,
                                "POLY spline has fewer than two control points")
                continue
            item["splines"].append({"type": "POLY", "control_points_world": points})
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
                world = obj.matrix_world @ Vector((point.co[0], point.co[1], point.co[2]))
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
                world = obj.matrix_world @ Vector((point[0], point[1], point[2]))
                span.append(vector_values(world))
            spans.append(span)
        if not spans:
            add_unsupported(unsupported, item_id,
                            "spline:%d:BEZIER" % spline_index,
                            "open BEZIER spline has fewer than two control points")
            continue
        item["splines"].append({"type": "BEZIER", "spans_world": spans})
    for modifier in obj.modifiers:
        add_unsupported(unsupported, item_id, "modifier:" + modifier.name,
                        "exact spline import preserves source control points; modifier was not applied")
    return item


def extract_surface_patches(obj, scene, materials, unsupported):
    """Extract each Blender SURFACE NURBS patch as a separate item."""
    from mathutils import Vector

    source_id = stable_id(obj)
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
                world = obj.matrix_world @ Vector(
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

        item = object_metadata(obj, scene)
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
    return patches


def depsgraph_for_scene(scene):
    if not scene.view_layers:
        raise RuntimeError("scene has no view layer for evaluated mesh extraction")
    view_layer = scene.view_layers[0]
    with bpy.context.temp_override(scene=scene, view_layer=view_layer):
        return bpy.context.evaluated_depsgraph_get()


def extract_scene(input_path, mode):
    global SOURCE_PROJECT_NAMESPACE
    require_pinned_blender()
    if mode not in {"original", "evaluated"}:
        raise ValueError("mode must be 'original' or 'evaluated'")
    bpy.ops.wm.open_mainfile(filepath=os.path.abspath(input_path))
    require_pinned_blender()
    source_filepath = bpy.data.filepath
    SOURCE_PROJECT_NAMESPACE = source_project_namespace(source_filepath)

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
        "scenes": [],
        "collections": [],
        "items": [],
        "unsupported_items": [],
        "unsupported_features": [],
        "materials": {},
        "summary": {},
    }
    object_count = 0
    mesh_count = 0
    surface_patch_count = 0
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
    for collection in collections_by_id.values():
        parent_ids = sorted(
            stable_id(parent) for parent in collections_by_id.values()
            if collection in parent.children
        )
        output["collections"].append({
            "collection_id": stable_id(collection),
            "name": collection.name_full,
            "library_path": source_library_path(collection),
            "parent_ids": parent_ids,
            "source_scene_names": sorted(scene_names_by_collection.get(stable_id(collection), set())),
            "hide_viewport": bool(collection.hide_viewport),
            "hide_render": bool(collection.hide_render),
            "view_layer_states": states_by_collection.get(stable_id(collection), []),
        })
    for scene in bpy.data.scenes:
        depsgraph = depsgraph_for_scene(scene) if mode == "evaluated" else None
        render = scene.render
        scene_record = {
            "scene_id": stable_id(scene),
            "name": scene.name_full,
            "library_path": source_library_path(scene),
            "view_layer_names": [view_layer.name for view_layer in scene.view_layers],
            "view_layers": [{
                "name": view_layer.name,
                "key": view_layer_key(scene, view_layer),
                "collection_instances": layer_collection_records(scene, view_layer),
            } for view_layer in scene.view_layers],
            "collection_ids": sorted(
                collection_id for collection_id, scene_names in scene_names_by_collection.items()
                if scene.name_full in scene_names
            ),
            "object_ids": [],
            "active_camera": scene_camera_record(scene, depsgraph),
            "color_management": {
                "display_device": scene.display_settings.display_device,
                "view_transform": scene.view_settings.view_transform,
                "look": scene.view_settings.look,
                "exposure": finite_number(scene.view_settings.exposure),
                "gamma": finite_number(scene.view_settings.gamma),
            },
            "render_output": {
                "resolution_x": int(render.resolution_x),
                "resolution_y": int(render.resolution_y),
                "resolution_percentage": int(render.resolution_percentage),
                "pixel_aspect_x": finite_number(render.pixel_aspect_x),
                "pixel_aspect_y": finite_number(render.pixel_aspect_y),
                "file_format": render.image_settings.file_format,
                "color_mode": render.image_settings.color_mode,
                "color_depth": render.image_settings.color_depth,
                "quality": render.image_settings.quality,
                "compression": render.image_settings.compression,
                "tiff_codec": render.image_settings.tiff_codec,
            },
        }
        for obj in scene.objects:
            object_count += 1
            object_id = stable_id(obj)
            scene_record["object_ids"].append(object_id)
            if obj.type == "CURVE":
                item = extract_poly_curve_object(
                    obj, scene, output["unsupported_features"])
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
                else:
                    output["unsupported_items"].append(unsupported_object_record(obj, scene))
                continue
            if obj.type == "SURFACE":
                try:
                    patches = extract_surface_patches(
                        obj, scene, output["materials"],
                        output["unsupported_features"])
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
                            unsupported_object_record(obj, scene))
                except Exception as error:
                    output["unsupported_items"].append({
                        **object_metadata(obj, scene),
                        "object_type": obj.type,
                        "reason": "surface extraction failed; whole object skipped: " + str(error),
                    })
                continue
            if obj.type != "MESH":
                output["unsupported_items"].append(unsupported_object_record(obj, scene))
                continue
            try:
                item = extract_mesh_object(obj, scene, mode, depsgraph,
                                           output["materials"],
                                           output["unsupported_features"])
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
                    **object_metadata(obj, scene),
                    "object_type": obj.type,
                    "reason": "mesh extraction failed; whole object skipped: " + str(error),
                })
        output["scenes"].append(scene_record)
    output["summary"] = {
        "scene_count": len(output["scenes"]),
        "source_object_occurrences": object_count,
        "mesh_items_extracted": mesh_count,
        "surface_patches_extracted": surface_patch_count,
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
