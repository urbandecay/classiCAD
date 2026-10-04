#pragma once

#include "object_id.h"

#include <QtGlobal>

namespace classiCAD {

enum class DimensionAnchorKind {
    None,
    CurveParameter,
    ControlPoint,
    ShapePoint,
    Center,
};

// Persistent dimensions refer to scene objects by stable identity. Curve
// positions use normalized NURBS parameters to survive knot-domain changes.
struct DimensionAnchorReference {
    ObjectId objectId = ObjectId::invalid();
    DimensionAnchorKind kind = DimensionAnchorKind::None;
    int componentIndex = -1;
    int pointIndex = -1;
    qreal parameterFraction = 0.0;
};

} // namespace classiCAD
