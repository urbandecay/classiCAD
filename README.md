# classiCAD

A Qt/C++ CAD modeler with a 3D camera, exact planar NURBS curves mapped onto
principal or oriented workplanes, and tensor-product NURBS surfaces in world
XYZ. Spatial NURBS curves and mesh modeling are not implemented yet.

The first pass is intentionally a Blender-inspired layout:

- top menu and workspace bar
- left vertical tool shelf
- central 3D viewport with workplane grid, pan, zoom, standard views, and orbit
- right outliner and properties panel
- bottom coordinate/status bar
- Rhino-style Ortho and OSnap toggles; enabling OSnap reveals Endpoint, Midpoint, Intersection, Center, Perpendicular, and Tangent controls for drawing and moving geometry
- Edit > Preferences with Blender-style categories and a Keymap page for choosing MMB or RMB panning
- Edit > Undo and Redo actions with Ctrl+Z/Ctrl+Y shortcuts for geometry changes
- A stationary click of the configured pan button repeats the last completed tool; moving while holding it pans the viewport. Shift+pan-button drag orbits the camera.
- A Control Points toggle on the tool shelf displays handles for the selected line or curve; the setting is remembered

The workspace bar selects the fallback XY/XZ/YZ drawing plane and its offset, alongside Top/Front/Right, Isometric, or Perspective camera views. When a drawing tool starts over a planar object, cursor input inherits that object's plane. In empty space, drawing follows the add-on's fallback within the app's principal-plane scope: perspective uses world XY; fixed orthographic views use XY, XZ, or YZ through the origin; oblique orthographic views use the principal plane whose normal is most aligned with the view. Line uses the actual camera-facing plane in oblique views and resolves axis constraints in world space, as described below. The remaining tool modules still need explicit frame capture. Existing OSnap candidates and controls are reused without another snap overlay. Selection, object snaps, trimming, and erase operate on the active plane. Projects can be saved as `.vignola` by default or `.blend` from Save As; both are native Blender 5.2.2 files. The exact classiCAD document snapshot, including NURBS knots, surface control nets, and plane frames, is stored in a Blender Text datablock. Blender Curve objects store rational NURBS spans split at the classiCAD curve knot boundaries, preserving the curves' shapes without relying on Blender to regenerate their knot vectors; the Text data retains exact source geometry. Blender displays NURBS surfaces through a derived mesh proxy while the Text datablock retains their exact tensor-product surface data. Either extension opens through the same classiCAD project loader. Rhino `.3dm` import remains a separate geometry interchange operation and imports supported planar NURBS curves and NURBS surfaces. Blender-side geometry edits are not yet synchronized back into classiCAD. Mesh modeling is not included.

Select editable points or curves and choose Extrude on the tool shelf or Edit > Extrude.
Move the cursor to define the extrusion vector, optionally press X, Y, or Z to
constrain it to a world axis, and click to create the result. OSnap uses the
existing spatial snap engine. Points produce edges and curves produce exact
ruled NURBS surfaces, using the same controls and shared vector. Mixed selections
commit together in one undoable operation. A surface is an open sheet without
caps.

The Line tool follows the add-on's 3D direction handling: left-click plants points, X/Y/Z constrain the next segment to a world axis, Shift locks its direction, N locks the drawing-plane normal, L toggles plane locking, Backspace removes the last point, and right-click finishes. Constrained placement finds the closest point between the mouse ray and the world line, including axes outside the initial plane. Passive inference checks all three world axes. OSnap uses the existing engine and preserves a target's actual world depth. Orthographic empty-space input uses the visible principal plane in fixed views and a camera-facing plane in oblique views; perspective starts on XY. The normal locks at the first point, and the drawing plane moves through each new pivot. Preview vertices are temporary world points; committed geometry remains local planar `NurbsCurve2D`. A planar chain stays one degree-1 curve; a chain changing planes becomes connected planar runs committed together with one Undo. Tilted runs use the existing oriented frame representation, without introducing spatial NURBS or meshes. Existing selection dragging and move snapping remain available.

