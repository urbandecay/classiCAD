#include "point_tool.h"

#include "tool_context.h"

namespace classiCAD {

PointTool::PointTool()
    : ShapeCreationTool(ToolId::Point, 1)
{
}

bool PointTool::buildShape(const ToolContext &context, Shape *shape) const
{
    return context.createShape(id(), points(), ArcMode::TwoPoint, 0.0, shape);
}

} // namespace classiCAD
