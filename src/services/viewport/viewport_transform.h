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
    void setViewPreset(ViewportViewPreset preset);
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
    ViewportViewPreset viewPreset_ = ViewportViewPreset::Top;
    qreal yawRadians_ = 0.0;
    qreal pitchRadians_ = 1.5707963267948966;
    bool perspective_ = false;
    qreal cameraDistance_ = 1000.0;
};

} // namespace classiCAD
