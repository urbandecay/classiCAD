#pragma once

#include "tool.h"
#include "core/document/object_id.h"
#include "core/history/document_transaction.h"

#include <functional>

namespace classiCAD {

class Document;
class SelectionModel;
class History;
class ViewportTransform;
class CurveSampler;
class CurveHitTester;
class SnapEngine;

struct SelectionHit {
    ObjectId objectId = ObjectId::invalid();
    int controlPointIndex = -1;

    bool isControlPoint() const
    {
        return objectId.isValid() && controlPointIndex >= 0;
    }
};

enum class SelectionGestureKind {
    BeginObjectDrag,
    BeginControlPointDrag,
    BeginSelectionBox,
};

class ToolContext {
public:
    using ShapeFactory = std::function<bool(ToolId,
                                            const QVector<QPointF> &,
                                            ArcMode,
                                            qreal,
                                            Shape *)>;
    using ShapeCommitter = std::function<bool(ToolId, const Shape &)>;
    using ShapesCommitter = std::function<bool(ToolId, const QVector<Shape> &)>;
    using ToolFinisher = std::function<void(ToolId)>;
    using TransactionCommitter = std::function<bool(DocumentTransaction &)>;
    using ChangeNotifier = std::function<void()>;
    using PreviewPublisher = std::function<void(const ToolPreview &)>;
    using StatusPublisher = std::function<void(const ToolStatus &)>;
    using PointConstraint = std::function<QPointF(ToolId,
                                                  const QPointF &,
                                                  const QVector<QPointF> &)>;
    using ArcModeProvider = std::function<ArcMode()>;
    using ArcSweepProvider = std::function<qreal()>;
    using PolygonSideCountProvider = std::function<int()>;
    using PolygonSideCountSetter = std::function<void(int)>;
    using SelectionHitTest = std::function<SelectionHit(const QPointF &, bool)>;
    using SelectionGestureHandler = std::function<void(
        SelectionGestureKind,
        const ToolInput &,
        const SelectionHit &,
        bool)>;

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
    void setShapesCommitter(ShapesCommitter committer);
    void setToolFinisher(ToolFinisher finisher);
    void setTransactionCommitter(TransactionCommitter committer);
    void setLayersChangedNotifier(ChangeNotifier notifier);
    void setSelectionChangedNotifier(ChangeNotifier notifier);
    void setPreviewPublisher(PreviewPublisher publisher);
    void setStatusPublisher(StatusPublisher publisher);
    void setPointConstraint(PointConstraint constraint);
    void setArcModeProvider(ArcModeProvider provider);
    void setArcSweepProvider(ArcSweepProvider provider);
    void setPolygonSideCountCallbacks(PolygonSideCountProvider provider,
                                     PolygonSideCountSetter setter);
    void setSelectionInteractionCallbacks(SelectionHitTest hitTest,
                                          SelectionGestureHandler gestureHandler);

    bool createShape(ToolId tool,
                     const QVector<QPointF> &points,
                     ArcMode arcMode,
                     qreal arcSweep,
                     Shape *shape) const;
    bool commitShape(ToolId tool, const Shape &shape) const;
    bool commitShapes(ToolId tool, const QVector<Shape> &shapes) const;
    void finishTool(ToolId tool) const;
    DocumentTransaction beginTransaction() const;
    bool commitTransaction(DocumentTransaction &transaction) const;
    void notifyLayersChanged() const;
    void notifySelectionChanged() const;
    void publishPreview(const ToolPreview &preview) const;
    void publishStatus(const ToolStatus &status) const;
    QPointF constrainPoint(ToolId tool,
                           const QPointF &rawPoint,
                           const QVector<QPointF> &points) const;
    ArcMode arcMode() const;
    qreal arcSweep() const;
    int polygonSideCount() const;
    void setPolygonSideCount(int sideCount) const;
    SelectionHit hitTestSelection(const QPointF &screenPosition,
                                  bool includeControlPoint) const;
    void beginSelectionGesture(SelectionGestureKind gesture,
                               const ToolInput &input,
                               const SelectionHit &hit,
                               bool additive) const;

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
    ShapesCommitter shapesCommitter_;
    ToolFinisher toolFinisher_;
    TransactionCommitter transactionCommitter_;
    ChangeNotifier layersChangedNotifier_;
    ChangeNotifier selectionChangedNotifier_;
    PreviewPublisher previewPublisher_;
    StatusPublisher statusPublisher_;
    PointConstraint pointConstraint_;
    ArcModeProvider arcModeProvider_;
    ArcSweepProvider arcSweepProvider_;
    PolygonSideCountProvider polygonSideCountProvider_;
    PolygonSideCountSetter polygonSideCountSetter_;
    SelectionHitTest selectionHitTest_;
    SelectionGestureHandler selectionGestureHandler_;
};

} // namespace classiCAD
