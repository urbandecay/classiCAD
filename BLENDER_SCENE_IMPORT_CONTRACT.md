# Blender Scene Mesh Import Contract

## Purpose

This contract authorizes a native classiCAD representation for polygon meshes
imported from Blender. It enables arbitrary Blender scene objects to load into
classiCAD while keeping the existing NURBS curve, surface, and solid models
authoritative for their own geometry. It does not require copying Blender's
renderer or editor architecture.

The importer targets Blender 5.2.2 behavior and data first. A `.blend` file is
Blender's native database format, so import must run through a compatible
Blender Python runtime (or an equivalent supported Blender interchange API).
Do not parse `.blend` bytes with an ad hoc classiCAD parser. The importer must
state the Blender version used and report data it cannot represent.

## Scope

Import visible and hidden scene objects with stable object identity, world
transforms, mesh topology, object display color, material slots, per-face
material indices, UV maps, and color attributes. Preserve source object and
library names for Workbench Random colors. Import evaluated mesh geometry only
when the user selects an evaluated import mode; otherwise retain the original
mesh topology and object transform. The chosen mode is recorded in import
metadata.

This contract covers polygon meshes and their Workbench display inputs. It does
not define Blender node-graph/shader conversion, armatures, animation,
simulation caches, procedural geometry graphs, or editable Blender modifier
stacks. Those unsupported features must be identified in the import report,
not silently baked or dropped without notice. Existing supported NURBS curves,
surfaces, and solids continue through their exact classiCAD geometry paths.

## Geometry and topology representation

Add a distinct committed mesh geometry type (proposed `GeometryType::Mesh`)
backed by `Shape::PolygonMesh3D`. Do not encode imported polygon meshes as
NURBS surfaces, curve strokes, or a display-only tessellation cache. The mesh is
the authoritative geometry for an imported Blender mesh object; evaluated GPU
buffers and triangulation are derived data.

Store finite world-independent local-space XYZ vertex positions, polygon face
loops as ordered vertex-index lists, and one material-slot index per polygon.
Keep the object's affine transform, including translation, rotation, scale,
and supported shear, in its scene placement/transform data. Preserve face
winding. Do not weld vertices or merge objects during import. Triangulate only
for rendering or algorithms that require triangles; keep original polygon
loops intact. Validate indices, loop lengths, and finite coordinates before
committing an object.

Per-corner data belongs to face loops, not just vertices: UV coordinates and
color-attribute values must preserve Blender's corner-domain seams. Preserve
attribute name, domain, data type, and color-space interpretation. Keep vertex-
domain attributes indexed by vertices and face-domain attributes indexed by
polygons. Unsupported Blender attribute domains/types are retained as opaque
import metadata if feasible and listed in the import report; they must not be
misrepresented as corner or point data.

Custom split normals are stored one per face loop in polygon order. The Blender
bridge extracts loop normals for custom-normal meshes in both original and
evaluated modes; native JSON omits the array when absent, preserving older
documents. Rendering transforms these local normals with the inverse transpose
of the mesh object's linear transform, so nonuniform scale and reflections do
not distort their directions. Meshes without stored loop normals use derived
topology normals.
Do not change NURBS storage: `NurbsSurface3D` and `NurbsExtrusionSolid3D` remain
the source of truth for those object types, and their tessellated meshes remain
derived proxies.

## Material and identity representation

Represent material datablocks by stable imported material records, with a
document-level material table and references from object slots. Preserve
material display color, roughness, metallic, alpha, source name, and library
identity. Preserve each mesh polygon's slot assignment. Keep object display
color and material color separate so Workbench Object, Random, and Material
color modes select the correct source. Random identity uses Blender's object
name and linked-library path when present; do not replace it with ObjectId.

Preserve image-texture references, UV-map selection, and color-attribute
selection as explicit inputs to Workbench Texture and Attribute modes. The
first implementation may retain image paths/packed-image payload references
without decoding unsupported formats, but must show a clear unavailable-input
state rather than silently substituting gray. Shader nodes remain unsupported
under this contract; Workbench material display values are imported separately
from node graphs.

## Serialization and compatibility

Extend native JSON using a new document version. Keep versions 1 through 7
readable with existing defaults and no mesh/material-table assumptions. New
records must carry an explicit geometry discriminator and validated mesh,
attribute, material, and transform payloads. Keep imported Blender source
version, import mode, and source identity as metadata, not as substitutes for
classiCAD geometry. Preserve enough source metadata to issue a useful report on
reopen. Reject malformed required geometry atomically; optional unsupported
attributes may be omitted only with an import warning.

Round-trip native classiCAD documents through the `.blend` adapter without
changing existing NURBS fidelity. When writing classiCAD mesh objects into a
`.blend`, create Blender mesh datablocks from the authoritative polygon mesh;
never export a NURBS display proxy as if it were the original NURBS. Keep the
current NURBS-to-Blender proxy/export behavior explicitly marked as a proxy.

## Viewport and interaction behavior

