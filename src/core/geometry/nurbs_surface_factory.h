#pragma once

#include "nurbs_curve.h"
#include "nurbs_surface.h"

namespace classiCAD {

// Creates the exact ruled surface swept by translating any planar or spatial
// NURBS curve. Each local control vertex is lifted through the curve's frame
// before the translated row is added to the surface control net.
bool makeNurbsExtrusionSurface(const NurbsCurve2D &curve,
                              const WorkPlaneFrame &curveFrame,
                              const Point3D &displacement,
                              NurbsSurface3D *surface);

// Creates a planar face from a closed planar NURBS curve. The underlying
// surface is a four-CV plane patch and the source curve is preserved exactly
// as its outer trim loop in UV space.
bool makeNurbsPlanarFillSurface(const NurbsCurve2D &curve,
                                const WorkPlaneFrame &curveFrame,
                                NurbsSurface3D *surface);

} // namespace classiCAD
