#pragma once

#include "core/document/object_id.h"
#include "tool.h"

namespace classiCAD {

struct TangentCircleCandidate {
    QPointF center;
    qreal radius = 0.0;
};

class CircleTangentTool final : public InteractionTool {
public:
    explicit CircleTangentTool(ToolId tool);

    ToolId id() const override;
    void begin(ToolContext &context) override;
    bool handleMousePress(const ToolInput &input, ToolContext &context) override;
    bool handleMouseMove(const ToolInput &input, ToolContext &context) override;
    bool handleKey(const ToolInput &input, ToolContext &context) override;
    void cancel(ToolContext &context) override;
    ToolPreview preview() const override;
    ToolStatus status() const override;

private:
    bool collectTargetCurves(const Shape &shape,
                             ToolContext &context,
                             QVector<QVector<QPointF>> *sampledCurves) const;
    bool updateCirclePreview(const ToolInput &input, ToolContext &context);
    bool cycleTangentSolution(int direction, ToolContext &context);
    void publish(ToolContext &context);

    ToolId tool_;
    QVector<ObjectId> selectedCurveIds_;
    QVector<QVector<QVector<QPointF>>> sampledCurveGroups_;
    QVector<bool> closedCurveTargets_;
    WorkPlaneFrame drawingFrame_;
    QVector<TangentCircleCandidate> circleTargets_;
    QVector<TangentCircleCandidate> exactSolutions_;
    ToolInput lastPreviewInput_;
    Shape previewShape_;
    bool previewAvailable_ = false;
    bool hasLastPreviewInput_ = false;
    bool solutionCycleActive_ = false;
    int tangentSolutionIndex_ = -1;
    ToolStatus status_;
};

} // namespace classiCAD
