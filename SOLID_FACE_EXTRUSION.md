# Constrained NURBS solid face extrusion

Extrude supports three explicit face behaviors. The controls sit together in
the viewport footer and preview and commit use the same exact geometry path:

- Weld off: retain the source geometry and create a separate exact extrusion
  solid from the selected planar face. Coincident coordinates do not create
  shared topology or weld groups. No gap is introduced. The new end cap is
  selected and the source object retains its placement and geometry.
- Weld on, Extrude Manifold off: replace the selected face in the existing
  solid. Outward movement adds four exact ruled walls and retains the old
  perimeter as a seam. Inward movement shortens adjoining orthogonal patches
  and creates a step beside unchanged coplanar patches, without overlapping rims.
  Outward movement that fills an existing recess also updates its shared walls
  rather than adding coincident walls inside the filled region.
- Extrude Manifold on: resize the selected face and update its adjacent planar
  patches along their shared edges, without adding an overlapping perimeter rim.
  Turning Manifold on enables Weld; turning Weld off disables Manifold.

Connected operations use exact NURBS boundary patches, without a display mesh
becoming authoritative. One operation creates one undo entry.

Fill-created boxes with rectangular parameter-domain cap trims are converted
to the equivalent exact connected boundary before a face is edited. This
preserves face indices and supports all six faces with Weld or Manifold.

The source solid must be a valid closed shell whose materialized faces are untrimmed,
nonrational degree-one `2x2` NURBS patches. Every face must be planar and its
stored orientation must point out of the shell. The selected face must satisfy
the same conditions. Unsupported trimmed, curved, rational, or nonplanar shells
are rejected without changing the source object.

The extrusion vector follows the selected face's normal. Outward and inward
motions are supported, including steps in a previously extruded box. The swept
region is checked against opposing patches in its footprint. Tangential motion,
collapse and crossing the opposite boundary are rejected. Axis constraints
remain available when parallel to the
selected face normal. Manifold corner updates must leave all adjoining patches
planar, closed and consistently oriented. Unsupported results are
rejected before replacing source geometry.
Orthogonal edge neighbors shrink or extend with the moving face. Coplanar edge
neighbors remain at their original plane and receive a connecting step wall.
Faces touching only a corner are unchanged. Partial edge neighbors are split
exactly within their existing parameter domains before editing their shared
region. Manifold does not tilt slanted walls;
oblique extrusion seeds are not resized through the implicit cap shortcut.

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

Only one face per solid may be extruded in one operation. Multiple solids may
participate when they accept the same displacement. This contract does not
cover simultaneous extrusion of adjacent faces, arbitrary BRep topology
changes, Boolean operations, or extrusion of trimmed or curved faces.

Blender behavior reference: `use_dissolve_ortho_edges` in
https://github.com/blender/blender/blob/main/source/blender/bmesh/operators/bmo_extrude.cc
merges the new walls with orthogonal neighboring faces. The implementation
above provides this effect within the supported exact planar NURBS contract.
