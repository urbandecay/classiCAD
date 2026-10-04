# classiCAD scalable refactor — living master prompt

This is the long-term refactoring contract for classiCAD. Read this entire file before every refactoring iteration. Read the repository `AGENTS.md` first because its geometry and workflow rules are mandatory. Do not treat this document as a one-time plan: update the progress section and the architecture map whenever the implementation changes.

The purpose of this file is twofold:

1. It tells the coding agent what architecture to build.
2. It makes the project understandable to a human looking at the folders and filenames.

The current execution plan for completing ownership migration, sharing compiled
modules, and measuring build/runtime improvements is
[`MODULARITY_AND_BUILD_REFACTOR_PLAN.md`](MODULARITY_AND_BUILD_REFACTOR_PLAN.md).
Read it after this contract before beginning that work. Its R0–R12 phases
continue this ledger; historical completed phases remain historical records.

## Mission

Refactor classiCAD into a scalable C++/Qt application that can support many more drawing, editing, document, layer, import/export, and automation features without putting all behavior in the viewport widget or the main window.

Preserve the behavior that already works. Refactoring is not permission to silently change arc behavior, snapping, NURBS representation, selection semantics, trim/erase behavior, joining, exploding, rotating, subdivision, update-session restoration, or keyboard/mouse controls. If a behavior must change, identify it explicitly, add or update a regression test, and describe the change.

The end state should make these questions answerable by looking at a path:

- Where is persistent geometry stored? `src/core/geometry/`
- Where is the document and layer tree stored? `src/core/document/`
- Where is undo/redo stored? `src/core/history/`
- Where is snapping or hit-testing calculated? `src/services/`
- Where is the Line or Arc interaction implemented? `src/tools/`
- Where is drawing and Qt event routing implemented? `src/ui/viewport/`
- Where are menus and application controls implemented? `src/ui/`

## Current baseline

The first structural split has already been made:

```text
src/main.cpp                       application entry point, the only source compiled directly into the executable
src/app/application_session.*      shared document, selection, history, tool-registry ownership, and change notifications
src/app/command_router.*           stable application command IDs mapped onto typed operation handlers
src/app/project_controller.*       candidate-based project open/save, document replacement, and Rhino import transactions
src/app/session_serializer.*        versioned update-session JSON and viewport camera/workplane codecs
src/app/update_controller.*         rebuild/relaunch process and window-state handoff orchestration
src/core/tool_id.*                 active interaction vocabulary and tool metadata
src/core/geometry/geometry_type.*  persistent geometry vocabulary and legacy mapping
src/core/geometry/arc_mode.*      arc construction mode and display name
src/core/geometry/construction_modes.h ellipse, rectangle, and polygon factory modes
src/core/geometry/curve_geometry_data.* shared rational curve-operation values
src/core/geometry/curve_construction.* exact curve and planar primitive factories
src/core/geometry/planar_geometry.* planar vector and segment-intersection helpers
src/core/geometry/point_text.*    formatted local point coordinates
src/core/geometry/shape_mapping.* Shape/component workplane mapping, boundary helpers, and NURBS component extraction
src/core/geometry/nurbs_curve.*    shared NURBS storage, knot expansion, validation
src/core/geometry/nurbs_surface.*  tensor-product 3D NURBS surface storage, UV trim loops, validation, and evaluation
src/core/geometry/nurbs_surface_evaluator.* validated prepared surface state for repeated evaluation
src/core/geometry/nurbs_surface_factory.* exact ruled extrusion and planar Fill with UV trim boundaries
src/core/geometry/arc_curve_factory.* signed direction and exact rational NURBS construction for three-point arcs
src/core/geometry/interpolating_curve_factory.* centripetal Catmull-Rom interpolation represented as exact piecewise cubic NURBS
src/core/geometry/work_plane.*     principal and oriented local-2D to world-3D frame mapping
src/core/geometry/curve_evaluator.* NURBS evaluation, endpoint evaluation, and parameter-domain operations
src/core/geometry/curve_editing.* exact rational Bezier span splitting, reversal, and parameter-domain-preserving NURBS trims
src/core/geometry/curve_erase_intervals.* intersection-bounded intervals, exact complement splitting, and frame-preserving fragment rebuilds
src/core/geometry/curve_intersections.* planar NURBS curve/curve intersection parameters and closest curve-to-point search
src/services/erase/curve_erase_query.* reusable visible-scene intersection candidates, NURBS point/curve contacts, screen-space proximity, box intervals, and eraser stroke intervals
src/services/erase/trim_erase_query.* Trim/Erase replacement calculation from screen input, intersection results, and exact curve-fragment geometry
src/core/geometry/curve_join.* connected-curve grouping, planar and world-frame NURBS ordering, overlap fusion, and continuity/gap handling
src/core/geometry/curve_subdivision.* equal arc-length NURBS parameter selection for subdivision markers
src/core/geometry/surface_trim_region.* prepared sampled UV trim loops and shared outer/hole containment
src/core/geometry/nurbs_surface_tessellator.* shared trimmed wireframe, visible triangles, and Blender display proxy data
benchmarks/nurbs_surface_benchmark.cpp opt-in evaluator, tessellation, and cache microbenchmark
benchmarks/viewport_query_benchmark.cpp opt-in whole-scene curve hit-test and selection-box microbenchmark
benchmarks/viewport_runtime_benchmark.cpp opt-in snap/pick, redraw, per-object cache-invalidation, and snapshot-history benchmark
src/core/geometry/geometry_transform.* world-plane-aware translation, reflection, uniform/one-axis scaling, and world-axis rotation transforms
src/core/document/object_id.h      stable scene-object identity value type
src/core/document/layer_id.h       stable layer identity value type
src/core/document/layer.h          layer record, visibility, locking, and object membership
src/core/document/dimension_anchor.h persistent dimension-to-geometry references
src/core/document/shape.h          persistent shape data, curves, surfaces, and workplane placement
src/core/document/scene_object.h   persistent object identity, layer, and Shape payload
src/core/document/document.*       document-owned scene objects, layers, IDs, and snapshots
src/core/document/document_settings.* persistent document display units and grid spacing
src/core/document/document_change_set.h changed object/layer IDs and invalidation categories for one edit
src/core/document/selection_model.* selected object and control-point references
src/core/history/history.*          document-level snapshot undo/redo ownership
src/core/history/document_transaction.* atomic snapshot-backed edits and change reporting
src/core/commands/delete_command.*   transactional deletion of editable objects
src/core/commands/duplicate_command.* source-copy planning and anchor remapping
src/core/commands/explode_command.* PolyCurve component extraction
src/core/commands/fill_command.*    planar Fill plan and transactional surface insertion
src/core/commands/join_command.*    joined-object plan construction and atomic source replacement
src/core/commands/layer_command.*   transactional layer and membership edits
src/core/commands/mirror_command.*  mirrored copy creation through shared geometry transforms
src/core/commands/subdivision_command.* subdivision-marker document edits
src/core/commands/trim_erase_command.* atomic replacement, removal, and fragment insertion for Trim/Erase commits
src/core/commands/transform_command.* shared transform edit routing for editable objects
src/core/serialization/document_serializer.* versioned classiCAD document/layer/object snapshot
src/core/serialization/shape_json_codec.* Shape, curve, surface, and point JSON codec
src/core/serialization/blender_project_file.* `.vignola`/`.blend` save/open through the pinned Blender 5.2.2 runtime
src/core/serialization/blender_project_adapter.py Blender-native collections, Curve datablocks, and document Text datablock
src/core/serialization/rhino3dm_interchange.* separate Rhino/openNURBS `.3dm` import with oriented-plane lifting
src/core/model.h                   forwarding compatibility umbrella; production callers use owning contracts
src/core/debug_log.*               application logging
src/services/viewport/viewport_transform.* quaternion 3D camera projection, ray/frame picking, presets, zoom, pan, and Blender-style turntable/trackball orbit math
src/services/sampling/curve_sampler.* NURBS display/erase sampling and scene cache generation
src/services/sampling/curve_sample_data.h sampled NURBS and erase-cache values
src/services/sampling/surface_tessellation_cache.* bounded ObjectId/revision cache for prepared surface display data
src/services/hit_testing/curve_hit_tester.* curve/control-point hit-testing, drawing-plane inheritance, and cross-workplane orbit-depth picking
src/services/hit_testing/projected_curve_bounds.* conservative perspective-aware projection of positive-weight NURBS control hulls
src/services/hit_testing/selection_box_query.* sampled NURBS/point box queries, projected fallback bounds, and camera clipping
src/services/snapping/snap_engine.* endpoint, midpoint, center, intersection, perpendicular, tangent, and projected-hull-filtered near snapping
src/services/snapping/snap_types.* snap result, candidate, drag-result contracts, and labels
src/tools/tool.*                  non-Qt interaction lifecycle, typed handled/unhandled dispatch results, and preview/status values
src/tools/tool_input.h            translated mouse, wheel, and keyboard input payload with the active workplane frame
src/tools/tool_context.*          document, history, services, factory, transaction, selection/layer notification, and preview ports
src/tools/tool_registry.*         active tool module lookup and ownership
src/tools/shape_creation_tool.*   shared pending-point creation lifecycle
src/tools/select_tool.*            click/Shift-click selection transitions, Select All, box and drag state
src/tools/point_tool.*             point creation
src/tools/point_construction_tool.* point chains, circular-arc intersection points, curve centers, and curve-span centers
src/tools/line_tool.*              world-space connected line input and planar NURBS run creation
src/tools/tangent_from_curve_tool.* tangent line creation from a selected NURBS curve
src/tools/perpendicular_from_curve_tool.* perpendicular line creation from a selected NURBS curve or straight span
src/tools/two_curve_line_tool.*    cursor-selected common tangent/perpendicular lines between two coplanar NURBS curves
src/tools/curve_drawing_tool.*     interpolated and freehand cubic NURBS drawing
src/tools/rectangle_tool.*         rectangle creation
src/tools/circle_tool.*            rational circle creation
src/tools/ellipse_tool.*           four-mode rational ellipse creation and staged preview
src/tools/polygon_tool.*           four-mode regular polygon construction, dimension entry, and closed degree-1 NURBS preview/commit
src/tools/arc_tool.*               Arc mode/state reset, frame capture, canonical staged points, click/key/axis stage decisions, unit-aware numeric input, preview math, chord solving, endpoint axis inference/projection, chord-length completion, vertical hysteresis, chord-plane construction, commit construction, and planar endpoint constraints; screen coordinates and final cursor/snap presentation remain in the viewport
src/tools/bezier_tool.*            Bezier creation
src/tools/nurbs_tool.*             NURBS creation
src/tools/point_extrude_tool.*     unified Extrude: points to edges, curves to ruled NURBS surfaces
src/tools/circle_tangent_tool.*   circle construction tangent to selected NURBS curves
src/tools/grab_tool.*              base-point move state and rollback snapshot lifecycle
src/tools/duplicate_tool.*         interactive duplicate source/base/destination and preview state
src/tools/join_tool.*              interactive Join input, curve planning, command transaction, selection result, and prompt
src/tools/subdivision_tool.*       section/wheel/preview state, translated wheel/key/click decisions, and command transaction
src/tools/rotate_tool.*             rotate pivot/reference/final-point transitions, axis/perpendicular-plane changes, keyboard input, angular preview/snap, typed-angle calculations, and transform commit
src/tools/mirror_tool.*             source selection, two-point mirror-axis lifecycle, typed second-click commit, and mirror command transaction
src/tools/scale_tool.*              scale stages, typed mouse/key dispatch and commit, preview/factor calculations, and transform commit
src/tools/trim_tool.*               Trim box lifecycle, hover/component choice, box candidates, and click/box commit intent values
src/tools/erase_tool.*              Erase stroke/cursor lifecycle and sampled screen-path candidate accumulation
src/ui/input_helpers.*             Qt event-position and icon helpers
src/ui/input/tool_input_translator.* Qt mouse/key/wheel fields and resolved cursor values translated into ToolInput
src/ui/viewport_widget_api.h       typed viewport settings, command, status, and callback boundary
src/ui/viewport_widget.cpp         Qt event routing, camera-dependent cursor/snap acquisition, render-frame assembly, and viewport presentation adapters; borrows ApplicationSession
src/ui/panels/document_grid_dialog.* document length-unit and base-grid-spacing dialog values
src/ui/panels/layer_style_widgets.* shared layer-linetype combo, icons, and descriptions
src/ui/panels/layers_panel.*         layer table, filtering, stable-ID selection, and typed edit callbacks
src/ui/panels/preferences_dialog.* preference pages and typed values returned to MainWindow
src/ui/panels/preferences_store.*   typed application preference loading, migration, and QSettings persistence
src/ui/panels/tool_shelf.*          tool-button layout, exclusive tool selection, and local scroll/help presentation
src/ui/theme/classicad_theme.*      application-wide Qt stylesheet
src/ui/viewport/navigation_controller.* Qt navigation gestures, preset animation, and navigation gizmo routing
src/ui/viewport/navigation_gizmo.*     navigation gizmo drawing, action hit-testing, and navigation action values
src/ui/viewport/viewport_arc_compass_renderer.* workplane-projected Arc/Rotate compass presentation
src/ui/viewport/viewport_hud_renderer.* typed tool-status and instruction HUD presentation
src/ui/viewport/viewport_snap_marker_renderer.* snap marker glyphs and label visibility
src/ui/viewport/viewport_erase_overlay_renderer.* erase cursor and candidate preview drawing
src/ui/viewport/viewport_tool_preview_renderer.* geometry, guide, and transform previews for drawing tools
src/ui/viewport/blender_grid_renderer.* 3D grid shader setup, offscreen/on-screen contexts, camera uniforms, and procedural GPU drawing
src/ui/viewport/viewport_gpu_surface.* native QOpenGLWidget presentation surface and renderer lifetime
src/ui/viewport/viewport_scene_renderer.* GPU committed-curve strokes, linetype/style resolution, and dashed control guides
src/ui/viewport/viewport_render_frame.* immutable camera, visible scene geometry, layer styling, selection, transform previews, revisions, and active ToolPreview snapshot
src/ui/viewport/viewport_geometry_cache.* revision-keyed immutable double-precision world geometry reused by GPU strokes, depth, and CPU fallback
src/ui/viewport/blender_grid_scale.*     Blender-compatible viewport grid step ladder and view-dependent LOD selection
src/ui/viewport/blender_grid_frame.*     Blender-compatible visual grid plane, camera-relative origin, orthographic distance, and global-axis mapping
src/ui/viewport/blender_grid_appearance.* shared theme colors, opacity, stipple, and camera-fade settings for GPU/Qt grid paths
src/ui/viewport/viewport_depth_geometry.* sampled scene curves, points, and picture planes for the GPU depth prepass
src/ui/viewport/shaders/blender_grid.* adapted Blender grid vertex/fragment shaders
src/ui/viewport/shaders/scene_depth.*     depth-only scene geometry shader pair
src/ui/viewport/shaders/scene_stroke.*    antialiased GPU scene stroke shader set
src/ui/viewport/shaders/scene_point.*     antialiased GPU point marker shader pair
src/ui/viewport/viewport_renderer.* committed-geometry projection, CPU grid fallback, axes, control-point, and subdivision drawing
src/ui/viewport/viewport_overlay.*  snap-marker/tool-preview coordination, generic selection box, and control-point/subdivision delegation
src/ui/main_window.*               menus, panel composition, setting application, and window wiring
cmake/ClassicadTargets.cmake       shared target configuration, unused Qt codegen exclusion, and object-module assembly
cmake/check_dependencies.py        downward-include and implementation-include audit
CMakePresets.json                  isolated app-only, full-test, and benchmark build configurations
tests/trim_seam.cpp                geometry, query, tool, command, selection-box, and update-session contract coverage without implementation includes
tests/core_contracts.cpp            vocabulary, ID, NURBS, session, camera, and orbit-math compatibility coverage
tests/viewport_render_contracts.cpp layer-line patterns and committed GPU stroke-style contract coverage
tests/viewport_interaction.cpp      viewport mouse/wheel and GPU/fallback interaction coverage
```

