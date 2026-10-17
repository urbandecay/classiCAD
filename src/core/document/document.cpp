#include "document.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace classiCAD {

bool isValidDocumentMaterial(const DocumentMaterial &material)
{
    if (material.id.isEmpty() || material.baseColorSceneLinear.size() != 4 ||
        !std::isfinite(material.roughness) || material.roughness < 0.0f ||
        material.roughness > 1.0f || !std::isfinite(material.metallic) ||
        material.metallic < 0.0f || material.metallic > 1.0f ||
        !std::isfinite(material.alpha) || material.alpha < 0.0f ||
        material.alpha > 1.0f) {
        return false;
    }
    for (const float component : material.baseColorSceneLinear) {
        if (!std::isfinite(component) || component < 0.0f || component > 1.0f) {
            return false;
        }
    }
    return true;
}

Document::Document() = default;

int Document::size() const
{
    return objects_.size();
}

bool Document::isEmpty() const
{
    return objects_.isEmpty();
}

const QVector<SceneObject> &Document::objects() const
{
    return objects_;
}

const QVector<DocumentMaterial> &Document::materials() const
{
    return materials_;
}

bool Document::addMaterial(const DocumentMaterial &material)
{
    if (!isValidDocumentMaterial(material)) return false;
    for (const DocumentMaterial &existing : materials_) {
        if (existing.id != material.id) continue;
        return existing.name == material.name &&
               existing.libraryPath == material.libraryPath &&
               existing.baseColorSceneLinear == material.baseColorSceneLinear &&
               existing.roughness == material.roughness &&
               existing.metallic == material.metallic &&
               existing.alpha == material.alpha;
    }
    materials_.append(material);
    return true;
}

const QVector<DocumentCollection> &Document::collections() const
{
    return collections_;
}

const DocumentSettings &Document::settings() const
{
    return settings_;
}

bool Document::setSettings(const DocumentSettings &settings)
{
    if (!isValidDocumentSettings(settings)) {
        return false;
    }
    if (settings_ == settings) {
        return true;
    }
    settings_ = settings;
    bumpRevision(&revisions_.settings);
    bumpRevision(&revisions_.epoch);
    return true;
}

const SceneObject *Document::object(ObjectId id) const
{
    const int index = indexOf(id);
    return index >= 0 ? &objects_[index] : nullptr;
}

const Shape *Document::shape(ObjectId id) const
{
    const SceneObject *sceneObject = object(id);
    return sceneObject != nullptr ? &sceneObject->geometry : nullptr;
}

SceneObject *Document::mutableObject(ObjectId id)
{
    const int index = indexOf(id);
    return index >= 0 ? &objects_[index] : nullptr;
}

Shape *Document::mutableShape(ObjectId id)
{
    SceneObject *sceneObject = mutableObject(id);
    return sceneObject != nullptr ? &sceneObject->geometry : nullptr;
}

ObjectId Document::objectIdAt(int index) const
{
    return index >= 0 && index < objects_.size() ? objects_[index].id
                                                  : ObjectId::invalid();
}

int Document::indexOf(ObjectId id) const
{
    if (!id.isValid()) {
        return -1;
    }
    return objectIndices_.value(id.value(), -1);
}

quint64 Document::objectGeometryRevision(ObjectId id) const
{
    return id.isValid() ? objectGeometryRevisions_.value(id.value(), 0) : 0;
}

const Document::RuntimeRevisions &Document::runtimeRevisions() const
{
    return revisions_;
}

void Document::invalidateAllGeometry()
{
    bumpRevision(&revisions_.geometry);
    bumpRevision(&revisions_.epoch);
}

void Document::applyChanges(const DocumentChangeSet &changes)
{
    if (changes.isEmpty()) {
        return;
    }
    bumpRevision(&revisions_.epoch);
    if (changes.geometryChanged) {
        bumpRevision(&revisions_.geometry);
        for (const ObjectId id : changes.objectIds) {
            if (objectIndices_.contains(id.value())) {
                objectGeometryRevisions_.insert(id.value(), revisionClock_);
                bumpRevision(&revisionClock_);
            }
        }
    }
    if (changes.placementChanged) {
        bumpRevision(&revisions_.placement);
    }
    if (changes.structureChanged) {
        bumpRevision(&revisions_.structure);
    }
    if (changes.collectionsChanged || !changes.collectionIds.isEmpty()) {
        bumpRevision(&revisions_.collection);
    }
    if (changes.visibilityChanged || changes.settingsChanged) {
        bumpRevision(&revisions_.visibility);
    }
    if (changes.settingsChanged) {
        bumpRevision(&revisions_.settings);
    }
}

void Document::replaceWith(const Document &document)
{
    restoreSnapshot(document.snapshot());
}

const Shape &Document::operator[](int index) const
{
    return objects_[index].geometry;
}

bool Document::mutateGeometry(ObjectId id,
                              const std::function<bool(Shape &)> &edit)
{
    Shape *geometry = mutableShape(id);
    if (geometry == nullptr || !edit || !edit(*geometry)) {
        return false;
    }
    noteGeometryChange(id);
    return true;
}

bool Document::setObjectPlacementTranslation(ObjectId id,
                                             const Point3D &translation)
{
    return setObjectPlacementTranslations({id}, {translation});
}

