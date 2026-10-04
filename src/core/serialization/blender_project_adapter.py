"""Blender 5.2.2 adapter for classiCAD's native .vignola and .blend projects."""

import json
import math
import os
import sys
import traceback

import bpy
from mathutils import Matrix, Quaternion, Vector


PINNED_BLENDER_VERSION = (5, 2, 2)
PROJECT_FORMAT = "classiCAD.vignola"
PROJECT_FORMAT_VERSION = 1
DOCUMENT_TEXT_NAME = "classiCAD.DocumentData"
ROOT_COLLECTION_NAME = "classiCAD"


def require_pinned_blender():
    if tuple(bpy.app.version[:3]) != PINNED_BLENDER_VERSION:
        raise RuntimeError(
            "classiCAD project files require Blender 5.2.2; found "
            + bpy.app.version_string
        )


def read_arguments():
    if "--" not in sys.argv:
        raise RuntimeError("missing classiCAD adapter arguments")
    arguments = sys.argv[sys.argv.index("--") + 1 :]
    if not arguments:
        raise RuntimeError("missing classiCAD adapter operation")
    return arguments


def frame_matrix(shape):
    frame = shape.get("workPlaneFrame")
    if frame:
        origin = frame["origin"]
        x_axis = frame["xAxis"]
        y_axis = frame["yAxis"]
        normal = frame["normal"]
    else:
        work_plane = int(shape.get("workPlane", 0))
        offset = float(shape.get("workPlaneOffset", 0.0))
        if work_plane == 1:
            origin = (0.0, offset, 0.0)
            x_axis = (1.0, 0.0, 0.0)
            y_axis = (0.0, 0.0, 1.0)
            normal = (0.0, -1.0, 0.0)
        elif work_plane == 2:
            origin = (offset, 0.0, 0.0)
            x_axis = (0.0, 1.0, 0.0)
            y_axis = (0.0, 0.0, 1.0)
            normal = (1.0, 0.0, 0.0)
        else:
            origin = (0.0, 0.0, offset)
            x_axis = (1.0, 0.0, 0.0)
            y_axis = (0.0, 1.0, 0.0)
            normal = (0.0, 0.0, 1.0)

    return Matrix(
        (
            (x_axis[0], y_axis[0], normal[0], origin[0]),
            (x_axis[1], y_axis[1], normal[1], origin[1]),
            (x_axis[2], y_axis[2], normal[2], origin[2]),
            (0.0, 0.0, 0.0, 1.0),
        )
    )


def prepare_nurbs(nurbs):
    control_points = nurbs.get("controlPoints", [])
    order = int(nurbs.get("order", 0))
    knots = [float(value) for value in nurbs.get("knots", [])]
    rational = bool(nurbs.get("rational", False))
    if len(control_points) < 2 or order < 2 or order > min(64, len(control_points)):
        return None
    if len(knots) != len(control_points) + order - 2:
        return None
    if not all(math.isfinite(value) for value in knots):
        return None
    if any(left > right for left, right in zip(knots, knots[1:])):
        return None

    try:
        points = [
            (float(point["x"]), float(point["y"])) for point in control_points
        ]
    except (KeyError, TypeError, ValueError):
        return None
    if not all(math.isfinite(value) for point in points for value in point):
        return None

    if rational:
        raw_weights = nurbs.get("weights", [])
        if len(raw_weights) != len(control_points):
            return None
        try:
            weights = [float(value) for value in raw_weights]
        except (TypeError, ValueError):
            return None
        if not all(math.isfinite(value) and value > 0.0 for value in weights):
            return None
    else:
        weights = [1.0] * len(control_points)

    reduced_knots = [knots[0]] + knots + [knots[-1]]
    degree = order - 1
    span_indices = [
        index
        for index in range(degree, len(points))
        if reduced_knots[index] < reduced_knots[index + 1]
    ]
    if not span_indices:
        return None

    x_values = [point[0] for point in points]
    y_values = [point[1] for point in points]
    scale = max(max(x_values) - min(x_values), max(y_values) - min(y_values), 1.0)
    return {
        "degree": degree,
        "full_knots": reduced_knots,
        "points": points,
        "spans": span_indices,
        "tolerance": scale * 1.0e-5,
        "weights": weights,
    }


