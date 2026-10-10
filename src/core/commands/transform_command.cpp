#include "transform_command.h"

#include "core/geometry/geometry_transform.h"

namespace classiCAD {

bool TransformCommand::apply(Document &document,
                             DocumentTransaction &transaction,
                             const QVector<ObjectId> &objectIds,
                             const GeometryEdit &edit,
                             int *editedCount)
{
    if (!edit) {
        if (editedCount != nullptr) {
            *editedCount = 0;
        }
        return false;
    }
    return applyPerObject(
        document, transaction, objectIds,
        [&edit](ObjectId, Shape &shape) { edit(shape); }, editedCount);
}

bool TransformCommand::applyPerObject(Document &document,
                                      DocumentTransaction &transaction,
                                      const QVector<ObjectId> &objectIds,
                                      const ObjectGeometryEdit &edit,
                                      int *editedCount)
{
    if (editedCount != nullptr) {
        *editedCount = 0;
    }
    if (!edit) {
        return false;
    }

    int count = 0;
    for (const ObjectId objectId : objectIds) {
        if (!document.isObjectEditable(objectId)) {
            continue;
        }
        const SceneObject *sceneObject = document.object(objectId);
        const Point3D placementTranslation = sceneObject != nullptr
                                                 ? sceneObject->placementTranslation
                                                 : Point3D{};
        Shape *shape = transaction.editGeometry(objectId);
        if (shape == nullptr) {
            continue;
        }
        if (placementTranslation.x != 0.0 || placementTranslation.y != 0.0 ||
            placementTranslation.z != 0.0) {
            if (!bakeShapePlacementTranslation(shape, placementTranslation) ||
                !transaction.setObjectPlacementTranslation(objectId, {})) {
                continue;
            }
        }
        edit(objectId, *shape);
        ++count;
    }
    if (editedCount != nullptr) {
        *editedCount = count;
    }
    return count > 0;
}

} // namespace classiCAD
