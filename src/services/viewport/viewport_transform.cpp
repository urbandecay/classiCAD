#include "viewport_transform.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace classiCAD {

qreal viewportWheelStepsFromDeltas(int angleDeltaY, int pixelDeltaY)
{
    if (angleDeltaY != 0) {
        return static_cast<qreal>(angleDeltaY) / 120.0;
    }
    return static_cast<qreal>(pixelDeltaY) / 40.0;
}

ViewportOrientation ViewportOrientation::fromAxisAngle(qreal axisX,
                                                       qreal axisY,
                                                       qreal axisZ,
                                                       qreal radians)
{
    const qreal half = radians * 0.5;
    const qreal sine = std::sin(half);
    return {std::cos(half), axisX * sine, axisY * sine, axisZ * sine};
}

ViewportOrientation ViewportOrientation::normalized() const
{
    const qreal length = std::sqrt(dot(*this));
    if (length <= 1.0e-15) {
        return {};
    }
    return {w / length, x / length, y / length, z / length};
}

ViewportOrientation ViewportOrientation::operator*(
    const ViewportOrientation &other) const
{
    return {w * other.w - x * other.x - y * other.y - z * other.z,
            w * other.x + x * other.w + y * other.z - z * other.y,
            w * other.y - x * other.z + y * other.w + z * other.x,
            w * other.z + x * other.y - y * other.x + z * other.w};
}

qreal ViewportOrientation::dot(const ViewportOrientation &other) const
{
    return w * other.w + x * other.x + y * other.y + z * other.z;
}

ViewportOrientation ViewportOrientation::slerp(
    const ViewportOrientation &start,
    const ViewportOrientation &end,
    qreal fraction)
{
    ViewportOrientation destination = end;
    qreal cosine = start.dot(destination);
    if (cosine < 0.0) {
        destination = {-end.w, -end.x, -end.y, -end.z};
        cosine = -cosine;
    }
    if (cosine > 0.9995) {
        return ViewportOrientation{
            start.w + (destination.w - start.w) * fraction,
            start.x + (destination.x - start.x) * fraction,
            start.y + (destination.y - start.y) * fraction,
            start.z + (destination.z - start.z) * fraction}.normalized();
    }
    const qreal angle = std::acos(std::clamp(cosine, -1.0, 1.0));
    const qreal denominator = std::sin(angle);
    const qreal first = std::sin((1.0 - fraction) * angle) / denominator;
    const qreal second = std::sin(fraction * angle) / denominator;
    return {start.w * first + destination.w * second,
            start.x * first + destination.x * second,
            start.y * first + destination.y * second,
            start.z * first + destination.z * second};
}

