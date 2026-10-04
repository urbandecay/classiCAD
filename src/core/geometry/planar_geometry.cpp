#include "planar_geometry.h"

#include <cmath>

namespace classiCAD {

qreal crossProduct(const QPointF &a, const QPointF &b)
{
    return a.x() * b.y() - a.y() * b.x();
}

bool segmentIntersection(const QPointF &a,
                         const QPointF &b,
                         const QPointF &c,
                         const QPointF &d,
                         QPointF *intersection)
{
    const QPointF firstDirection = b - a;
    const QPointF secondDirection = d - c;
    const qreal denominator = crossProduct(firstDirection, secondDirection);

    if (std::abs(denominator) < 1e-9) {
        return false;
    }

    const QPointF betweenStarts = c - a;
    const qreal firstParameter =
        crossProduct(betweenStarts, secondDirection) / denominator;
    const qreal secondParameter =
        crossProduct(betweenStarts, firstDirection) / denominator;
    constexpr qreal tolerance = 1e-9;

    if (firstParameter < -tolerance || firstParameter > 1.0 + tolerance ||
        secondParameter < -tolerance || secondParameter > 1.0 + tolerance) {
        return false;
    }

    if (intersection != nullptr) {
        *intersection = a + firstDirection * firstParameter;
    }
    return true;
}

} // namespace classiCAD
