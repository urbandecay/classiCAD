#include "bezier_tool.h"

#include "tool_context.h"

namespace classiCAD {

BezierTool::BezierTool()
    : ShapeCreationTool(ToolId::Bezier, 4)
{
}

bool BezierTool::buildShape(const ToolContext &context, Shape *shape) const
{
    return context.createShape(id(), points(), ArcMode::TwoPoint, 0.0, shape);
}

} // namespace classiCAD
