#include "mirror_command.h"

#include "core/geometry/geometry_transform.h"

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

        SceneObject mirroredObject;
        mirroredObject.layerId = sourceObject->layerId;
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

} // namespace classiCAD