The current split is useful, and phases 1 through 10 now give the remaining
modules explicit vocabulary, validation, document-ownership, history,
selection, reusable-service, tool-lifecycle, and viewport-rendering contracts.
`src/ui/viewport_widget.cpp` is still a large implementation module. It owns
Qt event routing, screen/camera-dependent cursor and snap acquisition, frame
assembly, viewport-setting application, presentation adaptation, and several
temporary compatibility helpers; tool state, document transactions,
persistence, and shared geometry algorithms have named owners.
3D grid rendering now delegates to `BlenderGridRenderer`; the CPU grid remains
a context/shader fallback. Geometry drawing delegates to `ViewportRenderer`,
while `ViewportToolPreviewRenderer` paints geometry, guide, and Rotate previews;
`ViewportOverlay` coordinates snap-marker/tool-preview renderers and delegates
control-point/subdivision drawing. Select and Trim share the same generic
selection-rectangle drawing, so Trim has no separate box-painting responsibility
to extract. `ViewportEraseOverlayRenderer` paints erase cursor and candidate
previews. Snap markers use `ViewportSnapMarkerRenderer`. Tool-status and instruction HUD text delegates to
`ViewportHudRenderer`. The workplane-projected Arc/Rotate compass delegates to
`ViewportArcCompassRenderer`. World/screen
conversion, NURBS evaluation and sampling, scene erase-cache generation,
snapping, hit-testing, and the point/line/rectangle/circle/polygon/Bezier/NURBS
creation paths now delegate to named core/service/tool modules. The
`ViewportRenderFrame` value snapshots camera and viewport size, visible scene
geometry, object IDs/revisions, layer style, selection/highlight state, and the
active `ToolPreview`. GPU drawing, depth picking, and control-point display use
the same frame entries; Scale/Rotate preview geometry is prepared once. The
`ViewportGeometryCache` prepares camera-independent double-precision
world-space geometry once per visible object revision. GPU strokes, the depth
pass, and the CPU fallback reuse that frame-owned value. CPU projection applies
bounded screen-space simplification and retains the adaptive renderer for
clipped or unsupported paths. The 1,024-curve synthetic fallback remains much
slower than native GL, as recorded in the runtime baseline.
Transformed preview geometry remains transient. The
viewport now stores one active preview value instead of per-tool copies for
Line, point construction, Extrude, creation geometry, guides, and HUD text.
`ViewportHudRenderer` owns tool-status and instruction text/panel painting from
a typed state snapshot. `ViewportNavigationGizmo` owns navigation-gizmo drawing
and hit-testing. `ViewportArcCompassRenderer` owns the projected Arc/Rotate
compass. `ViewportToolPreviewRenderer` owns line, Arc, circle, ellipse,
rectangle, polygon, point, and Rotate preview drawing from inputs supplied by
the widget. `ViewportEraseOverlayRenderer` owns erase cursor and candidate
previews; trim overlay and scene-query work remain for R10 follow-up.
The four ellipse construction modes use a dedicated interaction tool with oriented
plane capture, add-on-style numeric controls, and exact rational NURBS
previews. The four polygon modes now use a dedicated staged controller with
the add-on's center/corner, center/tangent, corner/corner, and edge
constructions, side-count wheel and numeric controls, and closed degree-1
NURBS for previews and committed shapes. Every
supported planar creation tool captures a `WorkPlaneFrame` before accepting
its first point; previews and committed curve or point data keep using that
frame. Connected Line remains the spatial exception and splits its world-space
chain into planar NURBS runs. The
`ViewportWidgetApi` exposes typed edit commands and callback registration so
`MainWindow` routes menu/tool actions without reaching into viewport mutation
methods or callback storage. `ToolShelf` owns the tool-button layout, selection
group, scroll behavior, and help display; the main window supplies action and
popup-menu callbacks. `LayersPanel` owns the layer table, filtering, row
selection, and local enablement; its stable-ID callbacks return layer edits to
MainWindow, which retains dialogs, command/status handling, and the top layer
properties toolbar. Shared linetype controls are in `layer_style_widgets.*`.
`ApplicationSession` owns the active Document,
SelectionModel, History, and ToolRegistry and publishes document/history/layer
change notifications. `ProjectController` validates project candidates before
replacement and records Rhino imports as one history operation.
`CommandRouter` maps typed application command IDs to registered operation
handlers. `SelectTool` owns click/Shift-click selection transitions, Select All,
selection box state, drag targets, and control-point drag state. SelectionModel
exposes read-only object and primary-ID access; edits use its mutation methods.
The viewport still supplies ordinary selection hit testing, geometry updates,
and selection-box presentation; `selection_box_query.*` owns curve/point box
intersections and the projected-bounds fallback for other shapes. Generic tool
routing uses explicit handled/unhandled results and reuses one translated event
payload. The public event-precedence suite covers modal Join/Duplicate priority,
Select All, Fill/Delete shortcuts, and Arc Escape; screen/camera-dependent
acquisition and presentation adaptation remain in the widget. `NavigationController` owns Qt camera gestures,
gizmo action routing, and preset animation; `ViewportNavigationGizmo` owns
gizmo drawing and hit-testing, while `ViewportTransform` keeps camera math.
`tests/core_contracts.cpp` now targets the navigation-gizmo module directly;
`ViewportOverlay` has no navigation-gizmo compatibility methods. `GrabTool` and
`DuplicateTool` own gesture state and cancellation
snapshots, while the viewport supplies scene picking and duplicate preview
geometry. `DuplicateCommand` validates and inserts copies atomically through
`DocumentTransaction`.
`SessionSerializer` owns update-session JSON and camera/workplane codecs while
the viewport applies validated view state. `UpdateController` writes the window
handoff, builds, and relaunches; `MainWindow` presents status and applies window
geometry. MainWindow owns menus, action wiring, application-level dialogs,
file workflow settings, and applying accepted preferences. `preferences_store.*`
owns preference key persistence and saved-palette migration;
`PreferencesDialog` owns preference-page construction and returns typed values,
while `classicad_theme.*` owns the application-wide stylesheet and LayersPanel and ToolShelf own their
presentation state. MainWindow contains no geometry algorithms. The viewport keeps temporary read-only views while
borrowing authoritative document and selection state. Generic tool events use
one translated payload per route and explicit handling results; modal event
precedence is covered by `viewport_event_precedence` and the public interaction
suite.
`Document` now provides O(1) ObjectId lookup, per-object geometry revisions, and
document runtime revision epochs. `DocumentTransaction` wraps snapshot History
with affected IDs/categories and rollback. Tool commits, settings, duplicate,
Join, Explode, Fill, Delete, Mirror, Scale, Rotate, and multi-object Trim/Erase
use that transaction boundary. Object, shape, and layer accessors are read-only;
live drag and associative-dimension edits go through a revision-reporting
geometry callback. Geometry edits advance revisions used by the current
revision-keyed caches. R5a extracts exact rational curve editing and common UV trim
containment into `curve_editing.*` and `surface_trim_region.*`; viewport
rotation and scaling use shared geometry transforms. `nurbs_surface_tessellator.*`
supplies the clipped wireframe and visible triangles consumed by viewport
display, depth, hit testing, and Blender proxy generation. Proxy mesh data is
transient; the exact surface and UV trim curves remain authoritative.
The duplicate pre-delegation
rendering, hit-testing, and NURBS-evaluation helper bodies have been removed
after the extracted modules were verified as the only live implementations.
NURBS endpoint evaluation now has one implementation in `curve_evaluator.*`,
and exact curve reversal has one implementation in `curve_editing.*`; Join uses
these shared operations instead of maintaining private copies.
Connected planar and world-frame component ordering, degree-1 overlap fusion,
continuity checks, and tolerance-based gap adjustment now live in
`curve_join.*`. The viewport supplies its view-derived tolerance and retains
Join selection, command orchestration, and status presentation.
Arc, Rotate, Mirror, Scale, Trim, and Erase have named lifecycle modules registered.
ArcTool owns the Arc mode and interaction-state record, canonical staged input
points, input reset and initial workplane capture, numeric-input buffer,
radius/angle/sagitta preview math, chord-length endpoint solving and stage
completion, angle-snap and plane-lock toggles, one-point sweep unwrapping,
numeric key/stage decisions, unit-aware radius/angle/chord/sagitta input,
one/two-point planar endpoint constraints, projected world-axis inference and
projection through the shared input constraint service, staged click
transitions, right-click completion/cancellation, XYZ constraint state,
perpendicular-plane transitions, vertical hysteresis, chord workplane
construction, and commit construction through ToolContext. ViewportTransform
supplies the camera projection and view direction. The viewport mirrors Arc
points for shared preview presentation, supplies screen coordinates and snap
results, applies returned cursor/snap presentation state, and refreshes the
cursor after frame changes. RotateTool owns pivot/reference/final-point stage
acceptance, typed key and right-click decisions, axis/perpendicular-plane
changes, angular preview/snap, typed-angle calculations, transform commit, and
state reset; the viewport applies returned cursor/frame updates and configures
the transaction commit port. MirrorTool owns source IDs, its two-point axis,
typed axis commit, Escape/right-click cancellation, and the MirrorCommand transaction.
ScaleTool owns staged point/factor transitions, typed mouse/key dispatch,
selection-center acceptance, Escape/right-click reset, preview calculations,
commit decisions, and prompt text; the viewport still resolves the
view-dependent selection-center geometry. JoinTool owns target validation,
selection, curve planning, JoinCommand execution, transaction, and selection
replacement; the viewport supplies scene picking and view-derived endpoint
tolerance. SubdivisionTool owns section/wheel state, cached equal-arc-length
marker parameters, key/click decisions, and the SubdivisionCommand
transaction; the viewport draws those markers. The parameter solver lives in
`curve_subdivision.*`, while curve ordering, fusion, continuity, and gap
algorithms live in `curve_join.*`. Qt event handling remains in the adapter.
`TrimTool` owns box/hover candidate decisions and preview state; `EraseTool`
owns stroke state and candidate accumulation. Geometry rebuilding, scene
intersection candidates, and screen-interval calculations live in
`curve_erase_intervals.*`, `curve_erase_query.*`, and `trim_erase_query.*`;
`TrimEraseCommand` applies atomic document edits. The widget still supplies
camera-dependent cursor/scene query inputs and presentation callbacks.
SelectTool owns selection transitions and drag/box state, while viewport hit
testing, geometry callbacks, and drawing remain in the view adapter. Erase
interval selection is delegated to the query service and tool; viewport camera
projection, candidate visibility, and rendering remain adapters. Phase 8 now provides a
stable-ID Layers panel through the same typed viewport command boundary:
visibility and locking filter rendering, sampling, snapping, and editable
selection; active-layer selection, rename, reorder, and selected-object moves
are history-backed; and version-3 update sessions persist document/layer/object
records while versions 1 and 2 remain readable. The remaining compatibility
bridges can be extracted incrementally in future work without changing these
ownership boundaries. Phase 9 also groups
application and test sources in CMake by the `src/` directory tree and records
the intentional compatibility boundaries in the README and this map.
Mirror is a copy command implemented through `geometry_transform.*` and the
viewport's existing two-point constrained-input path: it preserves the source
objects, reflects their stored points and NURBS control vertices, and selects
the new copies after commit. `MirrorTool` now owns source IDs and axis points;
`MirrorCommand` owns copy creation. Transient reflected geometry is still
presented by the viewport while the second axis point moves. Its axis therefore
receives the same Ortho and OSnap behavior as Line without introducing a
second snapping model.

