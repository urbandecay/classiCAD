#pragma once

#include <QPointF>
#include <QSize>

namespace classiCAD {

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

    void zoomAt(const QPointF &screenPosition,
                qreal factor,
                const QSize &viewportSize,
                qreal minimumZoom,
                qreal maximumZoom);

private:
    qreal zoom_ = 1.0;
    QPointF pan_{0.0, 0.0};
};

} // namespace classiCAD
