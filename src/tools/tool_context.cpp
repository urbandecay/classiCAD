#include "tool_context.h"

namespace classiCAD {

ToolContext::ToolContext(Document &document,
                         SelectionModel &selection,
                         History &history,
                         ViewportTransform &viewportTransform,
                         CurveSampler &curveSampler,
                         CurveHitTester &curveHitTester,
                         SnapEngine &snapEngine)
    : document_(document)
    , selection_(selection)
    , history_(history)
    , viewportTransform_(viewportTransform)
    , curveSampler_(curveSampler)
    , curveHitTester_(curveHitTester)
    , snapEngine_(snapEngine)
{
}

Document &ToolContext::document() const
{
    return document_;
}

SelectionModel &ToolContext::selection() const
{
    return selection_;
}

History &ToolContext::history() const
{
    return history_;
}

ViewportTransform &ToolContext::viewportTransform() const
{
    return viewportTransform_;
}

CurveSampler &ToolContext::curveSampler() const
{
    return curveSampler_;
}

CurveHitTester &ToolContext::curveHitTester() const
{
    return curveHitTester_;
}

SnapEngine &ToolContext::snapEngine() const
{
    return snapEngine_;
}

void ToolContext::setShapeFactory(ShapeFactory factory)
{
    shapeFactory_ = std::move(factory);
}

void ToolContext::setShapeCommitter(ShapeCommitter committer)
{
    shapeCommitter_ = std::move(committer);
}

void ToolContext::setToolFinisher(ToolFinisher finisher)
{
    toolFinisher_ = std::move(finisher);
}

void ToolContext::setPreviewPublisher(PreviewPublisher publisher)
{
    previewPublisher_ = std::move(publisher);
}

void ToolContext::setStatusPublisher(StatusPublisher publisher)
{
    statusPublisher_ = std::move(publisher);
}

void ToolContext::setPointConstraint(PointConstraint constraint)
{
    pointConstraint_ = std::move(constraint);
}

void ToolContext::setArcModeProvider(ArcModeProvider provider)
{
    arcModeProvider_ = std::move(provider);
}

void ToolContext::setArcSweepProvider(ArcSweepProvider provider)
{
    arcSweepProvider_ = std::move(provider);
}

bool ToolContext::createShape(ToolId tool,
                              const QVector<QPointF> &points,
                              ArcMode arcMode,
                              qreal arcSweep,
                              Shape *shape) const
{
    return shapeFactory_ && shapeFactory_(tool, points, arcMode, arcSweep, shape);
}

bool ToolContext::commitShape(ToolId tool, const Shape &shape) const
{
    return shapeCommitter_ && shapeCommitter_(tool, shape);
}

void ToolContext::finishTool(ToolId tool) const
{
    if (toolFinisher_) {
        toolFinisher_(tool);
    }
}

void ToolContext::publishPreview(const ToolPreview &preview) const
{
    if (previewPublisher_) {
        previewPublisher_(preview);
    }
}

void ToolContext::publishStatus(const ToolStatus &status) const
{
    if (statusPublisher_) {
        statusPublisher_(status);
    }
}

QPointF ToolContext::constrainPoint(ToolId tool,
                                    const QPointF &rawPoint,
                                    const QVector<QPointF> &points) const
{
    return pointConstraint_ ? pointConstraint_(tool, rawPoint, points) : rawPoint;
}

ArcMode ToolContext::arcMode() const
{
    return arcModeProvider_ ? arcModeProvider_() : ArcMode::TwoPoint;
}

qreal ToolContext::arcSweep() const
{
    return arcSweepProvider_ ? arcSweepProvider_() : 0.0;
}

} // namespace classiCAD
