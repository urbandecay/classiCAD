# Connected NURBS corner editing

G in vertex mode moves the selected corner and every coincident control
vertex on connected editable NURBS surfaces. Coordinates are compared in
world space including object placement; unrelated control vertices stay fixed.
Each preview starts from the captured source geometry. Commit and cancel use
one history snapshot for all affected objects.

G in edge mode moves both endpoints of each selected surface edge by the same
world-space displacement. Every coincident control vertex on connected
editable surfaces follows either endpoint, so adjoining faces stay connected.
Free movement and the existing axis constraints work through the same Grab
interaction as vertex mode.

G in face mode translates the selected faces' complete control nets with one
shared world displacement. Their adjoining unselected faces deform through
the shared vertices. Component selection survives preview, cancel, commit,
and any exact subdivisions needed to keep junctions connected.

Persistent weld groups connect points, curves, surfaces, and solids during
Grab. Coincident coordinates on separate objects do not establish a link.
Each preview validates all affected objects before publishing any geometry.
Weld to Snap is shared with Extrude and applies to moved vertices and edges
at completion; source attachment never depends on that option. Destination
splitting and movement share one history operation, while Escape restores
the original geometry and selection without destination welding.

Vertex mode exposes open spline endpoints and degree-one curve corners.
Turning CP on exposes all curve control points; turning it off removes hidden
handles from the active vertex selection. Closed smooth curves have no false
endpoint at their stored NURBS seam.

An extrusion cannot describe an independently moved corner. Rectangular
extrusion solids therefore materialize their exact caps and degree-one wall
panels into `NurbsExtrusionSolid3D::boundaryFaces` on the first corner edit.
This optional connected boundary representation becomes authoritative for
evaluation, display, picking, persistence and transforms. The extrusion seed
remains construction history only. Every boundary edge must have exactly two
face occurrences. Faces are untrimmed nonrational bilinear NURBS patches;
their shared endpoints move together, keeping matching exact linear edges.
Face orientation is retained in `boundaryFaceReversed`.

Connected extrusions may contain a corner partway along another face's longer
edge. Before moving a solid corner, subdivide its exact bilinear boundary
patches at those junctions, propagating matching edge cuts to adjacent patches.
Each subpatch retains the corresponding original UV interval and orientation.
Selection and weld identities map by the original world positions rather than
by indices that subdivision can change. Moving the shared corner then updates
all its incident patch corners, including the previously unsplit edge. Twisted
bilinear faces remain smooth exact NURBS patches. Failed subdivision or invalid
movement preserves the original shape.

Arbitrary trimmed or curved extrusion boundaries are not promoted by this
operation. Their complete planar caps can still move exactly by updating the
affine extrusion seed and its displacement; rational ruled walls follow the
same cap translation. An unsupported or invalid edit preserves the prior visible geometry.
This contract does not introduce mesh modeling, Boolean operations, implicit
joining, or arbitrary BRep topology changes. Standalone NURBS patches preserve
their degrees, knots, weights and trims while their selected control vertices move.
