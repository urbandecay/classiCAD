#include "curve_intersections.h"

#include "core/geometry/curve_evaluator.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace classiCAD {
namespace {

struct ParameterSpan {
    qreal start = 0.0;
    qreal end = 0.0;
    qreal minimumX = 0.0;
    qreal minimumY = 0.0;
    qreal maximumX = 0.0;
    qreal maximumY = 0.0;
};

QVector<ParameterSpan> parameterSpans(
    const NurbsCurve2D &curve,
    const WorkPlaneFrame *curveFrame = nullptr,
    const WorkPlaneFrame *referenceFrame = nullptr)
{
    QVector<ParameterSpan> spans;
    if (!validateNurbsCurve(curve)) {
        return spans;
    }

    const QVector<double> fullKnots = expandedNurbsKnotVector(curve);
    for (int knotIndex = curve.degree;
         knotIndex < curve.controlPoints.size();
         ++knotIndex) {
        const qreal spanStart = fullKnots[knotIndex];
        const qreal spanEnd = fullKnots[knotIndex + 1];
        if (spanEnd - spanStart <= 1.0e-12) {
            continue;
        }

        ParameterSpan span;
        span.start = spanStart;
        span.end = spanEnd;
        span.minimumX = std::numeric_limits<qreal>::infinity();
        span.minimumY = std::numeric_limits<qreal>::infinity();
        span.maximumX = -std::numeric_limits<qreal>::infinity();
        span.maximumY = -std::numeric_limits<qreal>::infinity();
        for (int controlPointIndex = knotIndex - curve.degree;
             controlPointIndex <= knotIndex;
             ++controlPointIndex) {
            QPointF point = curve.controlPoints[controlPointIndex];
            if (curveFrame != nullptr && referenceFrame != nullptr) {
                point = worldPointToWorkPlaneFrame(
                    workPlaneFramePointToWorld(point, *curveFrame),
                    *referenceFrame);
            }
            span.minimumX = std::min(span.minimumX, point.x());
            span.minimumY = std::min(span.minimumY, point.y());
            span.maximumX = std::max(span.maximumX, point.x());
            span.maximumY = std::max(span.maximumY, point.y());
        }
        spans.append(span);
    }
    return spans;
}

QPointF pointInReferenceFrame(const QPointF &localPoint,
                              const WorkPlaneFrame &curveFrame,
                              const WorkPlaneFrame &referenceFrame)
{
    return worldPointToWorkPlaneFrame(
        workPlaneFramePointToWorld(localPoint, curveFrame), referenceFrame);
}

QPointF derivativeInReferenceFrame(const QPointF &localDerivative,
                                   const WorkPlaneFrame &curveFrame,
                                   const WorkPlaneFrame &referenceFrame)
{
    const Point3D worldDerivative{
        curveFrame.xAxis.x * localDerivative.x() +
            curveFrame.yAxis.x * localDerivative.y(),
        curveFrame.xAxis.y * localDerivative.x() +
            curveFrame.yAxis.y * localDerivative.y(),
        curveFrame.xAxis.z * localDerivative.x() +
            curveFrame.yAxis.z * localDerivative.y()};
    return QPointF(
        worldDerivative.x * referenceFrame.xAxis.x +
            worldDerivative.y * referenceFrame.xAxis.y +
            worldDerivative.z * referenceFrame.xAxis.z,
        worldDerivative.x * referenceFrame.yAxis.x +
            worldDerivative.y * referenceFrame.yAxis.y +
            worldDerivative.z * referenceFrame.yAxis.z);
}

bool hullsOverlap(const ParameterSpan &first,
                  const ParameterSpan &second,
                  qreal tolerance)
{
    return first.maximumX + tolerance >= second.minimumX &&
           second.maximumX + tolerance >= first.minimumX &&
           first.maximumY + tolerance >= second.minimumY &&
           second.maximumY + tolerance >= first.minimumY;
}

qreal squaredDistance(const QPointF &first, const QPointF &second)
{
    const QPointF delta = first - second;
    return QPointF::dotProduct(delta, delta);
}

qreal goldenMinimumParameter(const NurbsCurve2D &curve,
                             qreal low,
                             qreal high,
                             const QPointF &point)
{
    constexpr qreal ratio = 0.6180339887498948482;
    const auto distanceAt = [&](qreal parameter) {
        QPointF curvePoint;
        if (!evaluateNurbsPoint(curve, parameter, &curvePoint)) {
            return std::numeric_limits<qreal>::infinity();
        }
        return squaredDistance(curvePoint, point);
    };

    qreal first = high - (high - low) * ratio;
    qreal second = low + (high - low) * ratio;
    qreal firstValue = distanceAt(first);
    qreal secondValue = distanceAt(second);
    for (int iteration = 0; iteration < 36; ++iteration) {
        if (firstValue <= secondValue) {
            high = second;
            second = first;
            secondValue = firstValue;
            first = high - (high - low) * ratio;
            firstValue = distanceAt(first);
        } else {
            low = first;
            first = second;
            firstValue = secondValue;
            second = low + (high - low) * ratio;
            secondValue = distanceAt(second);
        }
    }
    return (low + high) * 0.5;
}

} // namespace

