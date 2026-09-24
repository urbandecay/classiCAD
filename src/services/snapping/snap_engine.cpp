#include "snap_engine.h"

#include "core/geometry/curve_evaluator.h"

#include <algorithm>
#include <cmath>

namespace classiCAD {

void SnapEngine::setSettings(const SnapSettings &settings)
{
    settings_ = settings;
}

const SnapSettings &SnapEngine::settings() const
{
    return settings_;
}

QVector<QPointF> SnapEngine::rectangleVertices(const Shape &shape) const
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

bool SnapEngine::subdivisionCurve(const Shape &shape,
                                  Shape::NurbsCurve2D *curve) const
{
    if (curve == nullptr) {
        return false;
    }
    if (validateNurbsCurve(shape.nurbs)) {
        *curve = shape.nurbs;
        return true;
    }
    if (shape.geometryType == GeometryType::Line && shape.points.size() >= 2) {
        *curve = makeDegreeOneNurbs(shape.points);
        return validateNurbsCurve(*curve);
    }
    return false;
}

bool SnapEngine::nurbsCurveEndpoints(const Shape::NurbsCurve2D &curve,
                                     QPointF *start,
                                     QPointF *end) const
{
    if (!validateNurbsCurve(curve) || (start == nullptr && end == nullptr)) {
        return false;
    }
    qreal firstParameter = 0.0;
    qreal lastParameter = 0.0;
    if (!nurbsParameterDomain(curve, &firstParameter, &lastParameter)) {
        return false;
    }
    return (start == nullptr || evaluateNurbsPoint(curve, firstParameter, start)) &&
           (end == nullptr || evaluateNurbsPoint(curve, lastParameter, end));
}

bool SnapEngine::nurbsCurvePointAtFraction(const Shape::NurbsCurve2D &curve,
                                           qreal fraction,
                                           QPointF *point) const
{
    if (point == nullptr || !validateNurbsCurve(curve)) {
        return false;
    }
    qreal firstParameter = 0.0;
    qreal lastParameter = 0.0;
    if (!nurbsParameterDomain(curve, &firstParameter, &lastParameter)) {
        return false;
    }
    return evaluateNurbsPoint(curve,
                              firstParameter +
                                  (lastParameter - firstParameter) *
                                      std::clamp(fraction, 0.0, 1.0),
                              point);
}

bool SnapEngine::makeCircularArcGeometry(
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
        return angle < 0.0 ? angle + twoPi : angle;
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

bool SnapEngine::makeArcSnapGeometry(const Shape &shape,
                                     const ViewportTransform &transform,
                                     const QSize &viewportSize,
                                     QPointF *centerScreen,
                                     qreal *radius,
                                     qreal *startAngle,
                                     qreal *sweepAngle) const
{
    if (shape.geometryType != GeometryType::Arc || shape.points.size() < 3) {
        return false;
    }
    if (shape.arcMode == ArcMode::TwoPoint) {
        return makeCircularArcGeometry(shape.points[0],
                                       shape.points[1],
                                       shape.points[2],
                                       transform,
                                       viewportSize,
                                       centerScreen,
                                       radius,
                                       startAngle,
                                       sweepAngle);
    }

    const QPointF center = transform.worldToScreen(shape.points[0], viewportSize);
    const QPointF start = transform.worldToScreen(shape.points[1], viewportSize);
    const QPointF end = transform.worldToScreen(shape.points[2], viewportSize);
    const qreal arcRadius = std::hypot(start.x() - center.x(), start.y() - center.y());
    if (arcRadius <= 1.0e-9) {
        return false;
    }

    constexpr qreal pi = 3.14159265358979323846;
    constexpr qreal twoPi = 2.0 * pi;
    const qreal firstAngle = std::atan2(start.y() - center.y(),
                                        start.x() - center.x());
    qreal selectedSweep = shape.arcSweep;
    if (std::abs(selectedSweep) <= 1.0e-9) {
        const qreal endAngle = std::atan2(end.y() - center.y(),
                                          end.x() - center.x());
        selectedSweep = endAngle - firstAngle;
        if (selectedSweep > pi) {
            selectedSweep -= twoPi;
        } else if (selectedSweep < -pi) {
            selectedSweep += twoPi;
        }
    }

    if (centerScreen != nullptr) {
        *centerScreen = center;
    }
    if (radius != nullptr) {
        *radius = arcRadius;
    }
    if (startAngle != nullptr) {
        *startAngle = firstAngle;
    }
    if (sweepAngle != nullptr) {
        *sweepAngle = selectedSweep;
    }
    return true;
}

bool SnapEngine::arcAngleIsOnSweep(qreal startAngle,
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

bool SnapEngine::arcSnapPointAtFraction(const Shape &shape,
                                        qreal fraction,
                                        const ViewportTransform &transform,
                                        const QSize &viewportSize,
                                        QPointF *point) const
{
    QPointF center;
    qreal radius = 0.0;
    qreal startAngle = 0.0;
    qreal sweepAngle = 0.0;
    if (!makeArcSnapGeometry(shape,
                             transform,
                             viewportSize,
                             &center,
                             &radius,
                             &startAngle,
                             &sweepAngle)) {
        return false;
    }
    const qreal angle = startAngle + sweepAngle * fraction;
    if (point != nullptr) {
        *point = transform.screenToWorld(
            QPointF(center.x() + radius * std::cos(angle),
                    center.y() + radius * std::sin(angle)),
            viewportSize);
    }
    return true;
}

QVector<SnapCandidate> SnapEngine::snapCandidatesForShape(
    const Shape &shape,
    const ViewportTransform &transform,
    const QSize &viewportSize) const
{
    QVector<SnapCandidate> candidates;
    if (shape.points.isEmpty()) {
        return candidates;
    }

    Shape::NurbsCurve2D subdivisionCurveData;
    if (subdivisionCurve(shape, &subdivisionCurveData)) {
        for (const double parameter : shape.subdivisionParameters) {
            QPointF point;
            if (evaluateNurbsPoint(subdivisionCurveData, parameter, &point)) {
                candidates.append({SnapType::Endpoint, point});
            }
        }
    }

    if (shape.geometryType == GeometryType::Point) {
        candidates.append({SnapType::Endpoint, shape.points.first()});
        return candidates;
    }
    if (shape.geometryType == GeometryType::PolyCurve) {
        for (const Shape::NurbsCurve2D &component : shape.components) {
            QPointF start;
            QPointF end;
            if (nurbsCurveEndpoints(component, &start, &end)) {
                candidates.append({SnapType::Endpoint, start});
                candidates.append({SnapType::Endpoint, end});
            }
            QPointF midpoint;
            if (nurbsCurvePointAtFraction(component, 0.5, &midpoint)) {
                candidates.append({SnapType::Midpoint, midpoint});
            }
        }
        return candidates;
    }
    if (shape.geometryType == GeometryType::Circle) {
        candidates.append({SnapType::Center, shape.points.first()});
        return candidates;
    }
    if (shape.geometryType == GeometryType::Rectangle) {
        const QVector<QPointF> vertices = rectangleVertices(shape);
        for (const QPointF &vertex : vertices) {
            candidates.append({SnapType::Endpoint, vertex});
        }
        for (int index = 0; index < vertices.size(); ++index) {
            candidates.append({SnapType::Midpoint,
                               (vertices[index] + vertices[(index + 1) % vertices.size()]) /
                                   2.0});
        }
        return candidates;
    }
    if (shape.geometryType == GeometryType::Arc && shape.points.size() >= 3) {
        QPointF start = shape.arcMode == ArcMode::OnePoint ? shape.points[1]
                                                            : shape.points[0];
        QPointF end = shape.arcMode == ArcMode::OnePoint ? shape.points[2]
                                                          : shape.points[1];
        if (!nurbsCurveEndpoints(shape.nurbs, &start, &end)) {
            QPointF evaluatedEndpoint;
            if (arcSnapPointAtFraction(shape, 0.0, transform, viewportSize,
                                       &evaluatedEndpoint)) {
                start = evaluatedEndpoint;
            }
            if (arcSnapPointAtFraction(shape, 1.0, transform, viewportSize,
                                       &evaluatedEndpoint)) {
                end = evaluatedEndpoint;
            }
        }
        candidates.append({SnapType::Endpoint, start});
        candidates.append({SnapType::Endpoint, end});
        QPointF midpoint;
        if (arcSnapPointAtFraction(shape, 0.5, transform, viewportSize, &midpoint)) {
            candidates.append({SnapType::Midpoint, midpoint});
        }
        QPointF center;
        if (makeArcSnapGeometry(shape,
                                transform,
                                viewportSize,
                                &center,
                                nullptr,
                                nullptr,
                                nullptr)) {
            candidates.append({SnapType::Center,
                               transform.screenToWorld(center, viewportSize)});
        }
        return candidates;
    }
    if (shape.geometryType != GeometryType::Line) {
        return candidates;
    }

    QVector<LineSegment> segments;
    for (const QPointF &point : shape.points) {
        candidates.append({SnapType::Endpoint, point});
    }
    for (int index = 0; index + 1 < shape.points.size(); ++index) {
        const QPointF start = shape.points[index];
        const QPointF end = shape.points[index + 1];
        segments.append({start, end});
        candidates.append({SnapType::Midpoint, (start + end) / 2.0});
    }
    for (int first = 0; first < segments.size(); ++first) {
        for (int second = first + 1; second < segments.size(); ++second) {
            QPointF intersection;
            if (segmentIntersection(segments[first].start,
                                    segments[first].end,
                                    segments[second].start,
                                    segments[second].end,
                                    &intersection)) {
                candidates.append({SnapType::Intersection, intersection});
            }
        }
    }
    return candidates;
}

QVector<SnapCandidate> SnapEngine::snapCandidatesForScene(
    const Document &document,
    const QVector<int> &excludedShapeIndices,
    const ViewportTransform &transform,
    const QSize &viewportSize) const
{
    QVector<SnapCandidate> candidates;
    QVector<LineSegment> segments;
    for (int shapeIndex = 0; shapeIndex < document.size(); ++shapeIndex) {
        if (excludedShapeIndices.contains(shapeIndex)) {
            continue;
        }
        if (!document.isObjectVisible(document.objectIdAt(shapeIndex))) {
            continue;
        }
        const Shape &shape = document[shapeIndex];
        if (shape.points.isEmpty()) {
            continue;
        }

        if (settings_.endpoint) {
            Shape::NurbsCurve2D subdivisionCurveData;
            if (subdivisionCurve(shape, &subdivisionCurveData)) {
                for (const double parameter : shape.subdivisionParameters) {
                    QPointF point;
                    if (evaluateNurbsPoint(subdivisionCurveData, parameter, &point)) {
                        candidates.append({SnapType::Endpoint, point});
                    }
                }
            }
        }

        if (shape.geometryType == GeometryType::Point) {
            if (settings_.endpoint) {
                candidates.append({SnapType::Endpoint, shape.points.first()});
            }
            continue;
        }
        if (shape.geometryType == GeometryType::PolyCurve) {
            for (const Shape::NurbsCurve2D &component : shape.components) {
                QPointF start;
                QPointF end;
                if (settings_.endpoint && nurbsCurveEndpoints(component, &start, &end)) {
                    candidates.append({SnapType::Endpoint, start});
                    candidates.append({SnapType::Endpoint, end});
                }
                QPointF midpoint;
                if (settings_.midpoint &&
                    nurbsCurvePointAtFraction(component, 0.5, &midpoint)) {
                    candidates.append({SnapType::Midpoint, midpoint});
                }
            }
            continue;
        }
        if (shape.geometryType == GeometryType::Circle) {
            if (settings_.center) {
                candidates.append({SnapType::Center, shape.points.first()});
            }
            continue;
        }
        if (shape.geometryType == GeometryType::Rectangle) {
            const QVector<QPointF> vertices = rectangleVertices(shape);
            for (int index = 0; index < vertices.size(); ++index) {
                const QPointF start = vertices[index];
                const QPointF end = vertices[(index + 1) % vertices.size()];
                if (settings_.endpoint) {
                    candidates.append({SnapType::Endpoint, start});
                }
                segments.append({start, end});
                if (settings_.midpoint) {
                    candidates.append({SnapType::Midpoint, (start + end) / 2.0});
                }
            }
            continue;
        }
        if (shape.geometryType == GeometryType::Arc && shape.points.size() >= 3) {
            QPointF start = shape.arcMode == ArcMode::OnePoint ? shape.points[1]
                                                                : shape.points[0];
            QPointF end = shape.arcMode == ArcMode::OnePoint ? shape.points[2]
                                                              : shape.points[1];
            if (!nurbsCurveEndpoints(shape.nurbs, &start, &end)) {
                QPointF evaluatedEndpoint;
                if (arcSnapPointAtFraction(shape, 0.0, transform, viewportSize,
                                           &evaluatedEndpoint)) {
                    start = evaluatedEndpoint;
                }
                if (arcSnapPointAtFraction(shape, 1.0, transform, viewportSize,
                                           &evaluatedEndpoint)) {
                    end = evaluatedEndpoint;
                }
            }
            if (settings_.endpoint) {
                candidates.append({SnapType::Endpoint, start});
                candidates.append({SnapType::Endpoint, end});
            }
            if (settings_.midpoint) {
                QPointF midpoint;
                if (arcSnapPointAtFraction(shape, 0.5, transform, viewportSize,
                                           &midpoint)) {
                    candidates.append({SnapType::Midpoint, midpoint});
                }
            }
            if (settings_.center) {
                QPointF center;
                if (makeArcSnapGeometry(shape,
                                        transform,
                                        viewportSize,
                                        &center,
                                        nullptr,
                                        nullptr,
                                        nullptr)) {
                    candidates.append({SnapType::Center,
                                       transform.screenToWorld(center, viewportSize)});
                }
            }
            continue;
        }
        if (shape.geometryType != GeometryType::Line) {
            continue;
        }
        if (settings_.endpoint) {
            for (const QPointF &point : shape.points) {
                candidates.append({SnapType::Endpoint, point});
            }
        }
        for (int index = 0; index + 1 < shape.points.size(); ++index) {
            const QPointF start = shape.points[index];
            const QPointF end = shape.points[index + 1];
            segments.append({start, end});
            if (settings_.midpoint) {
                candidates.append({SnapType::Midpoint, (start + end) / 2.0});
            }
        }
    }

    if (settings_.intersection) {
        for (int first = 0; first < segments.size(); ++first) {
            for (int second = first + 1; second < segments.size(); ++second) {
                QPointF intersection;
                if (segmentIntersection(segments[first].start,
                                        segments[first].end,
                                        segments[second].start,
                                        segments[second].end,
                                        &intersection)) {
                    candidates.append({SnapType::Intersection, intersection});
                }
            }
        }
    }
    return candidates;
}

QVector<SnapCandidate> SnapEngine::perpendicularCandidates(
    const Document &document,
    const QPointF &origin,
    const QPointF &cursor,
    const ViewportTransform &transform,
    const QSize &viewportSize) const
{
    QVector<SnapCandidate> candidates;
    if (!settings_.perpendicular) {
        return candidates;
    }
    constexpr qreal epsilon = 1.0e-9;
    for (int shapeIndex = 0; shapeIndex < document.size(); ++shapeIndex) {
        const Shape &shape = document[shapeIndex];
        if (shape.points.isEmpty()) {
            continue;
        }
        if (shape.geometryType == GeometryType::Circle && shape.points.size() >= 2) {
            const QPointF center = shape.points[0];
            const QPointF edge = shape.points[1];
            const qreal radius = std::hypot(edge.x() - center.x(), edge.y() - center.y());
            if (radius <= epsilon) {
                continue;
            }
            const QPointF fromCenter = origin - center;
            const qreal distanceFromCenter = std::hypot(fromCenter.x(), fromCenter.y());
            if (distanceFromCenter <= epsilon) {
                const QPointF towardCursor = cursor - center;
                const qreal cursorDistance = std::hypot(towardCursor.x(), towardCursor.y());
                if (cursorDistance > epsilon) {
                    candidates.append({SnapType::Perpendicular,
                                       center + towardCursor * (radius / cursorDistance)});
                }
            } else {
                const QPointF radialDirection = fromCenter / distanceFromCenter;
                candidates.append({SnapType::Perpendicular,
                                   center + radialDirection * radius});
                candidates.append({SnapType::Perpendicular,
                                   center - radialDirection * radius});
            }
            continue;
        }
        if (shape.geometryType == GeometryType::Arc && shape.points.size() >= 3) {
            QPointF center;
            qreal radius = 0.0;
            qreal startAngle = 0.0;
            qreal sweepAngle = 0.0;
            if (!makeArcSnapGeometry(shape,
                                     transform,
                                     viewportSize,
                                     &center,
                                     &radius,
                                     &startAngle,
                                     &sweepAngle)) {
                continue;
            }
            const QPointF originScreen = transform.worldToScreen(origin, viewportSize);
            const QPointF fromCenter = originScreen - center;
            const qreal distanceFromCenter = std::hypot(fromCenter.x(), fromCenter.y());
            const auto appendIfOnArc = [&](const QPointF &candidateScreen) {
                const qreal candidateAngle = std::atan2(candidateScreen.y() - center.y(),
                                                         candidateScreen.x() - center.x());
                if (arcAngleIsOnSweep(startAngle, sweepAngle, candidateAngle)) {
                    candidates.append({SnapType::Perpendicular,
                                       transform.screenToWorld(candidateScreen,
                                                               viewportSize)});
                }
            };
            if (distanceFromCenter <= epsilon) {
                const QPointF cursorScreen = transform.worldToScreen(cursor, viewportSize);
                const QPointF towardCursor = cursorScreen - center;
                const qreal cursorDistance = std::hypot(towardCursor.x(), towardCursor.y());
                if (cursorDistance > epsilon) {
                    appendIfOnArc(center + towardCursor * (radius / cursorDistance));
                }
            } else {
                const QPointF radialDirection = fromCenter / distanceFromCenter;
                appendIfOnArc(center + radialDirection * radius);
                appendIfOnArc(center - radialDirection * radius);
            }
            continue;
        }
        if (shape.geometryType != GeometryType::Line) {
            continue;
        }
        for (int index = 0; index + 1 < shape.points.size(); ++index) {
            const QPointF start = shape.points[index];
            const QPointF end = shape.points[index + 1];
            const QPointF direction = end - start;
            const qreal lengthSquared = QPointF::dotProduct(direction, direction);
            if (lengthSquared <= epsilon) {
                continue;
            }
            const qreal projection = QPointF::dotProduct(origin - start, direction) /
                                     lengthSquared;
            if (projection < -epsilon || projection > 1.0 + epsilon) {
                continue;
            }
            const QPointF foot = start + direction * std::clamp(projection, 0.0, 1.0);
            if (std::hypot(origin.x() - foot.x(), origin.y() - foot.y()) <= epsilon) {
                continue;
            }
            candidates.append({SnapType::Perpendicular, foot});
        }
    }
    return candidates;
}

QVector<SnapCandidate> SnapEngine::tangentCandidates(
    const Document &document,
    const QPointF &origin,
    const ViewportTransform &transform,
    const QSize &viewportSize) const
{
    QVector<SnapCandidate> candidates;
    if (!settings_.tangent) {
        return candidates;
    }
    constexpr qreal epsilon = 1.0e-9;
    for (int shapeIndex = 0; shapeIndex < document.size(); ++shapeIndex) {
        const Shape &shape = document[shapeIndex];
        if (shape.geometryType == GeometryType::Circle && shape.points.size() >= 2) {
            const QPointF center = shape.points[0];
            const QPointF edge = shape.points[1];
            const qreal radius = std::hypot(edge.x() - center.x(), edge.y() - center.y());
            if (radius <= epsilon) {
                continue;
            }
            const QPointF fromCenter = origin - center;
            const qreal distanceFromCenter = std::hypot(fromCenter.x(), fromCenter.y());
            if (distanceFromCenter < radius - epsilon || distanceFromCenter <= epsilon) {
                continue;
            }
            const QPointF radialDirection = fromCenter / distanceFromCenter;
            const QPointF tangentDirection(-radialDirection.y(), radialDirection.x());
            const qreal radiusRatio = radius / distanceFromCenter;
            const qreal radialDistance = radius * radiusRatio;
            const qreal tangentDistance =
                radius * std::sqrt(std::max(0.0, 1.0 - radiusRatio * radiusRatio));
            candidates.append({SnapType::Tangent,
                               center + radialDirection * radialDistance +
                                   tangentDirection * tangentDistance});
            if (tangentDistance > epsilon) {
                candidates.append({SnapType::Tangent,
                                   center + radialDirection * radialDistance -
                                       tangentDirection * tangentDistance});
            }
            continue;
        }
        if (shape.geometryType != GeometryType::Arc || shape.points.size() < 3) {
            continue;
        }
        QPointF centerScreen;
        qreal radius = 0.0;
        qreal startAngle = 0.0;
        qreal sweepAngle = 0.0;
        if (!makeArcSnapGeometry(shape,
                                 transform,
                                 viewportSize,
                                 &centerScreen,
                                 &radius,
                                 &startAngle,
                                 &sweepAngle)) {
            continue;
        }
        const QPointF originScreen = transform.worldToScreen(origin, viewportSize);
        const QPointF fromCenter = originScreen - centerScreen;
        const qreal distanceFromCenter = std::hypot(fromCenter.x(), fromCenter.y());
        if (distanceFromCenter < radius - epsilon || distanceFromCenter <= epsilon) {
            continue;
        }
        const QPointF radialDirection = fromCenter / distanceFromCenter;
        const QPointF tangentDirection(-radialDirection.y(), radialDirection.x());
        const qreal radiusRatio = radius / distanceFromCenter;
        const qreal radialDistance = radius * radiusRatio;
        const qreal tangentDistance =
            radius * std::sqrt(std::max(0.0, 1.0 - radiusRatio * radiusRatio));
        const auto appendIfOnArc = [&](const QPointF &candidateScreen) {
            const qreal candidateAngle = std::atan2(candidateScreen.y() - centerScreen.y(),
                                                     candidateScreen.x() - centerScreen.x());
            if (arcAngleIsOnSweep(startAngle, sweepAngle, candidateAngle)) {
                candidates.append({SnapType::Tangent,
                                   transform.screenToWorld(candidateScreen, viewportSize)});
            }
        };
        appendIfOnArc(centerScreen + radialDirection * radialDistance +
                      tangentDirection * tangentDistance);
        if (tangentDistance > epsilon) {
            appendIfOnArc(centerScreen + radialDirection * radialDistance -
                          tangentDirection * tangentDistance);
        }
    }
    return candidates;
}

SnapResult SnapEngine::findSnapPoint(const Document &document,
                                     const QPointF &rawPoint,
                                     bool drawingSnapActive,
                                     const QVector<QPointF> &pendingPoints,
                                     const ViewportTransform &transform,
                                     const QSize &viewportSize) const
{
    SnapResult best;
    if (!settings_.enabled || !drawingSnapActive) {
        return best;
    }

    const QPointF cursorScreen = transform.worldToScreen(rawPoint, viewportSize);
    constexpr qreal snapRadiusPixels = 12.0;
    qreal bestDistance = snapRadiusPixels;
    const auto consider = [&](const SnapCandidate &candidate) {
        const QPointF candidateScreen = transform.worldToScreen(candidate.point,
                                                                viewportSize);
        const qreal distance = std::hypot(candidateScreen.x() - cursorScreen.x(),
                                           candidateScreen.y() - cursorScreen.y());
        if (distance <= bestDistance) {
            bestDistance = distance;
            best.type = candidate.type;
            best.point = candidate.point;
        }
    };

    for (const SnapCandidate &candidate :
         snapCandidatesForScene(document, {}, transform, viewportSize)) {
        consider(candidate);
    }
    if (!pendingPoints.isEmpty()) {
        for (const SnapCandidate &candidate :
             perpendicularCandidates(document,
                                     pendingPoints.back(),
                                     rawPoint,
                                     transform,
                                     viewportSize)) {
            consider(candidate);
        }
        for (const SnapCandidate &candidate :
             tangentCandidates(document,
                               pendingPoints.back(),
                               transform,
                               viewportSize)) {
            consider(candidate);
        }
    }
    return best;
}

DragSnapResult SnapEngine::findDragSnap(
    const Document &document,
    const QVector<int> &selectedShapeIndices,
    const ViewportTransform &transform,
    const QSize &viewportSize) const
{
    DragSnapResult best;
    if (!settings_.enabled || selectedShapeIndices.isEmpty()) {
        return best;
    }

    QVector<SnapCandidate> sourceCandidates;
    for (const int shapeIndex : selectedShapeIndices) {
        if (shapeIndex >= 0 && shapeIndex < document.size()) {
            sourceCandidates += snapCandidatesForShape(document[shapeIndex],
                                                       transform,
                                                       viewportSize);
        }
    }
    if (sourceCandidates.isEmpty()) {
        return best;
    }

    const QVector<SnapCandidate> targetCandidates =
        snapCandidatesForScene(document,
                               selectedShapeIndices,
                               transform,
                               viewportSize);
    constexpr qreal snapRadiusPixels = 12.0;
    qreal bestDistance = snapRadiusPixels;
    for (const SnapCandidate &source : sourceCandidates) {
        const QPointF sourceScreen = transform.worldToScreen(source.point, viewportSize);
        for (const SnapCandidate &target : targetCandidates) {
            const QPointF targetScreen = transform.worldToScreen(target.point, viewportSize);
            const qreal distance = std::hypot(targetScreen.x() - sourceScreen.x(),
                                               targetScreen.y() - sourceScreen.y());
            if (distance <= bestDistance) {
                bestDistance = distance;
                best.type = source.type;
                best.sourcePoint = source.point;
                best.targetPoint = target.point;
                best.translation = target.point - source.point;
            }
        }
    }
    return best;
}

DragSnapResult SnapEngine::findControlPointSnap(
    const Document &document,
    int selectedShapeIndex,
    const QPointF &controlPoint,
    const ViewportTransform &transform,
    const QSize &viewportSize) const
{
    DragSnapResult best;
    if (!settings_.enabled || selectedShapeIndex < 0 ||
        selectedShapeIndex >= document.size()) {
        return best;
    }

    const QPointF sourceScreen = transform.worldToScreen(controlPoint, viewportSize);
    const QVector<SnapCandidate> targetCandidates =
        snapCandidatesForScene(document,
                               {selectedShapeIndex},
                               transform,
                               viewportSize);
    constexpr qreal snapRadiusPixels = 12.0;
    qreal bestDistance = snapRadiusPixels;
    for (const SnapCandidate &target : targetCandidates) {
        const QPointF targetScreen = transform.worldToScreen(target.point, viewportSize);
        const qreal distance = std::hypot(targetScreen.x() - sourceScreen.x(),
                                           targetScreen.y() - sourceScreen.y());
        if (distance <= bestDistance) {
            bestDistance = distance;
            best.type = target.type;
            best.sourcePoint = controlPoint;
            best.targetPoint = target.point;
            best.translation = target.point - controlPoint;
        }
    }
    return best;
}

} // namespace classiCAD
