#include "select_tool.h"

#include "tool_context.h"

namespace classiCAD {

ToolId SelectTool::id() const
{
    return ToolId::Select;
}

void SelectTool::begin(ToolContext &context)
{
    status_.state = ToolLifecycleState::Active;
    status_.text = QStringLiteral("Select");
    status_.canCommit = false;
    context.publishStatus(status_);
}

ToolStatus SelectTool::status() const
{
    return status_;
}

} // namespace classiCAD
