#include "select_tool.h"

#include "tool_context.h"

#include "core/document/document.h"
#include "core/document/selection_model.h"

#include <cmath>

namespace classiCAD {

ToolId SelectTool::id() const
{
    return ToolId::Select;
}

void SelectTool::begin(ToolContext &context)
{
    status_.state = ToolLifecycleState::Active;
    status_.text = QStringLiteral("Select");
    status_.canCommit = false;
    context.publishStatus(status_);
}

bool SelectTool::handleMousePress(const ToolInput &input, ToolContext &context)
{
    if (input.button != Qt::LeftButton) {
        return false;
    }

    const bool additive = input.modifiers.testFlag(Qt::ShiftModifier);
    const SelectionHit hit = context.hitTestSelection(input.screenPosition,
                                                      !additive);
    SelectionModel &selection = context.selection();
    if (!additive && hit.isControlPoint()) {
        return beginControlPointSelectionDrag(input,
                                              hit.objectId,
                                              hit.controlPointIndex,
                                              context);
    }

    selection.clearActiveControlPoint();
    if (additive) {
        clearDragState();
        if (hit.objectId.isValid()) {
            selection.toggle(hit.objectId);
        } else {
            context.beginSelectionGesture(SelectionGestureKind::BeginSelectionBox,
                                          input,
                                          hit,
                                          true);
        }
        return true;
    }

    if (hit.objectId.isValid()) {
        if (selection.contains(hit.objectId)) {
            selection.setPrimaryObjectId(hit.objectId);
        } else {
            selection.setObjectIds({hit.objectId}, hit.objectId);
        }
        beginObjectDrag(selection.objectIds(),
                        input.screenPosition,
                        input.rawWorldPosition);
        context.beginSelectionGesture(SelectionGestureKind::BeginObjectDrag,
                                      input,
                                      hit,
                                      false);
    } else {
        clearDragState();
        context.beginSelectionGesture(SelectionGestureKind::BeginSelectionBox,
                                      input,
                                      hit,
                                      false);
    }
    return true;
}

bool SelectTool::beginControlPointSelectionDrag(const ToolInput &input,
                                                ObjectId objectId,
                                                int controlPointIndex,
                                                ToolContext &context)
{
    if (input.button != Qt::LeftButton || !objectId.isValid() ||
        controlPointIndex < 0 || !context.selection().contains(objectId)) {
        return false;
    }

    context.selection().setPrimaryObjectId(objectId);
    context.selection().setActiveControlPoint(objectId, controlPointIndex);
    beginControlPointDrag(objectId,
                          controlPointIndex,
                          input.screenPosition,
                          input.rawWorldPosition);
    context.beginSelectionGesture(SelectionGestureKind::BeginControlPointDrag,
                                  input,
                                  SelectionHit{objectId, controlPointIndex},
                                  false);
    return true;
}

bool SelectTool::handleKey(const ToolInput &input, ToolContext &context)
{
    if (input.key != Qt::Key_A || input.modifiers != Qt::NoModifier) {
        return false;
    }

    QVector<ObjectId> editableObjectIds;
    editableObjectIds.reserve(context.document().size());
    for (int index = 0; index < context.document().size(); ++index) {
        const ObjectId objectId = context.document().objectIdAt(index);
        if (context.document().isObjectEditable(objectId)) {
            editableObjectIds.append(objectId);
        }
    }
    context.selection().setObjectIds(editableObjectIds);
    return true;
}

bool SelectTool::handleMouseMove(const ToolInput &input, ToolContext &)
{
    if (!selectionBoxActive_) {
        return false;
    }
    updateSelectionBox(input.screenPosition);
    return true;
}

void SelectTool::beginSelectionBox(const QPointF &screenPosition, bool additive)
{
    selectionBoxActive_ = true;
    selectionBoxMoved_ = false;
    selectionBoxAdditive_ = additive;
    selectionBoxStart_ = screenPosition;
    selectionBoxCurrent_ = screenPosition;
}

void SelectTool::updateSelectionBox(const QPointF &screenPosition)
{
    if (!selectionBoxActive_) {
        return;
    }
    selectionBoxCurrent_ = screenPosition;
    const QPointF delta = selectionBoxCurrent_ - selectionBoxStart_;
    if (std::hypot(delta.x(), delta.y()) >= 3.0) {
        selectionBoxMoved_ = true;
    }
}

void SelectTool::cancelSelectionBox()
{
    resetSelectionBox();
}

bool SelectTool::hasSelectionBox() const
{
    return selectionBoxActive_;
}

bool SelectTool::selectionBoxMoved() const
{
    return selectionBoxMoved_;
}

bool SelectTool::selectionBoxAdditive() const
{
    return selectionBoxAdditive_;
}

QPointF SelectTool::selectionBoxStart() const
{
    return selectionBoxStart_;
}

QPointF SelectTool::selectionBoxCurrent() const
{
    return selectionBoxCurrent_;
}

void SelectTool::finishBoxSelection(const QVector<ObjectId> &objectIds,
                                    ToolContext &context)
{
    const QRectF box = QRectF(selectionBoxStart_, selectionBoxCurrent_).normalized();
    const bool moved = selectionBoxMoved_ ||
                       box.width() >= 3.0 || box.height() >= 3.0;
    const bool additive = selectionBoxAdditive_;
    resetSelectionBox();
    SelectionModel &selection = context.selection();
    if (!moved) {
        if (!additive) {
            selection.clear();
        }
        return;
    }

    QVector<ObjectId> editableObjectIds;
    editableObjectIds.reserve(objectIds.size());
    for (const ObjectId objectId : objectIds) {
        if (context.document().isObjectEditable(objectId)) {
            editableObjectIds.append(objectId);
        }
    }

    if (!additive) {
        selection.setObjectIds(editableObjectIds);
        return;
    }
    for (const ObjectId objectId : editableObjectIds) {
        selection.add(objectId);
    }
}

void SelectTool::beginObjectDrag(const QVector<ObjectId> &objectIds,
                                 const QPointF &screenPosition,
                                 const QPointF &worldPosition,
                                 bool gestureStarted)
{
    resetDragState();
    dragKind_ = objectIds.isEmpty() ? DragKind::None : DragKind::Objects;
    draggedObjectIds_ = objectIds;
    dragGestureStarted_ = gestureStarted;
    dragStartScreenPosition_ = screenPosition;
    lastDragWorldPosition_ = worldPosition;
}

void SelectTool::beginControlPointDrag(ObjectId objectId,
                                       int controlPointIndex,
                                       const QPointF &screenPosition,
                                       const QPointF &worldPosition)
{
    resetDragState();
    if (!objectId.isValid() || controlPointIndex < 0) {
        return;
    }
    dragKind_ = DragKind::ControlPoint;
    draggedControlPointObjectId_ = objectId;
    draggedControlPointIndex_ = controlPointIndex;
    dragStartScreenPosition_ = screenPosition;
    lastControlPointWorldPosition_ = worldPosition;
}

void SelectTool::clearDragState()
{
    resetDragState();
}

bool SelectTool::isObjectDragActive() const
{
    return dragKind_ == DragKind::Objects;
}

bool SelectTool::isControlPointDragActive() const
{
    return dragKind_ == DragKind::ControlPoint;
}

bool SelectTool::dragGestureStarted() const
{
    return dragGestureStarted_;
}

void SelectTool::setDragGestureStarted(bool started)
{
    dragGestureStarted_ = started;
}

const QVector<ObjectId> &SelectTool::draggedObjectIds() const
{
    return draggedObjectIds_;
}

QPointF SelectTool::dragStartScreenPosition() const
{
    return dragStartScreenPosition_;
}

QPointF SelectTool::lastDragWorldPosition() const
{
    return lastDragWorldPosition_;
}

void SelectTool::setLastDragWorldPosition(const QPointF &position)
{
    lastDragWorldPosition_ = position;
}

QPointF SelectTool::lastControlPointWorldPosition() const
{
    return lastControlPointWorldPosition_;
}

void SelectTool::setLastControlPointWorldPosition(const QPointF &position)
{
    lastControlPointWorldPosition_ = position;
}

void SelectTool::resetSelectionBox()
{
    selectionBoxActive_ = false;
    selectionBoxMoved_ = false;
    selectionBoxAdditive_ = false;
    selectionBoxStart_ = {};
    selectionBoxCurrent_ = {};
}

void SelectTool::resetDragState()
{
    dragKind_ = DragKind::None;
    draggedObjectIds_.clear();
    draggedControlPointObjectId_ = ObjectId::invalid();
    draggedControlPointIndex_ = -1;
    dragGestureStarted_ = false;
    dragStartScreenPosition_ = {};
    lastDragWorldPosition_ = {};
    lastControlPointWorldPosition_ = {};
}

ToolStatus SelectTool::status() const
{
    return status_;
}

} // namespace classiCAD
