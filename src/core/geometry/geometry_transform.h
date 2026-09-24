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

} // namespace classiCAD
