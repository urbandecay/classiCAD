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

- A single Line drawing session must commit one connected object. If its
  clicked world points occupy different planes, retain the planar NURBS runs
  as components of one PolyCurve with their own frames. Committing each run
  as a separate scene object lets selecting and dragging one edge tear the
  path apart. Detect arbitrary planes through the clicked points too; checking
  only the drawing plane and principal planes splits rotated planar loops.
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

## General snapping findings

- Compute snap geometry in the curve's stored workplane coordinates. Do not
  fit a circle or arc after projecting its points into screen space: an oblique
  view projects a circle as an ellipse and shifts center, midpoint,
  perpendicular, and tangent results. Project only when measuring cursor
  distance or drawing a marker.
- Intersection snaps must consider the stored NURBS spans for curved geometry,
  not just line-segment shapes. Use tessellation only to find candidate
  parameter intervals, then evaluate and refine intersections on the source
  curves.
- Preserve the full snap result through axis and plane constraints. Spatial
  snaps carry a world-space point as well as a plane-local point; use the world
  point when the constraint is spatial instead of reinterpreting the local
  projection in the active workplane.
- A curve's workplane origin is its local coordinate origin, not the identity
  of the geometric plane. Drawing tools may capture a different origin for
  every shape on the same plane. Compare coplanarity for drawing snaps, then
  map each target through world space into the active frame before computing
  the preview or clicked point; exact frame matching silently disables snaps
  after the first point.
- Verify snap behavior in oblique views and with intersections between curved
  objects. A top-view line-only test will not catch screen-space circle fitting
  or missing curve intersections.
- For dimension tools, log cursor-move snap results as well as clicks. A click
  can record `Near` successfully while the following preview move fails; click
  logs alone cannot identify which edge the preview is targeting.
- Keep `Near` acquisition tolerance large enough for the visible cursor-to-edge
  gap. In the angular-dimension trace, snapping stopped at a 12 px radius while
  the cursor was about 15 px from the adjacent edge; widen `Near` to 18 px while
  keeping specific snaps at 12 px.
- Box selection must project each object's local points through that object's
  stored frame into the view. Do not filter candidates to the active drawing
  plane or project every object's local coordinates as if they shared it.
- Left-to-right box selection uses the actual projected geometry bounds; do
  not enlarge those bounds by a screen-space margin, which forces unnecessary
  empty space between the object and the window edge.

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
- Corner-corner construction must preserve the two clicked adjacent vertices;
  choose the center on the side that makes the second click the next vertex.
- The polygon add-on still runs its implicit global-axis inference when the
  app's ortho modifier is active; it only skips inference for geometry snaps
  or Alt bypass. Do not suppress this behavior just because `orthoEnabled` is
  set.

## Rectangle port findings

- The user requested the line-then-width interaction shown in their reference
  images for the app's Corner, Corner entry. Use the add-on's edge construction
  stages there: first click anchors a line, second fixes the edge and begins the
  rectangle width preview, and the final click commits. The supplied add-on's
  named CornerCorner class uses opposite corners instead; document this explicit
  user override rather than claiming those source handlers are identical.
- Never commit the temporary edge as a finished rectangle through Enter or RMB.

- Drag snap sources must be lifted through the selected object's frame and
  converted to the fixed drag frame before distance checks. Convert movement
  into each object's local axes; preserve any normal movement in its frame
  origin. Do not change the input frame from hover while dragging. A marker
  on a target does not prove that the moved geometry reached that target.

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
- A valid OSnap must override automatic axis inference. In the height stage,
  calculate the height from the snapped world point, not the raw screen ray.
  Preserve the already chosen edge: an off-axis target supplies its height
  component rather than changing the rectangle's width. Do not apply legacy
  viewport rectangle/ellipse constraints before the active controller resolves
  its input; that can discard the snap before the preview receives it.
- Line drawing uses one spatial snap result for both its preview and marker.
  Do not let the viewport select a current-plane marker independently from the
  controller's 3D target; coincident projections on separate workplanes can
  otherwise produce inconsistent visible snaps.
- Distinguish drawing a line from dragging an existing line in Select mode.
  Logs with lineActive=0 and dragging=1 exercise object drag snapping. Compare
  its source and target endpoints in world XYZ and preserve the full world
  translation when applying a snap; projecting both into the drag plane loses
  depth and excluding off-plane targets makes visible endpoints unresponsive.
- Almost edge-on object planes magnify a few pixels of mouse motion into huge
  world displacements. For ordinary Select dragging in that case, resolve input
  on a camera-facing plane through the visible pick depth. Keep editing control
  points in the curve's own plane.
