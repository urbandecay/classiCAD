#pragma once

#include <QPointF>
#include <QVector>

#include <QtGlobal>

namespace classiCAD {

struct LineSegment {
    QPointF start;
    QPointF end;
};

struct HomogeneousControlPoint2D {
    QPointF weightedPosition;
    qreal weight = 1.0;
};

struct RationalBezierSpan2D {
    qreal startParameter = 0.0;
    qreal endParameter = 0.0;
    QVector<HomogeneousControlPoint2D> controlPoints;
};

struct ParameterInterval {
    qreal start = 0.0;
    qreal end = 0.0;
};

HomogeneousControlPoint2D blendHomogeneousControlPoints(
    const HomogeneousControlPoint2D &first,
    const HomogeneousControlPoint2D &second,
    qreal secondFraction);

} // namespace classiCAD
