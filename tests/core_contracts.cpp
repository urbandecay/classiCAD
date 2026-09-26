#include "core/document/document.h"
#include "core/document/layer_id.h"
#include "core/document/object_id.h"
#include "core/document/selection_model.h"
#include "core/geometry/geometry_type.h"
#include "core/geometry/geometry_transform.h"
#include "core/geometry/curve_evaluator.h"
#include "core/geometry/nurbs_curve.h"
#include "core/history/history.h"
#include "core/model.h"
#include "core/serialization/document_serializer.h"
#include "core/tool_id.h"
#include "services/hit_testing/curve_hit_tester.h"
#include "services/sampling/curve_sampler.h"
#include "services/snapping/snap_engine.h"
#include "services/viewport/viewport_transform.h"
#include "tools/line_tool.h"
#include "tools/perpendicular_from_curve_tool.h"
#include "tools/point_tool.h"
#include "tools/tangent_from_curve_tool.h"
#include "tools/tool_context.h"
#include "tools/tool_registry.h"

#include <QCoreApplication>
#include <QJsonObject>

#include <cmath>
#include <type_traits>

using namespace classiCAD;

namespace {

bool check(bool condition, const char *message)
{
    if (!condition) {
        qCritical("%s", message);
    }
    return condition;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    bool passed = true;

    passed &= check(!std::is_same_v<ToolId, GeometryType>,
                    "tool and geometry vocabularies must be distinct types");
    passed &= check(geometryTypeForTool(ToolId::Line) == GeometryType::Line,
                    "line tool must map to line geometry");
    passed &= check(geometryTypeForTool(ToolId::Trim) == GeometryType::Invalid,
                    "trim must not map to persisted geometry");
    passed &= check(geometryTypeForTool(ToolId::PerpendicularFromCurve) ==
                            GeometryType::Invalid &&
                        toolName(ToolId::PerpendicularFromCurve) ==
                            QStringLiteral("Perpendicular from Curve"),
                    "perpendicular-from-curve must remain a named line command, not persisted geometry");
    passed &= check(geometryTypeForTool(ToolId::Mirror) == GeometryType::Invalid &&
                        toolName(ToolId::Mirror) == QStringLiteral("Mirror"),
                    "mirror must remain a command rather than persisted geometry");
    passed &= check(legacyValueForGeometryType(GeometryType::PolyCurve) == 8,
                    "PolyCurve legacy value must remain eight");

    const ObjectId objectId = ObjectId::fromValue(42);
    const LayerId layerId = LayerId::fromValue(42);
    passed &= check(objectId.isValid() && objectId.value() == 42,
                    "object IDs must preserve their stable value");
    passed &= check(layerId.isValid() && layerId.value() == 42,
                    "layer IDs must preserve their stable value");
    passed &= check(ObjectId::invalid() != objectId && LayerId::invalid() != layerId,
                    "zero IDs must remain the invalid sentinel");

    const NurbsCurve2D line = makeDegreeOneNurbs({QPointF(0.0, 0.0), QPointF(10.0, 0.0)});
    QString validationError;
    passed &= check(validateNurbsCurve(line, &validationError) && validationError.isEmpty(),
                    "factory line must satisfy NURBS invariants");

    NurbsCurve2D invalidCurve = line;
    invalidCurve.order = 3;
    passed &= check(!validateNurbsCurve(invalidCurve),
                    "invalid NURBS order must be rejected");

    const Shape lineShape{GeometryType::Line,
                          {QPointF(0.0, 0.0), QPointF(10.0, 0.0)},
                          line,
                          ArcMode::TwoPoint,
                          0.0,
                          {},
                          {}};

    const Shape circle{GeometryType::Circle,
                       {QPointF(0.0, 0.0), QPointF(10.0, 0.0)},
                       makeCircleNurbs({QPointF(0.0, 0.0), QPointF(10.0, 0.0)}),
                       ArcMode::TwoPoint,
                       0.0,
                       {},
                       {}};
    passed &= check(validateNurbsCurve(circle.nurbs),
                    "factory circle must satisfy rational NURBS invariants");
    constexpr qreal quarterTurn = 0.78539816339744830962;
    QPointF circleDerivative;
    passed &= check(evaluateNurbsDerivative(circle.nurbs,
                                            quarterTurn,
                                            &circleDerivative) &&
                        std::abs(circleDerivative.x() + circleDerivative.y()) <= 1.0e-8 &&
                        circleDerivative.x() < 0.0 && circleDerivative.y() > 0.0,
                    "rational NURBS derivative must match the exact circle tangent");

    Shape mirroredLine;
    passed &= check(mirrorShapeAcrossLine(lineShape,
                                          QPointF(0.0, 0.0),
                                          QPointF(0.0, 10.0),
                                          &mirroredLine) &&
                        mirroredLine.points == QVector<QPointF>{QPointF(0.0, 0.0),
                                                                QPointF(-10.0, 0.0)} &&
                        mirroredLine.nurbs.controlPoints ==
                            QVector<QPointF>{QPointF(0.0, 0.0), QPointF(-10.0, 0.0)} &&
                        mirroredLine.nurbs.weights == lineShape.nurbs.weights &&
                        mirroredLine.nurbs.knots == lineShape.nurbs.knots &&
                        validateNurbsCurve(mirroredLine.nurbs),
                    "mirror must transform stored NURBS positions while preserving invariants");

    Shape rectangle{GeometryType::Rectangle,
                    {QPointF(1.0, 1.0), QPointF(3.0, 4.0)},
                    {},
                    ArcMode::TwoPoint,
                    0.0,
                    {},
                    {}};
    Shape mirroredRectangle;
    passed &= check(mirrorShapeAcrossLine(rectangle,
                                          QPointF(0.0, 0.0),
                                          QPointF(1.0, 0.0),
                                          &mirroredRectangle) &&
                        mirroredRectangle.points.size() == 4 &&
                        mirroredRectangle.points[0] == QPointF(1.0, -1.0) &&
                        mirroredRectangle.points[2] == QPointF(3.0, -4.0),
                    "mirror must expand and reflect two-point rectangles");

    const QJsonObject serialized = shapeToJson(circle);
    passed &= check(serialized.value(QStringLiteral("geometryType")).toInt(-1) == 6,
                    "new sessions must write the geometry type field");
    passed &= check(serialized.value(QStringLiteral("tool")).toInt(-1) == 6,
                    "new sessions must retain the legacy compatibility field");

    Shape restored{GeometryType::Invalid,
                   {},
                   {},
                   ArcMode::TwoPoint,
                   0.0,
                   {},
                   {}};
    passed &= check(shapeFromJson(serialized, &restored) &&
                        restored.geometryType == GeometryType::Circle,
                    "new geometry type sessions must restore");

    QJsonObject legacySerialized = serialized;
    legacySerialized.remove(QStringLiteral("geometryType"));
    Shape legacyRestored{GeometryType::Invalid,
                         {},
                         {},
                         ArcMode::TwoPoint,
                         0.0,
                         {},
                         {}};
    passed &= check(shapeFromJson(legacySerialized, &legacyRestored) &&
                        legacyRestored.geometryType == GeometryType::Circle,
                    "version-1 tool sessions must remain readable");

    QJsonObject commandAsShape = legacySerialized;
    commandAsShape.insert(QStringLiteral("tool"), static_cast<int>(ToolId::Trim));
    passed &= check(!shapeFromJson(commandAsShape, &legacyRestored),
                    "commands must not deserialize as persistent geometry");

    QJsonObject invalidCurveSerialized = serialized;
    QJsonObject invalidNurbs = invalidCurveSerialized.value(QStringLiteral("nurbs")).toObject();
    QJsonArray invalidWeights = invalidNurbs.value(QStringLiteral("weights")).toArray();
    invalidWeights.replace(0, -1.0);
    invalidNurbs.insert(QStringLiteral("weights"), invalidWeights);
    invalidCurveSerialized.insert(QStringLiteral("nurbs"), invalidNurbs);
    passed &= check(!shapeFromJson(invalidCurveSerialized, &restored),
                    "serialized curves with non-positive weights must be rejected");

    Document document;
    const LayerId sketchLayer = document.createLayer(QStringLiteral("Sketch"));
    const ObjectId lineObject = document.append(lineShape);
    const ObjectId circleObject = document.append(circle);
    passed &= check(document.indexOf(lineObject) == 0 &&
                        document.indexOf(circleObject) == 1,
                    "document must own objects in insertion order");
    passed &= check(document.moveObjectToLayer(circleObject, sketchLayer) &&
                        document.layer(sketchLayer)->objectIds.contains(circleObject),
                    "document must maintain layer object membership");
    passed &= check(document.setLayerLocked(sketchLayer, true) &&
                        !document.isObjectEditable(circleObject) &&
                        document.isObjectEditable(lineObject),
                    "layer locking must affect object editability");

    const Document::Snapshot documentSnapshot = document.snapshot();
    document.remove(lineObject);
    passed &= check(document.indexOf(circleObject) == 0,
                    "removing an object must not change its stable identity");
    document.restoreSnapshot(documentSnapshot);
    passed &= check(document.indexOf(lineObject) == 0 &&
                        document.indexOf(circleObject) == 1,
                    "document snapshots must restore stable object identities");

    SelectionModel selection;
    selection.setObjectIds({lineObject, circleObject}, circleObject);
    selection.setActiveControlPoint(circleObject, 2);
    passed &= check(selection.objectIds().size() == 2 &&
                        selection.primaryObjectId() == circleObject &&
                        selection.activeControlPoint().isValid(),
                    "selection model must own object and control-point references");
    document.remove(lineObject);
    selection.prune(document);
    passed &= check(selection.objectIds() == QVector<ObjectId>{circleObject} &&
                        selection.primaryObjectId() == circleObject,
                    "selection model must prune deleted object IDs");

    Document layerDocument;
    const LayerId layeredSketch = layerDocument.createLayer(QStringLiteral("Sketch"));
    const LayerId layeredConstruction =
        layerDocument.createLayer(QStringLiteral("Construction"));
    const ObjectId layeredObject = layerDocument.append(lineShape);
    passed &= check(layerDocument.moveObjectToLayer(layeredObject, layeredSketch) &&
                        layerDocument.renameLayer(layeredSketch, QStringLiteral("Sketch Curves")) &&
                        layerDocument.moveLayer(layeredConstruction, 0),
                    "document must support stable layer rename and reorder operations");
    passed &= check(layerDocument.setActiveLayer(layeredConstruction) &&
                        layerDocument.activeLayerId() == layeredConstruction,
                    "document must track an editable active layer");
    passed &= check(layerDocument.setLayerVisible(layeredSketch, false) &&
                        !layerDocument.isObjectVisible(layeredObject) &&
                        !layerDocument.isObjectEditable(layeredObject),
                    "layer visibility must filter object visibility and editability");
    passed &= check(layerDocument.setLayerVisible(layeredSketch, true) &&
                        layerDocument.setLayerLocked(layeredSketch, true) &&
                        !layerDocument.isObjectEditable(layeredObject) &&
                        !layerDocument.moveObjectToLayer(layeredObject, layeredConstruction),
                    "layer locking must prevent object movement from a locked layer");

    const QJsonObject serializedDocument = documentToJson(layerDocument);
    Document restoredLayerDocument;
    QString documentError;
    passed &= check(documentFromJson(serializedDocument,
                                     &restoredLayerDocument,
                                     &documentError) &&
                        documentError.isEmpty() &&
                        restoredLayerDocument.activeLayerId() == layeredConstruction &&
                        restoredLayerDocument.object(layeredObject) != nullptr &&
                        restoredLayerDocument.object(layeredObject)->layerId == layeredSketch &&
                        restoredLayerDocument.layer(layeredSketch) != nullptr &&
                        restoredLayerDocument.layer(layeredSketch)->name ==
                            QStringLiteral("Sketch Curves") &&
                        restoredLayerDocument.layer(layeredSketch)->locked,
                    "document serialization must preserve layer IDs, flags, active layer, and object membership");

    Document historyDocument;
    History history(historyDocument);
    history.record();
    const ObjectId historyObject = historyDocument.append(lineShape);
    passed &= check(history.canUndo() && history.undoCount() == 1,
                    "history must record the pre-edit document snapshot");
    passed &= check(history.undo() && historyDocument.isEmpty() && history.canRedo(),
                    "history undo must restore the document snapshot");
    passed &= check(history.redo() && historyDocument.indexOf(historyObject) == 0,
                    "history redo must restore object identity");
    history.record();
    historyDocument.replace(historyObject, circle);
    passed &= check(history.undo() &&
                        historyDocument.shape(historyObject) != nullptr &&
                        historyDocument.shape(historyObject)->geometryType == GeometryType::Line,
                    "history must restore geometry edits through the same path");

    const QSize viewportSize(640, 480);
    ViewportTransform viewportTransform;
    const QPointF serviceWorldPoint(12.5, -7.25);
    const QPointF serviceScreenPoint =
        viewportTransform.worldToScreen(serviceWorldPoint, viewportSize);
    const QPointF roundTripWorld =
        viewportTransform.screenToWorld(serviceScreenPoint, viewportSize);
    passed &= check(std::hypot(roundTripWorld.x() - serviceWorldPoint.x(),
                               roundTripWorld.y() - serviceWorldPoint.y()) <= 1.0e-9,
                    "viewport transform must preserve world/screen round trips");

    Document serviceDocument;
    serviceDocument.append(lineShape);
    CurveSampler sampler;
    SampledNurbsCurve2D sampled;
    passed &= check(sampler.sampleNurbsCurve(line,
                                             viewportTransform,
                                             viewportSize,
                                             &sampled) &&
                        sampled.parameters.size() >= 2 &&
                        sampled.parameters.size() == sampled.screenPoints.size(),
                    "curve sampler must sample the stored NURBS curve");
    const QVector<EraseCurveSampleCache> sampledScene =
        sampler.sampleDocument(serviceDocument, viewportTransform, viewportSize);
    passed &= check(sampledScene.size() == 1 &&
                        sampledScene.first().shapeIndex == 0 &&
                        sampledScene.first().componentIndex == 0,
                    "curve sampler must build erase caches from document geometry");

    CurveHitTester hitTester;
    const QPointF lineScreenPoint =
        viewportTransform.worldToScreen(QPointF(5.0, 0.0), viewportSize);
    passed &= check(hitTester.hitTestShape(serviceDocument,
                                           lineScreenPoint,
                                           viewportTransform,
                                           viewportSize) == 0,
                    "curve hit tester must hit committed NURBS geometry");
    int hitShapeIndex = -1;
    int hitControlPointIndex = -1;
    passed &= check(hitTester.hitTestSelectedControlPoint(
                        serviceDocument,
                        {0},
                        viewportTransform.worldToScreen(QPointF(0.0, 0.0),
                                                        viewportSize),
                        viewportTransform,
                        viewportSize,
                        &hitShapeIndex,
                        &hitControlPointIndex) &&
                        hitShapeIndex == 0 && hitControlPointIndex == 0,
                    "control-point hit tester must use selected curve control points");

    SnapEngine snapEngine;
    snapEngine.setSettings(SnapSettings{true, true, true, true, true, false, false});
    const SnapResult endpointSnap = snapEngine.findSnapPoint(
        serviceDocument,
        QPointF(0.25, 0.0),
        true,
        {},
        viewportTransform,
        viewportSize);
    passed &= check(endpointSnap.type == SnapType::Endpoint &&
                        std::hypot(endpointSnap.point.x(), endpointSnap.point.y()) <= 1.0e-9,
                    "snap engine must select the nearest enabled endpoint");

    Shape nurbsCircleShape{GeometryType::Nurbs,
                           {},
                           circle.nurbs,
                           ArcMode::TwoPoint,
                           0.0,
                           {},
                           {}};
    const QPointF nurbsCircleTangentOrigin(20.0, 7.0);
    const QVector<SnapCandidate> nurbsCircleTangents = snapEngine.tangentCandidatesForShape(
        nurbsCircleShape,
        nurbsCircleTangentOrigin,
        viewportTransform,
        viewportSize);
    bool nurbsTangenciesAreValid = nurbsCircleTangents.size() == 2;
    for (const SnapCandidate &candidate : nurbsCircleTangents) {
        const QPointF radial = candidate.point;
        const QPointF lineDirection = nurbsCircleTangentOrigin - candidate.point;
        const qreal normalizedDot = std::abs(QPointF::dotProduct(radial, lineDirection)) /
                                    (10.0 * std::hypot(lineDirection.x(), lineDirection.y()));
        nurbsTangenciesAreValid &= candidate.type == SnapType::Tangent &&
                                   std::abs(std::hypot(radial.x(), radial.y()) - 10.0) <=
                                       1.0e-7 &&
                                   normalizedDot <= 1.0e-7;
    }
    passed &= check(nurbsTangenciesAreValid,
                    "NURBS tangencies must lie on the curve with the line perpendicular to its derivative");

    const QPointF editedCircleCenter(30.0, 40.0);
    const QPointF editedCircleEdge(35.0, 40.0);
    const Shape editedCircleShape{GeometryType::Circle,
                                  {QPointF(0.0, 0.0), QPointF(10.0, 0.0)},
                                  makeCircleNurbs({editedCircleCenter, editedCircleEdge}),
                                  ArcMode::TwoPoint,
                                  0.0,
                                  {},
                                  {}};
    const QPointF editedCircleOrigin(50.0, 47.0);
    const QVector<SnapCandidate> editedCircleTangents = snapEngine.tangentCandidatesForShape(
        editedCircleShape,
        editedCircleOrigin,
        viewportTransform,
        viewportSize);
    bool editedCircleTangenciesUseVisibleCurve = editedCircleTangents.size() == 2;
    for (const SnapCandidate &candidate : editedCircleTangents) {
        const QPointF radial = candidate.point - editedCircleCenter;
        const QPointF tangent = editedCircleOrigin - candidate.point;
        const qreal radius = std::hypot(radial.x(), radial.y());
        const qreal tangentLength = std::hypot(tangent.x(), tangent.y());
        editedCircleTangenciesUseVisibleCurve &=
            candidate.type == SnapType::Tangent &&
            std::abs(radius - 5.0) <= 1.0e-7 && tangentLength > 1.0e-12 &&
            std::abs(QPointF::dotProduct(radial, tangent)) /
                    (radius * tangentLength) <=
                1.0e-7;
    }
    passed &= check(editedCircleTangenciesUseVisibleCurve,
                    "edited circles must calculate tangencies from stored NURBS, not stale construction points");

    const Shape editedArcShape{GeometryType::Arc,
                               {QPointF(0.0, 0.0),
                                QPointF(10.0, 0.0),
                                QPointF(0.0, 10.0)},
                               editedCircleShape.nurbs,
                               ArcMode::TwoPoint,
                               0.0,
                               {},
                               {}};
    const QVector<SnapCandidate> editedArcTangents = snapEngine.tangentCandidatesForShape(
        editedArcShape,
        editedCircleOrigin,
        viewportTransform,
        viewportSize);
    bool editedArcTangenciesUseVisibleCurve = editedArcTangents.size() == 2;
    for (const SnapCandidate &candidate : editedArcTangents) {
        const QPointF radial = candidate.point - editedCircleCenter;
        const QPointF tangent = editedCircleOrigin - candidate.point;
        const qreal radius = std::hypot(radial.x(), radial.y());
        const qreal tangentLength = std::hypot(tangent.x(), tangent.y());
        editedArcTangenciesUseVisibleCurve &=
            candidate.type == SnapType::Tangent &&
            std::abs(radius - 5.0) <= 1.0e-7 && tangentLength > 1.0e-12 &&
            std::abs(QPointF::dotProduct(radial, tangent)) /
                    (radius * tangentLength) <=
                1.0e-7;
    }
    passed &= check(editedArcTangenciesUseVisibleCurve,
                    "edited arcs must calculate tangencies from stored NURBS, not stale construction points");

    const QVector<QPointF> archControlPoints{QPointF(0.0, 0.0),
                                              QPointF(0.0, 100.0),
                                              QPointF(100.0, 100.0),
                                              QPointF(100.0, 0.0)};
    const Shape::NurbsCurve2D archCurve = makeBezierNurbs(archControlPoints);
    const Shape archShape{GeometryType::Bezier,
                          archControlPoints,
                          archCurve,
                          ArcMode::TwoPoint,
                          0.0,
                          {},
                          {}};
    const QPointF archOrigin(50.0, 130.0);
    const QVector<SnapCandidate> archTangents = snapEngine.tangentCandidatesForShape(
        archShape, archOrigin, viewportTransform, viewportSize);
    qreal archStartParameter = 0.0;
    qreal archEndParameter = 0.0;
    bool freeformTangenciesAreValid = !archTangents.isEmpty() &&
        nurbsParameterDomain(archCurve, &archStartParameter, &archEndParameter);
    for (const SnapCandidate &candidate : archTangents) {
        qreal low = archStartParameter;
        qreal high = archEndParameter;
        for (int iteration = 0; iteration < 64; ++iteration) {
            const qreal middle = (low + high) * 0.5;
            QPointF point;
            if (!evaluateNurbsPoint(archCurve, middle, &point)) {
                freeformTangenciesAreValid = false;
                break;
            }
            if (point.x() < candidate.point.x()) {
                low = middle;
            } else {
                high = middle;
            }
        }
        const qreal parameter = (low + high) * 0.5;
        QPointF point;
        QPointF derivative;
        if (!evaluateNurbsPoint(archCurve, parameter, &point) ||
            !evaluateNurbsDerivative(archCurve, parameter, &derivative)) {
            freeformTangenciesAreValid = false;
            continue;
        }
        const QPointF chord = point - archOrigin;
        const qreal denominator = std::hypot(chord.x(), chord.y()) *
                                  std::hypot(derivative.x(), derivative.y());
        const qreal tangentResidual = denominator <= 1.0e-12
                                          ? 1.0
                                          : std::abs(crossProduct(chord, derivative)) /
                                                denominator;
        freeformTangenciesAreValid &= candidate.type == SnapType::Tangent &&
                                      std::hypot(point.x() - candidate.point.x(),
                                                 point.y() - candidate.point.y()) <= 1.0e-6 &&
                                      tangentResidual <= 1.0e-7;
    }
    passed &= check(freeformTangenciesAreValid,
                    "freeform Bezier tangent lines must match the curve's analytic derivative");

    SnapSettings nearOnlySettings{true, false, false, false, false, false, false, true};
    SnapEngine nearSnapEngine;
    nearSnapEngine.setSettings(nearOnlySettings);
    const QPointF nearLineCursor = viewportTransform.screenToWorld(
        lineScreenPoint + QPointF(0.0, 5.0), viewportSize);
    const SnapResult nearLineSnap = nearSnapEngine.findSnapPoint(serviceDocument,
                                                                 nearLineCursor,
                                                                 true,
                                                                 {},
                                                                 viewportTransform,
                                                                 viewportSize);
    passed &= check(nearLineSnap.type == SnapType::Near &&
                        std::hypot(nearLineSnap.point.x() - 5.0,
                                   nearLineSnap.point.y()) <= 1.0e-6,
                    "Near OSnap must find the closest point along line geometry");

    SnapEngine controlPointOsnapEngine;
    controlPointOsnapEngine.setSettings(
        SnapSettings{true, false, false, false, false, false, false, false, true});
    Document controlPointOsnapDocument;
    controlPointOsnapDocument.append(archShape);
    const SnapResult bezierControlPointSnap = controlPointOsnapEngine.findSnapPoint(
        controlPointOsnapDocument,
        archControlPoints[1] + QPointF(2.0, 1.0),
        true,
        {},
        viewportTransform,
        viewportSize);
    passed &= check(bezierControlPointSnap.type == SnapType::ControlPoint &&
                        bezierControlPointSnap.point == archControlPoints[1],
                    "Control Points OSnap must target the stored NURBS control vertices");

    controlPointOsnapEngine.setSettings(SnapSettings{true,
                                                     false,
                                                     false,
                                                     false,
                                                     false,
                                                     false,
                                                     false,
                                                     false,
                                                     false});
    const SnapResult disabledControlPointSnap = controlPointOsnapEngine.findSnapPoint(
        controlPointOsnapDocument,
        archControlPoints[1],
        true,
        {},
        viewportTransform,
        viewportSize);
    passed &= check(!disabledControlPointSnap.isValid(),
                    "Control Points OSnap must not produce candidates when its mode is off");

    Document controlPointSnapDocument;
    Shape movingControlPointLine = lineShape;
    movingControlPointLine.points = {QPointF(17.0, 50.0), QPointF(100.0, 100.0)};
    movingControlPointLine.nurbs = makeDegreeOneNurbs(movingControlPointLine.points);
    controlPointSnapDocument.append(movingControlPointLine);
    controlPointSnapDocument.append(
        Shape{GeometryType::Rectangle,
              {QPointF(0.0, 0.0), QPointF(100.0, 50.0)},
              {},
              ArcMode::TwoPoint,
              0.0,
              {},
              {}});
    SnapEngine controlPointSnapEngine;
    controlPointSnapEngine.setSettings(
        SnapSettings{false, false, false, false, false, false, false, false, true});
    const DragSnapResult rectangleCornerSnap =
        controlPointSnapEngine.findControlPointSnap(controlPointSnapDocument,
                                                    0,
                                                    0,
                                                    QPointF(17.0, 50.0),
                                                    viewportTransform,
                                                    viewportSize);
    passed &= check(rectangleCornerSnap.type == SnapType::ControlPoint &&
                        rectangleCornerSnap.targetPoint == QPointF(0.0, 50.0),
                    "control-point dragging must snap to enabled control-point targets even when OSnap is disabled");

    controlPointSnapEngine.setSettings(
        SnapSettings{false, false, false, false, false, false, false, false, false});
    const DragSnapResult disabledControlPointDragSnap =
        controlPointSnapEngine.findControlPointSnap(controlPointSnapDocument,
                                                    0,
                                                    0,
                                                    QPointF(17.0, 50.0),
                                                    viewportTransform,
                                                    viewportSize);
    passed &= check(!disabledControlPointDragSnap.isValid(),
                    "control-point dragging must not snap to control points when that OSnap mode is off");

    SnapEngine endpointControlPointSnapEngine;
    endpointControlPointSnapEngine.setSettings(
        SnapSettings{true, true, false, false, false, false, false});
    const DragSnapResult endpointModeCornerSnap =
        endpointControlPointSnapEngine.findControlPointSnap(controlPointSnapDocument,
                                                            0,
                                                            0,
                                                            QPointF(17.0, 50.0),
                                                            viewportTransform,
                                                            viewportSize);
    passed &= check(endpointModeCornerSnap.type == SnapType::Endpoint &&
                        endpointModeCornerSnap.targetPoint == QPointF(0.0, 50.0),
                    "Endpoint OSnap must identify rectangle corners as endpoints while dragging a control point");

    const QPointF fixedLineEndpoint(20.0, 0.0);
    const QPointF circleTangentPoint(5.0, 8.660254037844386);
    const QPointF draggedLineEndpoint = circleTangentPoint + QPointF(1.0, 0.0);
    Document tangentControlPointDocument;
    tangentControlPointDocument.append(
        Shape{GeometryType::Line,
              {fixedLineEndpoint, draggedLineEndpoint},
              makeDegreeOneNurbs({fixedLineEndpoint, draggedLineEndpoint}),
              ArcMode::TwoPoint,
              0.0,
              {},
              {}});
    tangentControlPointDocument.append(circle);
    SnapEngine tangentControlPointSnapEngine;
    tangentControlPointSnapEngine.setSettings(
        SnapSettings{true, true, false, false, false, false, true, false, false});
    const DragSnapResult draggedEndpointTangentSnap =
        tangentControlPointSnapEngine.findControlPointSnap(
            tangentControlPointDocument,
            0,
            1,
            draggedLineEndpoint,
            viewportTransform,
            viewportSize);
    passed &= check(draggedEndpointTangentSnap.type == SnapType::Tangent &&
                        std::hypot(draggedEndpointTangentSnap.targetPoint.x() -
                                       circleTangentPoint.x(),
                                   draggedEndpointTangentSnap.targetPoint.y() -
                                       circleTangentPoint.y()) <= 1.0e-6,
                    "dragged line endpoints must snap to curve tangencies when Tangent OSnap is enabled");

    const NurbsCurve2D nurbsOnlyBezier = makeBezierNurbs(
        {QPointF(50.0, 50.0), QPointF(60.0, 50.0),
         QPointF(70.0, 50.0), QPointF(80.0, 50.0)});
    const NurbsCurve2D nearBezierCurve = makeBezierNurbs(
        {QPointF(0.0, 0.0), QPointF(0.0, 10.0),
         QPointF(10.0, 10.0), QPointF(10.0, 0.0)});
    Shape projectionBezier{GeometryType::Bezier,
                           {},
                           nearBezierCurve,
                           ArcMode::TwoPoint,
                           0.0,
                           {},
                           {}};
    QPointF perpendicularBezierPoint;
    const bool foundBezierPerpendicular = snapEngine.perpendicularPointForShape(
        projectionBezier,
        QPointF(5.0, 12.5),
        viewportTransform,
        viewportSize,
        &perpendicularBezierPoint);
    passed &= check(foundBezierPerpendicular &&
                        std::hypot(perpendicularBezierPoint.x() - 5.0,
                                   perpendicularBezierPoint.y() - 7.5) <= 1.0e-6,
                    "perpendicular projection must evaluate the stored NURBS curve");
    Document nearBezierDocument;
    nearBezierDocument.append(Shape{GeometryType::Bezier,
                                    {},
                                    nearBezierCurve,
                                    ArcMode::TwoPoint,
                                    0.0,
                                    {},
                                    {}});
    const QPointF nearBezierPoint(5.0, 7.5);
    const QPointF nearBezierCursor = viewportTransform.screenToWorld(
        viewportTransform.worldToScreen(nearBezierPoint, viewportSize),
        viewportSize);
    const SnapResult nearBezierSnap = nearSnapEngine.findSnapPoint(
        nearBezierDocument,
        nearBezierCursor,
        true,
        {},
        viewportTransform,
        viewportSize);
    passed &= check(nearBezierSnap.type == SnapType::Near &&
                        std::hypot(nearBezierSnap.point.x() - nearBezierPoint.x(),
                                   nearBezierSnap.point.y() - nearBezierPoint.y()) <= 1.0e-6,
                    "Near OSnap must find the closest evaluated point on stored NURBS curves");

    Document nurbsEndpointDocument;
    Shape nurbsEndpointSource = movingControlPointLine;
    nurbsEndpointSource.points = {QPointF(52.0, 50.0), QPointF(100.0, 100.0)};
    nurbsEndpointSource.nurbs = makeDegreeOneNurbs(nurbsEndpointSource.points);
    nurbsEndpointDocument.append(nurbsEndpointSource);
    nurbsEndpointDocument.append(Shape{GeometryType::Bezier,
                                       {},
                                       nurbsOnlyBezier,
                                       ArcMode::TwoPoint,
                                       0.0,
                                       {},
                                       {}});
    const DragSnapResult nurbsEndpointSnap =
        endpointControlPointSnapEngine.findControlPointSnap(nurbsEndpointDocument,
                                                            0,
                                                            0,
                                                            QPointF(52.0, 50.0),
                                                            viewportTransform,
                                                            viewportSize);
    passed &= check(nurbsEndpointSnap.type == SnapType::Endpoint &&
                        nurbsEndpointSnap.targetPoint == QPointF(50.0, 50.0),
                    "Endpoint OSnap must find evaluated endpoints on NURBS-only curves");

    Document reverseDragSnapDocument;
    reverseDragSnapDocument.append(
        Shape{GeometryType::Rectangle,
              {QPointF(50.0, 52.0), QPointF(100.0, 100.0)},
              {},
              ArcMode::TwoPoint,
              0.0,
              {},
              {}});
    reverseDragSnapDocument.append(Shape{GeometryType::Bezier,
                                         {},
                                         nurbsOnlyBezier,
                                         ArcMode::TwoPoint,
                                         0.0,
                                         {},
                                         {}});
    const DragSnapResult reverseDirectionSnap =
        endpointControlPointSnapEngine.findDragSnap(reverseDragSnapDocument,
                                                    {1},
                                                    viewportTransform,
                                                    viewportSize);
    passed &= check(reverseDirectionSnap.type == SnapType::Endpoint &&
                        reverseDirectionSnap.sourcePoint == QPointF(50.0, 50.0) &&
                        reverseDirectionSnap.targetPoint == QPointF(50.0, 52.0),
                    "Bezier endpoints must remain snap sources when dragging the curve toward a rectangle");

    Document toolDocument;
    SelectionModel toolSelection;
    History toolHistory(toolDocument);
    ViewportTransform toolTransform;
    CurveSampler toolSampler;
    CurveHitTester toolHitTester;
    SnapEngine toolSnapEngine;
    ToolContext toolContext(toolDocument,
                            toolSelection,
                            toolHistory,
                            toolTransform,
                            toolSampler,
                            toolHitTester,
                            toolSnapEngine);
    QVector<Shape> committedToolShapes;
    ToolId finishedTool = ToolId::Select;
    int previewUpdateCount = 0;
    toolContext.setShapeFactory(
        [](ToolId tool,
           const QVector<QPointF> &points,
           ArcMode arcMode,
           qreal arcSweep,
           Shape *shape) {
            if (shape == nullptr || points.isEmpty()) {
                return false;
            }
            *shape = Shape{geometryTypeForTool(tool),
                           points,
                           Shape::NurbsCurve2D{},
                           arcMode,
                           arcSweep,
                           {},
                           {}};
            if (tool == ToolId::Line) {
                shape->nurbs = makeDegreeOneNurbs(points);
            }
            return true;
        });
    toolContext.setShapeCommitter(
        [&committedToolShapes](ToolId, const Shape &shape) {
            committedToolShapes.append(shape);
            return true;
        });
    toolContext.setToolFinisher([&finishedTool](ToolId tool) {
        finishedTool = tool;
    });
    toolContext.setPreviewPublisher([&previewUpdateCount](const ToolPreview &) {
        ++previewUpdateCount;
    });

    ToolRegistry toolRegistry;
    passed &= check(toolRegistry.find(ToolId::Select) != nullptr &&
                        toolRegistry.find(ToolId::Point) != nullptr &&
                        toolRegistry.find(ToolId::Line) != nullptr &&
                        toolRegistry.find(ToolId::TangentFromCurve) != nullptr &&
                        toolRegistry.find(ToolId::PerpendicularFromCurve) != nullptr &&
                        toolRegistry.find(ToolId::Arc) != nullptr &&
                        toolRegistry.find(ToolId::Rectangle) != nullptr &&
                        toolRegistry.find(ToolId::Circle) != nullptr &&
                        toolRegistry.find(ToolId::Bezier) != nullptr &&
                        toolRegistry.find(ToolId::Nurbs) != nullptr &&
                        toolRegistry.find(ToolId::Rotate) != nullptr &&
                        toolRegistry.find(ToolId::Mirror) != nullptr &&
                        toolRegistry.find(ToolId::Trim) != nullptr &&
                        toolRegistry.find(ToolId::Erase) != nullptr,
                    "tool registry must expose the named interaction tool modules");

    PointTool pointTool;
    pointTool.begin(toolContext);
    ToolInput pointInput;
    pointInput.button = Qt::LeftButton;
    pointInput.worldPosition = QPointF(3.0, 4.0);
    passed &= check(pointTool.handleMousePress(pointInput, toolContext) &&
                        committedToolShapes.size() == 1 &&
                        committedToolShapes.back().geometryType == GeometryType::Point &&
                        committedToolShapes.back().points == QVector<QPointF>{QPointF(3.0, 4.0)} &&
                        finishedTool == ToolId::Select,
                    "point tool must commit through ToolContext and finish to Select");

    LineTool lineTool;
    lineTool.begin(toolContext);
    ToolInput lineInput;
    lineInput.button = Qt::LeftButton;
    lineInput.worldPosition = QPointF(0.0, 0.0);
    lineTool.handleMousePress(lineInput, toolContext);
    lineInput.worldPosition = QPointF(10.0, 0.0);
    lineTool.handleMousePress(lineInput, toolContext);
    lineInput.button = Qt::RightButton;
    passed &= check(lineTool.handleMousePress(lineInput, toolContext) &&
                        committedToolShapes.size() == 2 &&
                        committedToolShapes.back().geometryType == GeometryType::Line &&
                        validateNurbsCurve(committedToolShapes.back().nurbs) &&
                        previewUpdateCount >= 3 &&
                        finishedTool == ToolId::Select,
                    "line tool must retain connected points until right-click commit");

    const Shape tangentTestCircle{GeometryType::Circle,
                                  {QPointF(0.0, 0.0), QPointF(10.0, 0.0)},
                                  makeCircleNurbs({QPointF(0.0, 0.0), QPointF(10.0, 0.0)}),
                                  ArcMode::TwoPoint,
                                  0.0,
                                  {},
                                  {}};
    const ObjectId tangentCircleId = toolDocument.append(tangentTestCircle);
    constexpr QPointF tangentEndpoint(20.0, 0.0);
    const ObjectId tangentTargetId = toolDocument.append(
        Shape{GeometryType::Line,
              {tangentEndpoint, QPointF(30.0, 0.0)},
              makeDegreeOneNurbs({tangentEndpoint, QPointF(30.0, 0.0)}),
              ArcMode::TwoPoint,
              0.0,
              {},
              {}});
    constexpr QSize tangentViewportSize(640, 480);
    const auto tangentInputAt = [&](const QPointF &worldPoint) {
        ToolInput input;
        input.screenPosition = toolTransform.worldToScreen(worldPoint,
                                                           tangentViewportSize);
        input.rawWorldPosition = worldPoint;
        input.worldPosition = worldPoint;
        input.viewportSize = tangentViewportSize;
        input.button = Qt::LeftButton;
        return input;
    };

    TangentFromCurveTool tangentTool;
    tangentTool.begin(toolContext);
    const ToolInput tangentCurvePick = tangentInputAt(QPointF(10.0, 0.0));
    const bool tangentCurvePicked = tangentTool.handleMousePress(tangentCurvePick,
                                                                 toolContext);
    const ToolInput insideCircleCursor = tangentInputAt(QPointF(5.0, 0.0));
    tangentTool.handleMouseMove(insideCircleCursor, toolContext);
    const bool tangentUnavailableInside = tangentTool.preview().points.isEmpty();

    const QPointF rawTangentCursor(19.5, 0.25);
    SnapEngine tangentEndpointSnapEngine;
    tangentEndpointSnapEngine.setSettings(
        SnapSettings{true, true, false, false, false, false, false, false});
    const SnapResult tangentEndpointSnap = tangentEndpointSnapEngine.findSnapPoint(
        toolDocument,
        rawTangentCursor,
        true,
        {},
        toolTransform,
        tangentViewportSize);
    ToolInput tangentCursor = tangentInputAt(tangentEndpointSnap.point);
    tangentCursor.screenPosition = toolTransform.worldToScreen(rawTangentCursor,
                                                              tangentViewportSize);
    tangentCursor.rawWorldPosition = rawTangentCursor;
    tangentTool.handleMouseMove(tangentCursor, toolContext);
    const ToolPreview tangentPreview = tangentTool.preview();
    const bool tangentPreviewIsValid = tangentPreview.points.size() == 1 &&
        std::abs(QPointF::dotProduct(tangentPreview.points.first(),
                                     tangentPreview.points.first()) - 100.0) <= 1.0e-4 &&
        std::abs(QPointF::dotProduct(tangentPreview.points.first(),
                                     tangentEndpoint - tangentPreview.points.first())) <= 1.0e-4;
    const bool tangentLineCommitted = tangentTool.handleMousePress(tangentCursor,
                                                                   toolContext);
    passed &= check(tangentCurvePicked &&
                        toolSelection.primaryObjectId() == tangentCircleId &&
                        tangentTargetId.isValid() &&
                        tangentUnavailableInside &&
                        tangentEndpointSnap.type == SnapType::Endpoint &&
                        tangentEndpointSnap.point == tangentEndpoint &&
                        tangentPreviewIsValid &&
                        tangentLineCommitted &&
                        committedToolShapes.size() == 3 &&
                        committedToolShapes.back().geometryType == GeometryType::Line &&
                        committedToolShapes.back().points.first() == tangentPreview.points.first() &&
                        committedToolShapes.back().points.back() == tangentEndpoint &&
                        validateNurbsCurve(committedToolShapes.back().nurbs) &&
                        finishedTool == ToolId::Select,
                    "tangent-from-curve tool must preview and commit a tangent line from a selected curve");

    const ObjectId archObjectId = toolDocument.append(archShape);
    QPointF archPickPoint;
    const bool archPickPointEvaluated = evaluateNurbsPoint(archCurve,
                                                           0.25,
                                                           &archPickPoint);
    TangentFromCurveTool freeformTangentTool;
    freeformTangentTool.begin(toolContext);
    const bool freeformCurvePicked = archPickPointEvaluated &&
        freeformTangentTool.handleMousePress(tangentInputAt(archPickPoint), toolContext);
    const ToolInput archEndpointInput = tangentInputAt(archOrigin);
    freeformTangentTool.handleMouseMove(archEndpointInput, toolContext);
    const ToolPreview freeformPreview = freeformTangentTool.preview();
    bool committedFreeformTangentIsValid = freeformPreview.points.size() == 1;
    if (committedFreeformTangentIsValid) {
        qreal low = archStartParameter;
        qreal high = archEndParameter;
        for (int iteration = 0; iteration < 64; ++iteration) {
            const qreal middle = (low + high) * 0.5;
            QPointF point;
            evaluateNurbsPoint(archCurve, middle, &point);
            if (point.x() < freeformPreview.points.first().x()) {
                low = middle;
            } else {
                high = middle;
            }
        }
        const qreal parameter = (low + high) * 0.5;
        QPointF point;
        QPointF derivative;
        if (!evaluateNurbsPoint(archCurve, parameter, &point) ||
            !evaluateNurbsDerivative(archCurve, parameter, &derivative)) {
            committedFreeformTangentIsValid = false;
        } else {
            const QPointF chord = archOrigin - point;
            const qreal denominator = std::hypot(chord.x(), chord.y()) *
                                      std::hypot(derivative.x(), derivative.y());
            committedFreeformTangentIsValid = denominator > 1.0e-12 &&
                std::abs(crossProduct(chord, derivative)) / denominator <= 1.0e-7;
        }
    }
    const bool freeformLineCommitted = freeformTangentTool.handleMousePress(
        archEndpointInput, toolContext);
    passed &= check(archObjectId.isValid() && freeformCurvePicked &&
                        toolSelection.primaryObjectId() == archObjectId,
                    "tangent tool must select the requested freeform curve");
    passed &= check(committedFreeformTangentIsValid,
                    "freeform tangent preview must align with the curve derivative");
    passed &= check(freeformLineCommitted && committedToolShapes.size() == 4 &&
                        committedToolShapes.back().geometryType == GeometryType::Line &&
                        committedToolShapes.back().points.first() ==
                            freeformPreview.points.first() &&
                        committedToolShapes.back().points.back() == archOrigin &&
                        validateNurbsCurve(committedToolShapes.back().nurbs),
                    "tangent tool must commit a line aligned with a freeform curve derivative");

    PerpendicularFromCurveTool perpendicularTool;
    perpendicularTool.begin(toolContext);
    const bool perpendicularCurvePicked = perpendicularTool.handleMousePress(
        tangentInputAt(QPointF(10.0, 0.0)), toolContext);
    const QPointF perpendicularEndpoint(20.0, 7.0);
    const ToolInput perpendicularInput = tangentInputAt(perpendicularEndpoint);
    perpendicularTool.handleMouseMove(perpendicularInput, toolContext);
    const ToolPreview perpendicularPreview = perpendicularTool.preview();
    bool perpendicularPreviewIsValid = perpendicularPreview.points.size() == 1;
    if (perpendicularPreviewIsValid) {
        const QPointF foot = perpendicularPreview.points.first();
        const QPointF radial = foot;
        const QPointF lineDirection = perpendicularEndpoint - foot;
        const qreal denominator = std::hypot(radial.x(), radial.y()) *
                                  std::hypot(lineDirection.x(), lineDirection.y());
        perpendicularPreviewIsValid = denominator > 1.0e-12 &&
            std::abs(crossProduct(radial, lineDirection)) / denominator <= 1.0e-7 &&
            std::abs(std::hypot(radial.x(), radial.y()) - 10.0) <= 1.0e-7;
    }
    const bool perpendicularLineCommitted = perpendicularTool.handleMousePress(
        perpendicularInput, toolContext);
    passed &= check(perpendicularCurvePicked &&
                        toolSelection.primaryObjectId() == tangentCircleId,
                    "perpendicular-from-curve tool must select the requested curve");
    passed &= check(perpendicularPreviewIsValid,
                    "perpendicular-from-curve tool must preview a normal line from the selected curve");
    passed &= check(perpendicularLineCommitted && committedToolShapes.size() == 5 &&
                        committedToolShapes.back().geometryType == GeometryType::Line &&
                        committedToolShapes.back().points.first() ==
                            perpendicularPreview.points.first() &&
                        committedToolShapes.back().points.back() == perpendicularEndpoint &&
                        validateNurbsCurve(committedToolShapes.back().nurbs) &&
                        finishedTool == ToolId::Select,
                    "perpendicular-from-curve tool must commit the previewed line as NURBS geometry");

    return passed ? 0 : 1;
}
