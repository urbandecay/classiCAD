#include "viewport_renderer.h"

#include "core/geometry/curve_evaluator.h"

#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace classiCAD {

ViewportRenderer::ViewportRenderer(const ViewportTransform &transform,
                                   const CurveHitTester &curveHitTester)
    : transform_(transform)
    , curveHitTester_(curveHitTester)
{
}

QPointF ViewportRenderer::worldToScreen(const QPointF &world,
                                        const QSize &viewportSize) const
{
    return transform_.worldToScreen(world, viewportSize);
}

QPointF ViewportRenderer::screenToWorld(const QPointF &screen,
                                        const QSize &viewportSize) const
{
    return transform_.screenToWorld(screen, viewportSize);
}

bool ViewportRenderer::isValidNurbsCurve(const Shape::NurbsCurve2D &curve) const
{
    return validateNurbsCurve(curve);
}

bool ViewportRenderer::evaluateNurbsPoint(const Shape::NurbsCurve2D &curve,
                                          qreal parameter,
                                          QPointF *point) const
{
    return classiCAD::evaluateNurbsPoint(curve, parameter, point);
}

void ViewportRenderer::drawGrid(QPainter &painter,
                                const QSize &viewportSize) const
{
    const QPointF topLeft = screenToWorld(QPointF(0, 0), viewportSize);
    const QPointF bottomRight =
        screenToWorld(QPointF(viewportSize.width(), viewportSize.height()), viewportSize);
    constexpr qreal step = 25.0;

    painter.setPen(QPen(QColor(QStringLiteral("#353535")), 1));

    const qreal firstX = std::floor(topLeft.x() / step) * step;
    const qreal firstY = std::floor(bottomRight.y() / step) * step;

    for (qreal x = firstX; x <= bottomRight.x(); x += step) {
        const int screenX =
            qRound(worldToScreen(QPointF(x, 0), viewportSize).x());
        painter.drawLine(screenX, 0, screenX, viewportSize.height());
    }

    for (qreal y = firstY; y <= topLeft.y(); y += step) {
        const int screenY =
            qRound(worldToScreen(QPointF(0, y), viewportSize).y());
        painter.drawLine(0, screenY, viewportSize.width(), screenY);
    }
}

void ViewportRenderer::drawOrigin(QPainter &painter,
                                  const QSize &viewportSize) const
{
    const QPointF origin = worldToScreen(QPointF(0, 0), viewportSize);
    painter.setPen(QPen(QColor(QStringLiteral("#a85b5b")), 1));
    painter.drawLine(0, qRound(origin.y()), viewportSize.width(), qRound(origin.y()));
    painter.setPen(QPen(QColor(QStringLiteral("#628e65")), 1));
    painter.drawLine(qRound(origin.x()), 0, qRound(origin.x()), viewportSize.height());
}

QVector<QPointF> ViewportRenderer::rectangleVertices(const Shape &shape) const
{
    if (shape.geometryType != GeometryType::Rectangle || shape.points.size() < 2) {
        return {};
    }

    if (shape.points.size() >= 4) {
        return {shape.points[0], shape.points[1], shape.points[2], shape.points[3]};
    }

    const QPointF first = shape.points[0];
    const QPointF second = shape.points[1];
    return {first,
            QPointF(second.x(), first.y()),
            second,
            QPointF(first.x(), second.y())};
}

