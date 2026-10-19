# Constrained NURBS solid face extrusion

Extrude supports two connected face behaviors. The controls sit together in
the viewport footer and preview and commit use the same exact geometry path:

- Extrude with Division (`E`, then `E`, or the Extrude hover menu): replace the selected face in the existing
  solid. Outward movement adds four exact ruled walls and retains the old
  perimeter as a seam. Inward movement shortens adjoining orthogonal patches
  and creates a step beside unchanged coplanar patches, without overlapping rims.
  Outward movement that fills an existing recess also updates its shared walls
  rather than adding coincident walls inside the filled region.
- Extrude (`E`, the default): resize the selected face and update its adjacent planar
  patches along their shared edges, without adding an overlapping perimeter rim.

Source connectivity is mandatory in both modes. The separate `Weld to Snap (W)`
option controls attachment to a supported external snap target; it never detaches
the extrusion from its selection. The internal `manifold` flag is true for
ordinary Extrude and false for Extrude with Division. Pressing E again while
the tool is active switches between these modes before committing.

Whole loose points and curves become their extrusion under the same object ID;
an edge extracted from an existing face keeps its owner and attaches the new
sheet to that boundary. Adjacent standalone planar face sweeps remove matching
opposing walls and become one connected solid, retaining every translated cap
in the completion selection. Disconnected selections remain separate objects.

Destination welding uses persistent control-point groups, with exact knot
insertion on degree-one curves and surface boundaries for partial overlaps.
Solid vertices use the same unique cage ordering as component selection and
their groups persist in native JSON. General curved intersections and face
interior cuts are outside this operation.
Affine planar faces that acquired extra degree-one knots while welding remain
extrudable: their exact UV map is validated and canonicalized to a 2x2 seed
without changing its parameter domains or trims. Existing shared boundary
vertices are reinserted in the new solid and retain their weld groups even
when destination welding is disabled. Partial shared walls between unequal
adjacent face sweeps are partitioned before their internal wall pieces are
removed.

Connected operations use exact NURBS boundary patches, without a display mesh
becoming authoritative. One operation creates one undo entry.

`core/geometry/nurbs_solid_face_extrusion.*` owns the complete operation. Its
result contains the candidate solid, the surviving selected face index, and a
specific rejection reason. The tool builds that candidate once for each input
change; status, preview and commit consume that same value. There is no cap
resize implementation in the tool. Rejected movement disables commit and keeps
the last accepted display preview until the next accepted candidate or a mode
change. Source geometry, option values, rejection transitions and completed
face indices are logged so the exact operation can be reconstructed.

Fill-created boxes with rectangular parameter-domain cap trims are converted
to the equivalent exact connected boundary before a face is edited. This
preserves face indices and supports all six faces in either mode.

The source solid must be a valid closed shell whose materialized faces are untrimmed,
nonrational degree-one `2x2` NURBS patches. Every face must be planar and its
stored orientation must point out of the shell. The selected face must satisfy
the same conditions. Unsupported trimmed, curved, rational, or nonplanar shells
are rejected without changing the source object.

The extrusion vector follows the selected face's normal. Outward and inward
motions are supported, including steps in a previously extruded box. Tangential motion,
collapse and crossing the opposite boundary are rejected. Axis constraints
remain available when parallel to the
selected face normal. Manifold corner updates must leave all adjoining patches
planar, closed and consistently oriented. Unsupported results are
rejected before replacing source geometry.
The shared connected engine supports shells of mutually orthogonal rectangular
patches, in any world orientation. It constructs an analytic arrangement from
the exact source-face coordinates and requested end plane. Closed-boundary
parity classifies material; the selected rectangular sweep adds material for
outward movement and removes it for inward movement. A result wall exists
exactly where one side contains material and the other does not. This replaces
procedural neighbor resizing and handles multiple partial edge neighbors,
disappearing steps, and steps that change direction with the same rule.

The arrangement is transient and has no fixed sampling resolution. Output is
exact rectangular degree-one NURBS geometry. Original face ownership retains
unaffected seams; ordinary outward extrusion labels its new perimeter walls
separately. Manifold merges those walls with adjoining coplanar faces.
Subdivisions preserve source parameter subdomains; resized or reversed walls
retain their original domains. Every returned boundary is checked for closure,
and the complete translated selected face must survive. Operations exceeding
two million arrangement cells are rejected with an explicit complexity reason.

This is a restricted rectangular-face sweep contract, not an arbitrary Boolean
or general BRep API. The core also preserves ordinary outward extrusion of a
face on a convex planar quad shell, and exact implicit planar-cap resizing for
trimmed translational seeds. Manifold does not tilt slanted walls;
oblique extrusion seeds are not resized through the implicit cap path.

The geometry operation reports the moving face's resulting slot to both preview
and completion selection. A zero-height step wall is removed when the moving
face reaches its neighbor; the wall reverses naturally when the face moves past
that neighbor. Removing a wall remaps later face slots. For ordinary welded outward
extrusion, four ruled quad patches join the old perimeter to the translated one.
For manifold extrusion, adjacent patches keep their degrees and weights, and
exact subdivision retains each piece's parameter subdomain. A whole box face
retains six faces. A partial face beside a coplanar neighbor creates a step.
The authoritative
connected boundary requires each geometric edge segment to occur exactly twice,
including when a longer patch edge meets multiple shorter edges. Implicit extrusion
caps can resize their stored base/vector directly in Manifold mode, including
supported trimmed caps; connected wall editing remains limited to planar quads.

Multiple coplanar rectangular faces of one solid may extrude together with one
shared displacement and matching outward normals. Their sweep is evaluated as
one region, so shared internal edges do not produce internal walls. The result
reports each moving face's resulting slot in input order. Separate selected
solids may participate when they accept the same displacement. Noncoplanar
multi-face selections are rejected explicitly. Arbitrary BRep changes, Boolean
operations, and curved-face extrusion remain unsupported.

Blender behavior reference: `use_dissolve_ortho_edges` in
https://github.com/blender/blender/blob/main/source/blender/bmesh/operators/bmo_extrude.cc
merges the new walls with orthogonal neighboring faces. The implementation
above provides this effect within the supported exact planar NURBS contract.
