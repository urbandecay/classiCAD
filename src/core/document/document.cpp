#include "document.h"

#include <QSet>

#include <algorithm>
#include <cmath>

namespace classiCAD {

Document::Document()
{
    ensureDefaultLayer();
}

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

const QVector<Layer> &Document::layers() const
{
    return layers_;
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
    if (changes.layerPropertiesChanged || changes.visibilityChanged ||
        !changes.layerIds.isEmpty() || changes.structureChanged) {
        bumpRevision(&revisions_.layer);
    }
    if (changes.visibilityChanged) {
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
    object.layerId = normalizedLayerId(activeLayerId_);
    object.geometry = shape;
    return insertObject(index, object);
}

ObjectId Document::insertObject(int index, SceneObject object)
{
    ensureDefaultLayer();
    if (!object.id.isValid() || indexOf(object.id) >= 0) {
        object.id = allocateObjectId();
    } else {
        nextObjectValue_ = std::max(nextObjectValue_, object.id.value() + 1);
    }
    object.layerId = normalizedLayerId(object.layerId);
    index = std::clamp(index, 0, static_cast<int>(objects_.size()));
    objects_.insert(index, object);
    for (int objectIndex = index; objectIndex < objects_.size(); ++objectIndex) {
        objectIndices_.insert(objects_[objectIndex].id.value(), objectIndex);
    }
    objectGeometryRevisions_.insert(object.id.value(), revisionClock_);
    rebuildLayerObjectIds();
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

    ensureDefaultLayer();
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
        object.layerId = normalizedLayerId(object.layerId);
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
    rebuildLayerObjectIds();

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
    rebuildLayerObjectIds();
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
            changes.addLayer(object.layerId);
        } else {
            remainingObjects.append(object);
        }
    }

