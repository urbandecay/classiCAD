#pragma once

namespace classiCAD {

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

enum class PolygonMode {
    CenterCorner,
    CenterTangent,
    CornerCorner,
    Edge,
};

} // namespace classiCAD
