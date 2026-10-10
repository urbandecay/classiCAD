#pragma once

#include "core/document/shape.h"
#include "core/tool_id.h"
#include "services/snapping/snap_types.h"
#include "services/viewport/viewport_transform.h"
#include "tools/tool.h"
#include "viewport_renderer.h"
#include "viewport_snap_marker_renderer.h"

#include <QPainter>
#include <QSize>

namespace classiCAD {

class ViewportToolPreviewRenderer final {
public:
    ViewportToolPreviewRenderer(
        const ViewportRenderer &renderer,
        const ViewportTransform &transform,
        const ViewportSnapMarkerRenderer &snapMarkerRenderer);

    void drawLinePreview(QPainter &painter,
                         const QVector<QPointF> &pendingPoints,
                         const QPointF &cursorWorld,
                         bool cursorValid,
                         const SnapResult &currentSnap,
                         const QSize &viewportSize,
                         bool drawCurve = true,
                         const WorkPlaneFrame &workPlaneFrame = {}) const;
    void drawWorldLinePreview(QPainter &painter,
                              const QVector<Point3D> &points,
                              const Point3D &cursor,
                              bool cursorValid,
                              const QSize &viewportSize,
                              bool drawCurve,
                              const WorkPlaneFrame &workPlaneFrame = {}) const;
    void drawArcPreview(QPainter &painter,
                        const QVector<QPointF> &pendingPoints,
                        ArcMode arcMode,
                        const QPointF &cursorWorld,
                        bool cursorValid,
                        qreal arcSweep,
                        const SnapResult &currentSnap,
                        const QSize &viewportSize,
                        const WorkPlaneFrame &workPlaneFrame = {},
                        qreal compassRotation = 0.0,
                        const QColor &curveColor = QColor(),
                        bool drawCurve = true) const;
    void drawCirclePreview(QPainter &painter,
                           ToolId tool,
                           const QVector<QPointF> &pendingPoints,
                           const QPointF &cursorWorld,
                           bool cursorValid,
                           const SnapResult &currentSnap,
                           const QSize &viewportSize,
                           const WorkPlaneFrame &workPlaneFrame,
                           const QColor &curveColor,
                           bool drawCurve = true) const;
    void drawEllipsePreview(QPainter &painter,
                            const QVector<QPointF> &pendingPoints,
                            const QPointF &cursorWorld,
                            bool cursorValid,
                            const QVector<ToolPreviewGuide> &guides,
                            const SnapResult &currentSnap,
                            const QSize &viewportSize) const;
    void drawRectanglePreview(QPainter &painter,
                              ToolId tool,
                              const QVector<QPointF> &pendingPoints,
                              const QPointF &cursorWorld,
                              bool cursorValid,
                              const SnapResult &currentSnap,
                              const QSize &viewportSize,
                              bool drawCurve = true) const;
    void drawPolygonPreview(QPainter &painter,
                            ToolId tool,
                            int sideCount,
                            const QVector<QPointF> &pendingPoints,
                            const QPointF &cursorWorld,
                            bool cursorValid,
                            const SnapResult &currentSnap,
                            const QSize &viewportSize,
                            bool drawCurve = true) const;
    void drawPointPreview(QPainter &painter,
                          const QPointF &cursorWorld,
                          bool cursorValid,
                          const SnapResult &currentSnap,
                          const QSize &viewportSize,
                          bool drawPoint = true) const;
    void drawRotatePreview(QPainter &painter,
                           const QPointF &cursorWorld,
                           bool cursorValid,
                           int rotateStep,
                           const WorkPlaneFrame &rotateFrame,
                           const QPointF &rotateBaseWorld,
                           const QPointF &rotateReferenceWorld,
                           qreal rotatePreviewAngle,
                           const QString &rotateAngleInput,
                           bool rotateAngleInputActive,
                           bool rotateAngleSnapEnabled,
                           qreal rotateAngleSnapIncrementDegrees,
                           bool rotateAngleInputInRadians,
                           const SnapResult &currentSnap,
                           const QSize &viewportSize,
                           bool drawGeometry = true) const;

private:
    void drawSnapMarker(QPainter &painter,
                        const SnapResult &snap,
                        const QSize &viewportSize) const;

    const ViewportRenderer &renderer_;
    const ViewportTransform &transform_;
    const ViewportSnapMarkerRenderer &snapMarkerRenderer_;
};

} // namespace classiCAD
