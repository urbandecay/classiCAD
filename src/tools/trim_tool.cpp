#include "trim_tool.h"

#include "tool_context.h"

namespace classiCAD {

ToolId TrimTool::id() const
{
    return ToolId::Trim;
}

void TrimTool::begin(ToolContext &context)
{
    status_.state = ToolLifecycleState::Active;
    status_.text = QStringLiteral("Trim");
    status_.canCommit = false;
    context.publishStatus(status_);
}

ToolStatus TrimTool::status() const
{
    return status_;
}

} // namespace classiCAD