Every planar shape retains local 2D geometry plus an orthonormal `WorkPlaneFrame`
with a world origin, X/Y axes, and normal. Legacy records still map through
their principal XY/XZ/YZ workplane and offset. Drawing input resolves its
plane once in the shared viewport path before a tool receives plane-local
points. At the first point, hovering an existing planar shape makes the active
drawing frame follow that shape. In empty space, drawing follows the add-on's
fallback within the supported principal planes: perspective uses world XY
through the origin, fixed orthographic views use XY/XZ/YZ through the origin,
and other tools in oblique orthographic views use the most view-aligned
principal plane. Line uses the actual camera-facing plane there. It captures
the normal at the first point and advances the plane through each new pivot;
XYZ, Shift, and normal constraints use mouse-ray/world-line placement. Its
temporary world vertices become local degree-1 NURBS planar runs committed
in one history operation. Remaining tools still need explicit frame capture.
Line's existing SnapEngine resolves candidates across scene frames and keeps
their actual world depth; the existing markers are reused.
Rendering, hit-testing, sampling, depth geometry, session serialization, and
Rhino/openNURBS curve CV lifting consume the same frame mapping. `.3dm` import
keeps oblique planar curve frames. Tensor-product NURBS surfaces use the
separate `NurbsSurface3D` world-XYZ model; mesh modeling and nonplanar spatial
NURBS curves remain out of scope.