void ViewportRenderer::drawShape(QPainter &painter,
                                 const Shape &shape,
                                 const QSize &viewportSize,
                                 bool preview,
                                 bool selected) const
{
    if (shape.points.isEmpty()) {
        return;
    }

    const QColor curveColor = selected ? QColor(QStringLiteral("#5da9e9"))
                                       : preview ? QColor(QStringLiteral("#e6b85c"))
                                                 : QColor(QStringLiteral("#d28b45"));
    const QColor controlColor = QColor(QStringLiteral("#8aa7c7"));
    const qreal curveWidth = selected ? 3.5 : (preview ? 1.5 : 2.0);

    painter.setPen(QPen(curveColor, curveWidth));
    painter.setBrush(Qt::NoBrush);

    if (shape.geometryType == GeometryType::PolyCurve && !shape.components.isEmpty()) {
        for (const Shape::NurbsCurve2D &component : shape.components) {
            if (isValidNurbsCurve(component)) {
                drawNurbsCurve(painter, component, viewportSize);
            }
        }
    } else if (shape.geometryType == GeometryType::Point && !shape.points.isEmpty()) {
        painter.setPen(QPen(curveColor, selected ? 2.0 : 1.5));
        painter.setBrush(curveColor);
        painter.drawEllipse(worldToScreen(shape.points.first(), viewportSize),
                            selected ? 5.0 : 4.5,
                            selected ? 5.0 : 4.5);
    } else if (shape.geometryType == GeometryType::Line && shape.points.size() >= 2) {
        if (isValidNurbsCurve(shape.nurbs)) {
            drawNurbsCurve(painter, shape.nurbs, viewportSize);
        } else {
            for (int index = 0; index + 1 < shape.points.size(); ++index) {
                painter.drawLine(worldToScreen(shape.points[index], viewportSize),
                                 worldToScreen(shape.points[index + 1], viewportSize));
            }
        }
    } else if (shape.geometryType == GeometryType::Rectangle && shape.points.size() >= 2) {
        const QVector<QPointF> vertices = rectangleVertices(shape);
        if (vertices.size() >= 4) {
            QPainterPath rectanglePath;
            rectanglePath.moveTo(worldToScreen(vertices.first(), viewportSize));
            for (int index = 1; index < vertices.size(); ++index) {
                rectanglePath.lineTo(worldToScreen(vertices[index], viewportSize));
            }
            rectanglePath.closeSubpath();
            painter.drawPath(rectanglePath);
        }
    } else if (shape.geometryType == GeometryType::Circle && shape.points.size() >= 2) {
        if (isValidNurbsCurve(shape.nurbs)) {
            drawNurbsCurve(painter, shape.nurbs, viewportSize);
        } else {
            const QPointF center = worldToScreen(shape.points[0], viewportSize);
            const QPointF edge = worldToScreen(shape.points[1], viewportSize);
            const qreal radius =
                std::hypot(edge.x() - center.x(), edge.y() - center.y());
            painter.drawEllipse(center, radius, radius);
        }
    } else if (shape.geometryType == GeometryType::Arc && shape.points.size() >= 3) {
        if (isValidNurbsCurve(shape.nurbs)) {
            drawNurbsCurve(painter, shape.nurbs, viewportSize);
        } else if (shape.arcMode == ArcMode::OnePoint) {
            if (std::abs(shape.arcSweep) > 1e-9) {
                drawCenterArcWithSweep(painter,
                                       shape.points[0],
                                       shape.points[1],
                                       shape.arcSweep,
                                       viewportSize);
            } else {
                drawCenterArc(painter,
                              shape.points[0],
                              shape.points[1],
                              shape.points[2],
                              viewportSize);
            }
        } else {
            drawCircularArc(painter,
                            shape.points[0],
                            shape.points[1],
                            shape.points[2],
                            viewportSize);
        }
    } else if ((shape.geometryType == GeometryType::Bezier ||
                shape.geometryType == GeometryType::Nurbs) &&
               shape.points.size() >= 4) {
        const QVector<QPointF> controlPoints = shape.nurbs.controlPoints.isEmpty()
                                                   ? shape.points
                                                   : shape.nurbs.controlPoints;
        painter.setPen(QPen(controlColor, 1, Qt::DashLine));
        for (int index = 0; index + 1 < controlPoints.size(); ++index) {
            painter.drawLine(worldToScreen(controlPoints[index], viewportSize),
                             worldToScreen(controlPoints[index + 1], viewportSize));
        }

        painter.setPen(QPen(curveColor, curveWidth));
        if (isValidNurbsCurve(shape.nurbs)) {
            drawNurbsCurve(painter, shape.nurbs, viewportSize);
        } else {
            QPainterPath curve;
            curve.moveTo(worldToScreen(shape.points[0], viewportSize));
            curve.cubicTo(worldToScreen(shape.points[1], viewportSize),
                          worldToScreen(shape.points[2], viewportSize),
                          worldToScreen(shape.points[3], viewportSize));
            painter.drawPath(curve);
        }
    } else {
        painter.setPen(QPen(controlColor, 1, Qt::DashLine));
        for (int index = 0; index + 1 < shape.points.size(); ++index) {
            painter.drawLine(worldToScreen(shape.points[index], viewportSize),
                             worldToScreen(shape.points[index + 1], viewportSize));
        }
    }

    if (preview) {
        painter.setBrush(controlColor);
        painter.setPen(Qt::NoPen);
        for (const QPointF &point : shape.points) {
            painter.drawEllipse(worldToScreen(point, viewportSize), 4.0, 4.0);
        }
    }
}

