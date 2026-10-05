#include "duplicate_tool.h"

#include "services/snapping/snap_types.h"

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
    pickingBasePoint_ = true;
    return true;
}

void DuplicateTool::beginInPlace()
{
    if (!active_) {
        return;
    }
    pickingBasePoint_ = false;
    hasBasePoint_ = true;
    basePoint_ = {};
    cursorOffset_ = {};
    destination_ = {};
    previewShapes_.clear();
    previewPlacementTranslations_.clear();
    previewShapes_.reserve(sourceObjects_.size());
    previewPlacementTranslations_.reserve(sourceObjects_.size());
    for (const SceneObject &source : sourceObjects_) {
        previewShapes_.append(source.geometry);
        previewPlacementTranslations_.append(source.placementTranslation);
    }
}

void DuplicateTool::chooseBasePoint(const QPointF &rawPosition,
                                    const QPointF &resolvedBasePoint)
{
    if (!active_ || !pickingBasePoint_) {
        return;
    }
    basePoint_ = resolvedBasePoint;
    cursorOffset_ = rawPosition - basePoint_;
    destination_ = basePoint_;
    hasBasePoint_ = true;
    pickingBasePoint_ = false;
}

void DuplicateTool::updatePlacement(
    const QPointF &destinationCursor,
    const SnapResult &destinationSnap,
    const std::function<void(Shape &, Point3D &, const QPointF &)> &translate)
{
    if (!active_ || !hasBasePoint_ || !translate) {
        return;
    }
    destination_ = destinationSnap.isValid() ? destinationSnap.point
                                              : destinationCursor;
    const QPointF delta = destination_ - basePoint_;
    previewShapes_.clear();
    previewPlacementTranslations_.clear();
    previewShapes_.reserve(sourceObjects_.size());
    previewPlacementTranslations_.reserve(sourceObjects_.size());
    for (const SceneObject &source : sourceObjects_) {
        Shape preview = source.geometry;
        Point3D placement = source.placementTranslation;
        translate(preview, placement, delta);
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
    basePoint_ = {};
    cursorOffset_ = {};
    destination_ = {};
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

QPointF DuplicateTool::basePoint() const
{
    return basePoint_;
}

QPointF DuplicateTool::cursorOffset() const
{
    return cursorOffset_;
}

QPointF DuplicateTool::destination() const
{
    return destination_;
}

} // namespace classiCAD