Select-mode movement also supports an explicit Blender-style grab lifecycle:
`G` starts a move for the selected editable objects, `X`/`Y` constrains the
move independently of Ortho, left-click commits it, and Esc/right-click
restores the pre-grab document snapshot. Pressing `B` enters base-point mode:
the user picks an enabled OSnap point on the selection and moves that anchor
to another enabled OSnap point, even when global OSnap is off. Ordinary
selection dragging remains available as a separate path.

The current 3D viewport keeps curves as local `NurbsCurve2D` data and stores
their oriented planes in `WorkPlaneFrame`; legacy shapes retain principal
XY/XZ/YZ workplanes and offsets. It provides Top/Front/Right/Isometric/
Perspective views and camera-ray picking onto the active drawing frame. At the
start of a shape command, shared input can inherit a frame from a planar scene
object under the cursor, then locks that frame through the remaining points.
Existing 2D editing, object snaps, trimming, and erase stay scoped to matching
active frames so local operations do not unintentionally distort geometry on
another plane. `.vignola` is the default extension and `.blend` is an optional
extension for the same native Blender 5.2.2 project. The exact, versioned
classiCAD document is stored in a Blender Text datablock, while Blender
collections and Curve datablocks provide its scene structure. Blender Curve
splines store rational single-span NURBS pieces split at the classiCAD knot
boundaries because Curve RNA does not expose arbitrary knot arrays; the Text
datablock keeps the exact classiCAD curve definition. Blender-side curve edits
are not yet synchronized back into classiCAD. `.3dm` curve
interchange remains a separate import path that preserves supported arbitrary
planar curve frames and imports supported NURBS surfaces. Blender surface
display meshes are derived from exact Text-datablock surface data. Nonplanar
spatial NURBS curves and mesh modeling remain future work.

Do not begin by moving lines into arbitrary folders. First identify the owner of each piece of state and the direction of its dependencies.

## Organization rules

### Names must describe contents

Folder and file names must tell a human what kind of code is inside them. Prefer:

```text
arc_tool.cpp
snap_engine.cpp
document_serializer.cpp
viewport_renderer.cpp
layer_model.cpp
curve_intersections.cpp
```

Avoid vague dumping grounds such as:

```text
misc.cpp
stuff.cpp
common.cpp
helpers.cpp
utils.cpp
manager.cpp
data.cpp
```

An `input_helpers` file is acceptable only when its contents are genuinely limited to input conversion and input-related presentation helpers. When a helper grows a domain responsibility, move it to the domain module whose name describes that responsibility.

### One responsibility per implementation module

Each `.cpp` file should have one primary reason to change. A header should expose the smallest useful public contract. Do not create empty speculative files just to fill out the tree; create a module when there is a real responsibility to move into it.

One tool per tool module is encouraged, but shared behavior must remain shared. Do not copy NURBS evaluation, snapping, hit-testing, or history code into every tool.

### Dependency direction

The intended dependency direction is:

```text
app / ui  ->  tools  ->  services  ->  core
                         \--------> core
```

More specifically:

- `core/geometry` may use value types and QtCore where practical, but must not depend on QWidget, QPainter, MainWindow, or viewport classes.
- `core/document`, `core/layers`, `core/history`, and serialization must not know about buttons, tool shelves, or screen coordinates.
- `services` may inspect core geometry and document state. Services must not own Qt windows.
- `tools` may use core and services. Tools must not directly reach into `MainWindow`.
- `ui/viewport` routes Qt events and renders previews, but tools own interaction behavior and core owns committed data.
- `ui/main_window` wires menus and controls to application/tool interfaces. It must not implement curve algorithms.
- `main.cpp` should remain an entry point, not a second application module.

Avoid circular dependencies. If two modules need each other, introduce a narrow interface or move the shared value/contract into the lower-level module.

## Target source layout

This is the target map. Keep filenames descriptive and adjust the exact split only when the responsibility remains clear.

```text
src/
  main.cpp                              Qt application entry point only

  app/
    application.*                       application startup and lifetime wiring
    command_router.*                    routes named application commands to tools/actions

  core/
    geometry/
      nurbs_curve.*                     NurbsCurve2D data and invariants
      nurbs_surface.*                   NurbsSurface3D storage, UV trim loops, validation, and evaluation
      nurbs_surface_factory.*           exact ruled extrusion and trimmed planar Fill construction
      curve_factories.*                 line, Bezier, circle, and polycurve factories
      arc_curve_factory.*               signed winding and exact rational three-point arc construction
      curve_evaluator.*                 NURBS evaluation and parameter-domain operations
      curve_editing.*                   knot insertion, Bezier span extraction/splitting, exact trims
      curve_join.*                      connected planar/world ordering, line fusion, and gap decisions
      surface_trim_region.*              prepared UV loops and shared trimmed-region containment
      curve_intersections.*             curve/line intersection calculations
      curve_validation.*                NURBS and geometry validation
      geometry_transform.*               translation, rotation, and control-point transforms

    document/
      document.*                        document root and object/layer ownership
      layer.*                           layer identity, name, order, visibility, locking
      scene_object.*                    persistent object identity and geometry payload
      object_id.*                       stable IDs used by selection and tools
      selection_model.*                 selected object/control-point IDs

    history/
      edit_command.*                    reversible document edit contract
      history.*                         undo/redo stack and command execution

    serialization/
      document_serializer.*             versioned save/restore of document state
      session_serializer.*              update-session persistence if it remains distinct

  services/
    snapping/
      snap_engine.*                     endpoint, midpoint, center, intersection, perpendicular, tangent
      snap_candidate.*                  candidate data and ranking/tolerance rules
    hit_testing/
      curve_hit_tester.*                hit-testing against stored NURBS geometry
      control_point_hit_tester.*        control-point hit-testing
    sampling/
      curve_sampler.*                   cached display/erase samples from NURBS source data
    viewport/
      viewport_transform.*              world/screen conversion, zoom, pan, quaternion turntable, and trackball orbit math

  tools/
    tool.*                              common tool lifecycle/input/preview contract
    tool_context.*                      document, selection, history, services, and view access
    select_tool.*                       click, shift-click, and box selection
    point_tool.*                        point creation
    point_extrude_tool.*                unified selection-driven point/curve extrusion
    line_tool.*                         continuous line/polyline creation
    rectangle_tool.*                    dynamic rectangle preview and creation
    circle_tool.*                       circle creation
    arc_tool.*                          one-point and two-point arc creation
    bezier_tool.*                       Bezier creation
    nurbs_tool.*                        NURBS control-point creation/editing
    control_point_tool.*                control-point editing mode if it is separate from selection
    subdivide_tool.*                    subdivision preview, wheel steps, and committed markers
    join_tool.*                         ordered connected-curve joining
    explode_tool.*                      PolyCurve component extraction
    rotate_tool.*                       rotate selection around chosen base/reference points
    trim_tool.*                         click-to-trim at the nearest valid intersection
    erase_tool.*                        drag-to-erase selected curve portions
    delete_command.*                    deletion as a reversible document command

  ui/
    main_window.*                       menus, tool shelf, preferences, and high-level wiring
    input_helpers.*                     small Qt input conversions/icons only
    viewport/
      viewport_widget.*                 Qt event routing, focus, viewport lifecycle
      viewport_renderer.*               drawing document geometry and overlays
      viewport_overlay.*                snap markers, previews, selection boxes, tool labels

tests/
  core/                                 geometry, document, layer, history, serialization tests
  services/                             snapping, hit-testing, and sampling tests
  tools/                                tool interaction/regression tests
  ui/                                   focused viewport smoke tests where practical
```

The exact number of files is less important than the naming rule: a human should be able to predict what a file contains without opening ten unrelated files.

## Foundational model decisions

### Separate tool identity from geometry identity

The current `Tool` enum is used for both active commands and persisted shape types. During the refactor, separate these concepts:

- `ToolId` or equivalent: the currently active interaction (`Select`, `Line`, `Trim`, `Rotate`, etc.).
- `GeometryType` or equivalent: the persistent object kind (`Line`, `Arc`, `Circle`, `PolyCurve`, etc.).

Erase, Trim, Rotate, Join, Explode, and Delete are commands/tools, not geometry types. They must not be serialized as if they were shapes.

### Use stable IDs, not container indexes

Layers and future editing features will reorder, delete, join, and explode objects. Selection and tool state must use stable `ObjectId`, `LayerId`, and, where needed, control-point identifiers. Array indexes may be used temporarily for iteration, but must not be the long-term identity of an object.

The document model should become conceptually similar to:

```text
Document
  layers: ordered Layer records

Layer
  id, name, visible, locked, display properties
  ordered object IDs

SceneObject
  id, layer ID, geometry, object properties

SelectionModel
  selected object IDs
  selected control-point references
```

Keep the stored curve as `Shape::NurbsCurve2D` or its clearly named successor. Follow every NURBS requirement in `AGENTS.md`; do not create a second incompatible curve representation.

### Make history document-level

