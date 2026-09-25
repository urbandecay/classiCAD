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
src/core/geometry/curve_evaluator.* NURBS evaluation and parameter-domain operations
src/core/geometry/geometry_transform.* reflected geometry transforms
src/core/document/object_id.h      stable scene-object identity value type
src/core/document/layer_id.h       stable layer identity value type
src/core/document/layer.h          layer record, visibility, locking, and object membership
src/core/document/scene_object.h   persistent object identity, layer, and geometry payload
src/core/document/document.*       document-owned scene objects, layers, IDs, and snapshots
src/core/document/selection_model.* selected object and control-point references
src/core/history/history.*          document-level snapshot undo/redo ownership
src/core/serialization/document_serializer.* versioned document/layer/object save/restore
src/core/model.*                   compatibility model, factories, serialization, helpers
src/core/debug_log.*               application logging
src/services/viewport/viewport_transform.* world/screen conversion, zoom, and pan
src/services/sampling/curve_sampler.* NURBS display/erase sampling and scene cache generation
src/services/hit_testing/curve_hit_tester.* curve and control-point hit-testing
src/services/snapping/snap_engine.* endpoint, midpoint, center, intersection, perpendicular, and tangent snapping
src/tools/tool.*                  non-Qt interaction lifecycle contract and preview/status values
src/tools/tool_input.h            translated mouse, wheel, and keyboard input payload
src/tools/tool_context.*          document, history, services, factory, commit, and preview ports
src/tools/tool_registry.*         active tool module lookup and ownership
src/tools/shape_creation_tool.*   shared pending-point creation lifecycle
src/tools/select_tool.*            selection lifecycle bridge
src/tools/point_tool.*             point creation
src/tools/line_tool.*              connected line creation
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
src/ui/viewport_widget.cpp         current viewport state, tools, editing, snapping, and paint orchestration
src/ui/viewport/viewport_renderer.* committed geometry, grid, control-point, and subdivision drawing
src/ui/viewport/viewport_overlay.*  snap markers, tool previews, selection boxes, labels, and erase/trim overlays
src/ui/main_window.*               menus, tool shelf, preferences, and window wiring
tests/trim_seam.cpp                current geometry/editing regression coverage
tests/core_contracts.cpp            vocabulary, ID, NURBS, and session compatibility coverage
```

The current split is useful, and phases 1 through 9 now give the remaining
modules explicit vocabulary, validation, document-ownership, history,
selection, reusable-service, tool-lifecycle, and viewport-rendering contracts.
`src/ui/viewport_widget.cpp` is still a large implementation module. It
currently owns event routing, tool state, trim, erase, rotate, join, explode,
subdivision, geometry creation, persistence, and paint orchestration. Geometry
drawing now delegates to `ViewportRenderer`, while snap markers, transient tool
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

Select-mode movement also supports an explicit Blender-style grab lifecycle:
`G` starts a move for the selected editable objects, `X`/`Y` constrains the
move independently of Ortho, left-click commits it, and Esc/right-click
restores the pre-grab document snapshot. Ordinary selection dragging remains
available as a separate path.

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
      curve_factories.*                 line, Bezier, circle, arc, and polycurve factories
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
      viewport_transform.*              world/screen conversion, zoom, and pan

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

## Required iteration report

At the end of each iteration, report:

1. which phase and responsibility changed;
2. which files/folders were added, moved, or changed and what kind of code each contains;
3. what behavior was intentionally preserved;
4. what tests/build/smoke checks passed;
5. what remains before the current phase is complete;
6. the updated progress ledger in this file.

Do not commit or push unless the user explicitly requests it. Do not add unrelated features during the refactor. Keep the architecture understandable enough that a future contributor can continue from this document without reconstructing the design from a giant source file.
