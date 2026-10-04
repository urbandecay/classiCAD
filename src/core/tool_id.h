#pragma once

#include "core/geometry/construction_modes.h"

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
    PolygonCenterCorner = 21,
    PolygonCenterTangent = 22,
    PolygonCornerCorner = 23,
    PolygonEdge = 24,
    CircleDiameter = 25,
    CircleThreePoint = 26,
    CircleTangentTwo = 27,
    CircleTangentThree = 28,
    LinearDimension = 29,
    AngularDimension = 30,
    Scale = 31,
    Picture = 32,
    CurveInterpolate = 33,
    CurveFreehand = 34,
    PointByLine = 35,
    PointByArcs = 36,
    PointCenter = 37,
    PointEdgeCenter = 38,
    PerpendicularFromEdge = 39,
    TangentToTwoCurves = 40,
    PerpendicularToTwoCurves = 41,
    PointExtrude = 42,
};

enum class ScaleMode {
    OneD,
    TwoD,
};

// Transitional source alias for the existing UI implementation. New code
// should name this type ToolId; Shape stores GeometryType instead.
using Tool = ToolId;

bool isEraseLikeTool(ToolId tool);
bool isEllipseTool(ToolId tool);
EllipseMode ellipseModeForTool(ToolId tool);
bool isRectangleTool(ToolId tool);
RectangleMode rectangleModeForTool(ToolId tool);
bool isPolygonTool(ToolId tool);
PolygonMode polygonModeForTool(ToolId tool);
bool isCircleConstructionTool(ToolId tool);
bool isCircleTangentTool(ToolId tool);
bool isCircleTool(ToolId tool);
bool isPointCreationTool(ToolId tool);
bool isCurveCreationTool(ToolId tool);
bool isTwoCurveLineTool(ToolId tool);
bool isDimensionTool(ToolId tool);
QString toolName(ToolId tool);
QString scaleModeName(ScaleMode mode);
int requiredPoints(ToolId tool);

} // namespace classiCAD