bool ViewportRenderer::subdivisionCurve(const Shape &shape,
                                        Shape::NurbsCurve2D *curve) const
{
    if (curve == nullptr) {
        return false;
    }

    if (isValidNurbsCurve(shape.nurbs)) {
        *curve = shape.nurbs;
        return true;
    }

    if (shape.geometryType == GeometryType::Line && shape.points.size() >= 2) {
        *curve = makeDegreeOneNurbs(shape.points);
        return isValidNurbsCurve(*curve);
    }

    return false;
}

void ViewportRenderer::drawControlPoints(QPainter &painter,
                                         const Shape &shape,
                                         const QSize &viewportSize,
                                         ObjectId shapeObjectId,
                                         ObjectId selectedObjectId,
                                         bool draggingControlPoint,
                                         int activeControlPointIndex) const
{
    const QColor handleColor(QStringLiteral("#77b7e6"));
    const QColor handleFill(QStringLiteral("#263b4b"));
    painter.setPen(QPen(handleColor, 1.0, Qt::DashLine));
    painter.setBrush(Qt::NoBrush);

    int globalControlPointIndex = 0;
    const auto drawControlPointChain = [&](const QVector<QPointF> &controlPoints) {
        if (controlPoints.isEmpty()) {
            return;
        }

        for (int index = 0; index + 1 < controlPoints.size(); ++index) {
            painter.drawLine(worldToScreen(controlPoints[index], viewportSize),
                             worldToScreen(controlPoints[index + 1], viewportSize));
        }

        for (int index = 0; index < controlPoints.size(); ++index) {
            const bool active = draggingControlPoint &&
                                shapeObjectId == selectedObjectId &&
                                activeControlPointIndex == globalControlPointIndex + index;
            painter.setPen(QPen(active ? QColor(QStringLiteral("#f0a45a")) : handleColor,
                                1.5));
            painter.setBrush(active ? QColor(QStringLiteral("#f0a45a")) : handleFill);
            const QPointF screenPoint = worldToScreen(controlPoints[index], viewportSize);
            painter.drawRect(QRectF(screenPoint - QPointF(4.0, 4.0),
                                    screenPoint + QPointF(4.0, 4.0)));
        }
        globalControlPointIndex += controlPoints.size();
    };

    if (shape.geometryType == GeometryType::PolyCurve) {
        for (const Shape::NurbsCurve2D &component : shape.components) {
            drawControlPointChain(component.controlPoints);
        }
        return;
    }

    drawControlPointChain(curveHitTester_.controlPointsForShape(shape));
}

void ViewportRenderer::drawSubdivisionPoints(QPainter &painter,
                                             const Shape &shape,
                                             const QVector<double> &parameters,
                                             const QSize &viewportSize,
                                             bool preview) const
{
    if (parameters.isEmpty()) {
        return;
    }

    Shape::NurbsCurve2D curve;
    if (!subdivisionCurve(shape, &curve)) {
        return;
    }

    const QColor pointColor = preview ? QColor(QStringLiteral("#f0a45a"))
                                      : QColor(QStringLiteral("#e6b85c"));
    painter.setPen(QPen(pointColor, preview ? 2.0 : 1.5));
    painter.setBrush(pointColor);

    const QVector<double> fullKnots = expandedNurbsKnotVector(curve);
    if (fullKnots.size() > curve.controlPoints.size()) {
        const double firstParameter = fullKnots[curve.degree];
        const double lastParameter = fullKnots[curve.controlPoints.size()];
        for (const double parameter : {firstParameter, lastParameter}) {
            QPointF point;
            if (evaluateNurbsPoint(curve, parameter, &point)) {
                painter.drawEllipse(worldToScreen(point, viewportSize),
                                    preview ? 5.0 : 4.0,
                                    preview ? 5.0 : 4.0);
            }
        }
    }

    for (const double parameter : parameters) {
        QPointF point;
        if (!evaluateNurbsPoint(curve, parameter, &point)) {
            continue;
        }
        painter.drawEllipse(worldToScreen(point, viewportSize),
                            preview ? 5.0 : 4.0,
                            preview ? 5.0 : 4.0);
    }
}

