#include "rotate_tool.h"

#include "tool_context.h"

namespace classiCAD {

ToolId RotateTool::id() const
{
    return ToolId::Rotate;
}

void RotateTool::begin(ToolContext &context)
{
    status_.state = ToolLifecycleState::Active;
    status_.text = QStringLiteral("Rotate");
    status_.canCommit = false;
    context.publishStatus(status_);
}

ToolStatus RotateTool::status() const
{
    return status_;
}

} // namespace classiCAD