def insert_knot_once(control_points, knots, degree, knot):
    point_count = len(control_points)
    last_control = point_count - 1
    span = next(
        index
        for index in range(degree, point_count)
        if knots[index] <= knot < knots[index + 1]
    )
    multiplicity = sum(1 for value in knots if value == knot)

    refined_points = [None] * (point_count + 1)
    for index in range(span - degree + 1):
        refined_points[index] = control_points[index]
    for index in range(span - multiplicity, last_control + 1):
        refined_points[index + 1] = control_points[index]
    for index in range(span - degree + 1, span - multiplicity + 1):
        denominator = knots[index + degree] - knots[index]
        alpha = 0.0 if denominator == 0.0 else (knot - knots[index]) / denominator
        refined_points[index] = [
            alpha * control_points[index][axis]
            + (1.0 - alpha) * control_points[index - 1][axis]
            for axis in range(4)
        ]

    refined_knots = knots[: span + 1] + [knot] + knots[span + 1 :]
    return refined_points, refined_knots


def nurbs_bezier_spans(nurbs, local_transform=None):
    # Blender exposes spline order and endpoint settings, but not an arbitrary
    # knot array. Knot insertion turns each supported span into an exact
    # rational Bezier NURBS that Blender can store as a clamped spline.
    prepared = prepare_nurbs(nurbs)
    if prepared is None:
        return None

    degree = prepared["degree"]
    control_points = []
    for (x, y), weight in zip(prepared["points"], prepared["weights"]):
        homogeneous = Vector((x * weight, y * weight, 0.0, weight))
        if local_transform is not None:
            homogeneous = local_transform @ homogeneous
        control_points.append(list(homogeneous))
    knots = list(prepared["full_knots"])
    last_control = len(control_points) - 1
    parameter_start = knots[degree]
    parameter_end = knots[last_control + 1]
    if parameter_start >= parameter_end:
        return None

    if not all(value == parameter_start for value in knots[: degree + 1]):
        return None
    if not all(value == parameter_end for value in knots[-(degree + 1) :]):
        return None

    internal_knots = sorted(set(knots[degree + 1 : last_control + 1]))
    for knot in internal_knots:
        multiplicity = sum(1 for value in knots if value == knot)
        if multiplicity > degree + 1:
            return None
        while multiplicity < degree:
            control_points, knots = insert_knot_once(
                control_points, knots, degree, knot
            )
            multiplicity += 1

    spans = []
    for span in range(degree, len(control_points)):
        if knots[span] >= knots[span + 1]:
            continue
        spans.append(control_points[span - degree : span + 1])
    return spans or None


def add_nurbs_bezier_spline(curve_data, control_points, order):
    spline = curve_data.splines.new("NURBS")
    spline.points.add(len(control_points) - 1)
    for index, (weighted_x, weighted_y, weighted_z, weight) in enumerate(control_points):
        spline.points[index].co = (
            weighted_x / weight,
            weighted_y / weight,
            weighted_z / weight,
            weight,
        )
    spline.order_u = order
    spline.use_endpoint_u = True
    return True


