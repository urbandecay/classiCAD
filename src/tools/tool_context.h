#pragma once

#include "core/document/document.h"
#include "core/document/selection_model.h"
#include "core/history/history.h"
#include "services/hit_testing/curve_hit_tester.h"
#include "services/sampling/curve_sampler.h"
#include "services/snapping/snap_engine.h"
#include "services/viewport/viewport_transform.h"
#include "tool.h"

#include <functional>

namespace classiCAD {

class ToolContext {
public:
    using ShapeFactory = std::function<bool(ToolId,
                                            const QVector<QPointF> &,
                                            ArcMode,
                                            qreal,
                                            Shape *)>;
    using ShapeCommitter = std::function<bool(ToolId, const Shape &)>;
    using ToolFinisher = std::function<void(ToolId)>;
    using PreviewPublisher = std::function<void(const ToolPreview &)>;
    using StatusPublisher = std::function<void(const ToolStatus &)>;
    using PointConstraint = std::function<QPointF(ToolId,
                                                  const QPointF &,
                                                  const QVector<QPointF> &)>;
    using ArcModeProvider = std::function<ArcMode()>;
    using ArcSweepProvider = std::function<qreal()>;

    ToolContext(Document &document,
                SelectionModel &selection,
                History &history,
                ViewportTransform &viewportTransform,
                CurveSampler &curveSampler,
                CurveHitTester &curveHitTester,
                SnapEngine &snapEngine);

    Document &document() const;
    SelectionModel &selection() const;
    History &history() const;
    ViewportTransform &viewportTransform() const;
    CurveSampler &curveSampler() const;
    CurveHitTester &curveHitTester() const;
    SnapEngine &snapEngine() const;

    void setShapeFactory(ShapeFactory factory);
    void setShapeCommitter(ShapeCommitter committer);
    void setToolFinisher(ToolFinisher finisher);
    void setPreviewPublisher(PreviewPublisher publisher);
    void setStatusPublisher(StatusPublisher publisher);
    void setPointConstraint(PointConstraint constraint);
    void setArcModeProvider(ArcModeProvider provider);
    void setArcSweepProvider(ArcSweepProvider provider);

    bool createShape(ToolId tool,
                     const QVector<QPointF> &points,
                     ArcMode arcMode,
                     qreal arcSweep,
                     Shape *shape) const;
    bool commitShape(ToolId tool, const Shape &shape) const;
    void finishTool(ToolId tool) const;
    void publishPreview(const ToolPreview &preview) const;
    void publishStatus(const ToolStatus &status) const;
    QPointF constrainPoint(ToolId tool,
                           const QPointF &rawPoint,
                           const QVector<QPointF> &points) const;
    ArcMode arcMode() const;
    qreal arcSweep() const;

private:
    Document &document_;
    SelectionModel &selection_;
    History &history_;
    ViewportTransform &viewportTransform_;
    CurveSampler &curveSampler_;
    CurveHitTester &curveHitTester_;
    SnapEngine &snapEngine_;
    ShapeFactory shapeFactory_;
    ShapeCommitter shapeCommitter_;
    ToolFinisher toolFinisher_;
    PreviewPublisher previewPublisher_;
    StatusPublisher statusPublisher_;
    PointConstraint pointConstraint_;
    ArcModeProvider arcModeProvider_;
    ArcSweepProvider arcSweepProvider_;
};

} // namespace classiCAD
