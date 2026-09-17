# classiCAD

An initial Qt/C++ desktop shell for a 2D NURBS modeler.

The first pass is intentionally a Blender-inspired layout:

- top menu and workspace bar
- left vertical tool shelf
- central 2D viewport with grid, pan, and zoom
- right outliner and properties panel
- bottom coordinate/status bar
- Rhino-style Ortho and OSnap toggles; enabling OSnap reveals Endpoint, Midpoint, Intersection, and Center controls for drawing and moving geometry, plus Perpendicular for line drawing
- Edit > Preferences with Blender-style categories and a Keymap page for choosing MMB or RMB panning

The drawing buttons are placeholders. The Line tool behaves as a continuous point-placement command: left-click plants points, the next segment previews under the cursor, and right-click finishes the chain. Finished chains are stored as open, clamped, degree-1 NURBS curves with unit weights, which is the NURBS equivalent of connected straight line segments. In Select mode, a left-click near a finished line highlights it and dragging translates the connected chain and its NURBS control points together. Move snapping allows any source point to snap to any enabled target type (for example, an endpoint to a midpoint), with a short breakaway distance so snapped geometry can be separated without fighting the snap correction.

Arc, Bezier, NURBS, rectangle, and circle input currently draw lightweight preview geometry so the interaction can be tested before the full geometry kernel and Rhino `.3dm` reader/writer are added.

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
