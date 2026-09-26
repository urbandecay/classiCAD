#include "tool_registry.h"

#include "bezier_tool.h"
#include "arc_tool.h"
#include "circle_tool.h"
#include "erase_tool.h"
#include "ellipse_tool.h"
#include "line_tool.h"
#include "mirror_tool.h"
#include "nurbs_tool.h"
#include "perpendicular_from_curve_tool.h"
#include "point_tool.h"
#include "rectangle_tool.h"
#include "rotate_tool.h"
#include "select_tool.h"
#include "tangent_from_curve_tool.h"
#include "trim_tool.h"

namespace classiCAD {

ToolRegistry::ToolRegistry()
{
    add(std::make_unique<SelectTool>());
    add(std::make_unique<PointTool>());
    add(std::make_unique<LineTool>());
    add(std::make_unique<TangentFromCurveTool>());
    add(std::make_unique<PerpendicularFromCurveTool>());
    add(std::make_unique<ArcTool>());
    add(std::make_unique<RectangleTool>());
    add(std::make_unique<CircleTool>());
    add(std::make_unique<EllipseTool>(ToolId::Ellipse));
    add(std::make_unique<EllipseTool>(ToolId::EllipseFromEndpoints));
    add(std::make_unique<EllipseTool>(ToolId::EllipseFromCorners));
    add(std::make_unique<EllipseTool>(ToolId::EllipseFromFoci));
    add(std::make_unique<BezierTool>());
    add(std::make_unique<NurbsTool>());
    add(std::make_unique<RotateTool>());
    add(std::make_unique<MirrorTool>());
    add(std::make_unique<TrimTool>());
    add(std::make_unique<EraseTool>());
}

void ToolRegistry::add(std::unique_ptr<InteractionTool> tool)
{
    if (tool) {
        tools_.push_back(std::move(tool));
    }
}

InteractionTool *ToolRegistry::find(ToolId id) const
{
    for (const std::unique_ptr<InteractionTool> &tool : tools_) {
        if (tool && tool->id() == id) {
            return tool.get();
        }
    }
    return nullptr;
}

} // namespace classiCAD
