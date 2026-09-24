#pragma once

#include "tool.h"

namespace classiCAD {

class TrimTool final : public InteractionTool {
public:
    ToolId id() const override;
    void begin(ToolContext &context) override;
    ToolStatus status() const override;

private:
    ToolStatus status_;
};

} // namespace classiCAD
