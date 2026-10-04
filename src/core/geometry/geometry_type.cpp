#include "geometry_type.h"

#include "../tool_id.h"

namespace classiCAD {

bool isPersistentGeometryType(GeometryType type)
{
    return type >= GeometryType::Line && type <= GeometryType::NurbsSurface;
}

bool isDimensionGeometryType(GeometryType type)
{
    return type == GeometryType::LinearDimension ||
           type == GeometryType::AngularDimension;
}

QString geometryTypeName(GeometryType type)
{
    switch (type) {
    case GeometryType::Line:
        return QStringLiteral("Line");
    case GeometryType::Arc:
        return QStringLiteral("Arc");
    case GeometryType::Bezier:
        return QStringLiteral("Bezier");
    case GeometryType::Nurbs:
        return QStringLiteral("NURBS");
    case GeometryType::Rectangle:
        return QStringLiteral("Rectangle");
    case GeometryType::Circle:
        return QStringLiteral("Circle");
    case GeometryType::Point:
        return QStringLiteral("Point");
    case GeometryType::PolyCurve:
        return QStringLiteral("PolyCurve");
    case GeometryType::Ellipse:
        return QStringLiteral("Ellipse");
    case GeometryType::Polygon:
        return QStringLiteral("Polygon");
    case GeometryType::LinearDimension:
        return QStringLiteral("Linear Dimension");
    case GeometryType::AngularDimension:
        return QStringLiteral("Angular Dimension");
    case GeometryType::Picture:
        return QStringLiteral("Picture");
    case GeometryType::NurbsSurface:
        return QStringLiteral("NURBS Surface");
    case GeometryType::Invalid:
        return QStringLiteral("Invalid");
    }

    return QStringLiteral("Invalid");
}

bool geometryTypeFromLegacyValue(int value, GeometryType *type)
{
    if (type == nullptr || value < static_cast<int>(GeometryType::Line) ||
        value > static_cast<int>(GeometryType::PolyCurve)) {
        return false;
    }

    *type = static_cast<GeometryType>(value);
    return true;
}

bool geometryTypeFromValue(int value, GeometryType *type)
{
    if (type == nullptr || value < static_cast<int>(GeometryType::Line) ||
        value > static_cast<int>(GeometryType::NurbsSurface)) {
        return false;
    }

    *type = static_cast<GeometryType>(value);
    return true;
}

int legacyValueForGeometryType(GeometryType type)
{
    // The version-1 "tool" field only encoded persistent geometry values 1..8.
    // New geometry kinds must not be mistaken for active tool commands by old readers.
    return type >= GeometryType::Line && type <= GeometryType::PolyCurve
               ? static_cast<int>(type)
               : 0;
}

GeometryType geometryTypeForTool(ToolId tool)
{
    switch (tool) {
    case ToolId::LinearDimension:
        return GeometryType::LinearDimension;
    case ToolId::AngularDimension:
        return GeometryType::AngularDimension;
    case ToolId::Line:
    case ToolId::PerpendicularFromEdge:
    case ToolId::TangentToTwoCurves:
    case ToolId::PerpendicularToTwoCurves:
        return GeometryType::Line;
    case ToolId::Arc:
        return GeometryType::Arc;
    case ToolId::Bezier:
        return GeometryType::Bezier;
    case ToolId::Nurbs:
    case ToolId::CurveInterpolate:
    case ToolId::CurveFreehand:
        return GeometryType::Nurbs;
    case ToolId::Rectangle:
    case ToolId::RectangleFromCenter:
    case ToolId::RectangleThreePoint:
        return GeometryType::Rectangle;
    case ToolId::PolygonCenterCorner:
    case ToolId::PolygonCenterTangent:
    case ToolId::PolygonCornerCorner:
    case ToolId::PolygonEdge:
        return GeometryType::Polygon;
    case ToolId::Circle:
    case ToolId::CircleDiameter:
    case ToolId::CircleThreePoint:
    case ToolId::CircleTangentTwo:
    case ToolId::CircleTangentThree:
        return GeometryType::Circle;
    case ToolId::Ellipse:
    case ToolId::EllipseFromEndpoints:
    case ToolId::EllipseFromCorners:
    case ToolId::EllipseFromFoci:
        return GeometryType::Ellipse;
    case ToolId::Point:
    case ToolId::PointByLine:
    case ToolId::PointByArcs:
    case ToolId::PointCenter:
    case ToolId::PointEdgeCenter:
        return GeometryType::Point;
    case ToolId::Picture:
        return GeometryType::Picture;
    case ToolId::Select:
    case ToolId::Erase:
    case ToolId::Trim:
    case ToolId::Rotate:
    case ToolId::Scale:
    case ToolId::Mirror:
    case ToolId::TangentFromCurve:
    case ToolId::PerpendicularFromCurve:
    case ToolId::PointExtrude:
        return GeometryType::Invalid;
    }

    return GeometryType::Invalid;
}

} // namespace classiCAD
