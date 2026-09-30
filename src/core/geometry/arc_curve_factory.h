#pragma once

#include "nurbs_curve.h"

namespace classiCAD {

struct CircularArc2D {
    QPointF center;
    qreal radius = 0.0;
    qreal startAngle = 0.0;
    qreal sweepAngle = 0.0;
    NurbsCurve2D curve;
};

// Builds the circular arc from start through the third point to end. The
// chosen signed sweep follows that point order in the local workplane.
bool makeCircularArcThroughPoint(const QPointF &start,
                                 const QPointF &end,
                                 const QPointF &through,
                                 CircularArc2D *arc);

} // namespace classiCAD