Undo/redo must apply to the document, not to a private `QVector<Shape>` inside the viewport. A snapshot-based history is acceptable as an intermediate step. The long-term interface should support reversible edit commands so layers, selection-related edits, joins, trims, deletes, and future properties can share one history mechanism.

### Make the viewport a coordinator, not the application

`ViewportWidget` should eventually do four things:

1. receive Qt mouse, wheel, and keyboard events;
2. forward them to the active tool/controller;
3. request rendering of document state and tool previews;
4. expose viewport settings such as pan, zoom, and display options.

It should not own the document model, implement every tool, contain the NURBS algorithms, or serialize the entire application.

## Tool contract

Use a common tool lifecycle rather than a growing switch statement spread through the viewport:

```text
begin(context)
handleMousePress(input)
handleMouseMove(input)
handleMouseRelease(input)
handleWheel(input)
handleKey(input)
cancel()
commit()
preview()
statusText()
```

The exact C++ interface may differ, but every tool should have a clear owner for:

- temporary points and interaction state;
- snapping requests;
- preview geometry/overlays;
- document edits;
- cancellation and commit behavior;
- status/help text.

Tools should produce a document edit or command rather than modifying random viewport members. Shared snapping, hit-testing, curve sampling, and transforms belong in services.

## Layer readiness requirements

Before implementing a visible Layers panel, the refactor must provide:

- a document root that can own multiple layers;
- stable layer and object IDs;
- layer visibility and locking in the document model;
- selection that can cross layers while respecting hidden/locked rules;
- rendering that filters by layer state;
- save/restore that includes layer data and a version number;
- undo/redo for layer creation, deletion, reorder, rename, visibility, locking, and object moves.

Do not bolt a `layerIndex` field onto the current viewport arrays as the final design.

## Incremental migration plan

Do not perform this as one blind rewrite. Complete one phase, build it, run tests, inspect the diff, and only then begin the next phase.

### Phase 0 — Baseline and safety

- Inspect `git status` and preserve unrelated user changes.
- Build the current tree.
- Run all tests and the offscreen startup smoke test.
- Record current behavior and add regression coverage for anything that is fragile.

### Phase 1 — Separate vocabulary and core contracts

- Split active tool IDs from persisted geometry types.
- Define stable object/layer IDs.
- Add core headers with clear ownership and validation.
- Keep compatibility with existing saved sessions while serialization migrates.

### Phase 2 — Extract the document and layers model

- Move committed shapes out of `ViewportWidget` into `Document`.
- Add `Layer` and `SceneObject` ownership.
- Convert selection references from indexes to stable IDs.
- Keep the current UI behavior while layers initially remain a model capability.

### Phase 3 — Extract history and selection

- Move undo/redo into `core/history`.
- Move selection into `core/document/selection_model`.
- Ensure delete, join, explode, trim, erase, rotate, and control-point edits use the common history path.

### Phase 4 — Extract reusable services

- Move snapping into `SnapEngine`.
- Move hit-testing into curve/control-point testers.
- Move NURBS sampling and erase caches into the sampling service.
- Move world/screen conversion into `ViewportTransform`.
- Keep NURBS geometry as the source of truth.

### Phase 5 — Create the tool framework

- Define `Tool`, `ToolContext`, input data, preview data, and tool status contracts.
- Migrate tools one at a time, compiling and testing after each group.
- Recommended order: Select, Point, Line, Rectangle, Circle, Arc, Bezier/NURBS, Subdivide, Join/Explode, Rotate, Trim, Erase.
- Keep Trim and Erase late because they depend on reliable sampling, hit-testing, intersections, and document edits.

### Phase 6 — Split viewport rendering

- Move geometry drawing to `ViewportRenderer`.
- Move snap markers, tool previews, selection boxes, control points, and erase/trim previews to named overlay responsibilities.
- Leave the widget responsible for Qt lifecycle and event routing.

### Phase 7 — Simplify application/UI wiring

- Keep `MainWindow` responsible for menus, controls, preferences, and command routing.
- Remove geometry algorithms and document mutation from `MainWindow`.
- Keep the public viewport API small and intentional.

### Phase 8 — Add the Layers UI

- Build the layer panel on the already-tested document/layer model.
- Add visibility, locking, active layer, reorder, rename, and object movement through commands.
- Add serialization and regression tests before expanding the panel.

### Phase 9 — Cleanup and enforce the architecture

- Remove obsolete compatibility paths only after tests prove they are unused.
- Update the architecture map and README.
- Add CMake source grouping that mirrors the directory structure.
- Search for forbidden dependencies and accidental duplicate geometry representations.

## Validation required after every iteration

After any C++ refactor:

```sh
git diff --check
cmake --build build
ctest --test-dir build --output-on-failure
QT_QPA_PLATFORM=offscreen ./build/classiCAD
```

The startup command may need to be stopped after confirming that the window initializes; a normal event loop staying open is not a failure. Inspect the log for startup errors.

Also verify:

- no unexpected files are staged or overwritten;
- `AGENTS.md` rules are still satisfied;
- saved session compatibility is preserved or deliberately migrated;
- the test target still compiles independently of the application entry point;
- names and folders remain understandable in a file browser.

If a phase breaks the build or a regression test, stop that phase and fix it before moving on. Do not hide a broken intermediate state under a giant follow-up patch.

## Definition of done

The refactor is complete when:

- the viewport is no longer the owner of all document, tool, history, snapping, and rendering logic;
- every major tool has a named module with a clear responsibility;
- persistent geometry, document/layer state, history, services, tools, and UI have distinct ownership;
- selection and editing use stable IDs rather than fragile array indexes;
- layers can be added without rewriting every tool;
- all committed curves still satisfy the Rhino/openNURBS and NURBS rules in `AGENTS.md`;
- build, tests, and startup smoke checks pass;
- the folder and filenames explain the architecture to a human;
- this prompt's progress section and architecture map are current.

## Progress ledger

Update this table at the end of every refactoring iteration. Mark a phase complete only when its exit conditions are actually met.

