#include "weld_command.h"

#include "core/geometry/curve_editing.h"
#include "core/geometry/curve_evaluator.h"
#include "core/geometry/curve_intersections.h"
#include "core/geometry/shape_mapping.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <QSet>
#include <utility>

namespace classiCAD {
namespace {

struct WeldCut {
    qreal parameter = 0.0;
    Point3D worldPoint;
    int sampleCount = 0;
};

struct WeldCurveRecord {
    ObjectId objectId = ObjectId::invalid();
    Shape::NurbsCurve2D curve;
    int firstControlPointIndex = 0;
    WorkPlaneFrame frame;
    WorkPlaneFrame effectiveFrame;
    Point3D placementTranslation;
    QVector<WeldCut> cuts;
};

struct WeldOwner {
    ObjectId objectId = ObjectId::invalid();
    Shape sourceShape;
    QVector<int> curveRecordIndices;
};

struct WeldIntersectionEvent {
    int firstCurveIndex = -1;
    int secondCurveIndex = -1;
    qreal firstParameter = 0.0;
    qreal secondParameter = 0.0;
    Point3D worldPoint;
    int nodeIndex = -1;
};

struct WeldNode {
    Point3D worldPoint;
    int sampleCount = 0;
};

qreal squaredDistance(const Point3D &first, const Point3D &second)
{
    const qreal dx = first.x - second.x;
    const qreal dy = first.y - second.y;
    const qreal dz = first.z - second.z;
    return dx * dx + dy * dy + dz * dz;
}

Point3D subtract(const Point3D &first, const Point3D &second)
{
    return {first.x - second.x, first.y - second.y, first.z - second.z};
}

Point3D add(const Point3D &first, const Point3D &second)
{
    return {first.x + second.x, first.y + second.y, first.z + second.z};
}

Point3D scale(const Point3D &point, qreal factor)
{
    return {point.x * factor, point.y * factor, point.z * factor};
}

qreal dot(const Point3D &first, const Point3D &second)
{
    return first.x * second.x + first.y * second.y + first.z * second.z;
}

Point3D worldDerivative(const QPointF &localDerivative,
                        const WorkPlaneFrame &frame)
{
    return {frame.xAxis.x * localDerivative.x() +
                frame.yAxis.x * localDerivative.y(),
            frame.xAxis.y * localDerivative.x() +
                frame.yAxis.y * localDerivative.y(),
            frame.xAxis.z * localDerivative.x() +
                frame.yAxis.z * localDerivative.y()};
}

void translateFrame(WorkPlaneFrame *frame, const Point3D &translation)
{
    if (frame == nullptr) {
        return;
    }
    frame->origin.x += translation.x;
    frame->origin.y += translation.y;
    frame->origin.z += translation.z;
}

bool isTransverseCrossing(const WeldCurveRecord &first,
                          qreal firstParameter,
                          const WeldCurveRecord &second,
                          qreal secondParameter)
{
    QPointF firstLocalDerivative;
    QPointF secondLocalDerivative;
    if (!evaluateNurbsDerivative(first.curve,
                                 firstParameter,
                                 &firstLocalDerivative) ||
        !evaluateNurbsDerivative(second.curve,
                                 secondParameter,
                                 &secondLocalDerivative)) {
        return false;
    }
    const Point3D firstDerivative =
        worldDerivative(firstLocalDerivative, first.effectiveFrame);
    const Point3D secondDerivative =
        worldDerivative(secondLocalDerivative, second.effectiveFrame);
    const qreal firstLengthSquared = dot(firstDerivative, firstDerivative);
    const qreal secondLengthSquared = dot(secondDerivative, secondDerivative);
    if (firstLengthSquared <= 1.0e-24 || secondLengthSquared <= 1.0e-24) {
        return false;
    }

    // Match the X-crossing tolerance used by rCAD's weld helper: nearly
    // parallel segments are not treated as a crossing.
    constexpr qreal parallelCosineThreshold = 0.9995;
    const qreal cosine = std::abs(dot(firstDerivative, secondDerivative)) /
                         std::sqrt(firstLengthSquared * secondLengthSquared);
    return cosine < parallelCosineThreshold;
}

qreal coordinateScale(const Point3D &point)
{
    return std::max<qreal>({1.0, std::abs(point.x), std::abs(point.y),
                            std::abs(point.z)});
}

bool hasMultiplePolylineSegments(const Shape::NurbsCurve2D &curve)
{
    return curve.degree == 1 && curve.controlPoints.size() >= 4;
}

QVector<QPointF> polyCurvePoints(
    const QVector<Shape::NurbsCurve2D> &components,
    const QVector<WorkPlaneFrame> &componentFrames,
    const WorkPlaneFrame &resultFrame)
{
    QVector<QPointF> points;
    for (int index = 0; index < components.size(); ++index) {
        QPointF start;
        QPointF end;
        if (!nurbsCurveEndpoints(components[index], &start, &end)) {
            continue;
        }
        const WorkPlaneFrame componentFrame = componentFrames.value(index,
                                                                     resultFrame);
        const QPointF localStart = worldPointToWorkPlaneFrame(
            workPlaneFramePointToWorld(start, componentFrame), resultFrame);
        const QPointF localEnd = worldPointToWorkPlaneFrame(
            workPlaneFramePointToWorld(end, componentFrame), resultFrame);
        if (index == 0) {
            points.append(localStart);
        }
        points.append(localEnd);
    }
    return points;
}

bool splitCurveAtCuts(const WeldCurveRecord &record,
                      QVector<Shape::NurbsCurve2D> *pieces)
{
    if (pieces == nullptr || !validateNurbsCurve(record.curve)) {
        return false;
    }
    pieces->clear();
    if (record.cuts.isEmpty()) {
        pieces->append(record.curve);
        return true;
    }

    qreal domainStart = 0.0;
    qreal domainEnd = 0.0;
    if (!nurbsParameterDomain(record.curve, &domainStart, &domainEnd)) {
        return false;
    }
    const qreal domainLength = domainEnd - domainStart;
    const qreal parameterTolerance =
        std::max<qreal>(1.0e-9, domainLength * 1.0e-8);
    QVector<WeldCut> sortedCuts = record.cuts;
    std::sort(sortedCuts.begin(), sortedCuts.end(),
              [](const WeldCut &first, const WeldCut &second) {
                  return first.parameter < second.parameter;
              });

    QVector<WeldCut> uniqueCuts;
    for (const WeldCut &cut : sortedCuts) {
        if (cut.parameter <= domainStart + parameterTolerance ||
            cut.parameter >= domainEnd - parameterTolerance) {
            continue;
        }
        if (!uniqueCuts.isEmpty() &&
            std::abs(uniqueCuts.last().parameter - cut.parameter) <=
                parameterTolerance) {
            WeldCut &existing = uniqueCuts.last();
            existing.worldPoint = scale(
                add(scale(existing.worldPoint, existing.sampleCount),
                    cut.worldPoint),
                1.0 / (existing.sampleCount + 1));
            ++existing.sampleCount;
            continue;
        }
        WeldCut unique = cut;
        unique.sampleCount = 1;
        uniqueCuts.append(unique);
    }
    if (uniqueCuts.isEmpty()) {
        pieces->append(record.curve);
        return true;
    }

    QVector<qreal> boundaries;
    boundaries.reserve(uniqueCuts.size() + 2);
    boundaries.append(domainStart);
    for (const WeldCut &cut : uniqueCuts) {
        boundaries.append(cut.parameter);
    }
    boundaries.append(domainEnd);

    pieces->reserve(uniqueCuts.size() + 1);
    for (int intervalIndex = 0; intervalIndex + 1 < boundaries.size();
         ++intervalIndex) {
        Shape::NurbsCurve2D piece;
        if (!trimNurbsCurve(record.curve,
                            boundaries[intervalIndex],
                            boundaries[intervalIndex + 1],
                            &piece)) {
            pieces->clear();
            return false;
        }

        const auto snapEndpoint = [&](int controlPointIndex,
                                      const WeldCut &cut) {
            Point3D pointWithoutPlacement = subtract(
                cut.worldPoint, record.placementTranslation);
            piece.controlPoints[controlPointIndex] =
                worldPointToWorkPlaneFrame(pointWithoutPlacement,
                                           record.frame);
        };
        if (intervalIndex > 0) {
            snapEndpoint(0, uniqueCuts[intervalIndex - 1]);
        }
        if (intervalIndex < uniqueCuts.size()) {
            snapEndpoint(piece.controlPoints.size() - 1,
                         uniqueCuts[intervalIndex]);
        }
        if (!validateNurbsCurve(piece)) {
            pieces->clear();
            return false;
        }
        pieces->append(std::move(piece));
    }
    return !pieces->isEmpty();
}

} // namespace

bool buildWeldCommandPlan(const Document &document,
                          const QVector<ObjectId> &selectedObjectIds,
                          WeldCommandPlan *plan,
                          const QVector<ObjectId> &focusedObjectIds)
{
    if (plan == nullptr) {
        return false;
    }
    *plan = WeldCommandPlan{};
    QSet<quint64> focusedIds;
    for (const ObjectId objectId : focusedObjectIds) {
        if (objectId.isValid()) {
            focusedIds.insert(objectId.value());
        }
    }

    QVector<WeldOwner> owners;
    QVector<WeldCurveRecord> curves;
    for (const SceneObject &object : document.objects()) {
        if (!selectedObjectIds.contains(object.id) ||
            !document.isObjectEditable(object.id)) {
            continue;
        }

        const QVector<ShapeNurbsCurveComponent> sourceComponents =
            nurbsCurveComponentsForShape(object.geometry);
        if (sourceComponents.isEmpty()) {
            continue;
        }

        WeldOwner owner;
        owner.objectId = object.id;
        owner.sourceShape = object.geometry;
        const int ownerIndex = owners.size();
        owners.append(std::move(owner));
        int firstControlPointIndex = 0;
        for (const ShapeNurbsCurveComponent &component : sourceComponents) {
            WeldCurveRecord record;
            record.objectId = object.id;
            record.curve = component.curve;
            record.firstControlPointIndex = firstControlPointIndex;
            record.frame = component.workPlaneFrame;
            record.placementTranslation = object.placementTranslation;
            record.effectiveFrame = record.frame;
            translateFrame(&record.effectiveFrame,
                           object.placementTranslation);
            firstControlPointIndex += component.curve.controlPoints.size();

            if (!validateNurbsCurve(record.curve)) {
                owners[ownerIndex].curveRecordIndices.append(curves.size());
                curves.append(std::move(record));
                continue;
            }
            if (record.curve.dimension != 2) {
                plan->failureMessage = QStringLiteral(
                    "Weld supports planar curves only; the selection includes a spatial curve");
                return true;
            }
            if (!isValidWorkPlaneFrame(record.frame)) {
                plan->failureMessage = QStringLiteral(
                    "Weld could not resolve a selected curve's work plane");
                return true;
            }
            owners[ownerIndex].curveRecordIndices.append(curves.size());
            curves.append(std::move(record));
        }
    }

    if (curves.isEmpty()) {
        plan->failureMessage = QStringLiteral(
            "Weld needs at least one selected planar curve");
        return true;
    }

    QVector<WeldIntersectionEvent> events;
    for (int firstIndex = 0; firstIndex < curves.size(); ++firstIndex) {
        const WeldCurveRecord &first = curves[firstIndex];
        if (!validateNurbsCurve(first.curve)) {
            continue;
        }
        for (int secondIndex = firstIndex; secondIndex < curves.size();
             ++secondIndex) {
            const WeldCurveRecord &second = curves[secondIndex];
            if (!validateNurbsCurve(second.curve)) {
                continue;
            }
            if (!focusedIds.isEmpty() &&
                !focusedIds.contains(first.objectId.value()) &&
                !focusedIds.contains(second.objectId.value())) {
                continue;
            }
            const bool sameCurve = firstIndex == secondIndex;
            if (sameCurve && !hasMultiplePolylineSegments(first.curve)) {
                continue;
            }

            const NurbsCurveIntersectionResult intersections =
                intersectNurbsCurves(first.curve,
                                     first.effectiveFrame,
                                     second.curve,
                                     second.effectiveFrame);
            qreal firstDomainStart = 0.0;
            qreal firstDomainEnd = 0.0;
            qreal secondDomainStart = 0.0;
            qreal secondDomainEnd = 0.0;
            if (!nurbsParameterDomain(first.curve,
                                      &firstDomainStart,
                                      &firstDomainEnd) ||
                !nurbsParameterDomain(second.curve,
                                      &secondDomainStart,
                                      &secondDomainEnd)) {
                continue;
            }
            const qreal firstInteriorTolerance =
                std::max<qreal>(1.0e-9,
                                (firstDomainEnd - firstDomainStart) * 1.0e-8);
            const qreal secondInteriorTolerance =
                std::max<qreal>(1.0e-9,
                                (secondDomainEnd - secondDomainStart) * 1.0e-8);
            const qreal selfParameterTolerance =
                std::max(firstInteriorTolerance, secondInteriorTolerance);

            for (const NurbsCurveIntersection &intersection :
                 intersections.intersections) {
                const qreal firstParameter =
                    intersection.firstCurveParameter;
                const qreal secondParameter =
                    intersection.secondCurveParameter;
                if (sameCurve &&
                    firstParameter >= secondParameter -
                                          selfParameterTolerance) {
                    // Self-intersection queries return both (a, b) and (b, a),
                    // as well as the trivial diagonal. Keep one ordered pair.
                    continue;
                }
                if (firstParameter <= firstDomainStart +
                                          firstInteriorTolerance ||
                    firstParameter >= firstDomainEnd -
                                          firstInteriorTolerance ||
                    secondParameter <= secondDomainStart +
                                           secondInteriorTolerance ||
                    secondParameter >= secondDomainEnd -
                                           secondInteriorTolerance ||
                    !isTransverseCrossing(first,
                                          firstParameter,
                                          second,
                                          secondParameter)) {
                    continue;
                }

                QPointF firstLocalPoint;
                QPointF secondLocalPoint;
                if (!evaluateNurbsPoint(first.curve,
                                        firstParameter,
                                        &firstLocalPoint) ||
                    !evaluateNurbsPoint(second.curve,
                                        secondParameter,
                                        &secondLocalPoint)) {
                    continue;
                }
                const Point3D firstWorldPoint =
                    workPlaneFramePointToWorld(firstLocalPoint,
                                               first.effectiveFrame);
                const Point3D secondWorldPoint =
                    workPlaneFramePointToWorld(secondLocalPoint,
                                               second.effectiveFrame);
                const Point3D worldPoint = scale(
                    add(firstWorldPoint, secondWorldPoint), 0.5);
                events.append({firstIndex,
                               secondIndex,
                               firstParameter,
                               secondParameter,
                               worldPoint,
                               -1});
            }
        }
    }

    if (events.isEmpty()) {
        plan->failureMessage = QStringLiteral(
            "Weld found no interior crossings between the selected planar curves");
        return true;
    }

    qreal worldScale = 1.0;
    for (const WeldIntersectionEvent &event : events) {
        worldScale = std::max(worldScale, coordinateScale(event.worldPoint));
    }
    const qreal nodeTolerance = std::max<qreal>(1.0e-7,
                                                 worldScale * 1.0e-10);
    QVector<WeldNode> nodes;
    for (WeldIntersectionEvent &event : events) {
        int matchingNode = -1;
        for (int nodeIndex = 0; nodeIndex < nodes.size(); ++nodeIndex) {
            if (squaredDistance(nodes[nodeIndex].worldPoint,
                                event.worldPoint) <=
                nodeTolerance * nodeTolerance) {
                matchingNode = nodeIndex;
                break;
            }
        }
        if (matchingNode < 0) {
            matchingNode = nodes.size();
            nodes.append({event.worldPoint, 1});
        } else {
            WeldNode &node = nodes[matchingNode];
            node.worldPoint = scale(
                add(scale(node.worldPoint, node.sampleCount), event.worldPoint),
                1.0 / (node.sampleCount + 1));
            ++node.sampleCount;
        }
        event.nodeIndex = matchingNode;
    }

    for (const WeldIntersectionEvent &event : events) {
        const Point3D nodePoint = nodes[event.nodeIndex].worldPoint;
        curves[event.firstCurveIndex].cuts.append(
            {event.firstParameter, nodePoint});
        curves[event.secondCurveIndex].cuts.append(
            {event.secondParameter, nodePoint});
    }

    for (int curveIndex = 0; curveIndex < curves.size(); ++curveIndex) {
        if (!curves[curveIndex].cuts.isEmpty()) {
            ++plan->splitCurveCount;
        }
    }
    plan->intersectionCount = nodes.size();

    quint64 nextWeldGroupId = 1;
    for (const SceneObject &object : document.objects()) {
        for (const quint64 group : object.geometry.controlPointWeldGroups) {
            if (group == std::numeric_limits<quint64>::max()) {
                plan->failureMessage = QStringLiteral(
                    "Weld could not allocate a control-point group identifier");
                return true;
            }
            nextWeldGroupId = std::max(nextWeldGroupId, group + 1);
        }
    }
    QVector<quint64> nodeWeldGroupIds;
    nodeWeldGroupIds.reserve(nodes.size());
    for (int nodeIndex = 0; nodeIndex < nodes.size(); ++nodeIndex) {
        if (nextWeldGroupId == 0 ||
            nextWeldGroupId == std::numeric_limits<quint64>::max()) {
            plan->failureMessage = QStringLiteral(
                "Weld could not allocate a control-point group identifier");
            return true;
        }
        nodeWeldGroupIds.append(nextWeldGroupId++);
    }

    for (int ownerIndex = 0; ownerIndex < owners.size(); ++ownerIndex) {
        const WeldOwner &owner = owners[ownerIndex];
        bool ownerChanged = false;
        QVector<Shape::NurbsCurve2D> outputComponents;
        QVector<WorkPlaneFrame> outputFrames;
        QVector<QVector<quint64>> outputComponentWeldGroups;
        for (const int recordIndex : owner.curveRecordIndices) {
            const WeldCurveRecord &record = curves[recordIndex];
            if (!record.cuts.isEmpty()) {
                ownerChanged = true;
            }
            QVector<Shape::NurbsCurve2D> pieces;
            if (record.cuts.isEmpty()) {
                pieces.append(record.curve);
            } else if (!splitCurveAtCuts(record, &pieces)) {
                plan->failureMessage = QStringLiteral(
                    "Weld could not exactly split one of the selected curves");
                plan->replacements.clear();
                return true;
            }
            for (const Shape::NurbsCurve2D &piece : pieces) {
                outputComponents.append(piece);
                outputFrames.append(record.frame);
                QVector<quint64> pieceWeldGroups(
                    piece.controlPoints.size(), 0);
                const Shape &sourceShape = owner.sourceShape;
                const auto sourceGroupAt = [&](int localControlPointIndex) {
                    const int sourceIndex =
                        record.firstControlPointIndex + localControlPointIndex;
                    return sourceIndex >= 0 &&
                            sourceIndex < sourceShape.controlPointWeldGroups.size()
                        ? sourceShape.controlPointWeldGroups[sourceIndex]
                        : quint64(0);
                };
                if (record.cuts.isEmpty()) {
                    for (int controlPoint = 0;
                         controlPoint < pieceWeldGroups.size(); ++controlPoint) {
                        pieceWeldGroups[controlPoint] =
                            sourceGroupAt(controlPoint);
                    }
                } else if (pieceWeldGroups.size() >= 2 &&
                           record.curve.controlPoints.size() >= 2) {
                    const auto preserveEndpointGroup =
                        [&](int pieceEndpoint, int sourceEndpoint) {
                            const quint64 group = sourceGroupAt(sourceEndpoint);
                            if (group == 0) {
                                return;
                            }
                            Point3D pieceWorld = workPlaneFramePointToWorld(
                                piece.controlPoints[pieceEndpoint], record.frame);
                            Point3D sourceWorld = workPlaneFramePointToWorld(
                                record.curve.controlPoints[sourceEndpoint],
                                record.frame);
                            pieceWorld = add(pieceWorld,
                                             record.placementTranslation);
                            sourceWorld = add(sourceWorld,
                                              record.placementTranslation);
                            if (squaredDistance(pieceWorld, sourceWorld) <=
                                nodeTolerance * nodeTolerance) {
                                pieceWeldGroups[pieceEndpoint] = group;
                            }
                        };
                    preserveEndpointGroup(0, 0);
                    preserveEndpointGroup(pieceWeldGroups.size() - 1,
                                          record.curve.controlPoints.size() - 1);
                }
                outputComponentWeldGroups.append(std::move(pieceWeldGroups));
            }
        }
        if (!ownerChanged || outputComponents.isEmpty()) {
            continue;
        }

        Shape weldedShape = owner.sourceShape;
        weldedShape.geometryType = GeometryType::PolyCurve;
        weldedShape.nurbs = Shape::NurbsCurve2D{};
        weldedShape.arcMode = ArcMode::TwoPoint;
        weldedShape.arcSweep = 0.0;
        weldedShape.subdivisionParameters.clear();
        weldedShape.components = outputComponents;
        weldedShape.componentWorkPlaneFrames = outputFrames;
        weldedShape.controlPointWeldGroups.clear();
        for (int componentIndex = 0;
             componentIndex < outputComponents.size(); ++componentIndex) {
            const Shape::NurbsCurve2D &component =
                outputComponents[componentIndex];
            const WorkPlaneFrame &frame = outputFrames[componentIndex];
            const SceneObject *ownerObject = document.object(owner.objectId);
            const Point3D placement = ownerObject != nullptr
                ? ownerObject->placementTranslation : Point3D{};
            const auto assignNodeGroup = [&](int localControlPointIndex) {
                if (localControlPointIndex < 0 ||
                    localControlPointIndex >= component.controlPoints.size()) {
                    return;
                }
                Point3D worldPoint = workPlaneFramePointToWorld(
                    component.controlPoints[localControlPointIndex], frame);
                worldPoint = add(worldPoint, placement);
                for (int nodeIndex = 0; nodeIndex < nodes.size(); ++nodeIndex) {
                    if (squaredDistance(worldPoint, nodes[nodeIndex].worldPoint) <=
                        nodeTolerance * nodeTolerance) {
                        outputComponentWeldGroups[componentIndex]
                            [localControlPointIndex] =
                                nodeWeldGroupIds[nodeIndex];
                        break;
                    }
                }
            };
            if (!component.controlPoints.isEmpty()) {
                assignNodeGroup(0);
                assignNodeGroup(component.controlPoints.size() - 1);
            }
            for (const quint64 group :
                 outputComponentWeldGroups[componentIndex]) {
                weldedShape.controlPointWeldGroups.append(group);
            }
        }
        weldedShape.points = polyCurvePoints(
            outputComponents, outputFrames, shapeWorkPlaneFrame(weldedShape));
        plan->replacements.append({owner.objectId, std::move(weldedShape)});
    }

    if (plan->replacements.isEmpty()) {
        plan->failureMessage = QStringLiteral(
            "Weld found no interior crossings between the selected planar curves");
    }
    return true;
}

bool applyWeldCommand(DocumentTransaction &transaction,
                      const WeldCommandPlan &plan)
{
    if (plan.replacements.isEmpty()) {
        return false;
    }
    for (const WeldCommandReplacement &replacement : plan.replacements) {
        if (!transaction.replaceGeometry(replacement.objectId,
                                         replacement.geometry)) {
            return false;
        }
    }
    return true;
}

} // namespace classiCAD