bool Document::setObjectPlacementTranslations(
    const QVector<ObjectId> &ids, const QVector<Point3D> &translations)
{
    if (ids.size() != translations.size()) {
        return false;
    }
    DocumentChangeSet changes;
    for (int index = 0; index < ids.size(); ++index) {
        const ObjectId id = ids[index];
        const Point3D &translation = translations[index];
        SceneObject *sceneObject = mutableObject(id);
        const bool spatialNurbs = sceneObject != nullptr &&
            (sceneObject->geometry.geometryType == GeometryType::NurbsSurface ||
             sceneObject->geometry.geometryType == GeometryType::NurbsSolid);
        if (sceneObject == nullptr || !isObjectEditable(id) ||
            !std::isfinite(translation.x) || !std::isfinite(translation.y) ||
            !std::isfinite(translation.z) ||
            (!spatialNurbs && (translation.x != 0.0 || translation.y != 0.0 ||
                               translation.z != 0.0))) {
            continue;
        }
        const Point3D &current = sceneObject->placementTranslation;
        if (current.x == translation.x && current.y == translation.y &&
            current.z == translation.z) {
            continue;
        }
        sceneObject->placementTranslation = translation;
        changes.addObject(id);
    }
    if (changes.objectIds.isEmpty()) {
        return true;
    }
    changes.placementChanged = true;
    applyChanges(changes);
    return true;
}

bool Document::translateObjects(const QVector<ObjectId> &ids,
                                const Point3D &worldDelta)
{
    if (!std::isfinite(worldDelta.x) || !std::isfinite(worldDelta.y) ||
        !std::isfinite(worldDelta.z) ||
        (worldDelta.x == 0.0 && worldDelta.y == 0.0 && worldDelta.z == 0.0)) {
        return false;
    }

    QSet<quint64> seen;
    seen.reserve(ids.size());
    DocumentChangeSet changes;
    for (const ObjectId id : ids) {
        if (!id.isValid() || seen.contains(id.value()) ||
            !isObjectEditable(id)) {
            continue;
        }
        SceneObject *sceneObject = mutableObject(id);
        if (sceneObject == nullptr ||
            (sceneObject->geometry.geometryType != GeometryType::NurbsSurface &&
             sceneObject->geometry.geometryType != GeometryType::NurbsSolid)) {
            continue;
        }
        Point3D translated{sceneObject->placementTranslation.x + worldDelta.x,
                           sceneObject->placementTranslation.y + worldDelta.y,
                           sceneObject->placementTranslation.z + worldDelta.z};
        if (!std::isfinite(translated.x) || !std::isfinite(translated.y) ||
            !std::isfinite(translated.z)) {
            continue;
        }
        sceneObject->placementTranslation = translated;
        seen.insert(id.value());
        changes.addObject(id);
    }
    if (changes.objectIds.isEmpty()) {
        return false;
    }
    changes.placementChanged = true;
    applyChanges(changes);
    return true;
}

ObjectId Document::append(const Shape &shape)
{
    return insert(objects_.size(), shape);
}

ObjectId Document::insert(int index, const Shape &shape)
{
    SceneObject object;
    object.id = allocateObjectId();
    object.geometry = shape;
    return insertObject(index, object);
}

ObjectId Document::insertObject(int index, SceneObject object)
{
    if (!object.id.isValid() || indexOf(object.id) >= 0) {
        object.id = allocateObjectId();
    } else {
        nextObjectValue_ = std::max(nextObjectValue_, object.id.value() + 1);
    }
    index = std::clamp(index, 0, static_cast<int>(objects_.size()));
    objects_.insert(index, object);
    for (int objectIndex = index; objectIndex < objects_.size(); ++objectIndex) {
        objectIndices_.insert(objects_[objectIndex].id.value(), objectIndex);
    }
    objectGeometryRevisions_.insert(object.id.value(), revisionClock_);
    DocumentChangeSet changes;
    changes.addObject(object.id);
    changes.geometryChanged = true;
    changes.placementChanged = object.placementTranslation.x != 0.0 ||
                               object.placementTranslation.y != 0.0 ||
                               object.placementTranslation.z != 0.0;
    changes.structureChanged = true;
    applyChanges(changes);
    return object.id;
}

