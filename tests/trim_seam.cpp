#include <QApplication>
#include <QTemporaryDir>

#include "../src/app/session_serializer.h"
#include "../src/core/commands/explode_command.h"
#include "../src/core/commands/trim_erase_command.h"
#include "../src/core/geometry/curve_construction.h"
#include "../src/core/geometry/curve_editing.h"
#include "../src/core/geometry/arc_curve_factory.h"
#include "../src/core/geometry/curve_erase_intervals.h"
#include "../src/core/geometry/curve_evaluator.h"
#include "../src/core/geometry/curve_intersections.h"
#include "../src/core/geometry/curve_join.h"
#include "../src/core/geometry/geometry_transform.h"
#include "../src/core/geometry/shape_mapping.h"
#include "../src/core/history/document_transaction.h"
#include "../src/core/history/history.h"
#include "../src/core/serialization/shape_json_codec.h"
#include "../src/services/erase/curve_erase_query.h"
#include "../src/services/erase/trim_erase_query.h"
#include "../src/services/hit_testing/curve_hit_tester.h"
#include "../src/services/hit_testing/selection_box_query.h"
#include "../src/services/sampling/curve_sampler.h"
#include "../src/services/snapping/snap_engine.h"
#include "../src/services/viewport/viewport_transform.h"
#include "../src/tools/erase_tool.h"
#include "../src/tools/join_tool.h"
#include "../src/tools/mirror_tool.h"
#include "../src/tools/rotate_tool.h"
#include "../src/tools/scale_tool.h"
#include "../src/tools/select_tool.h"
#include "../src/tools/tool_context.h"
#include "../src/tools/tool_input.h"
#include "../src/tools/trim_tool.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace classiCAD;

