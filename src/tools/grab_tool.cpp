#include "grab_tool.h"

namespace classiCAD {

bool GrabTool::begin(Document &document,
                     const QVector<ObjectId> &selectedObjectIds,
                     const QPointF &startWorldPosition)
{
    reset();
    objectIds_.reserve(selectedObjectIds.size());
    sourceGeometry_.reserve(selectedObjectIds.size());
    sourcePlacementTranslations_.reserve(selectedObjectIds.size());
    for (const ObjectId objectId : selectedObjectIds) {
        const SceneObject *sceneObject = document.object(objectId);
        if (sceneObject != nullptr && document.isObjectEditable(objectId) &&
            !objectIds_.contains(objectId)) {
            objectIds_.append(objectId);
            sourceGeometry_.append(sceneObject->geometry);
            sourcePlacementTranslations_.append(sceneObject->placementTranslation);
        }
    }
    if (objectIds_.isEmpty()) {
        return false;
    }

    startSnapshot_ = document.snapshot();
    startWorldPosition_ = startWorldPosition;
    active_ = true;
    return true;
}

bool GrabTool::enterBasePointMode(Document &document)
{
    if (!active_) {
        return false;
    }

    const bool restored = moved_;
    if (restored) {
        restoreSourceGeometry(document);
    }
    moved_ = false;
    pickingBasePoint_ = true;
    hasBasePoint_ = false;
    basePoint_ = {};
    basePointWorld_ = {};
    basePointDragPlane_ = {};
    cursorOffset_ = {};
    cursorOffsetScreen_ = {};
    return restored;
}

void GrabTool::restoreSourceGeometry(Document &document,
                                     bool restoreSurfaceGeometry) const
{
    for (int i = 0; i < objectIds_.size(); ++i) {
        const GeometryType type = sourceGeometry_[i].geometryType;
        if (restoreSurfaceGeometry ||
            (type != GeometryType::NurbsSurface && type != GeometryType::NurbsSolid)) {
            document.mutateGeometry(objectIds_[i], [this, i](Shape &geometry) {
                geometry = sourceGeometry_[i];
                return true;
            });
        }
    }
    document.setObjectPlacementTranslations(objectIds_,
                                            sourcePlacementTranslations_);
}

void GrabTool::acceptBasePoint(const QPointF &basePoint,
                               const QPointF &cursorOffset,
                               const Point3D &basePointWorld,
                               const QPointF &basePointDragPlane,
                               const QPointF &cursorOffsetScreen)
{
    if (!active_ || !pickingBasePoint_) {
        return;
    }
    basePoint_ = basePoint;
    basePointWorld_ = basePointWorld;
    basePointDragPlane_ = basePointDragPlane;
    cursorOffset_ = cursorOffset;
    cursorOffsetScreen_ = cursorOffsetScreen;
    hasBasePoint_ = true;
    pickingBasePoint_ = false;
}

void GrabTool::setMoved(bool moved)
{
    moved_ = moved;
}

void GrabTool::reset()
{
    active_ = false;
    moved_ = false;
    pickingBasePoint_ = false;
    hasBasePoint_ = false;
    objectIds_.clear();
    sourceGeometry_.clear();
    sourcePlacementTranslations_.clear();
    startSnapshot_ = Document::Snapshot{};
    startWorldPosition_ = {};
    basePoint_ = {};
    basePointWorld_ = {};
    basePointDragPlane_ = {};
    cursorOffset_ = {};
    cursorOffsetScreen_ = {};
}

bool GrabTool::isActive() const
{
    return active_;
}

bool GrabTool::moved() const
{
    return moved_;
}

bool GrabTool::isPickingBasePoint() const
{
    return pickingBasePoint_;
}

bool GrabTool::hasBasePoint() const
{
    return hasBasePoint_;
}

const QVector<ObjectId> &GrabTool::objectIds() const
{
    return objectIds_;
}

const Document::Snapshot &GrabTool::startSnapshot() const
{
    return startSnapshot_;
}

QPointF GrabTool::startWorldPosition() const
{
    return startWorldPosition_;
}

QPointF GrabTool::basePoint() const
{
    return basePoint_;
}

Point3D GrabTool::basePointWorld() const
{
    return basePointWorld_;
}

QPointF GrabTool::basePointDragPlane() const
{
    return basePointDragPlane_;
}

QPointF GrabTool::cursorOffset() const
{
    return cursorOffset_;
}

QPointF GrabTool::cursorOffsetScreen() const
{
    return cursorOffsetScreen_;
}

} // namespace classiCAD
