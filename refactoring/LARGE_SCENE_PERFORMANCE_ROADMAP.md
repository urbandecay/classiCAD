# Large Scene Performance Roadmap

This document records likely ways to make large NURBS scenes faster. It is a
ranked investigation and implementation plan, not a claim that every item is
needed. Measure each change against the real scene before and after it.

## Current evidence and limits

- The running scene had 16,344 total objects after the latest logged duplicate
  operation. The log does not record geometry types, so the exact NURBS-only
  count is unknown.
- The current movement benchmark uses 512 synthetic capped-cylinder solids,
  not the user's full scene. On the development machine, native GL group-move
  frames measured 5.187 ms median and 5.946 ms p95. Timing includes model edit,
  render-frame creation, strokes, depth, grid, and `glFinish`; it excludes Qt
  overlays and event-loop latency.
- The current changes retain prepared geometry and GPU buffers through
  translation, apply draw-time offsets, avoid snapping moving selected objects
  against one another, simplify straight wire samples, and reduce display mesh
  work for affine flat caps and constant linear extrusion walls.
- Exact NURBS data remains authoritative. Triangle meshes and sampled lines are
  display and query proxies.
- The full CTest suite passed 11/11 after the movement changes. Those changes
  and the benchmark documentation are still local and uncommitted.

## Ranked opportunities

### 1. Batch document insertion and removal

**Expected impact:** High for large duplicate, delete, and import operations.
**Effort:** Low to medium.

`DuplicateCommand` inserts objects one at a time. Each call to
`Document::insertObject()` calls `rebuildLayerObjectIds()`, which walks every
object in the document. Duplicating 8,172 objects while the document grows
from 8,172 to 16,344 therefore visits about 100 million object records just
for those repeated layer-list rebuilds. Add a batch insertion/removal path, or
update layer membership incrementally, and rebuild indexes once per transaction
when needed.

**Implementation checkpoint (2026-10-04):** `Document::insertObjects()` and
`removeObjects()` now batch ordered changes and rebuild layer membership once;
Duplicate and Delete use the batch transaction APIs. `DocumentChangeSet` uses
hash membership to keep its public ordered changed-ID lists from quadratic
deduplication. A 16,344-object synthetic NURBS-curve run measured 3.232 s
repeated versus 25.586 ms batched insertion (126x), and 26.350 s repeated
versus 24.629 ms batched deletion (1,070x). These are transaction-plus-commit
document timings, without viewport drawing. They validate container scaling,
not performance on the user's surface/solid scene. A recent app session JSON
snapshot contained 16,344 objects (8,160 surfaces, 4,096 solids, 4,072
polygons, and 16 arcs). On that snapshot, repeated versus batched insertion
measured 3,487.17 ms versus 31.491 ms (110.7x), and deletion measured
25,871.3 ms versus 30.209 ms (856.4x). These measurements also cover only
document transactions and history commit, not viewport drawing or saving. The
full build passed, CTest passed 11/11 in 325.71 seconds, and `git diff --check`
passed.

The opt-in runtime harness supports saved `.vignola`/`.blend` files, app
session JSON snapshots, and a same-size synthetic fixture:

```sh
QT_QPA_PLATFORM=offscreen ./build/classicad_viewport_runtime_benchmark \
  --document-batch-only /path/to/scene.vignola
QT_QPA_PLATFORM=offscreen ./build/classicad_viewport_runtime_benchmark \
  --document-batch-synthetic-only 16344
```

Relevant code:

- `src/core/document/document.cpp`: `Document::insertObject()`,
  `Document::removeAt()`, and `Document::rebuildLayerObjectIds()`.
- `src/core/commands/duplicate_command.cpp`: repeated transaction insertion.

Add benchmark cases for duplicating and deleting 1k, 8k, and 16k objects.
Check object IDs, ordering, layer membership, rollback, and Undo/Redo.

### 2. Separate object placement from NURBS control points

**Expected impact:** Very high for moving large selections.
**Effort:** High; requires a persistent model and file-format contract.

