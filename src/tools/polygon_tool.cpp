#include "polygon_tool.h"

#include "tool_context.h"

namespace classiCAD {

PolygonTool::PolygonTool(ToolId tool)
    : ShapeCreationTool(tool, requiredPoints(tool))
{
}

bool PolygonTool::buildShape(const ToolContext &context, Shape *shape) const
{
    return context.createShape(id(), points(), ArcMode::TwoPoint, 0.0, shape);
}

} // namespace classiCAD
