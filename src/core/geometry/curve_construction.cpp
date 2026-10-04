#include "curve_construction.h"

#include <algorithm>
#include <cmath>

namespace classiCAD {

Shape::NurbsCurve2D makeDegreeOneNurbs(const QVector<QPointF> &points)
{
    Shape::NurbsCurve2D curve;
    curve.dimension = 2;
    curve.degree = 1;
    curve.order = 2;
    curve.rational = false;
    curve.controlPoints = points;
    curve.weights.fill(1.0, points.size());

    if (points.size() < 2) {
        return curve;
    }

    const int pointCount = points.size();
    curve.knots.reserve(pointCount);
    for (int index = 0; index < pointCount; ++index) {
        curve.knots.append(static_cast<double>(index));
    }
    return curve;
}

Shape::NurbsCurve2D makeBezierNurbs(const QVector<QPointF> &points)
{
    Shape::NurbsCurve2D curve;
    curve.dimension = 2;
    curve.controlPoints = points;
    curve.weights.fill(1.0, points.size());

    if (points.size() < 2) {
        return curve;
    }

    curve.degree = points.size() - 1;
    curve.order = curve.degree + 1;
    curve.rational = false;
    curve.knots.reserve(curve.controlPoints.size() + curve.degree - 1);
    for (int index = 0; index < curve.degree; ++index) {
        curve.knots.append(0.0);
    }
    for (int index = 0; index < curve.degree; ++index) {
        curve.knots.append(1.0);
    }
    return curve;
}

Shape::NurbsCurve2D makeCircleNurbs(const QVector<QPointF> &points)
{
    Shape::NurbsCurve2D curve;
    curve.dimension = 2;
    curve.degree = 2;
    curve.order = 3;
    curve.rational = true;

    if (points.size() < 2) {
        return curve;
    }

    constexpr qreal pi = 3.14159265358979323846;
    constexpr qreal quarterTurn = pi / 2.0;
    const QPointF center = points[0];
    const QPointF edge = points[1];
    const qreal radius =
        std::hypot(edge.x() - center.x(), edge.y() - center.y());
    if (radius <= 1.0e-9) {
        return curve;
    }

    const qreal startAngle = std::atan2(edge.y() - center.y(),
                                        edge.x() - center.x());
    const qreal middleWeight = std::cos(pi / 4.0);
    const qreal middleRadius = radius / middleWeight;
    curve.controlPoints.reserve(9);
    curve.weights.reserve(9);
    for (int span = 0; span < 4; ++span) {
        const qreal spanStart = startAngle + quarterTurn * span;
        const qreal spanEnd = spanStart + quarterTurn;
        const qreal spanMiddle = (spanStart + spanEnd) * 0.5;
        const auto circlePoint = [center, radius](qreal angle) {
            return QPointF(center.x() + radius * std::cos(angle),
                           center.y() + radius * std::sin(angle));
        };

        if (span == 0) {
            curve.controlPoints.append(circlePoint(spanStart));
            curve.weights.append(1.0);
        }
        curve.controlPoints.append(QPointF(
            center.x() + middleRadius * std::cos(spanMiddle),
            center.y() + middleRadius * std::sin(spanMiddle)));
        curve.weights.append(middleWeight);
        curve.controlPoints.append(circlePoint(spanEnd));
        curve.weights.append(1.0);
    }

    curve.knots = {0.0,
                   0.0,
                   quarterTurn,
                   quarterTurn,
                   pi,
                   pi,
                   3.0 * quarterTurn,
                   3.0 * quarterTurn,
                   2.0 * pi,
                   2.0 * pi};
    return curve;
}

Shape::NurbsCurve2D makeEllipseNurbs(EllipseMode mode,
                                    const QVector<QPointF> &points)
{
    Shape::NurbsCurve2D curve;
    curve.dimension = 2;
    curve.degree = 2;
    curve.order = 3;
    curve.rational = true;

    QPointF center;
    QPointF majorAxis;
    QPointF minorAxis;
    constexpr qreal minimumRadius = 1.0e-9;
    const auto length = [](const QPointF &vector) {
        return std::hypot(vector.x(), vector.y());
    };
    const auto perpendicular = [](const QPointF &vector) {
        return QPointF(-vector.y(), vector.x());
    };

    switch (mode) {
    case EllipseMode::CenterAxisRadius: {
        if (points.size() < 3) {
            return curve;
        }
        center = points[0];
        majorAxis = points[1] - center;
        minorAxis = points[2] - center;
        break;
    }
    case EllipseMode::AxisEndpoints: {
        if (points.size() < 3) {
            return curve;
        }
        center = (points[0] + points[1]) * 0.5;
        majorAxis = points[1] - center;
        minorAxis = points[2] - center;
        break;
    }
    case EllipseMode::Corners: {
        if (points.size() < 2) {
            return curve;
        }
        center = (points[0] + points[1]) * 0.5;
        majorAxis = QPointF((points[1].x() - points[0].x()) * 0.5, 0.0);
        minorAxis = QPointF(0.0, (points[1].y() - points[0].y()) * 0.5);
        break;
    }
    case EllipseMode::FociPoint: {
        if (points.size() < 3) {
            return curve;
        }
        const QPointF firstFocus = points[0];
        const QPointF secondFocus = points[1];
        center = (firstFocus + secondFocus) * 0.5;
        const QPointF focusAxis = secondFocus - firstFocus;
        const qreal focalDistance = length(focusAxis) * 0.5;
        const QPointF pointFromCenter = points[2] - center;
        const qreal majorRadius =
            (length(points[2] - firstFocus) + length(points[2] - secondFocus)) * 0.5;
        if (!std::isfinite(majorRadius) ||
            majorRadius <= focalDistance + minimumRadius) {
            return curve;
        }

        QPointF unitMajor;
        if (focalDistance > minimumRadius) {
            unitMajor = focusAxis / (2.0 * focalDistance);
        } else {
            const qreal pointRadius = length(pointFromCenter);
            if (pointRadius <= minimumRadius) {
                return curve;
            }
            unitMajor = pointFromCenter / pointRadius;
        }

        const QPointF unitMinor = perpendicular(unitMajor);
        const qreal minorRadius =
            std::sqrt(std::max<qreal>(0.0,
                                      majorRadius * majorRadius -
                                          focalDistance * focalDistance));
        majorAxis = unitMajor * majorRadius;
        minorAxis = unitMinor * minorRadius;
        break;
    }
    }

    const qreal majorRadius = length(majorAxis);
    if (!std::isfinite(majorRadius) || majorRadius <= minimumRadius) {
        return curve;
    }
    const QPointF unitMajor = majorAxis / majorRadius;
    const QPointF unitMinor = perpendicular(unitMajor);
    const qreal minorRadius = std::abs(QPointF::dotProduct(minorAxis, unitMinor));
    if (!std::isfinite(minorRadius) || minorRadius <= minimumRadius) {
        return curve;
    }

    constexpr qreal pi = 3.14159265358979323846;
    constexpr qreal quarterTurn = pi / 2.0;
    const qreal middleWeight = std::cos(pi / 4.0);
    const auto ellipsePoint = [center, unitMajor, unitMinor](qreal major,
                                                             qreal minor) {
        return center + unitMajor * major + unitMinor * minor;
    };

    curve.controlPoints = {
        ellipsePoint(majorRadius, 0.0),
        ellipsePoint(majorRadius, minorRadius),
        ellipsePoint(0.0, minorRadius),
        ellipsePoint(-majorRadius, minorRadius),
        ellipsePoint(-majorRadius, 0.0),
        ellipsePoint(-majorRadius, -minorRadius),
        ellipsePoint(0.0, -minorRadius),
        ellipsePoint(majorRadius, -minorRadius),
        ellipsePoint(majorRadius, 0.0),
    };
    curve.weights = {1.0,
                     middleWeight,
                     1.0,
                     middleWeight,
                     1.0,
                     middleWeight,
                     1.0,
                     middleWeight,
                     1.0};
    curve.knots = {0.0,
                   0.0,
                   quarterTurn,
                   quarterTurn,
                   pi,
                   pi,
                   3.0 * quarterTurn,
                   3.0 * quarterTurn,
                   2.0 * pi,
                   2.0 * pi};
    return curve;
}

QVector<QPointF> makeRectanglePoints(RectangleMode mode,
                                     const QVector<QPointF> &points)
{
    const int requiredPointCount = mode == RectangleMode::ThreePoint ? 3 : 2;
    if (points.size() < requiredPointCount) {
        return {};
    }

    if (mode == RectangleMode::CornerCorner) {
        const QPointF first = points[0];
        const QPointF second = points[1];
        return {first,
                QPointF(second.x(), first.y()),
                second,
                QPointF(first.x(), second.y())};
    }

    if (mode == RectangleMode::CenterCorner) {
        const QPointF center = points[0];
        const QPointF halfExtents = points[1] - center;
        return {center + halfExtents,
                center + QPointF(-halfExtents.x(), halfExtents.y()),
                center - halfExtents,
                center + QPointF(halfExtents.x(), -halfExtents.y())};
    }

    const QPointF first = points[0];
    const QPointF second = points[1];
    const QPointF edge = second - first;
    const qreal edgeLength = std::hypot(edge.x(), edge.y());
    if (edgeLength <= 1.0e-9) {
        return {first, second, second, first};
    }

    const QPointF edgeUnit = edge / edgeLength;
    const QPointF perpendicular(-edgeUnit.y(), edgeUnit.x());
    const qreal width = QPointF::dotProduct(points[2] - second, perpendicular);
    const QPointF offset = perpendicular * width;
    return {first, second, second + offset, first + offset};
}

QVector<QPointF> makeRegularPolygonPoints(PolygonMode mode,
                                          const QVector<QPointF> &points,
                                          int sides)
{
    if (points.size() < 2) {
        return {};
    }

    sides = std::clamp(sides, 3, 1001);
    if (mode == PolygonMode::Edge && sides % 2 == 0) {
        ++sides;
    }

    constexpr qreal pi = 3.14159265358979323846;
    constexpr qreal twoPi = 2.0 * pi;
    constexpr qreal minimumLength = 1.0e-9;
    QPointF center;
    qreal radius = 0.0;
    qreal startAngle = 0.0;

    if (mode == PolygonMode::CenterCorner) {
        center = points[0];
        const QPointF radiusVector = points[1] - center;
        radius = std::hypot(radiusVector.x(), radiusVector.y());
        if (radius <= minimumLength) {
            return {};
        }
        startAngle = std::atan2(radiusVector.y(), radiusVector.x());
    } else if (mode == PolygonMode::CenterTangent) {
        center = points[0];
        const QPointF apothemVector = points[1] - center;
        const qreal apothem = std::hypot(apothemVector.x(), apothemVector.y());
        if (apothem <= minimumLength) {
            return {};
        }
        const qreal halfStep = pi / sides;
        const qreal cosine = std::cos(halfStep);
        if (cosine <= minimumLength) {
            return {};
        }
        radius = apothem / cosine;
        startAngle = std::atan2(apothemVector.y(), apothemVector.x()) - halfStep;
    } else if (mode == PolygonMode::CornerCorner) {
        const QPointF first = points[0];
        const QPointF edge = points[1] - first;
        const qreal edgeLength = std::hypot(edge.x(), edge.y());
        if (edgeLength <= minimumLength) {
            return {};
        }
        const qreal halfStep = pi / sides;
        const qreal sine = std::sin(halfStep);
        if (sine <= minimumLength) {
            return {};
        }
        radius = edgeLength / (2.0 * sine);
        const qreal apothem = radius * std::cos(halfStep);
        const QPointF edgeUnit = edge / edgeLength;
        const QPointF leftNormal(-edgeUnit.y(), edgeUnit.x());
        center = (first + points[1]) * 0.5 + leftNormal * apothem;
        const QPointF toFirst = first - center;
        startAngle = std::atan2(toFirst.y(), toFirst.x());
    } else {
        const QPointF first = points[0];
        const QPointF span = points[1] - first;
        const qreal spanLength = std::hypot(span.x(), span.y());
        if (spanLength <= minimumLength) {
            return {};
        }
        const qreal halfStep = pi / sides;
        const qreal denominator = 1.0 + std::cos(halfStep);
        if (denominator <= minimumLength) {
            return {};
        }
        radius = spanLength / denominator;
        center = first + span * (radius / spanLength);
        const QPointF toFirst = first - center;
        startAngle = std::atan2(toFirst.y(), toFirst.x());
    }

    QVector<QPointF> vertices;
    vertices.reserve(sides);
    for (int index = 0; index < sides; ++index) {
        const qreal angle = startAngle + twoPi * index / sides;
        vertices.append(center + QPointF(std::cos(angle) * radius,
                                         std::sin(angle) * radius));
    }
    return vertices;
}

QVector<QPointF> makePictureFramePoints(const QPointF &firstCorner,
                                        const QPointF &cursorCorner,
                                        qreal imageAspectRatio)
{
    if (!std::isfinite(firstCorner.x()) || !std::isfinite(firstCorner.y()) ||
        !std::isfinite(cursorCorner.x()) || !std::isfinite(cursorCorner.y()) ||
        !std::isfinite(imageAspectRatio) || imageAspectRatio <= 0.0) {
        return {};
    }

    const qreal requestedWidth = std::abs(cursorCorner.x() - firstCorner.x());
    const qreal requestedHeight = std::abs(cursorCorner.y() - firstCorner.y());
    if (requestedWidth <= 1.0e-12 && requestedHeight <= 1.0e-12) {
        return {};
    }

    qreal width = requestedWidth;
    qreal height = requestedHeight;
    if (requestedWidth <= 1.0e-12) {
        width = requestedHeight * imageAspectRatio;
    } else if (requestedHeight <= 1.0e-12) {
        height = requestedWidth / imageAspectRatio;
    } else if (requestedWidth / requestedHeight > imageAspectRatio) {
        width = requestedHeight * imageAspectRatio;
    } else {
        height = requestedWidth / imageAspectRatio;
    }

    const qreal directionX = cursorCorner.x() < firstCorner.x() ? -1.0 : 1.0;
    const qreal directionY = cursorCorner.y() < firstCorner.y() ? -1.0 : 1.0;
    const QPointF opposite(firstCorner.x() + directionX * width,
                           firstCorner.y() + directionY * height);
    const qreal left = std::min(firstCorner.x(), opposite.x());
    const qreal right = std::max(firstCorner.x(), opposite.x());
    const qreal bottom = std::min(firstCorner.y(), opposite.y());
    const qreal top = std::max(firstCorner.y(), opposite.y());
    return {QPointF(left, top),
            QPointF(right, top),
            QPointF(right, bottom),
            QPointF(left, bottom)};
}

} // namespace classiCAD
