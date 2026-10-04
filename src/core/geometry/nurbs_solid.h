#pragma once

#include "nurbs_surface.h"

namespace classiCAD {

// Closed translational extrusion of an affine planar face. The two caps share
// the exact UV trims of baseSurface; one ruled wall closes every trim loop.
// Face tessellations are derived display data, never solid geometry.
struct NurbsExtrusionSolid3D {
    NurbsSurface3D baseSurface;
    Point3D displacement;
};

bool nurbsSolidBaseFrame(const NurbsSurface3D &surface, WorkPlaneFrame *frame);
bool validateNurbsSolid(const NurbsExtrusionSolid3D &solid);
bool makeNurbsExtrusionSolid(const NurbsSurface3D &surface,
                             const Point3D &displacement,
                             NurbsExtrusionSolid3D *solid);
QVector<NurbsSurface3D> nurbsSolidFaces(const NurbsExtrusionSolid3D &solid);
bool nurbsSolidFaceReversed(const NurbsExtrusionSolid3D &solid, int faceIndex);

} // namespace classiCAD