NurbsCurveIntersectionResult intersectNurbsCurves(
    const NurbsCurve2D &firstCurve,
    const WorkPlaneFrame &firstFrame,
    const NurbsCurve2D &secondCurve,
    const WorkPlaneFrame &secondFrame)
{
    NurbsCurveIntersectionResult result;
    if (!validateNurbsCurve(firstCurve) ||
        !validateNurbsCurve(secondCurve) ||
        firstCurve.dimension != 2 || secondCurve.dimension != 2 ||
        !isValidWorkPlaneFrame(firstFrame) ||
        !isValidWorkPlaneFrame(secondFrame) ||
        !workPlaneFramesCoplanar(firstFrame, secondFrame)) {
        return result;
    }

    qreal domainStart = 0.0;
    qreal domainEnd = 0.0;
    if (!nurbsParameterDomain(firstCurve, &domainStart, &domainEnd)) {
        return result;
    }
    const qreal domainLength = domainEnd - domainStart;
    if (domainLength <= 0.0) {
        return result;
    }

    const QVector<ParameterSpan> firstSpans = parameterSpans(
        firstCurve, &firstFrame, &firstFrame);
    const QVector<ParameterSpan> secondSpans = parameterSpans(
        secondCurve, &secondFrame, &firstFrame);
    qreal firstCoordinateScale = 1.0;
    for (const ParameterSpan &span : firstSpans) {
        firstCoordinateScale = std::max<qreal>(
            {firstCoordinateScale, std::abs(span.minimumX),
             std::abs(span.minimumY), std::abs(span.maximumX),
             std::abs(span.maximumY)});
    }
    // At a tangency, the residual is second-order in the distance along the
    // curves. Seeds can therefore converge to points a little apart in
    // parameter while still satisfying the machine-precision residual test.
    // Compare their evaluated positions at the corresponding square-root
    // tolerance, so one tangent contact is returned once.
    const qreal contactPositionTolerance =
        std::max<qreal>(1.0e-7, firstCoordinateScale * 1.0e-4);
    const qreal parameterTolerance = std::max<qreal>(
        1.0e-12, domainLength * 1.0e-9);
    const auto appendUniqueParameter = [&](qreal parameter) {
        const qreal boundedParameter =
            std::clamp(parameter, domainStart, domainEnd);
        QPointF candidatePoint;
        if (!evaluateNurbsPoint(firstCurve, boundedParameter, &candidatePoint)) {
            return;
        }
        for (const qreal existing : result.firstCurveParameters) {
            if (std::abs(existing - boundedParameter) <= parameterTolerance) {
                return;
            }
            QPointF existingPoint;
            if (evaluateNurbsPoint(firstCurve, existing, &existingPoint) &&
                squaredDistance(existingPoint, candidatePoint) <=
                    contactPositionTolerance * contactPositionTolerance) {
                return;
            }
        }
        result.firstCurveParameters.append(boundedParameter);
    };
    constexpr qreal seedFractions[] = {0.0, 0.25, 0.5, 0.75, 1.0};

    const auto refineIntersection =
        [&](const ParameterSpan &firstSpan,
            const ParameterSpan &secondSpan,
            qreal firstSeed,
            qreal secondSeed,
            qreal geometryTolerance,
            qreal *firstParameter) {
            qreal firstFraction = firstSeed;
            qreal secondFraction = secondSeed;
            qreal damping = 1.0e-4;
            const qreal firstSpanLength = firstSpan.end - firstSpan.start;
            const qreal secondSpanLength = secondSpan.end - secondSpan.start;
            const auto evaluatePair = [&](qreal firstFractionValue,
                                          qreal secondFractionValue,
                                          QPointF *firstPoint,
                                          QPointF *secondPoint) {
                QPointF firstLocalPoint;
                QPointF secondLocalPoint;
                if (firstPoint == nullptr || secondPoint == nullptr ||
                    !evaluateNurbsPoint(
                        firstCurve,
                        firstSpan.start + firstSpanLength * firstFractionValue,
                        &firstLocalPoint) ||
                    !evaluateNurbsPoint(
                        secondCurve,
                        secondSpan.start + secondSpanLength * secondFractionValue,
                        &secondLocalPoint)) {
                    return false;
                }
                *firstPoint = pointInReferenceFrame(firstLocalPoint,
                                                    firstFrame,
                                                    firstFrame);
                *secondPoint = pointInReferenceFrame(secondLocalPoint,
                                                     secondFrame,
                                                     firstFrame);
                return std::isfinite(firstPoint->x()) &&
                       std::isfinite(firstPoint->y()) &&
                       std::isfinite(secondPoint->x()) &&
                       std::isfinite(secondPoint->y());
            };

            for (int iteration = 0; iteration < 40; ++iteration) {
                const qreal currentFirstParameter =
                    firstSpan.start + firstSpanLength * firstFraction;
                const qreal currentSecondParameter =
                    secondSpan.start + secondSpanLength * secondFraction;
                QPointF firstPoint;
                QPointF secondPoint;
                QPointF firstLocalDerivative;
                QPointF secondLocalDerivative;
                if (!evaluatePair(firstFraction,
                                  secondFraction,
                                  &firstPoint,
                                  &secondPoint)) {
                    return false;
                }
                const QPointF residual = firstPoint - secondPoint;
                const qreal currentDistanceSquared =
                    QPointF::dotProduct(residual, residual);
                if (currentDistanceSquared <=
                    geometryTolerance * geometryTolerance) {
                    *firstParameter = currentFirstParameter;
                    return true;
                }
                if (!evaluateNurbsDerivative(firstCurve,
                                             currentFirstParameter,
                                             &firstLocalDerivative) ||
                    !evaluateNurbsDerivative(secondCurve,
                                             currentSecondParameter,
                                             &secondLocalDerivative)) {
                    return false;
                }
                const QPointF firstDerivative = derivativeInReferenceFrame(
                    firstLocalDerivative, firstFrame, firstFrame);
                const QPointF secondDerivative = derivativeInReferenceFrame(
                    secondLocalDerivative, secondFrame, firstFrame);
                const QPointF firstJacobian = firstDerivative * firstSpanLength;
                const QPointF secondJacobian =
                    secondDerivative * -secondSpanLength;
                const qreal h00 = QPointF::dotProduct(firstJacobian,
                                                       firstJacobian);
                const qreal h01 = QPointF::dotProduct(firstJacobian,
                                                       secondJacobian);
                const qreal h11 = QPointF::dotProduct(secondJacobian,
                                                       secondJacobian);
                const qreal g0 = QPointF::dotProduct(firstJacobian, residual);
                const qreal g1 = QPointF::dotProduct(secondJacobian, residual);
                const qreal hessianScale =
                    std::max<qreal>({h00, h11, 1.0e-24});

                bool acceptedStep = false;
                for (int dampingAttempt = 0;
                     dampingAttempt < 8 && !acceptedStep;
                     ++dampingAttempt) {
                    const qreal diagonal00 = h00 + damping * hessianScale;
                    const qreal diagonal11 = h11 + damping * hessianScale;
                    const qreal determinant =
                        diagonal00 * diagonal11 - h01 * h01;
                    if (std::abs(determinant) <= 1.0e-30) {
                        damping *= 10.0;
                        continue;
                    }
                    const qreal firstStep =
                        (-g0 * diagonal11 + h01 * g1) / determinant;
                    const qreal secondStep =
                        (h01 * g0 - diagonal00 * g1) / determinant;
                    if (!std::isfinite(firstStep) ||
                        !std::isfinite(secondStep)) {
                        damping *= 10.0;
                        continue;
                    }

                    for (int lineSearch = 0; lineSearch < 9; ++lineSearch) {
                        const qreal fraction = std::ldexp(1.0, -lineSearch);
                        const qreal candidateFirstFraction =
                            std::clamp(firstFraction + firstStep * fraction,
                                       0.0,
                                       1.0);
                        const qreal candidateSecondFraction =
                            std::clamp(secondFraction + secondStep * fraction,
                                       0.0,
                                       1.0);
                        if (candidateFirstFraction == firstFraction &&
                            candidateSecondFraction == secondFraction) {
                            continue;
                        }
                        QPointF candidateFirstPoint;
                        QPointF candidateSecondPoint;
                        if (!evaluatePair(candidateFirstFraction,
                                          candidateSecondFraction,
                                          &candidateFirstPoint,
                                          &candidateSecondPoint)) {
                            continue;
                        }
                        const qreal candidateDistanceSquared =
                            squaredDistance(candidateFirstPoint,
                                            candidateSecondPoint);
                        if (candidateDistanceSquared < currentDistanceSquared ||
                            candidateDistanceSquared <=
                                geometryTolerance * geometryTolerance) {
                            firstFraction = candidateFirstFraction;
                            secondFraction = candidateSecondFraction;
                            damping = std::max<qreal>(1.0e-12,
                                                      damping * 0.25);
                            acceptedStep = true;
                            break;
                        }
                    }
                    if (!acceptedStep) {
                        damping *= 10.0;
                    }
                }
                if (!acceptedStep) {
                    return false;
                }
            }

            QPointF firstPoint;
            QPointF secondPoint;
            if (!evaluatePair(firstFraction,
                              secondFraction,
                              &firstPoint,
                              &secondPoint) ||
                std::hypot(firstPoint.x() - secondPoint.x(),
                           firstPoint.y() - secondPoint.y()) >
                    geometryTolerance) {
                return false;
            }
            *firstParameter = firstSpan.start + firstSpanLength * firstFraction;
            return true;
        };

    for (const ParameterSpan &firstSpan : firstSpans) {
        for (const ParameterSpan &secondSpan : secondSpans) {
            qreal coordinateScale = 1.0;
            for (const qreal value : {
                     firstSpan.minimumX,
                     firstSpan.minimumY,
                     firstSpan.maximumX,
                     firstSpan.maximumY,
                     secondSpan.minimumX,
                     secondSpan.minimumY,
                     secondSpan.maximumX,
                     secondSpan.maximumY}) {
                coordinateScale = std::max(coordinateScale, std::abs(value));
            }
            const qreal geometryTolerance =
                std::max<qreal>(1.0e-7, coordinateScale * 1.0e-10);
            if (!hullsOverlap(firstSpan,
                              secondSpan,
                              geometryTolerance)) {
                continue;
            }

            for (const qreal firstSeed : seedFractions) {
                for (const qreal secondSeed : seedFractions) {
                    ++result.seedSolves;
                    qreal parameter = 0.0;
                    if (refineIntersection(firstSpan,
                                           secondSpan,
                                           firstSeed,
                                           secondSeed,
                                           geometryTolerance,
                                           &parameter)) {
                        appendUniqueParameter(parameter);
                    }
                }
            }
        }
    }

    return result;
}

