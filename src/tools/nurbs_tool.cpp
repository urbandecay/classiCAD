#include "nurbs_tool.h"

#include "tool_context.h"

namespace classiCAD {

NurbsTool::NurbsTool()
    : ShapeCreationTool(ToolId::Nurbs, 4)
{
}

bool NurbsTool::buildShape(const ToolContext &context, Shape *shape) const
{
    return context.createShape(id(), points(), ArcMode::TwoPoint, 0.0, shape);
}

} // namespace classiCAD
