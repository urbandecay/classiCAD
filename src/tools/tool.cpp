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

InteractionTool::EventResult InteractionTool::dispatchMousePress(
    const ToolInput &input, ToolContext &context)
{
    return handleMousePress(input, context) ? EventResult::Handled
                                           : EventResult::Unhandled;
}

InteractionTool::EventResult InteractionTool::dispatchMouseMove(
    const ToolInput &input, ToolContext &context)
{
    return handleMouseMove(input, context) ? EventResult::Handled
                                           : EventResult::Unhandled;
}

InteractionTool::EventResult InteractionTool::dispatchWheel(
    const ToolInput &input, ToolContext &context)
{
    return handleWheel(input, context) ? EventResult::Handled
                                       : EventResult::Unhandled;
}

InteractionTool::EventResult InteractionTool::dispatchKey(
    const ToolInput &input, ToolContext &context)
{
    return handleKey(input, context) ? EventResult::Handled
                                     : EventResult::Unhandled;
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