    objects_ = std::move(remainingObjects);
    for (const ObjectId id : removedIds) {
        objectGeometryRevisions_.remove(id.value());
    }
    rebuildObjectIndex();
    rebuildLayerObjectIds();

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
    layers_.clear();
    activeLayerId_ = LayerId::invalid();
    nextObjectValue_ = 1;
    nextLayerValue_ = 1;
    settings_ = DocumentSettings{};
    ensureDefaultLayer();
    DocumentChangeSet clearChanges;
    clearChanges.geometryChanged = true;
    clearChanges.structureChanged = true;
    clearChanges.layerPropertiesChanged = true;
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
    ensureDefaultLayer();
    for (SceneObject object : objects) {
        if (!object.id.isValid() || objectIndices_.contains(object.id.value())) {
            object.id = allocateObjectId();
        } else {
            nextObjectValue_ = std::max(nextObjectValue_, object.id.value() + 1);
        }
        object.layerId = normalizedLayerId(object.layerId);
        const int index = objects_.size();
        objects_.append(object);
        objectIndices_.insert(object.id.value(), index);
    }
    rebuildLayerObjectIds();
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

LayerId Document::activeLayerId() const
{
    return activeLayerId_;
}

bool Document::setActiveLayer(LayerId id)
{
    if (!isLayerEditable(id)) {
        return false;
    }
    if (activeLayerId_ == id) {
        return true;
    }
    activeLayerId_ = id;
    DocumentChangeSet changes;
    changes.addLayer(id);
    changes.layerPropertiesChanged = true;
    applyChanges(changes);
    return true;
}

LayerId Document::createLayer(const QString &name)
{
    Layer layer;
    layer.id = allocateLayerId();
    layer.name = name.isEmpty() ? QStringLiteral("Layer %1").arg(layer.id.value()) : name;
    layers_.append(layer);
    if (!activeLayerId_.isValid()) {
        activeLayerId_ = layer.id;
    }
    DocumentChangeSet changes;
    changes.addLayer(layer.id);
    changes.structureChanged = true;
    applyChanges(changes);
    return layer.id;
}

bool Document::removeLayer(LayerId id)
{
    if (layers_.size() <= 1) {
        return false;
    }

    const int layerIndex = [&]() {
        for (int index = 0; index < layers_.size(); ++index) {
            if (layers_[index].id == id) {
                return index;
            }
        }
        return -1;
    }();
    if (layerIndex < 0 || !layers_[layerIndex].objectIds.isEmpty()) {
        return false;
    }

    LayerId replacement = LayerId::invalid();
    if (activeLayerId_ == id) {
        for (const Layer &candidate : layers_) {
            if (candidate.id != id && candidate.visible && !candidate.frozen &&
                !candidate.locked) {
                replacement = candidate.id;
                break;
            }
        }
        if (!replacement.isValid()) {
            return false;
        }
    }

    layers_.removeAt(layerIndex);
    if (activeLayerId_ == id) {
        activeLayerId_ = replacement;
    }
    DocumentChangeSet changes;
    changes.addLayer(id);
    changes.structureChanged = true;
    changes.layerPropertiesChanged = true;
    applyChanges(changes);
    return true;
}

bool Document::renameLayer(LayerId id, const QString &name)
{
    Layer *candidate = mutableLayer(id);
    const QString trimmedName = name.trimmed();
    if (candidate == nullptr || trimmedName.isEmpty()) {
        return false;
    }

    candidate->name = trimmedName;
    DocumentChangeSet changes;
    changes.addLayer(id);
    changes.layerPropertiesChanged = true;
    applyChanges(changes);
    return true;
}

bool Document::moveLayer(LayerId id, int targetIndex)
{
    const int sourceIndex = [&]() {
        for (int index = 0; index < layers_.size(); ++index) {
            if (layers_[index].id == id) {
                return index;
            }
        }
        return -1;
    }();
    if (sourceIndex < 0 || targetIndex < 0 || targetIndex >= layers_.size()) {
        return false;
    }
    if (sourceIndex == targetIndex) {
        return true;
    }

    const Layer movedLayer = layers_.takeAt(sourceIndex);
    layers_.insert(targetIndex, movedLayer);
    DocumentChangeSet changes;
    changes.addLayer(id);
    changes.structureChanged = true;
    changes.layerPropertiesChanged = true;
    applyChanges(changes);
    return true;
}

Layer *Document::mutableLayer(LayerId id)
{
    for (Layer &candidate : layers_) {
        if (candidate.id == id) {
            return &candidate;
        }
    }
    return nullptr;
}

const Layer *Document::layer(LayerId id) const
{
    for (const Layer &candidate : layers_) {
        if (candidate.id == id) {
            return &candidate;
        }
    }
    return nullptr;
}

bool Document::setLayerVisible(LayerId id, bool visible)
{
    Layer *candidate = mutableLayer(id);
    if (candidate == nullptr) {
        return false;
    }

    if (!visible && id == activeLayerId_) {
        LayerId replacement = LayerId::invalid();
        for (const Layer &other : layers_) {
            if (other.id != id && other.visible && !other.frozen && !other.locked) {
                replacement = other.id;
                break;
            }
        }
        if (!replacement.isValid()) {
            return false;
        }
        activeLayerId_ = replacement;
    }

    candidate->visible = visible;
    DocumentChangeSet changes;
    changes.addLayer(id);
    changes.layerPropertiesChanged = true;
    changes.visibilityChanged = true;
    applyChanges(changes);
    return true;
}

bool Document::setLayerFrozen(LayerId id, bool frozen)
{
    Layer *candidate = mutableLayer(id);
    if (candidate == nullptr) {
        return false;
    }

    if (frozen && id == activeLayerId_) {
        LayerId replacement = LayerId::invalid();
        for (const Layer &other : layers_) {
            if (other.id != id && other.visible && !other.frozen && !other.locked) {
                replacement = other.id;
                break;
            }
        }
        if (!replacement.isValid()) {
            return false;
        }
        activeLayerId_ = replacement;
    }

    candidate->frozen = frozen;
    DocumentChangeSet changes;
    changes.addLayer(id);
    changes.layerPropertiesChanged = true;
    changes.visibilityChanged = true;
    applyChanges(changes);
    return true;
}

bool Document::setLayerLocked(LayerId id, bool locked)
{
    Layer *candidate = mutableLayer(id);
    if (candidate == nullptr) {
        return false;
    }

    if (locked && id == activeLayerId_) {
        LayerId replacement = LayerId::invalid();
        for (const Layer &other : layers_) {
            if (other.id != id && other.visible && !other.frozen && !other.locked) {
                replacement = other.id;
                break;
            }
        }
        if (!replacement.isValid()) {
            return false;
        }
        activeLayerId_ = replacement;
    }

    candidate->locked = locked;
    DocumentChangeSet changes;
    changes.addLayer(id);
    changes.layerPropertiesChanged = true;
    changes.visibilityChanged = true;
    applyChanges(changes);
    return true;
}

bool Document::setLayerColor(LayerId id, const QColor &color)
{
    Layer *candidate = mutableLayer(id);
    if (candidate == nullptr || !color.isValid()) {
        return false;
    }

    candidate->color = color;
    DocumentChangeSet changes;
    changes.addLayer(id);
    changes.layerPropertiesChanged = true;
    applyChanges(changes);
    return true;
}

bool Document::setLayerLineType(LayerId id, const QString &lineType)
{
    Layer *candidate = mutableLayer(id);
    const QString trimmedLineType = lineType.trimmed();
    if (candidate == nullptr || trimmedLineType.isEmpty()) {
        return false;
    }
    candidate->lineType = trimmedLineType;
    DocumentChangeSet changes;
    changes.addLayer(id);
    changes.layerPropertiesChanged = true;
    applyChanges(changes);
    return true;
}

bool Document::setLayerLineWeight(LayerId id, qreal lineWeightMm)
{
    Layer *candidate = mutableLayer(id);
    if (candidate == nullptr || !std::isfinite(lineWeightMm) || lineWeightMm < 0.0 ||
        lineWeightMm > 2.11) {
        return false;
    }
    candidate->lineWeightMm = lineWeightMm;
    DocumentChangeSet changes;
    changes.addLayer(id);
    changes.layerPropertiesChanged = true;
    applyChanges(changes);
    return true;
}

bool Document::setLayerPlotted(LayerId id, bool plotted)
{
    Layer *candidate = mutableLayer(id);
    if (candidate == nullptr) {
        return false;
    }
    candidate->plotted = plotted;
    DocumentChangeSet changes;
    changes.addLayer(id);
    changes.layerPropertiesChanged = true;
    applyChanges(changes);
    return true;
}

bool Document::setLayerDescription(LayerId id, const QString &description)
{
    Layer *candidate = mutableLayer(id);
    if (candidate == nullptr) {
        return false;
    }
    candidate->description = description;
    DocumentChangeSet changes;
    changes.addLayer(id);
    changes.layerPropertiesChanged = true;
    applyChanges(changes);
    return true;
}

bool Document::isLayerEditable(LayerId id) const
{
    const Layer *candidate = layer(id);
    return candidate != nullptr && candidate->visible && !candidate->frozen &&
           !candidate->locked;
}

bool Document::moveObjectToLayer(ObjectId objectId, LayerId layerId)
{
    SceneObject *sceneObject = mutableObject(objectId);
    if (sceneObject == nullptr || !isObjectEditable(objectId) ||
        !isLayerEditable(layerId)) {
        return false;
    }
    const LayerId oldLayerId = sceneObject->layerId;
    if (oldLayerId == layerId) {
        return true;
    }
    sceneObject->layerId = layerId;
    rebuildLayerObjectIds();
    DocumentChangeSet changes;
    changes.addObject(objectId);
    changes.addLayer(oldLayerId);
    changes.addLayer(layerId);
    changes.layerPropertiesChanged = true;
    applyChanges(changes);
    return true;
}

bool Document::isObjectVisible(ObjectId objectId) const
{
    const SceneObject *sceneObject = object(objectId);
    const Layer *objectLayer = sceneObject == nullptr ? nullptr : layer(sceneObject->layerId);
    return objectLayer != nullptr && objectLayer->visible && !objectLayer->frozen;
}

bool Document::isObjectEditable(ObjectId objectId) const
{
    const SceneObject *sceneObject = object(objectId);
    const Layer *objectLayer = sceneObject == nullptr ? nullptr : layer(sceneObject->layerId);
    return objectLayer != nullptr && objectLayer->visible && !objectLayer->frozen &&
           !objectLayer->locked;
}

Document::Snapshot Document::snapshot() const
{
    return Snapshot{layers_, objects_, activeLayerId_, nextObjectValue_, nextLayerValue_, settings_};
}

void Document::restoreSnapshot(const Snapshot &snapshot)
{
    layers_ = snapshot.layers;
    objects_ = snapshot.objects;
    activeLayerId_ = snapshot.activeLayerId;
    nextObjectValue_ = snapshot.nextObjectValue;
    nextLayerValue_ = snapshot.nextLayerValue;
    settings_ = isValidDocumentSettings(snapshot.settings)
                    ? snapshot.settings
                    : DocumentSettings{};
    ensureDefaultLayer();
    rebuildLayerObjectIds();
    rebuildObjectIndex();
    objectGeometryRevisions_.clear();
    DocumentChangeSet changes;
    changes.geometryChanged = true;
    changes.structureChanged = true;
    changes.layerPropertiesChanged = true;
    changes.visibilityChanged = true;
    changes.settingsChanged = true;
    changes.placementChanged = true;
    for (const SceneObject &object : objects_) {
        changes.addObject(object.id);
    }
    for (const Layer &layer : layers_) {
        changes.addLayer(layer.id);
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

LayerId Document::allocateLayerId()
{
    if (nextLayerValue_ == 0) {
        nextLayerValue_ = 1;
    }
    return LayerId::fromValue(nextLayerValue_++);
}

LayerId Document::normalizedLayerId(LayerId requested) const
{
    return layer(requested) != nullptr ? requested : activeLayerId_;
}

void Document::rebuildLayerObjectIds()
{
    for (Layer &layer : layers_) {
        layer.objectIds.clear();
    }
    for (const SceneObject &object : objects_) {
        Layer *objectLayer = mutableLayer(object.layerId);
        if (objectLayer != nullptr) {
            objectLayer->objectIds.append(object.id);
        }
    }
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

void Document::ensureDefaultLayer()
{
    if (!layers_.isEmpty()) {
        if (layer(activeLayerId_) == nullptr) {
            activeLayerId_ = layers_.first().id;
        }
        return;
    }

    Layer defaultLayer;
    defaultLayer.id = allocateLayerId();
    defaultLayer.name = QStringLiteral("0");
    layers_.append(defaultLayer);
    activeLayerId_ = defaultLayer.id;

    Layer definitionPointsLayer;
    definitionPointsLayer.id = allocateLayerId();
    definitionPointsLayer.name = QStringLiteral("Defpoints");
    definitionPointsLayer.color = QColor(QStringLiteral("#ffffff"));
    definitionPointsLayer.description = QStringLiteral("Non-plot definition points");
    definitionPointsLayer.plotted = false;
    layers_.append(definitionPointsLayer);
}

} // namespace classiCAD
