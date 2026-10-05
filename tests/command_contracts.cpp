#include "core/commands/delete_command.h"
#include "core/commands/duplicate_command.h"
#include "core/commands/explode_command.h"
#include "core/commands/fill_command.h"
#include "core/commands/join_command.h"
#include "core/document/document.h"
#include "core/document/selection_model.h"
#include "core/geometry/curve_construction.h"
#include "core/geometry/nurbs_surface.h"
#include "core/history/document_transaction.h"
#include "core/history/history.h"

#include <QDebug>

#include <cmath>

using namespace classiCAD;

namespace {

bool check(bool condition, const char *message)
{
    if (!condition) {
        qCritical() << message;
    }
    return condition;
}

Shape lineShape(const QVector<QPointF> &points)
{
    Shape shape;
    shape.geometryType = GeometryType::Line;
    shape.workPlaneFrame = makeWorkPlaneFrame(WorkPlane::XY);
    shape.points = points;
    shape.nurbs = makeDegreeOneNurbs(points);
    return shape;
}

Shape rectangleShape()
{
    Shape shape;
    shape.geometryType = GeometryType::Rectangle;
    shape.workPlaneFrame = makeWorkPlaneFrame(WorkPlane::XY);
    shape.points = {{0.0, 0.0}, {3.0, 2.0}};
    shape.nurbs = makeDegreeOneNurbs(
        {{0.0, 0.0}, {3.0, 0.0}, {3.0, 2.0}, {0.0, 2.0}, {0.0, 0.0}});
    return shape;
}

bool deleteContracts()
{
    Document document;
    const ObjectId editableId = document.append(lineShape({{0, 0}, {1, 0}}));
    const ObjectId lockedId = document.append(lineShape({{0, 1}, {1, 1}}));
    const LayerId lockedLayer = document.createLayer(QStringLiteral("Locked"));
    bool valid = document.moveObjectToLayer(lockedId, lockedLayer) &&
                 document.setLayerLocked(lockedLayer, true);
    History history(document);

    DocumentTransaction transaction(document, history);
    int deletedCount = -1;
    const bool applied = DeleteCommand::apply(
        document,
        transaction,
        {editableId, editableId, lockedId, ObjectId::invalid(),
         ObjectId::fromValue(999999)},
        &deletedCount);
    valid = valid && applied && deletedCount == 1 && transaction.commit() &&
            document.size() == 1 && document.shape(lockedId) != nullptr &&
            history.undoCount() == 1;
    valid = valid && history.undo() && document.size() == 2 &&
            document.indexOf(editableId) == 0 &&
            document.object(lockedId)->layerId == lockedLayer;

    const int historyCountBeforeNoOp = history.undoCount();
    DocumentTransaction noOpTransaction(document, history);
    deletedCount = -1;
    const bool noOpApplied = DeleteCommand::apply(
        document, noOpTransaction, {lockedId, ObjectId::invalid()},
        &deletedCount);
    valid = valid && !noOpApplied && deletedCount == 0 &&
            !noOpTransaction.commit() &&
            history.undoCount() == historyCountBeforeNoOp &&
            document.shape(lockedId) != nullptr;
    return check(valid,
                 "Delete must deduplicate IDs, skip locked/stale targets, and keep no-ops out of history");
}

bool batchDocumentMutationContracts()
{
    Document document;
    const LayerId defaultLayer = document.activeLayerId();
    const LayerId secondaryLayer = document.createLayer(QStringLiteral("Batch"));

    QVector<SceneObject> initialObjects;
    initialObjects.reserve(3);
    SceneObject first;
    first.layerId = defaultLayer;
    first.geometry = lineShape({{0, 0}, {1, 0}});
    initialObjects.append(first);
    SceneObject second;
    second.layerId = secondaryLayer;
    second.geometry = lineShape({{0, 1}, {1, 1}});
    initialObjects.append(second);
    SceneObject third;
    third.layerId = defaultLayer;
    third.geometry = lineShape({{0, 2}, {1, 2}});
    initialObjects.append(third);

    const QVector<ObjectId> initialIds = document.insertObjects(0, initialObjects);
    bool valid = initialIds.size() == 3 && document.size() == 3 &&
                 document.objectIdAt(0) == initialIds.value(0) &&
                 document.objectIdAt(1) == initialIds.value(1) &&
                 document.objectIdAt(2) == initialIds.value(2) &&
                 document.layer(defaultLayer)->objectIds ==
                     QVector<ObjectId>{initialIds[0], initialIds[2]} &&
                 document.layer(secondaryLayer)->objectIds ==
                     QVector<ObjectId>{initialIds[1]};

    // Existing and repeated incoming IDs must each be replaced with a unique ID.
    SceneObject sameIdFirst = *document.object(initialIds[0]);
    SceneObject sameIdSecond = sameIdFirst;
    const QVector<ObjectId> collisionIds = document.insertObjects(
        1, {sameIdFirst, sameIdSecond});
    valid = valid && collisionIds.size() == 2 &&
            collisionIds[0].isValid() && collisionIds[1].isValid() &&
            collisionIds[0] != collisionIds[1] &&
            collisionIds[0] != initialIds[0] &&
            document.objectIdAt(0) == initialIds[0] &&
            document.objectIdAt(1) == collisionIds[0] &&
            document.objectIdAt(2) == collisionIds[1] &&
            document.layer(defaultLayer)->objectIds ==
                QVector<ObjectId>{initialIds[0], collisionIds[0],
                                  collisionIds[1], initialIds[2]};

    const ObjectId staleId = ObjectId::fromValue(999999);
    const QVector<ObjectId> removedIds = document.removeObjects(
        {initialIds[1], collisionIds[0], initialIds[1],
         ObjectId::invalid(), staleId});
    valid = valid && removedIds == QVector<ObjectId>{collisionIds[0], initialIds[1]} &&
            document.size() == 3 && document.objectIdAt(0) == initialIds[0] &&
            document.objectIdAt(1) == collisionIds[1] &&
            document.objectIdAt(2) == initialIds[2] &&
            document.indexOf(initialIds[1]) == -1 &&
            document.layer(defaultLayer)->objectIds ==
                QVector<ObjectId>{initialIds[0], collisionIds[1], initialIds[2]} &&
            document.layer(secondaryLayer)->objectIds.isEmpty();

    Document transactionDocument;
    const ObjectId anchorId = transactionDocument.append(
        lineShape({{-1, 0}, {1, 0}}));
    History history(transactionDocument);
    QVector<SceneObject> transactionObjects;
    for (int index = 0; index < 3; ++index) {
        SceneObject object;
        object.layerId = transactionDocument.activeLayerId();
        object.geometry = lineShape({{0, double(index)}, {2, double(index)}});
        transactionObjects.append(object);
    }

    QVector<ObjectId> transactionIds;
    {
        DocumentTransaction transaction(transactionDocument, history);
        transactionIds = transaction.insertObjects(1, transactionObjects);
        valid = valid && transactionIds.size() == 3 && transaction.commit();
    }
    valid = valid && transactionDocument.size() == 4 &&
            transactionDocument.objectIdAt(0) == anchorId &&
            transactionDocument.objectIdAt(1) == transactionIds.value(0) &&
            history.undoCount() == 1 && history.undo() &&
            transactionDocument.size() == 1 &&
            transactionDocument.objectIdAt(0) == anchorId && history.redo() &&
            transactionDocument.size() == 4 &&
            transactionDocument.objectIdAt(3) == transactionIds.value(2);

    {
        DocumentTransaction transaction(transactionDocument, history);
        const QVector<ObjectId> removed = transaction.removeObjects(
            {transactionIds[0], transactionIds[0], staleId});
        valid = valid && removed == QVector<ObjectId>{transactionIds[0]};
        transaction.rollback();
    }
    valid = valid && transactionDocument.size() == 4 &&
            transactionDocument.object(transactionIds[0]) != nullptr;
    {
        DocumentTransaction transaction(transactionDocument, history);
        valid = valid && transaction.removeObjects(
                            {transactionIds[0], transactionIds[1],
                             transactionIds[2]}).size() == 3 &&
                transaction.commit();
    }
    valid = valid && transactionDocument.size() == 1 && history.undo() &&
            transactionDocument.size() == 4;

    return check(valid,
                 "Batch insert/remove must preserve IDs, ordering, layers, rollback, and Undo/Redo");
}

bool duplicateContracts()
{
    Document document;
    const LayerId sourceLayer = document.createLayer(QStringLiteral("Source"));
    const ObjectId sourceId = document.append(lineShape({{2, 3}, {7, 3}}));
    bool valid = document.moveObjectToLayer(sourceId, sourceLayer);
    History history(document);
    DuplicateCommandPlan plan;
    const Shape duplicateGeometry = lineShape({{2, 5}, {7, 5}});
    valid = valid && buildDuplicateCommandPlan(
                         document, {sourceId}, {duplicateGeometry}, &plan) &&
            plan.sourceObjectIds == QVector<ObjectId>{sourceId} &&
            plan.duplicateObjects.size() == 1 &&
            plan.duplicateObjects.first().layerId == sourceLayer;

    DocumentTransaction transaction(document, history);
    QVector<ObjectId> duplicateIds;
    valid = valid && applyDuplicateCommand(document, transaction, plan,
                                           &duplicateIds) &&
            duplicateIds.size() == 1 && duplicateIds.first() != sourceId &&
            transaction.commit() && document.size() == 2 &&
            document.object(duplicateIds.first())->layerId == sourceLayer &&
            document.shape(sourceId)->points == QVector<QPointF>{{2, 3}, {7, 3}} &&
            document.shape(duplicateIds.first())->points ==
                QVector<QPointF>{{2, 5}, {7, 5}};
    valid = valid && history.undo() && document.size() == 1 &&
            document.indexOf(sourceId) == 0;

    Shape invalidGeometry = duplicateGeometry;
    invalidGeometry.nurbs.order = 3;
    DuplicateCommandPlan rejectedPlan;
    valid = valid && !buildDuplicateCommandPlan(
                         document, {sourceId}, {invalidGeometry}, &rejectedPlan) &&
            rejectedPlan.duplicateObjects.isEmpty();
    valid = valid && document.setLayerLocked(sourceLayer, true) &&
            !buildDuplicateCommandPlan(document, {sourceId},
                                       {duplicateGeometry}, &rejectedPlan);
    return check(valid,
                 "Duplicate must validate geometry and preserve source layer and identity");
}

bool fillContracts()
{
    Document document;
    const Shape rectangle = rectangleShape();
    const ObjectId rectangleId = document.append(rectangle);
    History history(document);
    FillCommandPlan plan;
    bool valid = buildFillCommandPlan(document, {rectangleId}, &plan) &&
                 plan.candidates.size() == 1 &&
                 validateNurbsSurface(plan.candidates.first().surface);
    DocumentTransaction transaction(document, history);
    int filledCount = -1;
    valid = valid && applyFillCommand(transaction, plan, &filledCount) &&
            filledCount == 1 && transaction.commit() &&
            document.shape(rectangleId)->geometryType == GeometryType::NurbsSurface &&
            validateNurbsSurface(document.shape(rectangleId)->nurbsSurface) &&
            history.undoCount() == 1;
    valid = valid && history.undo() &&
            document.shape(rectangleId)->geometryType == GeometryType::Rectangle &&
            validateNurbsCurve(document.shape(rectangleId)->nurbs);

    const LayerId lockedLayer = document.createLayer(QStringLiteral("Locked"));
    valid = valid && document.moveObjectToLayer(rectangleId, lockedLayer) &&
            document.setLayerLocked(lockedLayer, true);
    FillCommandPlan lockedPlan;
    valid = valid && buildFillCommandPlan(document, {rectangleId}, &lockedPlan) &&
            lockedPlan.candidates.isEmpty();
    DocumentTransaction noOpTransaction(document, history);
    filledCount = -1;
    valid = valid && !applyFillCommand(noOpTransaction, lockedPlan, &filledCount) &&
            filledCount == 0 && !noOpTransaction.commit() && history.undoCount() == 0;
    return check(valid,
                 "Fill must preserve the source ID through an exact surface edit and skip locked geometry");
}

bool explodeContracts()
{
    Document document;
    const Shape rectangle = rectangleShape();
    const ObjectId rectangleId = document.append(rectangle);
    const LayerId sourceLayer = document.createLayer(QStringLiteral("Edges"));
    bool valid = document.moveObjectToLayer(rectangleId, sourceLayer);
    History history(document);
    ExplodeCommandPlan plan;
    valid = valid && buildExplodeCommandPlan(document, {rectangleId}, &plan) &&
            plan.sourceObjectCount == 1 && plan.outputComponentCount == 4 &&
            plan.selectedObjectIndices.size() == 4;
    DocumentTransaction transaction(document, history);
    valid = valid && applyExplodeCommand(transaction, plan) &&
            transaction.commit() && document.size() == 4 &&
            history.undoCount() == 1;
    for (const SceneObject &object : document.objects()) {
        valid = valid && object.layerId == sourceLayer &&
                object.geometry.geometryType == GeometryType::Line &&
                validateNurbsCurve(object.geometry.nurbs) &&
                object.geometry.nurbs.controlPoints.size() == 2;
    }
    valid = valid && history.undo() && document.size() == 1 &&
            document.object(rectangleId) != nullptr &&
            document.object(rectangleId)->layerId == sourceLayer;

    ExplodeCommandPlan noOpPlan;
    valid = valid && buildExplodeCommandPlan(document, {}, &noOpPlan) &&
            noOpPlan.sourceObjectCount == 0 &&
            noOpPlan.outputComponentCount == 0;
    DocumentTransaction noOpTransaction(document, history);
    valid = valid && !applyExplodeCommand(noOpTransaction, noOpPlan) &&
            !noOpTransaction.commit() && history.undoCount() == 0;
    return check(valid,
                 "Explode must create four independent, selected-layer edges and undo as one edit");
}

bool joinCommandContracts()
{
    Document document;
    const LayerId sourceLayer = document.createLayer(QStringLiteral("Curves"));
    const ObjectId firstId = document.append(lineShape({{0, 0}, {1, 0}}));
    const ObjectId secondId = document.append(lineShape({{1, 0}, {1, 1}}));
    bool valid = document.moveObjectToLayer(firstId, sourceLayer) &&
                 document.moveObjectToLayer(secondId, sourceLayer);
    History history(document);
    const WorkPlaneFrame frame = makeWorkPlaneFrame(WorkPlane::XY);
    JoinCommandPlan plan;
    valid = valid && buildJoinCommandPlan(
                         {document.shape(firstId)->nurbs,
                          document.shape(secondId)->nurbs},
                         {frame, frame}, false, frame, WorkPlane::XY, 0.0,
                         sourceLayer, {firstId, secondId}, 0, &plan);
    DocumentTransaction transaction(document, history);
    ObjectId joinedId = ObjectId::invalid();
    valid = valid && applyJoinCommand(document, transaction, plan, &joinedId) &&
            joinedId.isValid() && transaction.commit() && document.size() == 1 &&
            document.object(joinedId)->layerId == sourceLayer &&
            document.shape(joinedId)->geometryType == GeometryType::PolyCurve &&
            document.shape(joinedId)->components.size() == 2 &&
            history.undoCount() == 1;
    valid = valid && history.undo() && document.size() == 2 &&
            document.object(firstId) != nullptr && document.object(secondId) != nullptr;
    return check(valid,
                 "Join command must replace sources atomically and preserve the source layer on Undo");
}

} // namespace

int main()
{
    bool passed = deleteContracts();
    passed = batchDocumentMutationContracts() && passed;
    passed = duplicateContracts() && passed;
    passed = fillContracts() && passed;
    passed = explodeContracts() && passed;
    passed = joinCommandContracts() && passed;
    qInfo() << "Document command contracts:" << (passed ? "passed" : "failed");
    return passed ? 0 : 1;
}
