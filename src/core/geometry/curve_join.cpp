#include "curve_join.h"

#include "core/geometry/curve_construction.h"
#include "core/geometry/curve_editing.h"
#include "core/geometry/curve_evaluator.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

namespace classiCAD {
namespace {

qreal pointDistance(const QPointF &first, const QPointF &second)
{
    return std::hypot(first.x() - second.x(), first.y() - second.y());
}

qreal pointDistance(const Point3D &first, const Point3D &second)
{
    return std::hypot(std::hypot(first.x - second.x,
                                 first.y - second.y),
                      first.z - second.z);
}

qreal pointPrecision(const QPointF &first, const QPointF &second)
{
    const qreal scale = std::max({qreal(1.0), std::abs(first.x()),
                                  std::abs(first.y()), std::abs(second.x()),
                                  std::abs(second.y())});
    return std::max<qreal>(1.0e-10, scale * 1.0e-12);
}

qreal pointPrecision(const Point3D &first, const Point3D &second)
{
    const qreal scale = std::max({qreal(1.0), std::abs(first.x),
                                  std::abs(first.y), std::abs(first.z),
                                  std::abs(second.x), std::abs(second.y),
                                  std::abs(second.z)});
    return std::max<qreal>(1.0e-10, scale * 1.0e-12);
}

bool curveEndpointInWorld(const NurbsCurve2D &curve,
                          const WorkPlaneFrame &frame,
                          bool atStart,
                          Point3D *point)
{
    if (point == nullptr || !isValidWorkPlaneFrame(frame)) {
        return false;
    }
    qreal start = 0.0;
    qreal end = 0.0;
    Point3D localPoint;
    if (!nurbsParameterDomain(curve, &start, &end) ||
        !evaluateNurbsPoint3D(curve, atStart ? start : end, &localPoint)) {
        return false;
    }
    *point = workPlaneFramePointToWorld(
        {localPoint.x, localPoint.y}, localPoint.z, frame);
    return true;
}

bool closePlanarLoopSeam(QVector<NurbsCurve2D> *components,
                         bool shouldClose)
{
    if (components == nullptr ||
        std::any_of(components->cbegin(), components->cend(),
                    [](const NurbsCurve2D &curve) {
                        return curve.dimension != 2;
                    })) {
        return false;
    }
    if (!shouldClose) {
        return true;
    }
    if (components == nullptr || components->size() < 2) {
        return false;
    }
    NurbsCurve2D &last = components->last();
    if (last.controlPoints.isEmpty()) {
        return false;
    }

    QPointF firstStart;
    QPointF lastEnd;
    if (!nurbsCurveEndpoints(components->first(), &firstStart, nullptr) ||
        !nurbsCurveEndpoints(last, nullptr, &lastEnd)) {
        return false;
    }
    const qreal gap = pointDistance(firstStart, lastEnd);
    const qreal precision = pointPrecision(firstStart, lastEnd);
    if (gap <= precision) {
        return true;
    }

    const int endpointControlPoint = last.controlPoints.size() - 1;
    if (pointDistance(last.controlPoints[endpointControlPoint], lastEnd) >
        precision) {
        return false;
    }
    last.controlPoints[endpointControlPoint] = firstStart;

    QPointF verifiedEnd;
    return nurbsCurveEndpoints(last, nullptr, &verifiedEnd) &&
           pointDistance(firstStart, verifiedEnd) <= precision * 10.0;
}

bool closeWorldLoopSeam(QVector<NurbsCurve2D> *components,
                        QVector<WorkPlaneFrame> *frames,
                        bool shouldClose)
{
    if (!shouldClose) {
        return components != nullptr && frames != nullptr &&
               components->size() == frames->size();
    }
    if (components == nullptr || frames == nullptr ||
        components->size() != frames->size() || components->size() < 2) {
        return false;
    }
    NurbsCurve2D &last = components->last();
    if (last.controlPoints.isEmpty()) {
        return false;
    }

    Point3D firstWorld;
    Point3D lastWorld;
    if (!curveEndpointInWorld(components->first(), frames->first(), true,
                               &firstWorld) ||
        !curveEndpointInWorld(last, frames->last(), false, &lastWorld)) {
        return false;
    }
    const qreal gap = pointDistance(firstWorld, lastWorld);
    const qreal precision = pointPrecision(firstWorld, lastWorld);
    if (gap <= precision) {
        return true;
    }

    const int endpointControlPoint = last.controlPoints.size() - 1;
    const qreal endpointNormalCoordinate = last.dimension == 3
        ? last.normalCoordinates[endpointControlPoint] : 0.0;
    const Point3D endpointControlPointWorld = workPlaneFramePointToWorld(
        last.controlPoints[endpointControlPoint], endpointNormalCoordinate,
        frames->last());
    if (pointDistance(endpointControlPointWorld, lastWorld) > precision) {
        return false;
    }
    qreal normalCoordinate = 0.0;
    last.controlPoints[endpointControlPoint] = worldPointToWorkPlaneFrame(
        firstWorld, frames->last(), &normalCoordinate);
    if (last.dimension == 2 && std::abs(normalCoordinate) > 1.0e-9) {
        last.dimension = 3;
        last.normalCoordinates.fill(0.0, last.controlPoints.size());
    }
    if (last.dimension == 3) {
        last.normalCoordinates[endpointControlPoint] = normalCoordinate;
    }

    Point3D verifiedEnd;
    return curveEndpointInWorld(last, frames->last(), false, &verifiedEnd) &&
           pointDistance(firstWorld, verifiedEnd) <= precision * 10.0;
}

} // namespace

bool orderConnectedNurbsCurves(const QVector<NurbsCurve2D> &input,
                              QVector<NurbsCurve2D> *ordered,
                              qreal tolerance)
{
    if (ordered == nullptr || input.isEmpty() ||
        std::any_of(input.cbegin(), input.cend(),
                    [](const NurbsCurve2D &curve) {
                        return curve.dimension != 2;
                    })) {
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
    if (std::any_of(curves.cbegin(), curves.cend(),
                    [](const NurbsCurve2D &curve) {
                        return curve.dimension != 2;
                    })) {
        return {};
    }
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
        valid[index] = curveEndpointInWorld(curves[index], workPlaneFrames[index],
                                            true, &starts[index]) &&
                       curveEndpointInWorld(curves[index], workPlaneFrames[index],
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
        if (!curveEndpointInWorld(input[index], frames[index], true,
                                  &starts[index]) ||
            !curveEndpointInWorld(input[index], frames[index], false,
                                  &ends[index])) {
            return false;
        }
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
    if (std::any_of(components->cbegin(), components->cend(),
                    [](const NurbsCurve2D &curve) {
                        return curve.dimension != 2;
                    })) {
        return false;
    }

    bool shouldCloseLoop = false;
    if (components->size() > 1) {
        QPointF firstStart;
        QPointF lastEnd;
        if (!nurbsCurveEndpoints(components->first(), &firstStart, nullptr) ||
            !nurbsCurveEndpoints(components->last(), nullptr, &lastEnd)) {
            return false;
        }
        shouldCloseLoop = pointDistance(firstStart, lastEnd) <= tolerance;
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
    return closePlanarLoopSeam(components, shouldCloseLoop);
}

bool connectedNurbsCurvesAreContinuous(
    const QVector<NurbsCurve2D> &components,
    qreal tolerance)
{
    if (std::any_of(components.cbegin(), components.cend(),
                    [](const NurbsCurve2D &curve) {
                        return curve.dimension != 2;
                    })) {
        return false;
    }
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

    bool shouldCloseLoop = false;
    if (components->size() > 1) {
        Point3D firstWorld;
        Point3D lastWorld;
        if (!curveEndpointInWorld(components->first(), frames->first(), true,
                                  &firstWorld) ||
            !curveEndpointInWorld(components->last(), frames->last(), false,
                                  &lastWorld)) {
            return false;
        }
        shouldCloseLoop = pointDistance(firstWorld, lastWorld) <= tolerance;
    }

    for (int index = 0; index + 1 < components->size(); ++index) {
        Point3D previousWorld;
        Point3D nextWorld;
        if (!curveEndpointInWorld(components->at(index), frames->at(index),
                                  false, &previousWorld) ||
            !curveEndpointInWorld(components->at(index + 1),
                                  frames->at(index + 1), true, &nextWorld)) {
            return false;
        }
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
    return closeWorldLoopSeam(components, frames, shouldCloseLoop);
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
        Point3D first;
        Point3D second;
        if (!curveEndpointInWorld(components[index], frames[index], false,
                                  &first) ||
            !curveEndpointInWorld(components[index + 1], frames[index + 1],
                                  true, &second)) {
            return false;
        }
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
    if (std::any_of(components->cbegin(), components->cend(),
                    [](const NurbsCurve2D &curve) {
                        return curve.dimension != 2;
                    })) {
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
    // The overlap merger below operates on 2D line coordinates. Keep spatial
    // components separate so it cannot flatten their normal coordinates.
    if (std::any_of(components->cbegin(), components->cend(),
                    [](const NurbsCurve2D &curve) {
                        return curve.dimension == 3;
                    })) {
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
