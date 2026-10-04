#include "tool_context.h"

#include "core/document/document.h"
#include "core/document/selection_model.h"
#include "core/history/history.h"
#include "services/hit_testing/curve_hit_tester.h"
#include "services/sampling/curve_sampler.h"
#include "services/snapping/snap_engine.h"
#include "services/viewport/viewport_transform.h"

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

void ToolContext::setTransactionCommitter(TransactionCommitter committer)
{
    transactionCommitter_ = std::move(committer);
}

void ToolContext::setLayersChangedNotifier(ChangeNotifier notifier)
{
    layersChangedNotifier_ = std::move(notifier);
}

void ToolContext::setSelectionChangedNotifier(ChangeNotifier notifier)
{
    selectionChangedNotifier_ = std::move(notifier);
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

void ToolContext::setPolygonSideCountCallbacks(PolygonSideCountProvider provider,
                                                PolygonSideCountSetter setter)
{
    polygonSideCountProvider_ = std::move(provider);
    polygonSideCountSetter_ = std::move(setter);
}

void ToolContext::setSelectionInteractionCallbacks(
    SelectionHitTest hitTest,
    SelectionGestureHandler gestureHandler)
{
    selectionHitTest_ = std::move(hitTest);
    selectionGestureHandler_ = std::move(gestureHandler);
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

void ToolContext::setShapesCommitter(ShapesCommitter committer)
{
    shapesCommitter_ = std::move(committer);
}

bool ToolContext::commitShapes(ToolId tool, const QVector<Shape> &shapes) const
{
    if (shapesCommitter_) {
        return shapesCommitter_(tool, shapes);
    }
    for (const Shape &shape : shapes) {
        if (!commitShape(tool, shape)) {
            return false;
        }
    }
    return !shapes.isEmpty();
}

void ToolContext::finishTool(ToolId tool) const
{
    if (toolFinisher_) {
        toolFinisher_(tool);
    }
}

DocumentTransaction ToolContext::beginTransaction() const
{
    return DocumentTransaction(document_, history_);
}

bool ToolContext::commitTransaction(DocumentTransaction &transaction) const
{
    return transactionCommitter_ ? transactionCommitter_(transaction)
                                 : transaction.commit();
}

void ToolContext::notifyLayersChanged() const
{
    if (layersChangedNotifier_) {
        layersChangedNotifier_();
    }
}

void ToolContext::notifySelectionChanged() const
{
    if (selectionChangedNotifier_) {
        selectionChangedNotifier_();
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

int ToolContext::polygonSideCount() const
{
    return polygonSideCountProvider_ ? polygonSideCountProvider_() : 32;
}

void ToolContext::setPolygonSideCount(int sideCount) const
{
    if (polygonSideCountSetter_) {
        polygonSideCountSetter_(sideCount);
    }
}

SelectionHit ToolContext::hitTestSelection(const QPointF &screenPosition,
                                           bool includeControlPoint) const
{
    return selectionHitTest_ ? selectionHitTest_(screenPosition,
                                                 includeControlPoint)
                             : SelectionHit{};
}

void ToolContext::beginSelectionGesture(SelectionGestureKind gesture,
                                        const ToolInput &input,
                                        const SelectionHit &hit,
                                        bool additive) const
{
    if (selectionGestureHandler_) {
        selectionGestureHandler_(gesture, input, hit, additive);
    }
}

} // namespace classiCAD
