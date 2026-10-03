#pragma once

#include "tool.h"

namespace classiCAD {

class CurveDrawingTool final : public InteractionTool {
public:
    explicit CurveDrawingTool(ToolId tool);

    ToolId id() const override;
    void begin(ToolContext &context) override;
    bool handleMousePress(const ToolInput &input, ToolContext &context) override;
    bool handleMouseMove(const ToolInput &input, ToolContext &context) override;
    bool handleKey(const ToolInput &input, ToolContext &context) override;
    void cancel(ToolContext &context) override;
    ToolPreview preview() const override;
    ToolStatus status() const override;

private:
    bool isFreehand() const;
    bool appendPoint(const QPointF &point);
    Shape makePreviewShape(bool includeCursor) const;
    void finish(ToolContext &context);
    void publish(ToolContext &context);

    ToolId tool_;
    QVector<QPointF> points_;
    WorkPlaneFrame drawingFrame_;
    WorkPlaneFrame frameBeforeAxisLock_;
    QPointF cursorPoint_;
    QPointF lastSampleScreen_;
    bool hasCursorPoint_ = false;
    bool hasSampleScreen_ = false;
    bool drawing_ = false;
    bool planeLocked_ = false;
    int normalAxisLockKey_ = 0;
    ToolStatus status_;
};

} // namespace classiCAD
