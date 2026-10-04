#include "curve_erase_query.h"

#include "core/document/document.h"
#include "core/geometry/curve_evaluator.h"
#include "core/geometry/curve_intersections.h"
#include "core/geometry/shape_mapping.h"
#include "services/viewport/viewport_transform.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace classiCAD {
namespace {

qreal distanceToScreenSegment(const QPointF &point,
                              const QPointF &start,
                              const QPointF &end)
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

qreal squaredScreenDistanceAt(const NurbsCurve2D &curve,
                              const WorkPlaneFrame &workPlaneFrame,
                              qreal parameter,
                              const QPointF &screenPoint,
                              const ViewportTransform &viewportTransform,
                              const QSize &viewportSize)
{
    QPointF localPoint;
    if (!evaluateNurbsPoint(curve, parameter, &localPoint)) {
        return std::numeric_limits<qreal>::infinity();
    }
    const QPointF curveScreen = viewportTransform.workPlaneToScreen(
        localPoint, viewportSize, workPlaneFrame);
    const QPointF delta = curveScreen - screenPoint;
    return QPointF::dotProduct(delta, delta);
}

qreal goldenMinimumParameter(const NurbsCurve2D &curve,
                              const WorkPlaneFrame &workPlaneFrame,
                              qreal low,
                              qreal high,
                              const QPointF &screenPoint,
                              const ViewportTransform &viewportTransform,
                              const QSize &viewportSize,
                              int iterations)
{
    constexpr qreal ratio = 0.6180339887498948482;
    qreal first = high - (high - low) * ratio;
    qreal second = low + (high - low) * ratio;
    qreal firstValue = squaredScreenDistanceAt(curve,
                                               workPlaneFrame,
                                               first,
                                               screenPoint,
                                               viewportTransform,
                                               viewportSize);
    qreal secondValue = squaredScreenDistanceAt(curve,
                                                workPlaneFrame,
                                                second,
                                                screenPoint,
                                                viewportTransform,
                                                viewportSize);
    for (int iteration = 0; iteration < iterations; ++iteration) {
        if (firstValue <= secondValue) {
            high = second;
            second = first;
            secondValue = firstValue;
            first = high - (high - low) * ratio;
            firstValue = squaredScreenDistanceAt(curve,
                                                 workPlaneFrame,
                                                 first,
                                                 screenPoint,
                                                 viewportTransform,
                                                 viewportSize);
        } else {
            low = first;
            first = second;
            firstValue = secondValue;
            second = low + (high - low) * ratio;
            secondValue = squaredScreenDistanceAt(curve,
                                                  workPlaneFrame,
                                                  second,
                                                  screenPoint,
                                                  viewportTransform,
                                                  viewportSize);
        }
    }
    return (low + high) * 0.5;
}

QVector<ParameterInterval> mergeIntervals(
    QVector<ParameterInterval> intervals,
    qreal domainLength,
    bool discardTinyIntervals)
{
    const qreal tolerance =
        std::max<qreal>(1.0e-9, domainLength * 1.0e-8);
    std::sort(intervals.begin(),
              intervals.end(),
              [](const ParameterInterval &first,
                 const ParameterInterval &second) {
                  return first.start < second.start;
              });
    QVector<ParameterInterval> merged;
    for (const ParameterInterval &interval : intervals) {
        if (discardTinyIntervals && interval.end - interval.start <= tolerance) {
            continue;
        }
        if (!merged.isEmpty() &&
            interval.start <= merged.last().end + tolerance) {
            merged.last().end = std::max(merged.last().end, interval.end);
        } else {
            merged.append(interval);
        }
    }
    return merged;
}

} // namespace

