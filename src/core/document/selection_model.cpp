#include "selection_model.h"

#include "document.h"

namespace classiCAD {

const QVector<ObjectId> &SelectionModel::objectIds() const
{
    return objectIds_;
}

QVector<ObjectId> &SelectionModel::objectIds()
{
    return objectIds_;
}

ObjectId SelectionModel::primaryObjectId() const
{
    return primaryObjectId_;
}

ObjectId &SelectionModel::primaryObjectId()
{
    return primaryObjectId_;
}

bool SelectionModel::contains(ObjectId objectId) const
{
    return objectId.isValid() && objectIds_.contains(objectId);
}

void SelectionModel::clear()
{
    objectIds_.clear();
    primaryObjectId_ = ObjectId::invalid();
    clearActiveControlPoint();
}

void SelectionModel::setObjectIds(const QVector<ObjectId> &objectIds,
                                  ObjectId primaryObjectId)
{
    objectIds_.clear();
    for (const ObjectId objectId : objectIds) {
        if (objectId.isValid() && !objectIds_.contains(objectId)) {
            objectIds_.append(objectId);
        }
    }

    if (primaryObjectId.isValid() && objectIds_.contains(primaryObjectId)) {
        primaryObjectId_ = primaryObjectId;
    } else if (!objectIds_.isEmpty()) {
        primaryObjectId_ = objectIds_.back();
    } else {
        primaryObjectId_ = ObjectId::invalid();
    }

    if (!activeControlPoint_.isValid() ||
        !objectIds_.contains(activeControlPoint_.objectId)) {
        clearActiveControlPoint();
    }
}

void SelectionModel::setPrimaryObjectId(ObjectId objectId)
{
    primaryObjectId_ = objectId;
    if (!objectId.isValid() || !objectIds_.contains(objectId)) {
        clearActiveControlPoint();
    }
}

void SelectionModel::add(ObjectId objectId)
{
    if (!objectId.isValid()) {
        return;
    }
    if (!objectIds_.contains(objectId)) {
        objectIds_.append(objectId);
    }
    primaryObjectId_ = objectId;
}

void SelectionModel::remove(ObjectId objectId)
{
    objectIds_.removeOne(objectId);
    if (primaryObjectId_ == objectId) {
        primaryObjectId_ = objectIds_.isEmpty() ? ObjectId::invalid() : objectIds_.back();
    }
    if (activeControlPoint_.objectId == objectId) {
        clearActiveControlPoint();
    }
}

void SelectionModel::toggle(ObjectId objectId)
{
    if (contains(objectId)) {
        remove(objectId);
    } else {
        add(objectId);
    }
}

void SelectionModel::prune(const Document &document)
{
    for (auto iterator = objectIds_.begin(); iterator != objectIds_.end();) {
        if (document.indexOf(*iterator) < 0) {
            iterator = objectIds_.erase(iterator);
        } else {
            ++iterator;
        }
    }

    if (document.indexOf(primaryObjectId_) < 0) {
        primaryObjectId_ = objectIds_.isEmpty() ? ObjectId::invalid() : objectIds_.back();
    }
    if (document.indexOf(activeControlPoint_.objectId) < 0 ||
        !objectIds_.contains(activeControlPoint_.objectId)) {
        clearActiveControlPoint();
    }
}

const ControlPointReference &SelectionModel::activeControlPoint() const
{
    return activeControlPoint_;
}

int &SelectionModel::activeControlPointIndex()
{
    return activeControlPoint_.index;
}

void SelectionModel::setActiveControlPoint(ObjectId objectId, int index)
{
    if (objectId.isValid() && index >= 0) {
        activeControlPoint_ = ControlPointReference{objectId, index};
    } else {
        clearActiveControlPoint();
    }
}

void SelectionModel::clearActiveControlPoint()
{
    activeControlPoint_ = ControlPointReference{};
}

} // namespace classiCAD
