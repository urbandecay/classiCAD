#include "mirror_tool.h"

#include "core/commands/mirror_command.h"
#include "core/document/selection_model.h"
#include "services/dimensions/dimension_association.h"
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
        interactionState_.axisFrame = isValidWorkPlaneFrame(input.workPlaneFrame)
                                          ? input.workPlaneFrame
                                          : makeWorkPlaneFrame(WorkPlane::XY, 0.0);
        interactionState_.axisStart = isComponentEdit()
                                          ? input.positionInFrame(
                                                interactionState_.axisFrame)
                                          : input.worldPosition;
        interactionState_.hasAxisStart = true;
        status_.text = QStringLiteral("Mirror: choose the axis direction");
        status_.canCommit = false;
    } else {
        interactionState_.axisEnd = isComponentEdit()
                                        ? input.positionInFrame(
                                              interactionState_.axisFrame)
                                        : input.worldPosition;
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
    if (interactionState_.globalAxisMirror) {
        if (interactionState_.globalAxis < 0) {
            return EventResult::Handled;
        }
        lastCommitResult_ = commitGlobalAxis(context);
        hasLastCommitResult_ = true;
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
    hasLastCommitResult_ = false;
    if (interactionState_.globalAxisMirror) {
        if (input.key == Qt::Key_Escape) {
            clearInteraction();
            context.finishTool(ToolId::Select);
            return EventResult::Handled;
        }
        if (input.modifiers == Qt::NoModifier &&
            (input.key == Qt::Key_X || input.key == Qt::Key_Y ||
             input.key == Qt::Key_Z)) {
            interactionState_.globalAxis = input.key == Qt::Key_X ? 0
                                           : input.key == Qt::Key_Y ? 1
                                                                    : 2;
            const QChar axisName = input.key == Qt::Key_X ? QLatin1Char('X')
                                  : input.key == Qt::Key_Y ? QLatin1Char('Y')
                                                           : QLatin1Char('Z');
            status_.text = QStringLiteral(
                "Mirror: global %1 plane; press Enter or click to confirm")
                               .arg(axisName);
            status_.canCommit = true;
            context.publishStatus(status_);
            return EventResult::Handled;
        }
        if (input.modifiers == Qt::NoModifier &&
            (input.key == Qt::Key_Return || input.key == Qt::Key_Enter) &&
            interactionState_.globalAxis >= 0) {
            lastCommitResult_ = commitGlobalAxis(context);
            hasLastCommitResult_ = true;
            return EventResult::Handled;
        }
        return EventResult::Unhandled;
    }
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
    result.componentEdit = isComponentEdit();
    for (auto target = interactionState_.controlPointIndices.cbegin();
         target != interactionState_.controlPointIndices.cend(); ++target) {
        result.controlPointCount += target.value().size();
    }
    result.axisStart = interactionState_.axisStart;
    result.axisEnd = interactionState_.axisEnd;
    DocumentTransaction transaction = context.beginTransaction();
    const bool applied = result.componentEdit
        ? MirrorCommand::applyControlPoints(
              context.document(), transaction,
              interactionState_.sourceObjectIds,
              interactionState_.controlPointIndices,
              interactionState_.axisStart,
              interactionState_.axisEnd,
              interactionState_.axisFrame)
        : MirrorCommand::apply(context.document(), transaction,
                               interactionState_.sourceObjectIds,
                               interactionState_.axisStart,
                               interactionState_.axisEnd,
                               &result.createdObjectIds);
    if (!applied) {
        rejectAxisEndpoint();
        result.createdObjectIds.clear();
        return result;
    }
    if (!context.commitTransaction(transaction)) {
        result.createdObjectIds.clear();
        return result;
    }

    if (result.componentEdit) {
        updateAssociativeDimensions(context.document(), context.curveSampler());
    } else {
        context.selection().setObjectIds(result.createdObjectIds);
        context.notifySelectionChanged();
    }
    context.notifyCollectionsChanged();
    clearInteraction();
    context.finishTool(ToolId::Select);
    result.committed = true;
    return result;
}

MirrorCommitResult MirrorTool::commitGlobalAxis(ToolContext &context)
{
    MirrorCommitResult result;
    if (!interactionState_.globalAxisMirror ||
        interactionState_.globalAxis < 0 ||
        interactionState_.sourceObjectIds.isEmpty() ||
        !isComponentEdit()) {
        return result;
    }

    result.sourceCount = interactionState_.sourceObjectIds.size();
    result.componentEdit = true;
    result.globalAxisMirror = true;
    result.globalAxis = interactionState_.globalAxis;
    result.pivot = interactionState_.pivot;
    for (auto target = interactionState_.controlPointIndices.cbegin();
         target != interactionState_.controlPointIndices.cend(); ++target) {
        result.controlPointCount += target.value().size();
    }

    DocumentTransaction transaction = context.beginTransaction();
    const bool applied = MirrorCommand::applyControlPointsAcrossGlobalAxisPlane(
        context.document(), transaction,
        interactionState_.sourceObjectIds,
        interactionState_.controlPointIndices,
        interactionState_.pivot,
        interactionState_.globalAxis);
    if (!applied) {
        status_.text = QStringLiteral(
            "Mirror could not transform the selected components");
        status_.canCommit = false;
        context.publishStatus(status_);
        return result;
    }
    if (!context.commitTransaction(transaction)) {
        return result;
    }

    updateAssociativeDimensions(context.document(), context.curveSampler());
    context.notifyCollectionsChanged();
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

void MirrorTool::setComponentControlPointTargets(
    const QHash<quint64, QSet<int>> &controlPointIndices)
{
    interactionState_.controlPointIndices = controlPointIndices;
}

void MirrorTool::beginGlobalAxisComponentMirror(const Point3D &pivot,
                                                ToolContext &context)
{
    interactionState_.globalAxisMirror = true;
    interactionState_.globalAxis = -1;
    interactionState_.pivot = pivot;
    status_.text = QStringLiteral("Mirror: choose global X, Y, or Z plane");
    status_.canCommit = false;
    context.publishStatus(status_);
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
    return interactionState_.globalAxisMirror
               ? interactionState_.globalAxis >= 0
               : interactionState_.hasAxisStart;
}

bool MirrorTool::hasCompleteAxis() const
{
    return interactionState_.globalAxisMirror
               ? interactionState_.globalAxis >= 0
               : interactionState_.hasCompleteAxis;
}

const QPointF &MirrorTool::axisStart() const
{
    return interactionState_.axisStart;
}

const QPointF &MirrorTool::axisEnd() const
{
    return interactionState_.axisEnd;
}

const WorkPlaneFrame &MirrorTool::axisFrame() const
{
    return interactionState_.axisFrame;
}

bool MirrorTool::isComponentEdit() const
{
    return !interactionState_.controlPointIndices.isEmpty();
}

bool MirrorTool::isGlobalAxisMirror() const
{
    return interactionState_.globalAxisMirror;
}

int MirrorTool::globalAxis() const
{
    return interactionState_.globalAxis;
}

const Point3D &MirrorTool::mirrorPivot() const
{
    return interactionState_.pivot;
}

const QHash<quint64, QSet<int>> &MirrorTool::controlPointIndices() const
{
    return interactionState_.controlPointIndices;
}

const QVector<ObjectId> &MirrorTool::sourceObjectIds() const
{
    return interactionState_.sourceObjectIds;
}

} // namespace classiCAD
