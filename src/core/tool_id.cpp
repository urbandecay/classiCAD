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

bool isCircleConstructionTool(ToolId tool)
{
    return tool == ToolId::Circle || tool == ToolId::CircleDiameter ||
           tool == ToolId::CircleThreePoint;
}

bool isCircleTangentTool(ToolId tool)
{
    return tool == ToolId::CircleTangentTwo || tool == ToolId::CircleTangentThree;
}

bool isCircleTool(ToolId tool)
{
    return isCircleConstructionTool(tool) || isCircleTangentTool(tool);
}

bool isPointCreationTool(ToolId tool)
{
    return tool == ToolId::Point || tool == ToolId::PointByLine ||
           tool == ToolId::PointByArcs || tool == ToolId::PointCenter ||
           tool == ToolId::PointEdgeCenter;
}

bool isCurveCreationTool(ToolId tool)
{
    return tool == ToolId::Bezier || tool == ToolId::Nurbs ||
           tool == ToolId::CurveInterpolate || tool == ToolId::CurveFreehand;
}

bool isTwoCurveLineTool(ToolId tool)
{
    return tool == ToolId::TangentToTwoCurves ||
           tool == ToolId::PerpendicularToTwoCurves;
}

bool isDimensionTool(ToolId tool)
{
    return tool == ToolId::LinearDimension || tool == ToolId::AngularDimension;
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
        return QStringLiteral("Circle (Center, Radius)");
    case ToolId::Point:
        return QStringLiteral("Point");
    case ToolId::Erase:
        return QStringLiteral("Erase");
    case ToolId::Trim:
        return QStringLiteral("Trim");
    case ToolId::Rotate:
        return QStringLiteral("Rotate");
    case ToolId::Scale:
        return QStringLiteral("Scale");
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
    case ToolId::CircleDiameter:
        return QStringLiteral("Circle (2 Point Diameter)");
    case ToolId::CircleThreePoint:
        return QStringLiteral("Circle (3 Points)");
    case ToolId::CircleTangentTwo:
        return QStringLiteral("Circle Tangent to 2 Curves");
    case ToolId::CircleTangentThree:
        return QStringLiteral("Circle Tangent to 3 Curves");
    case ToolId::LinearDimension:
        return QStringLiteral("Linear Dimension");
    case ToolId::AngularDimension:
        return QStringLiteral("Angular Dimension");
    case ToolId::Picture:
        return QStringLiteral("Picture");
    case ToolId::CurveInterpolate:
        return QStringLiteral("Interpolate Curve");
    case ToolId::CurveFreehand:
        return QStringLiteral("Freehand Curve");
    case ToolId::PointByLine:
        return QStringLiteral("Point by Line");
    case ToolId::PointByArcs:
        return QStringLiteral("Point by Arcs");
    case ToolId::PointCenter:
        return QStringLiteral("Point Center");
    case ToolId::PointEdgeCenter:
        return QStringLiteral("Edge Center");
    case ToolId::PerpendicularFromEdge:
        return QStringLiteral("Perpendicular from Edge");
    case ToolId::TangentToTwoCurves:
        return QStringLiteral("Tangent to Two Curves");
    case ToolId::PerpendicularToTwoCurves:
        return QStringLiteral("Perpendicular to Two Curves");
    case ToolId::PointExtrude:
        return QStringLiteral("Extrude");
    }

    return QStringLiteral("Unknown");
}

QString scaleModeName(ScaleMode mode)
{
    switch (mode) {
    case ScaleMode::OneD:
        return QStringLiteral("Scale 1D");
    case ScaleMode::TwoD:
        return QStringLiteral("Scale 2D");
    }
    return QStringLiteral("Scale");
}

int requiredPoints(ToolId tool)
{
    switch (tool) {
    case ToolId::Line:
        return 2;
    case ToolId::LinearDimension:
    case ToolId::AngularDimension:
        return 3;
    case ToolId::Arc:
        return 3;
    case ToolId::Bezier:
    case ToolId::Nurbs:
        return 4;
    case ToolId::Rectangle:
    case ToolId::Circle:
    case ToolId::CircleDiameter:
    case ToolId::EllipseFromCorners:
    case ToolId::RectangleFromCenter:
    case ToolId::PolygonCenterCorner:
    case ToolId::PolygonCenterTangent:
    case ToolId::PolygonCornerCorner:
    case ToolId::PolygonEdge:
        return 2;
    case ToolId::Point:
    case ToolId::PointByLine:
    case ToolId::PointByArcs:
    case ToolId::PointCenter:
    case ToolId::PointEdgeCenter:
        return 1;
    case ToolId::Picture:
        return 2;
    case ToolId::Ellipse:
    case ToolId::EllipseFromEndpoints:
    case ToolId::EllipseFromFoci:
        return 3;
    case ToolId::Select:
    case ToolId::Erase:
    case ToolId::Trim:
    case ToolId::Rotate:
    case ToolId::Scale:
    case ToolId::Mirror:
    case ToolId::TangentFromCurve:
    case ToolId::PerpendicularFromCurve:
    case ToolId::PerpendicularFromEdge:
    case ToolId::TangentToTwoCurves:
    case ToolId::PerpendicularToTwoCurves:
    case ToolId::PointExtrude:
    case ToolId::CurveInterpolate:
    case ToolId::CurveFreehand:
        return 0;
    case ToolId::RectangleThreePoint:
    case ToolId::CircleThreePoint:
        return 3;
    case ToolId::CircleTangentTwo:
    case ToolId::CircleTangentThree:
        return 0;
    }

    return 0;
}

} // namespace classiCAD
