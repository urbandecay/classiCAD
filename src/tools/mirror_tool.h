#pragma once

#include "tool.h"

namespace classiCAD {

// Mirror's mature event-state behavior remains coordinated by the viewport,
// while this module owns its named tool lifecycle/status contract.
class MirrorTool final : public InteractionTool {
public:
    ToolId id() const override;
    void begin(ToolContext &context) override;
    ToolStatus status() const override;

private:
    ToolStatus status_;
};

} // namespace classiCAD
