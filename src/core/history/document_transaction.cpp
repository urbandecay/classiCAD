#include "document_transaction.h"

#include <QSet>

#include <utility>

namespace classiCAD {

DocumentTransaction::DocumentTransaction(Document &document, History &history)
    : document_(document),
      history_(history),
      before_(document.snapshot())
{
}

DocumentTransaction::~DocumentTransaction()
{
    if (!finished_ && !changes_.isEmpty()) {
        rollback();
    }
}

Shape *DocumentTransaction::editGeometry(ObjectId objectId)
{
    Shape *shape = document_.mutableShape(objectId);
    if (shape == nullptr) {
        return nullptr;
    }
    changes_.addObject(objectId);
    changes_.geometryChanged = true;
    return shape;
}

ObjectId DocumentTransaction::addShape(const Shape &shape)
{
    SceneObject object;
    object.geometry = shape;
    return insertObject(document_.size(), object);
}

ObjectId DocumentTransaction::insertObject(int index, const SceneObject &source)
{
    SceneObject object = source;
    const ObjectId objectId = document_.insertObject(index, object);
    if (objectId.isValid()) {
        changes_.addObject(objectId);
        changes_.geometryChanged = true;
        changes_.structureChanged = true;
        const SceneObject *sceneObject = document_.object(objectId);
        if (sceneObject != nullptr) {
            changes_.addLayer(sceneObject->layerId);
        }
    }
    return objectId;
}

QVector<ObjectId> DocumentTransaction::insertObjects(
    int index, const QVector<SceneObject> &objects)
{
    QVector<ObjectId> objectIds = document_.insertObjects(index, objects);
    if (objectIds.isEmpty()) {
        return objectIds;
    }

    for (const ObjectId objectId : objectIds) {
        changes_.addObject(objectId);
        const SceneObject *sceneObject = document_.object(objectId);
        if (sceneObject != nullptr) {
            changes_.addLayer(sceneObject->layerId);
        }
    }
    changes_.geometryChanged = true;
    changes_.structureChanged = true;
    return objectIds;
}

bool DocumentTransaction::replaceGeometry(ObjectId objectId, const Shape &shape)
{
    if (!document_.replace(objectId, shape)) {
        return false;
    }
    changes_.addObject(objectId);
    changes_.geometryChanged = true;
    return true;
}

bool DocumentTransaction::setObjectPlacementTranslation(
    ObjectId objectId, const Point3D &translation)
{
    if (!document_.setObjectPlacementTranslation(objectId, translation)) {
        return false;
    }
    changes_.addObject(objectId);
    changes_.placementChanged = true;
    return true;
}

bool DocumentTransaction::translateObjects(const QVector<ObjectId> &objectIds,
                                           const Point3D &worldDelta)
{
    if (!document_.translateObjects(objectIds, worldDelta)) {
        return false;
    }
    for (const ObjectId objectId : objectIds) {
        if (document_.object(objectId) != nullptr) {
            changes_.addObject(objectId);
        }
    }
    changes_.placementChanged = true;
    return true;
}

bool DocumentTransaction::removeObject(ObjectId objectId)
{
    const SceneObject *sceneObject = document_.object(objectId);
    if (sceneObject == nullptr) {
        return false;
    }
    const LayerId oldLayerId = sceneObject->layerId;
    if (!document_.remove(objectId)) {
        return false;
    }
    changes_.addObject(objectId);
    changes_.addLayer(oldLayerId);
    changes_.geometryChanged = true;
    changes_.structureChanged = true;
    return true;
}

QVector<ObjectId> DocumentTransaction::removeObjects(
    const QVector<ObjectId> &objectIds)
{
    QSet<quint64> seenLayerIds;
    seenLayerIds.reserve(objectIds.size());
    QVector<LayerId> oldLayerIds;
    oldLayerIds.reserve(objectIds.size());
    for (const ObjectId objectId : objectIds) {
        const SceneObject *sceneObject = document_.object(objectId);
        if (sceneObject != nullptr && sceneObject->layerId.isValid() &&
            !seenLayerIds.contains(sceneObject->layerId.value())) {
            seenLayerIds.insert(sceneObject->layerId.value());
            oldLayerIds.append(sceneObject->layerId);
        }
    }

    QVector<ObjectId> removedIds = document_.removeObjects(objectIds);
    if (removedIds.isEmpty()) {
        return removedIds;
    }
    for (const ObjectId objectId : removedIds) {
        changes_.addObject(objectId);
    }
    for (const LayerId layerId : oldLayerIds) {
        changes_.addLayer(layerId);
    }
    changes_.geometryChanged = true;
    changes_.structureChanged = true;
    return removedIds;
}

bool DocumentTransaction::moveObjectToLayer(ObjectId objectId, LayerId layerId)
{
    const SceneObject *sceneObject = document_.object(objectId);
    if (sceneObject == nullptr) {
        return false;
    }
    const LayerId oldLayerId = sceneObject->layerId;
    if (!document_.moveObjectToLayer(objectId, layerId)) {
        return false;
    }
    changes_.addObject(objectId);
    changes_.addLayer(oldLayerId);
    changes_.addLayer(layerId);
    changes_.layerPropertiesChanged = true;
    return true;
}

bool DocumentTransaction::editLayer(LayerId layerId,
                                    const std::function<void(Layer &)> &edit,
                                    bool visibilityChanged)
{
    Layer *layer = document_.mutableLayer(layerId);
    if (layer == nullptr || !edit) {
        return false;
    }
    edit(*layer);
    changes_.addLayer(layerId);
    changes_.layerPropertiesChanged = true;
    changes_.visibilityChanged = changes_.visibilityChanged || visibilityChanged;
    return true;
}

bool DocumentTransaction::setSettings(const DocumentSettings &settings)
{
    if (!document_.setSettings(settings)) {
        return false;
    }
    changes_.settingsChanged = true;
    return true;
}

void DocumentTransaction::replaceDocument(const Document::Snapshot &snapshot)
{
    document_.restoreSnapshot(snapshot);
    changes_.geometryChanged = true;
    changes_.structureChanged = true;
    changes_.layerPropertiesChanged = true;
    changes_.visibilityChanged = true;
    changes_.settingsChanged = true;
    for (const SceneObject &object : snapshot.objects) {
        changes_.addObject(object.id);
    }
    for (const Layer &layer : snapshot.layers) {
        changes_.addLayer(layer.id);
    }
}

void DocumentTransaction::replaceObjects(const QVector<SceneObject> &objects)
{
    document_.replaceObjects(objects);
    changes_.geometryChanged = true;
    changes_.structureChanged = true;
    for (const SceneObject &object : document_.objects()) {
        changes_.addObject(object.id);
        changes_.addLayer(object.layerId);
    }
}

void DocumentTransaction::markAllGeometryChanged()
{
    changes_.geometryChanged = true;
    for (const SceneObject &object : document_.objects()) {
        changes_.addObject(object.id);
    }
}

void DocumentTransaction::markSelectionChanged()
{
    changes_.selectionChanged = true;
}

bool DocumentTransaction::commit()
{
    if (finished_ || changes_.isEmpty()) {
        return false;
    }
    if (changes_.affectsPersistentDocument()) {
        history_.record(before_);
        document_.applyChanges(changes_);
    }
    finished_ = true;
    return true;
}

void DocumentTransaction::rollback()
{
    if (finished_) {
        return;
    }
    document_.restoreSnapshot(before_);
    finished_ = true;
}

const DocumentChangeSet &DocumentTransaction::changes() const
{
    return changes_;
}

} // namespace classiCAD
