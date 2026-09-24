#pragma once

#include "tool.h"

namespace classiCAD {

class ShapeCreationTool : public InteractionTool {
public:
    ShapeCreationTool(ToolId tool, int requiredPoints);

    ToolId id() const override;
    void begin(ToolContext &context) override;
    bool handleMousePress(const ToolInput &input, ToolContext &context) override;
    bool handleKey(const ToolInput &input, ToolContext &context) override;
    void cancel(ToolContext &context) override;
    ToolPreview preview() const override;
    ToolStatus status() const override;

protected:
    const QVector<QPointF> &points() const;
    int requiredPointCount() const;
    virtual bool buildShape(const ToolContext &context, Shape *shape) const = 0;
    void clearPoints(ToolContext &context);
    void publish(ToolContext &context);

private:
    ToolId tool_;
    int requiredPoints_;
    QVector<QPointF> points_;
    ToolStatus status_;
};

} // namespace classiCAD