namespace {

constexpr QSize kViewportSize{640, 480};

struct ToolHarness {
    Document document;
    SelectionModel selection;
    History history{document};
    ViewportTransform viewportTransform;
    CurveSampler curveSampler;
    CurveHitTester curveHitTester;
    SnapEngine snapEngine;
    ToolContext context{document,
                        selection,
                        history,
                        viewportTransform,
                        curveSampler,
                        curveHitTester,
                        snapEngine};
    JoinTool joinTool;
};

QVector<ObjectId> appendShapes(Document &document,
                               const QVector<Shape> &shapes)
{
    QVector<ObjectId> objectIds;
    objectIds.reserve(shapes.size());
    for (const Shape &shape : shapes) {
        objectIds.append(document.append(shape));
    }
    return objectIds;
}

Shape lineShape(const QVector<QPointF> &points,
                const WorkPlaneFrame &frame = makeWorkPlaneFrame(WorkPlane::XY))
{
    Shape shape;
    shape.geometryType = GeometryType::Line;
    shape.workPlaneFrame = frame;
    shape.nurbs = makeDegreeOneNurbs(points);
    shape.points = points;
    return shape;
}

Shape circleShape(const QPointF &center, qreal radius, qreal seamAngle = 0.0)
{
    const QPointF edge(center.x() + radius * std::cos(seamAngle),
                       center.y() + radius * std::sin(seamAngle));
    Shape shape;
    shape.geometryType = GeometryType::Circle;
    shape.workPlaneFrame = makeWorkPlaneFrame(WorkPlane::XY);
    shape.points = {center, edge};
    shape.nurbs = makeCircleNurbs({center, edge});
    return shape;
}

EraseIntersectionParameterResult resolveEraseIntersections(
    const Document &document,
    ObjectId sourceObjectId,
    int sourceComponentIndex,
    const NurbsCurve2D &sourceCurve,
    const WorkPlaneFrame &sourceFrame,
    qreal endpointProximityTolerance = 0.0)
{
    QVector<EraseCurveIntersectionCandidate> curveCandidates;
    QVector<ErasePointIntersectionCandidate> pointCandidates;
    for (int objectIndex = 0; objectIndex < document.size(); ++objectIndex) {
        const ObjectId objectId = document.objectIdAt(objectIndex);
        const Shape &shape = document[objectIndex];
        for (const ShapeNurbsCurveComponent &component :
             nurbsCurveComponentsForShape(shape)) {
            curveCandidates.append({objectId,
                                    component.componentIndex,
                                    component.curve,
                                    component.workPlaneFrame});
        }
        if (shape.geometryType == GeometryType::Point) {
            const WorkPlaneFrame pointFrame = shapeWorkPlaneFrame(shape);
            for (const QPointF &point : shape.points) {
                pointCandidates.append(
                    {objectId, workPlaneFramePointToWorld(point, pointFrame)});
            }
        }
    }
    return findEraseIntersectionParameters(sourceCurve,
                                           sourceFrame,
                                           sourceObjectId,
                                           sourceComponentIndex,
                                           curveCandidates,
                                           pointCandidates,
                                           endpointProximityTolerance);
}

bool calculateTrimErase(const Document &document,
                        int sourceShapeIndex,
                        const QVector<QPointF> &screenStroke,
                        const ViewportTransform &transform,
                        const QSize &viewportSize,
                        bool wholeObjectOnIntersectionFreeErase,
                        int onlyComponentIndex,
                        const QRectF *trimBox,
                        TrimEraseShapeQueryResult *result)
{
    if (sourceShapeIndex < 0 || sourceShapeIndex >= document.size()) {
        return false;
    }
    const ObjectId sourceObjectId = document.objectIdAt(sourceShapeIndex);
    CurveSampler sampler;
    const EraseIntersectionResolver resolver =
        [&document, sourceObjectId](int componentIndex,
                                    const NurbsCurve2D &curve,
                                    const WorkPlaneFrame &frame) {
            return resolveEraseIntersections(document,
                                             sourceObjectId,
                                             componentIndex,
                                             curve,
                                             frame);
        };
    const qreal endpointTolerance = std::max<qreal>(
        1.0e-5,
        3.0 / std::max(transform.viewScalePixelsPerWorldUnit(viewportSize),
                       1.0e-9));
    return calculateTrimEraseReplacement(
        document[sourceShapeIndex],
        sourceShapeIndex,
        screenStroke,
        trimBox,
        wholeObjectOnIntersectionFreeErase,
        onlyComponentIndex,
        nullptr,
        endpointTolerance,
        sampler,
        transform,
        viewportSize,
        resolver,
        result);
}

bool runJoinTopologyCases()
{
    constexpr qreal tolerance = 1.0e-5;
    for (const bool reversed : {false, true}) {
        ToolHarness harness;
        Shape bent = lineShape({{0, 0}, {100, 0}, {100, -10}});
        bent.geometryType = GeometryType::Nurbs;
        Shape firstOverlap = lineShape({{-60, -5}, {30, -5}},
                                       makeWorkPlaneFrame(WorkPlane::XY));
        firstOverlap.workPlaneFrame.origin = {10, 5, 0};
        Shape secondOverlap = lineShape({{20, 0}, {80, 0}});
        secondOverlap.geometryType = GeometryType::Line;
        if (reversed) {
            NurbsCurve2D reversedFirst;
            NurbsCurve2D reversedSecond;
            reverseNurbsCurve(firstOverlap.nurbs, &reversedFirst);
            reverseNurbsCurve(secondOverlap.nurbs, &reversedSecond);
            firstOverlap.nurbs = reversedFirst;
            secondOverlap.nurbs = reversedSecond;
        }
        const QVector<ObjectId> sources = appendShapes(
            harness.document, {bent, firstOverlap, secondOverlap});
        harness.selection.setObjectIds(sources, sources.last());
        harness.joinTool.begin(sources);
        const JoinExecutionResult join =
            harness.joinTool.executeJoin(tolerance, harness.context);
        const Shape *joined = harness.document.shape(join.joinedObjectId);
        bool valid = join.committed && !harness.joinTool.isActive() &&
                     harness.document.size() == 1 && joined != nullptr &&
                     joined->geometryType == GeometryType::PolyCurve &&
                     joined->components.size() == 2;
        qreal length = 0.0;
        if (valid) {
            for (const NurbsCurve2D &component : joined->components) {
                QPointF start;
                QPointF end;
                valid = valid && validateNurbsCurve(component) &&
                        nurbsCurveEndpoints(component, &start, &end);
                length += std::hypot(end.x() - start.x(), end.y() - start.y());
            }
            valid = valid && std::abs(length - 160.0) < 1.0e-7 &&
                    harness.selection.objectIds() ==
                        QVector<ObjectId>{join.joinedObjectId};
        }
        if (!valid) {
            qWarning() << "Join must fuse overlapping spans of a bent spline" << reversed;
            return false;
        }
    }

    for (const bool reversed : {false, true}) {
        ToolHarness harness;
        Shape bent = lineShape({{0, 0}, {100, 0}, {100, 30}});
        bent.geometryType = GeometryType::Nurbs;
        Shape overlap = lineShape({{20, 0}, {100, 0}},
                                  makeWorkPlaneFrame(WorkPlane::XZ));
        if (reversed) {
            NurbsCurve2D reversedCurve;
            reverseNurbsCurve(overlap.nurbs, &reversedCurve);
            overlap.nurbs = reversedCurve;
        }
        const QVector<ObjectId> sources = appendShapes(harness.document,
                                                       {bent, overlap});
        harness.selection.setObjectIds(sources, sources.last());
        harness.joinTool.begin(sources);
        const JoinExecutionResult join =
            harness.joinTool.executeJoin(tolerance, harness.context);
        const Shape *joined = harness.document.shape(join.joinedObjectId);
        bool valid = join.committed && joined != nullptr &&
                     harness.document.size() == 1 &&
                     joined->components.size() == 2 &&
                     connectedNurbsCurvesAreContinuousInWorld(
                         joined->components,
                         joined->componentWorkPlaneFrames,
                         tolerance);
        qreal length = 0.0;
        if (valid) {
            for (const NurbsCurve2D &curve : joined->components) {
                QPointF start;
                QPointF end;
                valid = valid && validateNurbsCurve(curve) &&
                        nurbsCurveEndpoints(curve, &start, &end);
                length += std::hypot(end.x() - start.x(), end.y() - start.y());
            }
            valid = valid && std::abs(length - 130.0) < 1.0e-7;
        }
        if (!valid) {
            qWarning() << "Join must fuse world-space overlapping legs across planes" << reversed;
            return false;
        }
    }

    ToolHarness harness;
    Shape vertical = lineShape({{50, 0}, {50, 40}},
                               makeWorkPlaneFrame(WorkPlane::XZ));
    Shape horizontal = lineShape({{0, 0}, {50, 0}});
    const QVector<ObjectId> sources = appendShapes(harness.document,
                                                    {vertical, horizontal});
    harness.selection.setObjectIds(sources, sources.last());
    harness.joinTool.begin(sources);
    const JoinExecutionResult join =
        harness.joinTool.executeJoin(tolerance, harness.context);
    const Shape *joined = harness.document.shape(join.joinedObjectId);
    QString failedCheck;
    bool valid = join.committed && !harness.joinTool.isActive() &&
                 harness.document.size() == 1 && joined != nullptr &&
                 joined->geometryType == GeometryType::PolyCurve &&
                 joined->components.size() == 2 &&
                 joined->componentWorkPlaneFrames.size() == 2;
    if (!valid) {
        failedCheck = QStringLiteral("join result");
    }
    for (int index = 0; valid && index < joined->components.size(); ++index) {
        QPointF start;
        QPointF end;
        valid = nurbsCurveEndpoints(joined->components[index], &start, &end) &&
                isValidWorkPlaneFrame(joined->componentWorkPlaneFrames[index]);
    }
    if (valid) {
        QPointF firstEnd;
        QPointF secondStart;
        nurbsCurveEndpoints(joined->components[0], nullptr, &firstEnd);
        nurbsCurveEndpoints(joined->components[1], &secondStart, nullptr);
        const Point3D firstWorld = shapeComponentPointToWorld(*joined, 0, firstEnd);
        const Point3D secondWorld = shapeComponentPointToWorld(*joined, 1, secondStart);
        valid = std::hypot(std::hypot(firstWorld.x - secondWorld.x,
                                      firstWorld.y - secondWorld.y),
                           firstWorld.z - secondWorld.z) < 1.0e-7;
        if (!valid) {
            failedCheck = QStringLiteral("component continuity");
        }
    }
    if (valid) {
        ViewportTransform transform = harness.viewportTransform;
        transform.setWorkPlaneFrame(shapeWorkPlaneFrame(*joined));
        const QPointF midpoint =
            (joined->components[0].controlPoints.first() +
             joined->components[0].controlPoints.last()) * 0.5;
        const Point3D midpointWorld = shapeComponentPointToWorld(*joined, 0, midpoint);
        QPointF midpointScreen;
        valid = transform.worldPointToScreen(midpointWorld, kViewportSize,
                                             &midpointScreen) &&
                harness.curveHitTester.distanceToShape(
                    midpointScreen, *joined, transform, kViewportSize) < 0.5;
        if (!valid) {
            failedCheck = QStringLiteral("world-space hit testing");
        }
    }
    Shape roundTripped;
    if (valid) {
        valid = shapeFromJson(shapeToJson(*joined), &roundTripped) &&
                roundTripped.componentWorkPlaneFrames.size() == 2;
        if (!valid) {
            failedCheck = QStringLiteral("JSON round trip");
        }
    }
    if (valid) {
        Shape moved = *joined;
        valid = translateShapeGeometry(&moved, {3.0, 4.0},
                                       makeWorkPlaneFrame(WorkPlane::XY));
        for (int index = 0; valid && index < moved.components.size(); ++index) {
            const QPointF local = joined->components[index].controlPoints.first();
            const Point3D before = shapeComponentPointToWorld(*joined, index, local);
            const Point3D after = shapeComponentPointToWorld(moved, index, local);
            valid = std::abs(after.x - before.x - 3.0) < 1.0e-7 &&
                    std::abs(after.y - before.y - 4.0) < 1.0e-7 &&
                    std::abs(after.z - before.z) < 1.0e-7;
            if (!valid) {
                failedCheck = QStringLiteral("translation");
            }
        }
    }
    if (!valid) {
        qWarning() << "Join must preserve connected curves on different workplanes"
                   << failedCheck << static_cast<int>(join.failure)
                   << join.committed << join.componentCount
                   << (joined == nullptr ? -1 : joined->components.size());
    }
    return valid;
}

bool runSelectionBoxCases()
{
    int failures = 0;
    Document document;
    SelectionModel selection;
    History history(document);
    ViewportTransform transform;
    CurveSampler sampler;
    CurveHitTester hitTester;
    SnapEngine snapEngine;
    ToolContext context(document, selection, history, transform, sampler,
                        hitTester, snapEngine);
    SelectTool selectTool;
    for (const bool perspective : {false, true}) {
        transform.setViewPreset(ViewportViewPreset::Top);
        transform.setPerspectiveEnabled(perspective);
        Shape distant = lineShape({{-10, 0}, {10, 0}});
        distant.workPlaneFrame.origin.z = 1.0e9;
        const QRectF box(280, 200, 80, 80);
        for (const bool crossing : {false, true}) {
            const auto query = queryCurveOrPointSelectionBox(
                distant, box, crossing, sampler, transform, kViewportSize);
            if (!query.applies || query.matches) {
                qWarning() << "Box must reject camera-clipped geometry"
                           << perspective << crossing;
                ++failures;
            }
        }
        Shape joined;
        joined.geometryType = GeometryType::PolyCurve;
        joined.components = {distant.nurbs};
        joined.componentWorkPlaneFrames = {distant.workPlaneFrame};
        const auto joinedQuery = queryCurveOrPointSelectionBox(
            joined, box, true, sampler, transform, kViewportSize);
        if (!joinedQuery.applies || joinedQuery.matches) {
            qWarning() << "Box must use each component's actual plane" << perspective;
            ++failures;
        }
        distant.workPlaneFrame.origin.z = 0.0;
        const ObjectId visibleId = document.append(distant);
        const ObjectId clippedId = document.append(joined);
        QVector<ObjectId> candidates;
        const auto visibleQuery = queryCurveOrPointSelectionBox(
            distant, box, true, sampler, transform, kViewportSize);
        const auto clippedQuery = queryCurveOrPointSelectionBox(
            joined, box, true, sampler, transform, kViewportSize);
        if (visibleQuery.applies && visibleQuery.matches) candidates.append(visibleId);
        if (clippedQuery.applies && clippedQuery.matches) candidates.append(clippedId);
        selectTool.beginSelectionBox(box.bottomRight(), false);
        selectTool.updateSelectionBox(box.topLeft());
        selectTool.finishBoxSelection(candidates, context);
        if (selection.objectIds() != QVector<ObjectId>{visibleId}) {
            qWarning() << "Crossing box must select only its visible curve" << perspective;
            ++failures;
        }
    }
    return failures == 0;
}

bool runEraseGeometryCases()
{
    int failures = 0;
    ViewportTransform transform;

    // Rotating a circle's storage seam must not change which half is retained.
    for (int angle = 0; angle < 360; angle += 15) {
        const qreal radians = angle * 3.14159265358979323846 / 180.0;
        ToolHarness harness;
        const Shape source = circleShape({0, 0}, 100.0, radians);
        const Shape cutter = lineShape({{-150, 0}, {150, 0}});
        appendShapes(harness.document, {source, cutter});
        const QPointF stroke = transform.workPlaneToScreen(
            {0, -100}, kViewportSize, shapeWorkPlaneFrame(source));
        TrimEraseShapeQueryResult result;
        const bool changed = calculateTrimErase(
            harness.document, 0, {stroke}, transform, kViewportSize,
            false, -1, nullptr, &result) && result.changed;
        if (!changed || result.replacementShapes.size() != 1 ||
            result.replacementShapes.first().components.isEmpty()) {
            qWarning() << "Circle seam erase must produce retained NURBS geometry" << angle;
            ++failures;
            continue;
        }
        for (const NurbsCurve2D &curve : result.replacementShapes.first().components) {
            const QVector<double> knots = expandedNurbsKnotVector(curve);
            for (int sample = 0; sample <= 100; ++sample) {
                QPointF point;
                const qreal parameter = knots[curve.degree] +
                    (knots[curve.controlPoints.size()] - knots[curve.degree]) *
                    sample / 100.0;
                if (!evaluateNurbsPoint(curve, parameter, &point) ||
                    point.y() < -0.1 ||
                    std::abs(std::hypot(point.x(), point.y()) - 100.0) > 1.0e-6) {
                    qWarning() << "Bad retained circle geometry at seam angle"
                               << angle << point;
                    ++failures;
                    break;
                }
            }
        }
    }

    ToolHarness unboundedEraseHarness;
    const Shape unboundedLine = lineShape({{-20, 0}, {20, 0}});
    unboundedEraseHarness.document.append(unboundedLine);
    const QPointF unboundedStroke = transform.workPlaneToScreen(
        {0, 0}, kViewportSize, shapeWorkPlaneFrame(unboundedLine));
    TrimEraseShapeQueryResult unboundedErase;
    if (!calculateTrimErase(unboundedEraseHarness.document, 0,
                            {unboundedStroke}, transform, kViewportSize,
                            true, -1, nullptr, &unboundedErase) ||
        !unboundedErase.changed || !unboundedErase.erasedWholeObject ||
        !unboundedErase.replacementShapes.isEmpty()) {
        qWarning() << "Intersection-free Erase must remove the complete target object";
        ++failures;
    }

    // Tangent-only contacts bound the erase interval just like crossings.
    ToolHarness tangentHarness;
    const QVector<Shape> tangentCircles{
        circleShape({0, 0}, 60.0, 0.31),
        circleShape({-120, 0}, 60.0, 0.83),
        circleShape({120, 0}, 60.0, 1.27)};
    appendShapes(tangentHarness.document, tangentCircles);
    const ObjectId tangentId = tangentHarness.document.objectIdAt(0);
    const EraseIntersectionParameterResult contacts = resolveEraseIntersections(
        tangentHarness.document, tangentId, 0,
        tangentHarness.document[0].nurbs,
        shapeWorkPlaneFrame(tangentHarness.document[0]));
    QVector<QPointF> contactPoints;
    for (const qreal parameter : contacts.parameters) {
        QPointF point;
        if (evaluateNurbsPoint(tangentHarness.document[0].nurbs, parameter, &point)) {
            contactPoints.append(point);
        }
    }
    std::sort(contactPoints.begin(), contactPoints.end(),
              [](const QPointF &first, const QPointF &second) {
                  return first.x() < second.x();
              });
    if (contactPoints.size() != 2 ||
        std::abs(contactPoints[0].x() + 60.0) > 0.1 ||
        std::abs(contactPoints[0].y()) > 0.1 ||
        std::abs(contactPoints[1].x() - 60.0) > 0.1 ||
        std::abs(contactPoints[1].y()) > 0.1) {
        qWarning() << "Erase must recognize both tangent-only circle contacts"
                   << contactPoints;
        ++failures;
    }
    const QPointF upperStroke = transform.workPlaneToScreen(
        {0, 60}, kViewportSize, shapeWorkPlaneFrame(tangentHarness.document[0]));
    TrimEraseShapeQueryResult tangentRemainder;
    if (!calculateTrimErase(tangentHarness.document, 0, {upperStroke}, transform,
                            kViewportSize, false, -1, nullptr,
                            &tangentRemainder) ||
        !tangentRemainder.changed || tangentRemainder.replacementShapes.size() != 1 ||
        tangentRemainder.replacementShapes.first().components.isEmpty()) {
        qWarning() << "Tangency-bounded erase must retain the rest of the circle";
        ++failures;
    } else {
        const NurbsCurve2D &lower =
            tangentRemainder.replacementShapes.first().components.first();
        const QVector<double> knots = expandedNurbsKnotVector(lower);
        for (int sample = 0; sample <= 100; ++sample) {
            QPointF point;
            const qreal parameter = knots[lower.degree] +
                (knots[lower.controlPoints.size()] - knots[lower.degree]) *
                sample / 100.0;
            if (!evaluateNurbsPoint(lower, parameter, &point) || point.y() > 0.1 ||
                std::abs(std::hypot(point.x(), point.y()) - 60.0) > 0.1) {
                qWarning() << "Tangency-bounded erase must keep the lower arc";
                ++failures;
                break;
            }
        }
    }

    // A screen hit in the middle of an intersection-bounded line section
    // removes the entire section and preserves two independently editable tails.
    ToolHarness crossingHarness;
    const Shape circle = circleShape({0, 0}, 50.0);
    const Shape sourceLine = lineShape({{-100, 0}, {100, 0}});
    appendShapes(crossingHarness.document, {circle, sourceLine});
    const QPointF center = transform.workPlaneToScreen(
        {0, 0}, kViewportSize, shapeWorkPlaneFrame(sourceLine));
    const auto hitIntervals = nurbsEraseIntervalsForStroke(
        sourceLine.nurbs, shapeWorkPlaneFrame(sourceLine), {center},
        transform, kViewportSize);
    const EraseIntersectionParameterResult lineContacts = resolveEraseIntersections(
        crossingHarness.document, crossingHarness.document.objectIdAt(1), 0,
        sourceLine.nurbs, shapeWorkPlaneFrame(sourceLine));
    const QVector<ParameterInterval> bounded = boundCurveEraseIntervals(
        sourceLine.nurbs, hitIntervals, lineContacts.parameters);
    if (lineContacts.parameters.size() != 2 || bounded.size() != 1 ||
        std::abs(bounded.first().start - 0.25) > 1.0e-6 ||
        std::abs(bounded.first().end - 0.75) > 1.0e-6) {
        qWarning() << "Erase stroke must expand to its intersection-bounded line section"
                   << lineContacts.parameters.size() << bounded.size();
        ++failures;
    }
    TrimEraseShapeQueryResult linePieces;
    if (!calculateTrimErase(crossingHarness.document, 1, {center}, transform,
                            kViewportSize, false, -1, nullptr, &linePieces) ||
        !linePieces.changed || linePieces.replacementShapes.size() != 2 ||
        !validateNurbsCurve(linePieces.replacementShapes.value(0).nurbs) ||
        !validateNurbsCurve(linePieces.replacementShapes.value(1).nurbs)) {
        qWarning() << "Middle cut must produce two valid, separate line tails";
        ++failures;
    } else {
        ToolHarness commandHarness;
        const QVector<ObjectId> sourceIds = appendShapes(
            commandHarness.document, {circle, sourceLine});
        const LayerId sourceLayer =
            commandHarness.document.object(sourceIds[1])->layerId;
        DocumentTransaction transaction(commandHarness.document,
                                        commandHarness.history);
        TrimEraseCommandResult commandResult;
        const bool applied = TrimEraseCommand::apply(
            commandHarness.document,
            transaction,
            {{sourceIds[1], 1, linePieces.replacementShapes}},
            &commandResult) && transaction.commit();
        const bool retainedIdentity =
            commandHarness.document.size() == 3 &&
            commandHarness.document.objectIdAt(1) == sourceIds[1] &&
            commandHarness.document.objectIdAt(2) != sourceIds[1] &&
            commandHarness.document.object(commandHarness.document.objectIdAt(2))->layerId ==
                sourceLayer;
        if (!applied || !retainedIdentity || commandResult.generatedPieceCount != 2 ||
            !commandHarness.history.undo() || commandHarness.document.size() != 2 ||
            commandHarness.document.objectIdAt(1) != sourceIds[1]) {
            qWarning() << "Trim command must preserve IDs/layers and undo atomically";
            ++failures;
        }
    }

    // Preserve the small-gap endpoint refinement case: an already trimmed
    // arc can sit a fraction of a world unit off its former intersection.
    const auto worldAtScreen = [&transform](qreal x, qreal y) {
        return transform.screenToWorld({x, y}, kViewportSize);
    };
    Shape arc;
    arc.geometryType = GeometryType::Arc;
    arc.points = {worldAtScreen(249, 169), worldAtScreen(391, 311),
                  worldAtScreen(391, 169)};
    CircularArc2D circularArc;
    if (makeCircularArcThroughPoint(arc.points[0], arc.points[1], arc.points[2],
                                    &circularArc)) {
        arc.nurbs = circularArc.curve;
    }
    const Shape diagonalLine = lineShape(
        {worldAtScreen(178, 98), worldAtScreen(462, 382)});
    ToolHarness endpointHarness;
    appendShapes(endpointHarness.document, {arc, diagonalLine});
    const QVector<QPointF> middleStroke{{300, 220}, {340, 260}};
    for (const auto &stroke : {middleStroke,
                               QVector<QPointF>{{255, 175}, {300, 220}}}) {
        const EraseIntersectionParameterResult intersections =
            resolveEraseIntersections(endpointHarness.document,
                                      endpointHarness.document.objectIdAt(1), 0,
                                      diagonalLine.nurbs,
                                      shapeWorkPlaneFrame(diagonalLine));
        const QVector<ParameterInterval> hits = nurbsEraseIntervalsForStroke(
            diagonalLine.nurbs, shapeWorkPlaneFrame(diagonalLine), stroke,
            transform, kViewportSize);
        const QVector<ParameterInterval> intervals = boundCurveEraseIntervals(
            diagonalLine.nurbs, hits, intersections.parameters);
        if (intervals.size() != 1 ||
            std::abs(intervals.first().start - 0.25) > 1.0e-6 ||
            std::abs(intervals.first().end - 0.75) > 1.0e-6) {
            qWarning() << "Near-intersection eraser hit must remain in the intended arc-bounded section";
            ++failures;
        }
    }
    endpointHarness.document.mutateGeometry(
        endpointHarness.document.objectIdAt(0),
        [](Shape &shape) {
            for (QPointF &control : shape.nurbs.controlPoints) {
                control += QPointF(0.02, 0.02);
            }
            return true;
        });
    const EraseIntersectionParameterResult refinedIntersections =
        resolveEraseIntersections(endpointHarness.document,
                                  endpointHarness.document.objectIdAt(1), 0,
                                  diagonalLine.nurbs,
                                  shapeWorkPlaneFrame(diagonalLine),
                                  3.0 / std::max<qreal>(
                                      transform.viewScalePixelsPerWorldUnit(
                                          kViewportSize),
                                      1.0e-9));
    const QVector<ParameterInterval> refinedIntervals = boundCurveEraseIntervals(
        diagonalLine.nurbs,
        nurbsEraseIntervalsForStroke(diagonalLine.nurbs,
                                     shapeWorkPlaneFrame(diagonalLine),
                                     middleStroke, transform, kViewportSize),
        refinedIntersections.parameters);
    if (refinedIntervals.size() != 1 ||
        std::abs(refinedIntervals.first().start - 0.25) > 1.0e-6 ||
        std::abs(refinedIntervals.first().end - 0.75) > 1.0e-6) {
        qWarning() << "Closest-point refinement must retain erase bounds after a small endpoint gap"
                   << refinedIntersections.parameters
                   << refinedIntervals.size()
                   << (refinedIntervals.isEmpty()
                           ? -1.0
                           : refinedIntervals.first().start)
                   << (refinedIntervals.isEmpty()
                           ? -1.0
                           : refinedIntervals.first().end);
        ++failures;
    }

    return failures == 0;
}

bool runPolyCurveAndToolCases()
{
    int failures = 0;
    ToolHarness harness;
    const NurbsCurve2D first = makeDegreeOneNurbs({{-100, 0}, {100, 0}});
    const NurbsCurve2D second = makeDegreeOneNurbs({{-100, 100}, {100, 100}});
    Shape joined;
    joined.geometryType = GeometryType::PolyCurve;
    joined.workPlaneFrame = makeWorkPlaneFrame(WorkPlane::XY);
    joined.points = {{-100, 0}, {100, 0}, {100, 100}};
    joined.components = {first, second};
    joined.componentWorkPlaneFrames = {joined.workPlaneFrame,
                                       joined.workPlaneFrame};
    const ObjectId joinedId = harness.document.append(joined);
    const QPointF hover = harness.viewportTransform.workPlaneToScreen(
        {0, 0}, kViewportSize, joined.workPlaneFrame);
    TrimTool trimTool;
    const TrimTool::HoverTarget target = trimTool.updateHover(
        hover, {joinedId}, ObjectId::invalid(), 10.0,
        [&](ObjectId objectId, int *componentIndex) {
            const Shape *shape = harness.document.shape(objectId);
            qreal closest = std::numeric_limits<qreal>::infinity();
            for (const ShapeNurbsCurveComponent &component :
                 nurbsCurveComponentsForShape(*shape)) {
                const qreal distance = distanceToNurbsCurveOnScreen(
                    component.curve, component.workPlaneFrame, hover,
                    harness.viewportTransform, kViewportSize);
                if (distance < closest) {
                    closest = distance;
                    *componentIndex = component.componentIndex;
                }
            }
            return closest;
        });
    if (!target.isValid() || target.componentIndex != 0 ||
        trimTool.candidateObjectIds() != QVector<ObjectId>{joinedId}) {
        qWarning() << "Trim hover must select the nearest PolyCurve component";
        ++failures;
    }
    TrimEraseShapeQueryResult componentResult;
    if (!calculateTrimErase(harness.document, 0, {hover},
                            harness.viewportTransform, kViewportSize,
                            false, target.componentIndex, nullptr,
                            &componentResult) || !componentResult.changed) {
        qWarning() << "Trim must calculate an edit for only the hovered component";
        ++failures;
    } else {
        const bool untouchedComponentRemains = std::any_of(
            componentResult.replacementShapes.cbegin(),
            componentResult.replacementShapes.cend(),
            [&](const Shape &shape) {
                return std::any_of(shape.components.cbegin(), shape.components.cend(),
                    [&](const NurbsCurve2D &curve) {
                        return curve.controlPoints == second.controlPoints;
                    });
            });
        const bool otherWasNotHit = componentResult.components.size() == 2 &&
            componentResult.components[1].removedIntervals.isEmpty();
        if (!untouchedComponentRemains || !otherWasNotHit) {
            qWarning() << "Point Trim must leave other PolyCurve components untouched";
            ++failures;
        }
    }

    // Navigation changes the screen query while the committed curve remains
    // in its original tilted workplane.
    for (int navigation = 0; navigation < 8; ++navigation) {
        ToolHarness navigationHarness;
        navigationHarness.viewportTransform.setViewPreset(ViewportViewPreset::Top);
        navigationHarness.viewportTransform.setPerspectiveEnabled(true);
        navigationHarness.viewportTransform.setWorkPlaneFrame(
            makeWorkPlaneFrameFromNormal({15, -10, 5}, {0.1, 0.2, 1.0}));
        const WorkPlaneFrame frame = navigationHarness.viewportTransform.workPlaneFrame();
        const Shape source = lineShape({{-80, 0}, {80, 0}}, frame);
        const Shape cutter = lineShape({{0, -80}, {0, 80}}, frame);
        appendShapes(navigationHarness.document, {source, cutter});
        QSize size = kViewportSize;
        switch (navigation) {
        case 0: navigationHarness.viewportTransform.orbitByPixels({45, -20}); break;
        case 1: navigationHarness.viewportTransform.zoomAt({330, 245}, 1.7, size); break;
        case 2: navigationHarness.viewportTransform.panByPixels({35, -20}, size); break;
        case 3: navigationHarness.viewportTransform.setPerspectiveEnabled(false); break;
        case 4: size = QSize(800, 600); break;
        case 5: {
            auto preferences = navigationHarness.viewportTransform.cameraPreferences();
            preferences.focalLengthMillimeters = 65;
            navigationHarness.viewportTransform.setCameraPreferences(preferences);
            break;
        }
        case 6: {
            auto preferences = navigationHarness.viewportTransform.cameraPreferences();
            preferences.clipEnd = 2000;
            navigationHarness.viewportTransform.setCameraPreferences(preferences);
            break;
        }
        case 7:
            navigationHarness.viewportTransform.setViewPreset(
                ViewportViewPreset::Isometric);
            break;
        }
        const QPointF hoverPoint = navigationHarness.viewportTransform.workPlaneToScreen(
            {-40, 0}, size, frame);
        TrimEraseShapeQueryResult result;
        const bool changed = calculateTrimErase(
            navigationHarness.document, 0, {hoverPoint},
            navigationHarness.viewportTransform, size, false, -1, nullptr, &result) &&
            result.changed;
        bool retainedRightSide = changed && result.replacementShapes.size() == 1;
        if (retainedRightSide) {
            const auto components = nurbsCurveComponentsForShape(
                result.replacementShapes.first());
            retainedRightSide = components.size() == 1;
            QPointF start;
            QPointF end;
            if (retainedRightSide) {
                retainedRightSide = nurbsCurveEndpoints(components.first().curve,
                                                        &start, &end) &&
                    std::abs(start.x()) < 1.0e-4 &&
                    std::abs(end.x() - 80.0) < 1.0e-4 &&
                    workPlaneFramesMatch(components.first().workPlaneFrame, frame);
            }
        }
        if (!retainedRightSide) {
            qWarning() << "Trim query must follow navigation and preserve its source frame"
                       << navigation;
            ++failures;
        }
    }

    // EraseTool owns the stroke path and stable candidate IDs.
    EraseTool eraseTool;
    eraseTool.beginStroke({10, 10});
    eraseTool.appendStrokeScreenPosition({20, 10});
    const ObjectId candidateId = harness.document.append(lineShape({{0, 0}, {1, 0}}));
    const int candidatesAdded = eraseTool.collectCandidatesAlongSegment(
        {10, 10}, {20, 10}, {candidateId},
        [](const QPointF &, ObjectId) { return 0.0; });
    eraseTool.finishStroke();
    if (candidatesAdded != 1 || eraseTool.strokeActive() ||
        eraseTool.screenPath().size() != 2 ||
        eraseTool.candidateObjectIds() != QVector<ObjectId>{candidateId}) {
        qWarning() << "EraseTool must retain the completed stroke and stable target ID";
        ++failures;
    }
    return failures == 0;
}

bool runCommandAndTransformCases()
{
    int failures = 0;
    ToolHarness explodeHarness;
    Shape polyCurve;
    polyCurve.geometryType = GeometryType::PolyCurve;
    polyCurve.workPlaneFrame = makeWorkPlaneFrame(WorkPlane::XY);
    polyCurve.components = {
        makeDegreeOneNurbs({{0, 0}, {1, 0}}),
        makeDegreeOneNurbs({{0, 1}, {1, 1}})};
    polyCurve.points = {{0, 0}, {1, 0}, {1, 1}};
    const ObjectId polyCurveId = explodeHarness.document.append(polyCurve);
    ExplodeCommandPlan explodePlan;
    bool valid = buildExplodeCommandPlan(explodeHarness.document,
                                         {polyCurveId}, &explodePlan) &&
                 explodePlan.outputComponentCount == 2 &&
                 explodePlan.selectedObjectIndices == QVector<int>{0, 1};
    DocumentTransaction explodeTransaction(explodeHarness.document,
                                           explodeHarness.history);
    valid = valid && applyExplodeCommand(explodeTransaction, explodePlan) &&
            explodeTransaction.commit() && explodeHarness.document.size() == 2;
    if (!valid) {
        qWarning() << "Explode command must create separate selected components";
        ++failures;
    }

    ToolHarness rotateHarness;
    const QVector<ObjectId> rotateIds = appendShapes(
        rotateHarness.document,
        {lineShape({{1, 0}, {2, 0}}), lineShape({{0, 1}, {0, 2}})});
    RotateTool rotate;
    rotate.beginSelection(rotateIds, true);
    ToolInput input;
    input.workPlaneFrame = makeWorkPlaneFrame(WorkPlane::XY);
    input.worldPosition = {0, 0};
    const auto pivot = rotate.acceptPoint(input, 15.0, 6.0, rotateHarness.context);
    input.worldPosition = {1, 0};
    const auto reference = rotate.acceptPoint(input, 15.0, 6.0, rotateHarness.context);
    input.worldPosition = {0, 1};
    const auto finalPoint = rotate.acceptPoint(input, 15.0, 6.0, rotateHarness.context);
    valid = pivot.action == RotatePointAction::PivotCaptured &&
            reference.action == RotatePointAction::ReferenceCaptured &&
            finalPoint.action == RotatePointAction::CommitRequested &&
            rotate.commitAngle(finalPoint.angle, true, rotateHarness.context);
    if (valid) {
        const Shape &firstShape = rotateHarness.document[0];
        const Shape &secondShape = rotateHarness.document[1];
        const Point3D firstPoint = workPlaneFramePointToWorld(
            firstShape.nurbs.controlPoints.first(),
            shapeWorkPlaneFrame(firstShape));
        const Point3D secondPoint = workPlaneFramePointToWorld(
            secondShape.nurbs.controlPoints.first(),
            shapeWorkPlaneFrame(secondShape));
        valid = std::abs(firstPoint.x) < 1.0e-9 &&
                std::abs(firstPoint.y - 1.0) < 1.0e-9 &&
                std::abs(secondPoint.x + 1.0) < 1.0e-9 &&
                std::abs(secondPoint.y) < 1.0e-9;
    }
    if (!valid) {
        qWarning() << "RotateTool must commit the accepted angle to all selected objects";
        ++failures;
    }

    ToolHarness rotateKeyHarness;
    const ObjectId rotateKeyId = rotateKeyHarness.document.append(
        lineShape({{1, 0}, {2, 0}}));
    RotateTool rotateKeyTool;
    rotateKeyTool.beginSelection({rotateKeyId}, false);
    rotateKeyTool.begin(rotateKeyHarness.context);
    ToolInput rotateKeyInput;
    rotateKeyInput.workPlaneFrame = makeWorkPlaneFrame(WorkPlane::XY);
    rotateKeyInput.worldPosition = {0, 0};
    rotateKeyTool.acceptPoint(rotateKeyInput, 15.0, 6.0,
                              rotateKeyHarness.context);
    rotateKeyInput.worldPosition = {1, 0};
    rotateKeyTool.acceptPoint(rotateKeyInput, 15.0, 6.0,
                              rotateKeyHarness.context);
    rotateKeyInput.key = Qt::Key_4;
    rotateKeyInput.text = QStringLiteral("4");
    const bool rotateDigitHandled =
        rotateKeyTool.dispatchKey(rotateKeyInput, rotateKeyHarness.context) ==
        InteractionTool::EventResult::Handled;
    rotateKeyTool.takeLastKeyDispatchResult();
    rotateKeyInput.key = Qt::Key_5;
    rotateKeyInput.text = QStringLiteral("5");
    const bool rotateSecondDigitHandled =
        rotateKeyTool.dispatchKey(rotateKeyInput, rotateKeyHarness.context) ==
        InteractionTool::EventResult::Handled;
    rotateKeyTool.takeLastKeyDispatchResult();
    rotateKeyInput.key = Qt::Key_Return;
    rotateKeyInput.text.clear();
    const bool rotateEnterHandled =
        rotateKeyTool.dispatchKey(rotateKeyInput, rotateKeyHarness.context) ==
        InteractionTool::EventResult::Handled;
    const RotateKeyResult rotateEnterResult =
        rotateKeyTool.takeLastKeyDispatchResult();
    valid = rotateDigitHandled && rotateSecondDigitHandled &&
            rotateEnterHandled && rotateEnterResult.commitRequested &&
            std::abs(rotateEnterResult.commitAngle -
                     45.0 * 3.14159265358979323846 / 180.0) < 1.0e-9 &&
            rotateKeyTool.commitAngle(rotateEnterResult.commitAngle, false,
                                      rotateKeyHarness.context);
    if (valid) {
        const Shape &shape = *rotateKeyHarness.document.shape(rotateKeyId);
        const Point3D endpoint = workPlaneFramePointToWorld(
            shape.nurbs.controlPoints.last(), shapeWorkPlaneFrame(shape));
        valid = std::abs(endpoint.x - 2.0 *
                         std::cos(45.0 * 3.14159265358979323846 / 180.0)) <
                    1.0e-9 &&
                std::abs(endpoint.y - 2.0 *
                         std::sin(45.0 * 3.14159265358979323846 / 180.0)) <
                    1.0e-9;
    }
    if (!valid) {
        qWarning() << "RotateTool typed key dispatch must commit its accepted angle";
        ++failures;
    }

    RotateTool rotateEscapeTool;
    rotateEscapeTool.beginSelection({rotateKeyId}, true);
    rotateEscapeTool.begin(rotateKeyHarness.context);
    rotateKeyInput.key = Qt::Key_Escape;
    rotateKeyInput.text.clear();
    const bool rotateEscapeHandled =
        rotateEscapeTool.dispatchKey(rotateKeyInput,
                                     rotateKeyHarness.context) ==
        InteractionTool::EventResult::Handled;
    const RotateKeyResult rotateEscapeResult =
        rotateEscapeTool.takeLastKeyDispatchResult();
    valid = rotateEscapeHandled && rotateEscapeResult.cancelled &&
            rotateEscapeTool.interactionState().sourceObjectIds.isEmpty();
    if (!valid) {
        qWarning() << "RotateTool key dispatch must own Escape cancellation";
        ++failures;
    }

    ToolHarness scaleHarness;
    const ObjectId scaleId = scaleHarness.document.append(
        lineShape({{0, 0}, {2, 3}}));
    ScaleTool scale;
    scale.beginSelection({scaleId}, ScaleMode::OneD);
    scale.acceptPoint({0, 0});
    scale.acceptPoint({2, 0});
    const bool previewed = scale.updatePreview({4, 0}) &&
        scaleHarness.document.shape(scaleId)->nurbs.controlPoints[1] == QPointF(2, 3);
    const ScalePointResult scaleCommit = scale.acceptPoint({4, 0});
    valid = previewed && scaleCommit.action == ScalePointAction::CommitRequested &&
            std::abs(scaleCommit.factor - 2.0) < 1.0e-9 &&
            scale.commitScale(scaleCommit.factor, scaleCommit.axisDirection,
                              scaleHarness.context);
    if (valid) {
        const QPointF end = scaleHarness.document.shape(scaleId)->nurbs.controlPoints.last();
        valid = std::abs(end.x() - 4.0) < 1.0e-9 &&
                std::abs(end.y() - 3.0) < 1.0e-9;
    }
    if (!valid) {
        qWarning() << "ScaleTool must preview without mutation and commit its selected axis";
        ++failures;
    }

    ToolHarness twoDimensionalScaleHarness;
    const ObjectId twoDimensionalId = twoDimensionalScaleHarness.document.append(
        lineShape({{1, 1}, {2, 1}}));
    ScaleTool twoDimensionalScale;
    twoDimensionalScale.beginSelection({twoDimensionalId}, ScaleMode::TwoD);
    twoDimensionalScale.acceptPoint({0, 0});
    twoDimensionalScale.acceptPoint({1, 0});
    const ScalePointResult twoDimensionalCommit =
        twoDimensionalScale.acceptPoint({0, 2});
    valid = twoDimensionalCommit.action == ScalePointAction::CommitRequested &&
            std::abs(twoDimensionalCommit.factor - 2.0) < 1.0e-9 &&
            twoDimensionalScale.commitScale(
                twoDimensionalCommit.factor,
                twoDimensionalCommit.axisDirection,
                twoDimensionalScaleHarness.context);
    if (valid) {
        const auto &points =
            twoDimensionalScaleHarness.document.shape(twoDimensionalId)
                ->nurbs.controlPoints;
        valid = points[0] == QPointF(2, 2) && points[1] == QPointF(4, 2);
    }
    if (!valid) {
        qWarning() << "ScaleTool 2D must resize equally around its picked base point";
        ++failures;
    }

    ToolHarness typedScaleHarness;
    const ObjectId typedScaleId = typedScaleHarness.document.append(
        lineShape({{1, 1}, {2, 1}}));
    ScaleTool typedScale;
    typedScale.beginSelection({typedScaleId}, ScaleMode::OneD);
    typedScale.acceptPoint({0, 0});
    typedScale.appendFactorCharacter(QLatin1Char('2'));
    qreal typedFactor = 0.0;
    valid = typedScale.acceptFactorInput(&typedFactor) &&
            std::abs(typedFactor - 2.0) < 1.0e-9;
    const ScalePointResult typedCommit = typedScale.acceptPoint({1, 0});
    valid = valid && typedCommit.action == ScalePointAction::CommitRequested &&
            std::abs(typedCommit.factor - 2.0) < 1.0e-9 &&
            typedScale.commitScale(typedCommit.factor,
                                   typedCommit.axisDirection,
                                   typedScaleHarness.context);
    if (!valid) {
        qWarning() << "ScaleTool must accept a typed factor and picked direction";
        ++failures;
    }

    ToolHarness scaleKeyHarness;
    const ObjectId scaleKeyId = scaleKeyHarness.document.append(
        lineShape({{0, 0}, {1, 0}}));
    ScaleTool scaleKeyTool;
    scaleKeyTool.beginSelection({scaleKeyId}, ScaleMode::TwoD);
    scaleKeyTool.begin(scaleKeyHarness.context);
    ToolInput scaleKeyInput;
    scaleKeyInput.key = Qt::Key_Return;
    scaleKeyInput.worldPosition = {3.0, 4.0};
    const bool centerEnterHandled =
        scaleKeyTool.dispatchKey(scaleKeyInput, scaleKeyHarness.context) ==
        InteractionTool::EventResult::Handled;
    const ScaleKeyDispatchResult centerEnterResult =
        scaleKeyTool.takeLastKeyDispatchResult();
    valid = centerEnterHandled && centerEnterResult.promptChanged &&
            centerEnterResult.point.action ==
                ScalePointAction::BasePointCaptured &&
            centerEnterResult.point.point == QPointF(3.0, 4.0) &&
            scaleKeyTool.interactionState().stage == 1;
    scaleKeyInput.key = Qt::Key_Escape;
    const bool escapeHandled =
        scaleKeyTool.dispatchKey(scaleKeyInput, scaleKeyHarness.context) ==
        InteractionTool::EventResult::Handled;
    const ScaleKeyDispatchResult escapeResult =
        scaleKeyTool.takeLastKeyDispatchResult();
    valid = valid && escapeHandled && escapeResult.cancelled &&
            scaleKeyTool.interactionState().sourceObjectIds.isEmpty();
    if (!valid) {
        qWarning() << "ScaleTool key dispatch must own center-enter and Escape transitions";
        ++failures;
    }

    ToolHarness mirrorHarness;
    const ObjectId mirrorSource = mirrorHarness.document.append(
        lineShape({{1, 2}, {3, 2}}));
    mirrorHarness.selection.setObjectIds({mirrorSource}, mirrorSource);
    MirrorTool mirror;
    mirror.begin(mirrorHarness.context);
    mirror.setSourceObjectIds({mirrorSource});
    ToolInput axisInput;
    axisInput.button = Qt::LeftButton;
    axisInput.worldPosition = {0, 0};
    mirror.handleMousePress(axisInput, mirrorHarness.context);
    axisInput.worldPosition = {0, 1};
    mirror.handleMousePress(axisInput, mirrorHarness.context);
    const MirrorCommitResult mirrored = mirror.commitAxis(mirrorHarness.context);
    valid = mirrored.committed && mirrorHarness.document.size() == 2 &&
            mirrorHarness.selection.objectIds() == mirrored.createdObjectIds &&
            mirrorHarness.document[0].nurbs.controlPoints[0] == QPointF(1, 2) &&
            mirrorHarness.document[1].nurbs.controlPoints[0] == QPointF(-1, 2) &&
            mirrorHarness.document[1].nurbs.controlPoints[1] == QPointF(-3, 2);
    if (!valid) {
        qWarning() << "MirrorTool must preserve its source and commit the reflected copy";
        ++failures;
    }

    ToolHarness cancelHarness;
    const ObjectId cancelId = cancelHarness.document.append(
        lineShape({{0, 0}, {1, 0}}));
    ToolInput cancelInput;
    cancelInput.button = Qt::RightButton;

    ScaleTool rightClickScale;
    rightClickScale.beginSelection({cancelId}, ScaleMode::TwoD);
    rightClickScale.begin(cancelHarness.context);
    const bool scaleRightClickHandled =
        rightClickScale.dispatchMousePress(cancelInput,
                                           cancelHarness.context) ==
        InteractionTool::EventResult::Handled;
    const ScaleDispatchResult scaleRightClickResult =
        rightClickScale.takeLastDispatchResult();

    RotateTool rightClickRotate;
    rightClickRotate.beginSelection({cancelId}, true);
    rightClickRotate.begin(cancelHarness.context);
    const bool rotateRightClickHandled =
        rightClickRotate.dispatchMousePress(cancelInput,
                                            cancelHarness.context) ==
        InteractionTool::EventResult::Handled;

    MirrorTool rightClickMirror;
    rightClickMirror.begin(cancelHarness.context);
    rightClickMirror.setSourceObjectIds({cancelId});
    const bool mirrorRightClickHandled =
        rightClickMirror.dispatchMousePress(cancelInput,
                                            cancelHarness.context) ==
        InteractionTool::EventResult::Handled;
    MirrorTool escapeMirror;
    escapeMirror.begin(cancelHarness.context);
    escapeMirror.setSourceObjectIds({cancelId});
    ToolInput mirrorEscapeInput;
    mirrorEscapeInput.key = Qt::Key_Escape;
    const bool mirrorEscapeHandled =
        escapeMirror.dispatchKey(mirrorEscapeInput,
                                 cancelHarness.context) ==
        InteractionTool::EventResult::Handled;
    valid = scaleRightClickHandled && scaleRightClickResult.cancelled &&
            rightClickScale.interactionState().sourceObjectIds.isEmpty() &&
            rotateRightClickHandled &&
            rightClickRotate.interactionState().sourceObjectIds.isEmpty() &&
            mirrorRightClickHandled &&
            rightClickMirror.sourceObjectIds().isEmpty() &&
            mirrorEscapeHandled && escapeMirror.sourceObjectIds().isEmpty();
    if (!valid) {
        qWarning() << "Transform tools must own right-click cancellation";
        ++failures;
    }
    return failures == 0;
}

} // namespace

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    if (application.arguments().contains(QStringLiteral("--box-only"))) {
        const bool passed = runSelectionBoxCases();
        qInfo() << "Box selection:" << (passed ? "passed" : "failed");
        return passed ? 0 : 1;
    }
    if (application.arguments().contains(QStringLiteral("--update-only"))) {
        QTemporaryDir directory;
        if (!directory.isValid()) {
            qWarning() << "Update session fixture could not create a temporary directory";
            return 1;
        }
        Document document;
        const Shape source = lineShape({{-15, 0}, {15, 0}},
                                       makeWorkPlaneFrame(WorkPlane::XZ, 12.0));
        document.append(source);
        for (const bool perspective : {false, true}) {
            ViewportTransform sourceTransform;
            sourceTransform.setViewPreset(ViewportViewPreset::Isometric);
            sourceTransform.setPerspectiveEnabled(perspective);
            sourceTransform.orbitByPixels({43, -21});
            sourceTransform.panByPixels({62, -35}, QSize(840, 620));
            sourceTransform.zoomAt({320, 240}, 1.4, QSize(840, 620));
            sourceTransform.setWorkPlane(WorkPlane::XZ, 12.0);
            UpdateSessionViewState sourceView;
            sourceView.zoom = sourceTransform.zoom();
            sourceView.pan = sourceTransform.pan();
            sourceView.camera = sourceTransform.cameraState();
            sourceView.workPlane = sourceTransform.workPlane();
            sourceView.workPlaneOffset = sourceTransform.workPlaneOffset();
            sourceView.workPlaneFrame = sourceTransform.workPlaneFrame();
            sourceView.cameraPreferences = sourceTransform.cameraPreferences();
            const QString path = directory.filePath(QStringLiteral("update.json"));
            RestoredUpdateSession restored;
            bool matches = SessionSerializer::write(path, document, sourceView) &&
                           SessionSerializer::read(path, ViewportCameraPreferences{},
                                                   false, &restored);
            ViewportTransform restoredTransform;
            restoredTransform.setCameraPreferences(restored.view.cameraPreferences);
            restoredTransform.setWorkPlane(restored.view.workPlane,
                                            restored.view.workPlaneOffset);
            restoredTransform.setWorkPlaneFrame(restored.view.workPlaneFrame);
            restoredTransform.setCameraState(restored.view.camera);
            matches = matches && restored.view.workPlane == sourceTransform.workPlane() &&
                      restored.view.workPlaneOffset == sourceTransform.workPlaneOffset() &&
                      restoredTransform.isPerspectiveEnabled() == perspective;
            for (const Point3D &point : {Point3D{0, 0, 0}, Point3D{30, 12, 8},
                                         Point3D{-14, 19, 32}}) {
                QPointF before;
                QPointF after;
                matches = matches && sourceTransform.worldPointToScreenUnclipped(
                    point, QSize(840, 620), &before) &&
                    restoredTransform.worldPointToScreenUnclipped(
                    point, QSize(840, 620), &after) &&
                    std::hypot(before.x() - after.x(), before.y() - after.y()) < 1.0e-7;
            }
            if (!matches) {
                qWarning() << "Update must preserve projected scene positions" << perspective;
                return 1;
            }
        }
        return 0;
    }

    bool passed = runJoinTopologyCases();
    passed = runSelectionBoxCases() && passed;
    passed = runEraseGeometryCases() && passed;
    passed = runPolyCurveAndToolCases() && passed;
    passed = runCommandAndTransformCases() && passed;
    qInfo() << "Geometry/tool contract fixture:" << (passed ? "passed" : "failed");
    return passed ? 0 : 1;
}
