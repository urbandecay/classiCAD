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

## Rectangle port findings

- Review `rectangle_tools.py` together with `modal_core.py`,
  `text_entry_utils.py`, and the rectangle branches in `tool_previews.py` and
  `hud_overlay.py`. X/Y dimension entry comes from the shared modal handler.
- Center-corner numeric X/Y values are full width/height, although the tool
  uses half-extents internally. Preserve the cursor's sign when locking a
  dimension. Keep pending text in the X/Y HUD field until Enter applies it.
- Shift toggles square mode only in center-corner and corner-corner modes.
  Use the larger live dimension, or the larger locked dimension, for its side.
  Direct number typing enters the shared square side only in square mode.
- The two corner tools must not infer world-axis directions: that would zero
  one dimension and collapse the rectangle. P chooses a view-aligned vertical
  plane from the reference normal before resolving the cursor ray; using the
  floor snap point in that mode collapses its height.
- Three-point rectangles first define an edge in the reference plane, then
  resolve height along the line through the edge endpoint. P changes that
  height direction to the captured reference normal. It does not turn the
  first edge-picking stage into the corner tools' vertical plane.
- Use the tool's actual preview shape for OpenGL rendering and markers.
  Rebuilding a rectangle from generic pending points loses locked dimensions
  and perpendicular behavior. Suppress the generic HUD hint in every mode.
- Store the closed rectangle as a degree-1 NURBS and use its CVs for rendering,
  hit testing, corner snaps, control points, and sampling after edits.
