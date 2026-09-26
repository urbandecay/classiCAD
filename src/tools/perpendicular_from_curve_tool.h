#pragma once

#include "core/document/object_id.h"
#include "tool.h"

namespace classiCAD {

class PerpendicularFromCurveTool final : public InteractionTool {
public:
    ToolId id() const override;
    void begin(ToolContext &context) override;
    bool handleMousePress(const ToolInput &input, ToolContext &context) override;
    bool handleMouseMove(const ToolInput &input, ToolContext &context) override;
    bool handleKey(const ToolInput &input, ToolContext &context) override;
    void cancel(ToolContext &context) override;
    ToolPreview preview() const override;
    ToolStatus status() const override;

private:
    bool updatePerpendicularPreview(const ToolInput &input, ToolContext &context);
    void publish(ToolContext &context);

    ObjectId curveObjectId_ = ObjectId::invalid();
    QPointF perpendicularPoint_;
    bool perpendicularPreviewAvailable_ = false;
    ToolStatus status_;
};

} // namespace classiCAD
