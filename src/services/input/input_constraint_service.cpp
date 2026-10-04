#include "input_constraint_service.h"

#include "services/viewport/viewport_transform.h"

#include <algorithm>
#include <cmath>

namespace classiCAD {
namespace {

Point3D subtract(const Point3D &first, const Point3D &second)
{
    return {first.x - second.x,
            first.y - second.y,
            first.z - second.z};
}

qreal dot(const Point3D &first, const Point3D &second)
{
    return first.x * second.x + first.y * second.y + first.z * second.z;
}

qreal length(const Point3D &value)
{
    return std::sqrt(dot(value, value));
}

Point3D normalized(const Point3D &value)
{
    const qreal magnitude = length(value);
    return magnitude > 1.0e-12
               ? Point3D{value.x / magnitude,
                         value.y / magnitude,
                         value.z / magnitude}
               : Point3D{};
}

} // namespace

Point3D InputConstraintService::worldAxisDirection(int key)
{
    switch (key) {
    case Qt::Key_X:
        return {1.0, 0.0, 0.0};
    case Qt::Key_Y:
        return {0.0, 1.0, 0.0};
    case Qt::Key_Z:
        return {0.0, 0.0, 1.0};
    default:
        return {};
    }
}

Point3D InputConstraintService::inferProjectedWorldAxis(
    const Point3D &reference,
    const QPointF &screenPosition,
    const ViewportTransform &transform,
    const QSize &viewportSize,
    qreal toleranceDegrees)
{
    QPointF referenceScreen;
    if (!transform.worldPointToScreen(reference,
                                     viewportSize,
                                     &referenceScreen)) {
        return {};
    }

    const QPointF cursorDelta = screenPosition - referenceScreen;
    const qreal cursorLength = std::hypot(cursorDelta.x(), cursorDelta.y());
    if (cursorLength <= 1.0) {
        return {};
    }

    constexpr qreal pi = 3.14159265358979323846;
    qreal bestAlignment = std::cos(toleranceDegrees * pi / 180.0);
    Point3D bestAxis;
    for (const int key : {Qt::Key_X, Qt::Key_Y, Qt::Key_Z}) {
        const Point3D axis = worldAxisDirection(key);
        QPointF axisScreen;
        if (!transform.worldPointToScreen(
                {reference.x + axis.x,
                 reference.y + axis.y,
                 reference.z + axis.z},
                viewportSize,
                &axisScreen)) {
            continue;
        }
        const QPointF axisDelta = axisScreen - referenceScreen;
        const qreal axisLength = std::hypot(axisDelta.x(), axisDelta.y());
        if (axisLength <= 1.0e-8) {
            continue;
        }
        const qreal alignment = std::abs(
            QPointF::dotProduct(cursorDelta, axisDelta) /
            (cursorLength * axisLength));
        if (alignment >= bestAlignment) {
            bestAlignment = alignment;
            bestAxis = axis;
        }
    }
    return bestAxis;
}

bool InputConstraintService::worldPointOnScreenAxis(
    const QPointF &screenPosition,
    const QSize &viewportSize,
    const Point3D &origin,
    const Point3D &direction,
    const ViewportTransform &transform,
    Point3D *worldPoint)
{
    return transform.screenToWorldAxis(screenPosition,
                                       viewportSize,
                                       origin,
                                       direction,
                                       worldPoint);
}

Point3D InputConstraintService::projectOntoWorldAxis(
    const Point3D &point,
    const Point3D &origin,
    const Point3D &direction)
{
    const Point3D unitDirection = normalized(direction);
    if (length(unitDirection) <= 1.0e-12) {
        return point;
    }
    const qreal distance = dot(subtract(point, origin), unitDirection);
    return {origin.x + unitDirection.x * distance,
            origin.y + unitDirection.y * distance,
            origin.z + unitDirection.z * distance};
}

QPointF InputConstraintService::nearestPlanarAxis(const QPointF &point,
                                                  const QPointF &origin)
{
    const qreal deltaX = point.x() - origin.x();
    const qreal deltaY = point.y() - origin.y();
    return std::abs(deltaX) >= std::abs(deltaY)
               ? QPointF(point.x(), origin.y())
               : QPointF(origin.x(), point.y());
}

} // namespace classiCAD
