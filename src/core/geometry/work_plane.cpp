#include "work_plane.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace classiCAD {

namespace {

qreal dot(const Point3D &first, const Point3D &second)
{
    return first.x * second.x + first.y * second.y + first.z * second.z;
}

Point3D cross(const Point3D &first, const Point3D &second)
{
    return {first.y * second.z - first.z * second.y,
            first.z * second.x - first.x * second.z,
            first.x * second.y - first.y * second.x};
}

qreal length(const Point3D &vector)
{
    return std::sqrt(dot(vector, vector));
}

Point3D normalized(const Point3D &vector)
{
    const qreal magnitude = length(vector);
    return magnitude > 1.0e-12
               ? Point3D{vector.x / magnitude,
                         vector.y / magnitude,
                         vector.z / magnitude}
               : Point3D{};
}

Point3D subtract(const Point3D &first, const Point3D &second)
{
    return {first.x - second.x, first.y - second.y, first.z - second.z};
}

Point3D add(const Point3D &first, const Point3D &second)
{
    return {first.x + second.x, first.y + second.y, first.z + second.z};
}

Point3D multiply(const Point3D &vector, qreal scale)
{
    return {vector.x * scale, vector.y * scale, vector.z * scale};
}

bool finite(const Point3D &point)
{
    return std::isfinite(point.x) && std::isfinite(point.y) &&
           std::isfinite(point.z);
}

bool near(qreal first, qreal second, qreal tolerance = 1.0e-8)
{
    const qreal scale = std::max<qreal>({1.0, std::abs(first), std::abs(second)});
    return std::abs(first - second) <= tolerance * scale;
}

} // namespace

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

WorkPlaneFrame makeWorkPlaneFrame(WorkPlane plane, qreal offset)
{
    WorkPlaneFrame frame;
    if (!std::isfinite(offset)) {
        return frame;
    }
    switch (plane) {
    case WorkPlane::XY:
        frame.origin = {0.0, 0.0, offset};
        frame.xAxis = {1.0, 0.0, 0.0};
        frame.yAxis = {0.0, 1.0, 0.0};
        frame.normal = {0.0, 0.0, 1.0};
        break;
    case WorkPlane::XZ:
        frame.origin = {0.0, offset, 0.0};
        frame.xAxis = {1.0, 0.0, 0.0};
        frame.yAxis = {0.0, 0.0, 1.0};
        frame.normal = {0.0, -1.0, 0.0};
        break;
    case WorkPlane::YZ:
        frame.origin = {offset, 0.0, 0.0};
        frame.xAxis = {0.0, 1.0, 0.0};
        frame.yAxis = {0.0, 0.0, 1.0};
        frame.normal = {1.0, 0.0, 0.0};
        break;
    }
    frame.valid = true;
    return frame;
}

WorkPlaneFrame makeWorkPlaneFrameFromNormal(const Point3D &origin,
                                            const Point3D &normal,
                                            const Point3D &preferredXAxis)
{
    WorkPlaneFrame frame;
    if (!finite(origin) || !finite(normal) || !finite(preferredXAxis) ||
        length(normal) <= 1.0e-12) {
        return frame;
    }

    frame.origin = origin;
    frame.normal = normalized(normal);
    Point3D projected = subtract(preferredXAxis,
                                 multiply(frame.normal,
                                          dot(preferredXAxis, frame.normal)));
    if (length(projected) <= 1.0e-8) {
        const Point3D fallback = std::abs(frame.normal.x) < 0.8
                                     ? Point3D{1.0, 0.0, 0.0}
                                     : Point3D{0.0, 1.0, 0.0};
        projected = subtract(fallback,
                             multiply(frame.normal, dot(fallback, frame.normal)));
    }
    frame.xAxis = normalized(projected);
    frame.yAxis = normalized(cross(frame.normal, frame.xAxis));
    frame.valid = true;
    if (!isValidWorkPlaneFrame(frame)) {
        return {};
    }
    return frame;
}

