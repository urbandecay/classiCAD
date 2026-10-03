#pragma once

#include "tool.h"

namespace classiCAD {

class RectangleTool final : public InteractionTool {
public:
    explicit RectangleTool(ToolId tool = ToolId::Rectangle);

    ToolId id() const override;
    void begin(ToolContext &context) override;
    bool handleMousePress(const ToolInput &input, ToolContext &context) override;
    bool handleMouseMove(const ToolInput &input, ToolContext &context) override;
    bool handleKey(const ToolInput &input, ToolContext &context) override;
    void cancel(ToolContext &context) override;
    ToolPreview preview() const override;
    ToolStatus status() const override;

private:
    enum class NumericInput { None, X, Y, Square };
    bool threePoint() const;
    bool fromCenter() const;
    Point3D eventPoint(const ToolInput &input) const;
    Point3D planePoint(const ToolInput &input, ToolContext &context, bool allowSnap) const;
    Point3D inferAxis(const ToolInput &input, ToolContext &context,
                      const Point3D &anchor, const Point3D &target) const;
    void updateGeometry(const ToolInput &input, ToolContext &context);
    void publish(ToolContext &context);
    void updateStatus(const ToolContext &context);
    Shape rectangleShape() const;
    void finish(ToolContext &context);

    ToolId tool_;
    int stage_ = 0;
    WorkPlaneFrame referenceFrame_;
    WorkPlaneFrame frame_;
    Point3D anchor_;
    Point3D edgeEnd_;
    Point3D cursor_;
    ToolInput lastInput_;
    QVector<QPointF> vertices_;
    qreal dx_ = 0.0;
    qreal dy_ = 0.0;
    qreal lockedX_ = 0.0;
    qreal lockedY_ = 0.0;
    qreal signX_ = 1.0;
    qreal signY_ = 1.0;
    bool xLocked_ = false;
    bool yLocked_ = false;
    bool perpendicular_ = false;
    bool square_ = false;
    bool planeLocked_ = false;
    bool cursorValid_ = false;
    NumericInput numeric_ = NumericInput::None;
    QString text_;
    QString dimensions_;
    QString instructions_;
    ToolStatus status_;
};

} // namespace classiCAD
