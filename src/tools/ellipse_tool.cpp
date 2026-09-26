#include "ellipse_tool.h"

#include "tool_context.h"

namespace classiCAD {

EllipseTool::EllipseTool(ToolId tool)
    : ShapeCreationTool(tool, requiredPoints(tool))
{
}

bool EllipseTool::buildShape(const ToolContext &context, Shape *shape) const
{
    return context.createShape(id(), points(), ArcMode::TwoPoint, 0.0, shape);
}

} // namespace classiCAD
