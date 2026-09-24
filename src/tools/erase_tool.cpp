#include "erase_tool.h"

#include "tool_context.h"

namespace classiCAD {

ToolId EraseTool::id() const
{
    return ToolId::Erase;
}

void EraseTool::begin(ToolContext &context)
{
    status_.state = ToolLifecycleState::Active;
    status_.text = QStringLiteral("Erase");
    status_.canCommit = false;
    context.publishStatus(status_);
}

ToolStatus EraseTool::status() const
{
    return status_;
}

} // namespace classiCAD