QVector<ObjectId> Document::insertObjects(
    int index, const QVector<SceneObject> &sourceObjects)
{
    if (sourceObjects.isEmpty()) {
        return {};
    }

    index = std::clamp(index, 0, static_cast<int>(objects_.size()));

    QVector<SceneObject> insertedObjects;
    insertedObjects.reserve(sourceObjects.size());
    QVector<ObjectId> insertedIds;
    insertedIds.reserve(sourceObjects.size());
    QSet<quint64> batchIds;
    batchIds.reserve(sourceObjects.size());
    bool insertedPlacementChanged = false;

    for (const SceneObject &source : sourceObjects) {
        SceneObject object = source;
        if (!object.id.isValid() || indexOf(object.id) >= 0 ||
            batchIds.contains(object.id.value())) {
            object.id = allocateObjectId();
        } else {
            nextObjectValue_ = std::max(nextObjectValue_, object.id.value() + 1);
        }
        insertedPlacementChanged = insertedPlacementChanged ||
            object.placementTranslation.x != 0.0 ||
            object.placementTranslation.y != 0.0 ||
            object.placementTranslation.z != 0.0;
        batchIds.insert(object.id.value());
        insertedIds.append(object.id);
        insertedObjects.append(std::move(object));
    }

    const int oldSize = objects_.size();
    const int newSize = oldSize + insertedObjects.size();
    objectIndices_.reserve(newSize);
    objectGeometryRevisions_.reserve(newSize);
    if (index == oldSize) {
        objects_.reserve(newSize);
        for (int insertedIndex = 0; insertedIndex < insertedObjects.size();
             ++insertedIndex) {
            objects_.append(std::move(insertedObjects[insertedIndex]));
            objectIndices_.insert(insertedIds[insertedIndex].value(),
                                  oldSize + insertedIndex);
        }
    } else {
        QVector<SceneObject> reorderedObjects;
        reorderedObjects.reserve(newSize);
        for (int objectIndex = 0; objectIndex < index; ++objectIndex) {
            reorderedObjects.append(objects_[objectIndex]);
        }
        for (SceneObject &object : insertedObjects) {
            reorderedObjects.append(std::move(object));
        }
        for (int objectIndex = index; objectIndex < oldSize; ++objectIndex) {
            reorderedObjects.append(objects_[objectIndex]);
        }
        objects_ = std::move(reorderedObjects);
        rebuildObjectIndex();
    }

    for (const ObjectId id : insertedIds) {
        objectGeometryRevisions_.insert(id.value(), revisionClock_);
    }
    DocumentChangeSet changes;
    for (const ObjectId id : insertedIds) {
        changes.addObject(id);
    }
    changes.geometryChanged = true;
    changes.placementChanged = insertedPlacementChanged;
    changes.structureChanged = true;
    applyChanges(changes);
    return insertedIds;
}

bool Document::replace(ObjectId id, const Shape &shape)
{
    SceneObject *sceneObject = mutableObject(id);
    if (sceneObject == nullptr) {
        return false;
    }
    sceneObject->geometry = shape;
    noteGeometryChange(id);
    return true;
}

bool Document::remove(ObjectId id)
{
    const int index = indexOf(id);
    return index >= 0 && removeAt(index);
}

bool Document::removeAt(int index)
{
    if (index < 0 || index >= objects_.size()) {
        return false;
    }
    const ObjectId removedId = objects_[index].id;
    const Point3D removedPlacement = objects_[index].placementTranslation;
    objects_.removeAt(index);
    objectGeometryRevisions_.remove(removedId.value());
    objectIndices_.remove(removedId.value());
    for (int objectIndex = index; objectIndex < objects_.size(); ++objectIndex) {
        objectIndices_.insert(objects_[objectIndex].id.value(), objectIndex);
    }
    DocumentChangeSet changes;
    changes.addObject(removedId);
    changes.geometryChanged = true;
    changes.structureChanged = true;
    changes.placementChanged = removedPlacement.x != 0.0 ||
                               removedPlacement.y != 0.0 ||
                               removedPlacement.z != 0.0;
    applyChanges(changes);
    return true;
}

QVector<ObjectId> Document::removeObjects(const QVector<ObjectId> &objectIds)
{
    if (objectIds.isEmpty() || objects_.isEmpty()) {
        return {};
    }

    QSet<quint64> idsToRemove;
    idsToRemove.reserve(objectIds.size());
    for (const ObjectId id : objectIds) {
        if (id.isValid() && objectIndices_.contains(id.value())) {
            idsToRemove.insert(id.value());
        }
    }
    if (idsToRemove.isEmpty()) {
        return {};
    }

    QVector<SceneObject> remainingObjects;
    remainingObjects.reserve(objects_.size() - idsToRemove.size());
    QVector<ObjectId> removedIds;
    removedIds.reserve(idsToRemove.size());
    DocumentChangeSet changes;
    for (const SceneObject &object : objects_) {
        if (idsToRemove.contains(object.id.value())) {
            changes.placementChanged = changes.placementChanged ||
                object.placementTranslation.x != 0.0 ||
                object.placementTranslation.y != 0.0 ||
                object.placementTranslation.z != 0.0;
            removedIds.append(object.id);
            changes.addObject(object.id);
        } else {
            remainingObjects.append(object);
        }
    }

    objects_ = std::move(remainingObjects);
    for (const ObjectId id : removedIds) {
        objectGeometryRevisions_.remove(id.value());
    }
    rebuildObjectIndex();

    changes.geometryChanged = true;
    changes.structureChanged = true;
    applyChanges(changes);
    return removedIds;
}

QVector<ObjectId> Document::replaceShapes(const QVector<Shape> &shapes)
{
    objects_.clear();
    objectIndices_.clear();
    objectGeometryRevisions_.clear();
    nextObjectValue_ = 1;
    settings_ = DocumentSettings{};
    DocumentChangeSet clearChanges;
    clearChanges.geometryChanged = true;
    clearChanges.structureChanged = true;
    clearChanges.visibilityChanged = true;
    clearChanges.placementChanged = true;
    applyChanges(clearChanges);
    QVector<ObjectId> ids;
    ids.reserve(shapes.size());
    for (const Shape &shape : shapes) {
        ids.append(append(shape));
    }
    return ids;
}

