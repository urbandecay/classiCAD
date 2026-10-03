#pragma once

#include "tool.h"

namespace classiCAD {

class PolygonTool final : public InteractionTool {
public:
    explicit PolygonTool(ToolId tool);

    ToolId id() const override;
    void begin(ToolContext &context) override;
    bool handleMousePress(const ToolInput &input, ToolContext &context) override;
    bool handleMouseMove(const ToolInput &input, ToolContext &context) override;
    bool handleWheel(const ToolInput &input, ToolContext &context) override;
    bool handleKey(const ToolInput &input, ToolContext &context) override;
    void cancel(ToolContext &context) override;
    ToolPreview preview() const override;
    ToolStatus status() const override;

private:
    enum class NumericInput {
        None,
        Size,
        SideCount,
    };

    PolygonMode mode() const;
    Point3D cursorWorldPoint(const ToolInput &input,
                             const ToolContext &context) const;
    Point3D constrainedCursorWorld(const ToolInput &input,
                                   const ToolContext &context) const;
    void updateDrawingFrame(const Point3D &targetWorld,
                            ToolContext &context);
    QVector<QPointF> polygonVertices() const;
    Shape makePolygonShape(const QVector<QPointF> &vertices) const;
    void updateFromInput(const ToolInput &input, ToolContext &context);
    void applyNumericInput(ToolContext &context);
    void setSideCount(int sideCount, ToolContext &context, bool fromWheel = false);
    int effectiveSideCount(int sideCount) const;
    void updateStatus(const ToolContext &context);
    void beginNumericInput(NumericInput input, const QString &initialText = {});
    void publish(ToolContext &context);
    void finish(ToolContext &context);
    void reset();

    ToolId tool_ = ToolId::PolygonCenterCorner;
    QVector<QPointF> points_;
    WorkPlaneFrame drawingFrame_;
    WorkPlaneFrame referenceFrame_;
    WorkPlaneFrame frameBeforeLock_;
    ToolInput lastInput_;
    Point3D anchorWorld_;
    QPointF cursorPoint_;
    QString numericText_;
    NumericInput numericInput_ = NumericInput::None;
    int sideCount_ = 32;
    int wheelRemainder_ = 0;
    int planeAxisLockKey_ = 0;
    int verticalOverrideAxis_ = 0;
    bool hasCursorPoint_ = false;
    bool frameCaptured_ = false;
    bool planeLocked_ = false;
    bool manualPlaneLock_ = false;
    bool perpendicularMode_ = false;
    bool wasVertical_ = false;
    bool numericPointLocked_ = false;
    ToolStatus status_;
    QString hudDimensionsLine_;
    QString hudInstructionsLine_;
};

} // namespace classiCAD
