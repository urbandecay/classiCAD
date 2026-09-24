#include "mirror_tool.h"

#include "tool_context.h"

namespace classiCAD {

ToolId MirrorTool::id() const
{
    return ToolId::Mirror;
}

void MirrorTool::begin(ToolContext &context)
{
    status_.state = ToolLifecycleState::Active;
    status_.text = QStringLiteral("Mirror: draw an axis");
    status_.canCommit = false;
    context.publishStatus(status_);
}

ToolStatus MirrorTool::status() const
{
    return status_;
}

} // namespace classiCAD