| Phase | Status | Notes |
|---|---|---|
| 0. Baseline and safety | Complete | Existing modular split builds; trim regression and offscreen startup smoke checks passed before phase 1 changes. |
| 1. Vocabulary and core contracts | Complete | Added distinct ToolId/GeometryType contracts, stable ObjectId/LayerId value types, shared NURBS validation, and geometryType session serialization with legacy tool-field/version-1 compatibility. Core-contract and existing trim regressions pass. |
| 2. Document and layers model | Complete | Added Document-owned SceneObject records, default and additional Layer records, stable object/layer membership, visibility/locking/editability APIs, document snapshots, and ID-based viewport selection/tool state. Preserved current UI behavior with a temporary container-compatible viewport bridge; build, both registered tests, diff check, and offscreen startup smoke passed. |
| 3. History and selection | Complete | Added core/history/History for document-level snapshot undo/redo and core/document/SelectionModel for object IDs, primary selection, control-point references, and pruning. Migrated viewport history operations and all existing edit paths—delete, join, explode, trim/erase, rotate, creation, and control-point edits—through the shared history path while preserving UI behavior. Added core contracts; build, both registered tests, diff check, and offscreen startup smoke passed. Next: phase 4, reusable services. |
| 4. Reusable services | Complete | Added core/geometry/curve_evaluator, services/viewport/ViewportTransform, services/sampling/CurveSampler, services/hit_testing/CurveHitTester, and services/snapping/SnapEngine. Viewport runtime paths now delegate world/screen conversion, zoom-at-cursor, NURBS evaluation, NURBS/scene sampling, snapping, curve hit-testing, and control-point hit-testing to those services while retaining compatibility bridges and malformed-geometry fallbacks. Added service contract coverage, including document erase-cache generation; build, both registered tests, diff check, and offscreen startup smoke passed. Next: phase 5, the tool framework. |
| 5. Tool framework | Complete | Added non-Qt ToolInput, ToolPreview, ToolStatus, InteractionTool, ToolContext, and ToolRegistry contracts. Migrated point, continuous line, rectangle, circle, Bezier, and NURBS creation state/commit paths into named runtime tool modules; registered named Select, Arc, Rotate, Trim, and Erase lifecycle modules as compatibility bridges while their mature viewport event-state behavior remains unchanged. Added direct tool-context tests for registry coverage, point commits, connected-line completion, preview publication, and NURBS invariants. Build, both registered tests, diff check, and offscreen startup smoke passed. Next: phase 6, split viewport rendering. |
| 6. Viewport rendering | Complete | Added `ViewportRenderer` for grid, origin, committed geometry, NURBS evaluation/fallback drawing, control points, and subdivision markers, plus `ViewportOverlay` for snap markers, selection boxes, creation/rotation previews, tool labels, and erase/trim overlays. `ViewportWidget::paintEvent` retains Qt paint orchestration and delegates drawing responsibilities to these named modules; existing helper entry points remain thin compatibility bridges and mature event routing/state stay in the widget. Build, both registered tests, diff check, and offscreen startup smoke passed. Next: phase 7, simplify application/UI wiring. |
| 7. Application/UI wiring | Complete | Replaced the wide viewport edit-method surface used by `MainWindow` with typed `ViewportCommand` dispatch and `ViewportCommandResult` values for undo/redo, subdivision, join, explode, and rotate. Encapsulated viewport-to-window notifications behind `ViewportUiCallbacks` and `setUiCallbacks`; `MainWindow` remains responsible for menus, controls, preferences, status presentation, and update-session orchestration without geometry algorithms or direct document mutation methods. Build, both registered tests, diff check, and offscreen startup smoke passed. Next: phase 8, add the Layers UI. |
| 8. Layers UI | Complete | Added `core/serialization/document_serializer.*` and regression coverage for stable layer IDs, object membership, active layer, visibility, locking, rename, reorder, and NURBS-bearing document records. Added the right-panel Layers UI with add/remove, rename, reorder, active-layer selection, visibility/locking controls, and move-selected-objects commands routed through `ViewportWidgetApi`; rendering, sampling, snapping, and editable selection now respect layer state. Version-3 update sessions persist the document/layer model while versions 1 and 2 remain readable. Build, both registered tests, diff check, and offscreen startup smoke passed. Next: phase 9, cleanup and enforce the architecture. |
| 9. Cleanup and enforcement | Complete | Removed unreachable duplicate viewport rendering, hit-testing, NURBS-evaluation, and preview fallback implementations after confirming their extracted renderer/service/overlay paths are live. Retained only compatibility bridges still referenced by editing, session migration, or regression tests. Added CMake source groups mirroring `src/core`, `src/services`, `src/tools`, and `src/ui`; audited core/services/tools for forbidden UI dependencies and confirmed `NurbsCurve2D` is the sole committed curve representation. Updated the architecture map and README. `git diff --check`, `cmake --build build`, both registered tests, and the offscreen startup smoke passed. Refactoring phases 0–9 are complete; future extraction of the remaining explicit bridges is optional follow-up work. |
| 10. 3D camera and planar workplanes | Complete | Added per-shape XY/XZ/YZ plane and offset metadata, 3D camera projection with orthographic/perspective presets and orbit, workplane ray-picking, plane-aware rendering/grid/axes, active-plane selection/snapping/erase/dimension anchors, an active-plane offset control, and plane-aware `.vignola`/`.3dm` geometry mapping. Existing curves remain local 2D NURBS; no mesh or arbitrary spatial NURBS was added. `cmake --build build`, all three offscreen CTest suites, `git diff --check`, and offscreen application startup passed. |
| 11. Blender-derived 3D grid | Complete | Added GPL-2.0-or-later adaptations of Blender's procedural grid shaders and draw setup. The normal QWidget/QPainter viewport is preserved; perspective and orthographic grids render through an offscreen GL framebuffer and composite beneath scene geometry, with the existing painter renderer as a context/shader fallback. Added `COPYING`, source notices, and README licensing/architecture notes. `cmake --build build -j2`, all three offscreen CTest suites, `git diff --check`, an offscreen app startup/fallback smoke, and a software-OpenGL shader render test for both perspective and orthographic views passed. |
| 12. Blender grid scale and LOD | Complete | Added `BlenderGridScale` for Blender's 8-step decimal ladder, three extra fixed-axis orthographic subdivisions, camera-dependent selection, and linear fractional level blending. The GPU shader clamps its three adjacent draw levels to the same ladder. Perspective focus distance follows `overlay_grid.hh`; fixed-axis orthographic distance follows Blender's `10 * 12px / (region width * projection[0][0])` rule. Added CPU regression coverage for transition values and axis-view classification. `cmake --build build -j2`, all three CTest suites, `git diff --check`, offscreen startup/fallback, and a brief desktop software-OpenGL startup passed. |
| 13. Blender camera-relative grid frame and view mapping | Complete | Added `BlenderGridFrame` to resolve the Blender display plane, camera-relative grid origin, focus distance, and default global-axis visibility without mutating the CAD construction plane. Fixed views map Top/Bottom→XY, Front/Back→XZ, Right/Left→YZ; free-angle and perspective views use XY. The GPU shader now draws true global X/Y/Z axes, and the CPU fallback consumes the same resolved plane/origin/axis mapping. Added `gridViewDistance` to camera state for free-angle orthographic grid LOD and update it during orthographic zoom/navigation animation. Added contracts for all six axis planes, axis visibility, unchanged CAD plane, pan tracking, and custom-orthographic distance/zoom. `cmake --build build -j2`, all three CTest suites, `git diff --check`, and software-OpenGL shader startup validation passed. |
| 14. Blender grid scene depth and occlusion | Complete | Added sampled 3D depth geometry for visible NURBS/vector curves, point markers, and picture planes; viewport depth-only pass; transparent grid composition over the scene image; GL_LEQUAL depth testing; first-iteration-only depth writes; and Blender-style progressive perspective clip-space z-bias. Kept Qt-painted scene geometry, screen annotations, previews, and the CPU grid fallback. Opaque picture planes occlude correctly; alpha cutouts in RGBA images are a documented follow-up limitation. `cmake --build build -j2`, all three CTest suites, `git diff --check`, and five-second XCB/Mesa software-OpenGL initialization passed without shader/context setup errors. Fixed-view image comparison and the broader depth-occlusion matrix are deferred to Stage 5. |
| 15. Blender grid units and theme appearance | Complete | Added persistent document length-unit/base-spacing settings with version-4 serialization and legacy defaults, shared `BlenderGridAppearance` inputs for GPU and Qt fallback, and document-grid/preferences controls for units, spacing, theme colors, opacity, and low-alpha stipple. Kept geometry coordinates in millimeters. Added unit-scaled LOD, settings serialization, and theme validation coverage; build, all three CTest suites, `git diff --check`, and offscreen startup/fallback passed. |
| 16. Blender viewport parity regression matrix | In progress | Added CPU-fallback image regressions for six axis views, isometric ortho, perspective, axis colors, horizon fade, close/far zoom, pan, and orbit; added document grid-settings undo/redo coverage. A dedicated widget interaction test sends wheel, configured pan, Shift+MMB pan, and Shift+configured-button orbit events. The offscreen CTest checks the Qt fallback; an XCB/Mesa run directly requires covered GPU-rendered orthographic and perspective grids. All four CTest suites and `git diff --check` pass. Direct Blender-vs-classiCAD screenshot comparison remains to validate pixel-level parity. |
| 17. Blender navigation and projection comparison | In progress | Traced Blender's view zoom, smooth view, camera projection, grid setup, and theme source; read the user's Blender 5.2 preferences. Matched the 2x viewport projection factor, 1.2 wheel distance ratio, 200 ms smoothstep preset animation, target-depth cursor zoom, Blender distance bounds, 151/301 perspective/orthographic grid line counts, unbounded floor focus distance, and Blender-derived theme colors. The exact old saved palette migrates in memory without overwriting edited colors. Added projection and far-zoom recovery tests. At this phase, the QWidget renderer synchronously read GPU frames into a QImage; phase 18 addresses that. Blender screenshot/orbit comparison remains to establish full parity. |
| 17a. Quaternion orbit and depth navigation | In progress | Replaced yaw/pitch camera storage and preset interpolation with double-precision quaternion rotation/slerp so orbit can pass through poles without losing CAD projection precision; updated GPU camera up vector to use the same orientation. Orbit-start picking now uses the GPU depth buffer over shared visible-scene geometry, with a small on-demand readback and sampled CPU fallback when the GPU path is unavailable or finds no hit. Both paths preserve the perspective eye when the target depth changes. Offset-plane, overlapping-depth, pole, and GPU frontmost-overlap regressions pass. Direct visual/interaction comparison remains open. |
| 17b. Blender 5.2 trackball rotation | In progress | Added selectable Turntable/Trackball settings, a separately saved trackball sensitivity, and Blender's 1.1-radius aspect-correct virtual sphere/hyperbola mapping with drag-start quaternion and cross-product axis math in `ViewportTransform`. Wired begin/move/end gesture handling to the viewport gizmo and mouse orbit drags; Turntable remains the default. Added transform-level sensitivity and absolute-drag regressions in `tests/core_contracts.cpp`. Build, all four CTest suites, the XCB/Mesa GPU interaction regression, and offscreen fallback startup pass. Live pointer-gesture calibration against Blender remains open. |
| 18. Native viewport GPU presentation and curve strokes | In progress | Added a QOpenGLWidget surface, direct grid framebuffer composition without per-frame GPU image readback, OpenGL scene strokes for common CAD curves and points, cached world tessellation, and dashed GPU Bézier/NURBS control guides. Qt remains for pictures, dimensions, tool previews, overlays, noncontinuous layer line styles, and offscreen fallback. Full build, all four CTest suites, XCB/Mesa interaction checks for rectangle/Bézier guides/point pixels, `git diff --check`, and offscreen startup/fallback passed. Remaining scene migration and close-zoom curve inspection are tracked in Stage 7 of `BLENDER_VIEWPORT_PARITY_PLAN.md`. |
| 19. Workplane arc direction geometry | Complete | Added `core/geometry/arc_curve_factory.*` to choose the signed sweep containing the third point in local workplane coordinates and build exact rational quadratic NURBS spans. Two-point arc preview, committed geometry, and arc snap geometry now share that signed direction; screen-space circle fitting no longer controls the arc. Invalid/collinear arc definitions are rejected before commit. Added minor-side, major-sweep, and degenerate-input regression coverage. `cmake --build build`, all four CTest suites, `git diff --check`, and offscreen application startup passed. |
| 20. Shared oriented drawing-plane input | In progress | Added `WorkPlaneFrame` geometry and backwards-compatible shape serialization while keeping committed curves as local `NurbsCurve2D`. Hovering a planar scene object makes the drawing frame follow that object's stored plane. In empty space, shared drawing input follows the add-on fallback within the supported principal planes: world XY through the origin in perspective, XY/XZ/YZ through the origin in fixed orthographic views, and the most view-aligned principal plane in oblique orthographic views. `ShapeCreationTool` captures the frame at the first point and keeps later input, previews, and committed geometry in it; point, rectangle, circle, Bezier, and NURBS tools use that path. The four polygon modes use a dedicated `PolygonTool` with add-on click semantics, a 32-side default, scroll/`S` side-count controls, `R` radius, `A` apothem, `L` length entry, mode-specific odd-side handling, projected screen-space 6-degree global-axis inference, and X/Y/Z constraints projected into the captured frame. Polygon geometry is previewed and committed as a closed degree-1 NURBS; its stored curve drives rendering, hit testing, snapping, control-point display, and sampling, with no point-only polygon fallback. Polygon `P` uses fixed-workplane perpendicular sizing/orientation behavior to preserve the captured frame. The four ellipse modes now use a dedicated `EllipseTool` with captured oriented frames, perpendicular and vertical-axis overrides, add-on-style 6-degree major-axis inference, Alt bypass, D/R/F numeric entry, mode-specific guides/HUD, and exact rational NURBS preview/commit geometry. The viewport's Arc input keeps the same frame from its first point. Arc click state remains viewport-owned; the lifecycle controller must not publish its empty click list over the Arc's input points. One-point Arc sweep tracking, preview, and committed rational NURBS now use captured workplane coordinates instead of screen-space circles. The 1-point Arc now ports CADdraw 7's plane-projected 125-pixel compass (72-sample ring, 24 ticks, chorded inner protractor arcs, center cross, axis colors, light-neutral oblique-plane contrast for the dark viewport, a camera-right size held at 125 screen pixels independent of zoom, and an unclipped HUD anchor for extreme camera distances), stage-specific radius/arc guides, 15-degree/6-degree soft angle snapping with geometry-snap precedence, the C snap toggle, P perpendicular-plane toggle, radius/angle entry, and Enter/Space/right-click completion. Preview tessellation is display-only and the committed arc remains exact rational NURBS; the tool exposes no segment-count control. Existing OSnap remains the sole geometry snap model. The viewport now ports radCAD's Arc axis behavior: One Point XYZ selects/toggles the plane normal before drawing; Two Point XYZ sets the chord axis before drawing; endpoint movement infers a world axis from its projected screen direction at the add-on's 6-degree threshold and intersects the cursor ray with that world axis. Near-vertical chords reorient the Arc plane using the current view direction, with the add-on's 0.995/0.98 hysteresis and X/Y override, while committed geometry stays planar local NURBS. The two-point height pick uses the chord-perpendicular measurement and relative semicircle snap in that selected plane. Curve tangent/perpendicular and tangent-circle tools inherit selected-curve planes. Added Point by Line, Point by Arcs, Point Center, and Curve Span Center point commands; added interpolated/freehand NURBS curves, perpendicular-from-edge, common tangent, and common perpendicular tools. Interpolate/freehand create centripetal Catmull-Rom paths as piecewise cubic NURBS; Curve Span Center names the closest NURBS knot-span operation because this project has no mesh-edge topology. Existing OSnap is reused; no second snap model was added. `LineTool` captures its frame and can split a world-space chain into planar runs; camera-facing oblique input and line normal locking are covered by phase 21. Interactive Arc and Z-plane drawing still need a GUI comparison against radCAD. |
| 21. Add-on Line world-axis input | Complete | Replaced plane-projected Line constraints with closest-point mouse-ray/world-line placement for XYZ, passive global-axis inference, Shift, and N normal locking. L controls plane locking; each new pivot moves the locked-normal plane. Line uses the actual camera-facing plane in oblique ortho, and the existing SnapEngine resolves enabled OSnaps across scene planes with actual world depth and preview endpoint snapping. Preview world vertices and planar NURBS runs feed GPU and painter rendering. Committed data remains local degree-1 NURBS; a chain changing planes creates planar component objects together through the ToolContext batch commit port, with one Undo. Added actual Qt-event/save-reload regressions for side-view Z drawing, all perspective world axes, a mixed-plane chain, atomic Undo, and cross-plane OSnap. Corrected the Rhino import test to distinguish tilted planar lines from genuinely nonplanar cubic curves. Build, all four CTest suites, the XCB native GPU interaction run with inspected Z preview, diff checks, and offscreen startup passed. Core regressions also cover normal locking, Shift direction preservation, and Backspace depth restoration. Remaining tool migrations belong to phase 20. Open app processes were confirmed to still run deleted older executables; the Update action is required to load the rebuilt app while preserving their scenes. |

