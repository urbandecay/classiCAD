#pragma once

#include "core/model.h"
#include "core/tool_id.h"
#include "viewport_renderer.h"

#include <QPainter>
#include <QSize>

namespace classiCAD {

class ViewportOverlay {
public:
    ViewportOverlay(const ViewportRenderer &renderer,
                    const ViewportTransform &transform);

    void drawSnapMarker(QPainter &painter,
                        SnapType type,
                        const QPointF &worldPoint,
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
                           int activeControlPointIndex) const;
    void drawSubdivisionPoints(QPainter &painter,
                               const Shape &shape,
                               const QVector<double> &parameters,
                               const QSize &viewportSize,
                               bool preview) const;

    void drawLinePreview(QPainter &painter,
                         const QVector<QPointF> &pendingPoints,
                         const QPointF &cursorWorld,
                         bool cursorValid,
                         const SnapResult &currentSnap,
                         const QSize &viewportSize) const;
    void drawArcPreview(QPainter &painter,
                        const QVector<QPointF> &pendingPoints,
                        ArcMode arcMode,
                        const QPointF &cursorWorld,
                        bool cursorValid,
                        qreal arcSweep,
                        const SnapResult &currentSnap,
                        const QSize &viewportSize) const;
    void drawCirclePreview(QPainter &painter,
                           const QVector<QPointF> &pendingPoints,
                           const QPointF &cursorWorld,
                           bool cursorValid,
                           const SnapResult &currentSnap,
                           const QSize &viewportSize) const;
    void drawRectanglePreview(QPainter &painter,
                              const QVector<QPointF> &pendingPoints,
                              const QPointF &cursorWorld,
                              bool cursorValid,
                              const SnapResult &currentSnap,
                              const QSize &viewportSize) const;
    void drawPointPreview(QPainter &painter,
                          const QPointF &cursorWorld,
                          bool cursorValid,
                          const SnapResult &currentSnap,
                          const QSize &viewportSize) const;
    void drawRotatePreview(QPainter &painter,
                           const QPointF &cursorWorld,
                           bool cursorValid,
                           int rotateStep,
                           const QPointF &rotateBaseWorld,
                           const QPointF &rotateReferenceWorld,
                           qreal rotatePreviewAngle,
                           const SnapResult &currentSnap,
                           const QSize &viewportSize) const;
    void drawEraseCandidatePreview(
        QPainter &painter,
        const QVector<EraseCurveSampleCache> &targetCurves,
        int shapeIndex,
        bool eraseStrokeHasPoints) const;
    void drawErasePreview(QPainter &painter,
                          ToolId activeTool,
                          const QPointF &eraseCursorScreen,
                          bool cursorValid,
                          bool eraseCursorPressed,
                          int candidateCount) const;
    void drawToolStatus(QPainter &painter,
                        const QSize &viewportSize,
                        ToolId activeTool,
                        ArcMode arcMode,
                        bool subdivisionActive,
                        int subdivisionSections,
                        bool joinActive,
                        int joinCount,
                        bool lineCommandActive,
                        int rotateStep) const;

private:
    void drawSampledEraseIntervals(QPainter &painter,
                                   const SampledNurbsCurve2D &sampled,
                                   const QVector<ParameterInterval> &intervals) const;

    const ViewportRenderer &renderer_;
    const ViewportTransform &transform_;
};

} // namespace classiCAD