bool isValidWorkPlaneFrame(const WorkPlaneFrame &frame)
{
    if (!frame.valid || !finite(frame.origin) || !finite(frame.xAxis) ||
        !finite(frame.yAxis) || !finite(frame.normal)) {
        return false;
    }
    constexpr qreal axisTolerance = 1.0e-6;
    return near(length(frame.xAxis), 1.0, axisTolerance) &&
           near(length(frame.yAxis), 1.0, axisTolerance) &&
           near(length(frame.normal), 1.0, axisTolerance) &&
           std::abs(dot(frame.xAxis, frame.yAxis)) <= axisTolerance &&
           std::abs(dot(frame.xAxis, frame.normal)) <= axisTolerance &&
           std::abs(dot(frame.yAxis, frame.normal)) <= axisTolerance &&
           dot(cross(frame.xAxis, frame.yAxis), frame.normal) >=
               1.0 - axisTolerance;
}

Point3D workPlaneFramePointToWorld(const QPointF &point,
                                   const WorkPlaneFrame &frame)
{
    return workPlaneFramePointToWorld(point, 0.0, frame);
}

Point3D workPlaneFramePointToWorld(const QPointF &point,
                                   qreal normalCoordinate,
                                   const WorkPlaneFrame &frame)
{
    if (!isValidWorkPlaneFrame(frame) || !std::isfinite(normalCoordinate)) {
        return {};
    }
    return add(frame.origin,
               add(multiply(frame.xAxis, point.x()),
                   add(multiply(frame.yAxis, point.y()),
                       multiply(frame.normal, normalCoordinate))));
}

QPointF worldPointToWorkPlaneFrame(const Point3D &point,
                                   const WorkPlaneFrame &frame)
{
    return worldPointToWorkPlaneFrame(point, frame, nullptr);
}

QPointF worldPointToWorkPlaneFrame(const Point3D &point,
                                   const WorkPlaneFrame &frame,
                                   qreal *normalCoordinate)
{
    if (!isValidWorkPlaneFrame(frame) || !finite(point)) {
        return {};
    }
    const Point3D relative = subtract(point, frame.origin);
    if (normalCoordinate != nullptr) {
        *normalCoordinate = dot(relative, frame.normal);
    }
    return {dot(relative, frame.xAxis), dot(relative, frame.yAxis)};
}

qreal signedDistanceFromWorkPlaneFrame(const Point3D &point,
                                       const WorkPlaneFrame &frame)
{
    if (!isValidWorkPlaneFrame(frame) || !finite(point)) {
        return std::numeric_limits<qreal>::quiet_NaN();
    }
    return dot(subtract(point, frame.origin), frame.normal);
}

bool workPlaneFramesMatch(const WorkPlaneFrame &first,
                          const WorkPlaneFrame &second)
{
    if (!isValidWorkPlaneFrame(first) || !isValidWorkPlaneFrame(second)) {
        return false;
    }
    return near(first.origin.x, second.origin.x) &&
           near(first.origin.y, second.origin.y) &&
           near(first.origin.z, second.origin.z) &&
           near(first.xAxis.x, second.xAxis.x) &&
           near(first.xAxis.y, second.xAxis.y) &&
           near(first.xAxis.z, second.xAxis.z) &&
           near(first.yAxis.x, second.yAxis.x) &&
           near(first.yAxis.y, second.yAxis.y) &&
           near(first.yAxis.z, second.yAxis.z) &&
           near(first.normal.x, second.normal.x) &&
           near(first.normal.y, second.normal.y) &&
           near(first.normal.z, second.normal.z);
}

bool workPlaneFramesCoplanar(const WorkPlaneFrame &first,
                             const WorkPlaneFrame &second,
                             qreal distanceTolerance)
{
    if (!isValidWorkPlaneFrame(first) || !isValidWorkPlaneFrame(second) ||
        !std::isfinite(distanceTolerance) || distanceTolerance < 0.0) {
        return false;
    }

    const qreal normalAlignment = std::abs(dot(first.normal, second.normal));
    if (1.0 - normalAlignment > 1.0e-8) {
        return false;
    }

    return std::abs(dot(subtract(second.origin, first.origin), first.normal)) <=
           distanceTolerance;
}

} // namespace classiCAD
