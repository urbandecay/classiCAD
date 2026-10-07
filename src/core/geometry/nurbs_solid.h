#pragma once

#include "nurbs_surface.h"

namespace classiCAD {

// Initially a closed translational extrusion of an affine planar face.
// Supported corner edits materialize an authoritative connected boundary of
// exact bilinear NURBS patches (see CONNECTED_SURFACE_VERTEX_EDITING.md).
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

bool nurbsSolidBaseFrame(const NurbsSurface3D &surface, WorkPlaneFrame *frame);
bool validateNurbsSolid(const NurbsExtrusionSolid3D &solid);
bool makeNurbsExtrusionSolid(const NurbsSurface3D &surface,
                             const Point3D &displacement,
                             NurbsExtrusionSolid3D *solid);
QVector<NurbsSurface3D> nurbsSolidFaces(const NurbsExtrusionSolid3D &solid);
bool nurbsSolidFaceReversed(const NurbsExtrusionSolid3D &solid, int faceIndex);

} // namespace classiCAD
