#pragma once

#include <QPointF>

namespace classiCAD {

qreal crossProduct(const QPointF &a, const QPointF &b);
bool segmentIntersection(const QPointF &a,
                         const QPointF &b,
                         const QPointF &c,
                         const QPointF &d,
                         QPointF *intersection);

} // namespace classiCAD
