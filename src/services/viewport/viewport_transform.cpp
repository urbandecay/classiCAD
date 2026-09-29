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

Vec3 cross(const Vec3 &first, const Vec3 &second)
{
    return {first.y * second.z - first.z * second.y,
            first.z * second.x - first.x * second.z,
            first.x * second.y - first.y * second.x};
}

Vec3 normalized(const Vec3 &vector)
{
    const qreal length = std::sqrt(dot(vector, vector));
    if (length <= 1.0e-15) {
        return {};
    }
    return multiply(vector, 1.0 / length);
}

Vec3 rotateAroundAxis(const Vec3 &vector, const Vec3 &axis, qreal radians)
{
    const qreal cosine = std::cos(radians);
    const qreal sine = std::sin(radians);
    return add(add(multiply(vector, cosine),
                   multiply(cross(axis, vector), sine)),
               multiply(axis, dot(axis, vector) * (1.0 - cosine)));
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

CameraBasis cameraBasis(qreal yaw, qreal pitch, qreal roll)
{
    const qreal elevationCos = std::cos(pitch);
    const Vec3 cameraOut{elevationCos * std::sin(yaw),
                         -elevationCos * std::cos(yaw),
                         std::sin(pitch)};
    const Vec3 forward = multiply(cameraOut, -1.0);
    const Vec3 levelRight{std::cos(yaw), std::sin(yaw), 0.0};
    const Vec3 levelUp = normalized({levelRight.y * forward.z - levelRight.z * forward.y,
                                     levelRight.z * forward.x - levelRight.x * forward.z,
                                     levelRight.x * forward.y - levelRight.y * forward.x});
    const qreal cosine = std::cos(roll);
    const qreal sine = std::sin(roll);
    const Vec3 right = add(multiply(levelRight, cosine), multiply(levelUp, sine));
    const Vec3 up = add(multiply(levelUp, cosine), multiply(levelRight, -sine));
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

qreal perspectiveCameraDistance(const QSize &viewportSize, qreal zoom)
{
    // Keep the camera a stable number of viewport-widths from its pivot.
    // A fixed world-unit distance makes perspective wildly dependent on CAD
    // drawing units and causes zooming to change the apparent lens strength.
    const qreal viewportExtent = std::max(viewportSize.width(),
                                          viewportSize.height());
    constexpr qreal viewportExtentsFromPivot = 2.0;
    return viewportExtentsFromPivot * viewportExtent / zoom;
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

    const CameraBasis basis = cameraBasis(yawRadians_, pitchRadians_, rollRadians_);
    const Vec3 target = cameraTarget(basis, pan_, orbitPivot_);

    const qreal pixelX = screenPosition.x() - viewportSize.width() / 2.0;
    const qreal pixelY = viewportSize.height() / 2.0 - screenPosition.y();
    Vec3 rayOrigin;
    Vec3 rayDirection;
    if (perspective_) {
        const qreal cameraDistance = perspectiveCameraDistance(viewportSize, zoom_);
        rayOrigin = subtract(target, multiply(basis.forward, cameraDistance));
        const qreal focalLength = zoom_ * cameraDistance;
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

    const CameraBasis basis = cameraBasis(yawRadians_, pitchRadians_, rollRadians_);
    const Vec3 target = cameraTarget(basis, pan_, orbitPivot_);
    const Vec3 relative = subtract(asVec(worldPosition), target);
    const qreal viewX = dot(relative, basis.right);
    const qreal viewY = dot(relative, basis.up);
    qreal scale = zoom_;
    if (perspective_) {
        const qreal cameraDistance = perspectiveCameraDistance(viewportSize, zoom_);
        const qreal depth = cameraDistance + dot(relative, basis.forward);
        if (depth <= cameraDistance * 0.01) {
            return false;
        }
        scale *= cameraDistance / depth;
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

bool ViewportTransform::isThreeDimensionalView() const
{
    return viewPreset_ == ViewportViewPreset::Isometric ||
           viewPreset_ == ViewportViewPreset::Perspective ||
           viewPreset_ == ViewportViewPreset::Custom;
}

bool ViewportTransform::isPerspectiveEnabled() const
{
    return perspective_;
}

ViewportDirectionProjection ViewportTransform::worldDirectionToView(
    const Point3D &direction) const
{
    const CameraBasis basis = cameraBasis(yawRadians_, pitchRadians_, rollRadians_);
    const Vec3 vector = asVec(direction);
    return {dot(vector, basis.right),
            dot(vector, basis.up),
            -dot(vector, basis.forward)};
}

ViewportCameraState ViewportTransform::cameraState() const
{
    return {zoom_,
            pan_,
            orbitPivot_,
            yawRadians_,
            pitchRadians_,
            rollRadians_,
            perspective_,
            viewPreset_};
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
        !std::isfinite(state.rollRadians)) {
        return;
    }

    zoom_ = std::clamp(state.zoom, 0.01, 12.0);
    pan_ = state.pan;
    orbitPivot_ = state.orbitPivot;
    yawRadians_ = state.yawRadians;
    pitchRadians_ = state.pitchRadians;
    rollRadians_ = state.rollRadians;
    perspective_ = state.perspective;
    viewPreset_ = state.preset;
}

void ViewportTransform::setViewPreset(ViewportViewPreset preset)
{
    constexpr qreal halfPi = 1.57079632679489661923;
    constexpr qreal radians = 0.01745329251994329577;
    viewPreset_ = preset;
    if (preset == ViewportViewPreset::Custom) {
        return;
    }
    const CameraBasis previousBasis = cameraBasis(yawRadians_, pitchRadians_, rollRadians_);
    const Vec3 previousTarget = cameraTarget(previousBasis, pan_, orbitPivot_);
    orbitPivot_ = {previousTarget.x, previousTarget.y, previousTarget.z};
    pan_ = {};
    perspective_ = preset == ViewportViewPreset::Perspective;
    rollRadians_ = 0.0;
    switch (preset) {
    case ViewportViewPreset::Top:
        yawRadians_ = 0.0;
        pitchRadians_ = halfPi;
        break;
    case ViewportViewPreset::Front:
        yawRadians_ = 0.0;
        pitchRadians_ = 0.0;
        break;
    case ViewportViewPreset::Back:
        yawRadians_ = 180.0 * radians;
        pitchRadians_ = 0.0;
        break;
    case ViewportViewPreset::Right:
        yawRadians_ = halfPi;
        pitchRadians_ = 0.0;
        break;
    case ViewportViewPreset::Left:
        yawRadians_ = -halfPi;
        pitchRadians_ = 0.0;
        break;
    case ViewportViewPreset::Bottom:
        yawRadians_ = 0.0;
        pitchRadians_ = -halfPi;
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

void ViewportTransform::setViewDirection(const Point3D &cameraDirection)
{
    const Vec3 direction = normalized(asVec(cameraDirection));
    if (dot(direction, direction) <= 1.0e-15) {
        return;
    }

    const CameraBasis previousBasis = cameraBasis(yawRadians_, pitchRadians_, rollRadians_);
    const Vec3 previousTarget = cameraTarget(previousBasis, pan_, orbitPivot_);
    orbitPivot_ = {previousTarget.x, previousTarget.y, previousTarget.z};
    pan_ = {};
    yawRadians_ = std::atan2(direction.x, -direction.y);
    pitchRadians_ = std::asin(std::clamp(direction.z, -1.0, 1.0));
    rollRadians_ = 0.0;
    viewPreset_ = ViewportViewPreset::Custom;
}

void ViewportTransform::rotateViewAroundScreenAxis(const QPointF &screenAxis,
                                                    qreal degrees)
{
    if (!std::isfinite(screenAxis.x()) || !std::isfinite(screenAxis.y()) ||
        !std::isfinite(degrees)) {
        return;
    }
    const qreal screenAxisLength = std::hypot(screenAxis.x(), screenAxis.y());
    if (screenAxisLength <= 1.0e-12) {
        return;
    }

    const CameraBasis previousBasis = cameraBasis(yawRadians_, pitchRadians_, rollRadians_);
    const Vec3 previousTarget = cameraTarget(previousBasis, pan_, orbitPivot_);
    orbitPivot_ = {previousTarget.x, previousTarget.y, previousTarget.z};
    pan_ = {};

    const Vec3 worldAxis = normalized(add(
        multiply(previousBasis.right, screenAxis.x() / screenAxisLength),
        multiply(previousBasis.up, screenAxis.y() / screenAxisLength)));
    constexpr qreal radiansPerDegree = 0.01745329251994329577;
    const qreal radians = degrees * radiansPerDegree;
    const Vec3 cameraOut = multiply(previousBasis.forward, -1.0);
    const Vec3 rotatedCameraOut = normalized(rotateAroundAxis(cameraOut,
                                                               worldAxis,
                                                               radians));
    const Vec3 rotatedRight = rotateAroundAxis(previousBasis.right,
                                                worldAxis,
                                                radians);
    yawRadians_ = std::atan2(rotatedCameraOut.x, -rotatedCameraOut.y);
    pitchRadians_ = std::asin(std::clamp(rotatedCameraOut.z, -1.0, 1.0));

    const CameraBasis zeroRollBasis = cameraBasis(yawRadians_, pitchRadians_, 0.0);
    rollRadians_ = std::atan2(dot(zeroRollBasis.up, rotatedRight),
                              dot(zeroRollBasis.right, rotatedRight));
    viewPreset_ = ViewportViewPreset::Custom;
}

void ViewportTransform::rollByDegrees(qreal degrees)
{
    if (!std::isfinite(degrees)) {
        return;
    }
    const CameraBasis previousBasis = cameraBasis(yawRadians_, pitchRadians_, rollRadians_);
    const Vec3 previousTarget = cameraTarget(previousBasis, pan_, orbitPivot_);
    orbitPivot_ = {previousTarget.x, previousTarget.y, previousTarget.z};
    pan_ = {};
    constexpr qreal radiansPerDegree = 0.01745329251994329577;
    rollRadians_ = std::remainder(rollRadians_ + degrees * radiansPerDegree,
                                  2.0 * 3.14159265358979323846);
    viewPreset_ = ViewportViewPreset::Custom;
}

void ViewportTransform::reverseView()
{
    rotateViewAroundScreenAxis(QPointF(0.0, 1.0), 180.0);
}

void ViewportTransform::setPerspectiveEnabled(bool enabled)
{
    perspective_ = enabled;
    if (enabled) {
        viewPreset_ = ViewportViewPreset::Perspective;
    } else if (viewPreset_ == ViewportViewPreset::Perspective) {
        viewPreset_ = ViewportViewPreset::Custom;
    }
}

void ViewportTransform::setOrbitPivot(const Point3D &pivot)
{
    if (!std::isfinite(pivot.x) || !std::isfinite(pivot.y) ||
        !std::isfinite(pivot.z)) {
        return;
    }
    orbitPivot_ = pivot;
    pan_ = {};
}

bool ViewportTransform::frameWorldPoints(const QVector<Point3D> &points,
                                         const QSize &viewportSize,
                                         qreal marginPixels)
{
    if (points.isEmpty() || viewportSize.width() <= 0 ||
        viewportSize.height() <= 0 || !std::isfinite(marginPixels)) {
        return false;
    }

    Point3D minimum{std::numeric_limits<qreal>::infinity(),
                    std::numeric_limits<qreal>::infinity(),
                    std::numeric_limits<qreal>::infinity()};
    Point3D maximum{-minimum.x, -minimum.y, -minimum.z};
    int finitePointCount = 0;
    for (const Point3D &point : points) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
            !std::isfinite(point.z)) {
            continue;
        }
        minimum.x = std::min(minimum.x, point.x);
        minimum.y = std::min(minimum.y, point.y);
        minimum.z = std::min(minimum.z, point.z);
        maximum.x = std::max(maximum.x, point.x);
        maximum.y = std::max(maximum.y, point.y);
        maximum.z = std::max(maximum.z, point.z);
        ++finitePointCount;
    }
    if (finitePointCount == 0) {
        return false;
    }

    const qreal previousZoom = zoom_;
    const QPointF previousPan = pan_;
    const Point3D previousOrbitPivot = orbitPivot_;
    const auto restoreCamera = [this, previousZoom, previousPan, previousOrbitPivot]() {
        zoom_ = previousZoom;
        pan_ = previousPan;
        orbitPivot_ = previousOrbitPivot;
    };
    setOrbitPivot({(minimum.x + maximum.x) * 0.5,
                   (minimum.y + maximum.y) * 0.5,
                   (minimum.z + maximum.z) * 0.5});
    const qreal availableWidth = std::max<qreal>(1.0,
        viewportSize.width() - 2.0 * std::max<qreal>(0.0, marginPixels));
    const qreal availableHeight = std::max<qreal>(1.0,
        viewportSize.height() - 2.0 * std::max<qreal>(0.0, marginPixels));
    zoom_ = 0.15;
    if (perspective_) {
        const qreal worldDiagonal = std::sqrt(
            (maximum.x - minimum.x) * (maximum.x - minimum.x) +
            (maximum.y - minimum.y) * (maximum.y - minimum.y) +
            (maximum.z - minimum.z) * (maximum.z - minimum.z));
        if (worldDiagonal > 1.0e-9) {
            const qreal viewportExtent = std::max(viewportSize.width(),
                                                  viewportSize.height());
            zoom_ = std::min(zoom_, 2.0 * viewportExtent / worldDiagonal);
        }
        zoom_ = std::max<qreal>(0.01, zoom_);
    }

    bool projectedAnyPoint = false;
    constexpr int maximumFitIterations = 8;
    for (int iteration = 0; iteration < maximumFitIterations; ++iteration) {
        qreal minX = std::numeric_limits<qreal>::infinity();
        qreal minY = std::numeric_limits<qreal>::infinity();
        qreal maxX = -minX;
        qreal maxY = -minY;
        int projectedCount = 0;
        for (const Point3D &point : points) {
            QPointF screenPoint;
            if (!worldPointToScreen(point, viewportSize, &screenPoint)) {
                continue;
            }
            minX = std::min(minX, screenPoint.x());
            minY = std::min(minY, screenPoint.y());
            maxX = std::max(maxX, screenPoint.x());
            maxY = std::max(maxY, screenPoint.y());
            ++projectedCount;
        }
        if (projectedCount == 0) {
            restoreCamera();
            return false;
        }
        projectedAnyPoint = true;
        const qreal boundsWidth = maxX - minX;
        const qreal boundsHeight = maxY - minY;
        const QPointF viewportCenter(viewportSize.width() * 0.5,
                                     viewportSize.height() * 0.5);
        panByPixels(viewportCenter - QPointF((minX + maxX) * 0.5,
                                             (minY + maxY) * 0.5),
                    viewportSize);
        const qreal fitFactor = std::min(
            availableWidth / std::max<qreal>(1.0, boundsWidth),
            availableHeight / std::max<qreal>(1.0, boundsHeight));
        if (!std::isfinite(fitFactor) || fitFactor <= 0.0) {
            restoreCamera();
            return false;
        }
        const qreal previousZoom = zoom_;
        zoom_ = std::clamp(zoom_ * fitFactor * 0.96, 0.01, 12.0);
        if (std::abs(zoom_ - previousZoom) <= std::max<qreal>(1.0e-6, zoom_ * 1.0e-4)) {
            break;
        }
    }

    if (!projectedAnyPoint) {
        restoreCamera();
    }
    return projectedAnyPoint;
}

void ViewportTransform::orbitByPixels(const QPointF &delta)
{
    constexpr qreal radiansPerPixel = 0.008;
    constexpr qreal pitchLimit = 1.5690509975;
    const CameraBasis previousBasis = cameraBasis(yawRadians_, pitchRadians_, rollRadians_);
    const Vec3 previousTarget = cameraTarget(previousBasis, pan_, orbitPivot_);
    orbitPivot_ = {previousTarget.x, previousTarget.y, previousTarget.z};
    pan_ = {};
    yawRadians_ += delta.x() * radiansPerPixel;
    pitchRadians_ = std::clamp(pitchRadians_ + delta.y() * radiansPerPixel,
                               -pitchLimit,
                               pitchLimit);
    rollRadians_ = 0.0;
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
    orbitPivot_ = {};
    rollRadians_ = 0.0;
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
