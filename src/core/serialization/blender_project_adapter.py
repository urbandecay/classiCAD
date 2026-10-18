"""Blender 5.2.2 adapter for classiCAD's native .vignola and .blend projects."""

import json
import atexit
from array import array
import base64
import hashlib
import math
import os
import shutil
import struct
import sys
import tempfile
import traceback
from types import SimpleNamespace

import bpy
from mathutils import Matrix, Quaternion, Vector


PINNED_BLENDER_VERSION = (5, 2, 2)
PROJECT_FORMAT = "classiCAD.vignola"
PROJECT_FORMAT_VERSION = 1
DOCUMENT_TEXT_NAME = "classiCAD.DocumentData"
ROOT_COLLECTION_NAME = "classiCAD"
MAX_PROXY_IMAGE_BYTES = 32 * 1024 * 1024
MAX_PROXY_IMAGE_PIXELS = MAX_PROXY_IMAGE_BYTES // 16
MAX_OUTLINER_ORPHAN_ITEMS = 20000
MAX_RELATIVE_KEYING_SET_OBJECTS = 2000
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
TEMPORARY_PROXY_IMAGE_PATHS = []


def _remove_temporary_image_file(path):
    try:
        os.remove(path)
    except OSError:
        pass


def _remove_temporary_image_directory(path):
    shutil.rmtree(path, ignore_errors=True)


