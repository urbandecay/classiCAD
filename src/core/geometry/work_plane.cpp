#include "work_plane.h"

namespace classiCAD {

bool workPlaneFromValue(int value, WorkPlane *plane)
{
    if (plane == nullptr || value < static_cast<int>(WorkPlane::XY) ||
        value > static_cast<int>(WorkPlane::YZ)) {
        return false;
    }
    *plane = static_cast<WorkPlane>(value);
    return true;
}

QString workPlaneName(WorkPlane plane)
{
    switch (plane) {
    case WorkPlane::XY:
        return QStringLiteral("XY");
    case WorkPlane::XZ:
        return QStringLiteral("XZ");
    case WorkPlane::YZ:
        return QStringLiteral("YZ");
    }
    return QStringLiteral("XY");
}

Point3D workPlanePointToWorld(const QPointF &point, WorkPlane plane, qreal offset)
{
    switch (plane) {
    case WorkPlane::XY:
        return {point.x(), point.y(), offset};
    case WorkPlane::XZ:
        return {point.x(), offset, point.y()};
    case WorkPlane::YZ:
        return {offset, point.x(), point.y()};
    }
    return {point.x(), point.y(), offset};
}

QPointF worldPointToWorkPlane(const Point3D &point, WorkPlane plane)
{
    switch (plane) {
    case WorkPlane::XY:
        return {point.x, point.y};
    case WorkPlane::XZ:
        return {point.x, point.z};
    case WorkPlane::YZ:
        return {point.y, point.z};
    }
    return {point.x, point.y};
}

qreal signedDistanceFromWorkPlane(const Point3D &point,
                                  WorkPlane plane,
                                  qreal offset)
{
    switch (plane) {
    case WorkPlane::XY:
        return point.z - offset;
    case WorkPlane::XZ:
        return point.y - offset;
    case WorkPlane::YZ:
        return point.x - offset;
    }
    return point.z - offset;
}

Point3D workPlaneNormal(WorkPlane plane)
{
    switch (plane) {
    case WorkPlane::XY:
        return {0.0, 0.0, 1.0};
    case WorkPlane::XZ:
        return {0.0, -1.0, 0.0};
    case WorkPlane::YZ:
        return {1.0, 0.0, 0.0};
    }
    return {0.0, 0.0, 1.0};
}

} // namespace classiCAD