def evaluate_nurbs(prepared, span, parameter):
    degree = prepared["degree"]
    knots = prepared["full_knots"]
    control_points = prepared["points"]
    weights = prepared["weights"]
    first_control = span - degree
    work = []
    for index in range(first_control, span + 1):
        x, y = control_points[index]
        weight = weights[index]
        work.append([x * weight, y * weight, weight])

    for level in range(1, degree + 1):
        for point_index in range(degree, level - 1, -1):
            knot_index = span - degree + point_index
            lower_knot = knots[knot_index]
            upper_knot = knots[span + 1 + point_index - level]
            denominator = upper_knot - lower_knot
            alpha = (
                0.0
                if denominator == 0.0
                else (parameter - lower_knot) / denominator
            )
            alpha = min(max(alpha, 0.0), 1.0)
            work[point_index] = [
                (1.0 - alpha) * work[point_index - 1][axis]
                + alpha * work[point_index][axis]
                for axis in range(3)
            ]

    x, y, weight = work[degree]
    if abs(weight) <= 1.0e-15:
        raise ValueError("NURBS evaluation produced a zero homogeneous weight")
    return (x / weight, y / weight)


def distance(left, right):
    return math.hypot(left[0] - right[0], left[1] - right[1])


def adaptive_append(prepared, span, parameter0, point0, parameter1, point1, path, depth=0):
    deviations = []
    for fraction in (0.25, 0.5, 0.75):
        parameter = parameter0 + (parameter1 - parameter0) * fraction
        actual = evaluate_nurbs(prepared, span, parameter)
        chord = (
            point0[0] + (point1[0] - point0[0]) * fraction,
            point0[1] + (point1[1] - point0[1]) * fraction,
        )
        deviations.append(distance(actual, chord))

    if max(deviations) <= prepared["tolerance"] or depth >= 10:
        path.append(point1)
        return

    midpoint_parameter = (parameter0 + parameter1) * 0.5
    midpoint = evaluate_nurbs(prepared, span, midpoint_parameter)
    adaptive_append(
        prepared,
        span,
        parameter0,
        point0,
        midpoint_parameter,
        midpoint,
        path,
        depth + 1,
    )
    adaptive_append(
        prepared,
        span,
        midpoint_parameter,
        midpoint,
        parameter1,
        point1,
        path,
        depth + 1,
    )


def sample_nurbs(nurbs):
    prepared = prepare_nurbs(nurbs)
    if prepared is None:
        return []

    paths = []
    current_path = []
    knots = prepared["full_knots"]
    for span in prepared["spans"]:
        parameter0 = knots[span]
        parameter1 = knots[span + 1]
        point0 = evaluate_nurbs(prepared, span, parameter0)
        point1 = evaluate_nurbs(prepared, span, parameter1)

        if current_path and distance(current_path[-1], point0) > prepared["tolerance"]:
            if len(current_path) >= 2:
                paths.append(current_path)
            current_path = []
        if not current_path:
            current_path.append(point0)

        segment_count = 4
        for segment in range(segment_count):
            segment_start = parameter0 + (parameter1 - parameter0) * (
                segment / segment_count
            )
            segment_end = parameter0 + (parameter1 - parameter0) * (
                (segment + 1) / segment_count
            )
            segment_point0 = evaluate_nurbs(prepared, span, segment_start)
            segment_point1 = evaluate_nurbs(prepared, span, segment_end)
            adaptive_append(
                prepared,
                span,
                segment_start,
                segment_point0,
                segment_end,
                segment_point1,
                current_path,
            )

    if len(current_path) >= 2:
        paths.append(current_path)
    return paths


def add_polyline_spline(curve_data, points):
    if len(points) < 2:
        return False
    spline = curve_data.splines.new("POLY")
    spline.points.add(len(points) - 1)
    for index, point in enumerate(points):
        z = float(point[2]) if len(point) > 2 else 0.0
        spline.points[index].co = (float(point[0]), float(point[1]), z, 1.0)
    return True


def add_nurbs_curve(curve_data, nurbs, local_transform=None):
    bezier_spans = nurbs_bezier_spans(nurbs, local_transform)
    if bezier_spans is not None:
        order = int(nurbs["order"])
        added = False
        for span in bezier_spans:
            added = add_nurbs_bezier_spline(curve_data, span, order) or added
        return added

    added = False
    for points in sample_nurbs(nurbs):
        if local_transform is not None:
            points = [
                tuple(local_transform @ Vector((point[0], point[1], 0.0)))
                for point in points
            ]
        added = add_polyline_spline(curve_data, points) or added
    return added


