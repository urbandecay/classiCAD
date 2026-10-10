#pragma once

#include "tool.h"

namespace classiCAD {

class PointExtrudeTool final : public InteractionTool {
public:
    struct ControlPointSource {
        ObjectId objectId = ObjectId::invalid();
        Point3D worldPoint;
        WorkPlaneFrame workPlaneFrame;
        int controlPointIndex = -1;
    };
    struct CurveSource {
        ObjectId objectId = ObjectId::invalid();
        Shape::NurbsCurve2D curve;
        WorkPlaneFrame workPlaneFrame;
        QVector<int> sourceControlPointIndices;
    };

    ToolId id() const override;
    void setControlPointSources(
        const QVector<ControlPointSource> &sources);
    void setCurveSources(const QVector<CurveSource> &sources);
    void begin(ToolContext &context) override;
    bool handleMousePress(const ToolInput &input, ToolContext &context) override;
    bool handleMouseMove(const ToolInput &input, ToolContext &context) override;
    bool handleKey(const ToolInput &input, ToolContext &context) override;
    void cancel(ToolContext &context) override;
    void commit(ToolContext &context) override;
    ToolPreview preview() const override;
    ToolStatus status() const override;
    bool weldEnabled() const;
    void toggleWeld(ToolContext &context);

private:
    struct SourcePoint {
        ObjectId objectId = ObjectId::invalid();
        Point3D worldPoint;
        WorkPlaneFrame workPlaneFrame;
        Shape::NurbsCurve2D curve;
        Shape::NurbsSurface3D surface;
        int sourceControlPointIndex = -1;
        QVector<int> sourceControlPointIndices;
    };

    Point3D resolveTarget(const ToolInput &input, ToolContext &context);
    bool makeLineShape(const SourcePoint &source,
                       const Point3D &endPoint,
                       Shape *shape) const;
    QVector<Shape> makeLineShapes(const Point3D &endPoint) const;
    void updateStatus();
    void publish(ToolContext &context);

    QVector<SourcePoint> sourcePoints_;
    QVector<ControlPointSource> stagedControlPointSources_;
    QVector<CurveSource> stagedCurveSources_;
    bool stagedControlPointSourcesRequested_ = false;
    WorkPlaneFrame inputFrame_;
    Point3D cursorPoint_;
    ToolInput lastInput_;
    int constraintAxisKey_ = 0;
    bool normalConstraint_ = false;
    bool weldEnabled_ = true;
    bool hasLastInput_ = false;
    bool hasCursorPoint_ = false;
    SnapResult snap_;
    ToolStatus status_;
};

} // namespace classiCAD