| 22. Blender-native `.vignola`/`.blend` file foundation | Complete | Replaced the native project save/open boundary with a Blender 5.2.2 background adapter using Blender's `open_mainfile` and `save_as_mainfile` APIs. Save As keeps `.vignola` as the default and offers `.blend`; both extensions store the exact versioned classiCAD document snapshot in a Blender Text datablock and organize existing curves under Blender collections and Curve objects. Because Curve RNA has no arbitrary knot-array field, rational NURBS are split into exact single-span NURBS pieces at their stored knot boundaries; general non-clamped curves use an adaptive sampled fallback. The Text snapshot retains each exact source curve. This step creates no mesh data. Removed the old 3DM-backed project save/load implementation; Rhino `.3dm` remains a separate import path, and old 3DM-backed `.vignola` archives are rejected without migration. The CMake build passed. Blender reopened the regenerated `Test_fixed.blend`, confirmed its classiCAD data, and showed the circle as four rational quadratic NURBS spans with coincident endpoints. The CTest suite was not run. |

| 23. NURBS surface model and curve extrusion | Complete | Added `NurbsSurface3D` with Rhino-style tensor-product U/V storage, validation, evaluation, exact ruled extrusion, persistence, transforms, rendering, depth sampling, hit-testing, selection bounds, Rhino surface import, and derived Blender display meshes. Extrude uses the existing controller: points create edges and curves create surfaces, with identical spatial input, XYZ constraints, snaps, preview, and completion. Removed the separate surface button, command, and controller; selection survives activation. Mixed previews retain surfaces when GPU edge previews are present, and the shared HUD covers both source types. Full CMake build passed. Actual viewport-event regressions passed in offscreen and native XCB/Mesa modes for point and curve selection, mixed previews with verified interior pixels, exact rational geometry and domains, free and edge-on constrained input, OSnap, native save/reload, and atomic Undo. Surface preview captures were inspected; offscreen app startup passed. |

## Continuation progress ledger

| Phase | Status | Notes |
|---|---|---|
| 24. Modularity, scalability, and build improvements | Complete | CPU fallback and GPU/depth share revision-keyed world samples; projected positive-weight hulls conservatively filter hit, box, and near-snap queries. Runtime snap/pick, CPU/native-GL redraw, one-object invalidation, and snapshot-history latency/RSS measurements are recorded. Matched warm R0/current build scenarios, all ten tests, dependency audit, preset configuration, and offscreen startup are recorded in the companion plan and baseline files. App-only clean/header/one-tool/link timings are not consistently faster; the measured all-target clean improvement and its conditions are stated without a blanket speed claim. |

R12 follow-up checkpoint: added isolated `app-dev` and `full-test` CMake
configure/build presets and documented them in the README. `cmake --list-presets`
recognized both; neither ccache nor Ninja is available in this environment.
The generic selection rectangle is shared by Select and Trim, so no second
Trim overlay module was introduced. Three no-op and three timestamp-only
one-tool builds passed; their medians are 3.53 seconds / 85,556 KB and
4.97 seconds / 201,920 KB respectively. `git diff --check` passed; no tests or
startup were run. The clean-build and runtime comparisons remain open.

R9 query cleanup: `curve_erase_query.*` now builds visible curve/point
intersection candidates once and `prepareEraseGeometryCache` reuses that set
for every selected component. The full build passed with the 268-source audit;
tests were not run.

R12 app-only clean build: rebuilt the 144 project objects referenced by the
current application link in 103.01 seconds / 580,984 KB with dependencies warm.
R1's 104.05-second / 726,188-KB result removed generated autogen files too,
so this comparison is directional. A following all-target build passed and
restored test/benchmark links; executables were not run.

R8 Mirror routing correction: generic mouse dispatch now leaves Mirror clicks
to the adapter that commits on the second axis point. The full build passed;
public interaction behavior remains unverified because tests were not run.

R8 Subdivision wheel routing: the viewport now passes translated wheel input
to `SubdivisionTool::handleWheel`, which updates section state and its preview;
the widget retains event consumption, status, and diagnostics. Full build and
268-source dependency audit passed. Tests were not run.

R12 latest repeated timing update: after adding the second benchmark executable,
three no-op builds measured a 3.61-second / 86,640-KB median; three
timestamp-only one-tool all-target builds measured 5.75 seconds / 201,748 KB.
The source remained unchanged by those timing cycles. The benchmark preset is
included in the CMake preset map. All six builds passed; tests and startup were
not run. See `BUILD_BASELINE.md` for ranges and the target-graph caveat.

R8 Mirror commit ownership: `MirrorTool::dispatchMousePress` now commits the
existing mirror transaction on the second accepted axis click. The widget
routes Mirror through typed dispatch and retains only log/presentation
adaptation; its `handleMirrorPoint` decision adapter was removed. Full build
and the 268-source dependency audit passed; no tests or startup were run, so
public click behavior remains pending verification.

R8 Scale click ownership: `ScaleTool::dispatchMousePress` now accepts scale
click stages and commits direct-click completion; `ScaleDispatchResult`
provides prompt/log/redraw facts for the viewport adapter. The dedicated Scale
left-click event branch was removed, while keyboard Enter and factor entry
remain in the viewport. Full build and the 268-source dependency audit passed;
tests and startup were not run, so click behavior remains unverified.

