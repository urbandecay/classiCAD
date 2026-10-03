#pragma once

#include "core/document/object_id.h"
#include "tool.h"

namespace classiCAD {

class TwoCurveLineTool final : public InteractionTool {
public:
    explicit TwoCurveLineTool(ToolId tool);

    ToolId id() const override;
    void begin(ToolContext &context) override;
    bool handleMousePress(const ToolInput &input, ToolContext &context) override;
    bool handleMouseMove(const ToolInput &input, ToolContext &context) override;
    bool handleKey(const ToolInput &input, ToolContext &context) override;
    void cancel(ToolContext &context) override;
    ToolPreview preview() const override;
    ToolStatus status() const override;

private:
    bool collectCurves(const Shape &shape,
                       QVector<Shape::NurbsCurve2D> *curves) const;
    bool updateSolution(const ToolInput &input, const ToolContext &context);
    void finish(ToolContext &context);
    void publish(ToolContext &context);

    ToolId tool_;
    ObjectId firstCurveId_ = ObjectId::invalid();
    ObjectId secondCurveId_ = ObjectId::invalid();
    QVector<Shape::NurbsCurve2D> firstCurves_;
    QVector<Shape::NurbsCurve2D> secondCurves_;
    WorkPlaneFrame drawingFrame_;
    QPointF firstPoint_;
    QPointF secondPoint_;
    bool solutionAvailable_ = false;
    ToolStatus status_;
};

} // namespace classiCAD
