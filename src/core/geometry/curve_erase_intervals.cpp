#include "curve_erase_intervals.h"

#include "core/geometry/curve_join.h"
#include "core/geometry/curve_evaluator.h"
#include "core/geometry/curve_editing.h"
#include "core/geometry/shape_mapping.h"

#include <algorithm>
#include <cmath>

namespace classiCAD {

QVector<ParameterInterval> boundCurveEraseIntervals(
    const NurbsCurve2D &curve,
    const QVector<ParameterInterval> &hitIntervals,
    const QVector<qreal> &intersectionParameters)
{
    if (hitIntervals.isEmpty() || !validateNurbsCurve(curve)) {
        return {};
    }

    const QVector<double> fullKnots = expandedNurbsKnotVector(curve);
    const qreal domainStart = fullKnots[curve.degree];
    const qreal domainEnd = fullKnots[curve.controlPoints.size()];
    const qreal domainLength = domainEnd - domainStart;
    const qreal tolerance = std::max<qreal>(1.0e-9, domainLength * 1.0e-8);

    QVector<qreal> boundaries{domainStart, domainEnd};
    boundaries += intersectionParameters;
    std::sort(boundaries.begin(), boundaries.end());

    QVector<qreal> uniqueBoundaries;
    for (const qreal boundary : boundaries) {
        const qreal clampedBoundary = std::clamp(boundary,
                                                 domainStart,
                                                 domainEnd);
        if (uniqueBoundaries.isEmpty() ||
            clampedBoundary > uniqueBoundaries.back() + tolerance) {
            uniqueBoundaries.append(clampedBoundary);
        } else {
            uniqueBoundaries.back() =
                std::max(uniqueBoundaries.back(), clampedBoundary);
        }
    }

    QVector<ParameterInterval> removedIntervals;
    for (const ParameterInterval &hit : hitIntervals) {
        const qreal hitMidpoint = (hit.start + hit.end) * 0.5;
        for (int boundaryIndex = 0;
             boundaryIndex + 1 < uniqueBoundaries.size();
             ++boundaryIndex) {
            const qreal pieceStart = uniqueBoundaries[boundaryIndex];
            const qreal pieceEnd = uniqueBoundaries[boundaryIndex + 1];
            if (pieceEnd - pieceStart <= tolerance ||
                hit.end <= pieceStart + tolerance ||
                hit.start >= pieceEnd - tolerance) {
                continue;
            }

            // A brush can overlap adjacent pieces around a real intersection.
            // Select the piece containing the hit center, plus pieces whose
            // centers are fully covered by a long stroke.
            const qreal pieceMidpoint = (pieceStart + pieceEnd) * 0.5;
            const bool hitCenterIsInPiece =
                hitMidpoint > pieceStart + tolerance &&
                hitMidpoint < pieceEnd - tolerance;
            const bool pieceCenterIsInHit =
                pieceMidpoint >= hit.start - tolerance &&
                pieceMidpoint <= hit.end + tolerance;
            if (hitCenterIsInPiece || pieceCenterIsInHit) {
                removedIntervals.append({pieceStart, pieceEnd});
            }
        }
    }

    if (removedIntervals.isEmpty()) {
        return removedIntervals;
    }

    // The domain endpoints of a closed curve are one point. Keep the first
    // and last pieces connected unless a real intersection cuts the seam.
    QPointF firstPoint;
    QPointF lastPoint;
    const bool closed = evaluateNurbsPoint(curve, domainStart, &firstPoint) &&
                        evaluateNurbsPoint(curve, domainEnd, &lastPoint) &&
                        std::hypot(firstPoint.x() - lastPoint.x(),
                                   firstPoint.y() - lastPoint.y()) <= 1.0e-8;
    const bool seamIsIntersection = std::any_of(
        intersectionParameters.begin(), intersectionParameters.end(),
        [&](qreal parameter) {
            return std::abs(parameter - domainStart) <= tolerance ||
                   std::abs(parameter - domainEnd) <= tolerance;
        });
    if (closed && !seamIsIntersection && uniqueBoundaries.size() > 2) {
        const bool removesFirstPiece = std::any_of(
            removedIntervals.begin(), removedIntervals.end(),
            [&](const ParameterInterval &interval) {
                return interval.start <= domainStart + tolerance &&
                       interval.end > domainStart + tolerance;
            });
        const bool removesLastPiece = std::any_of(
            removedIntervals.begin(), removedIntervals.end(),
            [&](const ParameterInterval &interval) {
                return interval.end >= domainEnd - tolerance &&
                       interval.start < domainEnd - tolerance;
            });
        if (removesFirstPiece || removesLastPiece) {
            removedIntervals.append({domainStart, uniqueBoundaries[1]});
            removedIntervals.append(
                {uniqueBoundaries[uniqueBoundaries.size() - 2], domainEnd});
        }
    }

    std::sort(removedIntervals.begin(),
              removedIntervals.end(),
              [](const ParameterInterval &first,
                 const ParameterInterval &second) {
                  return first.start < second.start;
              });
    QVector<ParameterInterval> merged;
    for (const ParameterInterval &interval : removedIntervals) {
        if (!merged.isEmpty() &&
            interval.start <= merged.last().end + tolerance) {
            merged.last().end = std::max(merged.last().end, interval.end);
        } else {
            merged.append(interval);
        }
    }
    return merged;
}

bool keepNurbsCurveOutsideIntervals(
    const NurbsCurve2D &source,
    const QVector<ParameterInterval> &removedIntervals,
    QVector<NurbsCurve2D> *remainingCurves)
{
    if (remainingCurves == nullptr || !validateNurbsCurve(source)) {
        return false;
    }
    remainingCurves->clear();
    if (removedIntervals.isEmpty()) {
        remainingCurves->append(source);
        return true;
    }

    const QVector<double> fullKnots = expandedNurbsKnotVector(source);
    const qreal domainStart = fullKnots[source.degree];
    const qreal domainEnd = fullKnots[source.controlPoints.size()];
    qreal keepStart = domainStart;
    const qreal tolerance =
        std::max<qreal>(1.0e-9, std::abs(domainEnd - domainStart) * 1.0e-9);
    for (const ParameterInterval &removed : removedIntervals) {
        if (removed.start - keepStart > tolerance) {
            NurbsCurve2D kept;
            if (!trimNurbsCurve(source, keepStart, removed.start, &kept)) {
                remainingCurves->clear();
                return false;
            }
            remainingCurves->append(kept);
        }
        keepStart = std::max(keepStart, removed.end);
    }
    if (domainEnd - keepStart > tolerance) {
        NurbsCurve2D kept;
        if (!trimNurbsCurve(source, keepStart, domainEnd, &kept)) {
            remainingCurves->clear();
            return false;
        }
        remainingCurves->append(kept);
    }
    return true;
}

bool rebuildShapeFromCurveEraseFragments(
    const Shape &source,
    const QVector<NurbsCurve2D> &remainingCurves,
    const QVector<WorkPlaneFrame> &workPlaneFrames,
    qreal endpointTolerance,
    QVector<Shape> *replacementShapes)
{
    if (replacementShapes == nullptr ||
        remainingCurves.size() != workPlaneFrames.size()) {
        return false;
    }
    replacementShapes->clear();
    if (remainingCurves.isEmpty()) {
        return true;
    }

    const QVector<NurbsCurveFrameGroup> groups =
        connectedNurbsCurveGroupsInWorld(remainingCurves,
                                         workPlaneFrames,
                                         endpointTolerance);
    if (groups.isEmpty()) {
        return false;
    }

    const auto makePolyCurve = [&](const NurbsCurveFrameGroup &group) {
        Shape result = source;
        result.geometryType = GeometryType::PolyCurve;
        result.nurbs = NurbsCurve2D{};
        result.arcMode = ArcMode::TwoPoint;
        result.arcSweep = 0.0;
        result.subdivisionParameters.clear();
        result.components = group.curves;
        result.componentWorkPlaneFrames = group.workPlaneFrames;

        const WorkPlaneFrame resultFrame = shapeWorkPlaneFrame(source);
        QVector<QPointF> points;
        for (int index = 0; index < group.curves.size(); ++index) {
            QPointF start;
            QPointF end;
            if (!isValidWorkPlaneFrame(resultFrame) ||
                !isValidWorkPlaneFrame(group.workPlaneFrames[index]) ||
                !nurbsCurveEndpoints(group.curves[index], &start, &end)) {
                return Shape{};
            }
            const QPointF startInResultFrame = worldPointToWorkPlaneFrame(
                workPlaneFramePointToWorld(start, group.workPlaneFrames[index]),
                resultFrame);
            const QPointF endInResultFrame = worldPointToWorkPlaneFrame(
                workPlaneFramePointToWorld(end, group.workPlaneFrames[index]),
                resultFrame);
            if (index == 0) {
                points.append(startInResultFrame);
            }
            points.append(endInResultFrame);
        }
        result.points = points;
        return result;
    };

    for (const NurbsCurveFrameGroup &group : groups) {
        if (group.curves.size() != group.workPlaneFrames.size()) {
            replacementShapes->clear();
            return false;
        }
        if (source.geometryType == GeometryType::Line &&
            group.curves.size() == 1) {
            Shape result = source;
            result.geometryType = GeometryType::Line;
            result.nurbs = group.curves.first();
            result.points = result.nurbs.controlPoints;
            result.workPlaneFrame = group.workPlaneFrames.first();
            result.subdivisionParameters.clear();
            result.components.clear();
            result.componentWorkPlaneFrames.clear();
            replacementShapes->append(result);
            continue;
        }

        Shape result = makePolyCurve(group);
        if (result.geometryType != GeometryType::PolyCurve) {
            replacementShapes->clear();
            return false;
        }
        replacementShapes->append(result);
    }
    return true;
}

} // namespace classiCAD
