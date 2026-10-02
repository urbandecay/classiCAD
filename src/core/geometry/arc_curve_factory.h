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

// Builds an exact rational circular arc from its center, radius, start angle,
// and signed sweep in local workplane coordinates.
bool makeCircularArcFromCenterSweep(const QPointF &center,
                                    qreal radius,
                                    qreal startAngle,
                                    qreal sweepAngle,
                                    CircularArc2D *arc);

} // namespace classiCAD