def add_poly_spline(curve_data, points, cyclic=False):
    if len(points) < 2:
        return False
    spline = curve_data.splines.new("POLY")
    spline.points.add(len(points) - 1)
    for index, point in enumerate(points):
        spline.points[index].co = (float(point["x"]), float(point["y"]), 0.0, 1.0)
    spline.use_cyclic_u = bool(cyclic)
    return True


def add_scene_object(layer_collections, record):
    object_id = str(record["id"])
    layer_id = str(record["layerId"])
    shape = record["geometry"]
    geometry_type = int(shape.get("geometryType", shape.get("tool", 0)))
    layer_collection = layer_collections.get(layer_id)
    if layer_collection is None:
        raise RuntimeError("scene object references an unknown layer " + layer_id)

    object_name = "classiCAD.%s.%s" % (geometry_type, object_id)
    frame = frame_matrix(shape)
    curve_data = bpy.data.curves.new(object_name, "CURVE")
    curve_data.dimensions = "3D"
    curve_data.resolution_u = 16

    components = shape.get("components", [])
    component_frames = shape.get("componentWorkPlaneFrames", [])
    has_curve = False
    if components:
        for index, component in enumerate(components):
            local_transform = None
            if len(component_frames) == len(components):
                component_frame = frame_matrix(
                    {"workPlaneFrame": component_frames[index]}
                )
                local_transform = frame.inverted() @ component_frame
            has_curve = add_nurbs_curve(
                curve_data, component, local_transform
            ) or has_curve
    else:
        has_curve = add_nurbs_curve(curve_data, shape.get("nurbs", {}))

    if not has_curve and geometry_type in {5, 10, 13}:
        has_curve = add_poly_spline(
            curve_data,
            shape.get("points", []),
            cyclic=geometry_type in {5, 10, 13},
        )

    if has_curve:
        obj = bpy.data.objects.new(object_name, curve_data)
        obj.matrix_world = frame
    else:
        bpy.data.curves.remove(curve_data)
        obj = bpy.data.objects.new(object_name, None)
        obj.empty_display_type = "PLAIN_AXES"
        obj.empty_display_size = 25.0
        points = shape.get("points", [])
        if geometry_type == 7 and points:
            point = points[0]
            obj.matrix_world = frame @ Matrix.Translation(
                Vector((float(point["x"]), float(point["y"]), 0.0))
            )
        else:
            obj.matrix_world = frame

    obj["classiCAD_object_id"] = object_id
    obj["classiCAD_layer_id"] = layer_id
    obj["classiCAD_geometry_type"] = geometry_type
    obj["classiCAD_data_block"] = DOCUMENT_TEXT_NAME
    layer_collection.objects.link(obj)


