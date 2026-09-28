#include "viewport_transform.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace classiCAD {
namespace {

struct Vec3 {
    qreal x = 0.0;
    qreal y = 0.0;
    qreal z = 0.0;
};

Vec3 add(const Vec3 &first, const Vec3 &second)
{
    return {first.x + second.x, first.y + second.y, first.z + second.z};
}

Vec3 subtract(const Vec3 &first, const Vec3 &second)
{
    return {first.x - second.x, first.y - second.y, first.z - second.z};
}

Vec3 multiply(const Vec3 &vector, qreal scalar)
{
    return {vector.x * scalar, vector.y * scalar, vector.z * scalar};
}

qreal dot(const Vec3 &first, const Vec3 &second)
{
    return first.x * second.x + first.y * second.y + first.z * second.z;
}

Vec3 normalized(const Vec3 &vector)
{
    const qreal length = std::sqrt(dot(vector, vector));
    if (length <= 1.0e-15) {
        return {};
    }
    return multiply(vector, 1.0 / length);
}

Vec3 asVec(const Point3D &point)
{
    return {point.x, point.y, point.z};
}

struct CameraBasis {
    Vec3 right;
    Vec3 up;
    Vec3 forward;
};

CameraBasis cameraBasis(qreal yaw, qreal pitch)
{
    const qreal elevationCos = std::cos(pitch);
    const Vec3 cameraOut{elevationCos * std::sin(yaw),
                         -elevationCos * std::cos(yaw),
                         std::sin(pitch)};
    const Vec3 forward = multiply(cameraOut, -1.0);
    const Vec3 right{std::cos(yaw), std::sin(yaw), 0.0};
    const Vec3 up = normalized({right.y * forward.z - right.z * forward.y,
                                right.z * forward.x - right.x * forward.z,
                                right.x * forward.y - right.y * forward.x});
    return {right, up, forward};
}

Vec3 cameraTarget(const CameraBasis &basis, const QPointF &pan)
{
    return add(multiply(basis.right, -pan.x()),
               multiply(basis.up, -pan.y()));
}

} // namespace

qreal ViewportTransform::zoom() const
{
    return zoom_;
}

qreal &ViewportTransform::zoom()
{
    return zoom_;
}

QPointF ViewportTransform::pan() const
{
    return pan_;
}

QPointF &ViewportTransform::pan()
{
    return pan_;
}

QPointF ViewportTransform::screenToWorld(const QPointF &screenPosition,
                                         const QSize &viewportSize) const
{
    QPointF workPlanePosition;
    if (!screenToWorkPlane(screenPosition,
                           viewportSize,
                           workPlane_,
                           workPlaneOffset_,
                           &workPlanePosition)) {
        return {};
    }
    return workPlanePosition;
}

QPointF ViewportTransform::worldToScreen(const QPointF &worldPosition,
                                         const QSize &viewportSize) const
{
    return workPlaneToScreen(worldPosition,
                             viewportSize,
                             workPlane_,
                             workPlaneOffset_);
}

bool ViewportTransform::screenToWorkPlane(const QPointF &screenPosition,
                                          const QSize &viewportSize,
                                          WorkPlane plane,
                                          qreal planeOffset,
                                          QPointF *workPlanePosition) const
{
    if (workPlanePosition == nullptr || viewportSize.width() <= 0 ||
        viewportSize.height() <= 0 || zoom_ <= 1.0e-15) {
        return false;
    }

    const CameraBasis basis = cameraBasis(yawRadians_, pitchRadians_);
    const Vec3 target = cameraTarget(basis, pan_);

    const qreal pixelX = screenPosition.x() - viewportSize.width() / 2.0;
    const qreal pixelY = viewportSize.height() / 2.0 - screenPosition.y();
    Vec3 rayOrigin;
    Vec3 rayDirection;
    if (perspective_) {
        rayOrigin = subtract(target, multiply(basis.forward, cameraDistance_));
        const qreal focalLength = zoom_ * cameraDistance_;
        rayDirection = normalized(add(basis.forward,
                                      add(multiply(basis.right, pixelX / focalLength),
                                          multiply(basis.up, pixelY / focalLength))));
    } else {
        rayOrigin = add(target,
                        add(multiply(basis.right, pixelX / zoom_),
                            multiply(basis.up, pixelY / zoom_)));
        rayDirection = basis.forward;
    }

    const Vec3 planePoint = asVec(workPlanePointToWorld({}, plane, planeOffset));
    const Vec3 planeNormal = asVec(workPlaneNormal(plane));
    const qreal denominator = dot(planeNormal, rayDirection);
    if (std::abs(denominator) <= 1.0e-12) {
        return false;
    }
    qreal distance = dot(planeNormal, subtract(planePoint, rayOrigin)) /
                     denominator;
    constexpr qreal rayOriginTolerance = 1.0e-9;
    if (!std::isfinite(distance) || distance < -rayOriginTolerance) {
        return false;
    }
    distance = std::max<qreal>(0.0, distance);

    const Vec3 world = add(rayOrigin, multiply(rayDirection, distance));
    *workPlanePosition = worldPointToWorkPlane({world.x, world.y, world.z}, plane);
    return std::isfinite(workPlanePosition->x()) &&
           std::isfinite(workPlanePosition->y());
}

