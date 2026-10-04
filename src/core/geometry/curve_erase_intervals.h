#pragma once

#include "core/geometry/curve_geometry_data.h"
#include "core/geometry/nurbs_curve.h"
#include "core/document/shape.h"

#include <QVector>

namespace classiCAD {

// Selects whole parameter pieces intersected by an erase hit, stopping each
// piece at a real curve intersection. Closed-curve seams stay joined unless
// an intersection explicitly cuts the seam.
QVector<ParameterInterval> boundCurveEraseIntervals(
    const NurbsCurve2D &curve,
    const QVector<ParameterInterval> &hitIntervals,
    const QVector<qreal> &intersectionParameters);

// Builds the unchanged sections on either side of sorted erase intervals.
// Each result keeps the source degree, weights, and parameter range.
bool keepNurbsCurveOutsideIntervals(
    const NurbsCurve2D &source,
    const QVector<ParameterInterval> &removedIntervals,
    QVector<NurbsCurve2D> *remainingCurves);

// Rebuilds a trimmed scene shape from its surviving NURBS pieces, keeping
// component workplanes and grouping pieces that still meet in world space.
bool rebuildShapeFromCurveEraseFragments(
    const Shape &source,
    const QVector<NurbsCurve2D> &remainingCurves,
    const QVector<WorkPlaneFrame> &workPlaneFrames,
    qreal endpointTolerance,
    QVector<Shape> *replacementShapes);

} // namespace classiCAD