EraseIntersectionCandidates makeEraseIntersectionCandidates(
    const Document &document,
    const QVector<EraseCurveSampleCache> *sceneCache)
{
    EraseIntersectionCandidates candidates;
    if (sceneCache != nullptr) {
        candidates.curves.reserve(sceneCache->size());
        for (const EraseCurveSampleCache &cachedCurve : *sceneCache) {
            const int shapeIndex = cachedCurve.shapeIndex;
            if (shapeIndex < 0 || shapeIndex >= document.size()) {
                continue;
            }

            EraseCurveIntersectionCandidate candidate;
            candidate.objectId = document.objectIdAt(shapeIndex);
            candidate.componentIndex = cachedCurve.componentIndex;
            candidate.workPlaneFrame =
                isValidWorkPlaneFrame(cachedCurve.workPlaneFrame)
                    ? cachedCurve.workPlaneFrame
                    : shapeComponentWorkPlaneFrame(document[shapeIndex],
                                                   cachedCurve.componentIndex);
            candidate.curve = cachedCurve.curve;
            candidates.curves.append(candidate);
        }
    } else {
        for (int shapeIndex = 0; shapeIndex < document.size(); ++shapeIndex) {
            const ObjectId objectId = document.objectIdAt(shapeIndex);
            if (!document.isObjectVisible(objectId)) {
                continue;
            }
            const QVector<ShapeNurbsCurveComponent> components =
                nurbsCurveComponentsForShape(document[shapeIndex]);
            for (const ShapeNurbsCurveComponent &component : components) {
                candidates.curves.append({objectId,
                                          component.componentIndex,
                                          component.curve,
                                          component.workPlaneFrame});
            }
        }
    }

    for (int shapeIndex = 0; shapeIndex < document.size(); ++shapeIndex) {
        const ObjectId objectId = document.objectIdAt(shapeIndex);
        if (!document.isObjectVisible(objectId)) {
            continue;
        }
        const Shape &shape = document[shapeIndex];
        if (shape.geometryType != GeometryType::Point || shape.points.isEmpty()) {
            continue;
        }
        candidates.points.append({objectId,
                                  shapePointToWorld(shape,
                                                    shape.points.first())});
    }
    return candidates;
}