void Document::replaceObjects(const QVector<SceneObject> &objects)
{
    const bool removedPlacement = std::any_of(
        objects_.cbegin(), objects_.cend(), [](const SceneObject &object) {
            return object.placementTranslation.x != 0.0 ||
                   object.placementTranslation.y != 0.0 ||
                   object.placementTranslation.z != 0.0;
        });
    objects_.clear();
    objectIndices_.clear();
    objectGeometryRevisions_.clear();
    for (SceneObject object : objects) {
        if (!object.id.isValid() || objectIndices_.contains(object.id.value())) {
            object.id = allocateObjectId();
        } else {
            nextObjectValue_ = std::max(nextObjectValue_, object.id.value() + 1);
        }
        const int index = objects_.size();
        objects_.append(object);
        objectIndices_.insert(object.id.value(), index);
    }
    DocumentChangeSet changes;
    changes.geometryChanged = true;
    changes.structureChanged = true;
    for (const SceneObject &object : objects_) {
        changes.addObject(object.id);
    }
    changes.placementChanged = removedPlacement || std::any_of(
        objects_.cbegin(), objects_.cend(), [](const SceneObject &object) {
            return object.placementTranslation.x != 0.0 ||
                   object.placementTranslation.y != 0.0 ||
                   object.placementTranslation.z != 0.0;
        });
    applyChanges(changes);
}

const DocumentCollection *Document::collection(CollectionId id) const
{
    for (const DocumentCollection &candidate : collections_) {
        if (candidate.id == id) return &candidate;
    }
    return nullptr;
}

CollectionId Document::createCollection(const QString &name)
{
    DocumentCollection value;
    value.id = allocateCollectionId();
    value.name = name.trimmed().isEmpty()
                     ? QStringLiteral("Collection %1").arg(value.id.value())
                     : name.trimmed();
    return insertCollection(value);
}

CollectionId Document::insertCollection(DocumentCollection value)
{
    if (!value.id.isValid()) value.id = allocateCollectionId();
    if (!value.sourceId.isEmpty()) {
        for (DocumentCollection &existing : collections_) {
            if (existing.sourceId == value.sourceId &&
                existing.libraryPath == value.libraryPath) {
                for (const QString &scene : value.sourceSceneNames)
                    if (!existing.sourceSceneNames.contains(scene)) existing.sourceSceneNames.append(scene);
                if (!value.sourceSceneName.isEmpty() && !existing.sourceSceneNames.contains(value.sourceSceneName))
                    existing.sourceSceneNames.append(value.sourceSceneName);
                return existing.id;
            }
        }
    }
    if (value.name.trimmed().isEmpty() || collection(value.id) != nullptr) return CollectionId::invalid();
    for (CollectionId parent : value.parentIds) {
        if (parent.isValid() && parent == value.id) return CollectionId::invalid();
    }
    value.name = value.name.trimmed();
    collections_.append(value);
    nextCollectionValue_ = qMax(nextCollectionValue_, value.id.value() + 1);
    DocumentChangeSet changes;
    changes.addCollection(value.id);
    changes.structureChanged = true;
    changes.collectionsChanged = true;
    changes.visibilityChanged = value.hideViewport || value.hideSelect;
    applyChanges(changes);
    return value.id;
}

bool Document::removeCollection(CollectionId id)
{
    const int index = [&]() {
        for (int i = 0; i < collections_.size(); ++i)
            if (collections_[i].id == id) return i;
        return -1;
    }();
    if (index < 0) return false;
    collections_.removeAt(index);
    for (DocumentCollection &candidate : collections_) candidate.parentIds.removeAll(id);
    for (SceneObject &object : objects_) object.collectionIds.removeAll(id);
    DocumentChangeSet changes;
    changes.addCollection(id);
    changes.collectionsChanged = true;
    changes.visibilityChanged = true;
    changes.structureChanged = true;
    applyChanges(changes);
    return true;
}

bool Document::renameCollection(CollectionId id, const QString &name)
{
    DocumentCollection *candidate = mutableCollection(id);
    const QString trimmed = name.trimmed();
    if (candidate == nullptr || trimmed.isEmpty()) return false;
    if (candidate->name == trimmed) return true;
    candidate->name = trimmed;
    DocumentChangeSet changes;
    changes.addCollection(id);
    changes.collectionsChanged = true;
    applyChanges(changes);
    return true;
}

bool Document::setCollectionParents(CollectionId id, const QVector<CollectionId> &parentIds)
{
    DocumentCollection *candidate = mutableCollection(id);
    if (candidate == nullptr) return false;
    QVector<CollectionId> unique;
    for (CollectionId parent : parentIds) {
        if (!parent.isValid() || parent == id || collection(parent) == nullptr) return false;
        if (!unique.contains(parent)) unique.append(parent);
    }
    const auto reachesId = [&](const auto &self, CollectionId current,
                               QSet<quint64> &visited) -> bool {
        if (current == id) return true;
        if (visited.contains(current.value())) return false;
        visited.insert(current.value());
        const DocumentCollection *node = collection(current);
        if (node == nullptr) return false;
        for (CollectionId parent : node->parentIds)
            if (self(self, parent, visited)) return true;
        return false;
    };
    for (CollectionId parent : unique) {
        QSet<quint64> visited;
        if (reachesId(reachesId, parent, visited)) return false;
    }
    if (candidate->parentIds == unique) return true;
    candidate->parentIds = unique;
    DocumentChangeSet changes;
    changes.addCollection(id);
    changes.collectionsChanged = true;
    changes.structureChanged = true;
    applyChanges(changes);
    return true;
}

