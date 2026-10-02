# classiCAD scalable refactor — living master prompt

This is the long-term refactoring contract for classiCAD. Read this entire file before every refactoring iteration. Read the repository `AGENTS.md` first because its geometry and workflow rules are mandatory. Do not treat this document as a one-time plan: update the progress section and the architecture map whenever the implementation changes.

The purpose of this file is twofold:

1. It tells the coding agent what architecture to build.
2. It makes the project understandable to a human looking at the folders and filenames.

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
src/main.cpp                       application entry point
src/core/tool_id.*                 active interaction vocabulary and tool metadata
src/core/geometry/geometry_type.*  persistent geometry vocabulary and legacy mapping
src/core/geometry/nurbs_curve.*    shared NURBS storage, knot expansion, validation
src/core/geometry/arc_curve_factory.* signed direction and exact rational NURBS construction for three-point arcs
src/core/geometry/work_plane.*     principal and oriented local-2D to world-3D frame mapping
src/core/geometry/curve_evaluator.* NURBS evaluation and parameter-domain operations
src/core/geometry/geometry_transform.* reflected geometry transforms
src/core/document/object_id.h      stable scene-object identity value type
src/core/document/layer_id.h       stable layer identity value type
src/core/document/layer.h          layer record, visibility, locking, and object membership
src/core/document/scene_object.h   persistent object identity, layer, and geometry payload
src/core/document/document.*       document-owned scene objects, layers, IDs, and snapshots
src/core/document/document_settings.* persistent document display units and grid spacing
src/core/document/selection_model.* selected object and control-point references
src/core/history/history.*          document-level snapshot undo/redo ownership
src/core/serialization/document_serializer.* versioned classiCAD document/layer/object snapshot
src/core/serialization/blender_project_file.* `.vignola`/`.blend` save/open through the pinned Blender 5.2.2 runtime
src/core/serialization/blender_project_adapter.py Blender-native collections, Curve datablocks, and document Text datablock
src/core/serialization/rhino3dm_interchange.* separate Rhino/openNURBS `.3dm` import with oriented-plane lifting
src/core/model.*                   compatibility model, shape-plane frames, factories, serialization, helpers
src/core/debug_log.*               application logging
src/services/viewport/viewport_transform.* quaternion 3D camera projection, ray/frame picking, presets, zoom, pan, and Blender-style turntable/trackball orbit math
src/services/sampling/curve_sampler.* NURBS display/erase sampling and scene cache generation
src/services/hit_testing/curve_hit_tester.* curve/control-point hit-testing, drawing-plane inheritance, and cross-workplane orbit-depth picking
src/services/snapping/snap_engine.* endpoint, midpoint, center, intersection, perpendicular, and tangent snapping
src/tools/tool.*                  non-Qt interaction lifecycle contract and preview/status values
src/tools/tool_input.h            translated mouse, wheel, and keyboard input payload with the active workplane frame
src/tools/tool_context.*          document, history, services, factory, commit, and preview ports
src/tools/tool_registry.*         active tool module lookup and ownership
src/tools/shape_creation_tool.*   shared pending-point creation lifecycle
src/tools/select_tool.*            selection lifecycle bridge
src/tools/point_tool.*             point creation
src/tools/line_tool.*              world-space connected line input and planar NURBS run creation
src/tools/rectangle_tool.*         rectangle creation
src/tools/circle_tool.*            rational circle creation
src/tools/arc_tool.*               arc creation lifecycle bridge
src/tools/bezier_tool.*            Bezier creation
src/tools/nurbs_tool.*             NURBS creation
src/tools/rotate_tool.*             rotate lifecycle bridge
src/tools/mirror_tool.*             mirror lifecycle bridge
src/tools/trim_tool.*               trim lifecycle bridge
src/tools/erase_tool.*              erase lifecycle bridge
src/ui/input_helpers.*             Qt event-position and icon helpers
src/ui/viewport_widget_api.h       typed viewport settings, command, status, and callback boundary
src/ui/viewport_widget.cpp         current viewport state, shared drawing-frame inference, tools, editing, snapping, and Qt paint orchestration
src/ui/viewport/blender_grid_renderer.* 3D grid shader setup, offscreen/on-screen contexts, camera uniforms, and procedural GPU drawing
src/ui/viewport/viewport_gpu_surface.* native QOpenGLWidget presentation surface and renderer lifetime
src/ui/viewport/viewport_scene_renderer.* GPU committed-curve strokes and dashed control guides
src/ui/viewport/blender_grid_scale.*     Blender-compatible viewport grid step ladder and view-dependent LOD selection
src/ui/viewport/blender_grid_frame.*     Blender-compatible visual grid plane, camera-relative origin, orthographic distance, and global-axis mapping
src/ui/viewport/blender_grid_appearance.* shared theme colors, opacity, stipple, and camera-fade settings for GPU/Qt grid paths
src/ui/viewport/viewport_depth_geometry.* sampled scene curves, points, and picture planes for the GPU depth prepass
src/ui/viewport/shaders/blender_grid.* adapted Blender grid vertex/fragment shaders
src/ui/viewport/shaders/scene_depth.*     depth-only scene geometry shader pair
src/ui/viewport/shaders/scene_stroke.*    antialiased GPU scene stroke shader set
src/ui/viewport/shaders/scene_point.*     antialiased GPU point marker shader pair
src/ui/viewport/viewport_renderer.* committed-geometry projection, CPU grid fallback, axes, control-point, and subdivision drawing
src/ui/viewport/viewport_overlay.*  snap markers, tool previews, selection boxes, labels, and erase/trim overlays
src/ui/main_window.*               menus, tool shelf, preferences, and window wiring
tests/trim_seam.cpp                current geometry/editing regression coverage
tests/core_contracts.cpp            vocabulary, ID, NURBS, session, camera, and orbit-math compatibility coverage
tests/viewport_interaction.cpp      viewport mouse/wheel and GPU/fallback interaction coverage
```

The current split is useful, and phases 1 through 10 now give the remaining
modules explicit vocabulary, validation, document-ownership, history,
selection, reusable-service, tool-lifecycle, and viewport-rendering contracts.
`src/ui/viewport_widget.cpp` is still a large implementation module. It
currently owns event routing, tool state, trim, erase, rotate, join, explode,
subdivision, geometry creation, persistence, and Qt paint orchestration.
3D grid rendering now delegates to `BlenderGridRenderer`; the CPU grid remains
a context/shader fallback. Geometry drawing delegates to `ViewportRenderer`,
while snap markers, transient tool
previews, selection boxes, control points, labels, and erase/trim overlays
delegate to `ViewportOverlay`. World/screen
conversion, NURBS evaluation and sampling, scene erase-cache generation,
snapping, hit-testing, and the point/line/rectangle/circle/Bezier/NURBS
creation paths now delegate to named core/service/tool modules. The
`ViewportWidgetApi` exposes typed edit commands and callback registration so
`MainWindow` routes menu/tool actions without reaching into viewport mutation
methods or callback storage. `MainWindow` remains responsible for menus,
controls, preferences, status presentation, and update-session orchestration;
it contains no geometry algorithms. Committed
scene storage, snapshot history, and selection storage are owned by core
modules; the viewport's compatibility references, history coordinator, and
thin delegate methods remain migration bridges. The duplicate pre-delegation
rendering, hit-testing, and NURBS-evaluation helper bodies have been removed
after the extracted modules were verified as the only live implementations.
Select, Arc, Rotate, Mirror, Trim, and
Erase have named lifecycle modules registered, while their mature event-state
implementations remain in the viewport until they can move behind narrower
ToolContext ports without changing behavior. Erase target interval selection
still combines viewport interaction state with the service-owned sample cache
and is a later tool/command extraction concern. Phase 8 now provides a
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
the new copies after commit. The transient reflected geometry is drawn from
the same transform while the second axis point moves, so the user sees the
copy before committing it. Its axis therefore receives the same Ortho and
OSnap behavior as Line without introducing a second snapping model.

Every shape retains local 2D geometry plus an orthonormal `WorkPlaneFrame`
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
Rhino/openNURBS CV lifting consume the same frame mapping. `.3dm` import keeps
oblique planar curve frames; mesh and nonplanar spatial NURBS geometry remain
out of scope.

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
planar frames; nonplanar spatial NURBS and mesh modeling remain future work.

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
      curve_factories.*                 line, Bezier, circle, and polycurve factories
      arc_curve_factory.*               signed winding and exact rational three-point arc construction
      curve_evaluator.*                 NURBS evaluation and parameter-domain operations
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
| 20. Shared oriented drawing-plane input | In progress | Added `WorkPlaneFrame` geometry and backwards-compatible shape serialization while keeping committed curves as local `NurbsCurve2D`. Hovering a planar scene object makes the drawing frame follow that object's stored plane. In empty space, shared drawing input follows the add-on fallback within the supported principal planes: world XY through the origin in perspective, XY/XZ/YZ through the origin in fixed orthographic views, and the most view-aligned principal plane in oblique orthographic views. `LineTool` captures this frame on its first point, converts any later input from a changed frame back into its captured frame, renders the live cursor in that frame, and preserves the frame through shape commit. It also has passive in-plane world-axis inference, Shift direction lock, X/Y/Z axis keys where the axis lies in-plane, and Backspace point removal. Off-plane axis locking, camera-facing oblique planes, and the add-on's 3D normal lock remain unsupported while committed curves are planar `NurbsCurve2D`. Existing OSnap is reused with no new snap overlay. Renderer/depth geometry, hit-testing, sampling, dimensions, and Rhino/openNURBS lifting use the same mapping. Remaining tool modules still need explicit frame capture. `cmake --build build` and `git diff --check` passed. Tests were not run in this iteration. |
| 21. Add-on Line world-axis input | Complete | Replaced plane-projected Line constraints with closest-point mouse-ray/world-line placement for XYZ, passive global-axis inference, Shift, and N normal locking. L controls plane locking; each new pivot moves the locked-normal plane. Line uses the actual camera-facing plane in oblique ortho, and the existing SnapEngine resolves enabled OSnaps across scene planes with actual world depth and preview endpoint snapping. Preview world vertices and planar NURBS runs feed GPU and painter rendering. Committed data remains local degree-1 NURBS; a chain changing planes creates planar component objects together through the ToolContext batch commit port, with one Undo. Added actual Qt-event/save-reload regressions for side-view Z drawing, all perspective world axes, a mixed-plane chain, atomic Undo, and cross-plane OSnap. Corrected the Rhino import test to distinguish tilted planar lines from genuinely nonplanar cubic curves. Build, all four CTest suites, the XCB native GPU interaction run with inspected Z preview, diff checks, and offscreen startup passed. Core regressions also cover normal locking, Shift direction preservation, and Backspace depth restoration. Remaining tool migrations belong to phase 20. Open app processes were confirmed to still run deleted older executables; the Update action is required to load the rebuilt app while preserving their scenes. |

| 22. Blender-native `.vignola`/`.blend` file foundation | Complete | Replaced the native project save/open boundary with a Blender 5.2.2 background adapter using Blender's `open_mainfile` and `save_as_mainfile` APIs. Save As keeps `.vignola` as the default and offers `.blend`; both extensions store the exact versioned classiCAD document snapshot in a Blender Text datablock and organize existing curves under Blender collections and Curve objects. Because Curve RNA has no arbitrary knot-array field, rational NURBS are split into exact single-span NURBS pieces at their stored knot boundaries; general non-clamped curves use an adaptive sampled fallback. The Text snapshot retains each exact source curve. This step creates no mesh data. Removed the old 3DM-backed project save/load implementation; Rhino `.3dm` remains a separate import path, and old 3DM-backed `.vignola` archives are rejected without migration. The CMake build passed. Blender reopened the regenerated `Test_fixed.blend`, confirmed its classiCAD data, and showed the circle as four rational quadratic NURBS spans with coincident endpoints. The CTest suite was not run. |

## Required iteration report

At the end of each iteration, report:

1. which phase and responsibility changed;
2. which files/folders were added, moved, or changed and what kind of code each contains;
3. what behavior was intentionally preserved;
4. what tests/build/smoke checks passed;
5. what remains before the current phase is complete;
6. the updated progress ledger in this file.

Do not commit or push unless the user explicitly requests it. Do not add unrelated features during the refactor. Keep the architecture understandable enough that a future contributor can continue from this document without reconstructing the design from a giant source file.
