#include "viewport_transform.h"

#include <algorithm>

namespace classiCAD {

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
    return QPointF((screenPosition.x() - viewportSize.width() / 2.0) / zoom_ - pan_.x(),
                   (viewportSize.height() / 2.0 - screenPosition.y()) / zoom_ - pan_.y());
}

QPointF ViewportTransform::worldToScreen(const QPointF &worldPosition,
                                         const QSize &viewportSize) const
{
    return QPointF(viewportSize.width() / 2.0 + (worldPosition.x() + pan_.x()) * zoom_,
                   viewportSize.height() / 2.0 - (worldPosition.y() + pan_.y()) * zoom_);
}

void ViewportTransform::zoomAt(const QPointF &screenPosition,
                               qreal factor,
                               const QSize &viewportSize,
                               qreal minimumZoom,
                               qreal maximumZoom)
{
    const QPointF beforeZoom = screenToWorld(screenPosition, viewportSize);
    zoom_ = std::clamp(zoom_ * factor, minimumZoom, maximumZoom);
    const QPointF afterZoom = screenToWorld(screenPosition, viewportSize);
    pan_ += afterZoom - beforeZoom;
}

} // namespace classiCAD
