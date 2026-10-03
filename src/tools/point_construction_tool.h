#pragma once

#include "tool.h"

#include <QString>

namespace classiCAD {

class PointConstructionTool final : public InteractionTool {
public:
    explicit PointConstructionTool(ToolId tool);

    ToolId id() const override;
    void begin(ToolContext &context) override;
    bool handleMousePress(const ToolInput &input, ToolContext &context) override;
    bool handleMouseMove(const ToolInput &input, ToolContext &context) override;
    bool handleKey(const ToolInput &input, ToolContext &context) override;
    void cancel(ToolContext &context) override;
    ToolPreview preview() const override;
    ToolStatus status() const override;

private:
    enum class NumericInputTarget {
        None,
        Radius,
        Angle,
        Length,
    };

    bool addPointOnCurve(const Shape &shape,
                         const QPointF &cursor,
                         bool edgeCenter,
                         QPointF *point,
                         WorkPlaneFrame *frame) const;
    bool buildPointCenterPreview(ToolContext &context);
    void updateArcPreview(const ToolInput &input, ToolContext &context);
    void updateArcIntersections(bool secondArcIsFullCircle);
    bool commitArcIntersections(ToolContext &context);
    bool commitPointCenter(ToolContext &context);
    bool commitEdgeCenter(ToolContext &context);
    void updatePointLineCursor(const ToolInput &input, ToolContext &context);
    void togglePerpendicularPlane(const ToolInput &input, ToolContext &context);
    bool applyNumericInput(ToolContext &context);
    void finishPointChain(ToolContext &context);
    void publish(ToolContext &context);

    ToolId tool_;
    QVector<Point3D> pointLineWorldPoints_;
    QVector<QPointF> arcCenters_;
    QVector<qreal> arcRadii_;
    QVector<qreal> arcStartAngles_;
    QVector<qreal> arcSweepAngles_;
    QVector<WorkPlaneFrame> arcFrames_;
    QVector<QPointF> arcOneEndpoints_;
    QVector<QPointF> arcIntersections_;
    QVector<Shape> pointCenterSourceShapes_;
    int pointCenterSourceShapeCount_ = 0;
    WorkPlaneFrame drawingFrame_;
    WorkPlaneFrame initialDrawingFrame_;
    WorkPlaneFrame pointCenterFrame_;
    WorkPlaneFrame edgeCenterFrame_;
    WorkPlaneFrame frameBeforeAxisLock_;
    QPointF cursorPoint_;
    QPointF pointCenter_;
    QPointF edgeCenter_;
    Point3D pointLineWorldCursor_;
    Point3D pointLineShiftDirection_;
    Point3D pointLineAxisDirection_;
    qreal compassRadius_ = 0.0;
    qreal compassRotation_ = 0.0;
    bool hasCursorPoint_ = false;
    bool hasPointCenter_ = false;
    bool hasEdgeCenter_ = false;
    bool planeLocked_ = false;
    bool angleSnapEnabled_ = true;
    bool perpendicularMode_ = false;
    bool pointLineShiftActive_ = false;
    bool hasPointLineAxisDirection_ = false;
    int normalAxisLockKey_ = 0;
    int pointLineAxisLockKey_ = 0;
    int arcStage_ = 0;
    qreal previousArcAngle_ = 0.0;
    bool hasPreviousArcAngle_ = false;
    NumericInputTarget numericInputTarget_ = NumericInputTarget::None;
    QString numericInput_;
    ToolInput lastToolInput_;
    SnapResult pointLineSnap_;
    ToolStatus status_;
};

} // namespace classiCAD
