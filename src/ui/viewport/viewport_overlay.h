#pragma once

#include "core/model.h"
#include "core/tool_id.h"
#include "tools/tool.h"
#include "viewport_renderer.h"

#include <QPainter>
#include <QSize>

namespace classiCAD {

enum class BlenderNavigationAction {
    None,
    Orbit,
    Axis,
    Zoom,
    Pan,
    Camera,
    Projection,
};

struct BlenderNavigationHit {
    BlenderNavigationAction action = BlenderNavigationAction::None;
    Point3D direction;
};

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
                           const QPointF &rotateBaseWorld,
                           const QPointF &rotateReferenceWorld,
                           qreal rotatePreviewAngle,
                           const SnapResult &currentSnap,
                           const QSize &viewportSize,
                           bool drawGeometry = true) const;
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
                        const QString &lineCommandStatus,
                        int rotateStep,
                        bool grabActive,
                        bool grabPickingBasePoint,
                        bool grabHasBasePoint,
                        bool duplicateActive,
                        bool duplicatePickingBasePoint,
                        bool duplicateHasBasePoint) const;
    void drawBlenderNavigationGizmo(QPainter &painter,
                                    const QSize &viewportSize,
                                    const QPointF &hoverPosition) const;
    void drawBlenderNavigationGizmoContents(QPainter &painter,
                                            const QSize &viewportSize,
                                            const QPointF &hoverPosition) const;
    BlenderNavigationHit blenderNavigationGizmoHitAt(
        const QPointF &screenPosition,
        const QSize &viewportSize) const;

private:
    void drawSampledEraseIntervals(QPainter &painter,
                                   const SampledNurbsCurve2D &sampled,
                                   const QVector<ParameterInterval> &intervals) const;

    const ViewportRenderer &renderer_;
    const ViewportTransform &transform_;
    bool snapLabelsVisible_ = true;
};

} // namespace classiCAD
