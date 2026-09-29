#include "viewport_transform.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace classiCAD {
namespace {

constexpr qreal kViewportSensorWidthMillimeters = 36.0;
constexpr qreal kViewportReferenceDistance = 60.0;
// BKE_camera_params_from_view3d uses CAMERA_PARAM_ZOOM_INIT_PERSP = 2.
constexpr qreal kBlenderViewportProjectionZoom = 2.0;

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

qreal minimumPerspectiveZoom(const ViewportCameraPreferences &preferences)
{
    // ED_view3d_dist_soft_range_get allows a view distance up to 10 * clip_end.
    return perspectiveReferenceDistance(preferences) /
           std::max<qreal>(preferences.clipEnd * 10.0, 0.001);
}

qreal maximumPerspectiveZoom(const ViewportCameraPreferences &preferences)
{
    // Blender uses grid spacing * 0.001 for ordinary 3D zoom's soft minimum.
    constexpr qreal minimumDistance = 0.001;
    return perspectiveReferenceDistance(preferences) / minimumDistance;
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

Vec3 cameraTarget(const CameraBasis &basis,
                  const QPointF &pan,
                  const Point3D &orbitPivot)
{
    return add(asVec(orbitPivot),
               add(multiply(basis.right, -pan.x()),
                   multiply(basis.up, -pan.y())));
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
    const Vec3 target = cameraTarget(basis, pan_, orbitPivot_);
    const qreal focalLength = viewportFocalLengthPixels(
        viewportSize, cameraPreferences_.focalLengthMillimeters);

    const qreal pixelX = screenPosition.x() - viewportSize.width() / 2.0;
    const qreal pixelY = viewportSize.height() / 2.0 - screenPosition.y();
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
    const Vec3 target = cameraTarget(basis, pan_, orbitPivot_);
    const Vec3 relative = subtract(asVec(worldPosition), target);
    const qreal viewX = dot(relative, basis.right);
    const qreal viewY = dot(relative, basis.up);
    qreal scale = zoom_;
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

ViewportDirectionProjection ViewportTransform::worldDirectionToView(
    const Point3D &direction) const
{
    const CameraBasis basis = cameraBasis(yawRadians_, pitchRadians_);
    const Vec3 vector = asVec(direction);
    return {dot(vector, basis.right),
            dot(vector, basis.up),
            -dot(vector, basis.forward)};
}

Point3D ViewportTransform::viewDirection() const
{
    const qreal elevationCos = std::cos(pitchRadians_);
    return {elevationCos * std::sin(yawRadians_),
            -elevationCos * std::cos(yawRadians_),
            std::sin(pitchRadians_)};
}

Point3D ViewportTransform::viewTarget() const
{
    const CameraBasis basis = cameraBasis(yawRadians_, pitchRadians_);
    const Vec3 target = cameraTarget(basis, pan_, orbitPivot_);
    return {target.x, target.y, target.z};
}

Point3D ViewportTransform::cameraPosition(const QSize &viewportSize) const
{
    Q_UNUSED(viewportSize)
    const CameraBasis basis = cameraBasis(yawRadians_, pitchRadians_);
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
    if (perspective_) {
        zoom_ = std::clamp(zoom_,
                           minimumPerspectiveZoom(cameraPreferences_),
                           maximumPerspectiveZoom(cameraPreferences_));
    }
    return true;
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
        preferences.turntableSensitivityRadiansPerPixel > 0.08726646259971647) {
        return;
    }
    navigationPreferences_ = preferences;
}

ViewportCameraState ViewportTransform::cameraState() const
{
    return {zoom_, pan_, orbitPivot_, yawRadians_, pitchRadians_,
            perspective_, viewPreset_, gridViewDistance_};
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
    perspective_ = state.perspective;
    zoom_ = perspective_
                ? std::clamp(state.zoom,
                             minimumPerspectiveZoom(cameraPreferences_),
                             maximumPerspectiveZoom(cameraPreferences_))
                : std::clamp(state.zoom, 0.01, 12.0);
    pan_ = state.pan;
    orbitPivot_ = state.orbitPivot;
    yawRadians_ = state.yawRadians;
    pitchRadians_ = state.pitchRadians;
    viewPreset_ = state.preset;
    gridViewDistance_ = std::clamp(state.gridViewDistance, 0.001, 1.0e8);
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
                           minimumPerspectiveZoom(cameraPreferences_),
                           maximumPerspectiveZoom(cameraPreferences_));
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
    orbitPivotLocked_ = false;
    const CameraBasis previousBasis = cameraBasis(yawRadians_, pitchRadians_);
    const Vec3 previousTarget = cameraTarget(previousBasis, pan_, orbitPivot_);
    orbitPivot_ = {previousTarget.x, previousTarget.y, previousTarget.z};
    pan_ = {};
    viewPreset_ = preset;
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
    case ViewportViewPreset::Bottom:
        yawRadians_ = 0.0;
        pitchRadians_ = -halfPi;
        break;
    case ViewportViewPreset::Back:
        yawRadians_ = 180.0 * radians;
        pitchRadians_ = 0.0;
        break;
    case ViewportViewPreset::Left:
        yawRadians_ = -halfPi;
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
    if (perspective_) {
        zoom_ = std::clamp(zoom_,
                           minimumPerspectiveZoom(cameraPreferences_),
                           maximumPerspectiveZoom(cameraPreferences_));
    }
}

void ViewportTransform::setViewDirection(const Point3D &cameraDirection)
{
    const Vec3 direction = normalized(asVec(cameraDirection));
    if (dot(direction, direction) <= 1.0e-15) {
        return;
    }

    const CameraBasis previousBasis = cameraBasis(yawRadians_, pitchRadians_);
    const Vec3 previousTarget = cameraTarget(previousBasis, pan_, orbitPivot_);
    orbitPivot_ = {previousTarget.x, previousTarget.y, previousTarget.z};
    pan_ = {};
    orbitPivotLocked_ = false;
    yawRadians_ = std::atan2(direction.x, -direction.y);
    pitchRadians_ = std::asin(std::clamp(direction.z, -1.0, 1.0));
    perspective_ = false;
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

void ViewportTransform::orbitByPixels(const QPointF &delta)
{
    constexpr qreal pitchLimit = 1.5690509975;
    if (!orbitPivotLocked_) {
        const CameraBasis previousBasis = cameraBasis(yawRadians_, pitchRadians_);
        const Vec3 previousTarget = cameraTarget(previousBasis, pan_, orbitPivot_);
        orbitPivot_ = {previousTarget.x, previousTarget.y, previousTarget.z};
        pan_ = {};
    }
    if (navigationPreferences_.autoPerspective &&
        (std::abs(delta.x()) > 0.0 || std::abs(delta.y()) > 0.0)) {
        perspective_ = true;
        zoom_ = std::clamp(zoom_,
                           minimumPerspectiveZoom(cameraPreferences_),
                           maximumPerspectiveZoom(cameraPreferences_));
    }
    const qreal radiansPerPixel =
        navigationPreferences_.turntableSensitivityRadiansPerPixel;
    yawRadians_ += delta.x() * radiansPerPixel;
    pitchRadians_ = std::clamp(pitchRadians_ + delta.y() * radiansPerPixel,
                               -pitchLimit,
                               pitchLimit);
    viewPreset_ = ViewportViewPreset::Custom;
}

void ViewportTransform::setOrbitPivotPreservingView(const Point3D &pivot)
{
    if (!std::isfinite(pivot.x) || !std::isfinite(pivot.y) ||
        !std::isfinite(pivot.z)) {
        return;
    }
    const CameraBasis basis = cameraBasis(yawRadians_, pitchRadians_);
    const Vec3 target = cameraTarget(basis, pan_, orbitPivot_);
    const Vec3 relative = subtract(target, asVec(pivot));
    orbitPivot_ = pivot;
    pan_ = QPointF(-dot(relative, basis.right), -dot(relative, basis.up));
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
    pan_ += QPointF(delta.x() / zoom_, -delta.y() / zoom_);
}

void ViewportTransform::resetView()
{
    zoom_ = 1.0;
    pan_ = {};
    orbitPivot_ = {};
    gridViewDistance_ = 60.0;
    orbitPivotLocked_ = false;
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
    if (!std::isfinite(factor) || factor <= 0.0) {
        return;
    }
    const QPointF zoomPosition = navigationPreferences_.zoomToMouse
                                    ? screenPosition
                                    : QPointF(viewportSize.width() * 0.5,
                                              viewportSize.height() * 0.5);
    const qreal beforeWorldUnitsPerPixel = perspective_
        ? perspectiveCameraDistance(zoom_, cameraPreferences_) /
              std::max<qreal>(viewportFocalLengthPixels(
                                  viewportSize,
                                  cameraPreferences_.focalLengthMillimeters),
                              1.0e-15)
        : 1.0 / zoom_;
    const auto applyZoom = [this, factor, minimumZoom, maximumZoom] {
        if (perspective_ && navigationPreferences_.zoomMethod ==
                                ViewportZoomMethod::Scale) {
            cameraPreferences_.focalLengthMillimeters = std::clamp(
                cameraPreferences_.focalLengthMillimeters * factor,
                1.0,
                2000.0);
            return;
        }
        const qreal effectiveMinimumZoom = perspective_
            ? minimumPerspectiveZoom(cameraPreferences_)
            : minimumZoom;
        const qreal effectiveMaximumZoom = perspective_
            ? maximumPerspectiveZoom(cameraPreferences_)
            : maximumZoom;
        if (effectiveMaximumZoom >= effectiveMinimumZoom) {
            zoom_ = std::clamp(zoom_ * factor,
                               effectiveMinimumZoom,
                               effectiveMaximumZoom);
        }
    };
    if (!perspective_) {
        gridViewDistance_ = std::clamp(gridViewDistance_ / factor,
                                       0.001,
                                       1.0e8);
    }
    applyZoom();
    if (navigationPreferences_.zoomToMouse) {
        // Blender projects the cursor at the view target's depth, independent
        // of the current construction plane. Keep that point under the cursor.
        const qreal afterWorldUnitsPerPixel = perspective_
            ? perspectiveCameraDistance(zoom_, cameraPreferences_) /
                  std::max<qreal>(viewportFocalLengthPixels(
                                      viewportSize,
                                      cameraPreferences_.focalLengthMillimeters),
                                  1.0e-15)
            : 1.0 / zoom_;
        const qreal scaleChange = beforeWorldUnitsPerPixel -
                                  afterWorldUnitsPerPixel;
        const qreal offsetX = zoomPosition.x() - viewportSize.width() * 0.5;
        const qreal offsetY = viewportSize.height() * 0.5 - zoomPosition.y();
        pan_ -= QPointF(offsetX * scaleChange,
                        offsetY * scaleChange);
    }
}

} // namespace classiCAD
