#pragma once

#include "tool.h"

namespace classiCAD {

class DimensionTool final : public InteractionTool {
public:
    explicit DimensionTool(ToolId tool);

    ToolId id() const override;
    void begin(ToolContext &context) override;
    bool handleMousePress(const ToolInput &input, ToolContext &context) override;
    bool handleMouseMove(const ToolInput &input, ToolContext &context) override;
    bool handleKey(const ToolInput &input, ToolContext &context) override;
    void cancel(ToolContext &context) override;
    ToolPreview preview() const override;
    ToolStatus status() const override;

private:
    bool makeShape(const QVector<QPointF> &points, Shape *shape) const;
    void publish(ToolContext &context);

    ToolId tool_;
    QVector<QPointF> fixedPoints_;
    QPointF cursor_;
    qreal minimumOffsetWorld_ = 2.0;
    bool cursorValid_ = false;
    ToolStatus status_;
};

} // namespace classiCAD