Store NURBS geometry in object-local coordinates and give each scene object a
placement transform. A drag can update one preview transform and commit the
placement without rewriting every surface control point. The renderer can
apply the transform while drawing. This also removes the current need to scan
surface control points to detect a rigid translation after each geometry
revision.

Relevant code:

- `src/core/document/scene_object.h`: currently stores ID, layer, and geometry.
- `src/core/commands/transform_command.cpp`: applies edits object by object.
- `src/core/geometry/geometry_transform.cpp`: currently translates surface
  control points.
- `src/services/sampling/surface_tessellation_cache.cpp` and
  `src/ui/viewport/viewport_geometry_cache.cpp`: recognize translation and
  retain prepared geometry.
- Serialization, Rhino/openNURBS interchange, hit testing, snapping, and
  workplane mapping must all agree on the local-to-world transform.

Keep the first implementation narrowly scoped to translation if that can be
done without creating incompatible geometry paths. Define how rotation, scale,
reflection, and oriented workplanes compose before generalizing the transform.

### 3. Share repeated geometry and render instances

**Expected impact:** Very high for scenes made from many copies of a few shapes.
**Effort:** High.

Intern identical NURBS geometry and its prepared display proxy. Store each
object's transform, style, and selection state separately. Draw repeated
geometry with per-instance transforms, and copy the underlying geometry only
when an instance is edited independently. This avoids one CV array, tessellated
mesh, and sampled wireframe per visually identical duplicate.

This depends on a clear distinction between shared geometry identity and object
identity. Undo, serialization, export, and editing must preserve independent
object selection and allow safe copy-on-write edits.

### 4. Cull objects before preparing the viewport frame

**Expected impact:** High when many objects are outside the camera view.
**Effort:** Medium.

`buildViewportRenderFrame()` currently walks all visible layer objects and
copies their shape values into the frame. Add cheap object bounds and frustum
or screen-space culling before expensive surface preparation, depth building,
and hit testing. Keep conservative bounds so clipped or perspective cases do
not disappear incorrectly. Preserve a nearby-object path for snapping when the
cursor approaches the viewport edge.

Relevant code: `src/ui/viewport/viewport_render_frame.cpp` and
`src/ui/viewport_widget.cpp` (`viewportRenderFrame()`).

### 5. Update render-frame data incrementally

**Expected impact:** Medium to high in large, mostly static documents.
**Effort:** Medium to high.

The viewport constructs a fresh frame record for every visible object during
each frame. Retain object-to-frame records and update only objects affected by
geometry, layer, visibility, selection, or placement changes. Keep camera-only
state separate from geometry state. Avoid copying large geometry values into a
new frame when an immutable shared reference is sufficient.

Relevant code: `buildViewportRenderFrame()` and
`ViewportGeometryCache::prepareFrame()`.

### 6. Use screen-space tessellation levels

**Expected impact:** High for dense or distant surface scenes.
**Effort:** Medium.

The default surface proxy uses a 48 by 48 grid, eight isocurves per direction,
128 samples per isocurve, and 256 trim samples. These fixed settings can do
unnecessary work for small or distant objects. Choose tessellation from a
pixel-error target, reuse the current level while the camera barely moves, and
refine as the user zooms in. Preserve sufficient detail around trim boundaries
and keep exact NURBS and trim curves unchanged.

Already implemented special cases reduce work for convex affine caps and
constant linear extrusion walls. Retain those paths and add correctness tests
for concave trims, holes, reversed boundary orientation, and curved surfaces.

Relevant code: `src/core/geometry/nurbs_surface_tessellator.h/.cpp`.

### 7. Reduce per-object renderer work further

**Expected impact:** Medium to high; strongest with shared geometry.
**Effort:** Medium to high.

The renderer already keeps a combined depth buffer and merges adjacent ranges
with compatible offsets. Continue by grouping compatible strokes and display
meshes independent of document order where drawing order permits. Use a
per-instance transform/color buffer and instanced or indirect draws for repeated
geometry. Keep selected and highlighted styling in separate batches when that
avoids rebuilding base geometry.

