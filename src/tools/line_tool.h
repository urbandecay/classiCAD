#pragma once

#include "tool.h"

namespace classiCAD {

class LineTool final : public InteractionTool {
public:
    ToolId id() const override;
    void begin(ToolContext &context) override;
    bool handleMousePress(const ToolInput &input, ToolContext &context) override;
    bool handleKey(const ToolInput &input, ToolContext &context) override;
    void cancel(ToolContext &context) override;
    void commit(ToolContext &context) override;
    ToolPreview preview() const override;
    ToolStatus status() const override;

private:
    void publish(ToolContext &context);
    QVector<QPointF> points_;
    ToolStatus status_;
};

} // namespace classiCAD
