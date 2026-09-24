#include "rectangle_tool.h"

#include "tool_context.h"

namespace classiCAD {

RectangleTool::RectangleTool()
    : ShapeCreationTool(ToolId::Rectangle, 2)
{
}

bool RectangleTool::buildShape(const ToolContext &context, Shape *shape) const
{
    return context.createShape(id(), points(), ArcMode::TwoPoint, 0.0, shape);
}

} // namespace classiCAD
