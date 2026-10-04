# classiCAD development instructions

## Curve and spline geometry

Every committed curve or spline type must use the shared `Shape::NurbsCurve2D`
representation and conform to Rhino/openNURBS conventions.

- Store the curve dimension, degree, order, rational state, control vertices,
  weights, and knot array.
- Always keep `order == degree + 1`.
- Use Rhino's reduced knot-array convention: the knot count is
  `control_vertex_count + order - 2`; do not store the two redundant outer
  entries from the mathematical full knot vector.
- Store rational curves as Euclidean 2D control-vertex positions plus weights
  internally. `Shape::workPlaneFrame` maps those local coordinates to world
  XYZ through an origin, orthonormal X/Y axes, and their right-handed normal.
  Legacy records without a frame use `Shape::workPlane` and
  `Shape::workPlaneOffset`, defaulting to XY at offset zero. When exporting to
  Rhino/openNURBS, lift each CV to world `(x, y, z)` first, then write rational
  homogeneous form `(weight * x, weight * y, weight * z, weight)`.
- Curves may lie on any oriented plane but remain local `NurbsCurve2D` data.
  Do not add nonplanar spatial curves or mesh geometry until their modeling and
  interchange contracts are designed. The viewport resolves the drawing frame
  before sending input to a tool. Hovering a planar scene object inherits its
  frame. In empty space, drawing matches the add-on fallback within the
  supported planes: perspective uses world XY through the origin; fixed
  orthographic views use XY, XZ, or YZ through the origin; oblique orthographic
  views use the principal plane whose normal is most aligned with the view.
  Each drawing tool must capture its reference frame at its first point and
  keep its anchor stable in world space. A tool modifier such as the add-on's
  perpendicular mode may derive a new drawing frame after that point; remap
  the preview and committed local geometry into that frame. Existing OSnap
  supplies snap points; do not add a second snap overlay or snap model for
  plane handling.
  Line follows the requested add-on behavior beyond the principal-plane
  fallback: oblique orthographic input uses the actual camera-facing plane;
  world-axis and normal constraints can leave the initial plane. Keep its
  transient input in world XYZ and commit the longest planar runs as separate
  local degree-1 NURBS curves with their own frames, in one history operation.
  Reuse the existing SnapEngine for spatial snap targets and markers.
- Use positive rational weights and nondecreasing knots. Preserve the curve's
  parameter domain when editing, evaluating, or exporting it.
- Circles and circular arcs must be exact rational degree-2 NURBS curves. Split
  circular geometry into spans no larger than 90 degrees, share endpoint CVs
  between adjacent spans, and use the correct circular-arc weights.
- A Bezier curve is a clamped single-span NURBS with degree equal to the number
  of control vertices minus one. A polyline is an open clamped degree-1 NURBS.
- The renderer, hit-testing, control-point display, transforms, and export code
  must use the stored NURBS curve plus its workplane mapping as their source of
  truth. Direct
  `QPainterPath`, ellipse, or cubic approximations are preview/fallback logic
  only and must not replace committed curve data.
- Do not silently merge separate curve objects. A future Join/PolyCurve command
  must explicitly preserve Rhino-style component curves, continuity, and
  parameterization.
- Before adding a new spline type, check the official Rhino/openNURBS
  representation and add a factory/validation path for it instead of inventing
  a separate geometry format.

## NURBS surface geometry

- Store committed tensor-product surfaces as `Shape::NurbsSurface3D`, separate
  from the planar curve representation. Store dimension, U/V degrees and
  orders, U/V control-vertex counts, rational state, Euclidean XYZ control
  vertices, weights, and independent U/V knot arrays. Keep each order equal to
  its degree plus one. Flatten the control net with V varying fastest:
  `uIndex * controlVertexCountV + vIndex`.
- Use Rhino's reduced knot-array convention independently in both directions:
  each knot count is `control_vertex_count + order - 2`. Require finite,
  nondecreasing knots and positive rational weights. Preserve each parameter
  domain when evaluating or exchanging surfaces.
- NURBS surface control vertices are Euclidean world-space XYZ positions;
  rational storage keeps those positions with one weight per control vertex.
  Rhino/openNURBS import maps its surface CV net and knots into this model.
  Native classiCAD JSON retains the exact surface. A Blender mesh, when used as
  a display proxy, is derived from that surface and is not authoritative.
- Extruding a selected planar NURBS curve creates an exact ruled surface:
  the U direction copies the curve's degree, order, knots, and control-point
  weights; the V direction is clamped degree one over `[0, 1]`; each source
  control vertex is paired with the same vertex translated by the extrusion
  vector, with its weight duplicated. This creates an open sheet without caps.
  Multiple selected curves create separate surface objects in one history
  operation; never merge their components implicitly.
- Extrude is one selection-driven tool: selected points create edges and
  selected curves create surfaces. Both use the existing Extrude interaction,
  spatial cursor, XYZ axis constraints, snap engine, preview, and completion.
  Mixed selections use one shared displacement and one history operation.
- Evaluation, rendering, hit-testing, selection bounds, transforms,
  serialization, and interchange use the stored surface as their source of
  truth. Tessellation and display meshes are derived data only. Do not create
  spatial NURBS curves or mesh modeling without a separate contract.

Reference documentation:

- https://developer.rhino3d.com/api/cpp/class_o_n___nurbs_curve.html
- https://developer.rhino3d.com/samples/cpp/create-nurbs-circle/
- https://developer.rhino3d.com/api/cpp/class_o_n___nurbs_surface.html
- https://developer.rhino3d.com/en/samples/cpp/create-nurbs-surface/

## Project workflow

- Keep the application UI in C++/Qt and the geometry model in C++.
- Use Python later for optional tools or automation without creating a second
  incompatible geometry model.
- Use `apply_patch` for source edits.
- Build with `cmake --build build` after C++ changes.
- Run an offscreen startup smoke test with
  `QT_QPA_PLATFORM=offscreen ./build/classiCAD` when practical.
- Preserve existing user changes and inspect `git status` before committing.

## Add-on tool ports

- Read `ADDON_TOOL_PORTING_ISSUES.md` completely before starting each add-on
  tool port. Add every newly discovered mismatch or failed assumption there
  while the port is in progress, then keep its checklist current.
- Port the add-on's effective behavior from its actual final method
  definitions, including later definitions that shadow earlier ones. Do not
  infer key behavior from a dead/overridden implementation or from a summary.

## Scalable refactoring

- Before any refactoring iteration, read `refactoring/MASTER_REFACTOR_PROMPT.md` completely.
- Keep that prompt's folder/file map and progress ledger current as the refactor advances.
- The prompt is a living architecture contract; follow its dependency direction, naming rules, migration phases, and validation requirements.
