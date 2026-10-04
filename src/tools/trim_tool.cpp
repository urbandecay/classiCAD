#include "trim_tool.h"

#include "tool_context.h"

#include <cmath>

namespace classiCAD {

ToolId TrimTool::id() const
{
    return ToolId::Trim;
}

void TrimTool::begin(ToolContext &context)
{
    resetInteraction();
    status_.state = ToolLifecycleState::Active;
    status_.text = QStringLiteral("Trim");
    status_.canCommit = false;
    context.publishStatus(status_);
}

void TrimTool::cancel(ToolContext &context)
{
    resetInteraction();
    status_.state = ToolLifecycleState::Cancelled;
    status_.canCommit = false;
    context.publishStatus(status_);
}

ToolStatus TrimTool::status() const
{
    return status_;
}

void TrimTool::resetInteraction()
{
    clearBoxSelection();
    invalidateHover();
    clearCandidates();
    clearScreenPath();
}

void TrimTool::beginBoxSelection(const QPointF &screenPosition)
{
    boxSelectionActive_ = true;
    boxSelectionMoved_ = false;
    boxStartPosition_ = screenPosition;
    boxCurrentPosition_ = screenPosition;
}

void TrimTool::updateBoxSelection(const QPointF &screenPosition)
{
    if (!boxSelectionActive_) {
        return;
    }
    boxCurrentPosition_ = screenPosition;
    const QPointF delta = boxCurrentPosition_ - boxStartPosition_;
    if (std::hypot(delta.x(), delta.y()) >= 3.0) {
        boxSelectionMoved_ = true;
    }
}

void TrimTool::clearBoxSelection()
{
    boxSelectionActive_ = false;
    boxSelectionMoved_ = false;
    boxStartPosition_ = {};
    boxCurrentPosition_ = {};
}

bool TrimTool::boxSelectionActive() const
{
    return boxSelectionActive_;
}

bool TrimTool::boxSelectionMoved() const
{
    return boxSelectionMoved_;
}

QPointF TrimTool::boxStartPosition() const
{
    return boxStartPosition_;
}

QPointF TrimTool::boxCurrentPosition() const
{
    return boxCurrentPosition_;
}

void TrimTool::invalidateHover()
{
    hoverPositionValid_ = false;
    hoverComponentIndex_ = -1;
}

bool TrimTool::hoverPositionValid() const
{
    return hoverPositionValid_;
}

bool TrimTool::hoverPositionMatches(const QPointF &screenPosition) const
{
    return hoverPositionValid_ && hoverScreenPosition_ == screenPosition;
}

void TrimTool::setHoverPosition(const QPointF &screenPosition,
                                int componentIndex)
{
    hoverScreenPosition_ = screenPosition;
    hoverPositionValid_ = true;
    hoverComponentIndex_ = componentIndex;
}

int TrimTool::hoverComponentIndex() const
{
    return hoverComponentIndex_;
}

TrimTool::HoverTarget TrimTool::updateHover(
    const QPointF &screenPosition,
    const QVector<ObjectId> &targetObjectIds,
    ObjectId primarySelection,
    qreal hitRadius,
    const CurveDistanceQuery &distanceQuery)
{
    HoverTarget closest;
    closest.distance = hitRadius;
    clearCandidates();
    for (const ObjectId objectId : targetObjectIds) {
        if (!objectId.isValid() || !distanceQuery) {
            continue;
        }
        int componentIndex = -1;
        const qreal distance = distanceQuery(objectId, &componentIndex);
        const bool closer = distance < closest.distance - 1.0e-6;
        const bool tieOnActiveSelection =
            std::abs(distance - closest.distance) <= 1.0e-6 &&
            objectId == primarySelection;
        if (closer || tieOnActiveSelection) {
            closest.objectId = objectId;
            closest.distance = distance;
            closest.componentIndex = componentIndex;
        }
    }
    setHoverPosition(screenPosition, closest.componentIndex);
    if (closest.isValid()) {
        addCandidate(closest.objectId);
    }
    return closest;
}

ObjectId TrimTool::primaryCandidate() const
{
    return candidateObjectIds_.isEmpty() ? ObjectId::invalid()
                                         : candidateObjectIds_.first();
}

int TrimTool::selectBoxCandidates(
    const QVector<ObjectId> &targetObjectIds,
    const BoxTargetPredicate &matchesBox)
{
    clearCandidates();
    for (const ObjectId objectId : targetObjectIds) {
        if (objectId.isValid() && matchesBox && matchesBox(objectId)) {
            addCandidate(objectId);
        }
    }
    return candidateObjectIds_.size();
}

QVector<ObjectId> &TrimTool::candidateObjectIds()
{
    return candidateObjectIds_;
}

const QVector<ObjectId> &TrimTool::candidateObjectIds() const
{
    return candidateObjectIds_;
}

void TrimTool::clearCandidates()
{
    candidateObjectIds_.clear();
}

bool TrimTool::addCandidate(ObjectId objectId)
{
    if (!objectId.isValid() || candidateObjectIds_.contains(objectId)) {
        return false;
    }
    candidateObjectIds_.append(objectId);
    return true;
}

QVector<QPointF> &TrimTool::screenPath()
{
    return screenPath_;
}

const QVector<QPointF> &TrimTool::screenPath() const
{
    return screenPath_;
}

void TrimTool::clearScreenPath()
{
    screenPath_.clear();
}

void TrimTool::setSinglePointScreenPath(const QPointF &screenPosition)
{
    screenPath_ = {screenPosition};
}

} // namespace classiCAD
