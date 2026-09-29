# classiCAD

A Qt/C++ CAD modeler with a 3D camera and exact 2D NURBS geometry drawn on
selectable world XY, XZ, and YZ workplanes. Spatial curves and mesh modeling
are not implemented yet.

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

The workspace bar selects the active XY/XZ/YZ drawing plane, its offset, and Top/Front/Right, Isometric, or Perspective camera views. Existing tools create and edit plane-local curves; selection, object snaps, trimming, and erase operate on the active plane. The native `.vignola` document stores each object’s workplane and offset. Rhino `.3dm` interchange lifts those planar NURBS curves into world XYZ; import accepts curves lying in XY, XZ, or YZ planes. Exported curves are still curves, not meshes, and arbitrary spatial NURBS and mesh modeling are not implemented yet.

The Line tool behaves as a continuous point-placement command: left-click plants points, the next segment previews under the cursor, and right-click finishes the chain. Finished chains are stored as open, clamped, degree-1 NURBS curves with unit weights and the Rhino/openNURBS knot-array convention. In Select mode, dragging a finished line translates its connected chain and its NURBS control points together. Move snapping allows any source point to snap to any enabled target type (for example, an endpoint to a midpoint), with a short breakaway distance so snapped geometry can be separated without fighting the snap correction.

Arc has a press-and-hold flyout. A normal click starts the default 1 Point Arc (center, start/radius, endpoint); holding the Arc button exposes 1 Point Arc and 2 Point Arc. Arc input uses the same OSnap and Ortho constraints as Line, including endpoint, midpoint, intersection, center, perpendicular, and tangent candidates. With Ortho enabled, a 1 Point Arc endpoint stays on its fixed-radius circle and snaps to 90-degree sweep increments. The 1 Point Arc preserves the cursor's rotation direction through 180 degrees and stores the resulting sweep. The 2 Point Arc uses start, end, and a through point; with Ortho enabled, the through point snaps to either exact semicircle only when the cursor is near that semicircle snap point, while remaining free elsewhere. Finished arcs are stored as exact rational degree-2 NURBS curves: spans are joined at shared vertices, middle vertices use the circular-arc weights, and the curve stores a clamped Rhino/openNURBS knot array. Circles use the same rational degree-2 NURBS representation as Rhino's four-quarter-span circle. The Control Points toggle displays those NURBS control vertices, including the outer weighted vertices that are not the construction center/start/end points. The Bezier and NURBS buttons currently commit a single-span cubic NURBS from four control vertices, with degree, order, weights, and knots stored using the same representation; arbitrary multi-span NURBS editing is still ahead of the placeholder interaction.

## Build

Install the Qt development package for your platform, then run:

```sh
cmake -S . -B build
cmake --build build
./build/classiCAD
```

The CMake file accepts either Qt 6 or Qt 5 Widgets, preferring Qt 6 when both are available.

## Blender-inspired architecture

Blender’s source separates the screen into areas and regions, builds controls through layout objects, and routes actions through operators. classiCAD will use the same broad separation while keeping the implementation smaller:

```text
Qt widgets/layouts  ->  C++ commands  ->  document/model  ->  NURBS geometry
Python tools later  ------------------------------^ 
```

The current source layout follows that boundary:

```text
src/main.cpp                 application entry point
src/core/geometry/*          shared NurbsCurve2D storage, workplane mapping, evaluation, validation, and transforms
src/core/model.*             compatibility shape records, factories, and legacy helpers
src/core/document/*          document, layers, scene objects, and stable selection IDs
src/core/history/*            document-level undo/redo snapshots
src/core/serialization/*     versioned document, layer, and object save/restore
src/services/*                viewport transforms, sampling, hit-testing, and snapping
src/tools/*                   named non-Qt interaction tool contracts and modules
src/core/debug_log.*         application logging
src/ui/input_helpers.*       Qt event and icon helpers
src/ui/viewport_widget_api.h typed viewport settings, command, status, and callback boundary
src/ui/viewport_widget.cpp   viewport event routing, selection, editing, and lifecycle state
src/ui/viewport/*             OpenGL 3D grid, geometry renderer, and transient overlays
src/ui/main_window.*         menus, tool shelf, preferences, and window wiring
tests/trim_seam.cpp          focused geometry/editing regression coverage
tests/core_contracts.cpp     core, service, tool, and session-contract coverage
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
are still used by live editing and session-compatibility tests. There is one
committed curve representation (`NurbsCurve2D`); each shape stores its
principal workplane mapping. Homogeneous Bezier spans and sample caches are
transient algorithm/rendering data, not alternate stored geometry.

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
