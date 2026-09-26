#include "curve_hit_tester.h"

#include "core/geometry/curve_evaluator.h"

#include <algorithm>
#include <cmath>

namespace classiCAD {

qreal CurveHitTester::distanceToSegment(const QPointF &point,
                                        const QPointF &start,
                                        const QPointF &end) const
{
    const QPointF direction = end - start;
    const QPointF fromStart = point - start;
    const qreal lengthSquared = QPointF::dotProduct(direction, direction);
    if (lengthSquared <= 1.0e-12) {
        return std::hypot(point.x() - start.x(), point.y() - start.y());
    }

    const qreal projection = std::clamp(
        QPointF::dotProduct(fromStart, direction) / lengthSquared,
        0.0,
        1.0);
    const QPointF closest = start + direction * projection;
    return std::hypot(point.x() - closest.x(), point.y() - closest.y());
}

QVector<QPointF> CurveHitTester::rectangleVertices(const Shape &shape) const
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

qreal CurveHitTester::distanceToNurbsCurve(
    const QPointF &screenPosition,
    const Shape::NurbsCurve2D &curve,
    const ViewportTransform &transform,
    const QSize &viewportSize) const
{
    if (!validateNurbsCurve(curve)) {
        return 1.0e9;
    }

    qreal firstParameter = 0.0;
    qreal lastParameter = 0.0;
    if (!nurbsParameterDomain(curve, &firstParameter, &lastParameter)) {
        return 1.0e9;
    }

    const QVector<double> fullKnots = expandedNurbsKnotVector(curve);
    int nonZeroSpans = 0;
    for (int index = curve.degree; index < curve.controlPoints.size(); ++index) {
        if (fullKnots[index + 1] > fullKnots[index]) {
            ++nonZeroSpans;
        }
    }

    const int sampleCount = std::max(64, nonZeroSpans * 32);
    QPointF previousWorld;
    if (!evaluateNurbsPoint(curve, firstParameter, &previousWorld)) {
        return 1.0e9;
    }

    qreal closestDistance = 1.0e9;
    QPointF previous = transform.worldToScreen(previousWorld, viewportSize);
    for (int sample = 1; sample <= sampleCount; ++sample) {
        const qreal fraction = static_cast<qreal>(sample) / sampleCount;
        const qreal parameter = firstParameter +
                                (lastParameter - firstParameter) * fraction;
        QPointF currentWorld;
        if (!evaluateNurbsPoint(curve, parameter, &currentWorld)) {
            continue;
        }
        const QPointF current = transform.worldToScreen(currentWorld, viewportSize);
        closestDistance = std::min(closestDistance,
                                   distanceToSegment(screenPosition,
                                                    previous,
                                                    current));
        previous = current;
    }
    return closestDistance;
}

bool CurveHitTester::makeCircularArcGeometry(
    const QPointF &startWorld,
    const QPointF &endWorld,
    const QPointF &throughWorld,
    const ViewportTransform &transform,
    const QSize &viewportSize,
    QPointF *center,
    qreal *radius,
    qreal *startAngle,
    qreal *sweepAngle) const
{
    const QPointF start = transform.worldToScreen(startWorld, viewportSize);
    const QPointF end = transform.worldToScreen(endWorld, viewportSize);
    const QPointF through = transform.worldToScreen(throughWorld, viewportSize);
    const qreal startSquared = QPointF::dotProduct(start, start);
    const qreal endSquared = QPointF::dotProduct(end, end);
    const qreal throughSquared = QPointF::dotProduct(through, through);
    const qreal denominator = 2.0 *
        (start.x() * (end.y() - through.y()) +
         end.x() * (through.y() - start.y()) +
         through.x() * (start.y() - end.y()));
    if (std::abs(denominator) < 1.0e-9) {
        return false;
    }

    const QPointF circleCenter(
        (startSquared * (end.y() - through.y()) +
         endSquared * (through.y() - start.y()) +
         throughSquared * (start.y() - end.y())) / denominator,
        (startSquared * (through.x() - end.x()) +
         endSquared * (start.x() - through.x()) +
         throughSquared * (end.x() - start.x())) / denominator);
    const qreal circleRadius = std::hypot(start.x() - circleCenter.x(),
                                          start.y() - circleCenter.y());
    if (circleRadius <= 1.0e-9) {
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

    const qreal first = std::atan2(start.y() - circleCenter.y(),
                                   start.x() - circleCenter.x());
    const qreal second = std::atan2(end.y() - circleCenter.y(),
                                    end.x() - circleCenter.x());
    const qreal throughAngle = std::atan2(through.y() - circleCenter.y(),
                                          through.x() - circleCenter.x());
    const qreal counterClockwiseSweep = normalizeAngle(second - first);
    const qreal throughSweep = normalizeAngle(throughAngle - first);
    if (counterClockwiseSweep <= 1.0e-9) {
        return false;
    }

    const qreal selectedSweep = throughSweep <= counterClockwiseSweep + 1.0e-7
                                    ? counterClockwiseSweep
                                    : -(twoPi - counterClockwiseSweep);
    if (center != nullptr) {
        *center = circleCenter;
    }
    if (radius != nullptr) {
        *radius = circleRadius;
    }
    if (startAngle != nullptr) {
        *startAngle = first;
    }
    if (sweepAngle != nullptr) {
        *sweepAngle = selectedSweep;
    }
    return true;
}

bool CurveHitTester::arcAngleIsOnSweep(qreal startAngle,
                                       qreal sweepAngle,
                                       qreal angle) const
{
    constexpr qreal twoPi = 6.28318530717958647692;
    constexpr qreal epsilon = 1.0e-7;
    if (std::abs(sweepAngle) >= twoPi - epsilon) {
        return true;
    }

    const auto positiveAngle = [twoPi](qreal value) {
        value = std::fmod(value, twoPi);
        return value < 0.0 ? value + twoPi : value;
    };
    if (sweepAngle >= 0.0) {
        return positiveAngle(angle - startAngle) <= sweepAngle + epsilon;
    }
    return positiveAngle(startAngle - angle) <= -sweepAngle + epsilon;
}

qreal CurveHitTester::distanceToArc(const QPointF &screenPosition,
                                    const Shape &shape,
                                    const ViewportTransform &transform,
                                    const QSize &viewportSize) const
{
    if (shape.points.size() < 3) {
        return 1.0e9;
    }

    QPointF center;
    qreal radius = 0.0;
    qreal startAngle = 0.0;
    qreal sweepAngle = 0.0;
    if (shape.arcMode == ArcMode::TwoPoint) {
        if (!makeCircularArcGeometry(shape.points[0],
                                     shape.points[1],
                                     shape.points[2],
                                     transform,
                                     viewportSize,
                                     &center,
                                     &radius,
                                     &startAngle,
                                     &sweepAngle)) {
            return 1.0e9;
        }
    } else {
        center = transform.worldToScreen(shape.points[0], viewportSize);
        const QPointF start = transform.worldToScreen(shape.points[1], viewportSize);
        const QPointF end = transform.worldToScreen(shape.points[2], viewportSize);
        radius = std::hypot(start.x() - center.x(), start.y() - center.y());
        if (radius <= 1.0e-9) {
            return 1.0e9;
        }
        startAngle = std::atan2(start.y() - center.y(), start.x() - center.x());
        sweepAngle = shape.arcSweep;
        if (std::abs(sweepAngle) <= 1.0e-9) {
            const qreal endAngle = std::atan2(end.y() - center.y(), end.x() - center.x());
            sweepAngle = endAngle - startAngle;
            constexpr qreal pi = 3.14159265358979323846;
            if (sweepAngle > pi) {
                sweepAngle -= 2.0 * pi;
            } else if (sweepAngle < -pi) {
                sweepAngle += 2.0 * pi;
            }
        }
    }

    const QPointF fromCenter = screenPosition - center;
    const qreal distanceFromCenter = std::hypot(fromCenter.x(), fromCenter.y());
    if (arcAngleIsOnSweep(startAngle,
                          sweepAngle,
                          std::atan2(fromCenter.y(), fromCenter.x()))) {
        return std::abs(distanceFromCenter - radius);
    }

    const QPointF startPoint = center +
                                QPointF(radius * std::cos(startAngle),
                                        radius * std::sin(startAngle));
    const QPointF endPoint = center +
                              QPointF(radius * std::cos(startAngle + sweepAngle),
                                      radius * std::sin(startAngle + sweepAngle));
    return std::min(distanceToSegment(screenPosition, startPoint, startPoint),
                    distanceToSegment(screenPosition, endPoint, endPoint));
}

qreal CurveHitTester::distanceToCubicCurve(const QPointF &screenPosition,
                                           const Shape &shape,
                                           const ViewportTransform &transform,
                                           const QSize &viewportSize) const
{
    if (shape.points.size() < 4) {
        return 1.0e9;
    }

    const QPointF first = transform.worldToScreen(shape.points[0], viewportSize);
    const QPointF second = transform.worldToScreen(shape.points[1], viewportSize);
    const QPointF third = transform.worldToScreen(shape.points[2], viewportSize);
    const QPointF fourth = transform.worldToScreen(shape.points[3], viewportSize);
    constexpr int sampleCount = 64;
    qreal closestDistance = 1.0e9;
    QPointF previous = first;
    for (int sample = 1; sample <= sampleCount; ++sample) {
        const qreal t = static_cast<qreal>(sample) / sampleCount;
        const qreal inverse = 1.0 - t;
        const QPointF current =
            first * (inverse * inverse * inverse) +
            second * (3.0 * inverse * inverse * t) +
            third * (3.0 * inverse * t * t) +
            fourth * (t * t * t);
        closestDistance = std::min(closestDistance,
                                   distanceToSegment(screenPosition,
                                                    previous,
                                                    current));
        previous = current;
    }
    return closestDistance;
}

qreal CurveHitTester::distanceToShape(const QPointF &screenPosition,
                                      const Shape &shape,
                                      const ViewportTransform &transform,
                                      const QSize &viewportSize) const
{
    if (shape.geometryType == GeometryType::PolyCurve && !shape.components.isEmpty()) {
        qreal distance = 1.0e9;
        for (const Shape::NurbsCurve2D &component : shape.components) {
            distance = std::min(distance,
                                distanceToNurbsCurve(screenPosition,
                                                     component,
                                                     transform,
                                                     viewportSize));
        }
        return distance;
    }
    if (shape.geometryType == GeometryType::Point && !shape.points.isEmpty()) {
        const QPointF point = transform.worldToScreen(shape.points.first(), viewportSize);
        return std::hypot(screenPosition.x() - point.x(), screenPosition.y() - point.y());
    }
    if (shape.geometryType == GeometryType::Circle && shape.points.size() >= 2) {
        if (validateNurbsCurve(shape.nurbs)) {
            return distanceToNurbsCurve(screenPosition,
                                       shape.nurbs,
                                       transform,
                                       viewportSize);
        }
        const QPointF center = transform.worldToScreen(shape.points[0], viewportSize);
        const QPointF edge = transform.worldToScreen(shape.points[1], viewportSize);
        const qreal radius = std::hypot(edge.x() - center.x(), edge.y() - center.y());
        return std::abs(std::hypot(screenPosition.x() - center.x(),
                                   screenPosition.y() - center.y()) - radius);
    }
    if (shape.geometryType == GeometryType::Ellipse) {
        return validateNurbsCurve(shape.nurbs)
                   ? distanceToNurbsCurve(screenPosition,
                                         shape.nurbs,
                                         transform,
                                         viewportSize)
                   : 1.0e9;
    }
    if (shape.geometryType == GeometryType::Arc && shape.points.size() >= 3) {
        return validateNurbsCurve(shape.nurbs)
                   ? distanceToNurbsCurve(screenPosition,
                                         shape.nurbs,
                                         transform,
                                         viewportSize)
                   : distanceToArc(screenPosition, shape, transform, viewportSize);
    }
    if ((shape.geometryType == GeometryType::Bezier ||
         shape.geometryType == GeometryType::Nurbs) && shape.points.size() >= 4) {
        return validateNurbsCurve(shape.nurbs)
                   ? distanceToNurbsCurve(screenPosition,
                                         shape.nurbs,
                                         transform,
                                         viewportSize)
                   : distanceToCubicCurve(screenPosition,
                                          shape,
                                          transform,
                                          viewportSize);
    }
    if (shape.geometryType == GeometryType::Rectangle) {
        const QVector<QPointF> vertices = rectangleVertices(shape);
        if (vertices.size() < 4) {
            return 1.0e9;
        }
        qreal distance = 1.0e9;
        for (int index = 0; index < vertices.size(); ++index) {
            distance = std::min(
                distance,
                distanceToSegment(screenPosition,
                                  transform.worldToScreen(vertices[index], viewportSize),
                                  transform.worldToScreen(vertices[(index + 1) % vertices.size()],
                                                         viewportSize)));
        }
        return distance;
    }
    if (shape.geometryType == GeometryType::Line) {
        if (validateNurbsCurve(shape.nurbs)) {
            return distanceToNurbsCurve(screenPosition,
                                       shape.nurbs,
                                       transform,
                                       viewportSize);
        }
        qreal distance = 1.0e9;
        for (int index = 0; index + 1 < shape.points.size(); ++index) {
            distance = std::min(
                distance,
                distanceToSegment(screenPosition,
                                  transform.worldToScreen(shape.points[index], viewportSize),
                                  transform.worldToScreen(shape.points[index + 1], viewportSize)));
        }
        return distance;
    }
    return 1.0e9;
}

QVector<QPointF> CurveHitTester::controlPointsForShape(const Shape &shape) const
{
    if (shape.geometryType == GeometryType::Point) {
        return {};
    }
    if (shape.geometryType == GeometryType::PolyCurve) {
        QVector<QPointF> controlPoints;
        for (const Shape::NurbsCurve2D &component : shape.components) {
            controlPoints += component.controlPoints;
        }
        return controlPoints;
    }
    if (!shape.nurbs.controlPoints.isEmpty()) {
        return shape.nurbs.controlPoints;
    }
    return shape.points;
}

bool CurveHitTester::hitTestSelectedControlPoint(
    const Document &document,
    const QVector<int> &selectedShapeIndices,
    const QPointF &screenPosition,
    const ViewportTransform &transform,
    const QSize &viewportSize,
    int *shapeIndex,
    int *controlPointIndex) const
{
    constexpr qreal hitRadiusPixels = 10.0;
    int closestShapeIndex = -1;
    int closestControlPointIndex = -1;
    qreal closestDistance = hitRadiusPixels;

    for (const int candidateShapeIndex : selectedShapeIndices) {
        if (candidateShapeIndex < 0 || candidateShapeIndex >= document.size()) {
            continue;
        }
        const QVector<QPointF> controlPoints =
            controlPointsForShape(document[candidateShapeIndex]);
        for (int candidateControlPointIndex = 0;
             candidateControlPointIndex < controlPoints.size();
             ++candidateControlPointIndex) {
            const QPointF screenPoint =
                transform.worldToScreen(controlPoints[candidateControlPointIndex],
                                        viewportSize);
            const qreal distance = std::hypot(screenPosition.x() - screenPoint.x(),
                                              screenPosition.y() - screenPoint.y());
            if (distance <= closestDistance) {
                closestDistance = distance;
                closestShapeIndex = candidateShapeIndex;
                closestControlPointIndex = candidateControlPointIndex;
            }
        }
    }

    if (shapeIndex != nullptr) {
        *shapeIndex = closestShapeIndex;
    }
    if (controlPointIndex != nullptr) {
        *controlPointIndex = closestControlPointIndex;
    }
    return closestShapeIndex >= 0 && closestControlPointIndex >= 0;
}

int CurveHitTester::hitTestShape(const Document &document,
                                 const QPointF &screenPosition,
                                 const ViewportTransform &transform,
                                 const QSize &viewportSize,
                                 bool editableOnly) const
{
    constexpr qreal hitRadiusPixels = 9.0;
    int closestShape = -1;
    qreal closestDistance = hitRadiusPixels;
    for (int index = 0; index < document.size(); ++index) {
        const ObjectId objectId = document.objectIdAt(index);
        if (!document.isObjectVisible(objectId) ||
            (editableOnly && !document.isObjectEditable(objectId))) {
            continue;
        }
        const qreal distance = distanceToShape(screenPosition,
                                               document[index],
                                               transform,
                                               viewportSize);
        if (distance <= closestDistance) {
            closestDistance = distance;
            closestShape = index;
        }
    }
    return closestShape;
}

} // namespace classiCAD