EraseIntersectionParameterResult findEraseIntersectionParameters(
    const NurbsCurve2D &sourceCurve,
    const WorkPlaneFrame &sourceWorkPlaneFrame,
    ObjectId sourceObjectId,
    int sourceComponentIndex,
    const QVector<EraseCurveIntersectionCandidate> &otherCurves,
    const QVector<ErasePointIntersectionCandidate> &otherPoints,
    qreal endpointProximityTolerance)
{
    EraseIntersectionParameterResult result;
    if (!validateNurbsCurve(sourceCurve) ||
        !isValidWorkPlaneFrame(sourceWorkPlaneFrame)) {
        return result;
    }

    qreal domainStart = 0.0;
    qreal domainEnd = 0.0;
    if (!nurbsParameterDomain(sourceCurve, &domainStart, &domainEnd)) {
        return result;
    }
    const qreal domainLength = domainEnd - domainStart;
    if (domainLength <= 0.0) {
        return result;
    }

    const auto appendUniqueParameter = [&](qreal parameter) {
        const qreal tolerance =
            std::max<qreal>(1.0e-12, domainLength * 1.0e-9);
        for (const qreal existing : result.parameters) {
            if (std::abs(existing - parameter) <= tolerance) {
                return;
            }
        }
        result.parameters.append(std::clamp(parameter, domainStart, domainEnd));
    };
    const auto appendUniqueObjectId = [&](ObjectId objectId) {
        if (objectId.isValid() &&
            !result.intersectingObjectIds.contains(objectId)) {
            result.intersectingObjectIds.append(objectId);
        }
    };

    for (const EraseCurveIntersectionCandidate &candidate : otherCurves) {
        if (candidate.objectId == sourceObjectId &&
            candidate.componentIndex == sourceComponentIndex) {
            continue;
        }
        const NurbsCurveIntersectionResult intersection =
            intersectNurbsCurves(sourceCurve,
                                 sourceWorkPlaneFrame,
                                 candidate.curve,
                                 candidate.workPlaneFrame);
        result.nurbsSeedSolves += intersection.seedSolves;
        for (const qreal parameter : intersection.firstCurveParameters) {
            appendUniqueParameter(parameter);
        }
        if (intersection.firstCurveParameters.isEmpty() &&
            std::isfinite(endpointProximityTolerance) &&
            endpointProximityTolerance > 0.0 &&
            validateNurbsCurve(candidate.curve) &&
            isValidWorkPlaneFrame(candidate.workPlaneFrame)) {
            qreal candidateDomainStart = 0.0;
            qreal candidateDomainEnd = 0.0;
            if (nurbsParameterDomain(candidate.curve,
                                     &candidateDomainStart,
                                     &candidateDomainEnd)) {
                for (const qreal endpointParameter :
                     {candidateDomainStart, candidateDomainEnd}) {
                    QPointF candidateLocalPoint;
                    if (!evaluateNurbsPoint(candidate.curve,
                                            endpointParameter,
                                            &candidateLocalPoint)) {
                        continue;
                    }
                    const Point3D candidateWorldPoint =
                        workPlaneFramePointToWorld(candidateLocalPoint,
                                                   candidate.workPlaneFrame);
                    const qreal planeDistance = std::abs(
                        signedDistanceFromWorkPlaneFrame(
                            candidateWorldPoint, sourceWorkPlaneFrame));
                    if (!std::isfinite(planeDistance) ||
                        planeDistance > endpointProximityTolerance) {
                        continue;
                    }
                    const QPointF sourceLocalPoint = worldPointToWorkPlaneFrame(
                        candidateWorldPoint, sourceWorkPlaneFrame);
                    NurbsPointClosestParameter closest;
                    if (closestNurbsParameterToPoint(sourceCurve,
                                                     sourceLocalPoint,
                                                     &closest) &&
                        closest.distanceSquared <=
                            endpointProximityTolerance *
                                endpointProximityTolerance) {
                        appendUniqueParameter(closest.parameter);
                        appendUniqueObjectId(candidate.objectId);
                    }
                }
            }
        }
        if (!intersection.firstCurveParameters.isEmpty() &&
            candidate.objectId != sourceObjectId) {
            appendUniqueObjectId(candidate.objectId);
        }
    }

    for (const ErasePointIntersectionCandidate &candidate : otherPoints) {
        if (candidate.objectId == sourceObjectId) {
            continue;
        }
        ++result.pointChecks;
        const Point3D &pointWorld = candidate.worldPosition;
        const qreal worldCoordinateScale = std::max<qreal>(
            {1.0, std::abs(pointWorld.x), std::abs(pointWorld.y),
             std::abs(pointWorld.z),
             std::abs(sourceWorkPlaneFrame.origin.x),
             std::abs(sourceWorkPlaneFrame.origin.y),
             std::abs(sourceWorkPlaneFrame.origin.z)});
        const qreal planeTolerance =
            std::max<qreal>(1.0e-7, worldCoordinateScale * 1.0e-12);
        const qreal planeDistance =
            signedDistanceFromWorkPlaneFrame(pointWorld,
                                             sourceWorkPlaneFrame);
        if (!std::isfinite(planeDistance) ||
            std::abs(planeDistance) > planeTolerance) {
            continue;
        }

        const QPointF pointLocal = worldPointToWorkPlaneFrame(
            pointWorld, sourceWorkPlaneFrame);
        qreal localCoordinateScale = std::max<qreal>(
            {1.0, std::abs(pointLocal.x()), std::abs(pointLocal.y())});
        for (const QPointF &controlPoint : sourceCurve.controlPoints) {
            localCoordinateScale = std::max<qreal>(
                {localCoordinateScale, std::abs(controlPoint.x()),
                 std::abs(controlPoint.y())});
        }
        const qreal geometryTolerance =
            std::max<qreal>(1.0e-7, localCoordinateScale * 1.0e-9);

        NurbsPointClosestParameter closest;
        if (closestNurbsParameterToPoint(sourceCurve,
                                         pointLocal,
                                         &closest) &&
            closest.distanceSquared <=
                geometryTolerance * geometryTolerance) {
            appendUniqueParameter(closest.parameter);
            appendUniqueObjectId(candidate.objectId);
        }
    }
    return result;
}