bool Document::setCollectionVisibility(CollectionId id, bool hideViewport, bool hideRender)
{
    DocumentCollection *candidate = mutableCollection(id);
    if (candidate == nullptr) return false;
    if (candidate->hideViewport == hideViewport && candidate->hideRender == hideRender) return true;
    candidate->hideViewport = hideViewport;
    candidate->hideRender = hideRender;
    DocumentChangeSet changes;
    changes.addCollection(id);
    changes.collectionsChanged = true;
    changes.visibilityChanged = true;
    applyChanges(changes);
    return true;
}

bool Document::setCollectionHideSelect(CollectionId id, bool hideSelect)
{
    DocumentCollection *candidate = mutableCollection(id);
    if (candidate == nullptr) return false;
    if (candidate->hideSelect == hideSelect) return true;
    candidate->hideSelect = hideSelect;
    DocumentChangeSet changes;
    changes.addCollection(id);
    changes.collectionsChanged = true;
    changes.visibilityChanged = true;
    applyChanges(changes);
    return true;
}

bool Document::setObjectCollections(ObjectId objectId, const QVector<CollectionId> &collectionIds)
{
    SceneObject *candidate = mutableObject(objectId);
    if (candidate == nullptr) return false;
    QVector<CollectionId> unique;
    for (CollectionId id : collectionIds) {
        if (!id.isValid() || collection(id) == nullptr) return false;
        if (!unique.contains(id)) unique.append(id);
    }
    if (candidate->collectionIds == unique) return true;
    candidate->collectionIds = unique;
    DocumentChangeSet changes;
    changes.addObject(objectId);
    changes.visibilityChanged = true;
    applyChanges(changes);
    return true;
}

bool Document::setObjectVisibilityFlags(ObjectId objectId,
                                        bool hideViewport,
                                        bool hideSelect)
{
    SceneObject *candidate = mutableObject(objectId);
    if (candidate == nullptr) return false;
    if (candidate->hideViewport == hideViewport &&
        candidate->hideSelect == hideSelect) {
        return true;
    }
    candidate->hideViewport = hideViewport;
    candidate->hideSelect = hideSelect;
    DocumentChangeSet changes;
    changes.addObject(objectId);
    changes.visibilityChanged = true;
    applyChanges(changes);
    return true;
}

bool Document::setObjectViewLayerHidden(ObjectId objectId,
                                       const QString &viewLayerKey,
                                       bool hidden)
{
    SceneObject *candidate = mutableObject(objectId);
    if (candidate == nullptr || viewLayerKey.isEmpty()) return false;
    if (candidate->viewLayerHidden.value(viewLayerKey).toBool() == hidden &&
        candidate->viewLayerHidden.contains(viewLayerKey)) {
        return true;
    }
    candidate->viewLayerHidden.insert(viewLayerKey, hidden);
    DocumentChangeSet changes;
    changes.addObject(objectId);
    changes.visibilityChanged = true;
    applyChanges(changes);
    return true;
}

bool Document::setObjectParent(ObjectId objectId, ObjectId parentObjectId)
{
    SceneObject *candidate = mutableObject(objectId);
    if (candidate == nullptr || !candidate->libraryPath.isEmpty()) return false;

    QString parentIdentity;
    if (parentObjectId.isValid()) {
        const SceneObject *parent = object(parentObjectId);
        if (parent == nullptr || parentObjectId == objectId) return false;
        parentIdentity = parent->blenderSourceObjectId.isEmpty()
            ? QStringLiteral("classiCAD-local-object:%1").arg(parentObjectId.value())
            : parent->blenderSourceObjectId;

        QHash<QString, ObjectId> objectIdsByIdentity;
        for (const SceneObject &sceneObject : objects_) {
            if (sceneObject.blenderLooseMeshEdgeCompanion) continue;
            const QString identity = sceneObject.blenderSourceObjectId.isEmpty()
                ? QStringLiteral("classiCAD-local-object:%1")
                      .arg(sceneObject.id.value())
                : sceneObject.blenderSourceObjectId;
            objectIdsByIdentity.insert(identity, sceneObject.id);
        }

        QSet<quint64> visited;
        ObjectId current = parentObjectId;
        while (current.isValid() && !visited.contains(current.value())) {
            if (current == objectId) return false;
            visited.insert(current.value());
            const SceneObject *currentObject = object(current);
            if (currentObject == nullptr ||
                currentObject->blenderParentObjectId.isEmpty()) {
                break;
            }
            current = objectIdsByIdentity.value(
                currentObject->blenderParentObjectId, ObjectId::invalid());
        }
    }

    if (candidate->blenderParentObjectId == parentIdentity) return true;
    candidate->blenderParentObjectId = parentIdentity;
    DocumentChangeSet changes;
    changes.addObject(objectId);
    changes.structureChanged = true;
    applyChanges(changes);
    return true;
}

