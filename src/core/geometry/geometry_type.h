#pragma once

#include <QString>

namespace classiCAD {

enum class ToolId : int;

// GeometryType is the persistent kind of a scene object. Its numeric values
// 1..8 intentionally match the legacy Shape::tool values used by version-1
// update sessions.
enum class GeometryType : int {
    Invalid = 0,
    Line = 1,
    Arc = 2,
    Bezier = 3,
    Nurbs = 4,
    Rectangle = 5,
    Circle = 6,
    Point = 7,
    PolyCurve = 8,
};

bool isPersistentGeometryType(GeometryType type);
QString geometryTypeName(GeometryType type);

// The legacy field was named "tool" and stored these integer values. Keep
// the conversion in the geometry vocabulary so active commands cannot leak
// into persisted scene data.
bool geometryTypeFromLegacyValue(int value, GeometryType *type);
int legacyValueForGeometryType(GeometryType type);

GeometryType geometryTypeForTool(ToolId tool);

} // namespace classiCAD
