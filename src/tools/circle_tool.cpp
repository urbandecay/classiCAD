#include "circle_tool.h"

#include "tool_context.h"

namespace classiCAD {

CircleTool::CircleTool()
    : ShapeCreationTool(ToolId::Circle, 2)
{
}

bool CircleTool::buildShape(const ToolContext &context, Shape *shape) const
{
    return context.createShape(id(), points(), ArcMode::TwoPoint, 0.0, shape);
}

} // namespace classiCAD
