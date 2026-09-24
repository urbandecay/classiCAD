#include "tool.h"

namespace classiCAD {

void InteractionTool::begin(ToolContext &)
{
}

bool InteractionTool::handleMousePress(const ToolInput &, ToolContext &)
{
    return false;
}

bool InteractionTool::handleMouseMove(const ToolInput &, ToolContext &)
{
    return false;
}

bool InteractionTool::handleMouseRelease(const ToolInput &, ToolContext &)
{
    return false;
}

bool InteractionTool::handleWheel(const ToolInput &, ToolContext &)
{
    return false;
}

bool InteractionTool::handleKey(const ToolInput &, ToolContext &)
{
    return false;
}

void InteractionTool::cancel(ToolContext &)
{
}

void InteractionTool::commit(ToolContext &)
{
}

ToolPreview InteractionTool::preview() const
{
    return {};
}

ToolStatus InteractionTool::status() const
{
    return {};
}

} // namespace classiCAD