void ViewportRenderer::drawNurbsCurve(QPainter &painter,
                                      const Shape::NurbsCurve2D &curve,
                                      const QSize &viewportSize) const
{
    if (!isValidNurbsCurve(curve)) {
        return;
    }

    const int controlPointCount = curve.controlPoints.size();
    const QVector<double> fullKnots = expandedNurbsKnotVector(curve);
    const qreal firstParameter = fullKnots[curve.degree];
    const qreal lastParameter = fullKnots[controlPointCount];
    int nonZeroSpans = 0;
    for (int index = curve.degree; index < controlPointCount; ++index) {
        if (fullKnots[index + 1] > fullKnots[index]) {
            ++nonZeroSpans;
        }
    }
    const int sampleCount = std::max(32, nonZeroSpans * 24);

    QPainterPath path;
    bool hasStart = false;
    for (int sample = 0; sample <= sampleCount; ++sample) {
        const qreal fraction = static_cast<qreal>(sample) / sampleCount;
        const qreal parameter = firstParameter +
                                (lastParameter - firstParameter) * fraction;
        QPointF point;
        if (!evaluateNurbsPoint(curve, parameter, &point)) {
            continue;
        }
        const QPointF screenPoint = worldToScreen(point, viewportSize);
        if (!hasStart) {
            path.moveTo(screenPoint);
            hasStart = true;
        } else {
            path.lineTo(screenPoint);
        }
    }

    if (hasStart) {
        painter.drawPath(path);
    }
}

bool ViewportRenderer::makeCircularArcGeometry(const QPointF &startWorld,
                                               const QPointF &endWorld,
                                               const QPointF &throughWorld,
                                               const QSize &viewportSize,
                                               QPointF *center,
                                               qreal *radius,
                                               qreal *startAngle,
                                               qreal *sweepAngle) const
{
    const QPointF start = worldToScreen(startWorld, viewportSize);
    const QPointF end = worldToScreen(endWorld, viewportSize);
    const QPointF through = worldToScreen(throughWorld, viewportSize);

    const qreal startSquared = start.x() * start.x() + start.y() * start.y();
    const qreal endSquared = end.x() * end.x() + end.y() * end.y();
    const qreal throughSquared = through.x() * through.x() + through.y() * through.y();
    const qreal denominator = 2.0 *
        (start.x() * (end.y() - through.y()) +
         end.x() * (through.y() - start.y()) +
         through.x() * (start.y() - end.y()));

    if (std::abs(denominator) < 1e-9) {
        return false;
    }

    const QPointF circleCenter(
        (startSquared * (end.y() - through.y()) +
         endSquared * (through.y() - start.y()) +
         throughSquared * (start.y() - end.y())) / denominator,
        (startSquared * (through.x() - end.x()) +
         endSquared * (start.x() - through.x()) +
         throughSquared * (end.x() - start.x())) / denominator);
    const qreal circleRadius =
        std::hypot(start.x() - circleCenter.x(), start.y() - circleCenter.y());
    if (circleRadius <= 1e-9) {
        return false;
    }

    constexpr qreal twoPi = 6.28318530717958647692;
    const auto normalizeAngle = [twoPi](qreal angle) {
        angle = std::fmod(angle, twoPi);
        if (angle < 0.0) {
            angle += twoPi;
        }
        return angle;
    };

    const qreal firstAngle = std::atan2(start.y() - circleCenter.y(),
                                        start.x() - circleCenter.x());
    const qreal secondAngle = std::atan2(end.y() - circleCenter.y(),
                                         end.x() - circleCenter.x());
    const qreal throughAngle = std::atan2(through.y() - circleCenter.y(),
                                          through.x() - circleCenter.x());
    const qreal counterClockwiseSweep = normalizeAngle(secondAngle - firstAngle);
    const qreal throughSweep = normalizeAngle(throughAngle - firstAngle);
    if (counterClockwiseSweep <= 1e-9) {
        return false;
    }

    const qreal selectedSweep = throughSweep <= counterClockwiseSweep + 1e-7
                                    ? counterClockwiseSweep
                                    : -(twoPi - counterClockwiseSweep);
    if (center != nullptr) {
        *center = circleCenter;
    }
    if (radius != nullptr) {
        *radius = circleRadius;
    }
    if (startAngle != nullptr) {
        *startAngle = firstAngle;
    }
    if (sweepAngle != nullptr) {
        *sweepAngle = selectedSweep;
    }
    return true;
}

