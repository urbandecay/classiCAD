#pragma once

#include "nurbs_curve.h"

namespace classiCAD {

// Builds a clamped, nonrational NURBS curve that passes through each input
// point using chord-length parameterization and global interpolation.
bool makeCentripetalCatmullRomNurbsCurve(const QVector<QPointF> &points,
                                         NurbsCurve2D *curve);

} // namespace classiCAD