bool closestNurbsParameterToPoint(const NurbsCurve2D &curve,
                                  const QPointF &point,
                                  NurbsPointClosestParameter *closest)
{
    if (closest == nullptr || !validateNurbsCurve(curve)) {
        return false;
    }
    const QVector<ParameterSpan> spans = parameterSpans(curve);
    if (spans.isEmpty()) {
        return false;
    }

    const QVector<double> fullKnots = expandedNurbsKnotVector(curve);
    qreal domainStart = fullKnots[curve.degree];
    qreal closestDistanceSquared = std::numeric_limits<qreal>::infinity();
    qreal closestParameter = domainStart;
    constexpr int subdivisions = 8;
    for (const ParameterSpan &span : spans) {
        qreal sampleParameters[subdivisions + 1];
        qreal sampleDistances[subdivisions + 1];
        for (int sample = 0; sample <= subdivisions; ++sample) {
            const qreal fraction = static_cast<qreal>(sample) / subdivisions;
            const qreal parameter =
                span.start + (span.end - span.start) * fraction;
            QPointF curvePoint;
            sampleParameters[sample] = parameter;
            sampleDistances[sample] =
                evaluateNurbsPoint(curve, parameter, &curvePoint)
                    ? squaredDistance(curvePoint, point)
                    : std::numeric_limits<qreal>::infinity();
            if (sampleDistances[sample] < closestDistanceSquared) {
                closestDistanceSquared = sampleDistances[sample];
                closestParameter = parameter;
            }
        }
        for (int sample = 0; sample <= subdivisions; ++sample) {
            const qreal previousDistance =
                sample > 0 ? sampleDistances[sample - 1]
                           : std::numeric_limits<qreal>::infinity();
            const qreal nextDistance =
                sample < subdivisions ? sampleDistances[sample + 1]
                                      : std::numeric_limits<qreal>::infinity();
            if (sampleDistances[sample] > previousDistance ||
                sampleDistances[sample] > nextDistance) {
                continue;
            }
            const qreal low = sampleParameters[std::max(0, sample - 1)];
            const qreal high = sampleParameters[std::min(subdivisions,
                                                         sample + 1)];
            if (high - low <= 1.0e-14) {
                continue;
            }
            const qreal candidateParameter =
                goldenMinimumParameter(curve, low, high, point);
            QPointF candidatePoint;
            if (!evaluateNurbsPoint(curve, candidateParameter, &candidatePoint)) {
                continue;
            }
            const qreal candidateDistanceSquared =
                squaredDistance(candidatePoint, point);
            if (candidateDistanceSquared < closestDistanceSquared) {
                closestDistanceSquared = candidateDistanceSquared;
                closestParameter = candidateParameter;
            }
        }
    }
    if (!std::isfinite(closestDistanceSquared)) {
        return false;
    }
    closest->parameter = closestParameter;
    closest->distanceSquared = closestDistanceSquared;
    return true;
}

} // namespace classiCAD
