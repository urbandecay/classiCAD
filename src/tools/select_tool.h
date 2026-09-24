#pragma once

#include "tool.h"

namespace classiCAD {

// SelectTool establishes the common lifecycle for selection interaction. The
// viewport still owns its mature box-selection and drag behavior until the
// next migration step can move those state transitions behind ToolContext.
class SelectTool final : public InteractionTool {
public:
    ToolId id() const override;
    void begin(ToolContext &context) override;
    ToolStatus status() const override;

private:
    ToolStatus status_;
};

} // namespace classiCAD
