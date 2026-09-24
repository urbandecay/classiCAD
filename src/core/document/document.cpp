#include "document.h"

#include <algorithm>

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

QVector<SceneObject> &Document::objects()
{
    return objects_;
}

const QVector<Layer> &Document::layers() const
{
    return layers_;
}

const SceneObject *Document::object(ObjectId id) const
{
    const int index = indexOf(id);
    return index >= 0 ? &objects_[index] : nullptr;
}

SceneObject *Document::object(ObjectId id)
{
    const int index = indexOf(id);
    return index >= 0 ? &objects_[index] : nullptr;
}

const Shape *Document::shape(ObjectId id) const
{
    const SceneObject *sceneObject = object(id);
    return sceneObject != nullptr ? &sceneObject->geometry : nullptr;
}

Shape *Document::shape(ObjectId id)
{
    SceneObject *sceneObject = object(id);
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

    for (int index = 0; index < objects_.size(); ++index) {
        if (objects_[index].id == id) {
            return index;
        }
    }
    return -1;
}

Shape &Document::operator[](int index)
{
    return objects_[index].geometry;
}

const Shape &Document::operator[](int index) const
{
    return objects_[index].geometry;
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
    rebuildLayerObjectIds();
    return object.id;
}

bool Document::replace(ObjectId id, const Shape &shape)
{
    SceneObject *sceneObject = object(id);
    if (sceneObject == nullptr) {
        return false;
    }
    sceneObject->geometry = shape;
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
    objects_.removeAt(index);
    rebuildLayerObjectIds();
    return true;
}

QVector<ObjectId> Document::replaceShapes(const QVector<Shape> &shapes)
{
    objects_.clear();
    layers_.clear();
    activeLayerId_ = LayerId::invalid();
    nextObjectValue_ = 1;
    nextLayerValue_ = 1;
    ensureDefaultLayer();
    QVector<ObjectId> ids;
    ids.reserve(shapes.size());
    for (const Shape &shape : shapes) {
        ids.append(append(shape));
    }
    return ids;
}

void Document::replaceObjects(const QVector<SceneObject> &objects)
{
    objects_.clear();
    ensureDefaultLayer();
    for (SceneObject object : objects) {
        if (!object.id.isValid() || indexOf(object.id) >= 0) {
            object.id = allocateObjectId();
        } else {
            nextObjectValue_ = std::max(nextObjectValue_, object.id.value() + 1);
        }
        object.layerId = normalizedLayerId(object.layerId);
        objects_.append(object);
    }
    rebuildLayerObjectIds();
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
    activeLayerId_ = id;
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
            if (candidate.id != id && candidate.visible && !candidate.locked) {
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
    return true;
}

bool Document::renameLayer(LayerId id, const QString &name)
{
    Layer *candidate = layer(id);
    const QString trimmedName = name.trimmed();
    if (candidate == nullptr || trimmedName.isEmpty()) {
        return false;
    }

    candidate->name = trimmedName;
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
    return true;
}

Layer *Document::layer(LayerId id)
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
    Layer *candidate = layer(id);
    if (candidate == nullptr) {
        return false;
    }

    if (!visible && id == activeLayerId_) {
        LayerId replacement = LayerId::invalid();
        for (const Layer &other : layers_) {
            if (other.id != id && other.visible && !other.locked) {
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
    return true;
}

bool Document::setLayerLocked(LayerId id, bool locked)
{
    Layer *candidate = layer(id);
    if (candidate == nullptr) {
        return false;
    }

    if (locked && id == activeLayerId_) {
        LayerId replacement = LayerId::invalid();
        for (const Layer &other : layers_) {
            if (other.id != id && other.visible && !other.locked) {
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
    return true;
}

bool Document::isLayerEditable(LayerId id) const
{
    const Layer *candidate = layer(id);
    return candidate != nullptr && candidate->visible && !candidate->locked;
}

bool Document::moveObjectToLayer(ObjectId objectId, LayerId layerId)
{
    SceneObject *sceneObject = object(objectId);
    if (sceneObject == nullptr || !isObjectEditable(objectId) ||
        !isLayerEditable(layerId)) {
        return false;
    }
    sceneObject->layerId = layerId;
    rebuildLayerObjectIds();
    return true;
}

bool Document::isObjectVisible(ObjectId objectId) const
{
    const SceneObject *sceneObject = object(objectId);
    const Layer *objectLayer = sceneObject == nullptr ? nullptr : layer(sceneObject->layerId);
    return objectLayer != nullptr && objectLayer->visible;
}

bool Document::isObjectEditable(ObjectId objectId) const
{
    const SceneObject *sceneObject = object(objectId);
    const Layer *objectLayer = sceneObject == nullptr ? nullptr : layer(sceneObject->layerId);
    return objectLayer != nullptr && objectLayer->visible && !objectLayer->locked;
}

Document::Snapshot Document::snapshot() const
{
    return Snapshot{layers_, objects_, activeLayerId_, nextObjectValue_, nextLayerValue_};
}

void Document::restoreSnapshot(const Snapshot &snapshot)
{
    layers_ = snapshot.layers;
    objects_ = snapshot.objects;
    activeLayerId_ = snapshot.activeLayerId;
    nextObjectValue_ = snapshot.nextObjectValue;
    nextLayerValue_ = snapshot.nextLayerValue;
    ensureDefaultLayer();
    rebuildLayerObjectIds();
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
        Layer *objectLayer = layer(object.layerId);
        if (objectLayer != nullptr) {
            objectLayer->objectIds.append(object.id);
        }
    }
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
    defaultLayer.name = QStringLiteral("Default");
    layers_.append(defaultLayer);
    activeLayerId_ = defaultLayer.id;
}

} // namespace classiCAD