qreal distanceToNurbsCurveOnScreen(
    const NurbsCurve2D &curve,
    const WorkPlaneFrame &workPlaneFrame,
    const QPointF &screenPosition,
    const ViewportTransform &viewportTransform,
    const QSize &viewportSize)
{
    if (!validateNurbsCurve(curve) ||
        !isValidWorkPlaneFrame(workPlaneFrame)) {
        return std::numeric_limits<qreal>::infinity();
    }
    const QVector<double> fullKnots = expandedNurbsKnotVector(curve);
    qreal closestDistanceSquared =
        std::numeric_limits<qreal>::infinity();
    for (int knotIndex = curve.degree;
         knotIndex < curve.controlPoints.size();
         ++knotIndex) {
        const qreal spanStart = fullKnots[knotIndex];
        const qreal spanEnd = fullKnots[knotIndex + 1];
        if (spanEnd - spanStart <= 1.0e-12) {
            continue;
        }
        const int subdivisions = std::max(24, curve.degree * 16);
        QVector<qreal> sampleParameters;
        QVector<qreal> sampleDistances;
        sampleParameters.reserve(subdivisions + 1);
        sampleDistances.reserve(subdivisions + 1);
        for (int sample = 0; sample <= subdivisions; ++sample) {
            const qreal fraction = static_cast<qreal>(sample) / subdivisions;
            const qreal parameter =
                spanStart + (spanEnd - spanStart) * fraction;
            const qreal distanceSquared = squaredScreenDistanceAt(
                curve, workPlaneFrame, parameter, screenPosition,
                viewportTransform, viewportSize);
            sampleParameters.append(parameter);
            sampleDistances.append(distanceSquared);
            closestDistanceSquared =
                std::min(closestDistanceSquared, distanceSquared);
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
            const qreal candidate = goldenMinimumParameter(
                curve, workPlaneFrame, low, high, screenPosition,
                viewportTransform, viewportSize, 32);
            closestDistanceSquared = std::min(
                closestDistanceSquared,
                squaredScreenDistanceAt(curve, workPlaneFrame, candidate,
                                        screenPosition, viewportTransform,
                                        viewportSize));
        }
    }
    return std::sqrt(closestDistanceSquared);
}

