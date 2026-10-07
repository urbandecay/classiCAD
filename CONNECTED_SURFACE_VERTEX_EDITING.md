# Connected NURBS corner editing

G in vertex mode moves the selected corner and every coincident control
vertex on connected editable NURBS surfaces. Coordinates are compared in
world space including object placement; unrelated control vertices stay fixed.
Each preview starts from the captured source geometry. Commit and cancel use
one history snapshot for all affected objects.

An extrusion cannot describe an independently moved corner. Rectangular
extrusion solids therefore materialize their exact caps and degree-one wall
panels into `NurbsExtrusionSolid3D::boundaryFaces` on the first corner edit.
This optional connected boundary representation becomes authoritative for
evaluation, display, picking, persistence and transforms. The extrusion seed
remains construction history only. Every boundary edge must have exactly two
face occurrences. Faces are untrimmed nonrational bilinear NURBS patches;
their shared endpoints move together, keeping matching exact linear edges.
Face orientation is retained in `boundaryFaceReversed`.

Arbitrary trimmed or curved extrusion boundaries are not promoted by this
operation. An unsupported or invalid edit preserves the prior visible geometry.
This contract does not introduce mesh modeling, Boolean operations, implicit
joining, or arbitrary BRep topology changes. Standalone NURBS patches preserve
their degrees, knots, weights and trims while their selected control vertices move.
