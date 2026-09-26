#include "tool_id.h"

namespace classiCAD {

bool isEraseLikeTool(ToolId tool)
{
    return tool == ToolId::Erase || tool == ToolId::Trim;
}

bool isEllipseTool(ToolId tool)
{
    return tool == ToolId::Ellipse || tool == ToolId::EllipseFromEndpoints ||
           tool == ToolId::EllipseFromCorners || tool == ToolId::EllipseFromFoci;
}

EllipseMode ellipseModeForTool(ToolId tool)
{
    switch (tool) {
    case ToolId::EllipseFromEndpoints:
        return EllipseMode::AxisEndpoints;
    case ToolId::EllipseFromCorners:
        return EllipseMode::Corners;
    case ToolId::EllipseFromFoci:
        return EllipseMode::FociPoint;
    case ToolId::Ellipse:
    default:
        return EllipseMode::CenterAxisRadius;
    }
}

bool isRectangleTool(ToolId tool)
{
    return tool == ToolId::Rectangle || tool == ToolId::RectangleFromCenter ||
           tool == ToolId::RectangleThreePoint;
}

RectangleMode rectangleModeForTool(ToolId tool)
{
    switch (tool) {
    case ToolId::RectangleFromCenter:
        return RectangleMode::CenterCorner;
    case ToolId::RectangleThreePoint:
        return RectangleMode::ThreePoint;
    case ToolId::Rectangle:
    default:
        return RectangleMode::CornerCorner;
    }
}

bool isPolygonTool(ToolId tool)
{
    return tool == ToolId::PolygonCenterCorner ||
           tool == ToolId::PolygonCenterTangent ||
           tool == ToolId::PolygonCornerCorner ||
           tool == ToolId::PolygonEdge;
}

PolygonMode polygonModeForTool(ToolId tool)
{
    switch (tool) {
    case ToolId::PolygonCenterTangent:
        return PolygonMode::CenterTangent;
    case ToolId::PolygonCornerCorner:
        return PolygonMode::CornerCorner;
    case ToolId::PolygonEdge:
        return PolygonMode::Edge;
    case ToolId::PolygonCenterCorner:
    default:
        return PolygonMode::CenterCorner;
    }
}

QString toolName(ToolId tool)
{
    switch (tool) {
    case ToolId::Select:
        return QStringLiteral("Select");
    case ToolId::Line:
        return QStringLiteral("Line");
    case ToolId::Arc:
        return QStringLiteral("Arc");
    case ToolId::Bezier:
        return QStringLiteral("Bezier");
    case ToolId::Nurbs:
        return QStringLiteral("NURBS");
    case ToolId::Rectangle:
        return QStringLiteral("Rectangle");
    case ToolId::Circle:
        return QStringLiteral("Circle");
    case ToolId::Point:
        return QStringLiteral("Point");
    case ToolId::Erase:
        return QStringLiteral("Erase");
    case ToolId::Trim:
        return QStringLiteral("Trim");
    case ToolId::Rotate:
        return QStringLiteral("Rotate");
    case ToolId::Mirror:
        return QStringLiteral("Mirror");
    case ToolId::TangentFromCurve:
        return QStringLiteral("Tangent from Curve");
    case ToolId::PerpendicularFromCurve:
        return QStringLiteral("Perpendicular from Curve");
    case ToolId::Ellipse:
        return QStringLiteral("Ellipse (Center, Axis, Radius)");
    case ToolId::EllipseFromEndpoints:
        return QStringLiteral("Ellipse (Axis Endpoints)");
    case ToolId::EllipseFromCorners:
        return QStringLiteral("Ellipse (Bounding Corners)");
    case ToolId::EllipseFromFoci:
        return QStringLiteral("Ellipse (Foci and Point)");
    case ToolId::RectangleFromCenter:
        return QStringLiteral("Rectangle (Center, Corner)");
    case ToolId::RectangleThreePoint:
        return QStringLiteral("Rectangle (3 Points)");
    case ToolId::PolygonCenterCorner:
        return QStringLiteral("Polygon (Center, Corner)");
    case ToolId::PolygonCenterTangent:
        return QStringLiteral("Polygon (Center, Tangent)");
    case ToolId::PolygonCornerCorner:
        return QStringLiteral("Polygon (Corner, Corner)");
    case ToolId::PolygonEdge:
        return QStringLiteral("Polygon (Side Size)");
    }

    return QStringLiteral("Unknown");
}

int requiredPoints(ToolId tool)
{
    switch (tool) {
    case ToolId::Line:
        return 2;
    case ToolId::Arc:
        return 3;
    case ToolId::Bezier:
    case ToolId::Nurbs:
        return 4;
    case ToolId::Rectangle:
    case ToolId::Circle:
    case ToolId::EllipseFromCorners:
    case ToolId::RectangleFromCenter:
    case ToolId::PolygonCenterCorner:
    case ToolId::PolygonCenterTangent:
    case ToolId::PolygonCornerCorner:
    case ToolId::PolygonEdge:
        return 2;
    case ToolId::Point:
        return 1;
    case ToolId::Ellipse:
    case ToolId::EllipseFromEndpoints:
    case ToolId::EllipseFromFoci:
        return 3;
    case ToolId::Select:
    case ToolId::Erase:
    case ToolId::Trim:
    case ToolId::Rotate:
    case ToolId::Mirror:
    case ToolId::TangentFromCurve:
    case ToolId::PerpendicularFromCurve:
        return 0;
    case ToolId::RectangleThreePoint:
        return 3;
    }

    return 0;
}

} // namespace classiCAD