QPointF ViewportTransform::workPlaneToScreen(const QPointF &workPlanePosition,
                                             const QSize &viewportSize,
                                             WorkPlane plane,
                                             qreal planeOffset) const
{
    QPointF screenPosition;
    if (!worldPointToScreen(workPlanePointToWorld(workPlanePosition,
                                                   plane,
                                                   planeOffset),
                            viewportSize,
                            &screenPosition)) {
        const qreal invalid = std::numeric_limits<qreal>::quiet_NaN();
        return {invalid, invalid};
    }
    return screenPosition;
}

bool ViewportTransform::worldPointToScreen(const Point3D &worldPosition,
                                           const QSize &viewportSize,
                                           QPointF *screenPosition) const
{
    if (screenPosition == nullptr || viewportSize.width() <= 0 ||
        viewportSize.height() <= 0) {
        return false;
    }

    const CameraBasis basis = cameraBasis(yawRadians_, pitchRadians_);
    const Vec3 target = cameraTarget(basis, pan_);
    const Vec3 relative = subtract(asVec(worldPosition), target);
    const qreal viewX = dot(relative, basis.right);
    const qreal viewY = dot(relative, basis.up);
    qreal scale = zoom_;
    if (perspective_) {
        const qreal depth = cameraDistance_ + dot(relative, basis.forward);
        if (depth <= cameraDistance_ * 0.01) {
            return false;
        }
        scale *= cameraDistance_ / depth;
    }
    *screenPosition = QPointF(viewportSize.width() / 2.0 + viewX * scale,
                              viewportSize.height() / 2.0 - viewY * scale);
    return std::isfinite(screenPosition->x()) &&
           std::isfinite(screenPosition->y());
}

WorkPlane ViewportTransform::workPlane() const
{
    return workPlane_;
}

qreal ViewportTransform::workPlaneOffset() const
{
    return workPlaneOffset_;
}

void ViewportTransform::setWorkPlane(WorkPlane plane, qreal offset)
{
    if (!std::isfinite(offset)) {
        return;
    }
    workPlane_ = plane;
    workPlaneOffset_ = offset;
}

ViewportViewPreset ViewportTransform::viewPreset() const
{
    return viewPreset_;
}

void ViewportTransform::setViewPreset(ViewportViewPreset preset)
{
    constexpr qreal halfPi = 1.57079632679489661923;
    constexpr qreal radians = 0.01745329251994329577;
    viewPreset_ = preset;
    if (preset == ViewportViewPreset::Custom) {
        return;
    }
    perspective_ = preset == ViewportViewPreset::Perspective;
    switch (preset) {
    case ViewportViewPreset::Top:
        yawRadians_ = 0.0;
        pitchRadians_ = halfPi;
        break;
    case ViewportViewPreset::Front:
        yawRadians_ = 0.0;
        pitchRadians_ = 0.0;
        break;
    case ViewportViewPreset::Right:
        yawRadians_ = halfPi;
        pitchRadians_ = 0.0;
        break;
    case ViewportViewPreset::Isometric:
        yawRadians_ = 45.0 * radians;
        pitchRadians_ = 35.2643896828 * radians;
        break;
    case ViewportViewPreset::Perspective:
        yawRadians_ = 45.0 * radians;
        pitchRadians_ = 35.2643896828 * radians;
        break;
    case ViewportViewPreset::Custom:
        break;
    }
}

void ViewportTransform::orbitByPixels(const QPointF &delta)
{
    constexpr qreal radiansPerPixel = 0.008;
    constexpr qreal pitchLimit = 1.5690509975;
    yawRadians_ += delta.x() * radiansPerPixel;
    pitchRadians_ = std::clamp(pitchRadians_ + delta.y() * radiansPerPixel,
                               -pitchLimit,
                               pitchLimit);
    viewPreset_ = ViewportViewPreset::Custom;
}

void ViewportTransform::panByPixels(const QPointF &delta,
                                    const QSize &viewportSize)
{
    Q_UNUSED(viewportSize)
    pan_ += QPointF(delta.x() / zoom_, -delta.y() / zoom_);
}

void ViewportTransform::resetView()
{
    zoom_ = 1.0;
    pan_ = {};
    workPlane_ = WorkPlane::XY;
    workPlaneOffset_ = 0.0;
    setViewPreset(ViewportViewPreset::Top);
}

void ViewportTransform::zoomAt(const QPointF &screenPosition,
                               qreal factor,
                               const QSize &viewportSize,
                               qreal minimumZoom,
                               qreal maximumZoom)
{
    QPointF beforeZoom;
    if (!screenToWorkPlane(screenPosition,
                           viewportSize,
                           workPlane_,
                           workPlaneOffset_,
                           &beforeZoom)) {
        zoom_ = std::clamp(zoom_ * factor, minimumZoom, maximumZoom);
        return;
    }
    const Point3D anchorWorld = workPlanePointToWorld(beforeZoom,
                                                       workPlane_,
                                                       workPlaneOffset_);
    zoom_ = std::clamp(zoom_ * factor, minimumZoom, maximumZoom);
    QPointF afterScreen;
    if (worldPointToScreen(anchorWorld, viewportSize, &afterScreen)) {
        panByPixels(screenPosition - afterScreen, viewportSize);
    }
}

} // namespace classiCAD
