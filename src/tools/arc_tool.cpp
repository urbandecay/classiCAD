#include "arc_tool.h"

#include "tool_context.h"

namespace classiCAD {

ArcTool::ArcTool()
    : ShapeCreationTool(ToolId::Arc, 3)
{
}

bool ArcTool::buildShape(const ToolContext &context, Shape *shape) const
{
    return context.createShape(id(),
                               points(),
                               context.arcMode(),
                               context.arcSweep(),
                               shape);
}

} // namespace classiCAD