def create_project(document, destination_path):
    # Use the user's saved Blender startup UI (workspaces and screen layout),
    # but create an empty scene for the classiCAD project.
    bpy.ops.wm.read_homefile(use_empty=True, use_factory_startup=False)
    scene = bpy.context.scene
    scene.name = "Vignola"
    scene.unit_settings.system = "METRIC"
    scene.unit_settings.length_unit = "MILLIMETERS"
    scene.unit_settings.scale_length = 0.001
    camera_settings = document.get("viewportCamera", {})
    view_state = camera_settings.get("viewState", {})
    for screen in bpy.data.screens:
        for area in screen.areas:
            if area.type != "VIEW_3D":
                continue
            view = area.spaces.active
            view.lens = float(camera_settings.get("focalLengthMillimeters", 50.0))
            view.clip_start = float(camera_settings.get("clipStart", 0.01))
            view.clip_end = float(camera_settings.get("clipEnd", 1000.0))
            if view_state:
                region_3d = view.region_3d
                zoom = max(float(view_state["zoom"]), 1.0e-12)
                region_3d.view_location = Vector(
                    (
                        float(view_state["targetX"]),
                        float(view_state["targetY"]),
                        float(view_state["targetZ"]),
                    )
                )
                region_3d.view_rotation = Quaternion(
                    (
                        float(view_state["orientationW"]),
                        float(view_state["orientationX"]),
                        float(view_state["orientationY"]),
                        float(view_state["orientationZ"]),
                    )
                )
                region_3d.view_distance = 60.0 / zoom
                region_3d.view_perspective = (
                    "PERSP" if bool(view_state["perspective"]) else "ORTHO"
                )

    scene["classiCAD_format"] = PROJECT_FORMAT
    scene["classiCAD_format_version"] = PROJECT_FORMAT_VERSION
    scene["classiCAD_blender_version"] = bpy.app.version_string
    scene["classiCAD_coordinate_unit"] = "millimeter"

    document_text = bpy.data.texts.new(DOCUMENT_TEXT_NAME)
    document_text.write(json.dumps(document, ensure_ascii=False, separators=(",", ":")))
    document_text["classiCAD_role"] = "authoritative classiCAD document snapshot"

    root_collection = bpy.data.collections.new(ROOT_COLLECTION_NAME)
    root_collection["classiCAD_collection_role"] = "document root"
    scene.collection.children.link(root_collection)

    layer_collections = {}
    for layer in document.get("layers", []):
        layer_id = str(layer["id"])
        collection = bpy.data.collections.new(str(layer["name"]))
        collection["classiCAD_layer_id"] = layer_id
        collection["classiCAD_color"] = str(layer.get("color", ""))
        collection["classiCAD_visible"] = bool(layer.get("visible", True))
        collection["classiCAD_locked"] = bool(layer.get("locked", False))
        root_collection.children.link(collection)
        layer_collections[layer_id] = collection

    for record in document.get("objects", []):
        add_scene_object(layer_collections, record)

    result = bpy.ops.wm.save_as_mainfile(
        filepath=os.path.abspath(destination_path),
        check_existing=False,
        compress=True,
    )
    if "FINISHED" not in result:
        raise RuntimeError("Blender did not finish saving the project")


def load_project(source_path, output_json_path):
    result = bpy.ops.wm.open_mainfile(
        filepath=os.path.abspath(source_path),
        load_ui=False,
        use_scripts=False,
    )
    if "FINISHED" not in result:
        raise RuntimeError("Blender did not finish opening the project")

    scene = next(
        (item for item in bpy.data.scenes if item.get("classiCAD_format") == PROJECT_FORMAT),
        None,
    )
    if scene is None:
        raise RuntimeError("This Blender file does not contain a classiCAD project")
    if int(scene.get("classiCAD_format_version", -1)) != PROJECT_FORMAT_VERSION:
        raise RuntimeError("This classiCAD project format version is not supported")

    document_text = bpy.data.texts.get(DOCUMENT_TEXT_NAME)
    if document_text is None:
        raise RuntimeError("The classiCAD document data is missing from this Blender file")
    document = json.loads(document_text.as_string())
    with open(output_json_path, "w", encoding="utf-8") as output_file:
        json.dump(document, output_file, ensure_ascii=False, separators=(",", ":"))


def main():
    require_pinned_blender()
    arguments = read_arguments()
    operation = arguments[0]
    if operation == "save" and len(arguments) == 3:
        with open(arguments[1], "r", encoding="utf-8") as input_file:
            document = json.load(input_file)
        create_project(document, arguments[2])
        return
    if operation == "load" and len(arguments) == 3:
        load_project(arguments[1], arguments[2])
        return
    raise RuntimeError("invalid classiCAD adapter arguments")


if __name__ == "__main__":
    try:
        main()
    except Exception:
        traceback.print_exc()
        sys.exit(2)
