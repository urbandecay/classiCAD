#pragma once

#include "core/document/shape.h"
#include "core/tool_id.h"
#include "services/snapping/snap_types.h"
#include "tools/tool.h"
#include "viewport_tool_preview_renderer.h"
#include "viewport_snap_marker_renderer.h"
#include "viewport_renderer.h"

#include <QPainter>
#include <QSize>

namespace classiCAD {

class ViewportOverlay {
public:
    ViewportOverlay(const ViewportRenderer &renderer,
                    const ViewportTransform &transform);

    void setSnapLabelsVisible(bool visible);
    bool snapLabelsVisible() const;
    void drawSnapMarker(QPainter &painter,
                        SnapType type,
                        const QPointF &worldPoint,
                        const QSize &viewportSize) const;
    void drawWorldSnapMarker(QPainter &painter,
                             SnapType type,
                             const Point3D &worldPoint,
                             const QSize &viewportSize) const;
    void drawSelectionBox(QPainter &painter,
                          const QPointF &startScreen,
                          const QPointF &currentScreen) const;
    void drawControlPoints(QPainter &painter,
                           const Shape &shape,
                           const QSize &viewportSize,
                           ObjectId shapeObjectId,
                           ObjectId selectedObjectId,
                           bool draggingControlPoint,
                           int activeControlPointIndex,
                           bool drawMarkers = true) const;
    void drawSubdivisionPoints(QPainter &painter,
                               const Shape &shape,
                               const QVector<double> &parameters,
                               const QSize &viewportSize,
                               bool preview) const;
    const ViewportToolPreviewRenderer &toolPreviewRenderer() const;

private:
    const ViewportRenderer &renderer_;
    ViewportSnapMarkerRenderer snapMarkerRenderer_;
    ViewportToolPreviewRenderer toolPreviewRenderer_;
};

} // namespace classiCAD
