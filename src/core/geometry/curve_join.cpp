#include "curve_join.h"

#include "core/geometry/curve_construction.h"
#include "core/geometry/curve_editing.h"
#include "core/geometry/curve_evaluator.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

namespace classiCAD {

bool orderConnectedNurbsCurves(const QVector<NurbsCurve2D> &input,
                              QVector<NurbsCurve2D> *ordered,
                              qreal tolerance)
{
    if (ordered == nullptr || input.isEmpty()) {
        return false;
    }

    QVector<QPointF> starts;
    QVector<QPointF> ends;
    starts.reserve(input.size());
    ends.reserve(input.size());
    for (const NurbsCurve2D &curve : input) {
        QPointF start;
        QPointF end;
        if (!nurbsCurveEndpoints(curve, &start, &end)) {
            return false;
        }
        starts.append(start);
        ends.append(end);
    }

    const auto endpointsMatch = [tolerance](const QPointF &first,
                                             const QPointF &second) {
        return std::hypot(first.x() - second.x(),
                          first.y() - second.y()) <= tolerance;
    };

    QVector<int> componentOrder;
    QVector<bool> componentReversed;
    QVector<char> used(input.size(), false);

    const auto makeResult = [&]() {
        ordered->clear();
        ordered->reserve(componentOrder.size());
        for (int position = 0; position < componentOrder.size(); ++position) {
            const int componentIndex = componentOrder[position];
            if (componentReversed[position]) {
                NurbsCurve2D reversed;
                if (!reverseNurbsCurve(input[componentIndex], &reversed)) {
                    ordered->clear();
                    return false;
                }
                ordered->append(reversed);
            } else {
                ordered->append(input[componentIndex]);
            }
        }
        return true;
    };

    std::function<bool(const QPointF &)> extendChain;
    extendChain = [&](const QPointF &currentEnd) {
        if (componentOrder.size() == input.size()) {
            return true;
        }

        for (int candidate = 0; candidate < input.size(); ++candidate) {
            if (used[candidate]) {
                continue;
            }

            if (endpointsMatch(currentEnd, starts[candidate])) {
                used[candidate] = true;
                componentOrder.append(candidate);
                componentReversed.append(false);
                if (extendChain(ends[candidate])) {
                    return true;
                }
                componentReversed.removeLast();
                componentOrder.removeLast();
                used[candidate] = false;
            }

            if (endpointsMatch(currentEnd, ends[candidate])) {
                used[candidate] = true;
                componentOrder.append(candidate);
                componentReversed.append(true);
                if (extendChain(starts[candidate])) {
                    return true;
                }
                componentReversed.removeLast();
                componentOrder.removeLast();
                used[candidate] = false;
            }
        }
        return false;
    };

    for (int first = 0; first < input.size(); ++first) {
        for (const bool reverseFirst : {false, true}) {
            std::fill(used.begin(), used.end(), false);
            componentOrder.clear();
            componentReversed.clear();
            used[first] = true;
            componentOrder.append(first);
            componentReversed.append(reverseFirst);
            const QPointF firstEnd = reverseFirst ? starts[first] : ends[first];
            if (extendChain(firstEnd)) {
                return makeResult();
            }
        }
    }

    return false;
}

QVector<QVector<NurbsCurve2D>> connectedNurbsCurveGroups(
    const QVector<NurbsCurve2D> &curves,
    qreal orderingTolerance)
{
    QVector<QPointF> starts;
    QVector<QPointF> ends;
    starts.reserve(curves.size());
    ends.reserve(curves.size());
    qreal coordinateScale = 1.0;
    for (const NurbsCurve2D &curve : curves) {
        QPointF start;
        QPointF end;
        if (!nurbsCurveEndpoints(curve, &start, &end)) {
            starts.append(QPointF());
            ends.append(QPointF());
            continue;
        }
        starts.append(start);
        ends.append(end);
        coordinateScale = std::max({coordinateScale,
                                    std::abs(start.x()),
                                    std::abs(start.y()),
                                    std::abs(end.x()),
                                    std::abs(end.y())});
    }

    const qreal endpointTolerance =
        std::max<qreal>(1.0e-8, coordinateScale * 1.0e-12);
    const auto endpointsMatch = [endpointTolerance](const QPointF &first,
                                                     const QPointF &second) {
        return std::hypot(first.x() - second.x(),
                          first.y() - second.y()) <= endpointTolerance;
    };
    QVector<QVector<NurbsCurve2D>> groups;
    QVector<bool> grouped(curves.size(), false);
    for (int seed = 0; seed < curves.size(); ++seed) {
        if (grouped[seed]) {
            continue;
        }
        QVector<int> connectedIndices{seed};
        grouped[seed] = true;
        for (int cursor = 0; cursor < connectedIndices.size(); ++cursor) {
            const int current = connectedIndices[cursor];
            if (!validateNurbsCurve(curves[current])) {
                continue;
            }
            for (int candidate = 0; candidate < curves.size(); ++candidate) {
                if (grouped[candidate] ||
                    !validateNurbsCurve(curves[candidate])) {
                    continue;
                }
                if (endpointsMatch(starts[current], starts[candidate]) ||
                    endpointsMatch(starts[current], ends[candidate]) ||
                    endpointsMatch(ends[current], starts[candidate]) ||
                    endpointsMatch(ends[current], ends[candidate])) {
                    grouped[candidate] = true;
                    connectedIndices.append(candidate);
                }
            }
        }

        std::sort(connectedIndices.begin(), connectedIndices.end());
        QVector<NurbsCurve2D> group;
        group.reserve(connectedIndices.size());
        for (const int index : connectedIndices) {
            group.append(curves[index]);
        }
        QVector<NurbsCurve2D> ordered;
        if (group.size() > 1 &&
            orderConnectedNurbsCurves(group, &ordered, orderingTolerance)) {
            group = std::move(ordered);
        }
        groups.append(std::move(group));
    }
    return groups;
}

QVector<NurbsCurveFrameGroup> connectedNurbsCurveGroupsInWorld(
    const QVector<NurbsCurve2D> &curves,
    const QVector<WorkPlaneFrame> &workPlaneFrames,
    qreal orderingTolerance)
{
    if (curves.size() != workPlaneFrames.size()) {
        return {};
    }

    const auto worldPoint = [](const NurbsCurve2D &curve,
                               const WorkPlaneFrame &frame,
                               bool atStart,
                               Point3D *point) {
        if (point == nullptr || !isValidWorkPlaneFrame(frame)) {
            return false;
        }
        QPointF localPoint;
        if (!nurbsCurveEndpoints(curve,
                                 atStart ? &localPoint : nullptr,
                                 atStart ? nullptr : &localPoint)) {
            return false;
        }
        *point = workPlaneFramePointToWorld(localPoint, frame);
        return true;
    };
    const auto distance = [](const Point3D &first, const Point3D &second) {
        return std::hypot(std::hypot(first.x - second.x,
                                     first.y - second.y),
                          first.z - second.z);
    };

    QVector<Point3D> starts(curves.size());
    QVector<Point3D> ends(curves.size());
    QVector<bool> valid(curves.size(), false);
    qreal coordinateScale = 1.0;
    for (int index = 0; index < curves.size(); ++index) {
        valid[index] = worldPoint(curves[index], workPlaneFrames[index],
                                  true, &starts[index]) &&
                       worldPoint(curves[index], workPlaneFrames[index],
                                  false, &ends[index]);
        if (!valid[index]) {
            continue;
        }
        coordinateScale = std::max({coordinateScale,
                                    std::abs(starts[index].x),
                                    std::abs(starts[index].y),
                                    std::abs(starts[index].z),
                                    std::abs(ends[index].x),
                                    std::abs(ends[index].y),
                                    std::abs(ends[index].z)});
    }
    const qreal endpointTolerance =
        std::max<qreal>(1.0e-8, coordinateScale * 1.0e-12);
    const auto endpointsMatch = [&](const Point3D &first,
                                    const Point3D &second) {
        return distance(first, second) <= endpointTolerance;
    };

    QVector<NurbsCurveFrameGroup> groups;
    QVector<bool> grouped(curves.size(), false);
    for (int seed = 0; seed < curves.size(); ++seed) {
        if (grouped[seed]) {
            continue;
        }
        QVector<int> connectedIndices{seed};
        grouped[seed] = true;
        for (int cursor = 0; cursor < connectedIndices.size(); ++cursor) {
            const int current = connectedIndices[cursor];
            if (!valid[current]) {
                continue;
            }
            for (int candidate = 0; candidate < curves.size(); ++candidate) {
                if (grouped[candidate] || !valid[candidate]) {
                    continue;
                }
                if (endpointsMatch(starts[current], starts[candidate]) ||
                    endpointsMatch(starts[current], ends[candidate]) ||
                    endpointsMatch(ends[current], starts[candidate]) ||
                    endpointsMatch(ends[current], ends[candidate])) {
                    grouped[candidate] = true;
                    connectedIndices.append(candidate);
                }
            }
        }

        std::sort(connectedIndices.begin(), connectedIndices.end());
        NurbsCurveFrameGroup group;
        group.curves.reserve(connectedIndices.size());
        group.workPlaneFrames.reserve(connectedIndices.size());
        for (const int index : connectedIndices) {
            group.curves.append(curves[index]);
            group.workPlaneFrames.append(workPlaneFrames[index]);
        }
        if (group.curves.size() > 1) {
            QVector<NurbsCurve2D> orderedCurves;
            QVector<WorkPlaneFrame> orderedFrames;
            if (orderConnectedNurbsCurvesInWorld(group.curves,
                                                 group.workPlaneFrames,
                                                 &orderedCurves,
                                                 &orderedFrames,
                                                 orderingTolerance)) {
                group.curves = std::move(orderedCurves);
                group.workPlaneFrames = std::move(orderedFrames);
            }
        }
        groups.append(std::move(group));
    }
    return groups;
}

bool orderConnectedNurbsCurvesInWorld(
    const QVector<NurbsCurve2D> &input,
    const QVector<WorkPlaneFrame> &frames,
    QVector<NurbsCurve2D> *ordered,
    QVector<WorkPlaneFrame> *orderedFrames,
    qreal tolerance)
{
    if (ordered == nullptr || orderedFrames == nullptr || input.isEmpty() ||
        input.size() != frames.size()) {
        return false;
    }

    const auto distance = [](const Point3D &first, const Point3D &second) {
        return std::hypot(std::hypot(first.x - second.x,
                                     first.y - second.y),
                          first.z - second.z);
    };
    QVector<Point3D> starts;
    QVector<Point3D> ends;
    starts.reserve(input.size());
    ends.reserve(input.size());
    for (int index = 0; index < input.size(); ++index) {
        QPointF start;
        QPointF end;
        if (!isValidWorkPlaneFrame(frames[index]) ||
            !nurbsCurveEndpoints(input[index], &start, &end)) {
            return false;
        }
        starts.append(workPlaneFramePointToWorld(start, frames[index]));
        ends.append(workPlaneFramePointToWorld(end, frames[index]));
    }

    QVector<int> componentOrder;
    QVector<bool> componentReversed;
    QVector<bool> used(input.size(), false);
    const auto makeResult = [&]() {
        ordered->clear();
        orderedFrames->clear();
        ordered->reserve(componentOrder.size());
        orderedFrames->reserve(componentOrder.size());
        for (int position = 0; position < componentOrder.size(); ++position) {
            const int componentIndex = componentOrder[position];
            if (componentReversed[position]) {
                NurbsCurve2D reversed;
                if (!reverseNurbsCurve(input[componentIndex], &reversed)) {
                    ordered->clear();
                    orderedFrames->clear();
                    return false;
                }
                ordered->append(reversed);
            } else {
                ordered->append(input[componentIndex]);
            }
            orderedFrames->append(frames[componentIndex]);
        }
        return true;
    };

    std::function<bool(const Point3D &)> extendChain;
    extendChain = [&](const Point3D &currentEnd) {
        if (componentOrder.size() == input.size()) {
            return true;
        }
        for (int candidate = 0; candidate < input.size(); ++candidate) {
            if (used[candidate]) {
                continue;
            }
            for (const bool reverseCandidate : {false, true}) {
                const Point3D &candidateStart = reverseCandidate
                                                    ? ends[candidate]
                                                    : starts[candidate];
                const Point3D &candidateEnd = reverseCandidate
                                                  ? starts[candidate]
                                                  : ends[candidate];
                if (distance(currentEnd, candidateStart) > tolerance) {
                    continue;
                }
                used[candidate] = true;
                componentOrder.append(candidate);
                componentReversed.append(reverseCandidate);
                if (extendChain(candidateEnd)) {
                    return true;
                }
                componentReversed.removeLast();
                componentOrder.removeLast();
                used[candidate] = false;
            }
        }
        return false;
    };

    for (int first = 0; first < input.size(); ++first) {
        for (const bool reverseFirst : {false, true}) {
            std::fill(used.begin(), used.end(), false);
            componentOrder.clear();
            componentReversed.clear();
            used[first] = true;
            componentOrder.append(first);
            componentReversed.append(reverseFirst);
            const Point3D &firstEnd = reverseFirst ? starts[first]
                                                   : ends[first];
            if (extendChain(firstEnd)) {
                return makeResult();
            }
        }
    }
    return false;
}

bool closeConnectedNurbsCurveGaps(QVector<NurbsCurve2D> *components,
                                  qreal tolerance)
{
    if (components == nullptr || components->isEmpty()) {
        return false;
    }

    for (int index = 0; index + 1 < components->size(); ++index) {
        QPointF previousEnd;
        QPointF nextStart;
        if (!nurbsCurveEndpoints(components->at(index), nullptr, &previousEnd) ||
            !nurbsCurveEndpoints(components->at(index + 1), &nextStart, nullptr)) {
            return false;
        }
        const QPointF delta = previousEnd - nextStart;
        if (std::hypot(delta.x(), delta.y()) > tolerance) {
            return false;
        }
        for (QPointF &controlPoint : (*components)[index + 1].controlPoints) {
            controlPoint += delta;
        }
    }
    return true;
}

bool connectedNurbsCurvesAreContinuous(
    const QVector<NurbsCurve2D> &components,
    qreal tolerance)
{
    for (int index = 0; index + 1 < components.size(); ++index) {
        QPointF previousEnd;
        QPointF nextStart;
        if (!nurbsCurveEndpoints(components[index], nullptr, &previousEnd) ||
            !nurbsCurveEndpoints(components[index + 1], &nextStart, nullptr)) {
            return false;
        }
        if (std::hypot(previousEnd.x() - nextStart.x(),
                       previousEnd.y() - nextStart.y()) > tolerance) {
            return false;
        }
    }
    return true;
}

bool closeConnectedNurbsCurveGapsInWorld(
    QVector<NurbsCurve2D> *components,
    QVector<WorkPlaneFrame> *frames,
    qreal tolerance)
{
    if (components == nullptr || frames == nullptr || components->isEmpty() ||
        components->size() != frames->size()) {
        return false;
    }
    for (int index = 0; index + 1 < components->size(); ++index) {
        QPointF previousEnd;
        QPointF nextStart;
        if (!nurbsCurveEndpoints(components->at(index), nullptr, &previousEnd) ||
            !nurbsCurveEndpoints(components->at(index + 1), &nextStart, nullptr)) {
            return false;
        }
        const Point3D previousWorld = workPlaneFramePointToWorld(
            previousEnd, frames->at(index));
        const Point3D nextWorld = workPlaneFramePointToWorld(
            nextStart, frames->at(index + 1));
        const Point3D delta{previousWorld.x - nextWorld.x,
                            previousWorld.y - nextWorld.y,
                            previousWorld.z - nextWorld.z};
        const qreal gap = std::hypot(std::hypot(delta.x, delta.y), delta.z);
        if (gap > tolerance) {
            return false;
        }
        WorkPlaneFrame &nextFrame = (*frames)[index + 1];
        nextFrame.origin.x += delta.x;
        nextFrame.origin.y += delta.y;
        nextFrame.origin.z += delta.z;
    }
    return true;
}

bool connectedNurbsCurvesAreContinuousInWorld(
    const QVector<NurbsCurve2D> &components,
    const QVector<WorkPlaneFrame> &frames,
    qreal tolerance)
{
    if (components.size() != frames.size()) {
        return false;
    }
    for (int index = 0; index + 1 < components.size(); ++index) {
        QPointF previousEnd;
        QPointF nextStart;
        if (!nurbsCurveEndpoints(components[index], nullptr, &previousEnd) ||
            !nurbsCurveEndpoints(components[index + 1], &nextStart, nullptr)) {
            return false;
        }
        const Point3D first = workPlaneFramePointToWorld(previousEnd,
                                                          frames[index]);
        const Point3D second = workPlaneFramePointToWorld(nextStart,
                                                           frames[index + 1]);
        const qreal gap = std::hypot(std::hypot(first.x - second.x,
                                               first.y - second.y),
                                     first.z - second.z);
        if (gap > tolerance) {
            return false;
        }
    }
    return true;
}

int fuseOverlappingNurbsLineComponents(QVector<NurbsCurve2D> *components,
                                       qreal tolerance)
{
    if (components == nullptr || components->isEmpty()) {
        return 0;
    }

    // Compare each straight span of a degree-one spline, including bent
    // polylines. Keep original knot domains and rational data when splitting;
    // only a fused union receives a new line representation.
    QVector<NurbsCurve2D> spans;
    for (const NurbsCurve2D &curve : *components) {
        if (curve.degree != 1 || curve.controlPoints.size() <= 2) {
            spans.append(curve);
            continue;
        }
        const QVector<double> knots = expandedNurbsKnotVector(curve);
        QVector<NurbsCurve2D> curveSpans;
        bool splitValid = true;
        for (int knotIndex = curve.degree;
             knotIndex < curve.controlPoints.size();
             ++knotIndex) {
            if (knots[knotIndex + 1] <= knots[knotIndex]) {
                continue;
            }
            NurbsCurve2D span;
            if (!trimNurbsCurve(curve,
                                knots[knotIndex],
                                knots[knotIndex + 1],
                                &span)) {
                splitValid = false;
                break;
            }
            curveSpans.append(span);
        }
        if (splitValid && !curveSpans.isEmpty()) {
            spans += curveSpans;
        } else {
            spans.append(curve);
        }
    }
    *components = spans;

    int mergeCount = 0;
    bool merged = true;
    while (merged) {
        merged = false;
        for (int firstIndex = 0;
             firstIndex < components->size() && !merged;
             ++firstIndex) {
            const NurbsCurve2D &firstCurve = components->at(firstIndex);
            if (firstCurve.degree != 1 ||
                firstCurve.controlPoints.size() != 2) {
                continue;
            }

            QPointF firstStart;
            QPointF firstEnd;
            if (!nurbsCurveEndpoints(firstCurve, &firstStart, &firstEnd)) {
                continue;
            }
            const QPointF firstDirection = firstEnd - firstStart;
            const qreal firstLength = std::hypot(firstDirection.x(),
                                                 firstDirection.y());
            if (firstLength <= tolerance) {
                continue;
            }
            const QPointF axis = firstDirection / firstLength;

            for (int secondIndex = firstIndex + 1;
                 secondIndex < components->size();
                 ++secondIndex) {
                const NurbsCurve2D &secondCurve = components->at(secondIndex);
                if (secondCurve.degree != 1 ||
                    secondCurve.controlPoints.size() != 2) {
                    continue;
                }

                QPointF secondStart;
                QPointF secondEnd;
                if (!nurbsCurveEndpoints(secondCurve,
                                         &secondStart,
                                         &secondEnd)) {
                    continue;
                }
                const QPointF secondDirection = secondEnd - secondStart;
                const qreal secondLength = std::hypot(secondDirection.x(),
                                                      secondDirection.y());
                if (secondLength <= tolerance) {
                    continue;
                }
                const QPointF secondAxis = secondDirection / secondLength;
                const qreal directionCross =
                    axis.x() * secondAxis.y() - axis.y() * secondAxis.x();
                if (std::abs(directionCross) > 1.0e-6) {
                    continue;
                }

                const auto perpendicularDistance = [&](const QPointF &point) {
                    const QPointF delta = point - firstStart;
                    return std::abs(axis.x() * delta.y() -
                                    axis.y() * delta.x());
                };
                if (perpendicularDistance(secondStart) > tolerance ||
                    perpendicularDistance(secondEnd) > tolerance) {
                    continue;
                }

                const qreal firstProjection =
                    QPointF::dotProduct(firstEnd - firstStart, axis);
                const qreal secondStartProjection =
                    QPointF::dotProduct(secondStart - firstStart, axis);
                const qreal secondEndProjection =
                    QPointF::dotProduct(secondEnd - firstStart, axis);
                const qreal firstLow = std::min<qreal>(0.0, firstProjection);
                const qreal firstHigh = std::max<qreal>(0.0, firstProjection);
                const qreal secondLow = std::min(secondStartProjection,
                                                 secondEndProjection);
                const qreal secondHigh = std::max(secondStartProjection,
                                                  secondEndProjection);
                if (secondLow > firstHigh + tolerance ||
                    firstLow > secondHigh + tolerance) {
                    continue;
                }

                const qreal unionLow = std::min(firstLow, secondLow);
                const qreal unionHigh = std::max(firstHigh, secondHigh);
                const QPointF fusedStart = firstStart + axis * unionLow;
                const QPointF fusedEnd = firstStart + axis * unionHigh;
                (*components)[firstIndex] =
                    makeDegreeOneNurbs({fusedStart, fusedEnd});
                components->removeAt(secondIndex);
                ++mergeCount;
                merged = true;
                break;
            }
        }
    }
    return mergeCount;
}

int fuseOverlappingNurbsLineComponentsInWorld(
    QVector<NurbsCurve2D> *components,
    QVector<WorkPlaneFrame> *frames,
    qreal tolerance)
{
    if (components == nullptr || frames == nullptr ||
        components->size() != frames->size()) {
        return 0;
    }

    // Split bent curves in their own planes before comparing spans. A straight
    // span can belong to several different planes.
    QVector<NurbsCurve2D> spans;
    QVector<WorkPlaneFrame> spanFrames;
    int mergeCount = 0;
    for (int index = 0; index < components->size(); ++index) {
        QVector<NurbsCurve2D> pieces{components->at(index)};
        mergeCount += fuseOverlappingNurbsLineComponents(&pieces, tolerance);
        for (const NurbsCurve2D &piece : pieces) {
            spans.append(piece);
            spanFrames.append(frames->at(index));
        }
    }
    *components = spans;
    *frames = spanFrames;

    bool merged = true;
    while (merged) {
        merged = false;
        for (int first = 0; first < components->size() && !merged; ++first) {
            if (components->at(first).degree != 1 ||
                components->at(first).controlPoints.size() != 2) {
                continue;
            }
            for (int second = first + 1; second < components->size(); ++second) {
                if (components->at(second).degree != 1 ||
                    components->at(second).controlPoints.size() != 2) {
                    continue;
                }
                NurbsCurve2D mapped = components->at(second);
                bool inPlane = true;
                for (QPointF &point : mapped.controlPoints) {
                    const Point3D world = workPlaneFramePointToWorld(
                        point, frames->at(second));
                    point = worldPointToWorkPlaneFrame(world,
                                                       frames->at(first));
                    const Point3D projected = workPlaneFramePointToWorld(
                        point, frames->at(first));
                    inPlane = inPlane && std::hypot(
                        std::hypot(world.x - projected.x,
                                   world.y - projected.y),
                        world.z - projected.z) <= 1.0e-7;
                }
                if (!inPlane) {
                    continue;
                }
                QVector<NurbsCurve2D> pair{components->at(first), mapped};
                const int pairMergeCount =
                    fuseOverlappingNurbsLineComponents(&pair, tolerance);
                if (pair.size() == 1) {
                    (*components)[first] = pair.first();
                    components->removeAt(second);
                    frames->removeAt(second);
                    mergeCount += pairMergeCount;
                    merged = true;
                    break;
                }
            }
        }
    }
    return mergeCount;
}

} // namespace classiCAD