Arc has a press-and-hold flyout. A normal click starts the default 1 Point Arc (center, start/radius, endpoint); holding the Arc button exposes 1 Point Arc and 2 Point Arc. Arc input uses the same OSnap and Ortho constraints as Line, including endpoint, midpoint, intersection, center, perpendicular, and tangent candidates. With Ortho enabled, a 1 Point Arc endpoint stays on its fixed-radius circle and snaps to 90-degree sweep increments. The 1 Point Arc preserves the cursor's rotation direction through 180 degrees and stores the resulting sweep. The 2 Point Arc uses start, end, and a through point; with Ortho enabled, the through point snaps to either exact semicircle only when the cursor is near that semicircle snap point, while remaining free elsewhere. Finished arcs are stored as exact rational degree-2 NURBS curves: spans are joined at shared vertices, middle vertices use the circular-arc weights, and the curve stores a clamped Rhino/openNURBS knot array. Circles use the same rational degree-2 NURBS representation as Rhino's four-quarter-span circle. The Control Points toggle displays those NURBS control vertices, including the outer weighted vertices that are not the construction center/start/end points. The Bezier and NURBS buttons currently commit a single-span cubic NURBS from four control vertices, with degree, order, weights, and knots stored using the same representation; arbitrary multi-span NURBS editing is still ahead of the placeholder interaction.

## Build

Install the Qt development package for your platform and Blender 5.2.2 for
runtime `.vignola`/`.blend` save/open. Blender must be on `PATH`, or set
`CLASSICAD_BLENDER_EXECUTABLE` to its executable. Then run:

```sh
cmake -S . -B build
cmake --build build
./build/classiCAD
```

The CMake file accepts either Qt 6 or Qt 5 Widgets, preferring Qt 6 when both are available.

For an application-only build, use the isolated `app-dev` preset. To configure
and build the registered regression-test executables, use `full-test`; run its
CTest preset when you want to execute them. The `benchmarks` preset builds the
surface, erase-query, viewport query, and viewport runtime benchmarks. Each preset
writes under a separate `build/` subdirectory and leaves an existing `build/`
configuration untouched.

```sh
cmake --preset app-dev
cmake --build --preset app-dev

cmake --preset full-test
cmake --build --preset full-test
ctest --preset full-test

cmake --preset benchmarks
cmake --build --preset benchmarks
./build/benchmarks/classicad_erase_scene_query_benchmark
./build/benchmarks/classicad_viewport_query_benchmark
QT_QPA_PLATFORM=xcb ./build/benchmarks/classicad_viewport_runtime_benchmark
QT_QPA_PLATFORM=offscreen ./build/benchmarks/classicad_viewport_runtime_benchmark --history-only 1024
```

The runtime benchmark reports snap/pick and screen-sampling costs, CPU fallback
and cached native-GL scene redraw, per-object prepared-geometry invalidation,
and snapshot-history timing/RSS. Run it with the XCB platform for native OpenGL;
the offscreen platform measures the CPU path. GPU timings include the committed
scene renderer but exclude the grid and Qt overlays. `--history-only N` runs one
history scene size in a fresh process so its peak-RSS delta is interpretable.

## Blender-inspired architecture

Blender’s source separates the screen into areas and regions, builds controls through layout objects, and routes actions through operators. classiCAD will use the same broad separation while keeping the implementation smaller:

```text
Qt widgets/layouts  ->  C++ commands  ->  document/model  ->  NURBS geometry
Python tools later  ------------------------------^ 
```

The current source layout follows that boundary:

```text
src/main.cpp                 application entry point
src/app/*                     application session, commands, project, and update controllers
src/core/geometry/*           shared NURBS storage, factories, evaluation, validation, editing, and transforms
src/core/model.h              temporary compatibility umbrella; implementations live with their owning modules
src/core/document/*           document, layers, scene objects, settings, and stable selection IDs
src/core/history/*            document-level undo/redo and edit transactions
src/core/commands/*           transactional document operations
src/core/serialization/*      versioned document, project, session, and Rhino interchange
src/services/*                viewport transforms, sampling, hit-testing, query bounds, and snapping
src/tools/*                   named non-Qt interaction tool contracts and modules
src/core/debug_log.*         application logging
src/ui/input_helpers.*       Qt event and icon helpers
src/ui/viewport_widget_api.h typed viewport settings, command, status, and callback boundary
src/ui/viewport_widget.cpp   viewport event routing, selection, editing, and lifecycle state
src/ui/viewport/*             camera controller, OpenGL grid/scene, and typed transient renderers
src/ui/panels/*               tool shelf, layers, preferences, and document-grid panels
src/ui/main_window.*          menus, app composition, and window wiring
tests/arc_tool_contracts.cpp direct Arc tool input and cancellation contracts
tests/command_contracts.cpp  direct transactional document-command contracts
tests/scene_query_contracts.cpp projected control-hull and scene-query coverage
tests/trim_seam.cpp          focused Trim/Erase and seam regression coverage
tests/core_contracts.cpp     core, geometry, service, tool, and session contracts
tests/viewport_render_contracts.cpp layer line-style and GPU stroke contracts
tests/viewport_event_precedence.cpp public Qt event-routing regressions
tests/viewport_interaction.cpp Qt viewport input, rendering, and round-trip coverage
tests/vignola_file.cpp       Blender-native save/open and Rhino import coverage
```

