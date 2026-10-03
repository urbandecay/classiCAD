# Add-on tool porting issues and checks

Read this file before every add-on tool port. Add newly found mismatches here
as soon as they are identified, and check the relevant items again before
calling a port complete. This log records behavior that was previously
misread or implemented incorrectly.

## Required source review

- Inspect the exact add-on source used for the requested port.
- Find the active implementation of every relevant input, update, preview,
  and commit handler. In Python, a later duplicate method definition replaces
  an earlier one; only the last definition is active.
- Compare the add-on's actual preview math and final geometry, not only its
  labels or its apparent interaction sequence.
- Do not add migration or compatibility work for old files unless the user
  explicitly asks for it.

## Geometry and input issues

- **`P` means a perpendicular drawing plane.** It must derive a 3D plane whose
  normal is perpendicular to the captured reference plane, then use that same
  frame for cursor projection, preview, and committed NURBS geometry. Rotating
  a 2D sizing vector by 90 degrees or reflecting a polygon across its edge is
  not perpendicular-plane behavior.
- Keep the first point stable in world space while a modifier changes the
  drawing frame. `ToolInput` point coordinates are local to the frame active
  when the viewport created that event. Convert through the event frame or
  resolve the mouse against the captured reference frame; never reinterpret
  those local coordinates in a newly selected frame.
- Reuse the existing OSnap result. Plane changes must not disable snapping or
  introduce a separate snap overlay/model.
- Preserve the model representation required by the tool. In particular,
  circles and circular arcs remain exact rational degree-2 NURBS; do not
  replace them with polygon segments because the preview looks similar.

## Preview, HUD, and controls

- Preview color comes from the active layer's configured color; do not
  substitute a generic yellow preview color.
- Keep the HUD readable and non-overlapping. Show the actual control name and
  mode; remove redundant instructions that the user can see from the geometry.
  A key hint must name its real action (for example, `P` means perpendicular);
  do not show stale or unexplained hints such as `base`, `off`, or an invented
  key absent from the add-on. When a tool has its own custom HUD, suppress the
  generic `drawToolStatus` instruction too; otherwise the two hints overlap.
- Numeric text is pending input until Enter applies it. Its preview and label
  must use the same geometric location/direction as the corresponding tool
  value (for example, a radius value stays on the radius direction).
- Escape exits the active drawing tool when that is the add-on behavior; it
  abandons the in-progress shape and must not leave the tool active with stale
  input.
- Keep preview rendering on the established OpenGL viewport path where the
  ported tool already uses it; do not silently replace it with a separate Qt
  painter preview.
- Match the add-on's key semantics: the line tool uses Shift to lock direction;
  do not add an unrelated `N` control while porting it.
- Arc angles use clockwise as the positive direction. The arc compass and
  sweep preview must stay aligned as the start point changes.

## Polygon port findings

- In `radCAD/operators/polygon_tools.py`, each polygon class defines
  `handle_input` more than once. The later version is effective: `P` toggles
  perpendicular mode and X/Y select the vertical fallback plane. Earlier
  X/Y/Z axis-constraint handlers are shadowed and must not be mistaken for the
  active key behavior.
- Perpendicular mode uses a reference normal and the 3D sizing bridge to
  derive the new plane. Near-vertical bridges need the add-on's view-based
  fallback, with X/Y selecting its plane normal. The add-on derives those
  fallback axes with `orthonormal_basis_from_normal(referenceNormal)`; do not
  silently substitute the captured frame's X/Y axes.
- Check all polygon modes (center-corner, center-tangent, corner-corner, and
  edge). A fix applied only to the easiest mode is not a complete port.
- The polygon add-on still runs its implicit global-axis inference when the
  app's ortho modifier is active; it only skips inference for geometry snaps
  or Alt bypass. Do not suppress this behavior just because `orthoEnabled` is
  set.
