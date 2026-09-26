#include "tool_id.h"

namespace classiCAD {

bool isEraseLikeTool(ToolId tool)
{
    return tool == ToolId::Erase || tool == ToolId::Trim;
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
        return 2;
    case ToolId::Point:
        return 1;
    case ToolId::Select:
    case ToolId::Erase:
    case ToolId::Trim:
    case ToolId::Rotate:
    case ToolId::Mirror:
    case ToolId::TangentFromCurve:
    case ToolId::PerpendicularFromCurve:
        return 0;
    }

    return 0;
}

} // namespace classiCAD
