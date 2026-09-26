#pragma once

#include "core/model.h"

namespace classiCAD {

// Reflect a shape across the infinite line defined by axisStart and axisEnd.
// The curve degree, order, weights, knots, and parameter domain are preserved;
// only Euclidean positions are transformed.
bool mirrorShapeAcrossLine(const Shape &source,
                           const QPointF &axisStart,
                           const QPointF &axisEnd,
                           Shape *mirrored);

// Move either duplicated endpoint CV of a closed NURBS curve while keeping
// the seam closed. Circle and ellipse shapes remain seam-linked even after
// an earlier edit has already separated the two CVs.
bool setClosedNurbsSeamControlPoint(Shape *shape,
                                    int controlPointIndex,
                                    const QPointF &position);

} // namespace classiCAD
