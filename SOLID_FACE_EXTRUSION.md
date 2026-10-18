# Constrained NURBS solid face extrusion

Extrude supports three explicit face behaviors. The controls sit together in
the viewport footer and preview and commit use the same exact geometry path:

- Weld off: retain the source geometry and create a separate exact extrusion
  solid from the selected planar face. Coincident coordinates do not create
  shared topology or weld groups. No gap is introduced. The new end cap is
  selected and the source object retains its placement and geometry.
- Weld on, Extrude Manifold off: replace the selected face in the existing
  solid and add four exact ruled walls. Retain the old perimeter as a seam.
- Extrude Manifold on: resize the selected face and extend its adjacent planar
  patches at their shared corners, without adding the new perimeter seam.
  Turning Manifold on enables Weld; turning Weld off disables Manifold.

Connected operations use exact NURBS boundary patches, without a display mesh
becoming authoritative. One operation creates one undo entry.

The source solid must be a valid convex shell whose faces are untrimmed,
nonrational degree-one `2x2` NURBS patches. Every face must be planar and its
stored orientation must point out of the shell. The selected face must satisfy
the same conditions. Trimmed, curved, rational, nonplanar, or nonconvex shells
are rejected without changing the source object.

The extrusion vector follows the selected face's normal. Outward and inward
motions are supported. Connected inward movement must remain inside all
other face half-spaces. Tangential motion, collapse and crossing the opposite
boundary are rejected. Axis constraints remain available when parallel to the
selected face normal. Manifold corner updates must leave all adjoining patches
planar, convex, closed and consistently oriented. Unsupported results are
rejected before replacing source geometry.
The adjoining faces must be perpendicular to the selected face normal, matching
Blender's orthogonal-edge dissolve rule. Manifold does not tilt slanted walls;
oblique extrusion seeds are not resized through the implicit cap shortcut.

The moved connected face stays in its original face slot, which supplies both
the live highlight and completion selection. For ordinary welded extrusion,
four ruled quad patches join the old perimeter to the translated one. For
manifold extrusion, the adjacent patches keep their degrees, knots and weights
while their shared corner CVs move. A box retains six faces. The authoritative
connected boundary requires each edge to occur exactly twice. Implicit extrusion
caps can resize their stored base/vector directly in Manifold mode, including
supported trimmed caps; connected wall editing remains limited to planar quads.

Only one face per solid may be extruded in one operation. Multiple solids may
participate when they accept the same displacement. This contract does not
cover simultaneous extrusion of adjacent faces, arbitrary BRep topology
changes, Boolean operations, or extrusion of trimmed or curved faces.

Blender behavior reference: `use_dissolve_ortho_edges` in
https://github.com/blender/blender/blob/main/source/blender/bmesh/operators/bmo_extrude.cc
merges the new walls with orthogonal neighboring faces. The implementation
above provides this effect within the supported exact planar NURBS contract.
