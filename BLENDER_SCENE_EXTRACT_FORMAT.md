# Blender Scene Extraction JSON

`src/core/serialization/blender_scene_extract.py` runs inside Blender 5.2.2 and
emits `classiCAD.blender-scene-extract`, format version `8`. This is a transport
format for the native importer; it is not classiCAD's saved document schema.

## Mesh object records

Each entry in `items` represents one mesh object occurrence in one Blender
scene. `object_id` is the Blender object name paired with its linked-library
path; `library_path` itself is preserved as Blender reports it. `transform_world`
is a row-major 4 by 4 world matrix. `vertices_local` contains local-space XYZ
positions, so the native importer must retain the transform separately.
`classiCAD_proxy_geometry` carries the classiCAD custom property used to mark
NURBS display proxies; these are reported and skipped as source mesh geometry.

`polygons` is an array of ordered vertex-index loops, preserving face winding.
`polygon_material_indices` has one slot index per polygon. `loop_vertex_indices`
lists each polygon-corner loop's vertex index in Blender's mesh-loop order.
Each UV layer and each `CORNER` attribute's values use that same loop order;
`POINT` attributes follow `vertices_local`, and `FACE` attributes follow
`polygons`. Polygon loops remain authoritative; triangulation belongs to later
rendering or geometry algorithms.

Material slots refer to entries in the document-level `materials` map by
`material_id`. Material records retain Blender source identity, diffuse color
in linear RGBA, roughness, metallic, alpha, and image-texture paths. Image paths
are not rewritten. UV layers include their active flags and the object's active
UV layer name. Color attributes retain their Blender name, domain, data type,
and color-space interpretation; unsupported attribute payloads are `null` and
have a matching entry in `unsupported_features`.

## Import mode and unsupported data

`source.import_mode` is either `original` or `evaluated`. Original mode exports
the source mesh and transform without applying modifiers. Evaluated mode exports
the evaluated mesh and transform and marks `evaluated_geometry_baked`; it also
reports the loss of parametric modifier editability.

`unsupported_items` contains objects without a matching mesh record, including
their identity, scene, parent, collections, visibility, and reason. A malformed
mesh is skipped whole and receives a reason there. `unsupported_features` is
per-item and covers data such as custom split normals, unsupported attributes,
missing linked libraries/images, unapplied modifiers, and shader-node graphs.
The importer validates required topology and finite coordinates before
committing any mesh object. Image paths are retained as material source
references in the native scene model, but image pixels are not embedded and
Blender shader-node graphs are not evaluated. Collection membership and hidden
state are reported but are not currently retained.
`summary` contains extraction counts only; it does not claim successful native
import.

## Curve object records

Supported Blender `CURVE` objects retain one item per source object. Each entry
in `splines` has an explicit `type`. `POLY` records contain
`control_points_world`; cyclic polylines repeat their first point at the end.
`BEZIER` records contain `spans_world`, with each cubic span ordered as start
point, outgoing handle, incoming handle, end point. Cyclic Bezier splines include
the final-to-first span. Every point is transformed by the source object's
`matrix_world` during extraction. The native importer stores each POLY spline
as a degree-one NURBS and each Bezier span as a separate nonrational clamped
degree-three NURBS component. Blender NURBS splines remain explicitly
unsupported. Bevel, extrude, bevel-object, and 2D fill surfaces remain omitted.