bool Document::setCollectionViewLayerState(CollectionId collectionId,
                                           const QString &viewLayerKey,
                                           const QStringList &sourcePath,
                                           const QString &stateKey,
                                           bool value)
{
    DocumentCollection *candidate = mutableCollection(collectionId);
    if (candidate == nullptr || viewLayerKey.isEmpty() || sourcePath.isEmpty() ||
        (stateKey != QStringLiteral("exclude") &&
         stateKey != QStringLiteral("hide_viewport"))) {
        return false;
    }

    const auto pathMatches = [&sourcePath](const QJsonArray &storedPath) {
        if (storedPath.size() != sourcePath.size()) return false;
        for (int index = 0; index < sourcePath.size(); ++index) {
            if (!storedPath[index].isString() ||
                storedPath[index].toString() != sourcePath[index]) {
                return false;
            }
        }
        return true;
    };

    QJsonArray states = candidate->viewLayerStates;
    for (qsizetype index = 0; index < states.size(); ++index) {
        QJsonObject state = states[index].toObject();
        if (state.value(QStringLiteral("view_layer_key")).toString() !=
                viewLayerKey ||
            !pathMatches(state.value(QStringLiteral("path")).toArray())) {
            continue;
        }
        if (state.value(stateKey).toBool() == value &&
            state.value(stateKey).isBool()) {
            return true;
        }
        state.insert(stateKey, value);
        states[index] = state;
        candidate->viewLayerStates = states;
        DocumentChangeSet changes;
        changes.addCollection(collectionId);
        changes.collectionsChanged = true;
        changes.visibilityChanged = true;
        applyChanges(changes);
        return true;
    }
    return false;
}

bool Document::setObjectDisplayName(ObjectId objectId, const QString &name)
{
    SceneObject *candidate = mutableObject(objectId);
    const QString trimmed = name.trimmed();
    if (candidate == nullptr || trimmed.isEmpty()) return false;
    if (candidate->displayName == trimmed) return true;
    candidate->displayName = trimmed;
    DocumentChangeSet changes;
    changes.addObject(objectId);
    changes.structureChanged = true;
    applyChanges(changes);
    return true;
}

bool Document::isObjectVisible(ObjectId objectId) const
{
    const SceneObject *sceneObject = object(objectId);
    if (sceneObject == nullptr || sceneObject->hideViewport ||
        sceneObject->hiddenInViewport) return false;
    const QString activeViewLayerKey = settings_.activeViewLayerKey.isEmpty()
        ? sceneObject->defaultViewLayerKey : settings_.activeViewLayerKey;
    if (!activeViewLayerKey.isEmpty() &&
        sceneObject->viewLayerHidden.contains(activeViewLayerKey) &&
        sceneObject->viewLayerHidden.value(activeViewLayerKey).toBool()) return false;
    const auto hasVisibleCollectionPath = [&](const auto &self, CollectionId id,
                                              QSet<quint64> path) -> bool {
        if (path.contains(id.value())) return false;
        path.insert(id.value());
        const DocumentCollection *entry = collection(id);
        if (entry == nullptr) return false;
        if (entry->hideViewport) return false;
        if (entry->parentIds.isEmpty()) return true;
        for (CollectionId parent : entry->parentIds)
            if (self(self, parent, path)) return true;
        return false;
    };
    if (!sceneObject->collectionIds.isEmpty()) {
        if (!activeViewLayerKey.isEmpty()) {
            const QString key = activeViewLayerKey;
            const auto pathMatches = [](const QJsonArray &stored, const QStringList &expected) {
                if (stored.size() != expected.size()) return false;
                for (int i = 0; i < expected.size(); ++i)
                    if (!stored[i].isString() || stored[i].toString() != expected[i]) return false;
                return true;
            };
            for (CollectionId membershipId : sceneObject->collectionIds) {
                const DocumentCollection *membership = collection(membershipId);
                if (membership == nullptr) continue;
                for (const QJsonValue &membershipStateValue : membership->viewLayerStates) {
                    const QJsonObject membershipState = membershipStateValue.toObject();
                    if (membershipState.value(QStringLiteral("view_layer_key")).toString() != key)
                        continue;
                    const QJsonArray pathArray = membershipState.value(QStringLiteral("path")).toArray();
                    QStringList sourcePath;
                    for (const QJsonValue &part : pathArray) sourcePath.append(part.toString());
                    if (sourcePath.isEmpty() || sourcePath.last() != membership->sourceId) continue;
                    bool visible = true;
                    CollectionId previous = CollectionId::invalid();
                    for (int index = 0; index < sourcePath.size(); ++index) {
                        const QString &sourceId = sourcePath[index];
                        const auto node = std::find_if(collections_.cbegin(), collections_.cend(),
                            [&sourceId](const DocumentCollection &candidate) {
                                return candidate.sourceId == sourceId;
                            });
                        if (node == collections_.cend() ||
                            (previous.isValid() && !node->parentIds.contains(previous)) ||
                            node->hideViewport) {
                            visible = false;
                            break;
                        }
                        QJsonObject nodeState;
                        bool foundNodeState = false;
                        for (const QJsonValue &stateValue : node->viewLayerStates) {
                            const QJsonObject state = stateValue.toObject();
                            QStringList prefix;
                            for (int prefixIndex = 0; prefixIndex <= index; ++prefixIndex)
                                prefix.append(sourcePath[prefixIndex]);
                            if (state.value(QStringLiteral("view_layer_key")).toString() == key &&
                                pathMatches(state.value(QStringLiteral("path")).toArray(), prefix)) {
                                nodeState = state;
                                foundNodeState = true;
                                break;
                            }
                        }
                        if (!foundNodeState || nodeState.value(QStringLiteral("exclude")).toBool() ||
                            nodeState.value(QStringLiteral("hide_viewport")).toBool()) {
                            visible = false;
                            break;
                        }
                        previous = node->id;
                    }
                    if (visible) return true;
                }
            }
            return false;
        }
        for (CollectionId id : sceneObject->collectionIds)
            if (hasVisibleCollectionPath(hasVisibleCollectionPath, id, {})) return true;
        return false;
    }
    return true;
}