Solid rendering, hit testing, selection, bounds, transforms, and duplication
must read the committed mesh geometry and its object transform. GPU vertex and
index buffers are caches. QPainter fallback may use a documented approximation
but must not mutate or replace mesh data. Mesh object identity remains separate
even when objects share Blender mesh datablocks.

Workbench Material, Object, Random, Attribute, and Texture color modes sample
the imported data using Blender's source and fallback rules. Material slots
select per-face material values. UV and color-attribute interpolation follows
the face-loop domain; seams must remain visible. Attribute mode falls back to
Material only when the selected attribute is absent, matching Blender's
behavior. Texture mode reports unavailable images/UV maps and uses Blender's
documented Material fallback when its required input is absent.

Imported meshes participate in object-level selection, snapping, and
transforms. Mesh component editing, topology-changing CAD commands, booleans,
and mesh extrusion are outside this contract unless separately specified.
Unsupported source objects remain separate import-report entries; do not
silently convert them into points or curves.

## Unsupported object policy

- Curves/surfaces with a supported exact NURBS mapping use the existing NURBS
  import contract; unsupported Blender curve kinds are reported.
- Meshes that fail validation are skipped as whole objects with a reason;
  partial topology is never committed.
- Cameras, lights, empties, collections, and instances are retained as
  supported scene metadata only when a matching classiCAD scene-object type
  exists. Otherwise report them and preserve their names/relationships in the
  import report; do not fabricate geometry.
- Modifiers and Geometry Nodes are not implicitly applied. Original mode
  imports the source mesh; evaluated mode explicitly bakes the evaluated mesh
  and reports that the result is no longer parametrically editable in Blender.
- Linked library paths and missing external files are reported. Never rewrite
  source paths silently.

An import operation is one undoable document transaction. Each imported mesh
receives the active CAD layer assignment required by the document model, while
its Blender collection links remain independent. Direct membership in multiple
collections, the collection parent hierarchy (including multiple parents),
global collection viewport/render hide flags, per-view-layer collection-instance
paths and exclusions/visibility, object viewport hide flags, per-view-layer
object hide state, and the object's `hide_select` selection lock are retained.
The first imported view layer controls viewport visibility, with visibility
resolved across all collection paths for each direct object membership. The
Collections panel controls global collection visibility separately from the
Layers panel. An active view-layer selector remains pending. The importer reports imported, skipped, baked,
and unsupported item counts with per-item reasons.

## Implementation phases

1. **Schema and validation — implemented:** `PolygonMesh3D`, validation,
   document version 12, shared material records and slot references, affine
   object transforms, active UV/color selectors, Blender version/import mode
   metadata, collections, view-layer visibility, and selection locks are serialized. Versions 1–11
   remain readable.
2. **Blender extraction — implemented:** File > Import > Blender Scene invokes
   a Blender 5.2.2 version-checked bridge. Original/evaluated mesh extraction,
   per-object validation, unsupported-feature reporting, and one undoable
   import transaction are wired. Tagged classiCAD NURBS display proxies are
   skipped. External image references persist with materials; packed image
   bytes up to 32 MiB per image, collection hierarchy/membership, and global
   object/collection hide state are persisted. Nonmesh object types are
   reported as unsupported.
3. **Basic viewport — implemented for object-level interaction:** mesh
   triangles are derived from authoritative polygon loops and rendered
   through the GPU Workbench surface renderer, with QPainter fallback, object
   selection, bounds, affine transforms, duplication, and vertex/edge-midpoint/
   center/near snapping. Mesh component editing is outside this contract.
4. **Workbench inputs — partially implemented:** per-face material slots,
   Material/Object/Random/Custom/Attribute color sources, and material alpha
   reach the GPU and QPainter paths. Point, face, and corner color attributes
   use their stored domains. Image-texture references persist with materials,
   and GPU Texture mode samples the active UV map per fragment for supported
   external images and packed image bytes up to 32 MiB per image. GPU sampling
   applies Blender's alpha-mode handling and cutoff. QPainter Texture mode
   samples external and packed images with affine per-triangle mapping and
   repeat wrapping, and applies PREMUL handling and the alpha cutoff; cutout
   interpolation, perspective correction, and filtering differ from the GPU
   path. Shader-node evaluation remains unsupported. UV maps and active
   selectors are retained as mesh data. Object viewport hide flags and global
   collection viewport hide flags affect viewport visibility; `hide_select`
   prevents imported objects from being selected or edited.
   Collection membership and hierarchy are shown in a Collections panel,
   separate from CAD layers. The first imported Blender view layer drives
   path-aware visibility; switching the active view layer remains pending.
5. **Round-trip and parity — pending review:** native JSON preserves the mesh
   data, but save/reopen and Blender 5.2.2 visual parity have not been checked
   as a comparison matrix. Blender mesh export from native classiCAD documents
   also remains pending; NURBS proxy tagging prevents proxy meshes from being
   mistaken for source geometry during scene import.

Do not mark arbitrary Blender scene import or full Workbench parity complete
until all phases applicable to the claimed feature set have passed review.
