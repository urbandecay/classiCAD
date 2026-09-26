#pragma once

#include <QString>

namespace classiCAD {

// ToolId identifies the active interaction. It is never used as the
// persistent type of a scene object.
enum class ToolId : int {
    Select = 0,
    Line = 1,
    Arc = 2,
    Bezier = 3,
    Nurbs = 4,
    Rectangle = 5,
    Circle = 6,
    Point = 7,
    // Value 8 was the legacy PolyCurve geometry value. Keep it unused in
    // this enum so old serialized values cannot be mistaken for a command.
    Erase = 9,
    Trim = 10,
    Rotate = 11,
    Mirror = 12,
    TangentFromCurve = 13,
    PerpendicularFromCurve = 14,
    Ellipse = 15,
    EllipseFromEndpoints = 16,
    EllipseFromCorners = 17,
    EllipseFromFoci = 18,
    RectangleFromCenter = 19,
    RectangleThreePoint = 20,
};

enum class EllipseMode {
    CenterAxisRadius,
    AxisEndpoints,
    Corners,
    FociPoint,
};

enum class RectangleMode {
    CornerCorner,
    CenterCorner,
    ThreePoint,
};

// Transitional source alias for the existing UI implementation. New code
// should name this type ToolId; Shape stores GeometryType instead.
using Tool = ToolId;

bool isEraseLikeTool(ToolId tool);
bool isEllipseTool(ToolId tool);
EllipseMode ellipseModeForTool(ToolId tool);
bool isRectangleTool(ToolId tool);
RectangleMode rectangleModeForTool(ToolId tool);
QString toolName(ToolId tool);
int requiredPoints(ToolId tool);

} // namespace classiCAD