bool Document::isObjectRenderable(ObjectId objectId) const
{
    const SceneObject *sceneObject = object(objectId);
    if (sceneObject == nullptr || sceneObject->hideRender) {
        return false;
    }

    const QString activeViewLayerKey = settings_.activeViewLayerKey.isEmpty()
        ? sceneObject->defaultViewLayerKey : settings_.activeViewLayerKey;
    const auto hasRenderableCollectionPath = [&](const auto &self,
                                                  CollectionId id,
                                                  QSet<quint64> path) -> bool {
        if (path.contains(id.value())) return false;
        path.insert(id.value());
        const DocumentCollection *entry = collection(id);
        if (entry == nullptr || entry->hideRender) return false;
        if (entry->parentIds.isEmpty()) return true;
        for (CollectionId parent : entry->parentIds) {
            if (self(self, parent, path)) return true;
        }
        return false;
    };

    if (sceneObject->collectionIds.isEmpty()) return true;
    if (activeViewLayerKey.isEmpty()) {
        for (CollectionId id : sceneObject->collectionIds) {
            if (hasRenderableCollectionPath(hasRenderableCollectionPath, id, {})) {
                return true;
            }
        }
        return false;
    }

    const auto pathMatches = [](const QJsonArray &stored,
                                const QStringList &expected) {
        if (stored.size() != expected.size()) return false;
        for (int i = 0; i < expected.size(); ++i) {
            if (!stored[i].isString() || stored[i].toString() != expected[i]) {
                return false;
            }
        }
        return true;
    };
    for (CollectionId membershipId : sceneObject->collectionIds) {
        const DocumentCollection *membership = collection(membershipId);
        if (membership == nullptr || membership->hideRender) continue;
        for (const QJsonValue &stateValue : membership->viewLayerStates) {
            const QJsonObject membershipState = stateValue.toObject();
            if (membershipState.value(QStringLiteral("view_layer_key")).toString() !=
                activeViewLayerKey) {
                continue;
            }
            const QJsonArray pathArray =
                membershipState.value(QStringLiteral("path")).toArray();
            QStringList sourcePath;
            for (const QJsonValue &part : pathArray) {
                sourcePath.append(part.toString());
            }
            if (sourcePath.isEmpty() || sourcePath.last() != membership->sourceId) {
                continue;
            }

            bool renderable = true;
            CollectionId previous = CollectionId::invalid();
            for (int index = 0; index < sourcePath.size(); ++index) {
                const QString &sourceId = sourcePath[index];
                const auto node = std::find_if(
                    collections_.cbegin(), collections_.cend(),
                    [&sourceId](const DocumentCollection &candidate) {
                        return candidate.sourceId == sourceId;
                    });
                if (node == collections_.cend() || node->hideRender ||
                    (previous.isValid() && !node->parentIds.contains(previous))) {
                    renderable = false;
                    break;
                }
                QJsonObject nodeState;
                bool foundNodeState = false;
                for (const QJsonValue &nodeStateValue : node->viewLayerStates) {
                    const QJsonObject state = nodeStateValue.toObject();
                    QStringList prefix;
                    for (int prefixIndex = 0; prefixIndex <= index; ++prefixIndex) {
                        prefix.append(sourcePath[prefixIndex]);
                    }
                    if (state.value(QStringLiteral("view_layer_key")).toString() ==
                            activeViewLayerKey &&
                        pathMatches(state.value(QStringLiteral("path")).toArray(),
                                    prefix)) {
                        nodeState = state;
                        foundNodeState = true;
                        break;
                    }
                }
                if (!foundNodeState ||
                    nodeState.value(QStringLiteral("exclude")).toBool() ||
                    nodeState.value(QStringLiteral("hide_render")).toBool()) {
                    renderable = false;
                    break;
                }
                previous = node->id;
            }
            if (renderable) return true;
        }
    }
    return false;
}