def _create_classicad_float_payload_image(reference):
    """Persist sampler-ready scene-linear float texels as a packed EXR."""
    encoded = reference.get("sceneLinearFloatDataBase64", "")
    width = reference.get("sceneLinearFloatWidth", 0)
    height = reference.get("sceneLinearFloatHeight", 0)
    if (not isinstance(encoded, str) or not encoded or
            len(encoded) > ((MAX_PROXY_IMAGE_BYTES + 2) // 3) * 4 or
            isinstance(width, bool) or not isinstance(width, int) or width <= 0 or
            isinstance(height, bool) or not isinstance(height, int) or height <= 0 or
            width * height > MAX_PROXY_IMAGE_PIXELS):
        return None
    try:
        payload = base64.b64decode(encoded, validate=True)
    except (ValueError, TypeError):
        return None
    expected_bytes = width * height * 16
    if len(payload) != expected_bytes or len(payload) > MAX_PROXY_IMAGE_BYTES:
        return None
    pixels = array("f")
    pixels.frombytes(payload)
    if sys.byteorder != "little":
        pixels.byteswap()
    if not all(math.isfinite(value) for value in pixels):
        return None

    generated = None
    image = None
    temporary_path = ""
    try:
        generated = bpy.data.images.new(
            reference.get("imageName") or "classiCAD Float Texture",
            width=width, height=height, float_buffer=True, alpha=True,
        )
        # The transport has already applied Blender's source color conversion
        # and PREMUL RGB operation, so store its values without another one.
        generated.colorspace_settings.name = "Non-Color"
        generated.alpha_mode = "STRAIGHT"
        generated.pixels.foreach_set(pixels)
        generated.file_format = "OPEN_EXR"
        with tempfile.NamedTemporaryFile(suffix=".exr", delete=False) as handle:
            temporary_path = handle.name
        render_settings = bpy.context.scene.render.image_settings
        previous_color_mode = render_settings.color_mode
        try:
            render_settings.color_mode = "RGBA"
            generated.filepath_raw = temporary_path
            generated.save()
        finally:
            render_settings.color_mode = previous_color_mode
        file_size = os.path.getsize(temporary_path)
        if file_size <= 0 or file_size > MAX_PROXY_IMAGE_BYTES:
            raise ValueError("packed float EXR exceeds the 32 MiB image limit")
        with open(temporary_path, "rb") as handle:
            exr_data = handle.read(MAX_PROXY_IMAGE_BYTES + 1)
        if len(exr_data) != file_size:
            raise ValueError("packed float EXR could not be read completely")
        image = bpy.data.images.load(temporary_path, check_existing=False)
        image.colorspace_settings.name = "Non-Color"
        image.alpha_mode = reference.get("alphaMode", "STRAIGHT")
        image.pack(data=exr_data, data_len=len(exr_data))
        if image.packed_file is None or bytes(image.packed_file.data) != exr_data:
            raise ValueError("Blender did not retain the packed float EXR bytes")
        image["classiCAD_proxy_image"] = True
        image["classiCAD_proxy_float_payload"] = True
        image["classiCAD_source_color_space"] = reference.get("colorSpace", "")
        image["classiCAD_source_alpha_mode"] = reference.get("alphaMode", "STRAIGHT")
        image.filepath = ""
        image.filepath_raw = ""
        TEMPORARY_PROXY_IMAGE_PATHS.append(temporary_path)
        atexit.register(_remove_temporary_image_file, temporary_path)
        temporary_path = ""
    except (RuntimeError, OSError, ValueError, TypeError, OverflowError):
        if image is not None:
            bpy.data.images.remove(image)
        image = None
    finally:
        if generated is not None:
            bpy.data.images.remove(generated)
        if temporary_path:
            _remove_temporary_image_file(temporary_path)
    return image


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


def prepare_nurbs_surface(surface):
    try:
        count_u = int(surface.get("controlVertexCountU", 0))
        count_v = int(surface.get("controlVertexCountV", 0))
        degree_u = int(surface.get("degreeU", 0))
        degree_v = int(surface.get("degreeV", 0))
        order_u = int(surface.get("orderU", 0))
        order_v = int(surface.get("orderV", 0))
        points = [tuple(float(value) for value in point)
                  for point in surface.get("controlPoints", [])]
        rational = bool(surface.get("rational", False))
        knots_u = [float(value) for value in surface.get("knotsU", [])]
        knots_v = [float(value) for value in surface.get("knotsV", [])]
        raw_weights = surface.get("weights", [])
        weights = [float(value) for value in raw_weights] if rational else [1.0] * len(points)
    except (TypeError, ValueError):
        return None

    if (int(surface.get("dimension", 0)) != 3 or
            order_u != degree_u + 1 or order_v != degree_v + 1 or
            count_u <= degree_u or count_v <= degree_v or
            count_u * count_v != len(points) or len(weights) != len(points) or
            len(knots_u) != count_u + order_u - 2 or
            len(knots_v) != count_v + order_v - 2):
        return None
    if not all(len(point) == 3 and all(math.isfinite(value) for value in point)
               for point in points):
        return None
    if not all(math.isfinite(value) and value > 0.0 for value in weights):
        return None
    if any(not math.isfinite(value) for value in knots_u + knots_v):
        return None
    if (any(left > right for left, right in zip(knots_u, knots_u[1:])) or
            any(left > right for left, right in zip(knots_v, knots_v[1:]))):
        return None

    full_u = [knots_u[0]] + knots_u + [knots_u[-1]]
    full_v = [knots_v[0]] + knots_v + [knots_v[-1]]
    domain_u = (full_u[degree_u], full_u[count_u])
    domain_v = (full_v[degree_v], full_v[count_v])
    if domain_u[1] <= domain_u[0] or domain_v[1] <= domain_v[0]:
        return None
    return {
        "count_u": count_u,
        "count_v": count_v,
        "degree_u": degree_u,
        "degree_v": degree_v,
        "full_u": full_u,
        "full_v": full_v,
        "domain_u": domain_u,
        "domain_v": domain_v,
        "points": points,
        "weights": weights,
    }


def surface_basis(index, degree, parameter, knots):
    if degree == 0:
        return 1.0 if knots[index] <= parameter < knots[index + 1] else 0.0
    value = 0.0
    left_denominator = knots[index + degree] - knots[index]
    if left_denominator > 0.0:
        value += ((parameter - knots[index]) / left_denominator) * surface_basis(
            index, degree - 1, parameter, knots
        )
    right_denominator = knots[index + degree + 1] - knots[index + 1]
    if right_denominator > 0.0:
        value += ((knots[index + degree + 1] - parameter) / right_denominator) * surface_basis(
            index + 1, degree - 1, parameter, knots
        )
    return value


def evaluate_nurbs_surface(surface, u, v):
    if u >= surface["domain_u"][1]:
        u = math.nextafter(surface["domain_u"][1], surface["domain_u"][0])
    if v >= surface["domain_v"][1]:
        v = math.nextafter(surface["domain_v"][1], surface["domain_v"][0])
    numerator = [0.0, 0.0, 0.0]
    denominator = 0.0
    for u_index in range(surface["count_u"]):
        basis_u = surface_basis(u_index, surface["degree_u"], u, surface["full_u"])
        if basis_u == 0.0:
            continue
        for v_index in range(surface["count_v"]):
            control_index = u_index * surface["count_v"] + v_index
            basis = basis_u * surface_basis(
                v_index, surface["degree_v"], v, surface["full_v"]
            ) * surface["weights"][control_index]
            denominator += basis
            for axis in range(3):
                numerator[axis] += surface["points"][control_index][axis] * basis
    if abs(denominator) <= 1.0e-15:
        raise ValueError("NURBS surface evaluation produced a zero weight")
    return tuple(value / denominator for value in numerator)


def add_nurbs_surface_mesh(collection, object_name, surface_data, display_mesh):
    # The .blend mesh is a display proxy. The exact NURBS surface stays in the
    # authoritative classiCAD document text and is rebuilt from that data.
    if display_mesh is not None:
        vertices = [tuple(float(axis) for axis in point)
                    for point in display_mesh.get("vertices", [])]
        faces = [tuple(int(index) for index in face)
                 for face in display_mesh.get("faces", [])]
        corner_uvs = display_mesh.get("cornerUvs")
        if not vertices or not faces:
            raise ValueError("derived NURBS surface display mesh is empty")
    else:
        if surface_data.get("trimLoops"):
            raise ValueError("trimmed NURBS surface is missing its display mesh")
        surface = prepare_nurbs_surface(surface_data)
        if surface is None:
            return None
        sample_count = 32
        vertices = []
        for u_index in range(sample_count + 1):
            u = surface["domain_u"][0] + (surface["domain_u"][1] - surface["domain_u"][0]) * u_index / sample_count
            for v_index in range(sample_count + 1):
                v = surface["domain_v"][0] + (surface["domain_v"][1] - surface["domain_v"][0]) * v_index / sample_count
                vertices.append(evaluate_nurbs_surface(surface, u, v))
        faces = []
        corner_uvs = []
        row_size = sample_count + 1
        for u_index in range(sample_count):
            for v_index in range(sample_count):
                lower_left = u_index * row_size + v_index
                lower_right = (u_index + 1) * row_size + v_index
                faces.append((lower_left, lower_right, lower_right + 1, lower_left + 1))
                u0 = u_index / sample_count
                u1 = (u_index + 1) / sample_count
                v0 = v_index / sample_count
                v1 = (v_index + 1) / sample_count
                corner_uvs.append(((u0, v0), (u1, v0), (u1, v1), (u0, v1)))
    mesh = bpy.data.meshes.new(object_name)
    mesh.from_pydata(vertices, [], faces)
    mesh.update()
    if corner_uvs is not None:
        attach_proxy_corner_uvs(mesh, corner_uvs)
    obj = bpy.data.objects.new(object_name, mesh)
    collection.objects.link(obj)
    return obj


def attach_proxy_corner_uvs(mesh, corner_uvs):
    """Attach normalized NURBS parameter UVs to each proxy polygon corner."""
    if len(corner_uvs) != len(mesh.polygons):
        raise ValueError("proxy UV face count does not match proxy polygons")
    flattened = []
    for polygon, face_uvs in zip(mesh.polygons, corner_uvs):
        if len(face_uvs) != polygon.loop_total:
            raise ValueError("proxy UV corner count does not match polygon loops")
        for uv in face_uvs:
            if len(uv) != 2:
                raise ValueError("proxy UV coordinate must have two values")
            pair = (float(uv[0]), float(uv[1]))
            if not all(math.isfinite(value) for value in pair):
                raise ValueError("proxy UV coordinate is not finite")
            flattened.extend(pair)
    layer = mesh.uv_layers.new(name="classiCAD.ParametricUV")
    layer.data.foreach_set("uv", flattened)
    mesh.uv_layers.active = layer
    mesh.uv_layers.active_render_index = mesh.uv_layers.active_index


def classiCAD_materials(document):
    """Create/find the Blender datablocks used by stable document material IDs."""
    result = {}
    for record in document.get("materials", []):
        material_id = str(record.get("id", ""))
        if not material_id:
            continue
        material = next(
            (candidate for candidate in bpy.data.materials
             if candidate.get("classiCAD_material_id") == material_id),
            None,
        )
        if material is None:
            material = bpy.data.materials.new(
                "classiCAD.Material." + str(record.get("name", material_id))
            )
            material["classiCAD_material_id"] = material_id
        color = record.get("baseColorSceneLinear", [0.8, 0.8, 0.8, 1.0])
        material.diffuse_color = tuple(float(value) for value in color[:4])
        material.roughness = float(record.get("roughness", 0.5))
        material.metallic = float(record.get("metallic", 0.0))
        _sync_classicad_image_texture(material, record)
        result[material_id] = material
    return result


def _sync_classicad_image_texture(material, record):
    """Mirror one supported active Image Texture without evaluating its graph."""
    reference = next(iter(record.get("imageTextures", [])), None)
    managed_nodes = ([node for node in material.node_tree.nodes
                      if node.get("classiCAD_proxy_image_node")]
                     if material.use_nodes else [])
    if not isinstance(reference, dict):
        _remove_classicad_image_nodes(material, managed_nodes)
        return
    node_type = reference.get("nodeType", "TEX_IMAGE")
    if node_type not in {"TEX_IMAGE", "TEX_ENVIRONMENT"}:
        _remove_classicad_image_nodes(material, managed_nodes)
        return
    tiles = reference.get("tiles") or []
    reference_key = hashlib.sha256(json.dumps(
        reference, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")).hexdigest()
    if (len(managed_nodes) == 1 and
            managed_nodes[0].get("classiCAD_proxy_image_key") == reference_key):
        _activate_classicad_image_node(material, managed_nodes[0])
        return
    _remove_classicad_image_nodes(material, managed_nodes)

    image = None
    float_payload_image = False
    packed_data = reference.get("packedDataBase64", "")
    if tiles:
        image = _create_classicad_tiled_image(reference)
        float_payload_image = bool(
            image is not None and image.get("classiCAD_proxy_float_payload", False)
        )
    elif packed_data:
        try:
            image_bytes = base64.b64decode(packed_data, validate=True)
        except (ValueError, TypeError):
            image_bytes = b""
        if image_bytes and len(image_bytes) <= MAX_PROXY_IMAGE_BYTES:
            suffix = os.path.splitext(reference.get("filepath", ""))[1]
            if not suffix:
                if image_bytes.startswith(b"\x89PNG\r\n\x1a\n"):
                    suffix = ".png"
                elif image_bytes.startswith(b"\xff\xd8"):
                    suffix = ".jpg"
                elif image_bytes.startswith(b"\x76\x2f\x31\x01"):
                    suffix = ".exr"
            if suffix:
                temporary_path = ""
                try:
                    with tempfile.NamedTemporaryFile(suffix=suffix, delete=False) as handle:
                        temporary_path = handle.name
                        handle.write(image_bytes)
                    image = bpy.data.images.load(temporary_path, check_existing=False)
                    image.pack(data=image_bytes, data_len=len(image_bytes))
                    atexit.register(_remove_temporary_image_file, temporary_path)
                    temporary_path = ""
                except (RuntimeError, OSError, ValueError, TypeError):
                    if image is not None:
                        bpy.data.images.remove(image)
                    image = None
                finally:
                    if temporary_path:
                        _remove_temporary_image_file(temporary_path)
    if image is None and not tiles:
        filepath = reference.get("filepath", "")
        if (reference.get("sourceFileExists") and filepath and
                os.path.isfile(filepath)):
            try:
                if os.path.getsize(filepath) <= MAX_PROXY_IMAGE_BYTES:
                    image = bpy.data.images.load(filepath, check_existing=False)
            except (RuntimeError, OSError, ValueError):
                image = None
    if image is None and not tiles:
        image = _create_classicad_float_payload_image(reference)
        float_payload_image = image is not None
    if image is None:
        return

    try:
        image["classiCAD_proxy_image"] = True
        image.name = reference.get("imageName") or image.name
        if not tiles and (packed_data or float_payload_image):
            image.filepath = ""
            image.filepath_raw = ""
        elif reference.get("filepath") and not tiles:
            image.filepath = reference["filepath"]
            image.filepath_raw = reference["filepath"]
        if not float_payload_image:
            image.alpha_mode = reference.get("alphaMode", "STRAIGHT")
        color_space = ("Non-Color" if float_payload_image else
                       reference.get("colorSpace", "sRGB"))
        if color_space:
            image.colorspace_settings.name = color_space
    except (TypeError, ValueError):
        if image is not None and image.get("classiCAD_proxy_image") and image.users == 0:
            bpy.data.images.remove(image)
        return

    material.use_nodes = True
    node_bl_idname = ("ShaderNodeTexImage" if node_type == "TEX_IMAGE"
                      else "ShaderNodeTexEnvironment")
    node = material.node_tree.nodes.new(node_bl_idname)
    node.name = reference.get("nodeName") or "classiCAD Image Texture"
    node.image = image
    node.interpolation = reference.get("interpolation", "Linear")
    if node_type == "TEX_IMAGE":
        node.extension = reference.get("extension", "REPEAT")
    node.show_texture = True
    node["classiCAD_proxy_image_node"] = True
    node["classiCAD_proxy_image_key"] = reference_key
    _activate_classicad_image_node(material, node)


def _classicad_float_tile_bytes(reference, tile, number):
    float_reference = {
        "imageName": reference.get("imageName", "") + "." + str(number),
        "alphaMode": reference.get("alphaMode", "STRAIGHT"),
        "colorSpace": reference.get("colorSpace", ""),
        "sceneLinearFloatDataBase64": tile.get(
            "sceneLinearFloatDataBase64", ""),
        "sceneLinearFloatWidth": tile.get("sceneLinearFloatWidth", 0),
        "sceneLinearFloatHeight": tile.get("sceneLinearFloatHeight", 0),
    }
    float_image = _create_classicad_float_payload_image(float_reference)
    if float_image is None:
        return b""
    try:
        return bytes(float_image.packed_file.data)
    except (AttributeError, TypeError, ValueError):
        return b""
    finally:
        bpy.data.images.remove(float_image)


def _create_classicad_tiled_image(reference):
    """Rebuild a bounded packed UDIM image for a Workbench image node."""
    tiles = reference.get("tiles")
    template = reference.get("filepath", "")
    if (not isinstance(tiles, list) or not tiles or
            not isinstance(template, str) or "<UDIM>" not in template):
        return None

    tile_payloads = []
    tile_numbers = set()
    payload_kinds = set()
    total_bytes = 0
    use_float_payloads = all(
        isinstance(tile, dict) and tile.get("sceneLinearFloatDataBase64")
        for tile in tiles
    )
    template_extension = os.path.splitext(template)[1].lower()
    for tile in tiles:
        if not isinstance(tile, dict):
            return None
        number = tile.get("number")
        if (isinstance(number, bool) or not isinstance(number, int) or
                number < 1001 or number > 1999 or number in tile_numbers):
            return None
        tile_numbers.add(number)
        tile_path = tile.get("filepath", "")
        expected_path = template.replace("<UDIM>", str(number))
        if (not isinstance(tile_path, str) or
                os.path.normpath(tile_path) != os.path.normpath(expected_path)):
            return None

        payload = (_classicad_float_tile_bytes(reference, tile, number)
                   if use_float_payloads else b"")
        payload_kind = "float" if payload else "source"
        encoded = tile.get("packedDataBase64", "")
        if not payload and encoded:
            if not isinstance(encoded, str) or len(encoded) > 44 * 1024 * 1024:
                payload = b""
            else:
                try:
                    payload = base64.b64decode(encoded, validate=True)
                except (ValueError, TypeError):
                    payload = b""
        if not payload and tile.get("sourceFileExists") and tile_path and os.path.isfile(tile_path):
            try:
                size = os.path.getsize(tile_path)
                if 0 < size <= MAX_PROXY_IMAGE_BYTES:
                    with open(tile_path, "rb") as handle:
                        payload = handle.read(MAX_PROXY_IMAGE_BYTES + 1)
                    if len(payload) != size:
                        payload = b""
            except OSError:
                payload = b""
        if not payload and not use_float_payloads:
            payload = _classicad_float_tile_bytes(reference, tile, number)
            if payload:
                payload_kind = "float"
        if not payload:
            return None
        total_bytes += len(payload)
        if total_bytes > 32 * 1024 * 1024:
            return None
        if payload_kind == "float":
            extension = ".exr"
        else:
            extension = os.path.splitext(tile_path)[1].lower() or template_extension
            if template_extension and extension != template_extension:
                return None
            if not extension:
                if payload.startswith(b"\x89PNG\r\n\x1a\n"):
                    extension = ".png"
                elif payload.startswith(b"\xff\xd8"):
                    extension = ".jpg"
                elif payload.startswith(b"\x76\x2f\x31\x01"):
                    extension = ".exr"
                else:
                    return None
        payload_kinds.add(payload_kind)
        tile_payloads.append((number, payload, extension, payload_kind))

    if (not tile_payloads or len(payload_kinds) != 1 or
            len({extension for _, _, extension, _ in tile_payloads}) != 1):
        return None
    tile_payloads.sort(key=lambda entry: entry[0])
    extension = tile_payloads[0][2]
    image = None
    directory = tempfile.mkdtemp(prefix="classiCAD_udim_proxy_")
    try:
        for number, payload, _, _ in tile_payloads:
            path = os.path.join(directory, "texture.%d%s" % (number, extension))
            with open(path, "wb") as handle:
                handle.write(payload)
        first_number = tile_payloads[0][0]
        first_path = os.path.join(
            directory, "texture.%d%s" % (first_number, extension)
        )
        image = bpy.data.images.load(first_path, check_existing=False)
        image.source = "TILED"
        existing_numbers = {int(tile.number) for tile in image.tiles}
        if first_number not in existing_numbers:
            bpy.data.images.remove(image)
            image = None
            _remove_temporary_image_directory(directory)
            return None
        for number, _, _, _ in tile_payloads:
            if number not in existing_numbers:
                image.tiles.new(number)
                existing_numbers.add(number)
        image.filepath = os.path.join(directory, "texture.<UDIM>" + extension)
        image.filepath_raw = image.filepath
        image.reload()
        float_payload_tiles = payload_kinds == {"float"}
        if float_payload_tiles:
            image.colorspace_settings.name = "Non-Color"
        image.alpha_mode = reference.get("alphaMode", "STRAIGHT")
        image.pack()
        packed = {
            int(item.tile_number): bytes(item.packed_file.data)
            for item in image.packed_files
        }
        if (set(packed) != tile_numbers or any(
                packed[number] != payload
                for number, payload, _, _ in tile_payloads)):
            bpy.data.images.remove(image)
            image = None
            _remove_temporary_image_directory(directory)
            return None
        image["classiCAD_proxy_float_payload"] = float_payload_tiles
        image["classiCAD_source_color_space"] = reference.get("colorSpace", "")
        image["classiCAD_source_alpha_mode"] = reference.get("alphaMode", "STRAIGHT")
        atexit.register(_remove_temporary_image_directory, directory)
        return image
    except (RuntimeError, OSError, ValueError, TypeError):
        if image is not None:
            bpy.data.images.remove(image)
        _remove_temporary_image_directory(directory)
        return None


def _activate_classicad_image_node(material, node):
    node.show_texture = True
    for candidate in material.node_tree.nodes:
        candidate.select = candidate == node
    material.node_tree.nodes.active = node


def _remove_classicad_image_nodes(material, nodes):
    if not material.use_nodes:
        return
    for node in nodes:
        image = getattr(node, "image", None)
        node.show_texture = False
        material.node_tree.nodes.remove(node)
        if (image is not None and image.get("classiCAD_proxy_image") and
                image.users == 0):
            bpy.data.images.remove(image)


def attach_solid_face_metadata(mesh, polygon_face_refs, document):
    """Tag each proxy polygon with its source object and semantic seed-face ID."""
    if len(polygon_face_refs) != len(mesh.polygons):
        raise ValueError("solid proxy face identity count does not match polygons")
    references = []
    reference_indices = {}
    tokens = []
    for reference in polygon_face_refs:
        if not reference or not reference[1]:
            tokens.append(-1)
            continue
        key = (str(reference[0]), str(reference[1]))
        token = reference_indices.get(key)
        if token is None:
            token = len(references)
            reference_indices[key] = token
            references.append([key[0], key[1]])
        tokens.append(token)
    attribute = mesh.attributes.get("classiCAD_solid_face_token")
    if attribute is not None:
        mesh.attributes.remove(attribute)
    attribute = mesh.attributes.new(
        "classiCAD_solid_face_token", "INT", "FACE"
    )
    attribute.data.foreach_set("value", tokens)
    mesh["classiCAD_solid_face_refs"] = json.dumps(
        references, ensure_ascii=False, separators=(",", ":")
    )
    materials = classiCAD_materials(document)
    for material_id, material in materials.items():
        mesh.materials.append(material)
    slots = {material_id: index for index, material_id in enumerate(materials)}
    unassigned = next(
        (material for material in bpy.data.materials
         if material.get("classiCAD_unassigned_material_slot")),
        None,
    )
    if unassigned is None:
        unassigned = bpy.data.materials.new("classiCAD.Unassigned")
        unassigned.diffuse_color = (0.8, 0.8, 0.8, 1.0)
    unassigned["classiCAD_material_id"] = ""
    unassigned["classiCAD_unassigned_material_slot"] = True
    unassigned_slot = len(mesh.materials)
    mesh.materials.append(unassigned)
    records = {str(record.get("id")): record
               for record in document.get("objects", [])}
    existing = {object_id: record.get("nurbsSolidFaceMaterialIds", {})
                for object_id, record in records.items()
                if int(record.get("geometry", {}).get("geometryType", -1)) == 15}
    for polygon, reference in zip(mesh.polygons, polygon_face_refs):
        if not reference or not reference[1]:
            continue
        polygon.material_index = unassigned_slot
        material_id = existing.get(str(reference[0]), {}).get(str(reference[1]))
        if material_id in slots:
            polygon.material_index = slots[material_id]


def merge_solid_proxy_materials(document):
    """Merge unambiguous proxy assignments into exact, unmaterialized seeds."""
    object_records = {str(record.get("id")): record
                      for record in document.get("objects", [])}
    material_ids = {str(record.get("id", ""))
                    for record in document.get("materials", [])}
    assignments = {}
    for obj in bpy.data.objects:
        if obj.get("classiCAD_proxy_geometry") != "NurbsSurface and NurbsSolid":
            continue
        mesh = obj.data if obj.type == "MESH" else None
        if mesh is None:
            continue
        attribute = mesh.attributes.get("classiCAD_solid_face_token")
        raw_references = mesh.get("classiCAD_solid_face_refs", "")
        if attribute is None or not raw_references:
            continue
        try:
            references = json.loads(raw_references)
        except (TypeError, ValueError):
            continue
        if len(attribute.data) != len(mesh.polygons):
            continue
        by_face = {}
        for polygon, item in zip(mesh.polygons, attribute.data):
            token = int(item.value)
            if token < 0 or token >= len(references):
                continue
            reference = references[token]
            if not isinstance(reference, list) or len(reference) != 2:
                continue
            object_id, face_id = str(reference[0]), str(reference[1])
            record = object_records.get(object_id)
            if record is None or not face_id:
                continue
            geometry = record.get("geometry", {})
            if int(geometry.get("geometryType", -1)) != 15:
                continue
            solid = geometry.get("nurbsSolid", {})
            if solid.get("boundaryFaces"):
                continue
            semantic_ids = {
                str(solid.get("baseCapFaceId", "")),
                str(solid.get("displacedCapFaceId", "")),
                *(str(value) for value in solid.get("wallFaceIds", [])),
            }
            if face_id not in semantic_ids:
                continue
            slot_index = int(polygon.material_index)
            material = (obj.material_slots[slot_index].material
                        if 0 <= slot_index < len(obj.material_slots) else None)
            if material is None:
                material_id = None
            elif material.get("classiCAD_unassigned_material_slot"):
                material_id = ""
            else:
                material_id = str(material.get("classiCAD_material_id", ""))
                if material_id not in material_ids:
                    material_id = None
            by_face.setdefault((object_id, face_id), set()).add(material_id)
        for key, values in by_face.items():
            # A seed face is one semantic surface. If a user assigns several
            # proxy materials across its tessellation, retain its prior value.
            if len(values) == 1:
                value = next(iter(values))
                assignments[key] = value
    for (object_id, face_id), material_id in assignments.items():
        face_materials = object_records[object_id].setdefault(
            "nurbsSolidFaceMaterialIds", {}
        )
        if material_id is None:
            continue
        if material_id:
            face_materials[face_id] = material_id
        else:
            face_materials.pop(face_id, None)


def add_scene_object(collections, record, surface_display_mesh=None,
                     document=None):
    object_id = str(record["id"])
    collection_key = "default"
    shape = record["geometry"]
    geometry_type = int(shape.get("geometryType", shape.get("tool", 0)))
    collection = collections.get(collection_key)
    if collection is None:
        raise RuntimeError("scene object references an unknown collection " + collection_key)

    object_name = "classiCAD.%s.%s" % (geometry_type, object_id)
    frame = frame_matrix(shape)
    if geometry_type in {14, 15}:
        obj = add_nurbs_surface_mesh(
            collection,
            object_name,
            shape.get("nurbsSurface", {}),
            surface_display_mesh,
        )
        if obj is None:
            raise RuntimeError("invalid NURBS surface data for object " + object_id)
        obj["classiCAD_object_id"] = object_id
        obj["classiCAD_geometry_type"] = geometry_type
        obj["classiCAD_data_block"] = DOCUMENT_TEXT_NAME
        obj["classiCAD_proxy_geometry"] = "NurbsSurface and NurbsSolid"
        obj.show_in_front = bool(record.get("showInFront", False))
        if geometry_type == 15 and surface_display_mesh is not None:
            face_ids = surface_display_mesh.get("solidFaceIds", [])
            attach_solid_face_metadata(
                obj.data,
                [(object_id, str(face_id)) if face_id else None
                 for face_id in face_ids],
                document or {},
            )
        return
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
    obj["classiCAD_geometry_type"] = geometry_type
    obj["classiCAD_data_block"] = DOCUMENT_TEXT_NAME
    obj.show_in_front = bool(record.get("showInFront", False))
    collection.objects.link(obj)


def add_curve_record_to_group(curve_data, record):
    shape = record["geometry"]
    geometry_type = int(shape.get("geometryType", shape.get("tool", 0)))
    frame = frame_matrix(shape)
    components = shape.get("components", [])
    component_frames = shape.get("componentWorkPlaneFrames", [])
    has_curve = False

    if components:
        for index, component in enumerate(components):
            transform = frame
            if len(component_frames) == len(components):
                transform = frame_matrix(
                    {"workPlaneFrame": component_frames[index]}
                )
            has_curve = add_nurbs_curve(
                curve_data, component, transform
            ) or has_curve
    else:
        has_curve = add_nurbs_curve(
            curve_data, shape.get("nurbs", {}), frame
        )

    if not has_curve and geometry_type in {5, 10, 13}:
        points = []
        for point in shape.get("points", []):
            world = frame @ Vector(
                (float(point["x"]), float(point["y"]), 0.0, 1.0)
            )
            points.append((world.x, world.y, world.z))
        if add_polyline_spline(curve_data, points):
            curve_data.splines[-1].use_cyclic_u = geometry_type in {5, 10, 13}
            has_curve = True
    return has_curve


def create_grouped_scene_objects(collections, records, surface_display_meshes,
                                 document):
    # Large projects use grouped Blender display proxies. Per-object identity,
    # exact NURBS data, and layer membership remain in the authoritative text.
    mesh_batches = {}
    curve_batches = {}
    fallback_records = []
    curve_types = {1, 2, 3, 4, 5, 6, 8, 9, 10}

    for record in records:
        object_id = str(record["id"])
        collection_key = "default"
        shape = record["geometry"]
        geometry_type = int(shape.get("geometryType", shape.get("tool", 0)))
        if geometry_type in {14, 15}:
            display_mesh = surface_display_meshes.get(object_id)
            if display_mesh is None:
                fallback_records.append(record)
                continue
            source_vertices = display_mesh.get("vertices", [])
            source_faces = display_mesh.get("faces", [])
            source_corner_uvs = display_mesh.get("cornerUvs")
            if not source_vertices or not source_faces:
                raise ValueError("derived NURBS surface display mesh is empty")
            if (source_corner_uvs is not None and
                    len(source_corner_uvs) != len(source_faces)):
                raise ValueError("derived NURBS surface proxy UV count is invalid")
            batch = mesh_batches.setdefault(
                collection_key, {"vertices": [], "faces": [], "corner_uvs": [],
                           "uv_incomplete": False, "face_refs": [], "count": 0}
            )
            vertex_offset = len(batch["vertices"])
            batch["vertices"].extend(
                (float(point[0]), float(point[1]), float(point[2]))
                for point in source_vertices
            )
            batch["faces"].extend(
                tuple(vertex_offset + int(index) for index in face)
                for face in source_faces
            )
            if source_corner_uvs is None:
                batch["uv_incomplete"] = True
            else:
                batch["corner_uvs"].extend(source_corner_uvs)
            semantic_ids = display_mesh.get("solidFaceIds", [])
            if len(semantic_ids) != len(source_faces):
                semantic_ids = [""] * len(source_faces)
            batch["face_refs"].extend(
                [(object_id, str(face_id)) if geometry_type == 15 and face_id else None
                 for face_id in semantic_ids]
            )
            batch["count"] += 1
            continue

        if geometry_type in curve_types:
            batch = curve_batches.get(collection_key)
            if batch is None:
                curve_data = bpy.data.curves.new(
                    "classiCAD.curves-proxy." + collection_key, "CURVE"
                )
                curve_data.dimensions = "3D"
                curve_data.resolution_u = 16
                batch = {"data": curve_data, "count": 0}
                curve_batches[collection_key] = batch
            if add_curve_record_to_group(batch["data"], record):
                batch["count"] += 1
            else:
                fallback_records.append(record)
            continue

        fallback_records.append(record)

    for collection_key, batch in mesh_batches.items():
        mesh = bpy.data.meshes.new("classiCAD.surface-proxy." + collection_key)
        mesh.from_pydata(batch["vertices"], [], batch["faces"])
        mesh.update()
        if (not batch["uv_incomplete"] and
                len(batch["corner_uvs"]) == len(batch["faces"])):
            attach_proxy_corner_uvs(mesh, batch["corner_uvs"])
        obj = bpy.data.objects.new("classiCAD.surface-proxy." + collection_key, mesh)
        attach_solid_face_metadata(mesh, batch["face_refs"], document)
        obj["classiCAD_proxy_geometry"] = "NurbsSurface and NurbsSolid"
        obj["classiCAD_proxy_object_count"] = batch["count"]
        obj["classiCAD_data_block"] = DOCUMENT_TEXT_NAME
        collections[collection_key].objects.link(obj)

    for collection_key, batch in curve_batches.items():
        if not batch["data"].splines:
            bpy.data.curves.remove(batch["data"])
            continue
        obj = bpy.data.objects.new(
            "classiCAD.curves-proxy." + collection_key, batch["data"]
        )
        obj["classiCAD_proxy_geometry"] = "NurbsCurves"
        obj["classiCAD_proxy_object_count"] = batch["count"]
        obj["classiCAD_data_block"] = DOCUMENT_TEXT_NAME
        collections[collection_key].objects.link(obj)

    for record in fallback_records:
        add_scene_object(
            collections,
            record,
            surface_display_meshes.get(str(record["id"])),
            document,
        )


def create_project(document, destination_path):
    # Use the user's saved Blender startup UI (workspaces and screen layout),
    # but create an empty scene for the classiCAD project.
    bpy.ops.wm.read_homefile(use_empty=True, use_factory_startup=False)
    scene = bpy.context.scene
    surface_display_meshes = document.pop("_surfaceDisplayMeshes", {})
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

    # classiCAD no longer invents CAD layers for Blender projects. Keep the
    # proxy objects under one ordinary Blender collection; imported Blender
    # collections are represented by the document's separate collection model.
    collections = {"default": root_collection}

    object_records = document.get("objects", [])
    if len(object_records) >= 1000:
        create_grouped_scene_objects(
            collections, object_records, surface_display_meshes, document
        )
    else:
        for record in object_records:
            add_scene_object(
                collections,
                record,
                surface_display_meshes.get(str(record["id"])),
                document,
            )

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
        # Let the C++ project loader route ordinary .blend files through the
        # supported scene extractor instead of interpreting display proxies.
        with open(output_json_path, "w", encoding="utf-8") as output_file:
            json.dump({"format": "classiCAD.ordinary-blend"}, output_file)
        return
    if int(scene.get("classiCAD_format_version", -1)) != PROJECT_FORMAT_VERSION:
        raise RuntimeError("This classiCAD project format version is not supported")

    document_text = bpy.data.texts.get(DOCUMENT_TEXT_NAME)
    if document_text is None:
        raise RuntimeError("The classiCAD document data is missing from this Blender file")
    document = json.loads(document_text.as_string())
    merge_solid_proxy_materials(document)
    with open(output_json_path, "w", encoding="utf-8") as output_file:
        json.dump(document, output_file, ensure_ascii=False, separators=(",", ":"))


def outliner_orphan_groups():
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
            id_key = id_block.as_pointer()
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


def outliner_data_ids():
    seen = set()
    for property_rna in bpy.data.bl_rna.properties:
        if getattr(property_rna, "type", "") != "COLLECTION":
            continue
        try:
            collection = getattr(bpy.data, property_rna.identifier)
        except (AttributeError, ReferenceError, RuntimeError, TypeError):
            continue
        for id_block in collection:
            if not isinstance(id_block, bpy.types.ID):
                continue
            pointer = id_block.as_pointer()
            if pointer in seen:
                continue
            seen.add(pointer)
            yield id_block


def normalized_library_path(path):
    if not path:
        return ""
    return os.path.normcase(os.path.realpath(bpy.path.abspath(path)))


def resolve_outliner_data_id(path):
    id_type = path.get("id_type", "")
    id_name = path.get("id_name", "")
    library_path = path.get("library_path", "")
    if not isinstance(id_type, str) or not id_type or len(id_type) > 256:
        raise ValueError("Keying Set path has an invalid ID type")
    if not isinstance(id_name, str) or len(id_name) > 4096:
        raise ValueError("Keying Set path has an invalid ID name")
    if not isinstance(library_path, str) or len(library_path) > 4096:
        raise ValueError("Keying Set path has an invalid library path")
    if not id_name:
        raise ValueError("Keying Set path has no target data block")

    expected_library = normalized_library_path(library_path)
    matches = []
    for id_block in outliner_data_ids():
        if (str(id_block.id_type) != id_type or
                str(id_block.name_full) != id_name):
            continue
        actual_library = normalized_library_path(
            getattr(getattr(id_block, "library", None), "filepath", ""))
        if actual_library == expected_library:
            matches.append(id_block)
    if len(matches) != 1:
        raise ValueError(
            "Could not resolve Keying Set target %s (%s) uniquely" %
            (id_name, id_type))
    return matches[0]


def relative_keying_set_type_id(keying_set):
    try:
        if keying_set.is_path_absolute:
            return ""
        type_info = keying_set.type_info
        if type_info is None:
            return ""
        type_id = str(type_info.bl_idname)
        return type_id if type_id else ""
    except (AttributeError, ReferenceError, RuntimeError, TypeError):
        return ""


def relative_keying_set_supported(keying_set):
    type_id = relative_keying_set_type_id(keying_set)
    if type_id not in SUPPORTED_OBJECT_RELATIVE_KEYING_SET_IDS:
        return False
    try:
        type_info = keying_set.type_info
        return bool(callable(type_info.poll) and callable(type_info.iterator))
    except (AttributeError, ReferenceError, RuntimeError, TypeError):
        return False


def outliner_keying_set_records(scene):
    records = []
    active_key = ""
    truncated = False
    collection = scene.keying_sets
    active = collection.active
    if len(collection) > 1024:
        raise ValueError("Scene has too many Keying Sets to edit safely")
    active_is_scene_set = active is not None and any(
        candidate == active for candidate in collection)
    scene_set_limit = 1024 if active_is_scene_set or active is None else 1023
    remaining_paths = 20000
    for index, keying_set in enumerate(collection):
        if index >= scene_set_limit:
            truncated = True
            break
        paths = []
        for source_path in keying_set.paths:
            if remaining_paths <= 0:
                truncated = True
                break
            id_block = source_path.id
            paths.append({
                "id_type": str(source_path.id_type)[:256],
                "id_name": (str(id_block.name_full)[:4096]
                            if isinstance(id_block, bpy.types.ID) else ""),
                "library_path": (
                    str(getattr(getattr(id_block, "library", None), "filepath", ""))[:4096]
                    if isinstance(id_block, bpy.types.ID) else ""),
                "data_path": str(source_path.data_path)[:4096],
                "array_index": int(source_path.array_index),
                "use_entire_array": bool(source_path.use_entire_array),
                "group": str(source_path.group)[:4096],
                "group_method": str(source_path.group_method)[:128],
            })
            remaining_paths -= 1
        name = str(keying_set.bl_label)
        key = "keying-set:%d:%s" % (index, name)
        relative_keyframe_supported = relative_keying_set_supported(keying_set)
        records.append({
            "key": key,
            "name": name[:4096],
            "is_absolute": bool(keying_set.is_path_absolute),
            "is_builtin": False,
            "relative_keyframe_supported": relative_keyframe_supported,
            "paths": paths,
        })
        if keying_set == active:
            active_key = key
        if remaining_paths <= 0 and len(keying_set.paths) > len(paths):
            truncated = True
            break

    # Blender's Outliner reads scene.keying_sets.active, which can refer to a
    # built-in relative Keying Set stored in scene.keying_sets_all rather than
    # in the scene-owned collection shown by its picker.
    if active is not None and not any(
            keying_set == active for keying_set in collection):
        type_id = relative_keying_set_type_id(active)
        if type_id:
            name = str(active.bl_label)[:4096]
            records.append({
                "key": "keying-set-info:" + type_id,
                "name": name,
                "is_absolute": False,
                "is_builtin": True,
                "relative_keyframe_supported": relative_keying_set_supported(
                    active),
                "paths": [],
            })
            active_key = "keying-set-info:" + type_id
    if len(collection) > scene_set_limit:
        truncated = True
    return records, active_key, truncated


def _keying_set_path_record(source_path):
    return {
        "id": source_path.id,
        "id_type": str(source_path.id_type),
        "data_path": str(source_path.data_path),
        "array_index": int(source_path.array_index),
        "use_entire_array": bool(source_path.use_entire_array),
        "group": str(source_path.group),
        "group_method": str(source_path.group_method),
        "use_insertkey_needed": bool(source_path.use_insertkey_needed),
        "use_insertkey_override_needed": bool(
            source_path.use_insertkey_override_needed),
        "use_insertkey_visual": bool(source_path.use_insertkey_visual),
        "use_insertkey_override_visual": bool(
            source_path.use_insertkey_override_visual),
    }


def _restore_keying_set_paths(keying_set, paths):
    for existing in list(keying_set.paths):
        keying_set.paths.remove(existing)
    for path in paths:
        restored = keying_set.paths.add(
            path["id"], path["data_path"], index=path["array_index"],
            group_method=path["group_method"], group_name=path["group"],
        )
        if restored is None:
            raise RuntimeError("Blender could not restore a relative Keying Set path")
        if path["id"] is None:
            restored.id_type = path["id_type"]
        restored.use_entire_array = path["use_entire_array"]
        restored.use_insertkey_needed = path["use_insertkey_needed"]
        restored.use_insertkey_override_needed = path[
            "use_insertkey_override_needed"]
        restored.use_insertkey_visual = path["use_insertkey_visual"]
        restored.use_insertkey_override_visual = path[
            "use_insertkey_override_visual"]


def _relative_keying_set_paths(scene, keying_set,
                               selected_source_object_ids,
                               active_source_object_id,
                               view_layer_name):
    if not selected_source_object_ids:
        raise ValueError(
            "Select one or more imported Blender objects before using a relative Keying Set")
    if len(selected_source_object_ids) > MAX_RELATIVE_KEYING_SET_OBJECTS:
        raise ValueError("Too many selected objects for a relative Keying Set")
    if len(keying_set.paths) > 20000:
        raise ValueError("The relative Keying Set has too many paths to edit safely")

    if not relative_keying_set_supported(keying_set):
        raise ValueError(
            "This relative Keying Set needs a Blender context classiCAD does not represent")
    type_info = keying_set.type_info

    normalized_filepath = os.path.normcase(
        os.path.realpath(os.path.abspath(bpy.data.filepath)))
    project_namespace = hashlib.sha256(
        normalized_filepath.encode("utf-8")).hexdigest()

    def source_object_id(obj):
        library = getattr(obj, "library", None)
        library_path = library.filepath if library is not None else ""
        return (project_namespace + "::" + library_path + "::" +
                obj.name_full)

    objects_by_source_id = {
        source_object_id(obj): obj for obj in bpy.data.objects
    }
    selected_objects = []
    for source_id in selected_source_object_ids:
        obj = objects_by_source_id.get(source_id)
        if obj is None:
            raise ValueError(
                "A selected classiCAD object no longer exists in the Blender file")
        if not bool(getattr(obj, "is_editable", False)):
            raise ValueError(
                "Selected Blender object %s is read-only" % obj.name_full)
        if obj.mode != "OBJECT":
            raise ValueError(
                "Relative object Keying Sets need Blender Object mode; pose-bone keying is not represented by the imported object selection")
        if obj not in selected_objects:
            selected_objects.append(obj)

    if not isinstance(view_layer_name, str) or not view_layer_name:
        raise ValueError("The active Blender View Layer is unavailable")
    view_layer = scene.view_layers.get(view_layer_name)
    if view_layer is None:
        raise ValueError("The active Blender View Layer no longer exists")
    layer_objects = list(view_layer.objects)
    if any(obj not in layer_objects for obj in selected_objects):
        raise ValueError(
            "A selected Blender object is outside the active View Layer")

    active_object = objects_by_source_id.get(active_source_object_id)
    if active_object not in selected_objects:
        active_object = selected_objects[-1]

    type_info = keying_set.type_info
    context = SimpleNamespace(
        area=SimpleNamespace(type="OUTLINER"),
        scene=scene,
        view_layer=view_layer,
        active_object=active_object,
        selected_objects=selected_objects,
        active_pose_bone=None,
        selected_pose_bones=[],
        active_strip=None,
        selected_strips=[],
    )
    previous_paths = [_keying_set_path_record(path)
                      for path in keying_set.paths]
    if any(not isinstance(path["id"], bpy.types.ID)
           for path in previous_paths):
        raise ValueError(
            "Blender cannot safely regenerate this relative Keying Set because one of its stored paths has no target ID")
    try:
        # Call Blender's registered KeyingSetInfo poll and iterator callbacks
        # with the imported selection represented as a headless OUTLINER
        # context. The supported built-ins use these context fields and their
        # own Blender path generators.
        _restore_keying_set_paths(keying_set, [])
        if not type_info.poll(context):
            raise ValueError(
                "Blender's relative Keying Set does not apply to the selected objects")
        type_info.iterator(context, keying_set)

        if len(keying_set.paths) > 20000:
            raise ValueError(
                "The relative Keying Set generated too many paths to edit safely")
        if not keying_set.paths:
            raise ValueError(
                "Blender's relative Keying Set generated no animation paths for the selected objects")
        generated_paths = [_keying_set_path_record(path)
                           for path in keying_set.paths]
    finally:
        _restore_keying_set_paths(keying_set, previous_paths)
    return generated_paths


def _apply_keying_set_keyframes(scene, keying_set, paths, insert_keyframes):
    resolved_paths = []
    for source_path in paths:
        id_block = source_path["id"]
        data_path = source_path["data_path"]
        if not isinstance(id_block, bpy.types.ID):
            raise ValueError("A Keying Set path has no target data block")
        if not bool(getattr(id_block, "is_editable", False)):
            raise ValueError(
                "Keying Set target %s is read-only" % id_block.name_full)
        if not isinstance(data_path, str) or not data_path or len(data_path) > 4096:
            raise ValueError("A Keying Set path has an invalid RNA path")
        try:
            id_block.path_resolve(data_path)
        except (AttributeError, ReferenceError, RuntimeError, TypeError,
                ValueError) as error:
            raise ValueError(
                "Keying Set RNA path could not be resolved: " + data_path
            ) from error
        array_index = source_path["array_index"]
        index = (-1 if source_path["use_entire_array"]
                 else array_index)
        if index < -1 or index > 32766:
            raise ValueError("A Keying Set path has an invalid array index")
        group_method = source_path["group_method"]
        if group_method == "NONE":
            group = ""
        elif group_method == "KEYINGSET":
            group = str(keying_set.bl_label)
        elif group_method == "NAMED":
            group = source_path["group"]
        else:
            raise ValueError("A Keying Set path has an unsupported group mode")
        resolved_paths.append((source_path, id_block, data_path, index, group))

    frame = float(scene.frame_current_final)
    if not math.isfinite(frame):
        raise ValueError("The active scene frame is invalid")

    preferences = bpy.context.preferences.edit
    tool_settings = scene.tool_settings
    base_needed = bool(getattr(
        preferences, "use_keyframe_insert_needed", False))
    base_visual = bool(getattr(preferences, "use_visual_keying", False))
    type_info = getattr(keying_set, "type_info", None)
    if type_info is not None:
        try:
            base_visual = base_visual or (
                "INSERTKEY_VISUAL" in set(type_info.bl_options))
        except (AttributeError, ReferenceError, RuntimeError, TypeError):
            pass
    base_cycle_aware = bool(getattr(
        tool_settings, "use_keyframe_cycle_aware", False))
    key_type = str(tool_settings.keyframe_type)

    def effective_keying_flag(base_value, keying_set_value,
                              keying_set_override, path_value,
                              path_override):
        value = (bool(keying_set_value) if keying_set_override
                 else base_value)
        return bool(path_value) if path_override else value

    changed_path_count = 0
    for (source_path, id_block, data_path, index,
         group) in resolved_paths:
        options = set()
        if effective_keying_flag(
                base_needed, keying_set.use_insertkey_needed,
                keying_set.use_insertkey_override_needed,
                source_path["use_insertkey_needed"],
                source_path["use_insertkey_override_needed"]):
            options.add("INSERTKEY_NEEDED")
        if effective_keying_flag(
                base_visual, keying_set.use_insertkey_visual,
                keying_set.use_insertkey_override_visual,
                source_path["use_insertkey_visual"],
                source_path["use_insertkey_override_visual"]):
            options.add("INSERTKEY_VISUAL")
        if base_cycle_aware:
            options.add("INSERTKEY_CYCLE_AWARE")

        if insert_keyframes:
            changed_path = id_block.keyframe_insert(
                data_path, index=index, frame=frame, group=group,
                options=options, keytype=key_type)
        else:
            changed_path = id_block.keyframe_delete(
                data_path, index=index, frame=frame, group=group)
        if changed_path:
            changed_path_count += 1

    return frame, changed_path_count


def edit_keying_set_paths(source_path, staged_path, request_path, report_path):
    with open(request_path, "r", encoding="utf-8") as request_file:
        request = json.load(request_file)
    if not isinstance(request, dict):
        raise ValueError("Keying Set edit request must be an object")
    edit_mode = request.get("edit_mode", "paths")
    scene_name = request.get("scene_name", "")
    keying_set_request = request.get("keying_set")
    paths = request.get("paths")
    add = request.get("add")
    insert_keyframes = request.get("insert_keyframes")
    selected_source_object_ids = request.get(
        "selected_source_object_ids", [])
    active_source_object_id = request.get("active_source_object_id", "")
    view_layer_name = request.get("view_layer_name", "")
    if (not isinstance(scene_name, str) or not scene_name or
            len(scene_name) > 4096 or
            not isinstance(keying_set_request, dict) or
            edit_mode not in {"paths", "keyframes"} or
            (edit_mode == "paths" and
             (not isinstance(paths, list) or len(paths) > 20000 or
              not isinstance(add, bool))) or
            (edit_mode == "keyframes" and
             (not isinstance(insert_keyframes, bool) or
              not isinstance(selected_source_object_ids, list) or
              len(selected_source_object_ids) >
                  MAX_RELATIVE_KEYING_SET_OBJECTS or
              any(not isinstance(value, str) or not value or len(value) > 8192
                  for value in selected_source_object_ids) or
              not isinstance(active_source_object_id, str) or
              len(active_source_object_id) > 8192 or
              not isinstance(view_layer_name, str) or
              len(view_layer_name) > 4096))):
        raise ValueError("Keying Set edit request is invalid")

    result = bpy.ops.wm.open_mainfile(
        filepath=os.path.abspath(source_path),
        load_ui=True,
        use_scripts=False,
    )
    if "FINISHED" not in result:
        raise RuntimeError("Blender did not finish opening the source project")
    if any(scene.get("classiCAD_format") == PROJECT_FORMAT
           for scene in bpy.data.scenes):
        raise RuntimeError(
            "Keying Set editing cannot edit a classiCAD-native Blender project")

    scene_matches = [scene for scene in bpy.data.scenes
                     if scene.name_full == scene_name]
    if len(scene_matches) != 1:
        raise ValueError("Could not resolve the imported Blender scene uniquely")
    scene = scene_matches[0]
    collection = scene.keying_sets
    key = keying_set_request.get("key", "")
    name = keying_set_request.get("name", "")
    if (not isinstance(key, str) or not key or len(key) > 8192 or
            not isinstance(name, str) or not name or len(name) > 4096):
        raise ValueError("Active Keying Set identity is invalid")

    keying_set = None
    is_builtin_request = False
    if key.startswith("keying-set-info:"):
        type_id = key[len("keying-set-info:"):]
        if (not type_id or len(type_id) > 256 or
                not bool(keying_set_request.get("is_builtin", False))):
            raise ValueError("Imported built-in Keying Set identity is invalid")
        matches = [candidate for candidate in scene.keying_sets_all
                   if relative_keying_set_type_id(candidate) == type_id]
        if len(matches) != 1 or str(matches[0].bl_label) != name:
            raise ValueError(
                "The active built-in Keying Set no longer matches the imported Blender file")
        keying_set = matches[0]
        is_builtin_request = True
    elif key.startswith("keying-set:"):
        encoded_index_and_name = key[len("keying-set:"):]
        index_text, separator, encoded_name = encoded_index_and_name.partition(":")
        if not separator:
            raise ValueError("Imported Keying Set identity is invalid")
        try:
            index = int(index_text)
        except ValueError as error:
            raise ValueError("Imported Keying Set index is invalid") from error
        if (index < 0 or index >= len(collection) or
                collection[index].bl_label != encoded_name or
                encoded_name != name):
            raise ValueError(
                "The active Keying Set no longer matches the imported Blender file")
        keying_set = collection[index]
    else:
        if not key.startswith("classiCAD-keying-set:"):
            raise ValueError("Unsupported Keying Set identity")
        if any(existing.bl_label == name for existing in collection):
            raise ValueError("A Blender Keying Set with this name already exists")
        keying_set = collection.new(idname="KeyingSet", name=name)

    # Blender's Outliner creates a scene-owned absolute set when there is no
    # active scene Keying Set (for example, when a built-in set is active).
    set_index = next((index for index, candidate in enumerate(collection)
                      if candidate == keying_set), -1)
    if set_index >= 0:
        changed = int(collection.active_index) != set_index
        collection.active_index = set_index
    elif is_builtin_request:
        changed = collection.active != keying_set
        scene.keying_sets_all.active = keying_set
    else:
        raise RuntimeError("Blender could not activate the edited Keying Set")

    # Blender's Outliner path-edit operators call verify_active_keyingset().
    # A built-in active set has a negative scene active index, so those
    # operators create a scene-owned absolute set before editing paths.
    if edit_mode == "paths" and is_builtin_request:
        set_name = "KeyingSet"
        suffix = 1
        existing_names = {candidate.bl_label for candidate in collection}
        while set_name in existing_names:
            set_name = "KeyingSet.%03d" % suffix
            suffix += 1
        keying_set = collection.new(idname="KeyingSet", name=set_name)
        set_index = len(collection) - 1
        if set_index < 0 or collection[set_index] != keying_set:
            raise RuntimeError(
                "Blender could not create a scene Keying Set for the Outliner path operation")
        collection.active_index = set_index
        changed = True

    absolute = bool(keying_set.is_path_absolute)

    changed_path_count = 0
    if edit_mode == "paths":
        normalized_paths = []
        for path in paths:
            if not isinstance(path, dict):
                raise ValueError("Keying Set path must be an object")
            data_path = path.get("data_path", "")
            array_index = path.get("array_index", 0)
            entire_array = path.get("use_entire_array", False)
            group = path.get("group", "")
            group_method = path.get("group_method", "KEYINGSET")
            if (not isinstance(data_path, str) or not data_path or
                    len(data_path) > 4096 or isinstance(array_index, bool) or
                    not isinstance(array_index, int) or
                    array_index < 0 or array_index > 32766 or
                    not isinstance(entire_array, bool) or
                    not isinstance(group, str) or len(group) > 4096 or
                    group_method not in {"NAMED", "NONE", "KEYINGSET"}):
                raise ValueError("Keying Set path metadata is invalid")
            id_block = resolve_outliner_data_id(path)
            try:
                id_block.path_resolve(data_path)
            except (AttributeError, ReferenceError, RuntimeError, TypeError,
                    ValueError) as error:
                raise ValueError(
                    "Keying Set RNA path could not be resolved: " + data_path
                ) from error
            normalized_paths.append((path, id_block, data_path, array_index,
                                     entire_array, group, group_method))

        for path, id_block, data_path, array_index, entire_array, group, group_method in normalized_paths:
            id_type = str(path.get("id_type", ""))
            if not id_type or len(id_type) > 256:
                raise ValueError("Keying Set path has an invalid ID type")
            matches = []
            for existing in keying_set.paths:
                same_id = existing.id == id_block
                if (same_id and existing.data_path == data_path and
                        int(existing.array_index) == array_index):
                    matches.append(existing)
            if add:
                if matches:
                    continue
                keying_path = keying_set.paths.add(
                    id_block, data_path, index=array_index,
                    group_method=group_method, group_name=group,
                )
                if keying_path is None:
                    raise RuntimeError(
                        "Blender could not add the selected path to the Keying Set")
                keying_path.use_entire_array = entire_array
                changed = True
                changed_path_count += 1
            else:
                for existing in matches:
                    keying_set.paths.remove(existing)
                    changed = True
                    changed_path_count += 1
    else:
        if absolute:
            paths_to_apply = [
                _keying_set_path_record(source_path)
                for source_path in keying_set.paths
            ]
        else:
            paths_to_apply = _relative_keying_set_paths(
                scene, keying_set, selected_source_object_ids,
                active_source_object_id, view_layer_name)
        frame, changed_path_count = _apply_keying_set_keyframes(
            scene, keying_set, paths_to_apply, insert_keyframes)
        changed = changed or changed_path_count > 0

    records, active_key, truncated = outliner_keying_set_records(scene)
    if not active_key:
        raise RuntimeError("Blender did not retain the active Keying Set")
    result = bpy.ops.wm.save_as_mainfile(
        filepath=os.path.abspath(staged_path),
        check_existing=False,
        compress=True,
    )
    if "FINISHED" not in result:
        raise RuntimeError("Blender did not finish saving the edited Keying Set")
    with open(report_path, "w", encoding="utf-8") as report_file:
        json.dump(
            {"scene_name": scene.name_full,
             "keying_sets": records,
             "active_keying_set_key": active_key,
             "keying_sets_truncated": truncated,
             "changed": changed,
             "frame": (float(scene.frame_current_final)
                       if edit_mode == "keyframes" else None),
             "changed_path_count": changed_path_count},
            report_file,
            ensure_ascii=False,
            separators=(",", ":"),
        )


def purge_project_orphans(source_path, staged_path, report_path):
    result = bpy.ops.wm.open_mainfile(
        filepath=os.path.abspath(source_path),
        load_ui=False,
        use_scripts=False,
    )
    if "FINISHED" not in result:
        raise RuntimeError("Blender did not finish opening the source project")
    if any(scene.get("classiCAD_format") == PROJECT_FORMAT
           for scene in bpy.data.scenes):
        raise RuntimeError(
            "Orphan Data Purge cannot edit a classiCAD-native Blender project")

    removed_count = bpy.data.orphans_purge(
        do_local_ids=True,
        do_linked_ids=True,
        do_recursive=True,
    )
    remaining_orphans = outliner_orphan_groups()
    result = bpy.ops.wm.save_as_mainfile(
        filepath=os.path.abspath(staged_path),
        check_existing=False,
        compress=True,
    )
    if "FINISHED" not in result:
        raise RuntimeError("Blender did not finish saving the purged project")
    with open(report_path, "w", encoding="utf-8") as report_file:
        json.dump(
            {"removed_count": int(removed_count), "orphans": remaining_orphans},
            report_file,
            ensure_ascii=False,
            separators=(",", ":"),
        )


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
    if operation == "purge" and len(arguments) == 4:
        purge_project_orphans(arguments[1], arguments[2], arguments[3])
        return
    if operation == "keying_set_paths" and len(arguments) == 5:
        edit_keying_set_paths(
            arguments[1], arguments[2], arguments[3], arguments[4])
        return
    raise RuntimeError("invalid classiCAD adapter arguments")


if __name__ == "__main__":
    try:
        main()
    except Exception:
        traceback.print_exc()
        sys.exit(2)
