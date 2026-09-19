# classiCAD

An initial Qt/C++ desktop shell for a 2D NURBS modeler.

The first pass is intentionally a Blender-inspired layout:

- top menu and workspace bar
- left vertical tool shelf
- central 2D viewport with grid, pan, and zoom
- right outliner and properties panel
- bottom coordinate/status bar
- Rhino-style Ortho and OSnap toggles; enabling OSnap reveals Endpoint, Midpoint, Intersection, Center, Perpendicular, and Tangent controls for drawing and moving geometry
- Edit > Preferences with Blender-style categories and a Keymap page for choosing MMB or RMB panning
- Edit > Undo and Redo actions with Ctrl+Z/Ctrl+Y shortcuts for geometry changes
- A stationary click of the configured pan button repeats the last completed tool; moving while holding it pans the viewport
- A Control Points toggle on the tool shelf displays handles for the selected line or curve; the setting is remembered

The drawing buttons are placeholders. The Line tool behaves as a continuous point-placement command: left-click plants points, the next segment previews under the cursor, and right-click finishes the chain. Finished chains are stored as open, clamped, degree-1 NURBS curves with unit weights and the Rhino/openNURBS knot-array convention. In Select mode, a left-click near a finished line highlights it and dragging translates the connected chain and its NURBS control points together. Move snapping allows any source point to snap to any enabled target type (for example, an endpoint to a midpoint), with a short breakaway distance so snapped geometry can be separated without fighting the snap correction.

Arc has a press-and-hold flyout. A normal click starts the default 1 Point Arc (center, start/radius, endpoint); holding the Arc button exposes 1 Point Arc and 2 Point Arc. Arc input uses the same OSnap and Ortho constraints as Line, including endpoint, midpoint, intersection, center, perpendicular, and tangent candidates. With Ortho enabled, a 1 Point Arc endpoint stays on its fixed-radius circle and snaps to 90-degree sweep increments. The 1 Point Arc preserves the cursor's rotation direction through 180 degrees and stores the resulting sweep. The 2 Point Arc uses start, end, and a through point; with Ortho enabled, the through point snaps to either exact semicircle only when the cursor is near that semicircle snap point, while remaining free elsewhere. Finished arcs are stored as exact rational degree-2 NURBS curves: spans are joined at shared vertices, middle vertices use the circular-arc weights, and the curve stores a clamped Rhino/openNURBS knot array. Circles use the same rational degree-2 NURBS representation as Rhino's four-quarter-span circle. The Control Points toggle displays those NURBS control vertices, including the outer weighted vertices that are not the construction center/start/end points. The Bezier and NURBS buttons currently commit a single-span cubic NURBS from four control vertices, with degree, order, weights, and knots stored using the same representation; arbitrary multi-span NURBS editing is still ahead of the placeholder interaction. Rhino `.3dm` reader/writer support is still planned.

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
