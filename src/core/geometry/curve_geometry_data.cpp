#include "curve_geometry_data.h"

namespace classiCAD {

HomogeneousControlPoint2D blendHomogeneousControlPoints(
    const HomogeneousControlPoint2D &first,
    const HomogeneousControlPoint2D &second,
    qreal secondFraction)
{
    const qreal firstFraction = 1.0 - secondFraction;
    return HomogeneousControlPoint2D{
        first.weightedPosition * firstFraction +
            second.weightedPosition * secondFraction,
        first.weightedNormalCoordinate * firstFraction +
            second.weightedNormalCoordinate * secondFraction,
        first.weight * firstFraction + second.weight * secondFraction};
}

} // namespace classiCAD
