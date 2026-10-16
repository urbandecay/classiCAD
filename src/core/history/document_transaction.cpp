#include "document_transaction.h"

#include <algorithm>

#include <cmath>
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

bool DocumentTransaction::setObjectAppearance(
    ObjectId objectId,
    const QVector4D &displayColorSceneLinear,
    const WorkbenchMaterialData &workbenchMaterial)
{
    SceneObject *object = document_.mutableObject(objectId);
    if (object == nullptr || !document_.isObjectEditable(objectId)) {
        return false;
    }

    const auto unitInterval = [](float value) {
        return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
    };
    if (!unitInterval(displayColorSceneLinear.x()) ||
        !unitInterval(displayColorSceneLinear.y()) ||
        !unitInterval(displayColorSceneLinear.z()) ||
        !unitInterval(displayColorSceneLinear.w()) ||
        !unitInterval(workbenchMaterial.baseColorSceneLinear.x()) ||
        !unitInterval(workbenchMaterial.baseColorSceneLinear.y()) ||
        !unitInterval(workbenchMaterial.baseColorSceneLinear.z()) ||
        !std::isfinite(workbenchMaterial.roughness) ||
        workbenchMaterial.roughness < 0.0 || workbenchMaterial.roughness > 1.0 ||
        !std::isfinite(workbenchMaterial.metallic) ||
        workbenchMaterial.metallic < 0.0 || workbenchMaterial.metallic > 1.0 ||
        !std::isfinite(workbenchMaterial.alpha) ||
        workbenchMaterial.alpha < 0.0 || workbenchMaterial.alpha > 1.0) {
        return false;
    }

    const WorkbenchMaterialData &currentMaterial = object->workbenchMaterial;
    const bool sameDisplayColor =
        object->displayColorSceneLinear == displayColorSceneLinear;
    const bool sameMaterial =
        object->hasWorkbenchMaterial &&
        currentMaterial.baseColorSceneLinear ==
            workbenchMaterial.baseColorSceneLinear &&
        currentMaterial.roughness == workbenchMaterial.roughness &&
        currentMaterial.metallic == workbenchMaterial.metallic &&
        currentMaterial.alpha == workbenchMaterial.alpha;
    if (sameDisplayColor && sameMaterial) {
        return false;
    }

    object->displayColorSceneLinear = displayColorSceneLinear;
    object->workbenchMaterial = workbenchMaterial;
    object->hasWorkbenchMaterial = true;
    changes_.addObject(objectId);
    changes_.objectAppearanceChanged = true;
    return true;
}

bool DocumentTransaction::addMaterial(const DocumentMaterial &material)
{
    const bool alreadyPresent = std::any_of(
        document_.materials().cbegin(), document_.materials().cend(),
        [&material](const DocumentMaterial &existing) {
            return existing.id == material.id;
        });
    if (!document_.addMaterial(material)) return false;
    if (!alreadyPresent) changes_.materialsChanged = true;
    return true;
}

CollectionId DocumentTransaction::insertCollection(
    const DocumentCollection &collection)
{
    const CollectionId existingId = collection.sourceId.isEmpty()
        ? CollectionId::invalid()
        : [&]() {
              for (const DocumentCollection &candidate : document_.collections()) {
                  if (candidate.sourceId == collection.sourceId &&
                      candidate.libraryPath == collection.libraryPath) {
                      return candidate.id;
                  }
              }
              return CollectionId::invalid();
          }();
    if (existingId.isValid()) return existingId;

    const CollectionId id = document_.insertCollection(collection);
    if (!id.isValid()) return id;
    changes_.addCollection(id);
    changes_.collectionsChanged = true;
    changes_.structureChanged = true;
    for (const CollectionId parentId : collection.parentIds) {
        changes_.addCollection(parentId);
    }
    if (collection.hideViewport) changes_.visibilityChanged = true;
    return id;
}

bool DocumentTransaction::setCollectionVisibility(CollectionId collectionId,
                                                   bool hideViewport,
                                                   bool hideRender)
{
    if (!document_.setCollectionVisibility(collectionId, hideViewport,
                                           hideRender)) {
        return false;
    }
    changes_.addCollection(collectionId);
    changes_.collectionsChanged = true;
    changes_.visibilityChanged = true;
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
    if (!document_.remove(objectId)) {
        return false;
    }
    changes_.addObject(objectId);
    changes_.geometryChanged = true;
    changes_.structureChanged = true;
    return true;
}

QVector<ObjectId> DocumentTransaction::removeObjects(
    const QVector<ObjectId> &objectIds)
{
    QVector<ObjectId> removedIds = document_.removeObjects(objectIds);
    if (removedIds.isEmpty()) {
        return removedIds;
    }
    for (const ObjectId objectId : removedIds) {
        changes_.addObject(objectId);
    }
    changes_.geometryChanged = true;
    changes_.structureChanged = true;
    return removedIds;
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
    changes_.visibilityChanged = true;
    changes_.settingsChanged = true;
    changes_.materialsChanged = true;
    for (const SceneObject &object : snapshot.objects) {
        changes_.addObject(object.id);
    }
}

void DocumentTransaction::replaceObjects(const QVector<SceneObject> &objects)
{
    document_.replaceObjects(objects);
    changes_.geometryChanged = true;
    changes_.structureChanged = true;
    for (const SceneObject &object : document_.objects()) {
        changes_.addObject(object.id);
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
