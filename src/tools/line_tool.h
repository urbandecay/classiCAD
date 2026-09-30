#pragma once
#include "tool.h"
namespace classiCAD {
class LineTool final : public InteractionTool {
public:
    ToolId id() const override;
    void begin(ToolContext &context) override;
    bool handleMousePress(const ToolInput &input, ToolContext &context) override;
    bool handleMouseMove(const ToolInput &input, ToolContext &context) override;
    bool handleKey(const ToolInput &input, ToolContext &context) override;
    void cancel(ToolContext &context) override;
    void commit(ToolContext &context) override;
    ToolPreview preview() const override;
    ToolStatus status() const override;
private:
    Point3D resolveCursorPoint(const ToolInput &input, const ToolContext &context);
    Point3D inferredAxisDirection(const ToolInput &input, const ToolContext &context) const;
    void publish(ToolContext &context);
    void reset();
    QVector<Point3D> points_;
    WorkPlaneFrame drawingFrame_;
    Point3D cursorPoint_;
    bool hasCursorPoint_ = false;
    int constraintAxisKey_ = 0;
    bool normalLock_ = false;
    bool planeLocked_ = false;
    Point3D shiftLockDirection_;
    bool shiftLockActive_ = false;
    ToolInput lastInput_;
    SnapResult snap_;
    ToolStatus status_;
};
} // namespace classiCAD
