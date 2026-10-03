#pragma once

#include "tool.h"

namespace classiCAD {

class PointConstructionTool final : public InteractionTool {
public:
    explicit PointConstructionTool(ToolId tool);

    ToolId id() const override;
    void begin(ToolContext &context) override;
    bool handleMousePress(const ToolInput &input, ToolContext &context) override;
    bool handleMouseMove(const ToolInput &input, ToolContext &context) override;
    bool handleKey(const ToolInput &input, ToolContext &context) override;
    void cancel(ToolContext &context) override;
    ToolPreview preview() const override;
    ToolStatus status() const override;

private:
    bool addPointOnCurve(const Shape &shape,
                         const QPointF &cursor,
                         bool edgeCenter,
                         QPointF *point,
                         WorkPlaneFrame *frame) const;
    void finishPointChain(ToolContext &context);
    void publish(ToolContext &context);

    ToolId tool_;
    QVector<QPointF> points_;
    QVector<QPointF> arcCenters_;
    QVector<qreal> arcRadii_;
    QVector<qreal> arcStartAngles_;
    QVector<qreal> arcSweepAngles_;
    WorkPlaneFrame drawingFrame_;
    WorkPlaneFrame frameBeforeAxisLock_;
    QPointF cursorPoint_;
    bool hasCursorPoint_ = false;
    bool planeLocked_ = false;
    int normalAxisLockKey_ = 0;
    int arcStage_ = 0;
    qreal previousArcAngle_ = 0.0;
    bool hasPreviousArcAngle_ = false;
    ToolStatus status_;
};

} // namespace classiCAD