QVector<ParameterInterval> nurbsEraseIntervalsForStrokeSegment(
    const NurbsCurve2D &curve,
    const WorkPlaneFrame &workPlaneFrame,
    const QPointF &strokeStart,
    const QPointF &strokeEnd,
    const ViewportTransform &viewportTransform,
    const QSize &viewportSize)
{
    if (!validateNurbsCurve(curve) ||
        !isValidWorkPlaneFrame(workPlaneFrame)) {
        return {};
    }
    const QVector<double> fullKnots = expandedNurbsKnotVector(curve);
    const qreal domainStart = fullKnots[curve.degree];
    const qreal domainEnd = fullKnots[curve.controlPoints.size()];
    const qreal domainLength = domainEnd - domainStart;
    if (domainLength <= 1.0e-12) {
        return {};
    }

    constexpr qreal eraserRadiusPixels = 10.0;
    constexpr qreal eraserRadiusSquared =
        eraserRadiusPixels * eraserRadiusPixels;
    const auto squaredDistanceToStroke = [&](qreal parameter) {
        QPointF localPoint;
        if (!evaluateNurbsPoint(curve, parameter, &localPoint)) {
            return std::numeric_limits<qreal>::infinity();
        }
        const QPointF curveScreen = viewportTransform.workPlaneToScreen(
            localPoint, viewportSize, workPlaneFrame);
        const qreal distance = distanceToScreenSegment(curveScreen,
                                                       strokeStart,
                                                       strokeEnd);
        return distance * distance;
    };
    const auto goldenMinimum = [&](qreal low, qreal high) {
        constexpr qreal ratio = 0.6180339887498948482;
        qreal first = high - (high - low) * ratio;
        qreal second = low + (high - low) * ratio;
        qreal firstValue = squaredDistanceToStroke(first);
        qreal secondValue = squaredDistanceToStroke(second);
        for (int iteration = 0; iteration < 32; ++iteration) {
            if (firstValue <= secondValue) {
                high = second;
                second = first;
                secondValue = firstValue;
                first = high - (high - low) * ratio;
                firstValue = squaredDistanceToStroke(first);
            } else {
                low = first;
                first = second;
                firstValue = secondValue;
                second = low + (high - low) * ratio;
                secondValue = squaredDistanceToStroke(second);
            }
        }
        return (low + high) * 0.5;
    };
    const auto refineBoundary = [&](qreal outsideParameter,
                                    qreal insideParameter,
                                    bool outsideIsFirst) {
        qreal low = outsideIsFirst ? outsideParameter : insideParameter;
        qreal high = outsideIsFirst ? insideParameter : outsideParameter;
        for (int iteration = 0; iteration < 36; ++iteration) {
            const qreal middle = (low + high) * 0.5;
            const bool middleInside =
                squaredDistanceToStroke(middle) <= eraserRadiusSquared;
            if (middleInside) {
                if (outsideIsFirst) {
                    high = middle;
                } else {
                    low = middle;
                }
            } else if (outsideIsFirst) {
                low = middle;
            } else {
                high = middle;
            }
        }
        return (low + high) * 0.5;
    };

    QVector<ParameterInterval> intervals;
    for (int knotIndex = curve.degree;
         knotIndex < curve.controlPoints.size();
         ++knotIndex) {
        const qreal spanStart = fullKnots[knotIndex];
        const qreal spanEnd = fullKnots[knotIndex + 1];
        if (spanEnd - spanStart <= 1.0e-12) {
            continue;
        }
        const int subdivisions = std::max(24, curve.degree * 16);
        QVector<qreal> sampleParameters;
        QVector<qreal> sampleDistances;
        sampleParameters.reserve(subdivisions + 1);
        sampleDistances.reserve(subdivisions + 1);
        for (int sample = 0; sample <= subdivisions; ++sample) {
            const qreal fraction = static_cast<qreal>(sample) / subdivisions;
            const qreal parameter =
                spanStart + (spanEnd - spanStart) * fraction;
            sampleParameters.append(parameter);
            sampleDistances.append(squaredDistanceToStroke(parameter));
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
            qreal minimumParameter = sampleParameters[sample];
            if (high - low > 1.0e-14) {
                minimumParameter = goldenMinimum(low, high);
            }
            if (squaredDistanceToStroke(minimumParameter) >
                eraserRadiusSquared) {
                continue;
            }

            qreal leftBoundary = spanStart;
            int previous = sample;
            while (previous >= 0 &&
                   sampleParameters[previous] >= minimumParameter) {
                --previous;
            }
            while (previous >= 0 &&
                   sampleDistances[previous] <= eraserRadiusSquared) {
                --previous;
            }
            if (previous >= 0) {
                leftBoundary = refineBoundary(sampleParameters[previous],
                                              minimumParameter,
                                              true);
            }
            qreal rightBoundary = spanEnd;
            int next = sample;
            while (next <= subdivisions &&
                   sampleParameters[next] <= minimumParameter) {
                ++next;
            }
            while (next <= subdivisions &&
                   sampleDistances[next] <= eraserRadiusSquared) {
                ++next;
            }
            if (next <= subdivisions) {
                rightBoundary = refineBoundary(sampleParameters[next],
                                               minimumParameter,
                                               false);
            }
            if (rightBoundary - leftBoundary > 0.0) {
                intervals.append({leftBoundary, rightBoundary});
            }
        }
    }
    return mergeIntervals(std::move(intervals), domainLength, true);
}