bool Document::isObjectEditable(ObjectId objectId) const
{
    const SceneObject *sceneObject = object(objectId);
    if (sceneObject == nullptr || sceneObject->hideSelect ||
        !isObjectVisible(objectId)) {
        return false;
    }
    if (sceneObject->collectionIds.isEmpty()) return true;

    const QString activeViewLayerKey = settings_.activeViewLayerKey.isEmpty()
        ? sceneObject->defaultViewLayerKey : settings_.activeViewLayerKey;
    if (activeViewLayerKey.isEmpty()) {
        const auto hasSelectableCollectionPath =
            [&](const auto &self, CollectionId id, QSet<quint64> path) -> bool {
                if (path.contains(id.value())) return false;
                path.insert(id.value());
                const DocumentCollection *entry = collection(id);
                if (entry == nullptr || entry->hideViewport || entry->hideSelect)
                    return false;
                if (entry->parentIds.isEmpty()) return true;
                for (CollectionId parent : entry->parentIds) {
                    if (self(self, parent, path)) return true;
                }
                return false;
            };
        for (CollectionId membershipId : sceneObject->collectionIds) {
            if (hasSelectableCollectionPath(hasSelectableCollectionPath,
                                            membershipId, {})) {
                return true;
            }
        }
        return false;
    }

    const auto pathMatches = [](const QJsonArray &stored,
                                const QStringList &expected) {
        if (stored.size() != expected.size()) return false;
        for (int i = 0; i < expected.size(); ++i) {
            if (!stored[i].isString() || stored[i].toString() != expected[i])
                return false;
        }
        return true;
    };
    for (CollectionId membershipId : sceneObject->collectionIds) {
        const DocumentCollection *membership = collection(membershipId);
        if (membership == nullptr) continue;
        for (const QJsonValue &membershipStateValue : membership->viewLayerStates) {
            const QJsonObject membershipState = membershipStateValue.toObject();
            if (membershipState.value(QStringLiteral("view_layer_key")).toString() !=
                activeViewLayerKey) {
                continue;
            }
            const QJsonArray pathArray =
                membershipState.value(QStringLiteral("path")).toArray();
            QStringList sourcePath;
            for (const QJsonValue &part : pathArray) {
                if (!part.isString()) {
                    sourcePath.clear();
                    break;
                }
                sourcePath.append(part.toString());
            }
            if (sourcePath.isEmpty() || sourcePath.last() != membership->sourceId)
                continue;

            bool selectable = true;
            CollectionId previous = CollectionId::invalid();
            for (int index = 0; index < sourcePath.size(); ++index) {
                const QString &sourceId = sourcePath[index];
                const auto node = std::find_if(
                    collections_.cbegin(), collections_.cend(),
                    [&sourceId](const DocumentCollection &candidate) {
                        return candidate.sourceId == sourceId;
                    });
                if (node == collections_.cend() ||
                    (previous.isValid() && !node->parentIds.contains(previous)) ||
                    node->hideViewport || node->hideSelect) {
                    selectable = false;
                    break;
                }

                QJsonObject nodeState;
                bool foundNodeState = false;
                QStringList prefix;
                for (int prefixIndex = 0; prefixIndex <= index; ++prefixIndex)
                    prefix.append(sourcePath[prefixIndex]);
                for (const QJsonValue &stateValue : node->viewLayerStates) {
                    const QJsonObject state = stateValue.toObject();
                    if (state.value(QStringLiteral("view_layer_key")).toString() ==
                            activeViewLayerKey &&
                        pathMatches(state.value(QStringLiteral("path")).toArray(),
                                    prefix)) {
                        nodeState = state;
                        foundNodeState = true;
                        break;
                    }
                }
                if (!foundNodeState ||
                    nodeState.value(QStringLiteral("exclude")).toBool()) {
                    selectable = false;
                    break;
                }
                previous = node->id;
            }
            // Blender ORs BASE_SELECTABLE across collection paths. A hidden
            // (but non-excluded) LayerCollection still contributes that flag;
            // visibility has already been checked independently above.
            if (selectable) return true;
        }
    }
    return false;
}

Document::Snapshot Document::snapshot() const
{
    return Snapshot{collections_, objects_, materials_, nextObjectValue_,
                    nextCollectionValue_, settings_};
}

void Document::restoreSnapshot(const Snapshot &snapshot)
{
    collections_ = snapshot.collections;
    objects_ = snapshot.objects;
    materials_ = snapshot.materials;
    nextObjectValue_ = snapshot.nextObjectValue;
    nextCollectionValue_ = snapshot.nextCollectionValue;
    settings_ = isValidDocumentSettings(snapshot.settings)
                    ? snapshot.settings
                    : DocumentSettings{};
    rebuildObjectIndex();
    objectGeometryRevisions_.clear();
    DocumentChangeSet changes;
    changes.geometryChanged = true;
    changes.structureChanged = true;
    changes.collectionsChanged = true;
    changes.visibilityChanged = true;
    changes.settingsChanged = true;
    changes.placementChanged = true;
    for (const SceneObject &object : objects_) {
        changes.addObject(object.id);
    }
    for (const DocumentCollection &collection : collections_) {
        changes.addCollection(collection.id);
    }
    applyChanges(changes);
}

Document &Document::operator=(const QVector<Shape> &shapes)
{
    replaceShapes(shapes);
    return *this;
}

Document::ConstShapeIterator Document::begin() const
{
    return ConstShapeIterator(objects_.cbegin());
}

Document::ConstShapeIterator Document::end() const
{
    return ConstShapeIterator(objects_.cend());
}

ObjectId Document::allocateObjectId()
{
    if (nextObjectValue_ == 0) {
        nextObjectValue_ = 1;
    }
    return ObjectId::fromValue(nextObjectValue_++);
}

CollectionId Document::allocateCollectionId()
{
    if (nextCollectionValue_ == 0) nextCollectionValue_ = 1;
    return CollectionId::fromValue(nextCollectionValue_++);
}

DocumentCollection *Document::mutableCollection(CollectionId id)
{
    for (DocumentCollection &candidate : collections_)
        if (candidate.id == id) return &candidate;
    return nullptr;
}

void Document::rebuildObjectIndex()
{
    objectIndices_.clear();
    objectIndices_.reserve(objects_.size());
    for (int index = 0; index < objects_.size(); ++index) {
        objectIndices_.insert(objects_[index].id.value(), index);
    }
}

void Document::bumpRevision(quint64 *revision)
{
    if (revision == nullptr) {
        return;
    }
    ++revisionClock_;
    if (revisionClock_ == 0) {
        ++revisionClock_;
    }
    *revision = revisionClock_;
}

void Document::noteGeometryChange(ObjectId id)
{
    DocumentChangeSet changes;
    changes.addObject(id);
    changes.geometryChanged = true;
    applyChanges(changes);
}

} // namespace classiCAD
