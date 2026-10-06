#include "viewport_overlay.h"

namespace classiCAD {

ViewportOverlay::ViewportOverlay(const ViewportRenderer &renderer,
    const ViewportTransform &transform)
    : renderer_(renderer)
    , snapMarkerRenderer_(transform)
    , toolPreviewRenderer_(renderer, transform, snapMarkerRenderer_)
{
}

void ViewportOverlay::setSnapLabelsVisible(bool visible)
{
    snapMarkerRenderer_.setLabelsVisible(visible);
}

bool ViewportOverlay::snapLabelsVisible() const
{
    return snapMarkerRenderer_.labelsVisible();
}

void ViewportOverlay::drawSnapMarker(QPainter &painter,
                                     SnapType type,
                                     const QPointF &worldPoint,
                                     const QSize &viewportSize) const
{
    snapMarkerRenderer_.draw(painter, type, worldPoint, viewportSize);
}

void ViewportOverlay::drawWorldSnapMarker(QPainter &painter,
                                          SnapType type,
                                          const Point3D &worldPoint,
                                          const QSize &viewportSize) const
{
    snapMarkerRenderer_.drawWorld(painter, type, worldPoint, viewportSize);
}

void ViewportOverlay::drawSelectionBox(QPainter &painter,
                                       const QPointF &startScreen,
                                       const QPointF &currentScreen) const
{
    const QRectF selectionBox(startScreen, currentScreen);
    painter.setPen(QPen(QColor(QStringLiteral("#63b5e8")), 1.0, Qt::DashLine));
    painter.setBrush(QColor(99, 181, 232, 35));
    painter.drawRect(selectionBox.normalized());
}

void ViewportOverlay::drawControlPoints(QPainter &painter,
                                        const Shape &shape,
                                        const QSize &viewportSize,
                                        ObjectId shapeObjectId,
                                        ObjectId selectedObjectId,
                                        bool draggingControlPoint,
                                        int activeControlPointIndex,
                                        bool drawMarkers) const
{
    renderer_.drawControlPoints(painter,
                                shape,
                                viewportSize,
                                shapeObjectId,
                                selectedObjectId,
                                draggingControlPoint,
                                activeControlPointIndex,
                                drawMarkers);
}

void ViewportOverlay::drawSubdivisionPoints(QPainter &painter,
                                            const Shape &shape,
                                            const QVector<double> &parameters,
                                            const QSize &viewportSize,
                                            bool preview) const
{
    renderer_.drawSubdivisionPoints(painter,
                                    shape,
                                    parameters,
                                    viewportSize,
                                    preview);
}

const ViewportToolPreviewRenderer &ViewportOverlay::toolPreviewRenderer() const
{
    return toolPreviewRenderer_;
}


} // namespace classiCAD
