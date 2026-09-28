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
  internally. When exporting to Rhino/openNURBS, convert rational vertices to
  homogeneous form `(weight * x, weight * y, weight * z, weight)` with `z = 0`.
- Use positive rational weights and nondecreasing knots. Preserve the curve's
  parameter domain when editing, evaluating, or exporting it.
- Circles and circular arcs must be exact rational degree-2 NURBS curves. Split
  circular geometry into spans no larger than 90 degrees, share endpoint CVs
  between adjacent spans, and use the correct circular-arc weights.
- A Bezier curve is a clamped single-span NURBS with degree equal to the number
  of control vertices minus one. A polyline is an open clamped degree-1 NURBS.
- The renderer, hit-testing, control-point display, transforms, and future
  export code must use the stored NURBS curve as their source of truth. Direct
  `QPainterPath`, ellipse, or cubic approximations are preview/fallback logic
  only and must not replace committed curve data.
- Do not silently merge separate curve objects. A future Join/PolyCurve command
  must explicitly preserve Rhino-style component curves, continuity, and
  parameterization.
- Before adding a new spline type, check the official Rhino/openNURBS
  representation and add a factory/validation path for it instead of inventing
  a separate geometry format.

Reference documentation:

- https://developer.rhino3d.com/api/cpp/class_o_n___nurbs_curve.html
- https://developer.rhino3d.com/samples/cpp/create-nurbs-circle/

## Project workflow

- Keep the application UI in C++/Qt and the geometry model in C++.
- Use Python later for optional tools or automation without creating a second
  incompatible geometry model.
- Use `apply_patch` for source edits.
- Build with `cmake --build build` after C++ changes.
- Run an offscreen startup smoke test with
  `QT_QPA_PLATFORM=offscreen ./build/classiCAD` when practical.
- Preserve existing user changes and inspect `git status` before committing.

## Scalable refactoring

- Before any refactoring iteration, read `refactoring/MASTER_REFACTOR_PROMPT.md` completely.
- Keep that prompt's folder/file map and progress ledger current as the refactor advances.
- The prompt is a living architecture contract; follow its dependency direction, naming rules, migration phases, and validation requirements.
