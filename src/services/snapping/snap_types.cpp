#include "snap_types.h"

namespace classiCAD {

QString snapTypeName(SnapType type)
{
    switch (type) {
    case SnapType::Endpoint:
        return QStringLiteral("Endpoint");
    case SnapType::Midpoint:
        return QStringLiteral("Midpoint");
    case SnapType::Intersection:
        return QStringLiteral("Intersection");
    case SnapType::Center:
        return QStringLiteral("Center");
    case SnapType::Perpendicular:
        return QStringLiteral("Perpendicular");
    case SnapType::Tangent:
        return QStringLiteral("Tangent");
    case SnapType::ControlPoint:
        return QStringLiteral("Control Point");
    case SnapType::Near:
        return QStringLiteral("Near");
    case SnapType::None:
        return QStringLiteral("None");
    }

    return QStringLiteral("None");
}

} // namespace classiCAD
