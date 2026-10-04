#pragma once

#include "core/geometry/nurbs_curve.h"
#include "core/geometry/work_plane.h"

#include <QVector>

namespace classiCAD {

struct NurbsCurveIntersectionResult {
    QVector<qreal> firstCurveParameters;
    int seedSolves = 0;
};

struct NurbsPointClosestParameter {
    qreal parameter = 0.0;
    qreal distanceSquared = 0.0;
};

// Finds intersections between planar curves in 3D workplane frames. Returned
// parameters belong to the first curve's original NURBS parameter domain.
NurbsCurveIntersectionResult intersectNurbsCurves(
    const NurbsCurve2D &firstCurve,
    const WorkPlaneFrame &firstFrame,
    const NurbsCurve2D &secondCurve,
    const WorkPlaneFrame &secondFrame);

// Finds the closest point on a NURBS curve to a point in the curve's local
// plane, preserving the curve's existing parameter domain.
bool closestNurbsParameterToPoint(const NurbsCurve2D &curve,
                                  const QPointF &point,
                                  NurbsPointClosestParameter *closest);

} // namespace classiCAD
