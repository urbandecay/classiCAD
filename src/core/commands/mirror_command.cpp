#include "mirror_command.h"

#include "core/commands/transform_command.h"
#include "core/geometry/geometry_transform.h"

#include <cmath>
#include <utility>

namespace classiCAD {

bool MirrorCommand::apply(const Document &document,
                          DocumentTransaction &transaction,
                          const QVector<ObjectId> &sourceObjectIds,
                          const QPointF &axisStart,
                          const QPointF &axisEnd,
                          QVector<ObjectId> *createdObjectIds)
{
    QVector<SceneObject> mirroredObjects;
    mirroredObjects.reserve(sourceObjectIds.size());
    for (const ObjectId objectId : sourceObjectIds) {
        const SceneObject *sourceObject = document.object(objectId);
        if (sourceObject == nullptr || !document.isObjectEditable(objectId)) {
            continue;
        }

        Shape sourceGeometry = sourceObject->geometry;
        if ((sourceObject->placementTranslation.x != 0.0 ||
             sourceObject->placementTranslation.y != 0.0 ||
             sourceObject->placementTranslation.z != 0.0) &&
            !bakeShapePlacementTranslation(&sourceGeometry,
                                           sourceObject->placementTranslation)) {
            continue;
        }
        Shape mirroredShape;
        if (!mirrorShapeAcrossLine(sourceGeometry,
                                   axisStart,
                                   axisEnd,
                                   &mirroredShape)) {
            continue;
        }

        SceneObject mirroredObject = *sourceObject;
        mirroredObject.id = ObjectId::invalid();
        // Placement was baked into the geometry before mirroring, so the
        // derived object must start with no remaining object translation.
        mirroredObject.placementTranslation = {};
        mirroredObject.geometry = std::move(mirroredShape);
        mirroredObjects.append(std::move(mirroredObject));
    }

    if (mirroredObjects.isEmpty()) {
        if (createdObjectIds != nullptr) {
            createdObjectIds->clear();
        }
        return false;
    }

    QVector<ObjectId> createdIds;
    createdIds.reserve(mirroredObjects.size());
    for (const SceneObject &mirroredObject : mirroredObjects) {
        createdIds.append(transaction.insertObject(document.size(), mirroredObject));
    }
    if (createdObjectIds != nullptr) {
        *createdObjectIds = std::move(createdIds);
    }
    return true;
}

bool MirrorCommand::applyControlPoints(
    Document &document,
    DocumentTransaction &transaction,
    const QVector<ObjectId> &sourceObjectIds,
    const QHash<quint64, QSet<int>> &controlPointIndices,
    const QPointF &axisStart,
    const QPointF &axisEnd,
    const WorkPlaneFrame &axisFrame,
    int *editedObjectCount)
{
    if (editedObjectCount != nullptr) {
        *editedObjectCount = 0;
    }
    const QPointF axis = axisEnd - axisStart;
    if (controlPointIndices.isEmpty() ||
        std::hypot(axis.x(), axis.y()) <= 1.0e-12 ||
        !isValidWorkPlaneFrame(axisFrame)) {
        return false;
    }

    return TransformCommand::applyPerObject(
        document, transaction, sourceObjectIds,
        [&controlPointIndices, &axisStart, &axisEnd, &axisFrame](
            ObjectId objectId, Shape &shape) {
            const auto targets = controlPointIndices.constFind(objectId.value());
            if (targets == controlPointIndices.cend() || targets->isEmpty()) {
                return;
            }
            transformShapeControlPoints(
                &shape, targets.value(),
                [&axisStart, &axisEnd, &axisFrame](const Point3D &point) {
                    return mirrorPointAcrossWorkPlaneLine(
                        point, axisStart, axisEnd, axisFrame);
                });
        },
        editedObjectCount);
}

} // namespace classiCAD
