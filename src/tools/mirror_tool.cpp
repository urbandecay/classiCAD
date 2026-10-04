#include "mirror_tool.h"

#include "core/commands/mirror_command.h"
#include "core/document/selection_model.h"
#include "tool_context.h"

namespace classiCAD {

ToolId MirrorTool::id() const
{
    return ToolId::Mirror;
}

void MirrorTool::begin(ToolContext &context)
{
    interactionState_ = InteractionState{};
    lastCommitResult_ = MirrorCommitResult{};
    hasLastCommitResult_ = false;
    status_.state = ToolLifecycleState::Active;
    status_.text = QStringLiteral("Mirror: draw an axis");
    status_.canCommit = false;
    context.publishStatus(status_);
}

bool MirrorTool::handleMousePress(const ToolInput &input, ToolContext &context)
{
    Q_UNUSED(context);
    if (input.button != Qt::LeftButton ||
        interactionState_.sourceObjectIds.isEmpty()) {
        return false;
    }

    if (!interactionState_.hasAxisStart) {
        interactionState_.axisStart = input.worldPosition;
        interactionState_.hasAxisStart = true;
        status_.text = QStringLiteral("Mirror: choose the axis direction");
        status_.canCommit = false;
    } else {
        interactionState_.axisEnd = input.worldPosition;
        interactionState_.hasCompleteAxis = true;
        status_.text = QStringLiteral("Mirror: axis ready");
        status_.canCommit = true;
    }
    return true;
}

InteractionTool::EventResult MirrorTool::dispatchMousePress(
    const ToolInput &input, ToolContext &context)
{
    if (input.button == Qt::RightButton) {
        clearInteraction();
        hasLastCommitResult_ = false;
        context.finishTool(ToolId::Select);
        return EventResult::Handled;
    }
    if (input.button != Qt::LeftButton) {
        return EventResult::Unhandled;
    }

    hasLastCommitResult_ = false;
    if (interactionState_.sourceObjectIds.isEmpty()) {
        return EventResult::Handled;
    }
    if (!handleMousePress(input, context)) {
        return EventResult::Unhandled;
    }
    if (hasCompleteAxis()) {
        lastCommitResult_ = commitAxis(context);
        hasLastCommitResult_ = true;
    }
    return EventResult::Handled;
}

InteractionTool::EventResult MirrorTool::dispatchKey(
    const ToolInput &input, ToolContext &context)
{
    if (input.key != Qt::Key_Escape) {
        return EventResult::Unhandled;
    }
    clearInteraction();
    context.finishTool(ToolId::Select);
    return EventResult::Handled;
}

MirrorCommitResult MirrorTool::commitAxis(ToolContext &context)
{
    MirrorCommitResult result;
    if (!interactionState_.hasCompleteAxis ||
        interactionState_.sourceObjectIds.isEmpty()) {
        return result;
    }

    result.sourceCount = interactionState_.sourceObjectIds.size();
    result.axisStart = interactionState_.axisStart;
    result.axisEnd = interactionState_.axisEnd;
    DocumentTransaction transaction = context.beginTransaction();
    if (!MirrorCommand::apply(context.document(), transaction,
                              interactionState_.sourceObjectIds,
                              interactionState_.axisStart,
                              interactionState_.axisEnd,
                              &result.createdObjectIds)) {
        rejectAxisEndpoint();
        result.createdObjectIds.clear();
        return result;
    }
    if (!context.commitTransaction(transaction)) {
        result.createdObjectIds.clear();
        return result;
    }

    context.selection().setObjectIds(result.createdObjectIds);
    context.notifySelectionChanged();
    context.notifyLayersChanged();
    clearInteraction();
    context.finishTool(ToolId::Select);
    result.committed = true;
    return result;
}

MirrorCommitResult MirrorTool::takeLastCommitResult()
{
    if (!hasLastCommitResult_) {
        return {};
    }
    hasLastCommitResult_ = false;
    MirrorCommitResult result = lastCommitResult_;
    lastCommitResult_ = MirrorCommitResult{};
    return result;
}

void MirrorTool::cancel(ToolContext &context)
{
    clearInteraction();
    status_.state = ToolLifecycleState::Cancelled;
    status_.text = QStringLiteral("Mirror cancelled");
    status_.canCommit = false;
    context.publishStatus(status_);
}

ToolStatus MirrorTool::status() const
{
    return status_;
}

void MirrorTool::setSourceObjectIds(const QVector<ObjectId> &objectIds)
{
    interactionState_.sourceObjectIds = objectIds;
}

void MirrorTool::clearInteraction()
{
    interactionState_ = InteractionState{};
}

void MirrorTool::rejectAxisEndpoint()
{
    interactionState_.axisEnd = QPointF();
    interactionState_.hasCompleteAxis = false;
}

bool MirrorTool::hasAxisStart() const
{
    return interactionState_.hasAxisStart;
}

bool MirrorTool::hasCompleteAxis() const
{
    return interactionState_.hasCompleteAxis;
}

const QPointF &MirrorTool::axisStart() const
{
    return interactionState_.axisStart;
}

const QPointF &MirrorTool::axisEnd() const
{
    return interactionState_.axisEnd;
}

const QVector<ObjectId> &MirrorTool::sourceObjectIds() const
{
    return interactionState_.sourceObjectIds;
}

} // namespace classiCAD