QVector<ParameterInterval> nurbsEraseIntervalsForStroke(
    const NurbsCurve2D &curve,
    const WorkPlaneFrame &workPlaneFrame,
    const QVector<QPointF> &stroke,
    const ViewportTransform &viewportTransform,
    const QSize &viewportSize)
{
    if (!validateNurbsCurve(curve) || stroke.isEmpty()) {
        return {};
    }
    QVector<ParameterInterval> intervals;
    if (stroke.size() == 1) {
        intervals = nurbsEraseIntervalsForStrokeSegment(
            curve, workPlaneFrame, stroke.first(), stroke.first(),
            viewportTransform, viewportSize);
    } else {
        for (int index = 1; index < stroke.size(); ++index) {
            intervals += nurbsEraseIntervalsForStrokeSegment(
                curve, workPlaneFrame, stroke[index - 1], stroke[index],
                viewportTransform, viewportSize);
        }
    }
    const QVector<double> fullKnots = expandedNurbsKnotVector(curve);
    const qreal domainLength = fullKnots[curve.controlPoints.size()] -
                               fullKnots[curve.degree];
    return mergeIntervals(std::move(intervals), domainLength, true);
}

QVector<ParameterInterval> nurbsCurveIntervalsInsideScreenBox(
    const NurbsCurve2D &curve,
    const SampledNurbsCurve2D &sampled,
    const WorkPlaneFrame &workPlaneFrame,
    const QRectF &box,
    const ViewportTransform &viewportTransform,
    const QSize &viewportSize)
{
    QVector<ParameterInterval> intervals;
    if (!validateNurbsCurve(curve) || sampled.parameters.size() < 2 ||
        sampled.parameters.size() != sampled.screenPoints.size()) {
        return intervals;
    }
    const QRectF region = box.normalized();
    const auto isInside = [&](qreal parameter) {
        QPointF localPoint;
        return evaluateNurbsPoint(curve, parameter, &localPoint) &&
               region.contains(viewportTransform.workPlaneToScreen(
                   localPoint, viewportSize, workPlaneFrame));
    };
    const auto refineBoundary = [&](qreal first,
                                    qreal second,
                                    bool firstInside) {
        qreal low = first;
        qreal high = second;
        for (int iteration = 0; iteration < 32; ++iteration) {
            const qreal middle = (low + high) * 0.5;
            if (isInside(middle) == firstInside) {
                low = middle;
            } else {
                high = middle;
            }
        }
        return (low + high) * 0.5;
    };

    qreal intervalStart = sampled.parameters.first();
    bool previousInside = region.contains(sampled.screenPoints.first());
    bool insideInterval = previousInside;
    for (int sample = 1; sample < sampled.parameters.size(); ++sample) {
        const bool currentInside = region.contains(sampled.screenPoints[sample]);
        if (currentInside != previousInside) {
            const qreal boundary = refineBoundary(sampled.parameters[sample - 1],
                                                  sampled.parameters[sample],
                                                  previousInside);
            if (currentInside) {
                intervalStart = boundary;
                insideInterval = true;
            } else if (insideInterval) {
                intervals.append({intervalStart, boundary});
                insideInterval = false;
            }
        }
        previousInside = currentInside;
    }
    if (insideInterval) {
        intervals.append({intervalStart, sampled.parameters.last()});
    }
    const qreal domainLength = sampled.parameters.last() -
                               sampled.parameters.first();
    const qreal tolerance = std::max<qreal>(1.0e-9,
                                            domainLength * 1.0e-8);
    intervals.erase(std::remove_if(
                        intervals.begin(), intervals.end(),
                        [tolerance](const ParameterInterval &interval) {
                            return interval.end - interval.start <= tolerance;
                        }),
                    intervals.end());
    return intervals;
}

} // namespace classiCAD
