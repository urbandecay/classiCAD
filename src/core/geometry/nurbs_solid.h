#pragma once

#include "nurbs_surface.h"

namespace classiCAD {

// Initially a closed translational extrusion of an affine planar face.
// Supported corner edits materialize an authoritative connected boundary of
// exact bilinear NURBS patches (see CONNECTED_SURFACE_VERTEX_EDITING.md);
// Connected planar face extrusion and orthogonal steps follow SOLID_FACE_EXTRUSION.md.
// Face tessellations are derived display data, never solid geometry.
struct NurbsExtrusionSolid3D {
    NurbsSurface3D baseSurface;
    Point3D displacement;
    // Once a shared corner is edited, these exact connected NURBS patches
    // are authoritative. The original extrusion remains the construction seed.
    QVector<NurbsSurface3D> boundaryFaces;
    QVector<bool> boundaryFaceReversed;
};

bool materializeNurbsSolidBoundary(NurbsExtrusionSolid3D *solid);
// Inserts exact bilinear subpatches at shared edge junctions before corner
// movement, so a corner on a neighboring longer edge remains connected.
bool conformNurbsSolidBoundaryForVertexEditing(
    NurbsExtrusionSolid3D *solid, QVector<int> *originalFaceIndices = nullptr);

// Accepts affine degree-one patches with exact knot-inserted CV rows/columns.
bool nurbsSolidBaseFrame(const NurbsSurface3D &surface, WorkPlaneFrame *frame);
bool validateNurbsSolid(const NurbsExtrusionSolid3D &solid);
// Canonicalizes an affine base to 2x2 without changing its UV domain or trims.
bool makeNurbsExtrusionSolid(const NurbsSurface3D &surface,
                             const Point3D &displacement,
                             NurbsExtrusionSolid3D *solid);
bool extrudeNurbsSolidFace(const NurbsExtrusionSolid3D &solid,
                          int faceIndex,
                          const Point3D &displacement,
                          NurbsExtrusionSolid3D *result,
                          bool manifold = false,
                          int *resultFaceIndex = nullptr);
QVector<NurbsSurface3D> nurbsSolidFaces(const NurbsExtrusionSolid3D &solid);
// Unique cage vertices in face order and flattened U/V order, matching solid
// component selection and control-point transforms. Coincident positions are
// shared at the control-cage tolerance (squared distance <= 1e-16).
QVector<Point3D> nurbsSolidControlPoints(const NurbsExtrusionSolid3D &solid);
bool nurbsSolidFaceReversed(const NurbsExtrusionSolid3D &solid, int faceIndex);

} // namespace classiCAD