void ViewportRenderer::drawCircularArc(QPainter &painter,
                                       const QPointF &start,
                                       const QPointF &end,
                                       const QPointF &through,
                                       const QSize &viewportSize) const
{
    QPointF center;
    qreal radius = 0.0;
    qreal startAngle = 0.0;
    qreal sweepAngle = 0.0;
    if (!makeCircularArcGeometry(start,
                                 end,
                                 through,
                                 viewportSize,
                                 &center,
                                 &radius,
                                 &startAngle,
                                 &sweepAngle)) {
        painter.drawLine(worldToScreen(start, viewportSize),
                         worldToScreen(end, viewportSize));
        return;
    }

    const int steps = std::clamp(
        static_cast<int>(std::ceil(std::abs(sweepAngle) * radius / 8.0)),
        12,
        256);
    QPainterPath path;
    for (int step = 0; step <= steps; ++step) {
        const qreal fraction = static_cast<qreal>(step) / steps;
        const qreal angle = startAngle + sweepAngle * fraction;
        const QPointF point(center.x() + radius * std::cos(angle),
                            center.y() + radius * std::sin(angle));
        if (step == 0) {
            path.moveTo(point);
        } else {
            path.lineTo(point);
        }
    }
    painter.drawPath(path);
}

void ViewportRenderer::drawCenterArcWithSweep(QPainter &painter,
                                              const QPointF &centerWorld,
                                              const QPointF &startWorld,
                                              qreal sweepAngle,
                                              const QSize &viewportSize) const
{
    const QPointF center = worldToScreen(centerWorld, viewportSize);
    const QPointF start = worldToScreen(startWorld, viewportSize);
    const qreal radius = std::hypot(start.x() - center.x(), start.y() - center.y());
    if (radius <= 1e-9 || std::abs(sweepAngle) <= 1e-9) {
        return;
    }

    const qreal startAngle = std::atan2(start.y() - center.y(),
                                        start.x() - center.x());
    const int steps = std::clamp(
        static_cast<int>(std::ceil(std::abs(sweepAngle) * radius / 8.0)),
        12,
        256);
    QPainterPath path;
    for (int step = 0; step <= steps; ++step) {
        const qreal fraction = static_cast<qreal>(step) / steps;
        const qreal angle = startAngle + sweepAngle * fraction;
        const QPointF point(center.x() + radius * std::cos(angle),
                            center.y() + radius * std::sin(angle));
        if (step == 0) {
            path.moveTo(point);
        } else {
            path.lineTo(point);
        }
    }
    painter.drawPath(path);
}

void ViewportRenderer::drawCenterArc(QPainter &painter,
                                     const QPointF &centerWorld,
                                     const QPointF &startWorld,
                                     const QPointF &endWorld,
                                     const QSize &viewportSize) const
{
    const QPointF center = worldToScreen(centerWorld, viewportSize);
    const QPointF start = worldToScreen(startWorld, viewportSize);
    const QPointF end = worldToScreen(endWorld, viewportSize);
    const qreal radius = std::hypot(start.x() - center.x(), start.y() - center.y());
    if (radius <= 1e-9) {
        painter.drawLine(start, end);
        return;
    }

    constexpr qreal pi = 3.14159265358979323846;
    constexpr qreal twoPi = 2.0 * pi;
    const qreal startAngle = std::atan2(start.y() - center.y(),
                                        start.x() - center.x());
    const qreal endAngle = std::atan2(end.y() - center.y(),
                                      end.x() - center.x());
    qreal sweepAngle = endAngle - startAngle;
    if (sweepAngle > pi) {
        sweepAngle -= twoPi;
    } else if (sweepAngle < -pi) {
        sweepAngle += twoPi;
    }

    if (std::abs(sweepAngle) <= 1e-9) {
        painter.drawLine(start, end);
        return;
    }
    drawCenterArcWithSweep(painter,
                           centerWorld,
                           startWorld,
                           sweepAngle,
                           viewportSize);
}

} // namespace classiCAD
