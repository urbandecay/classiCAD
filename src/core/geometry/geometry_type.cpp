#include "geometry_type.h"

#include "../tool_id.h"

namespace classiCAD {

bool isPersistentGeometryType(GeometryType type)
{
    return type >= GeometryType::Line && type <= GeometryType::Ellipse;
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
        value > static_cast<int>(GeometryType::Ellipse)) {
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
    case ToolId::Line:
        return GeometryType::Line;
    case ToolId::Arc:
        return GeometryType::Arc;
    case ToolId::Bezier:
        return GeometryType::Bezier;
    case ToolId::Nurbs:
        return GeometryType::Nurbs;
    case ToolId::Rectangle:
    case ToolId::RectangleFromCenter:
    case ToolId::RectangleThreePoint:
        return GeometryType::Rectangle;
    case ToolId::Circle:
        return GeometryType::Circle;
    case ToolId::Ellipse:
    case ToolId::EllipseFromEndpoints:
    case ToolId::EllipseFromCorners:
    case ToolId::EllipseFromFoci:
        return GeometryType::Ellipse;
    case ToolId::Point:
        return GeometryType::Point;
    case ToolId::Select:
    case ToolId::Erase:
    case ToolId::Trim:
    case ToolId::Rotate:
    case ToolId::Mirror:
    case ToolId::TangentFromCurve:
    case ToolId::PerpendicularFromCurve:
        return GeometryType::Invalid;
    }

    return GeometryType::Invalid;
}

} // namespace classiCAD
