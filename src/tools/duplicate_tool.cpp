#include "duplicate_tool.h"

#include <utility>

namespace classiCAD {

bool DuplicateTool::begin(const Document &document,
                          const QVector<ObjectId> &selectedObjectIds)
{
    reset();
    sourceObjects_.reserve(selectedObjectIds.size());
    for (const ObjectId objectId : selectedObjectIds) {
        const SceneObject *sceneObject = document.object(objectId);
        if (sceneObject != nullptr && document.isObjectEditable(objectId)) {
            sourceObjects_.append(*sceneObject);
        }
    }
    if (sourceObjects_.isEmpty()) {
        return false;
    }
    active_ = true;
    return true;
}

void DuplicateTool::beginMove(const Point3D &basePointWorld)
{
    if (!active_) {
        return;
    }
    pickingBasePoint_ = false;
    hasBasePoint_ = true;
    basePointWorld_ = basePointWorld;
    worldDelta_ = {};
    previewShapes_.clear();
    previewPlacementTranslations_.clear();
    previewShapes_.reserve(sourceObjects_.size());
    previewPlacementTranslations_.reserve(sourceObjects_.size());
    for (const SceneObject &source : sourceObjects_) {
        previewShapes_.append(source.geometry);
        previewPlacementTranslations_.append(source.placementTranslation);
    }
}

void DuplicateTool::beginInPlace()
{
    if (!active_) {
        return;
    }
    pickingBasePoint_ = false;
    hasBasePoint_ = true;
    previewShapes_.clear();
    previewPlacementTranslations_.clear();
    previewShapes_.reserve(sourceObjects_.size());
    previewPlacementTranslations_.reserve(sourceObjects_.size());
    for (const SceneObject &source : sourceObjects_) {
        previewShapes_.append(source.geometry);
        previewPlacementTranslations_.append(source.placementTranslation);
    }
    basePointWorld_ = {};
    worldDelta_ = {};
}

void DuplicateTool::updatePlacementWorld(
    const Point3D &worldDelta,
    const std::function<void(Shape &, Point3D &, const Point3D &)> &translate)
{
    if (!active_ || !hasBasePoint_ || !translate) {
        return;
    }
    worldDelta_ = worldDelta;
    previewShapes_.clear();
    previewPlacementTranslations_.clear();
    previewShapes_.reserve(sourceObjects_.size());
    previewPlacementTranslations_.reserve(sourceObjects_.size());
    for (const SceneObject &source : sourceObjects_) {
        Shape preview = source.geometry;
        Point3D placement = source.placementTranslation;
        translate(preview, placement, worldDelta);
        previewShapes_.append(preview);
        previewPlacementTranslations_.append(placement);
    }
}

void DuplicateTool::reset()
{
    active_ = false;
    pickingBasePoint_ = false;
    hasBasePoint_ = false;
    sourceObjects_.clear();
    previewShapes_.clear();
    previewPlacementTranslations_.clear();
    basePointWorld_ = {};
    worldDelta_ = {};
}

bool DuplicateTool::isActive() const
{
    return active_;
}

bool DuplicateTool::isPickingBasePoint() const
{
    return pickingBasePoint_;
}

bool DuplicateTool::hasBasePoint() const
{
    return hasBasePoint_;
}

const QVector<SceneObject> &DuplicateTool::sourceObjects() const
{
    return sourceObjects_;
}

QVector<ObjectId> DuplicateTool::sourceObjectIds() const
{
    QVector<ObjectId> objectIds;
    objectIds.reserve(sourceObjects_.size());
    for (const SceneObject &source : sourceObjects_) {
        objectIds.append(source.id);
    }
    return objectIds;
}

const QVector<Shape> &DuplicateTool::previewShapes() const
{
    return previewShapes_;
}

const QVector<Point3D> &DuplicateTool::previewPlacementTranslations() const
{
    return previewPlacementTranslations_;
}

Point3D DuplicateTool::basePointWorld() const
{
    return basePointWorld_;
}

Point3D DuplicateTool::worldDelta() const
{
    return worldDelta_;
}

} // namespace classiCAD