namespace {

constexpr qreal kViewportSensorWidthMillimeters = 36.0;
constexpr qreal kViewportReferenceDistance = 60.0;
// Keep orthographic navigation independent of camera clip settings. This
// preserves the default 10,000-unit zoom-out range that a 1,000-unit clip end
// previously happened to provide.
constexpr qreal kMaximumOrthographicViewDistance = 10000.0;
// BKE_camera_params_from_view3d uses CAMERA_PARAM_ZOOM_INIT_PERSP = 2.
constexpr qreal kBlenderViewportProjectionZoom = 2.0;
// Blender 5.2's V3D_OP_TRACKBALLSIZE from view3d_navigate.hh.
constexpr qreal kBlenderTrackballSize = 1.1;

bool isAxisAlignedOrthographicView(ViewportViewPreset preset)
{
    switch (preset) {
    case ViewportViewPreset::Top:
    case ViewportViewPreset::Bottom:
    case ViewportViewPreset::Front:
    case ViewportViewPreset::Back:
    case ViewportViewPreset::Right:
    case ViewportViewPreset::Left:
        return true;
    case ViewportViewPreset::Isometric:
    case ViewportViewPreset::Perspective:
    case ViewportViewPreset::Custom:
        return false;
    }
    return false;
}

qreal viewportFocalLengthPixels(const QSize &viewportSize, qreal lensMillimeters)
{
    const qreal sensorFitExtent = std::max(viewportSize.width(),
                                           viewportSize.height());
    return sensorFitExtent * lensMillimeters /
           (kViewportSensorWidthMillimeters * kBlenderViewportProjectionZoom);
}

qreal perspectiveReferenceDistance(const ViewportCameraPreferences &preferences)
{
    Q_UNUSED(preferences)
    // Initial scene framing is independent of the clipping range, like
    // RegionView3D::dist. The saved view state carries subsequent distance.
    return kViewportReferenceDistance;
}

qreal minimumViewDistance(qreal gridSpacing)
{
    if (!std::isfinite(gridSpacing) || gridSpacing <= 0.0) {
        gridSpacing = 1.0;
    }
    // Blender's ED_view3d_dist_soft_min_get uses grid * 0.001 for ordinary
    // viewport navigation.
    return std::max<qreal>(gridSpacing * 0.001, 1.0e-12);
}

qreal maximumViewDistance(const ViewportCameraPreferences &preferences,
                          qreal gridSpacing)
{
    // Blender's ED_view3d_dist_soft_range_get allows distance up to 10 * clip_end.
    return std::max(preferences.clipEnd * 10.0,
                    minimumViewDistance(gridSpacing));
}

qreal maximumOrthographicViewDistance(qreal gridSpacing)
{
    return std::max(kMaximumOrthographicViewDistance,
                    minimumViewDistance(gridSpacing));
}

qreal minimumViewZoom(const ViewportCameraPreferences &preferences,
                      qreal gridSpacing)
{
    return perspectiveReferenceDistance(preferences) /
           maximumViewDistance(preferences, gridSpacing);
}

qreal minimumOrthographicZoom(qreal gridSpacing)
{
    return kViewportReferenceDistance /
           maximumOrthographicViewDistance(gridSpacing);
}

qreal orthographicGridDistanceForZoom(qreal zoom, qreal gridSpacing)
{
    const qreal viewDistance = kViewportReferenceDistance /
                               std::max<qreal>(zoom, 1.0e-15);
    return std::clamp(viewDistance,
                      minimumViewDistance(gridSpacing),
                      maximumOrthographicViewDistance(gridSpacing));
}

qreal minimumZoomForView(bool perspective,
                         const ViewportCameraPreferences &preferences,
                         qreal gridSpacing)
{
    return perspective ? minimumViewZoom(preferences, gridSpacing)
                       : minimumOrthographicZoom(gridSpacing);
}

qreal maximumViewZoom(const ViewportCameraPreferences &preferences,
                      qreal gridSpacing)
{
    return perspectiveReferenceDistance(preferences) /
           minimumViewDistance(gridSpacing);
}

qreal perspectiveCameraDistance(qreal zoom,
                                const ViewportCameraPreferences &preferences)
{
    return perspectiveReferenceDistance(preferences) /
           std::max<qreal>(zoom, 1.0e-15);
}

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

ViewportOrientation orientationFromAngles(qreal yaw, qreal pitch)
{
    constexpr qreal halfPi = 1.57079632679489661923;
    return (ViewportOrientation::fromAxisAngle(0.0, 0.0, 1.0, yaw) *
            ViewportOrientation::fromAxisAngle(1.0, 0.0, 0.0,
                                               halfPi - pitch)).normalized();
}

Vec3 rotate(const ViewportOrientation &orientation, const Vec3 &vector)
{
    const Vec3 imaginary{orientation.x, orientation.y, orientation.z};
    const Vec3 cross{imaginary.y * vector.z - imaginary.z * vector.y,
                     imaginary.z * vector.x - imaginary.x * vector.z,
                     imaginary.x * vector.y - imaginary.y * vector.x};
    const Vec3 twice = multiply(cross, 2.0);
    const Vec3 second{imaginary.y * twice.z - imaginary.z * twice.y,
                      imaginary.z * twice.x - imaginary.x * twice.z,
                      imaginary.x * twice.y - imaginary.y * twice.x};
    return add(vector, add(multiply(twice, orientation.w), second));
}

CameraBasis cameraBasis(const ViewportOrientation &orientation)
{
    const Vec3 right = rotate(orientation, {1.0, 0.0, 0.0});
    const Vec3 up = rotate(orientation, {0.0, 1.0, 0.0});
    const Vec3 outward = rotate(orientation, {0.0, 0.0, 1.0});
    return {right, up, multiply(outward, -1.0)};
}

Vec3 cameraTarget(const CameraBasis &basis,
                  const QPointF &pan,
                  const Point3D &orbitPivot)
{
    return add(asVec(orbitPivot),
               add(multiply(basis.right, -pan.x()),
                   multiply(basis.up, -pan.y())));
}

Vec3 trackballVector(const QPointF &screenPosition, const QSize &viewportSize)
{
    if (viewportSize.width() <= 0 || viewportSize.height() <= 0) {
        return {0.0, 0.0, kBlenderTrackballSize};
    }

    // Match Blender's calctrackballvec: normalize both axes against half the
    // shorter viewport dimension so a nonsquare region does not distort the
    // virtual sphere. Qt's Y axis points down, unlike Blender's window space.
    const qreal halfMinimumDimension =
        std::min(viewportSize.width(), viewportSize.height()) * 0.5;
    const qreal x = (screenPosition.x() - viewportSize.width() * 0.5) /
                    halfMinimumDimension;
    const qreal y = (viewportSize.height() * 0.5 - screenPosition.y()) /
                    halfMinimumDimension;
    const qreal distance = std::sqrt(x * x + y * y);
    const qreal transition = kBlenderTrackballSize / std::sqrt(2.0);
    const qreal z = distance < transition
                        ? std::sqrt(std::max<qreal>(
                              0.0, kBlenderTrackballSize * kBlenderTrackballSize -
                                       distance * distance))
                        : transition * transition / distance;
    return {x, y, z};
}

Vec3 cross(const Vec3 &first, const Vec3 &second)
{
    return {first.y * second.z - first.z * second.y,
            first.z * second.x - first.x * second.z,
            first.x * second.y - first.y * second.x};
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

qreal ViewportTransform::viewScalePixelsPerWorldUnit(
    const QSize &viewportSize) const
{
    if (viewportSize.width() <= 0 || viewportSize.height() <= 0) {
        return zoom_;
    }
    return viewportFocalLengthPixels(
               viewportSize, cameraPreferences_.focalLengthMillimeters) *
           zoom_ / kViewportReferenceDistance;
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
    if (!screenToWorkPlane(screenPosition, viewportSize, workPlaneFrame_,
                           &workPlanePosition)) {
        return {};
    }
    return workPlanePosition;
}

QPointF ViewportTransform::worldToScreen(const QPointF &worldPosition,
                                         const QSize &viewportSize) const
{
    return workPlaneToScreen(worldPosition, viewportSize, workPlaneFrame_);
}

bool ViewportTransform::screenToWorkPlane(const QPointF &screenPosition,
                                          const QSize &viewportSize,
                                          WorkPlane plane,
                                          qreal planeOffset,
                                          QPointF *workPlanePosition) const
{
    return screenToWorkPlane(screenPosition,
                             viewportSize,
                             makeWorkPlaneFrame(plane, planeOffset),
                             workPlanePosition);
}

bool ViewportTransform::screenToWorkPlane(
    const QPointF &screenPosition,
    const QSize &viewportSize,
    const WorkPlaneFrame &frame,
    QPointF *workPlanePosition) const
{
    if (workPlanePosition == nullptr || viewportSize.width() <= 0 ||
        viewportSize.height() <= 0 || zoom_ <= 1.0e-15 ||
        !isValidWorkPlaneFrame(frame)) {
        return false;
    }

    const CameraBasis basis = cameraBasis(orientation_);
    const Vec3 target = cameraTarget(basis, pan_, orbitPivot_);
    const qreal focalLength = viewportFocalLengthPixels(
        viewportSize, cameraPreferences_.focalLengthMillimeters);

    const qreal pixelX = screenPosition.x() - viewportSize.width() / 2.0;
    const qreal pixelY = viewportSize.height() / 2.0 - screenPosition.y();
    const qreal viewScale = viewScalePixelsPerWorldUnit(viewportSize);
    Vec3 rayOrigin;
    Vec3 rayDirection;
    if (perspective_) {
        const qreal cameraDistance = perspectiveCameraDistance(
            zoom_, cameraPreferences_);
        rayOrigin = subtract(target, multiply(basis.forward, cameraDistance));
        rayDirection = normalized(add(basis.forward,
                                      add(multiply(basis.right, pixelX / focalLength),
                                          multiply(basis.up, pixelY / focalLength))));
    } else {
        rayOrigin = add(
            add(target, multiply(basis.forward, -cameraPreferences_.clipEnd)),
            add(multiply(basis.right, pixelX / viewScale),
                multiply(basis.up, pixelY / viewScale)));
        rayDirection = basis.forward;
    }

    const Vec3 planePoint = asVec(frame.origin);
    const Vec3 planeNormal = asVec(frame.normal);
    const qreal denominator = dot(planeNormal, rayDirection);
    if (std::abs(denominator) <= 1.0e-12) {
        return false;
    }
    qreal distance = dot(planeNormal, subtract(planePoint, rayOrigin)) /
                     denominator;
    constexpr qreal rayOriginTolerance = 1.0e-9;
    const qreal maximumRayDistance = perspective_
                                         ? cameraPreferences_.clipEnd
                                         : 2.0 * cameraPreferences_.clipEnd;
    const qreal viewDepth = perspective_
                                ? distance * dot(basis.forward, rayDirection)
                                : distance;
    if (!std::isfinite(viewDepth) ||
        viewDepth < cameraPreferences_.clipStart - rayOriginTolerance ||
        viewDepth > maximumRayDistance + rayOriginTolerance) {
        return false;
    }
    distance = std::max<qreal>(0.0, distance);

    const Vec3 world = add(rayOrigin, multiply(rayDirection, distance));
    *workPlanePosition = worldPointToWorkPlaneFrame(
        {world.x, world.y, world.z}, frame);
    return std::isfinite(workPlanePosition->x()) &&
           std::isfinite(workPlanePosition->y());
}

bool ViewportTransform::screenToWorkPlaneUnclipped(
    const QPointF &screenPosition,
    const QSize &viewportSize,
    WorkPlane plane,
    qreal planeOffset,
    QPointF *workPlanePosition) const
{
    return screenToWorkPlaneUnclipped(screenPosition,
                                      viewportSize,
                                      makeWorkPlaneFrame(plane, planeOffset),
                                      workPlanePosition);
}

bool ViewportTransform::screenToWorkPlaneUnclipped(
    const QPointF &screenPosition,
    const QSize &viewportSize,
    const WorkPlaneFrame &frame,
    QPointF *workPlanePosition) const
{
    if (workPlanePosition == nullptr || viewportSize.width() <= 0 ||
        viewportSize.height() <= 0 || zoom_ <= 1.0e-15 ||
        !isValidWorkPlaneFrame(frame)) {
        return false;
    }

    const CameraBasis basis = cameraBasis(orientation_);
    const Vec3 target = cameraTarget(basis, pan_, orbitPivot_);
    const qreal pixelX = screenPosition.x() - viewportSize.width() / 2.0;
    const qreal pixelY = viewportSize.height() / 2.0 - screenPosition.y();
    const qreal viewScale = std::max<qreal>(
        viewScalePixelsPerWorldUnit(viewportSize), 1.0e-15);
    Vec3 rayOrigin;
    Vec3 rayDirection;
    if (perspective_) {
        const qreal focalLength = viewportFocalLengthPixels(
            viewportSize, cameraPreferences_.focalLengthMillimeters);
        const qreal cameraDistance = perspectiveCameraDistance(
            zoom_, cameraPreferences_);
        rayOrigin = subtract(target, multiply(basis.forward, cameraDistance));
        rayDirection = normalized(add(
            basis.forward,
            add(multiply(basis.right, pixelX / focalLength),
                multiply(basis.up, pixelY / focalLength))));
    } else {
        rayOrigin = add(target,
                        add(multiply(basis.right, pixelX / viewScale),
                            multiply(basis.up, pixelY / viewScale)));
        rayDirection = basis.forward;
    }

    const Vec3 planePoint = asVec(frame.origin);
    const Vec3 planeNormal = asVec(frame.normal);
    const qreal denominator = dot(planeNormal, rayDirection);
    if (std::abs(denominator) <= 1.0e-12) {
        return false;
    }
    const qreal distance = dot(planeNormal, subtract(planePoint, rayOrigin)) /
                           denominator;
    const qreal viewDepth = perspective_
                                ? distance * dot(basis.forward, rayDirection)
                                : distance;
    if (!std::isfinite(viewDepth) || viewDepth < -1.0e-9) {
        return false;
    }

    const Vec3 world = add(rayOrigin, multiply(rayDirection, distance));
    *workPlanePosition = worldPointToWorkPlaneFrame(
        {world.x, world.y, world.z}, frame);
    return std::isfinite(workPlanePosition->x()) &&
           std::isfinite(workPlanePosition->y());
}

bool ViewportTransform::screenToWorldAxis(const QPointF &screenPosition,
                                         const QSize &viewportSize,
                                         const Point3D &origin,
                                         const Point3D &direction,
                                         Point3D *worldPosition) const
{
    if (worldPosition == nullptr || viewportSize.isEmpty() || zoom_ <= 1.0e-15) {
        return false;
    }
    const CameraBasis basis = cameraBasis(orientation_);
    const Vec3 target = cameraTarget(basis, pan_, orbitPivot_);
    const qreal pixelX = screenPosition.x() - viewportSize.width() / 2.0;
    const qreal pixelY = viewportSize.height() / 2.0 - screenPosition.y();
    Vec3 rayOrigin;
    Vec3 rayDirection;
    if (perspective_) {
        const qreal focal = viewportFocalLengthPixels(
            viewportSize, cameraPreferences_.focalLengthMillimeters);
        rayOrigin = subtract(target, multiply(basis.forward,
            perspectiveCameraDistance(zoom_, cameraPreferences_)));
        rayDirection = normalized(add(basis.forward,
            add(multiply(basis.right, pixelX / focal),
                multiply(basis.up, pixelY / focal))));
    } else {
        const qreal scale = viewScalePixelsPerWorldUnit(viewportSize);
        rayOrigin = add(target, add(multiply(basis.right, pixelX / scale),
                                    multiply(basis.up, pixelY / scale)));
        rayDirection = basis.forward;
    }
    const Vec3 axis = normalized(asVec(direction));
    const qreal alignment = dot(axis, rayDirection);
    const qreal denominator = dot(axis, axis) - alignment * alignment;
    if (denominator <= 1.0e-12) {
        return false;
    }
    // Closest point between the mouse ray and the constrained world line,
    // matching Blender's intersect_line_line rather than a floor projection.
    const Vec3 delta = subtract(rayOrigin, asVec(origin));
    const qreal parameter = (dot(axis, delta) - alignment * dot(rayDirection, delta)) /
                            denominator;
    const Vec3 point = add(asVec(origin), multiply(axis, parameter));
    *worldPosition = {point.x, point.y, point.z};
    return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
}

QPointF ViewportTransform::workPlaneToScreen(const QPointF &workPlanePosition,
                                             const QSize &viewportSize,
                                             WorkPlane plane,
                                             qreal planeOffset) const
{
    return workPlaneToScreen(workPlanePosition,
                             viewportSize,
                             makeWorkPlaneFrame(plane, planeOffset));
}

QPointF ViewportTransform::workPlaneToScreen(
    const QPointF &workPlanePosition,
    const QSize &viewportSize,
    const WorkPlaneFrame &frame) const
{
    QPointF screenPosition;
    if (!worldPointToScreen(workPlaneFramePointToWorld(workPlanePosition, frame),
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

    const CameraBasis basis = cameraBasis(orientation_);
    const Vec3 target = cameraTarget(basis, pan_, orbitPivot_);
    const Vec3 relative = subtract(asVec(worldPosition), target);
    const qreal viewX = dot(relative, basis.right);
    const qreal viewY = dot(relative, basis.up);
    qreal scale = viewScalePixelsPerWorldUnit(viewportSize);
    if (perspective_) {
        const qreal focalLength = viewportFocalLengthPixels(
            viewportSize, cameraPreferences_.focalLengthMillimeters);
        const qreal cameraDistance = perspectiveCameraDistance(
            zoom_, cameraPreferences_);
        const qreal depth = cameraDistance + dot(relative, basis.forward);
        if (depth < cameraPreferences_.clipStart ||
            depth > cameraPreferences_.clipEnd) {
            return false;
        }
        scale = focalLength / depth;
    } else {
        const qreal depth = cameraPreferences_.clipEnd +
                            dot(relative, basis.forward);
        if (depth < cameraPreferences_.clipStart ||
            depth > 2.0 * cameraPreferences_.clipEnd) {
            return false;
        }
    }
    *screenPosition = QPointF(viewportSize.width() / 2.0 + viewX * scale,
                              viewportSize.height() / 2.0 - viewY * scale);
    return std::isfinite(screenPosition->x()) &&
           std::isfinite(screenPosition->y());
}

bool ViewportTransform::worldPointToScreenUnclipped(
    const Point3D &worldPosition,
    const QSize &viewportSize,
    QPointF *screenPosition) const
{
    if (screenPosition == nullptr || viewportSize.width() <= 0 ||
        viewportSize.height() <= 0) {
        return false;
    }

    const CameraBasis basis = cameraBasis(orientation_);
    const Vec3 target = cameraTarget(basis, pan_, orbitPivot_);
    const Vec3 relative = subtract(asVec(worldPosition), target);
    const qreal viewX = dot(relative, basis.right);
    const qreal viewY = dot(relative, basis.up);
    qreal scale = viewScalePixelsPerWorldUnit(viewportSize);
    if (perspective_) {
        const qreal focalLength = viewportFocalLengthPixels(
            viewportSize, cameraPreferences_.focalLengthMillimeters);
        const qreal cameraDistance = perspectiveCameraDistance(zoom_,
                                                               cameraPreferences_);
        const qreal depth = cameraDistance + dot(relative, basis.forward);
        if (!std::isfinite(depth) || depth <= 1.0e-12) {
            return false;
        }
        scale = focalLength / depth;
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

const WorkPlaneFrame &ViewportTransform::workPlaneFrame() const
{
    return workPlaneFrame_;
}

void ViewportTransform::setWorkPlane(WorkPlane plane, qreal offset)
{
    if (!std::isfinite(offset)) {
        return;
    }
    workPlane_ = plane;
    workPlaneOffset_ = offset;
    workPlaneFrame_ = makeWorkPlaneFrame(plane, offset);
}

void ViewportTransform::setWorkPlaneFrame(const WorkPlaneFrame &frame)
{
    if (isValidWorkPlaneFrame(frame)) {
        workPlaneFrame_ = frame;
    }
}

ViewportViewPreset ViewportTransform::viewPreset() const
{
    return viewPreset_;
}

ViewportDirectionProjection ViewportTransform::worldDirectionToView(
    const Point3D &direction) const
{
    const CameraBasis basis = cameraBasis(orientation_);
    const Vec3 vector = asVec(direction);
    return {dot(vector, basis.right),
            dot(vector, basis.up),
            -dot(vector, basis.forward)};
}

Point3D ViewportTransform::viewDirection() const
{
    const CameraBasis basis = cameraBasis(orientation_);
    return {-basis.forward.x, -basis.forward.y, -basis.forward.z};
}

Point3D ViewportTransform::viewUp() const
{
    const CameraBasis basis = cameraBasis(orientation_);
    return {basis.up.x, basis.up.y, basis.up.z};
}

Point3D ViewportTransform::viewTarget() const
{
    const CameraBasis basis = cameraBasis(orientation_);
    const Vec3 target = cameraTarget(basis, pan_, orbitPivot_);
    return {target.x, target.y, target.z};
}

Point3D ViewportTransform::cameraPosition(const QSize &viewportSize) const
{
    Q_UNUSED(viewportSize)
    const CameraBasis basis = cameraBasis(orientation_);
    const Vec3 target = cameraTarget(basis, pan_, orbitPivot_);
    const qreal distance = perspective_ && zoom_ > 1.0e-15
                               ? perspectiveCameraDistance(zoom_,
                                                           cameraPreferences_)
                               : 0.0;
    const Vec3 position = subtract(target, multiply(basis.forward, distance));
    return {position.x, position.y, position.z};
}

ViewportCameraPreferences ViewportTransform::cameraPreferences() const
{
    return cameraPreferences_;
}

bool ViewportTransform::setCameraPreferences(
    const ViewportCameraPreferences &preferences)
{
    if (!std::isfinite(preferences.focalLengthMillimeters) ||
        preferences.focalLengthMillimeters < 1.0 ||
        preferences.focalLengthMillimeters > 2000.0 ||
        !std::isfinite(preferences.clipStart) ||
        preferences.clipStart < 0.000001 ||
        !std::isfinite(preferences.clipEnd) ||
        preferences.clipEnd <= preferences.clipStart ||
        preferences.clipEnd > 1.0e9) {
        return false;
    }
    cameraPreferences_ = preferences;
    zoom_ = std::clamp(zoom_,
                       minimumZoomForView(perspective_, cameraPreferences_,
                                          gridSpacing_),
                       maximumViewZoom(cameraPreferences_, gridSpacing_));
    gridViewDistance_ = std::clamp(
        gridViewDistance_,
        minimumViewDistance(gridSpacing_),
        maximumOrthographicViewDistance(gridSpacing_));
    return true;
}

void ViewportTransform::setGridSpacing(qreal gridSpacing)
{
    if (!std::isfinite(gridSpacing) || gridSpacing <= 0.0) {
        return;
    }
    gridSpacing_ = gridSpacing;
    zoom_ = std::clamp(zoom_,
                       minimumZoomForView(perspective_, cameraPreferences_,
                                          gridSpacing_),
                       maximumViewZoom(cameraPreferences_, gridSpacing_));
    gridViewDistance_ = std::clamp(
        gridViewDistance_,
        minimumViewDistance(gridSpacing_),
        maximumOrthographicViewDistance(gridSpacing_));
}

ViewportNavigationPreferences ViewportTransform::navigationPreferences() const
{
    return navigationPreferences_;
}

void ViewportTransform::setNavigationPreferences(
    const ViewportNavigationPreferences &preferences)
{
    if (!std::isfinite(preferences.turntableSensitivityRadiansPerPixel) ||
        preferences.turntableSensitivityRadiansPerPixel < 0.00017453292519943296 ||
        preferences.turntableSensitivityRadiansPerPixel > 0.08726646259971647 ||
        !std::isfinite(preferences.trackballSensitivity) ||
        preferences.trackballSensitivity < 0.01 ||
        preferences.trackballSensitivity > 10.0 ||
        (preferences.orbitMethod != ViewportOrbitMethod::Turntable &&
         preferences.orbitMethod != ViewportOrbitMethod::Trackball)) {
        return;
    }
    navigationPreferences_ = preferences;
}

ViewportCameraState ViewportTransform::cameraState() const
{
    const Point3D outward = viewDirection();
    const qreal yaw = std::atan2(outward.x, -outward.y);
    const qreal pitch = std::asin(std::clamp(outward.z, -1.0, 1.0));
    return {zoom_, pan_, orbitPivot_, yaw, pitch,
            perspective_, viewPreset_, gridViewDistance_, orientation_, true};
}

void ViewportTransform::setCameraState(const ViewportCameraState &state)
{
    if (!std::isfinite(state.zoom) || state.zoom <= 0.0 ||
        !std::isfinite(state.pan.x()) || !std::isfinite(state.pan.y()) ||
        !std::isfinite(state.orbitPivot.x) ||
        !std::isfinite(state.orbitPivot.y) ||
        !std::isfinite(state.orbitPivot.z) ||
        !std::isfinite(state.yawRadians) ||
        !std::isfinite(state.pitchRadians) ||
        !std::isfinite(state.gridViewDistance) || state.gridViewDistance <= 0.0) {
        return;
    }
    orbitGestureActive_ = false;
    perspective_ = state.perspective;
    const qreal minimumZoom = perspective_
                                  ? minimumViewZoom(cameraPreferences_, gridSpacing_)
                                  : std::numeric_limits<qreal>::min();
    zoom_ = std::clamp(state.zoom,
                       minimumZoom,
                       maximumViewZoom(cameraPreferences_, gridSpacing_));
    pan_ = state.pan;
    orbitPivot_ = state.orbitPivot;
    if (state.hasOrientation) {
        if (state.orientation.dot(state.orientation) <= 1.0e-12) {
            return;
        }
        orientation_ = state.orientation.normalized();
    } else {
        orientation_ = orientationFromAngles(state.yawRadians,
                                             state.pitchRadians);
    }
    viewPreset_ = state.preset;
    gridViewDistance_ = std::clamp(
        state.gridViewDistance,
        minimumViewDistance(gridSpacing_),
        maximumOrthographicViewDistance(gridSpacing_));
    orbitPivotLocked_ = false;
}

bool ViewportTransform::isPerspectiveEnabled() const
{
    return perspective_;
}

void ViewportTransform::setPerspectiveEnabled(bool enabled)
{
    perspective_ = enabled;
    if (perspective_) {
        zoom_ = std::clamp(zoom_,
                           minimumViewZoom(cameraPreferences_, gridSpacing_),
                           maximumViewZoom(cameraPreferences_, gridSpacing_));
    }
    if (!perspective_) {
        // Perspective zoom determines the camera distance used by grid LOD.
        // Carry the same distance into free-angle orthographic mode so its
        // grid does not retain a stale LOD from the previous ortho session.
        gridViewDistance_ = orthographicGridDistanceForZoom(zoom_, gridSpacing_);
    }
    if (enabled && viewPreset_ == ViewportViewPreset::Isometric) {
        viewPreset_ = ViewportViewPreset::Perspective;
    } else if (!enabled && viewPreset_ == ViewportViewPreset::Perspective) {
        viewPreset_ = ViewportViewPreset::Isometric;
    }
}

void ViewportTransform::setViewPreset(ViewportViewPreset preset)
{
    constexpr qreal halfPi = 1.57079632679489661923;
    constexpr qreal radians = 0.01745329251994329577;
    if (preset == ViewportViewPreset::Custom) {
        viewPreset_ = preset;
        return;
    }
    orbitGestureActive_ = false;
    orbitPivotLocked_ = false;
    const CameraBasis previousBasis = cameraBasis(orientation_);
    const Vec3 previousTarget = cameraTarget(previousBasis, pan_, orbitPivot_);
    orbitPivot_ = {previousTarget.x, previousTarget.y, previousTarget.z};
    pan_ = {};
    viewPreset_ = preset;
    perspective_ = preset == ViewportViewPreset::Perspective;
    switch (preset) {
    case ViewportViewPreset::Top:
        orientation_ = orientationFromAngles(0.0, halfPi);
        break;
    case ViewportViewPreset::Front:
        orientation_ = orientationFromAngles(0.0, 0.0);
        break;
    case ViewportViewPreset::Right:
        orientation_ = orientationFromAngles(halfPi, 0.0);
        break;
    case ViewportViewPreset::Bottom:
        orientation_ = orientationFromAngles(0.0, -halfPi);
        break;
    case ViewportViewPreset::Back:
        orientation_ = orientationFromAngles(180.0 * radians, 0.0);
        break;
    case ViewportViewPreset::Left:
        orientation_ = orientationFromAngles(-halfPi, 0.0);
        break;
    case ViewportViewPreset::Isometric:
        orientation_ = orientationFromAngles(45.0 * radians,
                                             35.2643896828 * radians);
        break;
    case ViewportViewPreset::Perspective:
        orientation_ = orientationFromAngles(45.0 * radians,
                                             35.2643896828 * radians);
        break;
    case ViewportViewPreset::Custom:
        break;
    }
    if (perspective_) {
        zoom_ = std::clamp(zoom_,
                           minimumViewZoom(cameraPreferences_, gridSpacing_),
                           maximumViewZoom(cameraPreferences_, gridSpacing_));
    }
    if (!perspective_) {
        gridViewDistance_ = orthographicGridDistanceForZoom(zoom_, gridSpacing_);
    }
}

void ViewportTransform::setViewDirection(const Point3D &cameraDirection)
{
    const Vec3 direction = normalized(asVec(cameraDirection));
    if (dot(direction, direction) <= 1.0e-15) {
        return;
    }

    orbitGestureActive_ = false;
    const CameraBasis previousBasis = cameraBasis(orientation_);
    const Vec3 previousTarget = cameraTarget(previousBasis, pan_, orbitPivot_);
    orbitPivot_ = {previousTarget.x, previousTarget.y, previousTarget.z};
    pan_ = {};
    orbitPivotLocked_ = false;
    orientation_ = orientationFromAngles(
        std::atan2(direction.x, -direction.y),
        std::asin(std::clamp(direction.z, -1.0, 1.0)));
    perspective_ = false;
    gridViewDistance_ = orthographicGridDistanceForZoom(zoom_, gridSpacing_);
    viewPreset_ = ViewportViewPreset::Custom;

    constexpr qreal axisTolerance = 1.0e-8;
    if (direction.x > 1.0 - axisTolerance) {
        viewPreset_ = ViewportViewPreset::Right;
    } else if (direction.x < -1.0 + axisTolerance) {
        viewPreset_ = ViewportViewPreset::Left;
    } else if (direction.y > 1.0 - axisTolerance) {
        viewPreset_ = ViewportViewPreset::Back;
    } else if (direction.y < -1.0 + axisTolerance) {
        viewPreset_ = ViewportViewPreset::Front;
    } else if (direction.z > 1.0 - axisTolerance) {
        viewPreset_ = ViewportViewPreset::Top;
    } else if (direction.z < -1.0 + axisTolerance) {
        viewPreset_ = ViewportViewPreset::Bottom;
    }
}

void ViewportTransform::beginOrbitGesture(const QPointF &screenPosition,
                                         const QSize &viewportSize)
{
    if (!orbitPivotLocked_) {
        const CameraBasis basis = cameraBasis(orientation_);
        const Vec3 target = cameraTarget(basis, pan_, orbitPivot_);
        orbitPivot_ = {target.x, target.y, target.z};
        pan_ = {};
    }
    const Vec3 vector = trackballVector(screenPosition, viewportSize);
    trackballStartVector_ = {vector.x, vector.y, vector.z};
    trackballStartOrientation_ = orientation_;
    orbitGestureActive_ = true;
}

void ViewportTransform::orbitByPixels(const QPointF &delta)
{
    if (!orbitPivotLocked_) {
        const CameraBasis previousBasis = cameraBasis(orientation_);
        const Vec3 previousTarget = cameraTarget(previousBasis, pan_, orbitPivot_);
        orbitPivot_ = {previousTarget.x, previousTarget.y, previousTarget.z};
        pan_ = {};
    }
    if (navigationPreferences_.autoPerspective && !perspective_ &&
        isAxisAlignedOrthographicView(viewPreset_) &&
        (std::abs(delta.x()) > 0.0 || std::abs(delta.y()) > 0.0)) {
        perspective_ = true;
        zoom_ = std::clamp(zoom_,
                           minimumZoomForView(perspective_, cameraPreferences_,
                                              gridSpacing_),
                           maximumViewZoom(cameraPreferences_, gridSpacing_));
    }
    const qreal radiansPerPixel =
        navigationPreferences_.turntableSensitivityRadiansPerPixel;
    // Horizontal mouse motion rotates the view in the opposite angular
    // direction because the camera is stored as a camera-to-world rotation.
    const ViewportOrientation yaw = ViewportOrientation::fromAxisAngle(
        0.0, 0.0, 1.0, -delta.x() * radiansPerPixel);
    const Vec3 right = rotate(yaw * orientation_, {1.0, 0.0, 0.0});
    const ViewportOrientation pitch = ViewportOrientation::fromAxisAngle(
        right.x, right.y, right.z, -delta.y() * radiansPerPixel);
    orientation_ = (pitch * yaw * orientation_).normalized();
    viewPreset_ = ViewportViewPreset::Custom;
}

void ViewportTransform::orbitToPosition(const QPointF &screenPosition,
                                        const QSize &viewportSize)
{
    if (!orbitGestureActive_) {
        beginOrbitGesture(screenPosition, viewportSize);
    }

    const Vec3 start{trackballStartVector_.x,
                     trackballStartVector_.y,
                     trackballStartVector_.z};
    const Vec3 current = trackballVector(screenPosition, viewportSize);
    const Vec3 drag = subtract(current, start);
    const qreal dragLength = std::sqrt(dot(drag, drag));
    if (dragLength <= 1.0e-15) {
        return;
    }

    if (navigationPreferences_.autoPerspective && !perspective_ &&
        isAxisAlignedOrthographicView(viewPreset_)) {
        perspective_ = true;
        zoom_ = std::clamp(zoom_,
                           minimumZoomForView(perspective_, cameraPreferences_,
                                              gridSpacing_),
                           maximumViewZoom(cameraPreferences_, gridSpacing_));
    }

    // Blender scales the virtual-sphere drag distance linearly, then uses the
    // cross product of the start/current vectors for the rotation axis.
    qreal angle = dragLength *
                  (3.14159265358979323846 /
                   (2.0 * kBlenderTrackballSize)) *
                  navigationPreferences_.trackballSensitivity;
    angle = std::remainder(angle, 2.0 * 3.14159265358979323846);

    const Vec3 axis = normalized(cross(start, current));
    if (dot(axis, axis) > 1.0e-15 && std::abs(angle) > 1.0e-15) {
        const ViewportOrientation inverseViewRotation =
            ViewportOrientation::fromAxisAngle(axis.x, axis.y, axis.z, -angle);
        // Blender applies this delta to its world-to-view quaternion. This
        // transform stores the inverse (camera-to-world), so invert the delta
        // and post-multiply it onto the drag-start orientation.
        orientation_ =
            (trackballStartOrientation_ * inverseViewRotation).normalized();
    }
    viewPreset_ = ViewportViewPreset::Custom;
}

void ViewportTransform::endOrbitGesture()
{
    orbitGestureActive_ = false;
}

void ViewportTransform::setOrbitPivotPreservingView(const Point3D &pivot)
{
    if (!std::isfinite(pivot.x) || !std::isfinite(pivot.y) ||
        !std::isfinite(pivot.z)) {
        return;
    }
    const CameraBasis basis = cameraBasis(orientation_);
    const Vec3 target = cameraTarget(basis, pan_, orbitPivot_);
    const Vec3 relative = subtract(target, asVec(pivot));
    orbitPivot_ = pivot;
    pan_ = QPointF(-dot(relative, basis.right), -dot(relative, basis.up));
    if (perspective_) {
        const qreal oldDistance = perspectiveCameraDistance(zoom_, cameraPreferences_);
        const qreal newDistance = oldDistance +
                                  dot(subtract(asVec(pivot), target), basis.forward);
        if (newDistance > 0.0) {
            zoom_ = std::clamp(kViewportReferenceDistance / newDistance,
                               minimumViewZoom(cameraPreferences_, gridSpacing_),
                               maximumViewZoom(cameraPreferences_, gridSpacing_));
        }
    }
    orbitPivotLocked_ = true;
}

void ViewportTransform::panByPixels(const QPointF &delta,
                                    const QSize &viewportSize)
{
    if (perspective_) {
        const qreal focalLength = viewportFocalLengthPixels(
            viewportSize, cameraPreferences_.focalLengthMillimeters);
        const qreal worldUnitsPerPixel =
            perspectiveCameraDistance(zoom_, cameraPreferences_) /
            std::max<qreal>(focalLength, 1.0e-15);
        pan_ += QPointF(delta.x() * worldUnitsPerPixel,
                        -delta.y() * worldUnitsPerPixel);
        return;
    }
    const qreal viewScale = viewScalePixelsPerWorldUnit(viewportSize);
    pan_ += QPointF(delta.x() / viewScale, -delta.y() / viewScale);
}

void ViewportTransform::resetView()
{
    zoom_ = std::clamp(1.0,
                       minimumOrthographicZoom(gridSpacing_),
                       maximumViewZoom(cameraPreferences_, gridSpacing_));
    pan_ = {};
    orbitPivot_ = {};
    gridViewDistance_ = std::clamp(
        kViewportReferenceDistance,
        minimumViewDistance(gridSpacing_),
        maximumOrthographicViewDistance(gridSpacing_));
    orbitPivotLocked_ = false;
    workPlane_ = WorkPlane::XY;
    workPlaneOffset_ = 0.0;
    workPlaneFrame_ = makeWorkPlaneFrame(workPlane_, workPlaneOffset_);
    setViewPreset(ViewportViewPreset::Top);
}

void ViewportTransform::zoomAt(const QPointF &screenPosition,
                               qreal factor,
                               const QSize &viewportSize)
{
    if (!std::isfinite(factor) || factor <= 0.0) {
        return;
    }
    const QPointF zoomPosition = navigationPreferences_.zoomToMouse
                                    ? screenPosition
                                    : QPointF(viewportSize.width() * 0.5,
                                              viewportSize.height() * 0.5);
    const qreal beforeWorldUnitsPerPixel =
        1.0 / std::max<qreal>(viewScalePixelsPerWorldUnit(viewportSize),
                              1.0e-15);
    const auto applyZoom = [this, factor] {
        if (perspective_ && navigationPreferences_.zoomMethod ==
                                ViewportZoomMethod::Scale) {
            cameraPreferences_.focalLengthMillimeters = std::clamp(
                cameraPreferences_.focalLengthMillimeters * factor,
                1.0,
                2000.0);
            return;
        }
        qreal minimumZoom = minimumZoomForView(
            perspective_, cameraPreferences_, gridSpacing_);
        if (!perspective_) {
            // If a projection switch carried in a wider view than the
            // orthographic navigation limit, preserve that scale until the
            // user zooms back inside the normal range.
            minimumZoom = std::min(minimumZoom, zoom_);
        }
        zoom_ = std::clamp(
            zoom_ * factor,
            minimumZoom,
            maximumViewZoom(cameraPreferences_, gridSpacing_));
    };
    if (!perspective_) {
        gridViewDistance_ = std::clamp(
            gridViewDistance_ / factor,
            minimumViewDistance(gridSpacing_),
            maximumOrthographicViewDistance(gridSpacing_));
    }
    applyZoom();
    if (navigationPreferences_.zoomToMouse) {
        // Blender projects the cursor at the view target's depth, independent
        // of the current construction plane. Keep that point under the cursor.
        const qreal afterWorldUnitsPerPixel =
            1.0 / std::max<qreal>(viewScalePixelsPerWorldUnit(viewportSize),
                                  1.0e-15);
        const qreal scaleChange = beforeWorldUnitsPerPixel -
                                  afterWorldUnitsPerPixel;
        const qreal offsetX = zoomPosition.x() - viewportSize.width() * 0.5;
        const qreal offsetY = viewportSize.height() * 0.5 - zoomPosition.y();
        pan_ -= QPointF(offsetX * scaleChange,
                        offsetY * scaleChange);
    }
}

} // namespace classiCAD