- Store the closed rectangle as a degree-1 NURBS and use its CVs for rendering,
  hit testing, corner snaps, control points, and sampling after edits.

## Point tool port findings

- Face extrusion extends the existing Extrude tool. Both the public command's
  source filter and the controller must accept planar faces; updating only the
  controller leaves the command unable to start. Faces default to their normal,
  including edge-on axis input and top-view drag fallback. X/Y/Z override it.
- Closed face extrusion is one stored `NurbsExtrusionSolid3D`, not three loose
  surface objects. Derived caps preserve exact trims and walls preserve rational
  boundary curves. Use face indices in tessellation-cache keys to prevent the
  base cap, top cap, and wall from sharing the same cached mesh.

- Extrude is one selection-driven command: points create edges and curves
  create exact NURBS surfaces. Capture both through the existing controller so
  spatial mouse input, free dragging, XYZ constraints, snaps, and completion
  behave identically. A separate curve command cleared selection before its
  controller began and was missing preview publication/rendering hooks.
- Mixed point/curve previews must draw surface previews even when edge
  previews succeeded on the GPU; one global GPU-preview flag must not suppress
  geometry that uses the painter path.
- Keep the shared Extrude HUD valid for both source types. The old hardcoded
  instruction referred only to selected points and overflowed the viewport.
- [x] Verify curve-only and mixed Extrude selections, visible previews,
  edge-on axis dragging, free displacement, native save/reload, and atomic Undo.

- `PointTool_ByLine` inherits `LineTool_Poly`. Shift direction locking applies
  even when OSnap is active, the axis lock is reset after each committed
  segment, and the tool commits points without line geometry. Reset the
  inferred axis when a geometry snap takes over so the active segment does not
  keep a stale axis color.
- For a constrained Point by Line snap, first resolve the hovered edge/axis
  crossing when edge or intersection snapping is enabled. A marker at the
  original edge point does not mean the constrained preview landed on the
  edge. If no crossing is available, project the snapped world point onto the
  constrained direction; do not project its screen ray instead.
- Point by Line and Point by Arcs self-snap their preview points whenever any
  OSnap mode is enabled, even when endpoint snapping is off. Compare that
  self-snap with the scene snap by cursor distance; do not gate it on the
  endpoint toggle or let a farther self point replace a nearer scene snap.
- The effective Point by Line `L` handler is inherited plane lock. The shared
  modal handler also advertises `L` for length, but that mapping is unreachable
  because the tool consumes `L` first. Direct numeric typing sets a length and
  Enter commits that point. Keep the UI aligned with the active handler rather
  than the conflicting hint.
- Point by Arcs uses a 125 px compass centered at the hover before the first
  center click, at the first arc center while choosing the second center, and
  at the active arc center afterward. Do not draw extra center/cursor crosses
  or a full second-circle outline at the second radius stage.
- Point by Arcs `L` toggles plane locking after drawing starts. When unlocked,
  the second arc center can be off the first arc's plane; when locked, project
  it onto the first arc's plane. `P` remaps the bridge and arcs into the
  perpendicular plane and changes the lock state with the perpendicular mode.
  The add-on only accepts `P` while choosing a radius or sweep, not while
  choosing the second arc center. Turning perpendicular mode off unlocks the
  current perpendicular plane; it does not restore the original plane. If `P`
  is turned on again after that unlock, the add-on uses world Z as its fallback
  reference normal unless `L` has locked a plane again. During radius stages,
  derive the perpendicular bridge from the displayed, angle-snapped radius
  endpoint, and size the stage-3 compass at Arc 1's center even though the
  pointer is choosing Arc 2's center.
- Match point marker defaults: world-axis crosses, 5 px for ordinary preview
  points, 3 px for final arc intersections, and a 50 px wide setup crosshair.
  The Point by Arcs compass is opaque black. Arc and guide colors match the
  add-on defaults; its separate color/size preferences still have no app-side
  controls.
- Point Center chooses the first selected edge chain, falling back to selected
  points, fits their best-fit plane and circle, and previews a smooth circle
  or Catmull-Rom outline plus the center. Edge Center is an OSnap midpoint
  operation; resolve the click's current midpoint instead of committing an
  earlier hover result.
- Point Center exits on its one-shot click or a finish key even when there is
  no valid fitted point. Edge Center stays active after a left-click without a
  midpoint, but Enter/Space/right-click exits even when no point is available.
