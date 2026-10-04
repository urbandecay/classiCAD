#include "erase_tool.h"

#include "tool_context.h"

#include <algorithm>
#include <cmath>

namespace classiCAD {

ToolId EraseTool::id() const
{
    return ToolId::Erase;
}

void EraseTool::begin(ToolContext &context)
{
    resetInteraction();
    status_.state = ToolLifecycleState::Active;
    status_.text = QStringLiteral("Erase");
    status_.canCommit = false;
    context.publishStatus(status_);
}

void EraseTool::cancel(ToolContext &context)
{
    resetInteraction();
    status_.state = ToolLifecycleState::Cancelled;
    status_.canCommit = false;
    context.publishStatus(status_);
}

ToolStatus EraseTool::status() const
{
    return status_;
}

void EraseTool::resetInteraction()
{
    strokeActive_ = false;
    cursorPressed_ = false;
    cursorScreenPosition_ = {};
    lastScreenPosition_ = {};
    screenPath_.clear();
    candidateObjectIds_.clear();
}

void EraseTool::beginStroke(const QPointF &screenPosition)
{
    strokeActive_ = true;
    cursorPressed_ = true;
    cursorScreenPosition_ = screenPosition;
    lastScreenPosition_ = screenPosition;
    screenPath_ = {screenPosition};
    candidateObjectIds_.clear();
}

void EraseTool::finishStroke()
{
    strokeActive_ = false;
    cursorPressed_ = false;
}

void EraseTool::cancelStroke()
{
    strokeActive_ = false;
    cursorPressed_ = false;
    candidateObjectIds_.clear();
    screenPath_.clear();
}

bool EraseTool::strokeActive() const
{
    return strokeActive_;
}

bool EraseTool::cursorPressed() const
{
    return cursorPressed_;
}

void EraseTool::setCursorPressed(bool pressed)
{
    cursorPressed_ = pressed;
}

QPointF EraseTool::cursorScreenPosition() const
{
    return cursorScreenPosition_;
}

void EraseTool::setCursorScreenPosition(const QPointF &screenPosition)
{
    cursorScreenPosition_ = screenPosition;
}

QPointF EraseTool::lastScreenPosition() const
{
    return lastScreenPosition_;
}

void EraseTool::setLastScreenPosition(const QPointF &screenPosition)
{
    lastScreenPosition_ = screenPosition;
}

void EraseTool::appendStrokeScreenPosition(const QPointF &screenPosition)
{
    if (screenPath_.isEmpty() || screenPath_.back() != screenPosition) {
        screenPath_.append(screenPosition);
    }
    lastScreenPosition_ = screenPosition;
    cursorScreenPosition_ = screenPosition;
}

int EraseTool::collectCandidatesAlongSegment(
    const QPointF &start,
    const QPointF &end,
    const QVector<ObjectId> &targetObjectIds,
    const ScreenDistanceQuery &distanceQuery,
    qreal hitRadiusPixels,
    qreal sampleSpacingPixels)
{
    if (!distanceQuery || targetObjectIds.isEmpty() ||
        hitRadiusPixels < 0.0 || sampleSpacingPixels <= 0.0) {
        return 0;
    }
    const qreal length = std::hypot(end.x() - start.x(), end.y() - start.y());
    const int sampleCount = std::max(
        1, static_cast<int>(std::ceil(length / sampleSpacingPixels)));
    int addedCandidates = 0;
    for (int sample = 0; sample <= sampleCount; ++sample) {
        const qreal fraction = static_cast<qreal>(sample) / sampleCount;
        const QPointF cursor = start + (end - start) * fraction;
        for (const ObjectId objectId : targetObjectIds) {
            if (!objectId.isValid() || candidateObjectIds_.contains(objectId)) {
                continue;
            }
            if (distanceQuery(cursor, objectId) <= hitRadiusPixels) {
                candidateObjectIds_.append(objectId);
                ++addedCandidates;
            }
        }
    }
    return addedCandidates;
}

QVector<QPointF> &EraseTool::screenPath()
{
    return screenPath_;
}

const QVector<QPointF> &EraseTool::screenPath() const
{
    return screenPath_;
}

void EraseTool::clearCandidates()
{
    candidateObjectIds_.clear();
}

bool EraseTool::addCandidate(ObjectId objectId)
{
    if (!objectId.isValid() || candidateObjectIds_.contains(objectId)) {
        return false;
    }
    candidateObjectIds_.append(objectId);
    return true;
}

QVector<ObjectId> &EraseTool::candidateObjectIds()
{
    return candidateObjectIds_;
}

const QVector<ObjectId> &EraseTool::candidateObjectIds() const
{
    return candidateObjectIds_;
}

} // namespace classiCAD