The window talks to the viewport through `ViewportWidgetApi`: menu/edit actions
use typed viewport commands, while settings and UI notifications use explicit
contracts. Geometry and serialization live in `core`, keeping document
mutation and curve algorithms out of the Qt window code. The Layers panel uses
stable layer IDs for active/visible/locked state, ordering, and moving selected
objects between editable layers. CMake groups application and test sources by
the same `core`, `services`, `tools`, and `ui` folders, so the build files do
not maintain separate flat copies of the architecture.

The remaining compatibility paths are intentional migration boundaries: the
legacy `Shape` factories/JSON helpers in `core/model.*`, the document's
container-style viewport bridge, and the viewport's selection/state aliases
are still used by live editing and session-compatibility tests. Committed
planar curves use `NurbsCurve2D`; committed tensor-product surfaces use
`NurbsSurface3D` with Euclidean world-space XYZ control vertices and independent
reduced U/V knot arrays. Curve shapes store their oriented workplane frame with
a principal workplane fallback for legacy data. Exact geometry in these models
is the source of truth. Homogeneous Bezier spans, surface tessellation, display
meshes, and sample caches are transient algorithm/rendering data, not alternate
stored geometry.

Selected-object movement also supports a Blender-style grab flow: press `G`,
move the selection, press `X` or `Y` to constrain the axis, then click to
confirm or press `Esc`/right-click to cancel. Press `B` during Grab to choose
an enabled OSnap point on the selection as the move base; that point can then
be snapped onto another enabled OSnap point even when the global OSnap switch
is off. Ordinary click-drag movement continues to work as before.

Mirror is a copy command: select one or more editable objects, choose Mirror
(`M`), then click two points for the axis. The axis uses the same Ortho and
OSnap constraints as Line, the originals remain in place, and the mirrored
copies become selected. After the first axis point, a temporary mirrored
preview follows the constrained cursor until the second point commits it.
NURBS control points and component curves are reflected through the shared
geometry transform while their curve structure is retained.

## Blender-derived viewport grid

The 3D viewport grid uses a standalone OpenGL adaptation of Blender's 3D View
overlay grid shaders. It retains Blender's procedural line generation,
camera-relative snapping, three-level grid transitions, perspective/ortho
fades, low-alpha stipple, and additive passes. On desktop OpenGL systems, a
native viewport surface presents the grid, CAD curve strokes, and points directly
through OpenGL, including dashed Bézier/NURBS control guides. Pictures,
dimensions, tool previews, and other screen overlays still use Qt painting.
The offscreen fallback uses the original Qt scene path. A GPU depth prepass lets scene
curves, points, and picture planes occlude the grid, with Blender-style
perspective depth bias. The adapted shader sources and upstream references are
documented in `THIRD_PARTY_NOTICES.md`. The combined
application is licensed under GPL-2.0-or-later; see `COPYING`.
Grid units and base spacing are document settings in View > Grid Units and
Spacing; choosing a unit changes the displayed grid interval, not stored
geometry coordinates. Grid colors, opacity, and low-alpha stipple are
configurable in Edit > Preferences > Viewport, and the same appearance settings
are used by the OpenGL renderer and its Qt fallback.
Grid parity is being implemented in explicit stages; see
`BLENDER_VIEWPORT_PARITY_PLAN.md` for completed work and remaining differences.