R8 Scale factor-key ownership: the typed key dispatch is now virtual, and
`ScaleTool::dispatchKey` owns Backspace, factor-character normalization and
entry, factor acceptance, 1D preview activation, and stage-two Enter commit.
The viewport supplies the resolved preview cursor and retains selection-center
Enter/Escape plus prompt, logging, snap-reset, and redraw adaptation. Full
build and the 268-source dependency audit passed; tests and startup were not
run, so the interaction remains unverified.

R6–R11 contract validation and transform correction: the full CTest run passed
four of six suites; `viewport_interaction` passed after Mirror and Scale moved
to typed dispatch. The first `core_contracts` failure was fixture contamination:
it restored a locked layer before asserting deleted-ID-only pruning. The fixture
now tests deleted IDs while the layer is editable, then separately asserts
pruning after the layer becomes locked. The targeted core suite passes.
`EraseTool::finishStroke()` now preserves its completed screen path and
candidate IDs until reset, matching the viewport query and presentation
lifecycle.

Mixed-workplane Join now passes component continuity, world-space hit testing,
JSON round-trip, and translation checks. The translation contract revealed that
the extracted `translateShapeGeometry` ignored its input frame for a PolyCurve;
it now derives world displacement from that frame and moves the parent and each
component frame origin while preserving local NURBS CVs. Direct Rotate, Scale,
and Mirror contracts pass. The targeted Trim/Erase suite now fails only the two
tangent-contact and endpoint-gap Erase assertions recorded in the R0 baseline.
The full build and 268-source dependency audit passed; `git diff --check` passed;
offscreen startup reached viewport construction. Remaining: finish Scale's
selection-center/Escape adapter and Rotate input ownership, complete R7 event
precedence and R9–R11 review, rerun full CTest after the next behavior
checkpoint, and collect matched clean-build and representative runtime
measurements.

R8 typed Scale and Rotate key dispatch: Scale's selection-center Enter and
Escape decisions now run through `ScaleTool::dispatchKey`. The viewport
supplies the view-derived bounds center and adapts cancellation, prompt, snap,
log, and redraw effects; the old widget `handleScalePoint` and `commitScale`
decision helpers were removed. Right-click cancellation remains in the
viewport adapter.

`RotateTool` now overrides typed key dispatch and owns Escape reset, typed
angle/snap decisions, axis/perpendicular-plane decisions, and commit requests.
The viewport supplies resolved cursor/frame and current snap preferences, then
applies returned cursor/frame and presentation/commit effects. The widget's
special `handleRotateKey` route was removed. Added direct Scale center-Enter/
Escape and Rotate typed 45-degree commit/Escape contracts; these pass. Full
build and 268-source dependency audit passed. Latest full CTest passed 5/6:
`box_selection`, `core_contracts`, `viewport_render_contracts`,
`viewport_interaction` (248.01 s), and `vignola_file`; `trim_seam` still reports
the two R0 baseline Erase tolerance failures. `git diff --check` passed.
Remaining: offscreen startup for this checkpoint, R7 event precedence, R8
right-click cancellation adapters, R9–R11 review, and matched R12 measurements.

R8 transform-tool right-click cancellation: ScaleTool, RotateTool, and
MirrorTool now consume right-click through typed mouse dispatch and reset their
own interaction state before finishing the command through ToolContext. The
viewport's per-tool `cancelScale`, `cancelRotate`, and `cancelMirror` right-click
branches are removed; shared snap clearing, debug logging, and redraw remain in
the adapter. Empty source lists still cancel the active tool.

Direct cancellation contracts for all three tools pass. The full build and
268-source dependency audit pass; `git diff --check` passes. Full CTest passed
5/6 after this routing change, with `viewport_interaction` passing in 249.02
seconds; only `trim_seam` reports the same two R0 baseline Erase tolerance
failures. The subsequent guard cleanup built, targeted contracts passed, and
the offscreen startup reached viewport construction. Remaining: close R7 event
precedence and R9–R11 ownership/test review, then collect matched clean-build
and representative runtime measurements.

R8 Mirror Escape dispatch: Mirror cancellation on Escape now runs through
`MirrorTool::dispatchKey`; the dedicated widget Escape branch and
`cancelMirror` helper were removed. The viewport consumes the result and clears
snap presentation, logs, and redraws. The direct Escape contract passes. Full
build and the 268-source dependency audit passed; `git diff --check` passed;
offscreen startup reached viewport construction. The latest full CTest passed
5/6, with `viewport_interaction` passing in 248.93 seconds; `trim_seam` still
reports only the two R0 baseline Erase tolerance failures. Remaining: R7 event
precedence/public cancellation coverage, R9–R11 ownership and test review, and
matched R12 measurements.

R9 Erase intersection-tolerance checkpoint: `curve_intersections.cpp` now
deduplicates seed solutions by evaluated contact position, so tangent-only
contacts no longer produce several near-identical cut parameters.
`curve_erase_query.*` accepts a view-scaled endpoint proximity value; when exact
intersections are absent, nearby endpoints on candidate curves project onto
the source curve and retain the intended erase bounds. The viewport supplies a
three-pixel world-space threshold; the core intersection routine remains exact.
Removed incomplete aggregate initialization in `viewport_widget.cpp`, which
also clears the missing-field warnings for Shape's new geometry members.

The focused Trim/Erase fixture passes both former R0 failures. Full CTest
passed all six suites in 254.67 seconds (`viewport_interaction`: 248.40 s;
`vignola_file`: 5.67 s). Full build and the 268-source dependency audit passed;
`git diff --check` passed; offscreen startup reached viewport construction.
R9 is complete. Remaining: R6–R8 event/tool review, R10 scene-query/render
profiling, R11 focused suite organization, and matched R12 build/runtime
measurements.

## Required iteration report

At the end of each iteration, report:

1. which phase and responsibility changed;
2. which files/folders were added, moved, or changed and what kind of code each contains;
3. what behavior was intentionally preserved;
4. what tests/build/smoke checks passed;
5. what remains before the current phase is complete;
6. the updated progress ledger in this file.

Do not commit or push unless the user explicitly requests it. Do not add unrelated features during the refactor. Keep the architecture understandable enough that a future contributor can continue from this document without reconstructing the design from a giant source file.

#### R7/R8/R10/R11 and R12 measurement checkpoint (2026-10-04)

R7 modal event precedence now preserves Join/Duplicate input across Select All,
Fill, and Delete; idle Select All remains available. Arc Escape cancellation is
owned by `ArcTool`, with direct tool and public viewport coverage. New
`services/hit_testing/projected_curve_bounds.*` safely rejects disjoint
positive-weight NURBS control hulls in scene hit and box queries and fails open
when projection is clipped. Synthetic 1,024-curve medians changed from 15.117
to 0.687 ms for hit testing and 14.671 to 0.524 ms for box selection; these do
not measure full-frame latency. R11 now has separate command, tool, scene-query,
render, event, Trim/Erase, serialization, and viewport suites, with no
production `.cpp` inclusion or private-access macros in tests.

The full CTest run passed 10/10 in 266.52 seconds; the final focused Arc,
scene-query, and event checks passed 3/3. Full builds passed with the 274-source
dependency audit. Offscreen startup reached viewport construction before its
expected five-second timeout. Three-run medians are 3.79 s / 89,588 KB for
no-op builds, 5.47 s / 201,752 KB for a timestamp-only one-tool rebuild, and
126.55 s / 580,000 KB for warm clean all-target builds (individual clean times:
130.64, 126.55, and 125.16 s). Historical clean results use smaller graphs and
are not a matched comparison. `PERFORMANCE_BASELINE.md` records the scene-query
comparison and limits it to service timing; no whole-frame CPU/GL improvement
is claimed.

#### Phase 24 completion audit (2026-10-04)

The previous `Complete` status was reopened after checking the plan's explicit
R0/R10/R12 requirements against current files. `PERFORMANCE_BASELINE.md` still
marks snap/pick, CPU/native-GL redraw, single-object invalidation, and undo
memory/latency as unmeasured, and the current build timings do not match all R0
scenarios. Source inspection also shows CPU `ViewportRenderer::drawShape`
resamples committed curves while `ViewportGeometryCache` is used by GPU/depth
paths. Added `benchmarks/viewport_runtime_benchmark.cpp` to measure those
service/render/cache/history paths; it is registered only in the opt-in
benchmark preset. The benchmark and CPU cache integration still need to build,
run, and pass the applicable rendering/interaction checks. Phase 24 remains in
progress until those gates and matched build scenarios are evidenced.

#### Final phase 24 completion checkpoint (2026-10-04)

This checkpoint supersedes the historical in-progress audit above. The CPU
fallback now consumes prepared double-precision world samples from
`ViewportGeometryCache`; bounded screen-space simplification preserves the
existing tolerance and clipped/unsupported cases retain the adaptive fallback.
The existing projected positive-weight control-hull bounds are also used to
skip disjoint near-snap curve searches. Runtime measurements for snap/pick,
CPU/native-GL redraw, per-object cache invalidation, and snapshot-history
latency/RSS are recorded in `PERFORMANCE_BASELINE.md`.

The matched warm R0/current build scenarios, build tradeoffs, and temporary
baseline harness conditions are recorded in `BUILD_BASELINE.md`. The
`app-dev`, `full-test`, and `benchmarks` presets configure independently. The
full clean build and 274-source dependency audit passed; CTest passed 10/10 in
257.01 seconds; focused render/query tests passed; offscreen startup reached
viewport construction; and `git diff --check` passed. App-only clean and
several incremental builds did not consistently improve, while the warm
all-target clean source build improved substantially. No blanket performance
claim is made. Phase 24 is complete; nothing was committed or pushed.