Measure CPU frame construction, range scans, uniform updates, draw-call count,
buffer upload bytes, and GPU time separately. Do not assume fewer draw calls
help if tessellation or fill rate is the real limit.

### 8. Maintain spatial indexes for snapping and selection

**Expected impact:** High when snapping, picking, or box selection scans many
objects.
**Effort:** Medium to high.

The drag snap path now builds local screen-space buckets and avoids searching
objects that move with the selection. A persistent world-space bounds tree or
camera-aware index could avoid rebuilding candidate buckets and narrow-phase
queries for stationary geometry. Update only dirty bounds after object edits.
Use conservative bounds and retain exact NURBS narrow-phase checks for nearby
candidates.

Cover endpoint, midpoint, center, intersection, tangent, perpendicular, near,
surface corner, point pick, and crossing/window selection queries.

### 9. Make Undo store edits instead of full document states

**Expected impact:** Medium to high for memory, duplicate, move, and Undo/Redo
latency in large scenes.
**Effort:** High.

Transactions currently take document snapshots. Qt implicit sharing makes an
untouched snapshot relatively cheap, but edits to a large object collection or
many individual shapes can trigger detach/copy work and retain large before and
after states. Consider operation records, persistent/chunked object storage, or
copy-on-write geometry blocks. A translation should be recorded as a placement
delta once that model exists.

Measure commit, rollback, Undo, Redo, retained memory, and peak memory at the
real scene size. Keep transactions atomic and preserve current undo behavior.

### 10. Prepare expensive geometry asynchronously

**Expected impact:** Medium for initial load and edits to complex surfaces;
little benefit to already-cached translation.
**Effort:** Medium.

Move tessellation and other rebuildable display-proxy work to worker threads.
Pass immutable geometry snapshots and revision IDs; discard stale results if
an object changes while work is in progress. Show the previous proxy or a
coarser proxy until the new one is ready. Keep all model edits and OpenGL
resource creation on their appropriate owning threads.

### 11. Explore GPU evaluation of NURBS

**Expected impact:** Potentially high for dense, deforming surfaces.
**Effort:** Very high; experimental.

Evaluate NURBS spans in GPU shaders or hardware tessellation stages while
retaining exact control nets on the CPU. Trim loops, adaptive quality, picking,
OpenGL 3.3 compatibility, and differences between viewport and export geometry
make this a later research path. Compare it only after profiling shows CPU
tessellation is a dominant cost.

## Measurement plan

Before optimizing the full scene, add timing/counter boundaries for:

1. Input event to displayed frame latency and p50/p95/p99.
2. Model edits, transaction snapshots, and commit.
3. Render-frame collection and visibility culling.
4. Cache lookup, translation comparison, and tessellation.
5. CPU stroke/range construction and GPU buffer uploads.
6. Depth pass, stroke pass, grid/overlay pass, and total GPU frame.
7. Snap, hit-test, and box-selection queries.
8. Undo/Redo latency and memory after repeated large edits.

Benchmark 512, 2,048, 8,172, and 16,344 objects, plus scenes with a small
number of repeated shapes and scenes with unique complex surfaces. Include
selection movement, unselected movement, duplicate, select, snap, view orbit,
zoom, and first display after load. Run on the target GPU and resolution.

For a Blender comparison, use the same hardware, viewport size, camera,
shading/wire display, object placement, and equivalent NURBS display workload.
Record Blender version and whether duplicate objects share geometry. Compare
interactive frame timings and tail latency; do not compare ClassiCAD's 512
synthetic benchmark with unrelated Blender scene timings.

## Blender design references

- [Blender dependency graph](https://developer.blender.org/docs/features/core/depsgraph/):
  updates are propagated through affected scene dependencies.
- [Blender draw manager](https://developer.blender.org/docs/features/gpu/draw_manager/):
  draw calls are grouped by GPU batch and object resources are tracked
  separately.
- [Blender GPU batch example](https://github.com/blender/blender/blob/main/doc/python_api/examples/gpu.1.py):
  batches are reusable and should be cached.

These references describe architectural techniques; they are not a measured
Blender-versus-ClassiCAD performance result.
