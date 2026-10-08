#pragma once

#include "core/geometry/nurbs_curve.h"
#include "core/geometry/work_plane.h"

#include <QVector>

namespace classiCAD {

struct NurbsCurveIntersection {
    qreal firstCurveParameter = 0.0;
    qreal secondCurveParameter = 0.0;
};

struct NurbsCurveIntersectionResult {
    QVector<NurbsCurveIntersection> intersections;
    // Kept for existing callers that only need parameters on the first curve.
    QVector<qreal> firstCurveParameters;
    int seedSolves = 0;
};

struct NurbsPointClosestParameter {
    qreal parameter = 0.0;
    qreal distanceSquared = 0.0;
};

// Finds intersections between planar curves in 3D workplane frames. Paired
// results preserve each curve's original NURBS parameter domain; the legacy
// firstCurveParameters list remains available to callers that need only the
// first curve's parameters.
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
