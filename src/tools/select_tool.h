#pragma once

#include "tool.h"

namespace classiCAD {

// SelectTool owns the selection transitions. Viewport-specific hit queries and
// drag/box presentation are supplied through the narrow ToolContext callbacks.
class SelectTool final : public InteractionTool {
public:
    ToolId id() const override;
    void begin(ToolContext &context) override;
    bool handleMousePress(const ToolInput &input, ToolContext &context) override;
    bool handleMouseMove(const ToolInput &input, ToolContext &context) override;
    bool handleKey(const ToolInput &input, ToolContext &context) override;
    ToolStatus status() const override;

    bool beginControlPointSelectionDrag(const ToolInput &input,
                                        ObjectId objectId,
                                        int controlPointIndex,
                                        ToolContext &context);

    void beginSelectionBox(const QPointF &screenPosition, bool additive);
    void updateSelectionBox(const QPointF &screenPosition);
    void cancelSelectionBox();
    bool hasSelectionBox() const;
    bool selectionBoxMoved() const;
    bool selectionBoxAdditive() const;
    QPointF selectionBoxStart() const;
    QPointF selectionBoxCurrent() const;
    void finishBoxSelection(const QVector<ObjectId> &objectIds,
                            ToolContext &context);

    void beginObjectDrag(const QVector<ObjectId> &objectIds,
                         const QPointF &screenPosition,
                         const QPointF &worldPosition,
                         bool gestureStarted = false);
    void beginControlPointDrag(ObjectId objectId,
                               int controlPointIndex,
                               const QPointF &screenPosition,
                               const QPointF &worldPosition);
    void clearDragState();
    bool isObjectDragActive() const;
    bool isControlPointDragActive() const;
    bool dragGestureStarted() const;
    void setDragGestureStarted(bool started);
    const QVector<ObjectId> &draggedObjectIds() const;
    QPointF dragStartScreenPosition() const;
    QPointF lastDragWorldPosition() const;
    void setLastDragWorldPosition(const QPointF &position);
    QPointF lastControlPointWorldPosition() const;
    void setLastControlPointWorldPosition(const QPointF &position);

private:
    enum class DragKind {
        None,
        Objects,
        ControlPoint,
    };

    void resetSelectionBox();
    void resetDragState();

    ToolStatus status_;
    DragKind dragKind_ = DragKind::None;
    QVector<ObjectId> draggedObjectIds_;
    ObjectId draggedControlPointObjectId_ = ObjectId::invalid();
    int draggedControlPointIndex_ = -1;
    bool dragGestureStarted_ = false;
    QPointF dragStartScreenPosition_;
    QPointF lastDragWorldPosition_;
    QPointF lastControlPointWorldPosition_;
    bool selectionBoxActive_ = false;
    bool selectionBoxMoved_ = false;
    bool selectionBoxAdditive_ = false;
    QPointF selectionBoxStart_;
    QPointF selectionBoxCurrent_;
};

} // namespace classiCAD
