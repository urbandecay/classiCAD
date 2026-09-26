#include "snap_engine.h"

#include "core/geometry/curve_evaluator.h"

#include <algorithm>
#include <cmath>
#include <limits>

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
    if ((shape.geometryType == GeometryType::Bezier ||
         shape.geometryType == GeometryType::Nurbs) &&
        shape.points.size() >= 2) {
        *curve = makeBezierNurbs(shape.points);
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

bool SnapEngine::nearestPointOnNurbsCurve(const Shape::NurbsCurve2D &curve,
                                          const QPointF &cursorScreen,
                                          const ViewportTransform &transform,
                                          const QSize &viewportSize,
                                          QPointF *nearestPoint,
                                          qreal *distanceSquared) const
{
    if (nearestPoint == nullptr || distanceSquared == nullptr ||
        !validateNurbsCurve(curve)) {
        return false;
    }

    const QVector<double> fullKnots = expandedNurbsKnotVector(curve);
    constexpr int samplesPerSpan = 32;
    qreal bestSegmentDistanceSquared = std::numeric_limits<qreal>::infinity();
    qreal bestParameterLow = 0.0;
    qreal bestParameterHigh = 0.0;
    qreal bestParameter = 0.0;
    bool foundSegment = false;

    for (int spanIndex = curve.degree;
         spanIndex < curve.controlPoints.size();
         ++spanIndex) {
        const qreal spanStart = fullKnots[spanIndex];
        const qreal spanEnd = fullKnots[spanIndex + 1];
        if (spanEnd <= spanStart) {
            continue;
        }

        QPointF previousWorld;
        if (!evaluateNurbsPoint(curve, spanStart, &previousWorld)) {
            continue;
        }
        QPointF previousScreen = transform.worldToScreen(previousWorld, viewportSize);
        qreal previousParameter = spanStart;

        for (int sample = 1; sample <= samplesPerSpan; ++sample) {
            const qreal fraction = static_cast<qreal>(sample) / samplesPerSpan;
            const qreal parameter = spanStart + (spanEnd - spanStart) * fraction;
            QPointF currentWorld;
            if (!evaluateNurbsPoint(curve, parameter, &currentWorld)) {
                continue;
            }
            const QPointF currentScreen = transform.worldToScreen(currentWorld, viewportSize);
            const QPointF segment = currentScreen - previousScreen;
            const qreal segmentLengthSquared = QPointF::dotProduct(segment, segment);
            const qreal projection = segmentLengthSquared <= 1.0e-18
                                         ? 0.0
                                         : std::clamp(
                                               QPointF::dotProduct(cursorScreen - previousScreen,
                                                                   segment) /
                                                   segmentLengthSquared,
                                               0.0,
                                               1.0);
            const QPointF projectedScreen = previousScreen + segment * projection;
            const QPointF screenDifference = projectedScreen - cursorScreen;
            const qreal projectedDistanceSquared =
                QPointF::dotProduct(screenDifference, screenDifference);
            if (projectedDistanceSquared < bestSegmentDistanceSquared) {
                bestSegmentDistanceSquared = projectedDistanceSquared;
                bestParameterLow = previousParameter;
                bestParameterHigh = parameter;
                bestParameter = previousParameter +
                                (parameter - previousParameter) * projection;
                foundSegment = true;
            }

            previousScreen = currentScreen;
            previousParameter = parameter;
        }
    }

    if (!foundSegment) {
        return false;
    }

    const auto distanceAtParameter = [&](qreal parameter, QPointF *worldPoint) {
        QPointF evaluatedPoint;
        if (!evaluateNurbsPoint(curve, parameter, &evaluatedPoint)) {
            return std::numeric_limits<qreal>::infinity();
        }
        const QPointF screenDifference =
            transform.worldToScreen(evaluatedPoint, viewportSize) - cursorScreen;
        if (worldPoint != nullptr) {
            *worldPoint = evaluatedPoint;
        }
        return QPointF::dotProduct(screenDifference, screenDifference);
    };

    qreal bestCurveDistanceSquared = distanceAtParameter(bestParameter, nearestPoint);
    qreal low = bestParameterLow;
    qreal high = bestParameterHigh;
    constexpr qreal goldenRatioConjugate = 0.6180339887498948482;
    qreal firstParameter = high - goldenRatioConjugate * (high - low);
    qreal secondParameter = low + goldenRatioConjugate * (high - low);
    qreal firstDistanceSquared = distanceAtParameter(firstParameter, nullptr);
    qreal secondDistanceSquared = distanceAtParameter(secondParameter, nullptr);

    for (int iteration = 0; iteration < 16; ++iteration) {
        if (firstDistanceSquared <= secondDistanceSquared) {
            high = secondParameter;
            secondParameter = firstParameter;
            secondDistanceSquared = firstDistanceSquared;
            firstParameter = high - goldenRatioConjugate * (high - low);
            firstDistanceSquared = distanceAtParameter(firstParameter, nullptr);
        } else {
            low = firstParameter;
            firstParameter = secondParameter;
            firstDistanceSquared = secondDistanceSquared;
            secondParameter = low + goldenRatioConjugate * (high - low);
            secondDistanceSquared = distanceAtParameter(secondParameter, nullptr);
        }
    }

    for (const qreal candidateParameter : {low, high, firstParameter, secondParameter}) {
        QPointF candidatePoint;
        const qreal candidateDistanceSquared =
            distanceAtParameter(candidateParameter, &candidatePoint);
        if (candidateDistanceSquared < bestCurveDistanceSquared) {
            bestCurveDistanceSquared = candidateDistanceSquared;
            *nearestPoint = candidatePoint;
        }
    }

    *distanceSquared = bestCurveDistanceSquared;
    return std::isfinite(bestCurveDistanceSquared);
}

QVector<SnapCandidate> SnapEngine::tangentCandidatesForNurbsCurve(
    const Shape::NurbsCurve2D &curve,
    const QPointF &originWorld,
    const ViewportTransform &transform,
    const QSize &viewportSize) const
{
    QVector<SnapCandidate> candidates;
    if (curve.degree < 2 || !validateNurbsCurve(curve)) {
        return candidates;
    }

    const QVector<double> fullKnots = expandedNurbsKnotVector(curve);
    constexpr qreal rootTolerance = 1.0e-8;
    const int samplesPerSpan = std::clamp(curve.degree * 32, 64, 512);
    const auto tangentCondition = [&](qreal parameter,
                                      qreal spanStart,
                                      qreal spanEnd,
                                      qreal *condition) {
        if (condition == nullptr) {
            return false;
        }
        qreal evaluationParameter = parameter;
        if (evaluationParameter <= spanStart) {
            evaluationParameter = std::nextafter(spanStart, spanEnd);
        } else if (evaluationParameter >= spanEnd) {
            evaluationParameter = std::nextafter(spanEnd, spanStart);
        }

        QPointF curvePoint;
        QPointF derivative;
        if (!evaluateNurbsPoint(curve, evaluationParameter, &curvePoint) ||
            !evaluateNurbsDerivative(curve, evaluationParameter, &derivative)) {
            return false;
        }

        const QPointF toCurve = curvePoint - originWorld;
        const qreal chordLength = std::hypot(toCurve.x(), toCurve.y());
        const qreal derivativeLength = std::hypot(derivative.x(), derivative.y());
        if (chordLength <= 1.0e-12 || derivativeLength <= 1.0e-12) {
            return false;
        }

        // A normalized cross product is the sine of the angle between the
        // candidate line and the curve tangent. It is scale-independent and
        // gives the root finder a stable residual across zoom levels and
        // differently sized curves.
        *condition = crossProduct(toCurve / chordLength,
                                  derivative / derivativeLength);
        return std::isfinite(*condition);
    };

    for (int spanIndex = curve.degree;
         spanIndex < curve.controlPoints.size();
         ++spanIndex) {
        const qreal spanStart = fullKnots[spanIndex];
        const qreal spanEnd = fullKnots[spanIndex + 1];
        if (spanEnd <= spanStart) {
            continue;
        }

        const auto appendRoot = [&](qreal parameter) {
            qreal evaluationParameter = parameter;
            if (evaluationParameter <= spanStart) {
                evaluationParameter = std::nextafter(spanStart, spanEnd);
            } else if (evaluationParameter >= spanEnd) {
                evaluationParameter = std::nextafter(spanEnd, spanStart);
            }
            QPointF worldPoint;
            qreal residual = 0.0;
            if (!evaluateNurbsPoint(curve, evaluationParameter, &worldPoint) ||
                !tangentCondition(evaluationParameter,
                                  spanStart,
                                  spanEnd,
                                  &residual) ||
                std::abs(residual) > rootTolerance) {
                return;
            }
            const QPointF screenPoint = transform.worldToScreen(worldPoint, viewportSize);
            const QPointF originScreen = transform.worldToScreen(originWorld, viewportSize);
            if (std::hypot(screenPoint.x() - originScreen.x(),
                           screenPoint.y() - originScreen.y()) <= 1.0e-6) {
                return;
            }
            for (const SnapCandidate &candidate : candidates) {
                const QPointF candidateScreen = transform.worldToScreen(candidate.point,
                                                                         viewportSize);
                if (std::hypot(candidateScreen.x() - screenPoint.x(),
                               candidateScreen.y() - screenPoint.y()) <= 0.05) {
                    return;
                }
            }
            candidates.append({SnapType::Tangent, worldPoint});
        };

        QVector<qreal> parameters(samplesPerSpan + 1);
        QVector<qreal> conditions(samplesPerSpan + 1, 0.0);
        QVector<bool> valid(samplesPerSpan + 1, false);
        for (int sample = 0; sample <= samplesPerSpan; ++sample) {
            const qreal fraction = static_cast<qreal>(sample) / samplesPerSpan;
            parameters[sample] = spanStart + (spanEnd - spanStart) * fraction;
            valid[sample] = tangentCondition(parameters[sample],
                                             spanStart,
                                             spanEnd,
                                             &conditions[sample]);
            // Check both ends of every knot span explicitly. A tangent at an
            // arc seam or trimmed endpoint may not produce a sign change.
            if (valid[sample] && std::abs(conditions[sample]) <= rootTolerance) {
                appendRoot(parameters[sample]);
            }
        }

        for (int sample = 1; sample <= samplesPerSpan; ++sample) {
            if (valid[sample - 1] && valid[sample] &&
                ((conditions[sample - 1] < 0.0 && conditions[sample] > 0.0) ||
                 (conditions[sample - 1] > 0.0 && conditions[sample] < 0.0))) {
                qreal low = parameters[sample - 1];
                qreal high = parameters[sample];
                qreal lowCondition = conditions[sample - 1];
                for (int iteration = 0; iteration < 64; ++iteration) {
                    const qreal middle = (low + high) * 0.5;
                    qreal middleCondition = 0.0;
                    if (!tangentCondition(middle,
                                          spanStart,
                                          spanEnd,
                                          &middleCondition)) {
                        break;
                    }
                    if ((lowCondition < 0.0 && middleCondition < 0.0) ||
                        (lowCondition > 0.0 && middleCondition > 0.0)) {
                        low = middle;
                        lowCondition = middleCondition;
                    } else {
                        high = middle;
                    }
                    if (high - low <= 1.0e-13 *
                                           std::max(1.0, std::abs((low + high) * 0.5))) {
                        break;
                    }
                }
                appendRoot((low + high) * 0.5);
            }
        }

        // Also catch an even-multiplicity/touching root, where the tangent
        // residual reaches zero without changing sign between samples.
        for (int sample = 1; sample < samplesPerSpan; ++sample) {
            if (!valid[sample - 1] || !valid[sample] || !valid[sample + 1] ||
                conditions[sample - 1] * conditions[sample] <= 0.0 ||
                conditions[sample] * conditions[sample + 1] <= 0.0 ||
                std::abs(conditions[sample]) > std::abs(conditions[sample - 1]) ||
                std::abs(conditions[sample]) > std::abs(conditions[sample + 1])) {
                continue;
            }

            qreal low = parameters[sample - 1];
            qreal high = parameters[sample + 1];
            constexpr qreal goldenRatioConjugate = 0.6180339887498948482;
            qreal first = high - goldenRatioConjugate * (high - low);
            qreal second = low + goldenRatioConjugate * (high - low);
            qreal firstCondition = 0.0;
            qreal secondCondition = 0.0;
            if (!tangentCondition(first, spanStart, spanEnd, &firstCondition) ||
                !tangentCondition(second, spanStart, spanEnd, &secondCondition)) {
                continue;
            }
            for (int iteration = 0; iteration < 48; ++iteration) {
                if (std::abs(firstCondition) <= std::abs(secondCondition)) {
                    high = second;
                    second = first;
                    secondCondition = firstCondition;
                    first = high - goldenRatioConjugate * (high - low);
                    if (!tangentCondition(first,
                                          spanStart,
                                          spanEnd,
                                          &firstCondition)) {
                        break;
                    }
                } else {
                    low = first;
                    first = second;
                    firstCondition = secondCondition;
                    second = low + goldenRatioConjugate * (high - low);
                    if (!tangentCondition(second,
                                          spanStart,
                                          spanEnd,
                                          &secondCondition)) {
                        break;
                    }
                }
            }
            const qreal root = (low + high) * 0.5;
            qreal residual = 0.0;
            if (tangentCondition(root, spanStart, spanEnd, &residual) &&
                std::abs(residual) <= rootTolerance) {
                appendRoot(root);
            }
        }
    }
    return candidates;
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
    if (shape.points.isEmpty() && !validateNurbsCurve(shape.nurbs) &&
        shape.components.isEmpty()) {
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
    if (shape.geometryType == GeometryType::Bezier ||
        shape.geometryType == GeometryType::Nurbs) {
        Shape::NurbsCurve2D curve;
        if (subdivisionCurve(shape, &curve)) {
            QPointF start;
            QPointF end;
            if (nurbsCurveEndpoints(curve, &start, &end)) {
                candidates.append({SnapType::Endpoint, start});
                candidates.append({SnapType::Endpoint, end});
            }
            QPointF midpoint;
            if (nurbsCurvePointAtFraction(curve, 0.5, &midpoint)) {
                candidates.append({SnapType::Midpoint, midpoint});
            }
        }
        return candidates;
    }
    if (shape.geometryType == GeometryType::Circle) {
        if (!shape.points.isEmpty()) {
            candidates.append({SnapType::Center, shape.points.first()});
        }
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
    if (shape.geometryType == GeometryType::Arc) {
        QPointF start;
        QPointF end;
        if (shape.points.size() >= 3) {
            start = shape.arcMode == ArcMode::OnePoint ? shape.points[1]
                                                        : shape.points[0];
            end = shape.arcMode == ArcMode::OnePoint ? shape.points[2]
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
        } else if (!nurbsCurveEndpoints(shape.nurbs, &start, &end)) {
            return candidates;
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
    if (shape.points.isEmpty()) {
        QPointF start;
        QPointF end;
        if (nurbsCurveEndpoints(shape.nurbs, &start, &end)) {
            candidates.append({SnapType::Endpoint, start});
            candidates.append({SnapType::Endpoint, end});
        }
        return candidates;
    }

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
        if (shape.points.isEmpty() && !validateNurbsCurve(shape.nurbs) &&
            shape.components.isEmpty()) {
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
            if (settings_.endpoint && !shape.points.isEmpty()) {
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
        if (shape.geometryType == GeometryType::Bezier ||
            shape.geometryType == GeometryType::Nurbs) {
            Shape::NurbsCurve2D curve;
            if (subdivisionCurve(shape, &curve)) {
                QPointF start;
                QPointF end;
                if (settings_.endpoint && nurbsCurveEndpoints(curve, &start, &end)) {
                    candidates.append({SnapType::Endpoint, start});
                    candidates.append({SnapType::Endpoint, end});
                }
                QPointF midpoint;
                if (settings_.midpoint &&
                    nurbsCurvePointAtFraction(curve, 0.5, &midpoint)) {
                    candidates.append({SnapType::Midpoint, midpoint});
                }
            }
            continue;
        }
        if (shape.geometryType == GeometryType::Circle) {
            if (settings_.center && !shape.points.isEmpty()) {
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
        if (shape.geometryType == GeometryType::Arc) {
            QPointF start;
            QPointF end;
            if (settings_.endpoint && nurbsCurveEndpoints(shape.nurbs, &start, &end)) {
                candidates.append({SnapType::Endpoint, start});
                candidates.append({SnapType::Endpoint, end});
            }
            continue;
        }
        if (shape.geometryType != GeometryType::Line) {
            continue;
        }
        if (settings_.endpoint) {
            QPointF start;
            QPointF end;
            if (nurbsCurveEndpoints(shape.nurbs, &start, &end)) {
                candidates.append({SnapType::Endpoint, start});
                candidates.append({SnapType::Endpoint, end});
            } else {
                for (const QPointF &point : shape.points) {
                    candidates.append({SnapType::Endpoint, point});
                }
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
    const QSize &viewportSize,
    const QVector<int> &excludedShapeIndices) const
{
    QVector<SnapCandidate> candidates;
    if (!settings_.perpendicular) {
        return candidates;
    }
    constexpr qreal epsilon = 1.0e-9;
    for (int shapeIndex = 0; shapeIndex < document.size(); ++shapeIndex) {
        if (excludedShapeIndices.contains(shapeIndex) ||
            !document.isObjectVisible(document.objectIdAt(shapeIndex))) {
            continue;
        }
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
    const QSize &viewportSize,
    const QVector<int> &excludedShapeIndices) const
{
    QVector<SnapCandidate> candidates;
    if (!settings_.tangent) {
        return candidates;
    }
    for (int shapeIndex = 0; shapeIndex < document.size(); ++shapeIndex) {
        if (excludedShapeIndices.contains(shapeIndex) ||
            !document.isObjectVisible(document.objectIdAt(shapeIndex))) {
            continue;
        }
        candidates += tangentCandidatesForShape(document[shapeIndex],
                                                origin,
                                                transform,
                                                viewportSize);
    }
    return candidates;
}

QVector<SnapCandidate> SnapEngine::tangentCandidatesForShape(
    const Shape &shape,
    const QPointF &origin,
    const ViewportTransform &transform,
    const QSize &viewportSize) const
{
    QVector<SnapCandidate> candidates;
    constexpr qreal epsilon = 1.0e-9;
    const QPointF originScreen = transform.worldToScreen(origin, viewportSize);

    if (shape.geometryType == GeometryType::Circle &&
        validateNurbsCurve(shape.nurbs)) {
        return tangentCandidatesForNurbsCurve(shape.nurbs,
                                              origin,
                                              transform,
                                              viewportSize);
    }

    if (shape.geometryType == GeometryType::Circle && shape.points.size() >= 2) {
        const QPointF center = shape.points[0];
        const QPointF edge = shape.points[1];
        const qreal radius = std::hypot(edge.x() - center.x(), edge.y() - center.y());
        if (radius <= epsilon) {
            return candidates;
        }
        const QPointF fromCenter = origin - center;
        const qreal distanceFromCenter = std::hypot(fromCenter.x(), fromCenter.y());
        if (distanceFromCenter < radius - epsilon || distanceFromCenter <= epsilon) {
            return candidates;
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
        return candidates;
    }

    if (shape.geometryType == GeometryType::Arc &&
        validateNurbsCurve(shape.nurbs)) {
        return tangentCandidatesForNurbsCurve(shape.nurbs,
                                              origin,
                                              transform,
                                              viewportSize);
    }

    if (shape.geometryType == GeometryType::Arc && shape.points.size() >= 3) {
        QPointF centerScreen;
        qreal radius = 0.0;
        qreal startAngle = 0.0;
        qreal sweepAngle = 0.0;
        if (makeArcSnapGeometry(shape,
                                transform,
                                viewportSize,
                                &centerScreen,
                                &radius,
                                &startAngle,
                                &sweepAngle)) {
            const QPointF fromCenter = originScreen - centerScreen;
            const qreal distanceFromCenter = std::hypot(fromCenter.x(), fromCenter.y());
            if (distanceFromCenter >= radius - epsilon && distanceFromCenter > epsilon) {
                const QPointF radialDirection = fromCenter / distanceFromCenter;
                const QPointF tangentDirection(-radialDirection.y(), radialDirection.x());
                const qreal radiusRatio = radius / distanceFromCenter;
                const qreal radialDistance = radius * radiusRatio;
                const qreal tangentDistance =
                    radius * std::sqrt(std::max(0.0, 1.0 - radiusRatio * radiusRatio));
                const auto appendIfOnArc = [&](const QPointF &candidateScreen) {
                    const qreal candidateAngle =
                        std::atan2(candidateScreen.y() - centerScreen.y(),
                                   candidateScreen.x() - centerScreen.x());
                    if (arcAngleIsOnSweep(startAngle, sweepAngle, candidateAngle)) {
                        candidates.append({
                            SnapType::Tangent,
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
    }

    if (shape.geometryType == GeometryType::PolyCurve) {
        for (const Shape::NurbsCurve2D &component : shape.components) {
            candidates += tangentCandidatesForNurbsCurve(component,
                                                         origin,
                                                         transform,
                                                         viewportSize);
        }
        return candidates;
    }

    Shape::NurbsCurve2D curve;
    if (subdivisionCurve(shape, &curve)) {
        candidates += tangentCandidatesForNurbsCurve(curve,
                                                     origin,
                                                     transform,
                                                     viewportSize);
    }
    return candidates;
}

QVector<SnapCandidate> SnapEngine::nearCandidatesForScene(
    const Document &document,
    const QPointF &cursor,
    const ViewportTransform &transform,
    const QSize &viewportSize,
    const QVector<int> &excludedShapeIndices) const
{
    QVector<SnapCandidate> candidates;
    if (!settings_.near) {
        return candidates;
    }

    constexpr qreal snapRadiusPixels = 12.0;
    const QPointF cursorScreen = transform.worldToScreen(cursor, viewportSize);
    for (int shapeIndex = 0; shapeIndex < document.size(); ++shapeIndex) {
        if (excludedShapeIndices.contains(shapeIndex) ||
            !document.isObjectVisible(document.objectIdAt(shapeIndex))) {
            continue;
        }

        const Shape &shape = document[shapeIndex];
        qreal nearestDistanceSquared = std::numeric_limits<qreal>::infinity();
        QPointF nearestPoint;
        const auto considerPoint = [&](const QPointF &worldPoint) {
            const QPointF screenPoint = transform.worldToScreen(worldPoint, viewportSize);
            const QPointF difference = screenPoint - cursorScreen;
            const qreal distanceSquared = QPointF::dotProduct(difference, difference);
            if (distanceSquared < nearestDistanceSquared) {
                nearestDistanceSquared = distanceSquared;
                nearestPoint = worldPoint;
            }
        };
        const auto considerSegment = [&](const QPointF &start, const QPointF &end) {
            const QPointF startScreen = transform.worldToScreen(start, viewportSize);
            const QPointF endScreen = transform.worldToScreen(end, viewportSize);
            const QPointF direction = endScreen - startScreen;
            const qreal lengthSquared = QPointF::dotProduct(direction, direction);
            const qreal fraction = lengthSquared <= 1.0e-18
                                       ? 0.0
                                       : std::clamp(
                                             QPointF::dotProduct(cursorScreen - startScreen,
                                                                 direction) /
                                                 lengthSquared,
                                             0.0,
                                             1.0);
            considerPoint(start + (end - start) * fraction);
        };

        if (shape.geometryType == GeometryType::Point) {
            if (!shape.points.isEmpty()) {
                considerPoint(shape.points.first());
            }
        } else if (shape.geometryType == GeometryType::Rectangle) {
            const QVector<QPointF> vertices = rectangleVertices(shape);
            for (int vertexIndex = 0; vertexIndex < vertices.size(); ++vertexIndex) {
                considerSegment(vertices[vertexIndex],
                                vertices[(vertexIndex + 1) % vertices.size()]);
            }
        } else if (shape.geometryType == GeometryType::PolyCurve) {
            for (const Shape::NurbsCurve2D &component : shape.components) {
                QPointF componentNearestPoint;
                qreal componentDistanceSquared = 0.0;
                if (nearestPointOnNurbsCurve(component,
                                             cursorScreen,
                                             transform,
                                             viewportSize,
                                             &componentNearestPoint,
                                             &componentDistanceSquared) &&
                    componentDistanceSquared < nearestDistanceSquared) {
                    nearestDistanceSquared = componentDistanceSquared;
                    nearestPoint = componentNearestPoint;
                }
            }
        } else {
            Shape::NurbsCurve2D curve;
            if (subdivisionCurve(shape, &curve)) {
                nearestPointOnNurbsCurve(curve,
                                         cursorScreen,
                                         transform,
                                         viewportSize,
                                         &nearestPoint,
                                         &nearestDistanceSquared);
            } else if (shape.geometryType == GeometryType::Arc) {
                constexpr int arcSegments = 96;
                QPointF previousPoint;
                if (arcSnapPointAtFraction(shape,
                                           0.0,
                                           transform,
                                           viewportSize,
                                           &previousPoint)) {
                    for (int segmentIndex = 1; segmentIndex <= arcSegments;
                         ++segmentIndex) {
                        QPointF currentPoint;
                        if (!arcSnapPointAtFraction(
                                shape,
                                static_cast<qreal>(segmentIndex) / arcSegments,
                                transform,
                                viewportSize,
                                &currentPoint)) {
                            break;
                        }
                        considerSegment(previousPoint, currentPoint);
                        previousPoint = currentPoint;
                    }
                }
            } else if (shape.geometryType == GeometryType::Circle &&
                       shape.points.size() >= 2) {
                constexpr int circleSegments = 96;
                const QPointF center = shape.points[0];
                const QPointF radiusPoint = shape.points[1];
                const qreal radius = std::hypot(radiusPoint.x() - center.x(),
                                                radiusPoint.y() - center.y());
                QPointF previousPoint = center + QPointF(radius, 0.0);
                for (int segmentIndex = 1; segmentIndex <= circleSegments;
                     ++segmentIndex) {
                    const qreal angle = 6.28318530717958647692 * segmentIndex /
                                        circleSegments;
                    const QPointF currentPoint(center.x() + radius * std::cos(angle),
                                               center.y() + radius * std::sin(angle));
                    considerSegment(previousPoint, currentPoint);
                    previousPoint = currentPoint;
                }
            }
        }

        if (nearestDistanceSquared <= snapRadiusPixels * snapRadiusPixels) {
            candidates.append({SnapType::Near, nearestPoint});
        }
    }
    return candidates;
}

SnapResult SnapEngine::findSnapPoint(const Document &document,
                                     const QPointF &rawPoint,
                                     bool drawingSnapActive,
                                     const QVector<QPointF> &pendingPoints,
                                     const ViewportTransform &transform,
                                     const QSize &viewportSize,
                                     const QVector<int> &excludedShapeIndices,
                                     bool forceEnabled) const
{
    SnapResult best;
    if ((!settings_.enabled && !forceEnabled) || !drawingSnapActive) {
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
         nearCandidatesForScene(document,
                                rawPoint,
                                transform,
                                viewportSize,
                                excludedShapeIndices)) {
        consider(candidate);
    }

    for (const SnapCandidate &candidate :
         snapCandidatesForScene(document,
                               excludedShapeIndices,
                               transform,
                               viewportSize)) {
        consider(candidate);
    }
    if (!pendingPoints.isEmpty()) {
        for (const SnapCandidate &candidate :
             perpendicularCandidates(document,
                                     pendingPoints.back(),
                                     rawPoint,
                                     transform,
                                     viewportSize,
                                     excludedShapeIndices)) {
            consider(candidate);
        }
        for (const SnapCandidate &candidate :
             tangentCandidates(document,
                               pendingPoints.back(),
                               transform,
                               viewportSize,
                               excludedShapeIndices)) {
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
    int selectedControlPointIndex,
    const QPointF &controlPoint,
    const ViewportTransform &transform,
    const QSize &viewportSize) const
{
    DragSnapResult best;
    const QPointF sourceScreen = transform.worldToScreen(controlPoint, viewportSize);
    constexpr qreal snapRadiusPixels = 20.0;
    qreal bestDistance = snapRadiusPixels;

    const auto consider = [&](SnapType type, const QPointF &targetPoint) {
        const QPointF targetScreen = transform.worldToScreen(targetPoint, viewportSize);
        const qreal distance = std::hypot(targetScreen.x() - sourceScreen.x(),
                                          targetScreen.y() - sourceScreen.y());
        constexpr qreal tieTolerancePixels = 1.0e-6;
        const bool closer = distance < bestDistance - tieTolerancePixels;
        const bool preferredTie = std::abs(distance - bestDistance) <= tieTolerancePixels &&
                                  (best.type == SnapType::None ||
                                   (type == SnapType::Endpoint &&
                                    best.type != SnapType::Endpoint));
        if (distance <= snapRadiusPixels && (closer || preferredTie)) {
            bestDistance = distance;
            best.type = type;
            best.sourcePoint = controlPoint;
            best.targetPoint = targetPoint;
            best.translation = targetPoint - controlPoint;
        }
    };

    if (settings_.enabled && selectedShapeIndex >= 0 &&
        selectedShapeIndex < document.size()) {
        const QVector<SnapCandidate> targetCandidates =
            snapCandidatesForScene(document,
                                   {selectedShapeIndex},
                                   transform,
                                   viewportSize);
        for (const SnapCandidate &target : targetCandidates) {
            consider(target.type, target.point);
        }
    }

    for (int shapeIndex = 0; shapeIndex < document.size(); ++shapeIndex) {
        const ObjectId objectId = document.objectIdAt(shapeIndex);
        if (!document.isObjectVisible(objectId)) {
            continue;
        }

        const Shape &shape = document[shapeIndex];
        QVector<QPointF> controlPoints;
        if (shape.geometryType == GeometryType::PolyCurve) {
            for (const Shape::NurbsCurve2D &component : shape.components) {
                controlPoints += component.controlPoints;
            }
        } else if (!shape.nurbs.controlPoints.isEmpty()) {
            controlPoints = shape.nurbs.controlPoints;
        } else {
            controlPoints = shape.points;
        }

        for (int pointIndex = 0; pointIndex < controlPoints.size(); ++pointIndex) {
            if (shapeIndex == selectedShapeIndex &&
                pointIndex == selectedControlPointIndex) {
                continue;
            }
            consider(SnapType::ControlPoint, controlPoints[pointIndex]);
        }

        // A two-point rectangle stores only its diagonal construction points,
        // but all four corners are visible geometric snap targets.
        if (shape.geometryType == GeometryType::Rectangle) {
            for (const QPointF &corner : rectangleVertices(shape)) {
                const QPointF difference = corner - controlPoint;
                if (shapeIndex == selectedShapeIndex &&
                    QPointF::dotProduct(difference, difference) <= 1.0e-18) {
                    continue;
                }
                consider(SnapType::ControlPoint, corner);
            }
        }
    }
    return best;
}

} // namespace classiCAD
