#include "circle_construction.h"

#include <algorithm>
#include <cmath>

namespace classiCAD {

bool makeCircleDefinitionFromDiameter(const QPointF &firstEndpoint,
                                      const QPointF &secondEndpoint,
                                      QVector<QPointF> *definition)
{
    if (definition == nullptr) {
        return false;
    }

    const QPointF radiusVector = (secondEndpoint - firstEndpoint) * 0.5;
    if (std::hypot(radiusVector.x(), radiusVector.y()) <= 1.0e-9) {
        return false;
    }

    const QPointF center = (firstEndpoint + secondEndpoint) * 0.5;
    *definition = {center, firstEndpoint};
    return true;
}

bool makeCircleDefinitionFromThreePoints(const QPointF &first,
                                         const QPointF &second,
                                         const QPointF &third,
                                         QVector<QPointF> *definition)
{
    if (definition == nullptr) {
        return false;
    }

    const QPointF firstChord = second - first;
    const QPointF secondChord = third - first;
    const qreal determinant = 2.0 * (firstChord.x() * secondChord.y() -
                                     firstChord.y() * secondChord.x());
    const qreal scale = std::max({std::hypot(firstChord.x(), firstChord.y()),
                                  std::hypot(secondChord.x(), secondChord.y()),
                                  std::hypot((third - second).x(),
                                             (third - second).y())});
    if (scale <= 1.0e-9 || std::abs(determinant) <= 1.0e-10 * scale * scale) {
        return false;
    }

    const qreal firstLengthSquared = firstChord.x() * firstChord.x() +
                                     firstChord.y() * firstChord.y();
    const qreal secondLengthSquared = secondChord.x() * secondChord.x() +
                                      secondChord.y() * secondChord.y();
    const QPointF centerOffset(
        (firstLengthSquared * secondChord.y() -
         secondLengthSquared * firstChord.y()) /
            determinant,
        (firstChord.x() * secondLengthSquared -
         secondChord.x() * firstLengthSquared) /
            determinant);
    const QPointF center = first + centerOffset;
    if (!std::isfinite(center.x()) || !std::isfinite(center.y())) {
        return false;
    }

    *definition = {center, first};
    return true;
}

} // namespace classiCAD
