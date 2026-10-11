#include "selection_model.h"

#include "document.h"

#include <utility>

namespace classiCAD {

void SelectionModel::setChangeNotifier(ChangeNotifier notifier)
{
    changeNotifier_ = std::move(notifier);
}

void SelectionModel::notifyChanged()
{
    if (changeNotifier_) {
        changeNotifier_();
    }
}

const QVector<ObjectId> &SelectionModel::objectIds() const
{
    return objectIds_;
}

const ObjectId &SelectionModel::primaryObjectId() const
{
    return primaryObjectId_;
}

bool SelectionModel::contains(ObjectId objectId) const
{
    return objectId.isValid() && objectIds_.contains(objectId);
}

void SelectionModel::clear()
{
    const bool changed = !objectIds_.isEmpty() || primaryObjectId_.isValid();
    objectIds_.clear();
    primaryObjectId_ = ObjectId::invalid();
    activeControlPoint_ = ControlPointReference{};
    if (changed) {
        notifyChanged();
    }
}

void SelectionModel::setObjectIds(const QVector<ObjectId> &objectIds,
                                  ObjectId primaryObjectId)
{
    const QVector<ObjectId> previousObjectIds = objectIds_;
    const ObjectId previousPrimaryObjectId = primaryObjectId_;
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
        activeControlPoint_ = ControlPointReference{};
    }
    if (previousObjectIds != objectIds_ ||
        previousPrimaryObjectId != primaryObjectId_) {
        notifyChanged();
    }
}

void SelectionModel::setPrimaryObjectId(ObjectId objectId)
{
    const ObjectId previousPrimaryObjectId = primaryObjectId_;
    if (objectId.isValid() && objectIds_.contains(objectId)) {
        primaryObjectId_ = objectId;
    } else {
        primaryObjectId_ = objectIds_.isEmpty()
                               ? ObjectId::invalid()
                               : objectIds_.back();
    }
    if (primaryObjectId_ != objectId) {
        activeControlPoint_ = ControlPointReference{};
    }
    if (previousPrimaryObjectId != primaryObjectId_) {
        notifyChanged();
    }
}

void SelectionModel::add(ObjectId objectId)
{
    if (!objectId.isValid()) {
        return;
    }
    const bool wasSelected = objectIds_.contains(objectId);
    const ObjectId previousPrimaryObjectId = primaryObjectId_;
    if (!wasSelected) {
        objectIds_.append(objectId);
    }
    primaryObjectId_ = objectId;
    if (!wasSelected || previousPrimaryObjectId != primaryObjectId_) {
        notifyChanged();
    }
}

void SelectionModel::remove(ObjectId objectId)
{
    const QVector<ObjectId> previousObjectIds = objectIds_;
    const ObjectId previousPrimaryObjectId = primaryObjectId_;
    objectIds_.removeOne(objectId);
    if (primaryObjectId_ == objectId) {
        primaryObjectId_ = objectIds_.isEmpty() ? ObjectId::invalid() : objectIds_.back();
    }
    if (activeControlPoint_.objectId == objectId) {
        activeControlPoint_ = ControlPointReference{};
    }
    if (previousObjectIds != objectIds_ ||
        previousPrimaryObjectId != primaryObjectId_) {
        notifyChanged();
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
    const QVector<ObjectId> previousObjectIds = objectIds_;
    const ObjectId previousPrimaryObjectId = primaryObjectId_;
    for (auto iterator = objectIds_.begin(); iterator != objectIds_.end();) {
        if (!document.isObjectEditable(*iterator)) {
            iterator = objectIds_.erase(iterator);
        } else {
            ++iterator;
        }
    }

    if (!document.isObjectEditable(primaryObjectId_)) {
        primaryObjectId_ = objectIds_.isEmpty() ? ObjectId::invalid() : objectIds_.back();
    }
    if (!document.isObjectEditable(activeControlPoint_.objectId) ||
        !objectIds_.contains(activeControlPoint_.objectId)) {
        activeControlPoint_ = ControlPointReference{};
    }
    if (previousObjectIds != objectIds_ ||
        previousPrimaryObjectId != primaryObjectId_) {
        notifyChanged();
    }
}

const ControlPointReference &SelectionModel::activeControlPoint() const
{
    return activeControlPoint_;
}

void SelectionModel::setActiveControlPoint(ObjectId objectId, int index)
{
    if (objectId.isValid() && objectIds_.contains(objectId) && index >= 0) {
        activeControlPoint_ = ControlPointReference{objectId, index};
    } else {
        activeControlPoint_ = ControlPointReference{};
    }
}

void SelectionModel::clearActiveControlPoint()
{
    activeControlPoint_ = ControlPointReference{};
}

} // namespace classiCAD
