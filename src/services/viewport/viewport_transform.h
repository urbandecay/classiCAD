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

struct ViewportDirectionProjection {
    qreal horizontal = 0.0;
    qreal vertical = 0.0;
    qreal towardCamera = 0.0;
};

struct ViewportCameraState {
    qreal zoom = 1.0;
    QPointF pan;
    Point3D orbitPivot;
    qreal yawRadians = 0.0;
    qreal pitchRadians = 0.0;
    bool perspective = false;
    ViewportViewPreset preset = ViewportViewPreset::Top;
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
    ViewportCameraState cameraState() const;
    void setCameraState(const ViewportCameraState &state);
    bool isPerspectiveEnabled() const;
    void setPerspectiveEnabled(bool enabled);
    void setViewPreset(ViewportViewPreset preset);
    void setViewDirection(const Point3D &cameraDirection);
    void orbitByPixels(const QPointF &delta);
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
    qreal yawRadians_ = 0.0;
    qreal pitchRadians_ = 1.5707963267948966;
    bool perspective_ = false;
    qreal cameraDistance_ = 1000.0;
};

} // namespace classiCAD
