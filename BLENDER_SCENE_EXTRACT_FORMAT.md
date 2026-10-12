# Blender Scene Extraction JSON

`src/core/serialization/blender_scene_extract.py` runs inside Blender 5.2.2 and
emits `classiCAD.blender-scene-extract`, format version `10`. This is a transport
format for the native importer; it is not classiCAD's saved document schema.

## Mesh object records

Each entry in `items` represents one mesh object occurrence in one Blender
scene. `object_id` is the Blender object name paired with its linked-library
path; `library_path` itself is preserved as Blender reports it. `transform_world`
is a row-major 4 by 4 world matrix. `vertices_local` contains local-space XYZ
positions, so the native importer must retain the transform separately.
`classiCAD_proxy_geometry` carries the classiCAD custom property used to mark
NURBS display proxies; these are reported and skipped as source mesh geometry.
In evaluated mode, Blender `FONT` objects use the same mesh record after
`Object.to_mesh()` evaluation; `source_object_type` is `FONT`, and
`evaluated_geometry_baked` records that the output is baked geometry. Original
mode reports FONT objects as unsupported because the native model does not
retain editable text.

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
shape keys, missing linked libraries/images, unapplied modifiers, and
shader-node graphs. Meshes also report `loose_geometry` when they contain edges
not used by polygon boundaries or isolated vertices; the native polygon mesh
stores polygon faces only, so those elements are omitted.
Supported mesh, curve, and surface child objects also report
`parent_hierarchy`: their world-space geometry/transform is retained, but the
Blender parent relationship itself is not represented in the native scene
model.
The importer validates required topology and finite coordinates before
committing any mesh object. Image paths are retained as material source
references in the native scene model; available packed image bytes within the
transport limit are also retained. Blender shader-node graphs are not evaluated.
Collection membership, hierarchy, and view-layer visibility are transported
and retained by the native importer.
`summary` contains extraction counts only; it does not claim successful native
import. `font_objects_meshed` counts evaluated FONT objects included in
`mesh_items_extracted`.

## Curve object records

Supported Blender `CURVE` objects retain one item per source object. Each entry
in `splines` has an explicit `type`. `POLY` records contain
`control_points_world`; cyclic polylines repeat their first point at the end.
`BEZIER` records contain `spans_world`, with each cubic span ordered as start
point, outgoing handle, incoming handle, end point. Cyclic Bezier splines include
the final-to-first span. `NURBS` records contain `degree`, Euclidean
`control_points_world`, per-point `weights`, a `rational` flag, and reduced
Rhino/openNURBS `knots`. Blender's generated full knot vector is reconstructed
from its order, cyclic, endpoint, and Bezier settings using the pinned 5.2.2
kernel rules, then its two redundant outer knots are removed. Cyclic NURBS
records append the first `order - 1` control points and weights to represent
Blender's wrapped evaluation net. Every point is transformed by the source
object's `matrix_world` during extraction; weights are not transformed. The
native importer stores these as dimension-3 NURBS components, preserving the
source knot domain and rational data. The native importer stores each POLY
spline as a degree-one NURBS and each Bezier span as a separate nonrational
clamped degree-three NURBS component. Bevel, extrude, bevel-object, and 2D fill
surfaces remain omitted.

## Surface object records

Each supported Blender `SURFACE` NURBS spline is emitted as a separate
`object_type: SURFACE` item. `object_id` combines the source Blender object ID
with the spline index so separate patches remain separate scene objects;
`source_object_id` retains the owning Blender object identity. `surface`
contains U/V degrees and orders, V-fastest control-net dimensions, flattened
world-space Euclidean control points, their weights, the rational flag, and
independent reduced Rhino/openNURBS `knots_u` and `knots_v`. The extractor
reconstructs each full Blender knot vector from the pinned 5.2.2 flags, expands
cyclic U/V control points by `order - 1`, and removes the two redundant outer
knots in each direction. Blender stores surface points U-fastest; extraction
reorders them to classiCAD's required V-fastest layout. Object transforms are
baked into the control points exactly once. The native importer validates each
patch and stores it as an exact `NurbsSurface3D`; display tessellation remains
derived data. `material_id` identifies the material assigned to that spline;
its Workbench display values are retained. Unsupported spline types are
reported per patch. FONT objects remain unsupported.
