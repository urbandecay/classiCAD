#pragma once

#include "core/geometry/work_plane.h"

#include <QPointF>
#include <QSize>

namespace classiCAD {

enum class ViewportViewPreset {
    Top = 0,
    Front = 1,
    Right = 2,
    Isometric = 3,
    Perspective = 4,
    Custom = 5,
    Bottom = 6,
    Back = 7,
    Left = 8,
};

enum class ViewportZoomMethod {
    Dolly,
    Scale,
};

enum class ViewportZoomAxis {
    Vertical,
    Horizontal,
};

struct ViewportCameraPreferences {
    qreal focalLengthMillimeters = 50.0;
    qreal clipStart = 0.01;
    qreal clipEnd = 1000.0;
};

struct ViewportNavigationPreferences {
    bool autoPerspective = true;
    bool zoomToMouse = true;
    bool orbitAroundActive = true;
    bool useMouseDepthNavigate = true;
    qreal turntableSensitivityRadiansPerPixel = 0.006981316953897476;
    bool invertMouseZoom = false;
    bool invertZoomWheel = false;
    ViewportZoomMethod zoomMethod = ViewportZoomMethod::Dolly;
    ViewportZoomAxis zoomAxis = ViewportZoomAxis::Vertical;
};

struct ViewportDirectionProjection {
    qreal horizontal = 0.0;
    qreal vertical = 0.0;
    qreal towardCamera = 0.0;
};

struct ViewportOrientation {
    // Qt's QQuaternion stores floats; CAD projection/picking needs qreal precision.
    qreal w = 1.0;
    qreal x = 0.0;
    qreal y = 0.0;
    qreal z = 0.0;

    static ViewportOrientation fromAxisAngle(qreal axisX, qreal axisY,
                                              qreal axisZ, qreal radians);
    static ViewportOrientation slerp(const ViewportOrientation &start,
                                     const ViewportOrientation &end,
                                     qreal fraction);
    ViewportOrientation normalized() const;
    ViewportOrientation operator*(const ViewportOrientation &other) const;
    qreal dot(const ViewportOrientation &other) const;
};

struct ViewportCameraState {
    qreal zoom = 1.0;
    QPointF pan;
    Point3D orbitPivot;
    qreal yawRadians = 0.0;
    qreal pitchRadians = 0.0;
    bool perspective = false;
    ViewportViewPreset preset = ViewportViewPreset::Top;
    // Blender's rv3d->dist drives free-angle orthographic grid LOD. The
    // orthographic projection itself remains controlled by zoom.
    qreal gridViewDistance = 60.0;
    ViewportOrientation orientation;
    bool hasOrientation = false;
};

class ViewportTransform final {
public:
    ViewportTransform() = default;

    qreal zoom() const;
    qreal &zoom();
    QPointF pan() const;
    QPointF &pan();

    QPointF screenToWorld(const QPointF &screenPosition,
                          const QSize &viewportSize) const;
    QPointF worldToScreen(const QPointF &worldPosition,
                          const QSize &viewportSize) const;
    bool screenToWorkPlane(const QPointF &screenPosition,
                           const QSize &viewportSize,
                           WorkPlane plane,
                           qreal planeOffset,
                           QPointF *workPlanePosition) const;
    QPointF workPlaneToScreen(const QPointF &workPlanePosition,
                              const QSize &viewportSize,
                              WorkPlane plane,
                              qreal planeOffset = 0.0) const;
    bool worldPointToScreen(const Point3D &worldPosition,
                            const QSize &viewportSize,
                            QPointF *screenPosition) const;

    WorkPlane workPlane() const;
    qreal workPlaneOffset() const;
    void setWorkPlane(WorkPlane plane, qreal offset = 0.0);

    ViewportViewPreset viewPreset() const;
    ViewportDirectionProjection worldDirectionToView(const Point3D &direction) const;
    Point3D viewDirection() const;
    Point3D viewUp() const;
    Point3D viewTarget() const;
    // Returns the finite eye position used by the perspective projection.
    Point3D cameraPosition(const QSize &viewportSize) const;
    ViewportCameraPreferences cameraPreferences() const;
    bool setCameraPreferences(const ViewportCameraPreferences &preferences);
    ViewportNavigationPreferences navigationPreferences() const;
    void setNavigationPreferences(const ViewportNavigationPreferences &preferences);
    ViewportCameraState cameraState() const;
    void setCameraState(const ViewportCameraState &state);
    bool isPerspectiveEnabled() const;
    void setPerspectiveEnabled(bool enabled);
    void setViewPreset(ViewportViewPreset preset);
    void setViewDirection(const Point3D &cameraDirection);
    void orbitByPixels(const QPointF &delta);
    void setOrbitPivotPreservingView(const Point3D &pivot);
    void panByPixels(const QPointF &delta, const QSize &viewportSize);
    void resetView();

    void zoomAt(const QPointF &screenPosition,
                qreal factor,
                const QSize &viewportSize,
                qreal minimumZoom,
                qreal maximumZoom);

private:
    qreal zoom_ = 1.0;
    QPointF pan_{0.0, 0.0};
    WorkPlane workPlane_ = WorkPlane::XY;
    qreal workPlaneOffset_ = 0.0;
    Point3D orbitPivot_{};
    ViewportViewPreset viewPreset_ = ViewportViewPreset::Top;
    ViewportOrientation orientation_;
    bool perspective_ = false;
    qreal gridViewDistance_ = 60.0;
    ViewportCameraPreferences cameraPreferences_;
    ViewportNavigationPreferences navigationPreferences_;
    bool orbitPivotLocked_ = false;
};

} // namespace classiCAD
