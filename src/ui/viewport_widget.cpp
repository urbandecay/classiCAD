#include "viewport_widget_api.h"
#include "../core/document/document.h"
#include "../core/document/selection_model.h"
#include "../core/debug_log.h"
#include "../core/geometry/circle_construction.h"
#include "../core/geometry/curve_evaluator.h"
#include "../core/geometry/geometry_transform.h"
#include "../core/history/history.h"
#include "../core/serialization/document_serializer.h"
#include "../core/serialization/vignola_document_file.h"
#include "../services/hit_testing/curve_hit_tester.h"
#include "../services/dimensions/dimension_association.h"
#include "../services/sampling/curve_sampler.h"
#include "../services/snapping/snap_engine.h"
#include "../services/viewport/viewport_transform.h"
#include "../tools/tool_context.h"
#include "../tools/tool_input.h"
#include "../tools/tool_registry.h"
#include "input_helpers.h"
#include "viewport/blender_grid_renderer.h"
#include "viewport/viewport_gpu_surface.h"
#include "viewport/line_type_style.h"
#include "viewport/viewport_scene_renderer.h"
#include "viewport/viewport_overlay.h"
#include "viewport/viewport_renderer.h"

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QCursor>
#include <QDateTime>
#include <QDebug>
#include <QEasingCurve>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineF>
#include <QImageReader>
#include <QKeyEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QPixmap>
#include <QRadialGradient>
#include <QSettings>
#include <QTextStream>
#include <QTimer>
#include <QToolTip>
#include <QVariantAnimation>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <functional>

namespace classiCAD {

namespace {

void fillViewportBackground(QPainter &painter, const QRect &bounds)
{
    if (bounds.isEmpty()) {
        return;
    }
    const QPointF center(bounds.x() + bounds.width() * 0.5,
                         bounds.y() + bounds.height() * 0.5);
    const qreal radius = 0.5 * std::hypot(bounds.width(), bounds.height());
    QRadialGradient gradient(center, std::max<qreal>(radius, 1.0), center);
    constexpr qreal gamma = 2.2;
    constexpr qreal high = 61.0 / 255.0;
    constexpr qreal low = 48.0 / 255.0;
    constexpr int stopCount = 32;
    for (int stopIndex = 0; stopIndex <= stopCount; ++stopIndex) {
        const qreal factor = static_cast<qreal>(stopIndex) / stopCount;
        const qreal gammaColor = std::pow(
            std::pow(high, 1.0 / gamma) * (1.0 - factor) +
                std::pow(low, 1.0 / gamma) * factor,
            gamma);
        const QColor color = QColor::fromRgbF(gammaColor,
                                              gammaColor,
                                              gammaColor);
        gradient.setColorAt(factor, color);
    }
    painter.fillRect(bounds, gradient);
}

} // namespace

enum class DragAxisLock {
    None,
    X,
    Y,
};

QString dragAxisLockName(DragAxisLock lock)
{
    switch (lock) {
    case DragAxisLock::X:
        return QStringLiteral("X");
    case DragAxisLock::Y:
        return QStringLiteral("Y");
    case DragAxisLock::None:
        return QStringLiteral("None");
    }

    return QStringLiteral("None");
}

QString precisePointText(const QPointF &point)
{
    return QStringLiteral("(%1, %2)")
        .arg(point.x(), 0, 'g', 12)
        .arg(point.y(), 0, 'g', 12);
}

class ViewportWidget final : public ViewportWidgetApi {
public:
    explicit ViewportWidget(QWidget *parent = nullptr)
        : ViewportWidgetApi(parent)
        , history_(document_)
        , viewportRenderer_(viewportTransform_, curveHitTester_)
        , viewportOverlay_(viewportRenderer_, viewportTransform_)
        , toolContext_(document_,
                       selection_,
                       history_,
                       viewportTransform_,
                       curveSampler_,
                       curveHitTester_,
                       snapEngine_)
        , toolRegistry_()
        , shapes_(document_)
        , pan_(viewportTransform_.pan())
        , selectedShapeIndices_(selection_.objectIds())
        , selectedShapeIndex_(selection_.primaryObjectId())
        , controlPointIndex_(selection_.activeControlPointIndex())
        , zoom_(viewportTransform_.zoom())
    {
        toolContext_.setShapeFactory(
            [this](ToolId tool,
                   const QVector<QPointF> &points,
                   ArcMode arcMode,
                   qreal arcSweep,
                   Shape *shape) {
                return makeToolShape(tool, points, arcMode, arcSweep, shape);
        });
        toolContext_.setShapeCommitter([this](ToolId tool, const Shape &shape) {
            recordGeometryChange();
            Shape committedShape = shape;
            committedShape.workPlane = viewportTransform_.workPlane();
            committedShape.workPlaneOffset = viewportTransform_.workPlaneOffset();
            shapes_.append(committedShape);
            DebugLog::instance().write(QStringLiteral("tool commit tool=%1 shapes=%2")
                                           .arg(toolName(tool))
                                           .arg(shapes_.size()));
            return true;
        });
        toolContext_.setToolFinisher([this](ToolId tool) {
            setTool(tool);
            if (commandFinished_) {
                commandFinished_(tool);
            }
        });
        toolContext_.setPreviewPublisher([this](const ToolPreview &preview) {
            // Rendering remains in the viewport for now, but the pending
            // points themselves are owned by the active tool.
            pendingPoints_ = preview.points;
            controllerPreviewShape_ = preview.shape;
            controllerPreviewShapeVisible_ = preview.hasShape;
            update();
        });
        toolContext_.setStatusPublisher([this](const ToolStatus &status) {
            toolStatus_ = status;
            if (activeTool_ == Tool::CircleTangentThree && toolStatusUpdate_) {
                toolStatusUpdate_(status.text);
            }
        });
        toolContext_.setPointConstraint(
            [this](ToolId, const QPointF &rawPoint, const QVector<QPointF> &) {
                return constrainLinePoint(rawPoint);
            });
        toolContext_.setArcModeProvider([this] {
            return arcMode_;
        });
        toolContext_.setArcSweepProvider([this] {
            return arcPreviewSweepAngle_;
        });
        setMinimumSize(480, 320);
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        qApp->installEventFilter(this);
        setCursor(Qt::CrossCursor);
        navigationAnimation_ = new QVariantAnimation(this);
        navigationAnimation_->setDuration(200);
        navigationAnimation_->setStartValue(0.0);
        navigationAnimation_->setEndValue(1.0);
        navigationAnimation_->setEasingCurve(QEasingCurve::Linear);
        connect(navigationAnimation_, &QVariantAnimation::valueChanged,
                this, [this](const QVariant &value) {
                    const qreal time = value.toReal();
                    // Blender's smooth view uses 3*t*t - 2*t*t*t.
                    const qreal progress = time * time * (3.0 - 2.0 * time);
                    ViewportCameraState state;
                    const qreal startDistance = 1.0 / navigationAnimationStart_.zoom;
                    const qreal endDistance = 1.0 / navigationAnimationEnd_.zoom;
                    state.zoom = 1.0 / (startDistance * (1.0 - progress) +
                                        endDistance * progress);
                    state.gridViewDistance =
                        navigationAnimationStart_.gridViewDistance * (1.0 - progress) +
                        navigationAnimationEnd_.gridViewDistance * progress;
                    state.pan = navigationAnimationStart_.pan * (1.0 - progress) +
                                navigationAnimationEnd_.pan * progress;
                    state.orbitPivot = {
                        navigationAnimationStart_.orbitPivot.x * (1.0 - progress) +
                            navigationAnimationEnd_.orbitPivot.x * progress,
                        navigationAnimationStart_.orbitPivot.y * (1.0 - progress) +
                            navigationAnimationEnd_.orbitPivot.y * progress,
                        navigationAnimationStart_.orbitPivot.z * (1.0 - progress) +
                            navigationAnimationEnd_.orbitPivot.z * progress};
                    state.orientation = ViewportOrientation::slerp(
                        navigationAnimationStart_.orientation,
                        navigationAnimationEnd_.orientation,
                        progress);
                    state.hasOrientation = true;
                    state.perspective = navigationAnimationStart_.perspective;
                    state.preset = progress >= 1.0
                                       ? navigationAnimationEnd_.preset
                                       : ViewportViewPreset::Custom;
                    viewportTransform_.setCameraState(state);
                    update();
                    emitCoordinateUpdate();
                });
        connect(navigationAnimation_, &QVariantAnimation::finished,
                this, [this]() {
                    viewportTransform_.setCameraState(navigationAnimationEnd_);
                    notifyViewStateChanged();
                    update();
                    emitCoordinateUpdate();
                });
        if (ViewportGpuSurface::isSupported()) {
            gpuSurface_ = new ViewportGpuSurface(this);
            gpuSurface_->setGeometry(rect());
            gpuSurface_->setAntiAliasingSamples(
                blenderGridRenderer_.antiAliasingSamples());
            gpuSurface_->setDrawCallback(
                [this](QPainter &painter, BlenderGridRenderer &gridRenderer,
                       ViewportSceneRenderer &sceneRenderer) {
                    paintViewport(painter, &gridRenderer, &sceneRenderer);
                });
            gpuSurface_->show();
        }
        DebugLog::instance().write(QStringLiteral("viewport constructed"));
    }

    ~ViewportWidget() override
    {
        qApp->removeEventFilter(this);
    }

    // Navigation updates the GPU surface directly, so its frame uses the
    // camera state from this event instead of waiting for a parent repaint.
    void update()
    {
        if (gpuSurface_ != nullptr) {
            gpuSurface_->update();
        } else {
            QWidget::update();
        }
    }

    void setTool(ToolId tool)
    {
        DebugLog::instance().write(QStringLiteral("setTool requested=%1 previous=%2")
                                       .arg(toolName(tool), toolName(activeTool_)));
        if (duplicateActive_) {
            cancelDuplicate();
        }
        if (grabActive_ && tool != Tool::Select) {
            cancelGrab();
        }
        const Tool previousTool = activeTool_;
        if (previousTool == Tool::Picture && tool != Tool::Picture) {
            pendingPictureImage_ = QImage();
            pendingPictureImageData_.clear();
            pendingPicturePath_.clear();
            repeatTool_ = Tool::Select;
        }
        if (previousTool != tool) {
            polygonWheelRemainder_ = 0;
        }
        if (isPolygonTool(tool) && polygonModeForTool(tool) == PolygonMode::Edge &&
            polygonSideCount_ % 2 == 0) {
            --polygonSideCount_;
        }
        if (!isPolygonTool(tool)) {
            polygonWheelRemainder_ = 0;
        }
        if (activeToolController_ != nullptr && activeToolController_->id() != tool) {
            activeToolController_->cancel(toolContext_);
        }
        if (subdivisionActive_ && tool != Tool::Select) {
            cancelSubdivisionPreview();
        }
        if (joinActive_ && tool != Tool::Select) {
            cancelJoinMode();
        }
        if (tool != Tool::Rotate) {
            resetRotateInteraction();
        }
        if (tool != Tool::Scale) {
            resetScaleInteraction();
        }
        if (tool != Tool::Mirror) {
            resetMirrorInteraction();
        }
        activeTool_ = tool;
        activeToolController_ = toolRegistry_.find(tool);
        pendingPoints_.clear();
        controllerPreviewShape_ = Shape{};
        controllerPreviewShapeVisible_ = false;
        resetArcPreviewTracking();
        lineCommandActive_ = tool == Tool::Line;
        if (!isEraseLikeTool(tool) || previousTool != tool) {
            eraseStrokeActive_ = false;
            eraseCursorPressed_ = false;
            eraseCandidateShapeIndices_.clear();
            eraseStrokeScreenPath_.clear();
            eraseTargetShapeIndices_.clear();
            eraseSceneCurveCaches_.clear();
            eraseTargetCurveCaches_.clear();
            eraseGeometryCachePrepared_ = false;
            trimHoverPositionValid_ = false;
            trimHoverComponentIndex_ = -1;
        }

        if (tool != Tool::Select) {
            repeatTool_ = tool;
            if (!isEraseLikeTool(tool) && tool != Tool::Rotate &&
                tool != Tool::Scale && tool != Tool::Mirror) {
                selectedShapeIndices_.clear();
                selectedShapeIndex_ = ObjectId::invalid();
            }
            selectionBoxActive_ = false;
            selectionBoxMoved_ = false;
            selectionBoxAdditive_ = false;
            trimBoxSelectionActive_ = false;
            draggingSelected_ = false;
            dragGestureStarted_ = false;
            draggingShapeIndices_.clear();
            draggingControlPoint_ = false;
            controlPointIndex_ = -1;
            dragHistoryRecorded_ = false;
            currentDragSnap_ = DragSnapResult{};
            dragSnapLocked_ = false;
            dragAxisLock_ = DragAxisLock::None;
        }

        if (lineCommandActive_) {
            const QPoint localCursor = mapFromGlobal(QCursor::pos());
            if (rect().contains(localCursor)) {
                rawCursorWorld_ = screenToWorld(localCursor);
                cursorValid_ = true;
            }
        }

        if (activeToolController_ != nullptr) {
            activeToolController_->begin(toolContext_);
        }

        refreshCursorConstraint();

        if (tool == Tool::Select) {
            setCursor(Qt::ArrowCursor);
        } else {
            setCursor(Qt::CrossCursor);
        }
        if (isCircleTangentTool(tool)) {
            setFocus(Qt::OtherFocusReason);
        }

        DebugLog::instance().write(QStringLiteral("setTool applied=%1 lineCommandActive=%2 cursorValid=%3 cursorWorld=%4")
                                       .arg(toolName(activeTool_))
                                       .arg(lineCommandActive_)
                                       .arg(cursorValid_)
                                       .arg(pointText(cursorWorld_)));
        update();
    }

    bool beginPicturePlacement(const QString &imagePath,
                               QString *errorMessage) override
    {
        QFile imageFile(imagePath);
        if (!imageFile.open(QIODevice::ReadOnly)) {
            if (errorMessage != nullptr) {
                *errorMessage = imageFile.errorString();
            }
            return false;
        }
        QByteArray imageData = imageFile.readAll();
        if (imageData.isEmpty() || imageFile.error() != QFileDevice::NoError) {
            if (errorMessage != nullptr) {
                *errorMessage = imageFile.errorString().isEmpty()
                                    ? QStringLiteral("The selected image file is empty.")
                                    : imageFile.errorString();
            }
            return false;
        }
        QBuffer imageBuffer;
        imageBuffer.setData(imageData);
        imageBuffer.open(QIODevice::ReadOnly);
        QImageReader reader(&imageBuffer);
        reader.setAutoTransform(true);
        QImage image = reader.read();
        if (image.isNull()) {
            if (errorMessage != nullptr) {
                *errorMessage = reader.errorString().isEmpty()
                                    ? QStringLiteral("The selected file is not a readable image.")
                                    : reader.errorString();
            }
            return false;
        }

        pendingPictureImage_ = std::move(image);
        pendingPictureImageData_ = std::move(imageData);
        pendingPicturePath_ = imagePath;
        setTool(Tool::Picture);
        setFocus(Qt::OtherFocusReason);
        return true;
    }

    ViewportCommandResult executeCommand(ViewportCommand command,
                                         int argument = 0) override
    {
        switch (command) {
        case ViewportCommand::Undo: {
            if (duplicateActive_) {
                cancelDuplicate();
            }
            const bool accepted = history_.canUndo();
            undo();
            return {accepted, 0};
        }
        case ViewportCommand::Redo: {
            if (duplicateActive_) {
                cancelDuplicate();
            }
            const bool accepted = history_.canRedo();
            redo();
            return {accepted, 0};
        }
        case ViewportCommand::BeginSubdivision:
            return {beginSubdivisionWheelMode(), 0};
        case ViewportCommand::CancelSubdivision:
            cancelSubdivisionWheelMode();
            return {true, 0};
        case ViewportCommand::ApplySubdivision:
            return {applySubdivision(argument), 0};
        case ViewportCommand::BeginJoin:
            return {beginJoinMode(), 0};
        case ViewportCommand::Explode: {
            const int explodedComponents = explodeSelectedShapes();
            return {explodedComponents > 0, explodedComponents};
        }
        case ViewportCommand::BeginRotate:
            return {beginRotate(), 0};
        case ViewportCommand::BeginScale:
            if (argument < static_cast<int>(ScaleMode::OneD) ||
                argument > static_cast<int>(ScaleMode::TwoD)) {
                return {};
            }
            return {beginScale(static_cast<ScaleMode>(argument)), 0};
        case ViewportCommand::BeginMirror:
            return {beginMirror(), 0};
        case ViewportCommand::BeginDuplicate:
            return {beginDuplicate(), 0};
        case ViewportCommand::DuplicateInPlace:
            return {duplicateInPlace(), 0};
        }

        return {};
    }

    QVector<ViewportLayerInfo> layerInfos() const override
    {
        QVector<ViewportLayerInfo> infos;
        infos.reserve(document_.layers().size());
        for (const Layer &layer : document_.layers()) {
            ViewportLayerInfo info;
            info.id = layer.id;
            info.name = layer.name;
            info.visible = layer.visible;
            info.locked = layer.locked;
            info.active = layer.id == document_.activeLayerId();
            info.objectCount = static_cast<int>(layer.objectIds.size());
            info.color = layer.color;
            info.lineType = layer.lineType;
            info.lineWeightMm = layer.lineWeightMm;
            info.description = layer.description;
            info.frozen = layer.frozen;
            info.plotted = layer.plotted;
            infos.append(info);
        }
        return infos;
    }

    ViewportLayerCommandResult executeLayerCommand(
        const ViewportLayerCommandRequest &request) override
    {
        const auto hasEditableAlternative = [this](LayerId excludedLayer) {
            for (const Layer &layer : document_.layers()) {
                if (layer.id != excludedLayer && layer.visible && !layer.frozen &&
                    !layer.locked) {
                    return true;
                }
            }
            return false;
        };

        switch (request.command) {
        case ViewportLayerCommand::Create: {
            recordLayerChange();
            const LayerId layerId = document_.createLayer(request.name);
            if (!document_.setActiveLayer(layerId)) {
                return {};
            }
            notifyLayersChanged();
            update();
            return {true, layerId, 0};
        }
        case ViewportLayerCommand::Remove: {
            const Layer *layer = document_.layer(request.layerId);
            if (layer == nullptr || document_.layers().size() <= 1 ||
                !layer->objectIds.isEmpty() ||
                (document_.activeLayerId() == request.layerId &&
                 !hasEditableAlternative(request.layerId))) {
                return {};
            }
            recordLayerChange();
            if (!document_.removeLayer(request.layerId)) {
                return {};
            }
            pruneSelectionToEditableLayers();
            notifyLayersChanged();
            update();
            return {true, document_.activeLayerId(), 0};
        }
        case ViewportLayerCommand::Activate: {
            if (document_.activeLayerId() == request.layerId) {
                return {true, request.layerId, 0};
            }
            if (!document_.isLayerEditable(request.layerId)) {
                return {};
            }
            recordLayerChange();
            if (!document_.setActiveLayer(request.layerId)) {
                return {};
            }
            notifyLayersChanged();
            update();
            return {true, request.layerId, 0};
        }
        case ViewportLayerCommand::SetVisible: {
            const Layer *layer = document_.layer(request.layerId);
            if (layer == nullptr || layer->visible == request.enabled) {
                return layer == nullptr ? ViewportLayerCommandResult{}
                                        : ViewportLayerCommandResult{true, request.layerId, 0};
            }
            if (!request.enabled && document_.activeLayerId() == request.layerId &&
                !hasEditableAlternative(request.layerId)) {
                return {};
            }
            recordLayerChange();
            if (!document_.setLayerVisible(request.layerId, request.enabled)) {
                return {};
            }
            pruneSelectionToEditableLayers();
            notifyLayersChanged();
            update();
            return {true, request.layerId, 0};
        }
        case ViewportLayerCommand::SetLocked: {
            const Layer *layer = document_.layer(request.layerId);
            if (layer == nullptr || layer->locked == request.enabled) {
                return layer == nullptr ? ViewportLayerCommandResult{}
                                        : ViewportLayerCommandResult{true, request.layerId, 0};
            }
            if (request.enabled && document_.activeLayerId() == request.layerId &&
                !hasEditableAlternative(request.layerId)) {
                return {};
            }
            recordLayerChange();
            if (!document_.setLayerLocked(request.layerId, request.enabled)) {
                return {};
            }
            pruneSelectionToEditableLayers();
            notifyLayersChanged();
            update();
            return {true, request.layerId, 0};
        }
        case ViewportLayerCommand::SetFrozen: {
            const Layer *layer = document_.layer(request.layerId);
            if (layer == nullptr || layer->frozen == request.enabled) {
                return layer == nullptr ? ViewportLayerCommandResult{}
                                        : ViewportLayerCommandResult{true, request.layerId, 0};
            }
            if (request.enabled && document_.activeLayerId() == request.layerId &&
                !hasEditableAlternative(request.layerId)) {
                return {};
            }
            recordLayerChange();
            if (!document_.setLayerFrozen(request.layerId, request.enabled)) {
                return {};
            }
            pruneSelectionToEditableLayers();
            notifyLayersChanged();
            update();
            return {true, request.layerId, 0};
        }
        case ViewportLayerCommand::SetColor: {
            const Layer *layer = document_.layer(request.layerId);
            if (layer == nullptr || !request.color.isValid()) {
                return {};
            }
            if (layer->color == request.color) {
                return {true, request.layerId, 0};
            }
            recordLayerChange();
            if (!document_.setLayerColor(request.layerId, request.color)) {
                return {};
            }
            notifyLayersChanged();
            update();
            return {true, request.layerId, 0};
        }
        case ViewportLayerCommand::SetLineType: {
            const Layer *layer = document_.layer(request.layerId);
            const QString lineType = request.name.trimmed();
            if (layer == nullptr || lineType.isEmpty()) {
                return {};
            }
            if (layer->lineType == lineType) {
                return {true, request.layerId, 0};
            }
            recordLayerChange();
            if (!document_.setLayerLineType(request.layerId, lineType)) {
                return {};
            }
            notifyLayersChanged();
            update();
            return {true, request.layerId, 0};
        }
        case ViewportLayerCommand::SetLineWeight: {
            const Layer *layer = document_.layer(request.layerId);
            if (layer == nullptr || layer->lineWeightMm == request.lineWeightMm) {
                return layer == nullptr ? ViewportLayerCommandResult{}
                                        : ViewportLayerCommandResult{true, request.layerId, 0};
            }
            recordLayerChange();
            if (!document_.setLayerLineWeight(request.layerId, request.lineWeightMm)) {
                return {};
            }
            notifyLayersChanged();
            update();
            return {true, request.layerId, 0};
        }
        case ViewportLayerCommand::SetPlotted: {
            const Layer *layer = document_.layer(request.layerId);
            if (layer == nullptr || layer->plotted == request.enabled) {
                return layer == nullptr ? ViewportLayerCommandResult{}
                                        : ViewportLayerCommandResult{true, request.layerId, 0};
            }
            recordLayerChange();
            if (!document_.setLayerPlotted(request.layerId, request.enabled)) {
                return {};
            }
            notifyLayersChanged();
            update();
            return {true, request.layerId, 0};
        }
        case ViewportLayerCommand::SetDescription: {
            const Layer *layer = document_.layer(request.layerId);
            if (layer == nullptr) {
                return {};
            }
            if (layer->description == request.name) {
                return {true, request.layerId, 0};
            }
            recordLayerChange();
            if (!document_.setLayerDescription(request.layerId, request.name)) {
                return {};
            }
            notifyLayersChanged();
            return {true, request.layerId, 0};
        }
        case ViewportLayerCommand::Rename: {
            const Layer *layer = document_.layer(request.layerId);
            const QString trimmedName = request.name.trimmed();
            if (layer == nullptr || trimmedName.isEmpty()) {
                return {};
            }
            if (layer->name == trimmedName) {
                return {true, request.layerId, 0};
            }
            recordLayerChange();
            if (!document_.renameLayer(request.layerId, trimmedName)) {
                return {};
            }
            notifyLayersChanged();
            update();
            return {true, request.layerId, 0};
        }
        case ViewportLayerCommand::Move: {
            if (document_.layer(request.layerId) == nullptr || request.index < 0 ||
                request.index >= document_.layers().size()) {
                return {};
            }
            recordLayerChange();
            if (!document_.moveLayer(request.layerId, request.index)) {
                return {};
            }
            notifyLayersChanged();
            update();
            return {true, request.layerId, 0};
        }
        case ViewportLayerCommand::MoveSelectedObjects: {
            if (!document_.isLayerEditable(request.layerId)) {
                return {};
            }

            QVector<ObjectId> movableObjects;
            for (const ObjectId objectId : selectedShapeIndices_) {
                if (document_.isObjectEditable(objectId) &&
                    document_.object(objectId)->layerId != request.layerId) {
                    movableObjects.append(objectId);
                }
            }
            if (movableObjects.isEmpty()) {
                return {};
            }

            recordLayerChange();
            int movedCount = 0;
            for (const ObjectId objectId : movableObjects) {
                if (document_.moveObjectToLayer(objectId, request.layerId)) {
                    ++movedCount;
                }
            }
            if (movedCount == 0) {
                return {};
            }
            notifyLayersChanged();
            update();
            return {true, request.layerId, movedCount};
        }
        }

        return {};
    }

    bool canUndo() const
    {
        return history_.canUndo();
    }

    bool canRedo() const
    {
        return history_.canRedo();
    }

    void undo()
    {
        if (!history_.canUndo()) {
            DebugLog::instance().write(QStringLiteral("undo ignored empty-history"));
            return;
        }

        history_.undo();
        resetInteractionAfterHistory();
        notifyHistoryChanged();
        notifyLayersChanged();
        DebugLog::instance().write(QStringLiteral("undo applied shapes=%1 undoRemaining=%2 redoAvailable=%3")
                                       .arg(shapes_.size())
                                       .arg(history_.undoCount())
                                       .arg(history_.redoCount()));
    }

    void redo()
    {
        if (!history_.canRedo()) {
            DebugLog::instance().write(QStringLiteral("redo ignored empty-history"));
            return;
        }

        history_.redo();
        resetInteractionAfterHistory();
        notifyHistoryChanged();
        notifyLayersChanged();
        DebugLog::instance().write(QStringLiteral("redo applied shapes=%1 undoAvailable=%2 redoRemaining=%3")
                                       .arg(shapes_.size())
                                       .arg(history_.undoCount())
                                       .arg(history_.redoCount()));
    }

    void repeatLastTool()
    {
        if (repeatTool_ == Tool::Select) {
            DebugLog::instance().write(QStringLiteral("repeatTool ignored no-last-tool"));
            return;
        }

        const ToolId tool = repeatTool_;
        DebugLog::instance().write(QStringLiteral("repeatTool tool=%1")
                                       .arg(toolName(tool)));
        if (tool == Tool::Rotate) {
            if (beginRotate() && toolRepeated_) {
                toolRepeated_(tool);
            }
            return;
        }
        if (tool == Tool::Scale) {
            if (beginScale(scaleMode_) && toolRepeated_) {
                toolRepeated_(tool);
            }
            return;
        }
        if (tool == Tool::Mirror) {
            if (beginMirror() && toolRepeated_) {
                toolRepeated_(tool);
            }
            return;
        }
        setTool(tool);
        if (toolRepeated_) {
            toolRepeated_(tool);
        }
    }

    ToolId activeTool() const
    {
        return activeTool_;
    }

    void setArcMode(ArcMode mode)
    {
        arcMode_ = mode;
        pendingPoints_.clear();
        resetArcPreviewTracking();
        currentSnap_ = SnapResult{};
        DebugLog::instance().write(QStringLiteral("setArcMode mode=%1")
                                       .arg(arcModeName(arcMode_)));
        update();
    }

    ArcMode arcMode() const
    {
        return arcMode_;
    }

    void setWorkPlane(WorkPlane plane, qreal offset = 0.0) override
    {
        stopNavigationAnimation();
        if (!std::isfinite(offset)) {
            return;
        }
        const bool planeChanged = viewportTransform_.workPlane() != plane;
        const bool offsetChanged = !workPlaneMatches(viewportTransform_.workPlane(),
                                                     viewportTransform_.workPlaneOffset(),
                                                     plane,
                                                     offset);
        if (!planeChanged && !offsetChanged) {
            return;
        }

        setTool(Tool::Select);
        selection_.clear();
        controlPointIndex_ = -1;
        currentSnap_ = SnapResult{};
        currentDragSnap_ = DragSnapResult{};
        viewportTransform_.setWorkPlane(plane, offset);
        if (planeChanged) {
            switch (plane) {
            case WorkPlane::XY:
                viewportTransform_.setViewPreset(ViewportViewPreset::Top);
                break;
            case WorkPlane::XZ:
                viewportTransform_.setViewPreset(ViewportViewPreset::Front);
                break;
            case WorkPlane::YZ:
                viewportTransform_.setViewPreset(ViewportViewPreset::Right);
                break;
            }
        }
        const QPoint localCursor = mapFromGlobal(QCursor::pos());
        if (rect().contains(localCursor)) {
            rawCursorWorld_ = screenToWorld(localCursor);
            cursorWorld_ = rawCursorWorld_;
        }
        DebugLog::instance().write(QStringLiteral("work plane changed plane=%1")
                                       .arg(workPlaneName(plane)));
        notifyViewStateChanged();
        update();
        emitCoordinateUpdate();
    }

    WorkPlane workPlane() const override
    {
        return viewportTransform_.workPlane();
    }

    qreal workPlaneOffset() const override
    {
        return viewportTransform_.workPlaneOffset();
    }

    void setViewPreset(ViewportViewPreset preset) override
    {
        animateCameraChange([this, preset]() {
            viewportTransform_.setViewPreset(preset);
        });
    }

    ViewportViewPreset viewPreset() const override
    {
        return viewportTransform_.viewPreset();
    }

    void setControlPointsVisible(bool visible)
    {
        controlPointsVisible_ = visible;
        if (!visible) {
            draggingControlPoint_ = false;
            controlPointIndex_ = -1;
            dragHistoryRecorded_ = false;
            currentDragSnap_ = DragSnapResult{};
            dragSnapLocked_ = false;
        }
        DebugLog::instance().write(QStringLiteral("setControlPointsVisible=%1 selectedShape=%2")
                                       .arg(controlPointsVisible_)
                                       .arg(objectIndex(selectedShapeIndex_)));
        update();
    }

    bool controlPointsVisible() const
    {
        return controlPointsVisible_;
    }

    void setSnapLabelsVisible(bool visible) override
    {
        viewportOverlay_.setSnapLabelsVisible(visible);
        DebugLog::instance().write(QStringLiteral("setSnapLabelsVisible=%1").arg(visible));
        update();
    }

    void setSmoothCurveDisplay(bool enabled) override
    {
        viewportRenderer_.setSmoothCurveDisplay(enabled);
        DebugLog::instance().write(QStringLiteral("setSmoothCurveDisplay=%1").arg(enabled));
        update();
    }

    DocumentSettings documentSettings() const override
    {
        return document_.settings();
    }

    bool setDocumentSettings(const DocumentSettings &settings) override
    {
        if (!isValidDocumentSettings(settings)) {
            return false;
        }
        if (document_.settings() == settings) {
            return true;
        }
        const Document::Snapshot previous = document_.snapshot();
        if (!document_.setSettings(settings)) {
            return false;
        }
        viewportRenderer_.setGridBaseStep(documentGridSpacingInMillimeters(settings));
        recordGeometrySnapshot(previous);
        update();
        return true;
    }

    BlenderGridAppearance gridAppearance() const override
    {
        return gridAppearance_;
    }

    void setGridAppearance(const BlenderGridAppearance &appearance) override
    {
        if (!isValidBlenderGridAppearance(appearance)) {
            return;
        }
        gridAppearance_ = appearance;
        viewportRenderer_.setGridAppearance(appearance);
        update();
    }

    ViewportCameraPreferences cameraPreferences() const override
    {
        return viewportTransform_.cameraPreferences();
    }

    bool setCameraPreferences(
        const ViewportCameraPreferences &preferences) override
    {
        if (!viewportTransform_.setCameraPreferences(preferences)) {
            return false;
        }
        update();
        emitCoordinateUpdate();
        return true;
    }

    ViewportNavigationPreferences navigationPreferences() const override
    {
        return viewportTransform_.navigationPreferences();
    }

    void setNavigationPreferences(
        const ViewportNavigationPreferences &preferences) override
    {
        viewportTransform_.setNavigationPreferences(preferences);
        update();
    }

    int viewportAntiAliasingSamples() const override
    {
        return blenderGridRenderer_.antiAliasingSamples();
    }

    void setViewportAntiAliasingSamples(int samples) override
    {
        blenderGridRenderer_.setAntiAliasingSamples(samples);
        if (gpuSurface_ != nullptr) {
            gpuSurface_->setAntiAliasingSamples(samples);
        }
        update();
    }

    void setArchitecturalDimensionFont(bool enabled) override
    {
        viewportRenderer_.setArchitecturalDimensionFont(enabled);
        curveHitTester_.setArchitecturalDimensionFont(enabled);
        DebugLog::instance().write(
            QStringLiteral("setArchitecturalDimensionFont=%1").arg(enabled));
        update();
    }

    void setPanButton(Qt::MouseButton button)
    {
        if (button != Qt::MiddleButton && button != Qt::RightButton) {
            DebugLog::instance().write(QStringLiteral("setPanButton ignored invalid=%1")
                                           .arg(static_cast<int>(button)));
            return;
        }

        panButton_ = button;
        DebugLog::instance().write(QStringLiteral("setPanButton applied=%1")
                                       .arg(inputButtonName(button)));
        update();
    }

    Qt::MouseButton panButton() const
    {
        return panButton_;
    }

    void setOrthoEnabled(bool enabled)
    {
        orthoEnabled_ = enabled;
        refreshCursorConstraint();

        if (activeTool_ == Tool::Arc && arcMode_ == ArcMode::OnePoint &&
            pendingPoints_.size() >= 2 && cursorValid_) {
            // Reconcile the live sweep immediately when Ortho changes while
            // the endpoint is being positioned. Otherwise the cursor marker
            // and the preview can briefly represent different angles.
            updateArcPreviewTracking(cursorWorld_);
        }

        DebugLog::instance().write(QStringLiteral("setOrthoEnabled=%1 cursor=%2")
                                       .arg(orthoEnabled_)
                                       .arg(pointText(cursorWorld_)));
        update();
    }

    bool orthoEnabled() const
    {
        return orthoEnabled_;
    }

    void setOsnapEnabled(bool enabled)
    {
        osnapEnabled_ = enabled;
        updateSnapEngineSettings();
        if (!enabled) {
            currentDragSnap_ = DragSnapResult{};
            dragSnapLocked_ = false;
        }
        refreshCursorConstraint();
        DebugLog::instance().write(QStringLiteral("setOsnapEnabled=%1 snap=%2")
                                       .arg(osnapEnabled_)
                                       .arg(snapTypeName(currentSnap_.type)));
        update();
    }

    bool osnapEnabled() const
    {
        return osnapEnabled_;
    }

    void setSnapModes(bool endpoint,
                      bool midpoint,
                      bool intersection,
                      bool center,
                      bool perpendicular,
                      bool tangent,
                      bool near,
                      bool controlPoint)
    {
        endpointSnapEnabled_ = endpoint;
        midpointSnapEnabled_ = midpoint;
        intersectionSnapEnabled_ = intersection;
        centerSnapEnabled_ = center;
        perpendicularSnapEnabled_ = perpendicular;
        tangentSnapEnabled_ = tangent;
        nearSnapEnabled_ = near;
        controlPointSnapEnabled_ = controlPoint;
        updateSnapEngineSettings();
        refreshCursorConstraint();
        DebugLog::instance().write(QStringLiteral("setSnapModes endpoint=%1 midpoint=%2 intersection=%3 center=%4 perpendicular=%5 tangent=%6 near=%7 controlPoint=%8 snap=%9")
                                       .arg(endpointSnapEnabled_)
                                       .arg(midpointSnapEnabled_)
                                       .arg(intersectionSnapEnabled_)
                                       .arg(centerSnapEnabled_)
                                       .arg(perpendicularSnapEnabled_)
                                       .arg(tangentSnapEnabled_)
                                       .arg(nearSnapEnabled_)
                                       .arg(controlPointSnapEnabled_)
                                       .arg(snapTypeName(currentSnap_.type)));
        update();
    }

    QString coordinateText() const
    {
        const QString firstAxis = viewportTransform_.workPlane() == WorkPlane::YZ
                                      ? QStringLiteral("Y")
                                      : QStringLiteral("X");
        const QString secondAxis = viewportTransform_.workPlane() == WorkPlane::XY
                                       ? QStringLiteral("Y")
                                       : QStringLiteral("Z");
        return QStringLiteral("%1 %2   %3 %4   Zoom %5%")
            .arg(firstAxis)
            .arg(lastWorldPosition_.x(), 0, 'f', 2)
            .arg(secondAxis)
            .arg(lastWorldPosition_.y(), 0, 'f', 2)
            .arg(zoom_ * 100.0, 0, 'f', 0);
    }

    bool beginSubdivisionWheelMode()
    {
        const int selectedIndex = objectIndex(selectedShapeIndex_);
        if (selectedIndex < 0 || !isSubdividableShape(shapes_[selectedIndex])) {
            DebugLog::instance().write(QStringLiteral("beginSubdivisionWheelMode ignored selectedShape=%1")
                                           .arg(selectedIndex));
            return false;
        }

        subdivisionShapeIndex_ = selectedShapeIndex_;
        subdivisionSections_ = std::clamp(
            static_cast<int>(shapes_[selectedIndex].subdivisionParameters.size()) + 1,
            2,
            maxSubdivisionSections);
        resetSubdivisionWheelTracking();
        subdivisionActive_ = true;
        setFocus(Qt::OtherFocusReason);
        notifySubdivisionStatus();
        update();
        DebugLog::instance().write(QStringLiteral("beginSubdivisionWheelMode shape=%1 sections=%2")
                                       .arg(selectedIndex)
                                       .arg(subdivisionSections_));
        return true;
    }

    void cancelSubdivisionWheelMode()
    {
        cancelSubdivisionPreview();
    }

    bool applySubdivision(int sections)
    {
        if (sections < 2 || sections > maxSubdivisionSections) {
            DebugLog::instance().write(QStringLiteral("applySubdivision rejected sections=%1")
                                           .arg(sections));
            return false;
        }

        const ObjectId shapeId = subdivisionActive_ ? subdivisionShapeIndex_ : selectedShapeIndex_;
        const int shapeIndex = objectIndex(shapeId);
        if (shapeIndex < 0 ||
            !isSubdividableShape(shapes_[shapeIndex])) {
            DebugLog::instance().write(QStringLiteral("applySubdivision ignored selectedShape=%1")
                                           .arg(shapeIndex));
            return false;
        }

        const QVector<double> parameters = subdivisionParametersForSections(
            shapes_[shapeIndex], sections);
        if (parameters.size() != sections - 1) {
            DebugLog::instance().write(QStringLiteral("applySubdivision failed shape=%1 sections=%2 generated=%3")
                                           .arg(shapeIndex)
                                           .arg(sections)
                                           .arg(parameters.size()));
            return false;
        }

        if (shapes_[shapeIndex].subdivisionParameters != parameters) {
            recordGeometryChange();
            shapes_[shapeIndex].subdivisionParameters = parameters;
        }

        subdivisionActive_ = false;
        subdivisionShapeIndex_ = ObjectId::invalid();
        subdivisionSections_ = 2;
        resetSubdivisionWheelTracking();
        notifySubdivisionStatus();
        update();
        DebugLog::instance().write(QStringLiteral("applySubdivision shape=%1 sections=%2 points=%3")
                                       .arg(shapeIndex)
                                       .arg(sections)
                                       .arg(parameters.size()));
        return true;
    }

    bool subdivisionActive() const
    {
        return subdivisionActive_;
    }

    QString subdivisionStatusText() const
    {
        if (!subdivisionActive_) {
            return QString();
        }

        return QStringLiteral("Subdivide: %1 sections  •  Scroll to change  •  Click/Enter to apply  •  Esc to cancel")
            .arg(subdivisionSections_);
    }

    bool beginRotate()
    {
        QVector<ObjectId> selected = selectedShapeIndices_;
        if (joinActive_) {
            selected = joinShapeIndices_;
        }
        if (selectedShapeIndex_.isValid() && objectIndex(selectedShapeIndex_) >= 0 &&
            !selected.contains(selectedShapeIndex_)) {
            selected.append(selectedShapeIndex_);
        }

        QVector<ObjectId> validSelection;
        for (const ObjectId objectId : selected) {
            if (objectIndex(objectId) >= 0 && !validSelection.contains(objectId)) {
                validSelection.append(objectId);
            }
        }
        if (validSelection.isEmpty()) {
            DebugLog::instance().write(QStringLiteral("beginRotate ignored no selection"));
            return false;
        }

        setTool(Tool::Rotate);
        rotateShapeIndices_ = validSelection;
        rotateStep_ = 0;
        rotateBaseWorld_ = QPointF();
        rotateReferenceWorld_ = QPointF();
        rotatePreviewAngle_ = 0.0;
        setFocus(Qt::OtherFocusReason);
        setCursor(Qt::CrossCursor);
        update();
        DebugLog::instance().write(QStringLiteral("beginRotate shapes=%1")
                                       .arg(rotateShapeIndices_.size()));
        return true;
    }

    bool beginScale(ScaleMode mode)
    {
        QVector<ObjectId> selected = selectedShapeIndices_;
        if (selectedShapeIndex_.isValid() && !selected.contains(selectedShapeIndex_)) {
            selected.append(selectedShapeIndex_);
        }

        QVector<ObjectId> validSelection;
        for (const ObjectId objectId : selected) {
            if (document_.isObjectEditable(objectId) && !validSelection.contains(objectId)) {
                validSelection.append(objectId);
            }
        }
        if (validSelection.isEmpty()) {
            DebugLog::instance().write(QStringLiteral("beginScale ignored no editable selection"));
            return false;
        }

        resetScaleInteraction();
        setTool(Tool::Scale);
        scaleMode_ = mode;
        scaleShapeIds_ = validSelection;
        scaleStep_ = 0;
        setFocus(Qt::OtherFocusReason);
        setCursor(Qt::CrossCursor);
        publishScalePrompt();
        update();
        DebugLog::instance().write(QStringLiteral("beginScale mode=%1 shapes=%2")
                                       .arg(scaleModeName(scaleMode_))
                                       .arg(scaleShapeIds_.size()));
        return true;
    }

    bool beginMirror()
    {
        QVector<ObjectId> selected = selectedShapeIndices_;
        if (selectedShapeIndex_.isValid() && !selected.contains(selectedShapeIndex_)) {
            selected.append(selectedShapeIndex_);
        }

        QVector<ObjectId> validSelection;
        for (const ObjectId objectId : selected) {
            if (document_.isObjectEditable(objectId) && !validSelection.contains(objectId)) {
                validSelection.append(objectId);
            }
        }
        if (validSelection.isEmpty()) {
            DebugLog::instance().write(QStringLiteral("beginMirror ignored no editable selection"));
            return false;
        }

        setTool(Tool::Mirror);
        mirrorShapeIndices_ = validSelection;
        pendingPoints_.clear();
        setFocus(Qt::OtherFocusReason);
        setCursor(Qt::CrossCursor);
        update();
        DebugLog::instance().write(QStringLiteral("beginMirror shapes=%1")
                                       .arg(mirrorShapeIndices_.size()));
        return true;
    }

    bool beginGrab()
    {
        if (activeTool_ != Tool::Select || grabActive_ || draggingSelected_) {
            return false;
        }

        QVector<ObjectId> selected = selectedShapeIndices_;
        if (selectedShapeIndex_.isValid() && !selected.contains(selectedShapeIndex_)) {
            selected.append(selectedShapeIndex_);
        }

        QVector<ObjectId> validSelection;
        for (const ObjectId objectId : selected) {
            if (document_.isObjectEditable(objectId) && !validSelection.contains(objectId)) {
                validSelection.append(objectId);
            }
        }
        if (validSelection.isEmpty()) {
            DebugLog::instance().write(QStringLiteral("beginGrab ignored no editable selection"));
            return false;
        }

        grabStartSnapshot_ = document_.snapshot();
        grabActive_ = true;
        grabMoved_ = false;
        grabPickingBasePoint_ = false;
        grabHasBasePoint_ = false;
        draggingSelected_ = true;
        dragGestureStarted_ = true;
        draggingShapeIndices_ = validSelection;
        if (!selectedShapeIndex_.isValid() || !validSelection.contains(selectedShapeIndex_)) {
            selectedShapeIndex_ = validSelection.back();
        }
        dragHistoryRecorded_ = false;
        currentDragSnap_ = DragSnapResult{};
        dragSnapLocked_ = false;
        dragAxisLock_ = DragAxisLock::None;
        currentSnap_ = SnapResult{};

        const QPoint localCursor = mapFromGlobal(QCursor::pos());
        if (rect().contains(localCursor)) {
            rawCursorWorld_ = screenToWorld(localCursor);
            cursorWorld_ = rawCursorWorld_;
            cursorValid_ = true;
        }
        dragStartScreen_ = localCursor;
        grabStartWorld_ = rawCursorWorld_;
        lastDragWorld_ = grabStartWorld_;
        setFocus(Qt::OtherFocusReason);
        setCursor(Qt::SizeAllCursor);
        update();
        DebugLog::instance().write(QStringLiteral("beginGrab shapes=%1")
                                       .arg(draggingShapeIndices_.size()));
        return true;
    }

    void beginGrabBasePointMode()
    {
        if (!grabActive_) {
            return;
        }

        if (grabMoved_) {
            document_.restoreSnapshot(grabStartSnapshot_);
            notifyLayersChanged();
        }
        grabMoved_ = false;
        grabPickingBasePoint_ = true;
        grabHasBasePoint_ = false;
        grabCursorOffset_ = QPointF();
        currentSnap_ = SnapResult{};
        currentDragSnap_ = DragSnapResult{};
        dragSnapLocked_ = false;
        setCursor(Qt::CrossCursor);
        update();
        DebugLog::instance().write(QStringLiteral("grab base-point selection started"));
    }

    void finishGrab()
    {
        if (!grabActive_) {
            return;
        }

        if (grabMoved_) {
            recordGeometrySnapshot(grabStartSnapshot_);
        }
        const bool moved = grabMoved_;
        resetGrabInteraction();
        draggingSelected_ = false;
        draggingShapeIndices_.clear();
        dragHistoryRecorded_ = false;
        currentDragSnap_ = DragSnapResult{};
        currentSnap_ = SnapResult{};
        dragSnapLocked_ = false;
        dragAxisLock_ = DragAxisLock::None;
        setCursor(Qt::ArrowCursor);
        update();
        DebugLog::instance().write(QStringLiteral("grab finished moved=%1").arg(moved));
    }

    void cancelGrab()
    {
        if (!grabActive_) {
            return;
        }

        const bool moved = grabMoved_;
        if (moved) {
            document_.restoreSnapshot(grabStartSnapshot_);
            notifyLayersChanged();
        }
        resetGrabInteraction();
        draggingSelected_ = false;
        draggingShapeIndices_.clear();
        dragHistoryRecorded_ = false;
        currentDragSnap_ = DragSnapResult{};
        currentSnap_ = SnapResult{};
        dragSnapLocked_ = false;
        dragAxisLock_ = DragAxisLock::None;
        setCursor(Qt::ArrowCursor);
        update();
        DebugLog::instance().write(QStringLiteral("grab canceled moved=%1").arg(moved));
    }

    bool beginDuplicate()
    {
        if (activeTool_ != Tool::Select || duplicateActive_ || grabActive_ ||
            draggingSelected_ || joinActive_ || subdivisionActive_) {
            return false;
        }

        QVector<ObjectId> selected = selectedShapeIndices_;
        if (selectedShapeIndex_.isValid() && !selected.contains(selectedShapeIndex_)) {
            selected.append(selectedShapeIndex_);
        }

        duplicateSourceObjects_.clear();
        duplicateSourceObjects_.reserve(selected.size());
        for (const ObjectId objectId : selected) {
            const SceneObject *sceneObject = document_.object(objectId);
            if (sceneObject != nullptr && document_.isObjectEditable(objectId)) {
                duplicateSourceObjects_.append(*sceneObject);
            }
        }
        if (duplicateSourceObjects_.isEmpty()) {
            return false;
        }

        duplicateActive_ = true;
        duplicatePickingBasePoint_ = true;
        duplicateHasBasePoint_ = false;
        duplicateBasePoint_ = QPointF();
        duplicateCursorOffset_ = QPointF();
        duplicateDestination_ = QPointF();
        duplicatePreviewShapes_.clear();
        currentSnap_ = SnapResult{};
        setFocus(Qt::OtherFocusReason);
        setCursor(Qt::CrossCursor);
        update();
        DebugLog::instance().write(
            QStringLiteral("beginDuplicate objects=%1")
                .arg(duplicateSourceObjects_.size()));
        return true;
    }

    bool duplicateInPlace()
    {
        if (!beginDuplicate()) {
            return false;
        }

        duplicatePickingBasePoint_ = false;
        duplicateHasBasePoint_ = true;
        duplicateBasePoint_ = QPointF();
        duplicateCursorOffset_ = QPointF();
        duplicateDestination_ = QPointF();
        duplicatePreviewShapes_.reserve(duplicateSourceObjects_.size());
        for (const SceneObject &source : duplicateSourceObjects_) {
            duplicatePreviewShapes_.append(source.geometry);
        }
        finishDuplicate();
        return true;
    }

    void updateDuplicatePreview(const QPointF &rawPoint)
    {
        if (!duplicateActive_ || !duplicateHasBasePoint_) {
            return;
        }

        const QPointF destinationCursor = rawPoint - duplicateCursorOffset_;
        currentSnap_ = findDuplicateDestinationSnap(destinationCursor);
        duplicateDestination_ = currentSnap_.isValid()
                                    ? currentSnap_.point
                                    : destinationCursor;
        cursorWorld_ = duplicateDestination_;
        lastWorldPosition_ = duplicateDestination_;

        const QPointF delta = duplicateDestination_ - duplicateBasePoint_;
        duplicatePreviewShapes_.clear();
        duplicatePreviewShapes_.reserve(duplicateSourceObjects_.size());
        for (const SceneObject &source : duplicateSourceObjects_) {
            Shape preview = source.geometry;
            translateShapeGeometry(preview, delta);
            duplicatePreviewShapes_.append(preview);
        }
        update();
    }

    void finishDuplicate()
    {
        if (!duplicateActive_ || !duplicateHasBasePoint_ ||
            duplicatePreviewShapes_.isEmpty()) {
            return;
        }

        const Document::Snapshot before = document_.snapshot();
        QVector<ObjectId> duplicateIds;
        duplicateIds.reserve(duplicatePreviewShapes_.size());
        for (int index = 0; index < duplicatePreviewShapes_.size(); ++index) {
            SceneObject duplicate = duplicateSourceObjects_[index];
            duplicate.id = ObjectId::invalid();
            duplicate.geometry = duplicatePreviewShapes_[index];
            duplicateIds.append(shapes_.insertObject(shapes_.size(), duplicate));
        }

        for (int duplicateIndex = 0; duplicateIndex < duplicateIds.size(); ++duplicateIndex) {
            Shape *duplicateGeometry = document_.shape(duplicateIds[duplicateIndex]);
            if (duplicateGeometry == nullptr ||
                !isDimensionGeometryType(duplicateGeometry->geometryType)) {
                continue;
            }
            for (DimensionAnchorReference &anchor : duplicateGeometry->dimensionAnchors) {
                bool remapped = false;
                for (int sourceIndex = 0;
                     sourceIndex < duplicateSourceObjects_.size();
                     ++sourceIndex) {
                    if (anchor.objectId == duplicateSourceObjects_[sourceIndex].id) {
                        anchor.objectId = duplicateIds[sourceIndex];
                        remapped = true;
                        break;
                    }
                }
                if (!remapped) {
                    // A copied dimension without its referenced geometry is a
                    // positioned copy, not a second annotation tied to the
                    // original objects.
                    anchor = DimensionAnchorReference{};
                }
            }
        }

        recordGeometrySnapshot(before);
        selection_.setObjectIds(duplicateIds,
                                duplicateIds.isEmpty() ? ObjectId::invalid()
                                                       : duplicateIds.back());
        selectedShapeIndex_ = selection_.primaryObjectId();
        const int duplicateCount = duplicateIds.size();
        const QPointF committedDelta = duplicateDestination_ - duplicateBasePoint_;
        resetDuplicateInteraction();
        currentSnap_ = SnapResult{};
        setCursor(Qt::ArrowCursor);
        notifyLayersChanged();
        update();
        emitCoordinateUpdate();
        if (commandFinished_) {
            commandFinished_(Tool::Select);
        }
        DebugLog::instance().write(
            QStringLiteral("duplicate committed objects=%1 delta=%2")
                .arg(duplicateCount)
                .arg(pointText(committedDelta)));
    }

    void cancelDuplicate()
    {
        if (!duplicateActive_) {
            return;
        }
        resetDuplicateInteraction();
        currentSnap_ = SnapResult{};
        setCursor(activeTool_ == Tool::Select ? Qt::ArrowCursor : Qt::CrossCursor);
        update();
        DebugLog::instance().write(QStringLiteral("duplicate canceled"));
    }

    bool beginJoinMode()
    {
        if (subdivisionActive_) {
            cancelSubdivisionPreview();
        }

        const QVector<ObjectId> preselectedShapes = selectedShapeIndices_;
        setTool(Tool::Select);
        joinActive_ = true;
        joinShapeIndices_ = preselectedShapes;
        if (joinShapeIndices_.isEmpty()) {
            selectedShapeIndices_.clear();
            selectedShapeIndex_ = ObjectId::invalid();
        } else {
            selectedShapeIndex_ = joinShapeIndices_.back();
        }
        selectionBoxActive_ = false;
        selectionBoxMoved_ = false;
        selectionBoxAdditive_ = false;
        trimBoxSelectionActive_ = false;
        draggingSelected_ = false;
        draggingShapeIndices_.clear();
        draggingControlPoint_ = false;
        controlPointIndex_ = -1;
        setFocus(Qt::OtherFocusReason);
        setCursor(Qt::CrossCursor);
        notifyJoinStatus();
        update();
        DebugLog::instance().write(QStringLiteral("beginJoinMode"));
        return true;
    }

    void cancelJoinMode()
    {
        if (!joinActive_) {
            return;
        }

        const QVector<ObjectId> joinedSelection = joinShapeIndices_;
        joinActive_ = false;
        joinShapeIndices_.clear();
        selectedShapeIndices_ = joinedSelection;
        selectedShapeIndex_ = selectedShapeIndices_.isEmpty()
                                  ? ObjectId::invalid()
                                  : selectedShapeIndices_.back();
        setCursor(Qt::ArrowCursor);
        notifyJoinStatus();
        update();
        DebugLog::instance().write(QStringLiteral("cancelJoinMode"));
    }

    QString joinStatusText() const
    {
        if (!joinActive_) {
            return QString();
        }

        return QStringLiteral("Join: %1 curves selected  •  Click connected curves in order  •  Enter to join  •  Esc to cancel")
            .arg(joinShapeIndices_.size());
    }

    bool applyJoin()
    {
        if (!joinActive_) {
            return false;
        }

        if (joinShapeIndices_.size() < 2) {
            notifyJoinStatus(QStringLiteral("Join needs at least two curves"));
            return false;
        }

        QVector<Shape::NurbsCurve2D> components;
        for (const ObjectId objectId : joinShapeIndices_) {
            const int shapeIndex = objectIndex(objectId);
            if (shapeIndex < 0 || shapeIndex >= shapes_.size() ||
                !appendJoinComponents(shapes_[shapeIndex], &components)) {
                notifyJoinStatus(QStringLiteral("Join failed — select lines or curves only"));
                return false;
            }
        }

        if (components.size() < 2) {
            notifyJoinStatus(QStringLiteral("Join failed — select at least two curves"));
            return false;
        }

        if (!joinComponentsAreContinuous(components)) {
            QVector<Shape::NurbsCurve2D> orderedComponents;
            if (!orderJoinComponents(components, &orderedComponents)) {
                notifyJoinStatus(QStringLiteral("Join failed — selected curves are not connected"));
                DebugLog::instance().write(
                    QStringLiteral("applyJoin rejected disconnected components=%1 tolerance=%2")
                        .arg(components.size())
                        .arg(joinEndpointTolerance(), 0, 'f', 6));
                return false;
            }
            components = orderedComponents;
            DebugLog::instance().write(QStringLiteral("applyJoin reordered/reversed connected components=%1")
                                           .arg(components.size()));
        }

        if (!closeJoinGaps(&components) || !joinComponentsAreContinuous(components)) {
            notifyJoinStatus(QStringLiteral("Join failed — selected curves are not connected"));
            DebugLog::instance().write(QStringLiteral("applyJoin rejected reordered components=%1")
                                           .arg(components.size()));
            return false;
        }

        int insertIndex = shapes_.size();
        for (const ObjectId objectId : joinShapeIndices_) {
            const int shapeIndex = objectIndex(objectId);
            if (shapeIndex >= 0) {
                insertIndex = std::min(insertIndex, shapeIndex);
            }
        }
        const int sourceShapeCount = joinShapeIndices_.size();
        recordGeometryChange();

        QVector<int> indicesToRemove;
        for (const ObjectId objectId : joinShapeIndices_) {
            const int shapeIndex = objectIndex(objectId);
            if (shapeIndex >= 0) {
                indicesToRemove.append(shapeIndex);
            }
        }
        std::sort(indicesToRemove.begin(), indicesToRemove.end());
        for (auto index = indicesToRemove.crbegin(); index != indicesToRemove.crend(); ++index) {
            shapes_.removeAt(*index);
        }

        Shape joined{GeometryType::PolyCurve,
                     polyCurvePoints(components),
                     Shape::NurbsCurve2D{},
                     ArcMode::TwoPoint,
                     0.0,
                     {},
                     components};
        joined.workPlane = viewportTransform_.workPlane();
        joined.workPlaneOffset = viewportTransform_.workPlaneOffset();
        const ObjectId joinedObjectId = shapes_.insert(insertIndex, joined);

        joinActive_ = false;
        joinShapeIndices_.clear();
        selectedShapeIndices_ = {joinedObjectId};
        selectedShapeIndex_ = joinedObjectId;
        setCursor(Qt::ArrowCursor);
        notifyJoinStatus(QStringLiteral("Joined %1 curves into one PolyCurve")
                             .arg(sourceShapeCount));
        update();
        DebugLog::instance().write(QStringLiteral("applyJoin committed components=%1 shapes=%2")
                                       .arg(components.size())
                                       .arg(shapes_.size()));
        return true;
    }

    int explodeSelectedShapes()
    {
        if (joinActive_) {
            cancelJoinMode();
        }

        QVector<ObjectId> selected = selectedShapeIndices_;
        if (selectedShapeIndex_.isValid() &&
            objectIndex(selectedShapeIndex_) >= 0 &&
            !selected.contains(selectedShapeIndex_)) {
            selected.append(selectedShapeIndex_);
        }

        if (selected.isEmpty()) {
            DebugLog::instance().write(QStringLiteral("explode ignored no selection"));
            return 0;
        }

        const auto selectedContains = [&selected](ObjectId objectId) {
            return selected.contains(objectId);
        };

        int explodeableShapeCount = 0;
        int explodedComponentCount = 0;
        for (const ObjectId objectId : selected) {
            const int shapeIndex = objectIndex(objectId);
            if (shapeIndex < 0 || shapeIndex >= shapes_.size()) {
                continue;
            }

            const Shape &shape = shapes_[shapeIndex];
            if (shape.geometryType != GeometryType::PolyCurve || shape.components.isEmpty()) {
                continue;
            }

            bool validComponents = true;
            for (const Shape::NurbsCurve2D &component : shape.components) {
                if (!isValidNurbsCurve(component)) {
                    validComponents = false;
                    break;
                }
            }
            if (validComponents) {
                ++explodeableShapeCount;
                explodedComponentCount += shape.components.size();
            }
        }

        if (explodeableShapeCount == 0) {
            DebugLog::instance().write(QStringLiteral("explode ignored no PolyCurve selection"));
            return 0;
        }

        recordGeometryChange();

        QVector<SceneObject> explodedObjects;
        QVector<int> explodedSelectionIndices;
        explodedObjects.reserve(shapes_.size() + explodedComponentCount - explodeableShapeCount);

        for (int sourceIndex = 0; sourceIndex < shapes_.size(); ++sourceIndex) {
            const SceneObject sourceObject = document_.objects()[sourceIndex];
            const Shape &source = sourceObject.geometry;
            const bool selectedSource = selectedContains(sourceObject.id);
            const bool canExplode = selectedSource &&
                                    source.geometryType == GeometryType::PolyCurve &&
                                    !source.components.isEmpty();
            bool validComponents = canExplode;
            if (validComponents) {
                for (const Shape::NurbsCurve2D &component : source.components) {
                    if (!isValidNurbsCurve(component)) {
                        validComponents = false;
                        break;
                    }
                }
            }

            if (!validComponents) {
                const int newIndex = explodedObjects.size();
                explodedObjects.append(sourceObject);
                if (selectedSource) {
                    explodedSelectionIndices.append(newIndex);
                }
                continue;
            }

            for (const Shape::NurbsCurve2D &component : source.components) {
                const int newIndex = explodedObjects.size();
                SceneObject componentObject;
                componentObject.layerId = sourceObject.layerId;
                componentObject.geometry = Shape{
                    GeometryType::PolyCurve,
                    polyCurvePoints({component}),
                    Shape::NurbsCurve2D{},
                    ArcMode::TwoPoint,
                    0.0,
                    {},
                    {component}};
                componentObject.geometry.workPlane = source.workPlane;
                componentObject.geometry.workPlaneOffset = source.workPlaneOffset;
                explodedObjects.append(componentObject);
                explodedSelectionIndices.append(newIndex);
            }
        }

        document_.replaceObjects(explodedObjects);
        selectedShapeIndices_.clear();
        for (const int selectedIndex : explodedSelectionIndices) {
            const ObjectId objectId = document_.objectIdAt(selectedIndex);
            if (objectId.isValid()) {
                selectedShapeIndices_.append(objectId);
            }
        }
        selectedShapeIndex_ = selectedShapeIndices_.isEmpty()
                                  ? ObjectId::invalid()
                                  : selectedShapeIndices_.back();
        draggingSelected_ = false;
        draggingShapeIndices_.clear();
        draggingControlPoint_ = false;
        controlPointIndex_ = -1;
        dragHistoryRecorded_ = false;
        currentDragSnap_ = DragSnapResult{};
        dragSnapLocked_ = false;
        update();
        DebugLog::instance().write(
            QStringLiteral("explode committed shapes=%1 components=%2")
                .arg(explodeableShapeCount)
                .arg(explodedComponentCount));
        return explodedComponentCount;
    }

    bool saveUpdateSession(const QString &path) const
    {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            DebugLog::instance().write(QStringLiteral("saveUpdateSession failed path=%1 error=%2")
                                           .arg(path, file.errorString()));
            return false;
        }

        QJsonObject root;
        // Version 3 stores the document once, including its layers and object
        // identities. The version 1/2 restore path below still reads legacy
        // shape-only sessions.
        root.insert(QStringLiteral("version"), 3);
        root.insert(QStringLiteral("zoom"), zoom_);
        root.insert(QStringLiteral("pan"), pointToJson(pan_));
        root.insert(QStringLiteral("document"), documentToJson(document_));

        const QByteArray data = QJsonDocument(root).toJson(QJsonDocument::Compact);
        if (file.write(data) != data.size()) {
            DebugLog::instance().write(QStringLiteral("saveUpdateSession write failed path=%1 error=%2")
                                           .arg(path, file.errorString()));
            return false;
        }

        DebugLog::instance().write(QStringLiteral("saveUpdateSession path=%1 shapes=%2 layers=%3")
                                       .arg(path)
                                       .arg(shapes_.size())
                                       .arg(document_.layers().size()));
        return true;
    }

    bool restoreUpdateSession(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            DebugLog::instance().write(QStringLiteral("restoreUpdateSession failed path=%1 error=%2")
                                           .arg(path, file.errorString()));
            return false;
        }

        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            DebugLog::instance().write(QStringLiteral("restoreUpdateSession parse failed path=%1 error=%2")
                                           .arg(path, parseError.errorString()));
            return false;
        }

        const QJsonObject root = document.object();
        const int version = root.value(QStringLiteral("version")).toInt(-1);
        if (version != 1 && version != 2 && version != 3) {
            DebugLog::instance().write(QStringLiteral("restoreUpdateSession unsupported version=%1 path=%2")
                                           .arg(version)
                                           .arg(path));
            return false;
        }

        QPointF restoredPan;
        if (!pointFromJson(root.value(QStringLiteral("pan")), &restoredPan)) {
            DebugLog::instance().write(QStringLiteral("restoreUpdateSession invalid pan path=%1")
                                           .arg(path));
            return false;
        }

        const QJsonValue zoomValue = root.value(QStringLiteral("zoom"));
        const qreal restoredZoom = zoomValue.toDouble(1.0);
        if (!zoomValue.isDouble() || !std::isfinite(restoredZoom) || restoredZoom <= 1e-9) {
            DebugLog::instance().write(QStringLiteral("restoreUpdateSession invalid zoom path=%1")
                                           .arg(path));
            return false;
        }

        if (version == 3) {
            QString documentError;
            if (!documentFromJson(root.value(QStringLiteral("document")),
                                  &document_,
                                  &documentError)) {
                DebugLog::instance().write(
                    QStringLiteral("restoreUpdateSession invalid document path=%1 error=%2")
                        .arg(path, documentError));
                return false;
            }
        } else {
            const QJsonValue shapesValue = root.value(QStringLiteral("shapes"));
            if (!shapesValue.isArray()) {
                DebugLog::instance().write(QStringLiteral("restoreUpdateSession invalid shapes path=%1")
                                               .arg(path));
                return false;
            }

            QVector<Shape> restoredShapes;
            const QJsonArray shapes = shapesValue.toArray();
            restoredShapes.reserve(shapes.size());
            for (const QJsonValue &shapeValue : shapes) {
                Shape shape{GeometryType::Invalid,
                            {},
                            Shape::NurbsCurve2D{},
                            ArcMode::TwoPoint,
                            0.0,
                            {},
                            {}};
                if (!shapeFromJson(shapeValue, &shape)) {
                    DebugLog::instance().write(QStringLiteral("restoreUpdateSession invalid shape path=%1")
                                                   .arg(path));
                    return false;
                }
                restoredShapes.append(shape);
            }
            shapes_ = restoredShapes;
        }
        normalizeDisconnectedPolyCurveObjects();
        history_.clear();
        pendingPoints_.clear();
        resetArcPreviewTracking();
        selectedShapeIndices_.clear();
        selectedShapeIndex_ = ObjectId::invalid();
        selectionBoxActive_ = false;
        selectionBoxMoved_ = false;
        selectionBoxAdditive_ = false;
        draggingSelected_ = false;
        draggingShapeIndices_.clear();
        draggingControlPoint_ = false;
        controlPointIndex_ = -1;
        dragHistoryRecorded_ = false;
        currentDragSnap_ = DragSnapResult{};
        dragSnapLocked_ = false;
        eraseStrokeActive_ = false;
        eraseCursorPressed_ = false;
        eraseCandidateShapeIndices_.clear();
        eraseStrokeScreenPath_.clear();
        eraseTargetShapeIndices_.clear();
        eraseSceneCurveCaches_.clear();
        eraseTargetCurveCaches_.clear();
        eraseGeometryCachePrepared_ = false;
        trimHoverPositionValid_ = false;
        currentSnap_ = SnapResult{};
        joinActive_ = false;
        joinShapeIndices_.clear();
        resetScaleInteraction();
        resetRotateInteraction();
        resetMirrorInteraction();
        subdivisionActive_ = false;
        subdivisionShapeIndex_ = ObjectId::invalid();
        subdivisionSections_ = 2;
        resetSubdivisionWheelTracking();
        lineCommandActive_ = false;
        activeTool_ = Tool::Select;
        repeatTool_ = Tool::Select;
        pan_ = restoredPan;
        zoom_ = restoredZoom;
        setCursor(Qt::ArrowCursor);
        update();
        emitCoordinateUpdate();
        notifyHistoryChanged();
        notifyLayersChanged();

        DebugLog::instance().write(QStringLiteral("restoreUpdateSession path=%1 shapes=%2 layers=%3")
                                       .arg(path)
                                       .arg(shapes_.size())
                                       .arg(document_.layers().size()));
        return true;
    }

    bool saveVignolaDocument(const QString &path, QString *errorMessage) const override
    {
        return classiCAD::saveVignolaDocument(path, document_, errorMessage);
    }

    bool loadVignolaDocument(const QString &path, QString *errorMessage) override
    {
        Document restoredDocument;
        if (!classiCAD::loadVignolaDocument(path, &restoredDocument, errorMessage)) {
            return false;
        }

        document_ = std::move(restoredDocument);
        normalizeDisconnectedPolyCurveObjects();
        resetForDocumentReplacement();
        viewportTransform_.resetView();
        notifyViewStateChanged();
        setCursor(Qt::ArrowCursor);
        update();
        emitCoordinateUpdate();
        notifyHistoryChanged();
        notifyLayersChanged();
        return true;
    }

    bool importRhino3dmDocument(const QString &path,
                                Rhino3dmImportReport *report,
                                QString *errorMessage) override
    {
        const Document::Snapshot beforeImport = document_.snapshot();
        if (!classiCAD::importRhino3dmDocument(path,
                                              &document_,
                                              report,
                                              errorMessage)) {
            return false;
        }

        normalizeDisconnectedPolyCurveObjects();
        recordGeometrySnapshot(beforeImport);
        update();
        return true;
    }

    void createNewDocument() override
    {
        document_ = Document{};
        resetForDocumentReplacement();
        viewportTransform_.resetView();
        notifyViewStateChanged();
        setCursor(Qt::ArrowCursor);
        update();
        emitCoordinateUpdate();
        notifyHistoryChanged();
        notifyLayersChanged();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        if (gpuSurface_ != nullptr) {
            return;
        }
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        paintViewport(painter, nullptr, nullptr);
    }

    void resizeEvent(QResizeEvent *event) override
    {
        if (gpuSurface_ != nullptr) {
            gpuSurface_->setGeometry(rect());
        }
        QWidget::resizeEvent(event);
    }

    QVector<Shape> visibleDepthShapes() const
    {
        QVector<Shape> visibleShapes;
        visibleShapes.reserve(shapes_.size());
        for (int index = 0; index < shapes_.size(); ++index) {
            const ObjectId objectId = shapes_.objectIdAt(index);
            if (!document_.isObjectVisible(objectId)) {
                continue;
            }
            Shape depthShape = shapes_[index];
            if (activeTool_ == Tool::Scale && scalePreviewValid_ &&
                scaleShapeIds_.contains(objectId)) {
                scaleShapeGeometry(&depthShape,
                                   scaleBaseWorld_,
                                   scalePreviewAxis_,
                                   scalePreviewFactor_,
                                   scaleMode_);
            } else if (activeTool_ == Tool::Rotate && rotateStep_ == 2 &&
                       rotateShapeIndices_.contains(objectId)) {
                rotateShapeGeometry(&depthShape,
                                    rotateBaseWorld_,
                                    rotatePreviewAngle_);
            }
            visibleShapes.append(std::move(depthShape));
        }
        return visibleShapes;
    }

    void paintViewport(QPainter &painter,
                       BlenderGridRenderer *nativeRenderer,
                       ViewportSceneRenderer *sceneRenderer)
    {
        updateAssociativeDimensions(document_, curveSampler_);
        const qreal baseGridStep = documentGridSpacingInMillimeters(document_.settings());
        viewportRenderer_.setGridBaseStep(baseGridStep);
        viewportRenderer_.setGridAppearance(gridAppearance_);
        const QVector<Shape> visibleShapes = visibleDepthShapes();
        QImage gpuViewportBackground;
        QImage committedSceneLayer;
        QPainter rasterScenePainter;
        if (nativeRenderer == nullptr) {
            gpuViewportBackground = blenderGridRenderer_.render(
                viewportTransform_, size(), devicePixelRatioF(), visibleShapes,
                baseGridStep, gridAppearance_);
            const qreal devicePixelRatio =
                std::max<qreal>(devicePixelRatioF(), 1.0);
            const QSize scenePixelSize(qRound(size().width() * devicePixelRatio),
                                       qRound(size().height() * devicePixelRatio));
            committedSceneLayer = QImage(scenePixelSize,
                                         QImage::Format_ARGB32_Premultiplied);
            committedSceneLayer.setDevicePixelRatio(devicePixelRatio);
            committedSceneLayer.fill(Qt::transparent);
            rasterScenePainter.begin(&committedSceneLayer);
            rasterScenePainter.setRenderHint(QPainter::Antialiasing, true);
        } else {
            painter.beginNativePainting();
            const bool backgroundDrawn =
                nativeRenderer->renderBackgroundToCurrentFramebuffer(
                    size(), devicePixelRatioF());
            painter.endNativePainting();
            if (!backgroundDrawn) {
                fillViewportBackground(painter, rect());
            }
        }
        QPainter &scenePainter = nativeRenderer == nullptr
                                     ? rasterScenePainter
                                     : painter;
        QVector<ViewportSceneStroke> gpuStrokes;
        int visibleShapeIndex = 0;

        for (int index = 0; index < shapes_.size(); ++index) {
            const ObjectId objectId = shapes_.objectIdAt(index);
            if (!document_.isObjectVisible(objectId)) {
                continue;
            }
            const Shape &visibleShape = visibleShapes[visibleShapeIndex++];
            if (isDimensionGeometryType(shapes_[index].geometryType)) {
                continue;
            }
            QColor layerColor;
            QString layerLineType;
            qreal layerLineWeightMm = 0.0;
            const SceneObject *sceneObject = document_.object(objectId);
            const Layer *objectLayer = sceneObject == nullptr
                                           ? nullptr
                                           : document_.layer(sceneObject->layerId);
            if (objectLayer != nullptr) {
                layerColor = objectLayer->color;
                layerLineType = objectLayer->lineType;
                layerLineWeightMm = objectLayer->lineWeightMm;
            }
            const bool selected = selectedShapeIndices_.contains(objectId) ||
                                  objectId == selectedShapeIndex_ ||
                                  joinShapeIndices_.contains(objectId);
            const bool scalePreview = activeTool_ == Tool::Scale &&
                                      scalePreviewValid_ &&
                                      scaleShapeIds_.contains(objectId);
            const bool rotatePreview = activeTool_ == Tool::Rotate &&
                                        rotateStep_ == 2 &&
                                        rotateShapeIndices_.contains(objectId);
            const GeometryType geometryType = visibleShape.geometryType;
            const bool gpuStrokeType =
                geometryType == GeometryType::Point ||
                geometryType == GeometryType::Line ||
                geometryType == GeometryType::Rectangle ||
                geometryType == GeometryType::Polygon ||
                geometryType == GeometryType::Circle ||
                geometryType == GeometryType::Ellipse ||
                geometryType == GeometryType::Arc ||
                geometryType == GeometryType::PolyCurve ||
                geometryType == GeometryType::Bezier ||
                geometryType == GeometryType::Nurbs;
            const bool nativeCurve = sceneRenderer != nullptr &&
                                     gpuStrokeType &&
                                     (geometryType == GeometryType::Point ||
                                      selected || scalePreview || rotatePreview ||
                                      layerLineTypePattern(layerLineType).isEmpty()) &&
                                     (geometryType != GeometryType::Arc ||
                                      validateNurbsCurve(visibleShape.nurbs)) &&
                                     (geometryType != GeometryType::Circle ||
                                      validateNurbsCurve(visibleShape.nurbs)) &&
                                     (geometryType != GeometryType::Ellipse ||
                                      validateNurbsCurve(visibleShape.nurbs)) &&
                                     ((geometryType != GeometryType::Bezier &&
                                       geometryType != GeometryType::Nurbs) ||
                                      validateNurbsCurve(visibleShape.nurbs)) &&
                                     (geometryType != GeometryType::PolyCurve ||
                                      !visibleShape.components.isEmpty());
            if (nativeCurve) {
                const bool highlighted = selected || scalePreview || rotatePreview;
                const qreal storedWidth = layerLineWeightMm > 0.0
                                              ? std::clamp(layerLineWeightMm * 6.0,
                                                           1.0, 10.0)
                                              : 2.0;
                if (geometryType == GeometryType::Bezier ||
                    geometryType == GeometryType::Nurbs) {
                    gpuStrokes.append({&visibleShape,
                                       QColor(QStringLiteral("#8aa7c7")),
                                       1.0f, true});
                }
                gpuStrokes.append({&visibleShape,
                                   highlighted ? QColor(QStringLiteral("#5da9e9"))
                                               : layerColor.isValid()
                                                     ? layerColor
                                                     : QColor(QStringLiteral("#d28b45")),
                                   static_cast<float>(highlighted ? 3.5
                                                                  : storedWidth),
                                   false,
                                   geometryType == GeometryType::Point
                                       ? (highlighted ? 10.0f : 9.0f)
                                       : 0.0f});
                continue;
            }
            if (scalePreview) {
                Shape previewShape = shapes_[index];
                scaleShapeGeometry(&previewShape,
                                   scaleBaseWorld_,
                                   scalePreviewAxis_,
                                   scalePreviewFactor_,
                                   scaleMode_);
                drawShape(scenePainter,
                          previewShape,
                          false,
                          true,
                          true,
                          layerColor,
                          layerLineType,
                          layerLineWeightMm);
            } else if (rotatePreview) {
                Shape previewShape = shapes_[index];
                rotateShapeGeometry(&previewShape,
                                    rotateBaseWorld_,
                                    rotatePreviewAngle_);
                drawShape(scenePainter,
                          previewShape,
                          false,
                          true,
                          true,
                          layerColor,
                          layerLineType,
                          layerLineWeightMm);
            } else {
                drawShape(scenePainter,
                          shapes_[index],
                          false,
                          selected,
                          true,
                          layerColor,
                          layerLineType,
                          layerLineWeightMm);
            }
        }
        if (nativeRenderer != nullptr) {
            painter.beginNativePainting();
            const bool sceneDrawn = sceneRenderer == nullptr ||
                                    sceneRenderer->draw(gpuStrokes,
                                                        viewportTransform_, size(),
                                                        devicePixelRatioF());
            const bool gridDrawn = nativeRenderer->renderToCurrentFramebuffer(
                viewportTransform_, size(), devicePixelRatioF(),
                visibleShapes, baseGridStep, gridAppearance_);
            painter.endNativePainting();
            if (!sceneDrawn) {
                for (const ViewportSceneStroke &stroke : gpuStrokes) {
                    if (stroke.shape != nullptr && !stroke.controlGuide) {
                        drawShape(painter, *stroke.shape, false,
                                  stroke.color == QColor(QStringLiteral("#5da9e9")),
                                  true, stroke.color);
                    }
                }
            }
            if (!gridDrawn) {
                drawGrid(painter);
                drawOrigin(painter);
            }
        } else {
            rasterScenePainter.end();
            fillViewportBackground(painter, rect());
            if (gpuViewportBackground.isNull()) {
                drawGrid(painter);
                drawOrigin(painter);
            }
            painter.drawImage(QPoint(0, 0), committedSceneLayer);
            if (!gpuViewportBackground.isNull()) {
                painter.drawImage(QPoint(0, 0), gpuViewportBackground);
            }
        }

        for (int index = 0; index < shapes_.size(); ++index) {
            const ObjectId objectId = shapes_.objectIdAt(index);
            if (!document_.isObjectVisible(objectId)) {
                continue;
            }
            const Shape &shape = shapes_[index];
            if (isDimensionGeometryType(shape.geometryType)) {
                QColor layerColor;
                QString layerLineType;
                qreal layerLineWeightMm = 0.0;
                const SceneObject *sceneObject = document_.object(objectId);
                const Layer *objectLayer = sceneObject == nullptr
                                               ? nullptr
                                               : document_.layer(sceneObject->layerId);
                if (objectLayer != nullptr) {
                    layerColor = objectLayer->color;
                    layerLineType = objectLayer->lineType;
                    layerLineWeightMm = objectLayer->lineWeightMm;
                }
                const bool selected = selectedShapeIndices_.contains(objectId) ||
                                      objectId == selectedShapeIndex_ ||
                                      joinShapeIndices_.contains(objectId);
                drawShape(painter,
                          shape,
                          false,
                          selected,
                          true,
                          layerColor,
                          layerLineType,
                          layerLineWeightMm);
            }
            if (!subdivisionActive_ || objectId != subdivisionShapeIndex_) {
                drawSubdivisionPoints(painter,
                                      shape,
                                      shape.subdivisionParameters,
                                      false);
            }
        }

        if (duplicateActive_) {
            for (const Shape &previewShape : duplicatePreviewShapes_) {
                drawShape(painter, previewShape, true);
            }
        }

        if (isEraseLikeTool(activeTool_) &&
            (!eraseStrokeScreenPath_.isEmpty() || eraseStrokeActive_)) {
            for (const ObjectId objectId : eraseCandidateShapeIndices_) {
                const int shapeIndex = objectIndex(objectId);
                if (shapeIndex >= 0) {
                    drawEraseCandidatePreview(painter, shapeIndex);
                }
            }
        }

        const int subdivisionIndex = objectIndex(subdivisionShapeIndex_);
        if (subdivisionActive_ && subdivisionIndex >= 0) {
            const QVector<double> previewParameters = subdivisionParametersForSections(
                shapes_[subdivisionIndex], subdivisionSections_);
            drawSubdivisionPoints(painter,
                                  shapes_[subdivisionIndex],
                                  previewParameters,
                                  true);
        }

        if (controlPointsVisible_) {
            for (const int shapeIndex : controlPointShapeIndices()) {
                if (shapeIndex >= 0 && shapeIndex < shapes_.size()) {
                    if (activeTool_ == Tool::Scale && scalePreviewValid_ &&
                        scaleShapeIds_.contains(shapes_.objectIdAt(shapeIndex))) {
                        Shape previewShape = shapes_[shapeIndex];
                        scaleShapeGeometry(&previewShape,
                                           scaleBaseWorld_,
                                           scalePreviewAxis_,
                                           scalePreviewFactor_,
                                           scaleMode_);
                        drawControlPoints(painter, previewShape, shapeIndex);
                    } else if (activeTool_ == Tool::Rotate && rotateStep_ == 2 &&
                        rotateShapeIndices_.contains(shapes_.objectIdAt(shapeIndex))) {
                        Shape previewShape = shapes_[shapeIndex];
                        rotateShapeGeometry(&previewShape,
                                            rotateBaseWorld_,
                                            rotatePreviewAngle_);
                        drawControlPoints(painter, previewShape, shapeIndex);
                    } else {
                        drawControlPoints(painter, shapes_[shapeIndex], shapeIndex);
                    }
                }
            }
        }

        if (activeTool_ == Tool::Picture && pendingPoints_.size() == 1 &&
            cursorValid_ && !pendingPictureImage_.isNull()) {
            const Shape picturePreview = pictureShapeForCorner(cursorWorld_);
            if (picturePreview.points.size() == 4) {
                drawShape(painter, picturePreview, true, false, false);
            }
        } else if (activeTool_ == Tool::TangentFromCurve) {
            drawLineToolPreview(painter);
            if (!pendingPoints_.isEmpty()) {
                drawSnapMarker(painter, SnapType::Tangent, pendingPoints_.first());
            }
        } else if (activeTool_ == Tool::PerpendicularFromCurve) {
            drawLineToolPreview(painter);
            if (!pendingPoints_.isEmpty()) {
                drawSnapMarker(painter,
                               SnapType::Perpendicular,
                               pendingPoints_.first());
            }
        } else if (activeTool_ == Tool::Line && lineCommandActive_) {
            drawLineToolPreview(painter);
        } else if (activeTool_ == Tool::Mirror) {
            drawMirrorToolPreview(painter);
            drawLineToolPreview(painter);
        } else if (activeTool_ == Tool::Arc) {
            drawArcToolPreview(painter);
        } else if (isCircleConstructionTool(activeTool_) && !pendingPoints_.isEmpty()) {
            drawCircleToolPreview(painter);
        } else if (isCircleTangentTool(activeTool_)) {
            drawCircleTangentToolPreview(painter);
        } else if (isEllipseTool(activeTool_)) {
            drawEllipseToolPreview(painter);
        } else if (isRectangleTool(activeTool_)) {
            drawRectangleToolPreview(painter);
        } else if (isPolygonTool(activeTool_)) {
            drawPolygonToolPreview(painter);
        } else if (activeTool_ == Tool::Point) {
            drawPointToolPreview(painter);
        } else if (activeTool_ == Tool::Rotate) {
            drawRotateToolPreview(painter);
        } else if (activeTool_ == Tool::Scale && scaleStep_ == 2) {
            drawScaleToolGuide(painter);
        } else if (isDimensionTool(activeTool_)) {
            if (controllerPreviewShapeVisible_) {
                drawShape(painter, controllerPreviewShape_, true);
            }
            if (currentSnap_.isValid()) {
                drawSnapMarker(painter, currentSnap_.type, currentSnap_.point);
            }
        } else if (isEraseLikeTool(activeTool_)) {
            drawErasePreview(painter);
        } else if (!pendingPoints_.isEmpty()) {
            Shape previewShape{geometryTypeForTool(activeTool_),
                               pendingPoints_,
                               Shape::NurbsCurve2D{},
                               ArcMode::TwoPoint,
                               0.0,
                               {},
                               {}};
            previewShape.workPlane = viewportTransform_.workPlane();
            previewShape.workPlaneOffset = viewportTransform_.workPlaneOffset();
            drawShape(painter, previewShape, true);
        }

        if ((grabActive_ || duplicateActive_ || activeTool_ == Tool::Scale ||
             activeTool_ == Tool::Picture) &&
            currentSnap_.isValid()) {
            drawSnapMarker(painter, currentSnap_.type, currentSnap_.point);
        }
        if ((draggingSelected_ || draggingControlPoint_) && currentDragSnap_.isValid()) {
            drawSnapMarker(painter,
                           currentDragSnap_.type,
                           currentDragSnap_.targetPoint);
        }

        if (selectionBoxActive_) {
            viewportOverlay_.drawSelectionBox(painter,
                                               selectionBoxStartScreen_,
                                               selectionBoxCurrentScreen_);
        }
        viewportOverlay_.drawToolStatus(painter,
                                         size(),
                                         activeTool_,
                                         arcMode_,
                                         subdivisionActive_,
                                         subdivisionSections_,
                                         joinActive_,
                                         joinShapeIndices_.size(),
                                         lineCommandActive_,
                                         rotateStep_,
                                         grabActive_,
                                         grabPickingBasePoint_,
                                         grabHasBasePoint_,
                                         duplicateActive_,
                                         duplicatePickingBasePoint_,
                                         duplicateHasBasePoint_);
        viewportOverlay_.drawBlenderNavigationGizmo(
            painter, size(), navigationHoverPosition_);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        stopNavigationAnimation();
        const QPointF screenPosition = eventPosition(event);
        if (event->button() == Qt::LeftButton) {
            const BlenderNavigationHit hit =
                viewportOverlay_.blenderNavigationGizmoHitAt(screenPosition, size());
            if (hit.action != BlenderNavigationAction::None) {
                navigationPressedAction_ = hit.action;
                navigationPressHit_ = hit;
                navigationPressPosition_ = screenPosition;
                navigationLastPosition_ = screenPosition;
                navigationMoved_ = false;
                if (hit.action == BlenderNavigationAction::Orbit ||
                    hit.action == BlenderNavigationAction::Axis) {
                    beginOrbitAt(screenPosition);
                    if (viewportTransform_.navigationPreferences().orbitMethod ==
                        ViewportOrbitMethod::Trackball) {
                        viewportTransform_.beginOrbitGesture(screenPosition, size());
                    }
                }
                setCursor(hit.action == BlenderNavigationAction::Camera ||
                                  hit.action == BlenderNavigationAction::Projection
                              ? Qt::PointingHandCursor
                              : Qt::OpenHandCursor);
                update();
                event->accept();
                return;
            }
        }
        QPointF rawWorldPosition;
        const bool worldPositionValid = viewportTransform_.screenToWorkPlane(
            screenPosition,
            size(),
            viewportTransform_.workPlane(),
            viewportTransform_.workPlaneOffset(),
            &rawWorldPosition);
        if (!worldPositionValid) {
            rawWorldPosition = {};
        }
        const QPointF worldPosition = constrainLinePoint(rawWorldPosition);
        DebugLog::instance().write(
            QStringLiteral("mousePress button=%1 screen=%2 worldRaw=%3 worldUsed=%4 tool=%5 lineActive=%6 ortho=%7 panButton=%8 modifiers=0x%9 snap=%10")
                .arg(inputButtonName(event->button()))
                .arg(pointText(screenPosition))
                .arg(pointText(rawWorldPosition))
                .arg(pointText(worldPosition))
                .arg(toolName(activeTool_))
                .arg(lineCommandActive_)
                .arg(orthoEnabled_)
                .arg(inputButtonName(panButton_))
                .arg(static_cast<int>(event->modifiers()), 0, 16)
                .arg(snapTypeName(currentSnap_.type)));

        if (!worldPositionValid && event->button() == Qt::LeftButton &&
            event->button() != panButton_) {
            event->ignore();
            return;
        }

        if (duplicateActive_) {
            rawCursorWorld_ = rawWorldPosition;
            cursorWorld_ = rawWorldPosition;
            lastWorldPosition_ = rawWorldPosition;
            cursorValid_ = true;
            if (event->button() == Qt::LeftButton) {
                if (duplicatePickingBasePoint_) {
                    const SnapResult baseSnap = findDuplicateBasePointSnap(rawWorldPosition);
                    duplicateBasePoint_ = baseSnap.isValid()
                                              ? baseSnap.point
                                              : rawWorldPosition;
                    duplicateCursorOffset_ = rawWorldPosition - duplicateBasePoint_;
                    duplicateHasBasePoint_ = true;
                    duplicatePickingBasePoint_ = false;
                    currentSnap_ = baseSnap;
                    setCursor(Qt::SizeAllCursor);
                    updateDuplicatePreview(rawWorldPosition);
                } else {
                    updateDuplicatePreview(rawWorldPosition);
                    finishDuplicate();
                }
                update();
                emitCoordinateUpdate();
            } else if (event->button() == Qt::RightButton) {
                cancelDuplicate();
            }
            return;
        }

        if (grabActive_) {
            rawCursorWorld_ = rawWorldPosition;
            cursorWorld_ = rawWorldPosition;
            lastWorldPosition_ = rawWorldPosition;
            cursorValid_ = true;
            if (event->button() == Qt::LeftButton) {
                if (grabPickingBasePoint_) {
                    const SnapResult baseSnap = findGrabBasePointSnap(rawWorldPosition);
                    if (baseSnap.isValid()) {
                        grabBasePoint_ = baseSnap.point;
                        grabCursorOffset_ = rawWorldPosition - grabBasePoint_;
                        grabHasBasePoint_ = true;
                        grabPickingBasePoint_ = false;
                        currentSnap_ = baseSnap;
                        setCursor(Qt::SizeAllCursor);
                        DebugLog::instance().write(
                            QStringLiteral("grab base-point selected type=%1 point=%2 offset=%3")
                                .arg(snapTypeName(baseSnap.type))
                                .arg(pointText(grabBasePoint_))
                                .arg(pointText(grabCursorOffset_)));
                    } else {
                        DebugLog::instance().write(
                            QStringLiteral("grab base-point click ignored no snap candidate"));
                    }
                    update();
                    emitCoordinateUpdate();
                } else {
                    if (grabHasBasePoint_) {
                        updateGrabPosition(draggingShapeIndices_);
                    }
                    finishGrab();
                }
            } else if (event->button() == Qt::RightButton) {
                cancelGrab();
            }
            return;
        }

        if (subdivisionActive_) {
            if (event->button() == Qt::LeftButton) {
                applySubdivision(subdivisionSections_);
            } else if (event->button() == Qt::RightButton) {
                cancelSubdivisionPreview();
            }
            return;
        }

        // While drawing a connected line, right-click is the command's
        // finish action. This takes priority over right-button panning.
        if (event->button() == Qt::RightButton && activeTool_ == Tool::Line &&
            lineCommandActive_) {
            DebugLog::instance().write(QStringLiteral("mousePress branch=finish-line points=%1")
                                           .arg(pendingPoints_.size()));
            const ToolInput input = makeToolInput(event,
                                                  screenPosition,
                                                  rawWorldPosition,
                                                  worldPosition);
            if (activeToolController_ == nullptr ||
                !activeToolController_->handleMousePress(input, toolContext_)) {
                finishLineCommand();
            }
            return;
        }

        if (event->button() == Qt::RightButton && isEraseLikeTool(activeTool_)) {
            exitEraseLikeTool();
            return;
        }

        if (event->button() == Qt::RightButton && activeTool_ == Tool::Rotate) {
            cancelRotate();
            return;
        }

        if (event->button() == Qt::RightButton && activeTool_ == Tool::Scale) {
            cancelScale();
            return;
        }

        if (event->button() == Qt::RightButton && activeTool_ == Tool::Mirror) {
            cancelMirror();
            return;
        }

        if (event->button() == Qt::RightButton && activeTool_ == Tool::Picture) {
            cancelPicturePlacement();
            return;
        }

        const bool middleMouseNavigation = event->button() == Qt::MiddleButton;
        const bool configuredPanButton = event->button() == panButton_;
        const bool altLeftNavigation =
            event->button() == Qt::LeftButton &&
            event->modifiers().testFlag(Qt::AltModifier);
        if (middleMouseNavigation || configuredPanButton || altLeftNavigation) {
            panning_ = true;
            if (middleMouseNavigation) {
                // Blender-style navigation: MMB orbits; Shift+MMB pans.
                orbiting_ = !event->modifiers().testFlag(Qt::ShiftModifier);
            } else {
                orbiting_ = configuredPanButton &&
                            event->modifiers().testFlag(Qt::ShiftModifier);
            }
            if (orbiting_) {
                beginOrbitAt(screenPosition);
                if (viewportTransform_.navigationPreferences().orbitMethod ==
                    ViewportOrbitMethod::Trackball) {
                    viewportTransform_.beginOrbitGesture(screenPosition, size());
                }
            }
            panMoved_ = false;
            panStartPosition_ = screenPosition.toPoint();
            lastMousePosition_ = screenPosition.toPoint();
            DebugLog::instance().write(
                QStringLiteral("mousePress branch=start-%1 at=%2")
                    .arg(orbiting_ ? QStringLiteral("orbit") : QStringLiteral("pan"))
                    .arg(pointText(screenPosition)));
            setCursor(Qt::ClosedHandCursor);
            return;
        }

        if (event->button() == Qt::LeftButton && activeToolController_ != nullptr &&
            activeTool_ != Tool::Select && activeTool_ != Tool::Arc) {
            rawCursorWorld_ = rawWorldPosition;
            lastWorldPosition_ = worldPosition;
            cursorWorld_ = worldPosition;
            cursorValid_ = true;
            const ToolInput input = makeToolInput(event,
                                                  screenPosition,
                                                  rawWorldPosition,
                                                  worldPosition);
            if (activeToolController_->handleMousePress(input, toolContext_)) {
                update();
                emitCoordinateUpdate();
                return;
            }
        }

        if (event->button() == Qt::LeftButton && activeTool_ == Tool::Rotate) {
            rawCursorWorld_ = rawWorldPosition;
            cursorWorld_ = worldPosition;
            lastWorldPosition_ = worldPosition;
            cursorValid_ = true;
            handleRotatePoint(worldPosition);
            emitCoordinateUpdate();
            return;
        }

        if (event->button() == Qt::LeftButton && activeTool_ == Tool::Scale) {
            rawCursorWorld_ = rawWorldPosition;
            cursorWorld_ = worldPosition;
            lastWorldPosition_ = worldPosition;
            cursorValid_ = true;
            handleScalePoint(worldPosition);
            emitCoordinateUpdate();
            return;
        }

        if (event->button() == Qt::LeftButton && activeTool_ == Tool::Mirror) {
            rawCursorWorld_ = rawWorldPosition;
            cursorWorld_ = worldPosition;
            lastWorldPosition_ = worldPosition;
            cursorValid_ = true;
            handleMirrorPoint(worldPosition);
            emitCoordinateUpdate();
            return;
        }

        if (joinActive_ && event->button() == Qt::LeftButton) {
            const int shapeIndex = hitTestShape(screenPosition);
            const ObjectId objectId = shapes_.objectIdAt(shapeIndex);
            if (shapeIndex < 0 || !objectId.isValid() || !isJoinableShape(shapes_[shapeIndex])) {
                notifyJoinStatus(QStringLiteral("Join: click a line or curve"));
                DebugLog::instance().write(QStringLiteral("join click ignored shape=%1")
                                               .arg(shapeIndex));
                return;
            }

            if (joinShapeIndices_.contains(objectId)) {
                notifyJoinStatus(QStringLiteral("Join: curve already selected"));
                return;
            }

            joinShapeIndices_.append(objectId);
            if (!selectedShapeIndices_.contains(objectId)) {
                selectedShapeIndices_.append(objectId);
            }
            selectedShapeIndex_ = objectId;
            notifyJoinStatus();
            DebugLog::instance().write(QStringLiteral("join selected shape=%1 total=%2")
                                           .arg(shapeIndex)
                                           .arg(joinShapeIndices_.size()));
            update();
            return;
        }

        if (event->button() == Qt::LeftButton && activeTool_ == Tool::Erase) {
            prepareEraseGeometryCache();
            if (eraseTargetShapeIndices_.isEmpty()) {
                DebugLog::instance().write(
                    QStringLiteral("erase stroke ignored no selected curve targets"));
                update();
                return;
            }

            eraseCursorPressed_ = true;
            rawCursorWorld_ = rawWorldPosition;
            cursorWorld_ = rawWorldPosition;
            lastWorldPosition_ = rawWorldPosition;
            cursorValid_ = true;
            eraseCursorScreen_ = screenPosition;
            lastEraseScreen_ = screenPosition;
            eraseStrokeScreenPath_.clear();
            eraseStrokeScreenPath_.append(screenPosition);
            eraseCandidateShapeIndices_.clear();
            eraseStrokeActive_ = true;
            eraseAlongScreenSegment(screenPosition, screenPosition);
            updateErasePreviewIntervals(false);
            setCursor(Qt::CrossCursor);
            DebugLog::instance().write(QStringLiteral("erase stroke start screen=%1")
                                           .arg(pointText(screenPosition)));
            update();
            emitCoordinateUpdate();
            return;
        }

        if (event->button() == Qt::LeftButton && activeTool_ == Tool::Trim) {
            eraseCursorPressed_ = false;
            rawCursorWorld_ = rawWorldPosition;
            cursorWorld_ = rawWorldPosition;
            lastWorldPosition_ = rawWorldPosition;
            cursorValid_ = true;
            eraseCursorScreen_ = screenPosition;
            eraseCandidateShapeIndices_.clear();
            eraseStrokeScreenPath_.clear();
            trimHoverPositionValid_ = false;
            trimHoverComponentIndex_ = -1;
            beginSelectionBox(screenPosition, false);
            trimBoxSelectionActive_ = true;
            return;
        }

        if (event->button() == Qt::LeftButton && activeTool_ == Tool::Select) {
            rawCursorWorld_ = rawWorldPosition;
            cursorWorld_ = rawWorldPosition;
            lastWorldPosition_ = rawWorldPosition;
            cursorValid_ = true;
            dragGestureStarted_ = false;
            dragStartScreen_ = screenPosition;
            const bool shiftPressed = event->modifiers().testFlag(Qt::ShiftModifier);

            if (!shiftPressed && controlPointsVisible_) {
                int grabbedShapeIndex = -1;
                int grabbedControlPoint = -1;
                if (hitTestSelectedControlPoint(screenPosition,
                                                &grabbedShapeIndex,
                                                &grabbedControlPoint)) {
                    selectedShapeIndex_ = shapes_.objectIdAt(grabbedShapeIndex);
                    draggingControlPoint_ = true;
                    draggingSelected_ = false;
                    selection_.setActiveControlPoint(selectedShapeIndex_, grabbedControlPoint);
                    lastControlPointWorld_ = rawWorldPosition;
                    dragHistoryRecorded_ = false;
                    currentDragSnap_ = DragSnapResult{};
                    dragSnapLocked_ = false;
                    dragAxisLock_ = DragAxisLock::None;
                    setCursor(Qt::SizeAllCursor);
                    DebugLog::instance().write(
                        QStringLiteral("control point drag start shape=%1 index=%2 world=%3")
                            .arg(objectIndex(selectedShapeIndex_))
                            .arg(controlPointIndex_)
                            .arg(pointText(lastControlPointWorld_)));
                    update();
                    emitCoordinateUpdate();
                    return;
                }
            }

            const int clickedShapeIndex = hitTestShape(screenPosition);
            draggingControlPoint_ = false;
            controlPointIndex_ = -1;
            dragHistoryRecorded_ = false;
            currentDragSnap_ = DragSnapResult{};
            dragSnapLocked_ = false;
            dragAxisLock_ = DragAxisLock::None;
            draggingSelected_ = false;

            if (shiftPressed) {
                if (clickedShapeIndex >= 0) {
                    toggleShapeSelection(clickedShapeIndex);
                    DebugLog::instance().write(
                        QStringLiteral("shift selection toggle shape=%1 selected=%2")
                            .arg(clickedShapeIndex)
                            .arg(selectedShapeIndices_.size()));
                    update();
                    emitCoordinateUpdate();
                } else {
                    beginSelectionBox(screenPosition, true);
                }
                return;
            }

            if (clickedShapeIndex >= 0) {
                const ObjectId clickedObjectId = shapes_.objectIdAt(clickedShapeIndex);
                if (!selectedShapeIndices_.contains(clickedObjectId)) {
                    setSingleSelection(clickedShapeIndex);
                } else {
                    // Clicking an already-selected shape starts a group drag
                    // without collapsing the current multi-selection.
                    selectedShapeIndex_ = clickedObjectId;
                }
                draggingShapeIndices_ = selectedShapeIndices_;
                draggingSelected_ = true;
                setFocus(Qt::MouseFocusReason);
                lastDragWorld_ = rawWorldPosition;
                setCursor(Qt::SizeAllCursor);
                DebugLog::instance().write(
                    QStringLiteral("selection hit shape=%1 tool=%2 dragStart=%3")
                        .arg(objectIndex(selectedShapeIndex_))
                        .arg(geometryTypeName(shapes_[objectIndex(selectedShapeIndex_)].geometryType))
                        .arg(pointText(lastDragWorld_)));
            } else {
                draggingSelected_ = false;
                DebugLog::instance().write(QStringLiteral("selection miss at=%1")
                                               .arg(pointText(screenPosition)));
                beginSelectionBox(screenPosition, false);
            }

            update();
            emitCoordinateUpdate();
            return;
        }

        if (event->button() != Qt::LeftButton || activeTool_ == Tool::Select) {
            DebugLog::instance().write(QStringLiteral("mousePress branch=ignored"));
            return;
        }

        rawCursorWorld_ = rawWorldPosition;
        lastWorldPosition_ = worldPosition;
        cursorWorld_ = worldPosition;
        cursorValid_ = true;

        if (activeTool_ == Tool::Picture) {
            if (pendingPictureImage_.isNull()) {
                cancelPicturePlacement();
                return;
            }
            if (pendingPoints_.isEmpty()) {
                pendingPoints_.append(worldPosition);
                DebugLog::instance().write(
                    QStringLiteral("picture first corner=%1 image=%2x%3")
                        .arg(pointText(worldPosition))
                        .arg(pendingPictureImage_.width())
                        .arg(pendingPictureImage_.height()));
            } else {
                Shape picture = pictureShapeForCorner(worldPosition);
                if (picture.points.size() == 4 &&
                    std::hypot(picture.points[1].x() - picture.points[0].x(),
                               picture.points[1].y() - picture.points[0].y()) > 1.0e-9 &&
                    std::hypot(picture.points[2].x() - picture.points[1].x(),
                               picture.points[2].y() - picture.points[1].y()) > 1.0e-9) {
                    recordGeometryChange();
                    shapes_.append(picture);
                    DebugLog::instance().write(
                        QStringLiteral("picture committed image=%1 frameCorners=%2")
                            .arg(pendingPicturePath_)
                            .arg(picture.points.size()));
                    setTool(Tool::Select);
                    if (commandFinished_) {
                        commandFinished_(Tool::Select);
                    }
                }
            }
            update();
            emitCoordinateUpdate();
            return;
        }

        if (activeTool_ == Tool::Line) {
            pendingPoints_.append(lastWorldPosition_);
            DebugLog::instance().write(QStringLiteral("line point planted index=%1 world=%2 total=%3")
                                           .arg(pendingPoints_.size() - 1)
                                           .arg(pointText(lastWorldPosition_))
                                           .arg(pendingPoints_.size()));
            update();
            return;
        }

        if (activeTool_ == Tool::Arc && arcMode_ == ArcMode::OnePoint &&
            pendingPoints_.size() == 2) {
            // A click can arrive without a final mouse-move event. Include
            // that last position before saving the arc's unwrapped sweep.
            updateArcPreviewTracking(lastWorldPosition_);
        }

        pendingPoints_.append(lastWorldPosition_);

        if (activeTool_ == Tool::Arc && arcMode_ == ArcMode::OnePoint &&
            pendingPoints_.size() == 2) {
            initializeArcPreviewTracking();
        }

        if (pendingPoints_.size() == requiredPoints(activeTool_)) {
            recordGeometryChange();
            const ArcMode completedArcMode = activeTool_ == Tool::Arc
                                                 ? arcMode_
                                                 : ArcMode::TwoPoint;
            Shape completedShape{geometryTypeForTool(activeTool_),
                                 pendingPoints_,
                                 Shape::NurbsCurve2D{},
                                 completedArcMode,
                                 0.0,
                                 {},
                                 {}};
            completedShape.workPlane = viewportTransform_.workPlane();
            completedShape.workPlaneOffset = viewportTransform_.workPlaneOffset();
            if (isRectangleTool(activeTool_)) {
                completedShape.points = makeRectanglePoints(
                    rectangleModeForTool(activeTool_), pendingPoints_);
            } else if (isPolygonTool(activeTool_)) {
                completedShape.points = makeRegularPolygonPoints(
                    polygonModeForTool(activeTool_), pendingPoints_, polygonSideCount_);
            }
            if (activeTool_ == Tool::Arc && arcMode_ == ArcMode::OnePoint) {
                completedShape.arcSweep = arcPreviewSweepAngle_;
            }
            if (activeTool_ == Tool::Arc) {
                completedShape.nurbs = makeArcNurbsCurve(completedShape);
            } else if (activeTool_ == Tool::Bezier || activeTool_ == Tool::Nurbs) {
                completedShape.nurbs = makeBezierNurbs(completedShape.points);
            } else if (activeTool_ == Tool::Circle) {
                completedShape.nurbs = makeCircleNurbs(completedShape.points);
            }
            shapes_.append(completedShape);
            QString commitMessage = QStringLiteral("placeholder shape committed tool=%1 points=%2")
                                        .arg(toolName(activeTool_))
                                        .arg(pendingPoints_.size());
            if (activeTool_ == Tool::Arc) {
                commitMessage += QStringLiteral(" mode=%1 p0=%2 p1=%3 p2=%4 sweep=%5")
                                     .arg(arcModeName(completedShape.arcMode))
                                     .arg(pointText(completedShape.points[0]))
                                     .arg(pointText(completedShape.points[1]))
                                     .arg(pointText(completedShape.points[2]))
                                     .arg(completedShape.arcSweep, 0, 'f', 4);
                commitMessage += QStringLiteral(" nurbsDimension=%1 nurbsDegree=%2 nurbsOrder=%3 rational=%4 controlPoints=%5 weights=%6 knots=%7")
                                     .arg(completedShape.nurbs.dimension)
                                     .arg(completedShape.nurbs.degree)
                                     .arg(completedShape.nurbs.order)
                                     .arg(completedShape.nurbs.rational)
                                     .arg(completedShape.nurbs.controlPoints.size())
                                     .arg(completedShape.nurbs.weights.size())
                                     .arg(completedShape.nurbs.knots.size());
            } else if (activeTool_ == Tool::Bezier || activeTool_ == Tool::Nurbs ||
                       activeTool_ == Tool::Circle) {
                commitMessage += QStringLiteral(" nurbsDimension=%1 nurbsDegree=%2 nurbsOrder=%3 rational=%4 controlPoints=%5 weights=%6 knots=%7")
                                     .arg(completedShape.nurbs.dimension)
                                     .arg(completedShape.nurbs.degree)
                                     .arg(completedShape.nurbs.order)
                                     .arg(completedShape.nurbs.rational)
                                     .arg(completedShape.nurbs.controlPoints.size())
                                     .arg(completedShape.nurbs.weights.size())
                                     .arg(completedShape.nurbs.knots.size());
            }
            commitMessage += QStringLiteral(" shapes=%1").arg(shapes_.size());
            DebugLog::instance().write(commitMessage);
            pendingPoints_.clear();

            setTool(Tool::Select);
            if (commandFinished_) {
                commandFinished_(Tool::Select);
            }
        }

        update();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const QPointF screenPosition = eventPosition(event);
        navigationHoverPosition_ = screenPosition;
        if (navigationPressedAction_ != BlenderNavigationAction::None &&
            (event->buttons() & Qt::LeftButton) != 0) {
            const QPointF delta = screenPosition - navigationLastPosition_;
            if (QLineF(navigationPressPosition_, screenPosition).length() >= 3.0) {
                navigationMoved_ = true;
            }
            if (navigationMoved_) {
                setCursor(Qt::ClosedHandCursor);
                const ViewportViewPreset previousPreset = viewportTransform_.viewPreset();
                switch (navigationPressedAction_) {
                case BlenderNavigationAction::Orbit:
                case BlenderNavigationAction::Axis:
                    if (viewportTransform_.navigationPreferences().orbitMethod ==
                        ViewportOrbitMethod::Trackball) {
                        viewportTransform_.orbitToPosition(screenPosition, size());
                    } else {
                        viewportTransform_.orbitByPixels(delta);
                    }
                    break;
                case BlenderNavigationAction::Zoom: {
                    const ViewportNavigationPreferences preferences =
                        viewportTransform_.navigationPreferences();
                    qreal zoomDelta = preferences.zoomAxis == ViewportZoomAxis::Vertical
                                          ? delta.y()
                                          : delta.x();
                    if (preferences.invertMouseZoom) {
                        zoomDelta = -zoomDelta;
                    }
                    const qreal factor = std::exp(-zoomDelta * 0.012);
                    viewportTransform_.zoomAt(QPointF(width() * 0.5,
                                                      height() * 0.5),
                                              factor,
                                              size(),
                                              0.15,
                                              12.0);
                    break;
                }
                case BlenderNavigationAction::Pan:
                    viewportTransform_.panByPixels(delta, size());
                    break;
                case BlenderNavigationAction::Camera:
                case BlenderNavigationAction::Projection:
                case BlenderNavigationAction::None:
                    break;
                }
                if (previousPreset != viewportTransform_.viewPreset()) {
                    notifyViewStateChanged();
                }
                update();
                emitCoordinateUpdate();
            }
            navigationLastPosition_ = screenPosition;
            update();
            event->accept();
            return;
        }
        if (panning_) {
            const QPoint current = screenPosition.toPoint();
            const QPoint delta = current - lastMousePosition_;
            const QPoint totalDelta = current - panStartPosition_;
            if (std::hypot(totalDelta.x(), totalDelta.y()) >= 3.0) {
                panMoved_ = true;
            }
            if (orbiting_) {
                const ViewportViewPreset previousPreset = viewportTransform_.viewPreset();
                if (viewportTransform_.navigationPreferences().orbitMethod ==
                    ViewportOrbitMethod::Trackball) {
                    viewportTransform_.orbitToPosition(screenPosition, size());
                } else {
                    viewportTransform_.orbitByPixels(QPointF(delta));
                }
                if (previousPreset != viewportTransform_.viewPreset()) {
                    notifyViewStateChanged();
                }
            } else {
                viewportTransform_.panByPixels(QPointF(delta), size());
            }
            lastMousePosition_ = current;
            update();
            emitCoordinateUpdate();
            return;
        }
        const BlenderNavigationHit navigationHit =
            viewportOverlay_.blenderNavigationGizmoHitAt(screenPosition, size());
        if (navigationHit.action != BlenderNavigationAction::None) {
            setToolTip(navigationTooltip(navigationHit));
            setCursor(navigationHit.action == BlenderNavigationAction::Camera ||
                              navigationHit.action == BlenderNavigationAction::Projection
                          ? Qt::PointingHandCursor
                          : Qt::OpenHandCursor);
            update();
            return;
        }
        setToolTip(QString());
        if (!grabActive_ && !duplicateActive_) {
            setCursor(activeTool_ == Tool::Select ? Qt::ArrowCursor : Qt::CrossCursor);
        }
        eraseCursorScreen_ = screenPosition;
        if (!viewportTransform_.screenToWorkPlane(screenPosition,
                                                  size(),
                                                  viewportTransform_.workPlane(),
                                                  viewportTransform_.workPlaneOffset(),
                                                  &rawCursorWorld_)) {
            cursorValid_ = false;
            currentSnap_ = SnapResult{};
            update();
            emitCoordinateUpdate();
            return;
        }
        cursorWorld_ = constrainLinePoint(rawCursorWorld_);
        lastWorldPosition_ = cursorWorld_;
        cursorValid_ = true;
        if (activeToolController_ != nullptr) {
            const ToolInput input = makeToolInput(event,
                                                  screenPosition,
                                                  rawCursorWorld_,
                                                  cursorWorld_);
            activeToolController_->handleMouseMove(input, toolContext_);
        }
        const bool pointPreviewActive = activeTool_ == Tool::Point;
        const bool picturePreviewActive = activeTool_ == Tool::Picture &&
                                          pendingPoints_.size() == 1;
        const bool circlePreviewActive = isCircleConstructionTool(activeTool_) &&
                                         !pendingPoints_.isEmpty();
        const bool tangentCirclePreviewActive = isCircleTangentTool(activeTool_);
        const bool ellipsePreviewActive = isEllipseTool(activeTool_);
        const bool rectanglePreviewActive = isRectangleTool(activeTool_);
        const bool polygonPreviewActive = isPolygonTool(activeTool_);
        const bool arcPreviewActive = activeTool_ == Tool::Arc;
        const bool mirrorPreviewActive = activeTool_ == Tool::Mirror;

        if (!panning_ && activeTool_ == Tool::Arc && arcMode_ == ArcMode::OnePoint &&
            pendingPoints_.size() >= 2) {
            updateArcPreviewTracking(cursorWorld_);
        }

        if (selectionBoxActive_) {
            selectionBoxCurrentScreen_ = screenPosition;
            const QPointF totalDelta = screenPosition - selectionBoxStartScreen_;
            if (std::hypot(totalDelta.x(), totalDelta.y()) >= 3.0) {
                selectionBoxMoved_ = true;
            }
            if (trimBoxSelectionActive_) {
                updateTrimBoxPreview();
            }
            update();
            emitCoordinateUpdate();
            return;
        }

        if (eraseStrokeActive_) {
            eraseAlongScreenSegment(lastEraseScreen_, screenPosition);
            lastEraseScreen_ = screenPosition;
            if (eraseStrokeScreenPath_.isEmpty() ||
                eraseStrokeScreenPath_.back() != screenPosition) {
                eraseStrokeScreenPath_.append(screenPosition);
            }
            updateErasePreviewIntervals(false);
            update();
            emitCoordinateUpdate();
            return;
        }

        if (activeTool_ == Tool::Trim && !panning_) {
            updateTrimHover(screenPosition);
            update();
            emitCoordinateUpdate();
            return;
        }

        if (activeTool_ == Tool::Scale) {
            updateScalePreview(cursorWorld_);
            update();
            emitCoordinateUpdate();
            return;
        }

        if (activeTool_ == Tool::Rotate) {
            if (!panning_ && rotateStep_ == 2) {
                rotatePreviewAngle_ = rotationAngleForPoint(cursorWorld_);
            }
            update();
            emitCoordinateUpdate();
            return;
        }

        if (activeTool_ == Tool::Mirror) {
            update();
            emitCoordinateUpdate();
            return;
        }

        if (duplicateActive_) {
            if (duplicatePickingBasePoint_) {
                currentSnap_ = findDuplicateBasePointSnap(rawCursorWorld_);
                cursorWorld_ = currentSnap_.isValid()
                                   ? currentSnap_.point
                                   : rawCursorWorld_;
                lastWorldPosition_ = cursorWorld_;
                update();
                emitCoordinateUpdate();
            } else {
                updateDuplicatePreview(rawCursorWorld_);
                emitCoordinateUpdate();
            }
            return;
        }

        if (grabActive_ && grabPickingBasePoint_) {
            currentSnap_ = findGrabBasePointSnap(rawCursorWorld_);
            cursorWorld_ = currentSnap_.isValid() ? currentSnap_.point : rawCursorWorld_;
            lastWorldPosition_ = cursorWorld_;
            update();
            emitCoordinateUpdate();
            return;
        }

        if ((draggingSelected_ || draggingControlPoint_) && !dragGestureStarted_) {
            constexpr qreal dragStartThresholdPixels = 4.0;
            const QPointF screenDelta = screenPosition - dragStartScreen_;
            if (std::hypot(screenDelta.x(), screenDelta.y()) >= dragStartThresholdPixels) {
                dragGestureStarted_ = true;
            }
        }

        const int selectedIndex = objectIndex(selectedShapeIndex_);
        if (draggingControlPoint_ && dragGestureStarted_ && selectedIndex >= 0 &&
            controlPointIndex_ >= 0) {
            const QPointF previousControlPointCursorWorld = lastControlPointWorld_;
            const QPointF cursorStepScreen =
                screenPosition - worldToScreen(previousControlPointCursorWorld);
            const QPointF delta = rawCursorWorld_ - lastControlPointWorld_;
            if (!qFuzzyIsNull(delta.x()) || !qFuzzyIsNull(delta.y())) {
                qint64 snapEvaluationMicroseconds = -1;
                constexpr qreal dragSnapBreakawayPixels = 18.0;
                const qreal cursorDistanceFromSnap =
                    std::hypot(screenPosition.x() - worldToScreen(dragSnapCursorWorld_).x(),
                               screenPosition.y() - worldToScreen(dragSnapCursorWorld_).y());

                if (dragSnapLocked_ && cursorDistanceFromSnap <= dragSnapBreakawayPixels) {
                    DebugLog::instance().write(
                        QStringLiteral("control point snap-hold shape=%1 index=%2 cursorDistance=%3 breakaway=%4")
                            .arg(selectedIndex)
                            .arg(controlPointIndex_)
                            .arg(cursorDistanceFromSnap, 0, 'f', 2)
                            .arg(dragSnapBreakawayPixels, 0, 'f', 2));
                } else if (dragSnapLocked_) {
                    const QPointF detachDelta = rawCursorWorld_ - dragSnapCursorWorld_;
                    beginDragHistory();
                    translateControlPoint(selectedShapeIndex_, controlPointIndex_, detachDelta);
                    currentDragSnap_ = DragSnapResult{};
                    dragSnapLocked_ = false;
                    DebugLog::instance().write(
                        QStringLiteral("control point snap-breakaway shape=%1 index=%2 cursorDistance=%3")
                            .arg(selectedIndex)
                            .arg(controlPointIndex_)
                            .arg(cursorDistanceFromSnap, 0, 'f', 2));
                } else {
                    beginDragHistory();
                    translateControlPoint(selectedShapeIndex_, controlPointIndex_, delta);

                    const QVector<QPointF> controlPoints =
                        controlPointsForShape(shapes_[selectedIndex]);
                    if (controlPointIndex_ < controlPoints.size()) {
                        QElapsedTimer snapTimer;
                        snapTimer.start();
                        currentDragSnap_ = findControlPointSnap(
                            selectedShapeIndex_,
                            controlPointIndex_,
                            controlPoints[controlPointIndex_]);
                        snapEvaluationMicroseconds = snapTimer.nsecsElapsed() / 1000;
                        if (currentDragSnap_.isValid()) {
                            translateControlPoint(selectedShapeIndex_,
                                                  controlPointIndex_,
                                                  currentDragSnap_.translation);
                            dragSnapLocked_ = true;
                            dragSnapCursorWorld_ = rawCursorWorld_;
                            DebugLog::instance().write(
                                QStringLiteral("control point snapped shape=%1 index=%2 type=%3 target=%4")
                                    .arg(selectedIndex)
                                    .arg(controlPointIndex_)
                                    .arg(snapTypeName(currentDragSnap_.type))
                                    .arg(pointText(currentDragSnap_.targetPoint)));
                        }
                    }
                }

                lastControlPointWorld_ = rawCursorWorld_;
                qreal snapCorrectionPixels = -1.0;
                if (currentDragSnap_.isValid()) {
                    const QPointF sourceScreen =
                        worldToScreen(currentDragSnap_.sourcePoint);
                    const QPointF targetScreen =
                        worldToScreen(currentDragSnap_.targetPoint);
                    snapCorrectionPixels =
                        std::hypot(targetScreen.x() - sourceScreen.x(),
                                   targetScreen.y() - sourceScreen.y());
                }
                const quint64 traceSequence = ++dragSnapTraceSequence_;
                DebugLog::instance().write(
                    QStringLiteral("drag-snap-trace seq=%1 mode=control-point shape=%2 index=%3 cursorScreen=%4 cursorStepPx=%5 cursorWorld=%6 worldDelta=%7 snapEvalUs=%8 locked=%9 snap=%10 snapSource=%11 snapTarget=%12 snapTranslation=%13 snapCorrectionPx=%14")
                        .arg(traceSequence)
                        .arg(selectedIndex)
                        .arg(controlPointIndex_)
                        .arg(precisePointText(screenPosition))
                        .arg(precisePointText(cursorStepScreen))
                        .arg(precisePointText(rawCursorWorld_))
                        .arg(precisePointText(delta))
                        .arg(snapEvaluationMicroseconds)
                        .arg(dragSnapLocked_)
                        .arg(snapTypeName(currentDragSnap_.type))
                        .arg(precisePointText(currentDragSnap_.sourcePoint))
                        .arg(precisePointText(currentDragSnap_.targetPoint))
                        .arg(precisePointText(currentDragSnap_.translation))
                        .arg(snapCorrectionPixels, 0, 'f', 4));
            }
        } else if (draggingSelected_ && dragGestureStarted_ && selectedIndex >= 0) {
            const QVector<ObjectId> dragIndices = draggingShapeIndices_.isEmpty()
                                                     ? QVector<ObjectId>{selectedShapeIndex_}
                                                     : draggingShapeIndices_;
            if (grabActive_) {
                updateGrabPosition(dragIndices);
            } else {
                const bool groupDrag = dragIndices.size() > 1;
                const QPointF previousDragCursorWorld = lastDragWorld_;
                const QPointF cursorStepScreen =
                    screenPosition - worldToScreen(previousDragCursorWorld);
                const QPointF rawDelta = rawCursorWorld_ - lastDragWorld_;
                const QPointF delta = constrainDragDelta(rawDelta);
                if (!qFuzzyIsNull(rawDelta.x()) || !qFuzzyIsNull(rawDelta.y())) {
                    qint64 snapEvaluationMicroseconds = -1;
                    if (dragAxisLock_ != DragAxisLock::None) {
                        if (!qFuzzyIsNull(delta.x()) || !qFuzzyIsNull(delta.y())) {
                            beginDragHistory();
                            translateShapes(dragIndices, delta);
                        }
                        // Axis locking takes priority over object snapping so the
                        // move remains exactly horizontal or vertical.
                        currentDragSnap_ = DragSnapResult{};
                        dragSnapLocked_ = false;
                    } else {
                        if (dragSnapLocked_ && currentDragSnap_.type == SnapType::Near) {
                            // Track the snapped point on the moving selection, not
                            // the mouse cursor: the cursor can be far from that
                            // point when dragging a whole curve. Keep an
                            // unconstrained copy so small moves away from the rail
                            // accumulate instead of being snapped back forever.
                            constexpr qreal nearSnapReleaseRadiusPixels = 12.0;
                            const QPointF movedSourcePoint =
                                currentDragSnap_.targetPoint + delta;
                            const QPointF freeSourcePoint =
                                (nearDragFreeSourcePointValid_
                                     ? nearDragFreeSourcePoint_
                                     : currentDragSnap_.targetPoint) + delta;
                            nearDragFreeSourcePoint_ = freeSourcePoint;
                            nearDragFreeSourcePointValid_ = true;
                            const int nearTargetShapeIndex =
                                currentDragSnap_.targetShapeIndex;
                            const int nearTargetComponentIndex =
                                currentDragSnap_.targetComponentIndex;
                            beginDragHistory();
                            translateShapes(dragIndices, delta);
                            QElapsedTimer snapTimer;
                            snapTimer.start();
                            // A Near lock must not mask a more specific target
                            // that enters range later in the same drag. Check
                            // explicit OSnaps first; only keep riding the Near
                            // rail when none of them currently applies.
                            const DragSnapResult specificDragSnap =
                                findDragSnap(dragIndices, false, false);
                            if (specificDragSnap.isValid()) {
                                currentDragSnap_ = specificDragSnap;
                                translateShapes(dragIndices,
                                                specificDragSnap.translation);
                                dragSnapCursorWorld_ = rawCursorWorld_;
                                nearDragFreeSourcePointValid_ = false;
                                DebugLog::instance().write(
                                    QStringLiteral("selection drag near-promoted snap=%1 source=%2 target=%3")
                                        .arg(snapTypeName(specificDragSnap.type))
                                        .arg(pointText(specificDragSnap.sourcePoint))
                                        .arg(pointText(specificDragSnap.targetPoint)));
                            } else {
                                currentDragSnap_ = trackNearDragSnap(
                                    dragIndices,
                                    freeSourcePoint,
                                    nearTargetShapeIndex,
                                    nearTargetComponentIndex,
                                    nearSnapReleaseRadiusPixels);
                            }
                            snapEvaluationMicroseconds =
                                snapTimer.nsecsElapsed() / 1000;
                            if (currentDragSnap_.isValid()) {
                                if (currentDragSnap_.type == SnapType::Near) {
                                    // The tracked result's source is the free point;
                                    // correct from the selection's actual moved point.
                                    currentDragSnap_.sourcePoint = movedSourcePoint;
                                    currentDragSnap_.translation =
                                        currentDragSnap_.targetPoint - movedSourcePoint;
                                    translateShapes(dragIndices,
                                                    currentDragSnap_.translation);
                                    dragSnapCursorWorld_ = rawCursorWorld_;
                                }
                            } else {
                                // Leave the rail at the accumulated free position.
                                translateShapes(dragIndices,
                                                freeSourcePoint - movedSourcePoint);
                                currentDragSnap_ = DragSnapResult{};
                                dragSnapLocked_ = false;
                                nearDragFreeSourcePointValid_ = false;
                            }
                        } else {
                            constexpr qreal dragSnapBreakawayPixels = 18.0;
                            const qreal cursorDistanceFromSnap =
                                std::hypot(
                                    screenPosition.x() -
                                        worldToScreen(dragSnapCursorWorld_).x(),
                                    screenPosition.y() -
                                        worldToScreen(dragSnapCursorWorld_).y());

                            if (dragSnapLocked_ &&
                                cursorDistanceFromSnap <= dragSnapBreakawayPixels) {
                                // Keep the geometry attached while the cursor is still near
                                // the snap point for non-Near snaps.
                                DebugLog::instance().write(
                                    QStringLiteral("selection drag snap-hold shape=%1 cursorDistance=%2 breakaway=%3")
                                        .arg(selectedIndex)
                                        .arg(cursorDistanceFromSnap, 0, 'f', 2)
                                        .arg(dragSnapBreakawayPixels, 0, 'f', 2));
                            } else if (dragSnapLocked_) {
                                // Release from the snap using the complete cursor movement
                                // since the snap was acquired, so the line leaves cleanly.
                                const QPointF detachDelta = rawCursorWorld_ - dragSnapCursorWorld_;
                                beginDragHistory();
                                translateShapes(dragIndices, detachDelta);
                                currentDragSnap_ = DragSnapResult{};
                                dragSnapLocked_ = false;
                                DebugLog::instance().write(
                                    QStringLiteral("selection drag snap-breakaway shape=%1 cursorDistance=%2")
                                        .arg(selectedIndex)
                                        .arg(cursorDistanceFromSnap, 0, 'f', 2));
                            } else {
                                beginDragHistory();
                                translateShapes(dragIndices, delta);
                                QElapsedTimer snapTimer;
                                snapTimer.start();
                                currentDragSnap_ = groupDrag
                                                        ? findDragSnap(dragIndices)
                                                        : findDragSnap(selectedShapeIndex_);
                                snapEvaluationMicroseconds =
                                    snapTimer.nsecsElapsed() / 1000;
                                if (currentDragSnap_.isValid()) {
                                    translateShapes(dragIndices, currentDragSnap_.translation);
                                    dragSnapLocked_ = true;
                                    dragSnapCursorWorld_ = rawCursorWorld_;
                                    nearDragFreeSourcePointValid_ =
                                        currentDragSnap_.type == SnapType::Near;
                                    if (nearDragFreeSourcePointValid_) {
                                        // This is the geometry point placed on the
                                        // target rail, independent of the grab location.
                                        nearDragFreeSourcePoint_ =
                                            currentDragSnap_.targetPoint;
                                    }
                                }
                            }
                        }
                    }

                    lastDragWorld_ = rawCursorWorld_;

                    qreal snapCorrectionPixels = -1.0;
                    if (currentDragSnap_.isValid()) {
                        const QPointF sourceScreen =
                            worldToScreen(currentDragSnap_.sourcePoint);
                        const QPointF targetScreen =
                            worldToScreen(currentDragSnap_.targetPoint);
                        snapCorrectionPixels =
                            std::hypot(targetScreen.x() - sourceScreen.x(),
                                       targetScreen.y() - sourceScreen.y());
                    }
                    const quint64 traceSequence = ++dragSnapTraceSequence_;
                    DebugLog::instance().write(
                        QStringLiteral("drag-snap-trace seq=%1 mode=object shape=%2 selectedCount=%3 sceneCount=%4 cursorScreen=%5 cursorStepPx=%6 cursorWorld=%7 worldDelta=%8 snapEvalUs=%9 locked=%10 snap=%11 snapSource=%12 snapTarget=%13 snapTranslation=%14 snapCorrectionPx=%15 axisLock=%16")
                            .arg(traceSequence)
                            .arg(selectedIndex)
                            .arg(dragIndices.size())
                            .arg(shapes_.size())
                            .arg(precisePointText(screenPosition))
                            .arg(precisePointText(cursorStepScreen))
                            .arg(precisePointText(rawCursorWorld_))
                            .arg(precisePointText(delta))
                            .arg(snapEvaluationMicroseconds)
                            .arg(dragSnapLocked_)
                            .arg(snapTypeName(currentDragSnap_.type))
                            .arg(precisePointText(currentDragSnap_.sourcePoint))
                            .arg(precisePointText(currentDragSnap_.targetPoint))
                            .arg(precisePointText(currentDragSnap_.translation))
                            .arg(snapCorrectionPixels, 0, 'f', 4)
                            .arg(dragAxisLockName(dragAxisLock_)));
                    if (groupDrag) {
                        DebugLog::instance().write(
                            QStringLiteral("group selection drag count=%1")
                                .arg(dragIndices.size()));
                    }
                }
            }
        }

        if (pointPreviewActive || picturePreviewActive || lineCommandActive_ ||
            arcPreviewActive || circlePreviewActive ||
            tangentCirclePreviewActive || ellipsePreviewActive ||
            rectanglePreviewActive || polygonPreviewActive || mirrorPreviewActive || panning_ ||
            draggingSelected_ ||
            draggingControlPoint_) {
            update();
        }

        if (pointPreviewActive || picturePreviewActive || lineCommandActive_ ||
            arcPreviewActive || circlePreviewActive ||
            tangentCirclePreviewActive || ellipsePreviewActive ||
            rectanglePreviewActive || polygonPreviewActive || mirrorPreviewActive ||
            activeTool_ == Tool::Erase || panning_ ||
            draggingSelected_ || draggingControlPoint_) {
            DebugLog::instance().write(
                QStringLiteral("mouseMove screen=%1 worldRaw=%2 worldUsed=%3 lineActive=%4 points=%5 panning=%6 dragging=%7 ortho=%8 pan=%9 zoom=%10 buttons=0x%11 arcMode=%12 arcSweep=%13 snap=%14")
                    .arg(pointText(screenPosition))
                    .arg(pointText(rawCursorWorld_))
                    .arg(pointText(cursorWorld_))
                    .arg(lineCommandActive_)
                    .arg(pendingPoints_.size())
                    .arg(panning_)
                    .arg(draggingSelected_)
                    .arg(orthoEnabled_)
                    .arg(pointText(pan_))
                    .arg(zoom_, 0, 'f', 4)
                    .arg(static_cast<int>(event->buttons()), 0, 16)
                    .arg(activeTool_ == Tool::Arc ? arcModeName(arcMode_) : QStringLiteral("None"))
                    .arg(arcPreviewSweepAngle_, 0, 'f', 4)
                    .arg(snapTypeName(currentSnap_.type)));
        }

        emitCoordinateUpdate();
    }

    void leaveEvent(QEvent *event) override
    {
        navigationHoverPosition_ = QPointF(-1000.0, -1000.0);
        if (navigationPressedAction_ == BlenderNavigationAction::None) {
            setToolTip(QString());
        }
        update();
        QWidget::leaveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton &&
            navigationPressedAction_ != BlenderNavigationAction::None) {
            const BlenderNavigationAction action = navigationPressedAction_;
            const BlenderNavigationHit hit = navigationPressHit_;
            const QPointF releasePosition = eventPosition(event);
            viewportTransform_.endOrbitGesture();
            if (!navigationMoved_) {
                switch (action) {
                case BlenderNavigationAction::Axis: {
                    Point3D targetDirection = hit.direction;
                    if (directionDot(viewportTransform_.viewDirection(),
                                     targetDirection) > 0.999) {
                        targetDirection.x = -targetDirection.x;
                        targetDirection.y = -targetDirection.y;
                        targetDirection.z = -targetDirection.z;
                    }
                    animateCameraChange([this, targetDirection]() {
                        viewportTransform_.setViewDirection(targetDirection);
                    });
                    break;
                }
                case BlenderNavigationAction::Projection:
                    viewportTransform_.setPerspectiveEnabled(
                        !viewportTransform_.isPerspectiveEnabled());
                    notifyViewStateChanged();
                    update();
                    emitCoordinateUpdate();
                    break;
                case BlenderNavigationAction::Camera:
                    QToolTip::showText(
                        mapToGlobal(releasePosition.toPoint()),
                        QStringLiteral("No active scene camera is available."),
                        this);
                    break;
                case BlenderNavigationAction::Orbit:
                case BlenderNavigationAction::Zoom:
                case BlenderNavigationAction::Pan:
                case BlenderNavigationAction::None:
                    break;
                }
            }
            navigationPressedAction_ = BlenderNavigationAction::None;
            navigationPressHit_ = {};
            navigationMoved_ = false;
            const BlenderNavigationHit hoverHit =
                viewportOverlay_.blenderNavigationGizmoHitAt(releasePosition, size());
            if (hoverHit.action == BlenderNavigationAction::None) {
                setCursor(activeTool_ == Tool::Select ? Qt::ArrowCursor
                                                      : Qt::CrossCursor);
                setToolTip(QString());
            }
            update();
            event->accept();
            return;
        }

        const bool repeatToolOnRelease =
            panning_ && event->button() == panButton_ &&
            event->button() != Qt::MiddleButton && !panMoved_ &&
            activeTool_ == Tool::Select && repeatTool_ != Tool::Select;

        DebugLog::instance().write(QStringLiteral("mouseRelease button=%1 screen=%2 panningBefore=%3 panMoved=%4 draggingBefore=%5 repeat=%6")
                                       .arg(inputButtonName(event->button()))
                                       .arg(pointText(eventPosition(event)))
                                       .arg(panning_)
                                       .arg(panMoved_)
                                       .arg(draggingSelected_)
                                       .arg(repeatToolOnRelease));
        if (grabActive_) {
            return;
        }

        const bool releaseEraseCursor =
            isEraseLikeTool(activeTool_) && event->button() == Qt::LeftButton;
        if (releaseEraseCursor) {
            eraseCursorPressed_ = false;
        }
        if (panning_ && (event->button() == panButton_ ||
                         event->button() == Qt::MiddleButton ||
                         event->button() == Qt::LeftButton)) {
            if (orbiting_) {
                viewportTransform_.endOrbitGesture();
            }
            panning_ = false;
            orbiting_ = false;
            setCursor(activeTool_ == Tool::Select ? Qt::ArrowCursor : Qt::CrossCursor);
            DebugLog::instance().write(QStringLiteral("mouseRelease branch=end-pan-orbit"));
        }
        panMoved_ = false;

        if (repeatToolOnRelease) {
            DebugLog::instance().write(QStringLiteral("mouseRelease branch=repeat-tool"));
            repeatLastTool();
        }

        if (eraseStrokeActive_ && event->button() == Qt::LeftButton) {
            applyEraseCandidates();
            eraseStrokeActive_ = false;
            eraseCandidateShapeIndices_.clear();
            eraseStrokeScreenPath_.clear();
            eraseTargetShapeIndices_.clear();
            eraseSceneCurveCaches_.clear();
            eraseTargetCurveCaches_.clear();
            eraseGeometryCachePrepared_ = false;
            setCursor(Qt::CrossCursor);
            DebugLog::instance().write(QStringLiteral("mouseRelease branch=end-erase-stroke"));
            update();
            return;
        }

        if (selectionBoxActive_ && event->button() == Qt::LeftButton) {
            if (trimBoxSelectionActive_) {
                selectionBoxCurrentScreen_ = eventPosition(event);
                const QPointF totalDelta =
                    selectionBoxCurrentScreen_ - selectionBoxStartScreen_;
                if (std::hypot(totalDelta.x(), totalDelta.y()) >= 3.0) {
                    selectionBoxMoved_ = true;
                }
                finishTrimBoxSelection();
            } else {
                finishSelectionBox();
            }
            return;
        }

        if (draggingControlPoint_ && event->button() == Qt::LeftButton) {
            draggingControlPoint_ = false;
            dragGestureStarted_ = false;
            controlPointIndex_ = -1;
            dragHistoryRecorded_ = false;
            currentDragSnap_ = DragSnapResult{};
            dragSnapLocked_ = false;
            dragAxisLock_ = DragAxisLock::None;
            setCursor(activeTool_ == Tool::Select ? Qt::ArrowCursor : Qt::CrossCursor);
            DebugLog::instance().write(QStringLiteral("mouseRelease branch=end-control-point-drag shape=%1")
                                           .arg(objectIndex(selectedShapeIndex_)));
            update();
        } else if (draggingSelected_ && event->button() == Qt::LeftButton) {
            draggingSelected_ = false;
            dragGestureStarted_ = false;
            draggingShapeIndices_.clear();
            dragHistoryRecorded_ = false;
            currentDragSnap_ = DragSnapResult{};
            dragSnapLocked_ = false;
            dragAxisLock_ = DragAxisLock::None;
            setCursor(activeTool_ == Tool::Select ? Qt::ArrowCursor : Qt::CrossCursor);
            DebugLog::instance().write(QStringLiteral("mouseRelease branch=end-selection-drag shape=%1")
                                           .arg(objectIndex(selectedShapeIndex_)));
            update();
        }

        if (releaseEraseCursor) {
            update();
        }
    }

    void wheelEvent(QWheelEvent *event) override
    {
        stopNavigationAnimation();
        if (subdivisionActive_) {
            const int angleDelta = event->angleDelta().y();
            const int pixelDelta = event->pixelDelta().y();
            const int logicalSteps = subdivisionWheelStepsFromEvent(angleDelta, pixelDelta);
            applySubdivisionWheelSteps(logicalSteps);

            DebugLog::instance().write(
                QStringLiteral("subdivision wheel angleDelta=%1 pixelDelta=%2 phase=%3 logicalSteps=%4 angleRemainder=%5 pixelRemainder=%6 sections=%7")
                    .arg(angleDelta)
                    .arg(pixelDelta)
                    .arg(static_cast<int>(event->phase()))
                    .arg(logicalSteps)
                    .arg(subdivisionWheelAccumulator_)
                    .arg(subdivisionPixelAccumulator_, 0, 'f', 2)
                    .arg(subdivisionSections_));
            event->accept();
            return;
        }

        if (isPolygonTool(activeTool_) && !pendingPoints_.isEmpty()) {
            polygonWheelRemainder_ += event->angleDelta().y();
            const int wheelSteps = polygonWheelRemainder_ / 120;
            if (wheelSteps != 0) {
                polygonWheelRemainder_ %= 120;
                const int stepSize = polygonModeForTool(activeTool_) == PolygonMode::Edge
                                         ? 2
                                         : 1;
                const int maximumSideCount = polygonModeForTool(activeTool_) ==
                                                     PolygonMode::Edge
                                                 ? 255
                                                 : 256;
                polygonSideCount_ = std::clamp(polygonSideCount_ + wheelSteps * stepSize,
                                               3,
                                               maximumSideCount);
                if (polygonModeForTool(activeTool_) == PolygonMode::Edge &&
                    polygonSideCount_ % 2 == 0) {
                    polygonSideCount_ += wheelSteps > 0 ? 1 : -1;
                }
                update();
                emitCoordinateUpdate();
            }
            event->accept();
            return;
        }

        const QPointF screenPosition = eventPosition(event);
        const QPointF beforeZoom = screenToWorld(screenPosition);
        const qreal oldZoom = zoom_;
        const int pixelDelta = event->pixelDelta().y();
        const int angleDelta = event->angleDelta().y();
        qreal wheelSteps = viewportWheelStepsFromDeltas(angleDelta,
                                                        pixelDelta);
        if (wheelSteps == 0.0) {
            event->accept();
            return;
        }
        if (viewportTransform_.navigationPreferences().invertZoomWheel) {
            wheelSteps = -wheelSteps;
        }
        wheelSteps = std::clamp(wheelSteps, -24.0, 24.0);
        // Blender's view_zoom_apply_step uses a 1.2 distance ratio per notch.
        const qreal factor = std::exp(std::log(1.2) * wheelSteps);
        viewportTransform_.zoomAt(screenPosition, factor, size(), 0.15, 12.0);

        const QPointF afterZoom = screenToWorld(screenPosition);

        DebugLog::instance().write(QStringLiteral("wheel screen=%1 angleDeltaY=%2 pixelDeltaY=%3 zoom=%4->%5 worldBefore=%6 worldAfter=%7 pan=%8")
                                       .arg(pointText(screenPosition))
                                       .arg(angleDelta)
                                       .arg(pixelDelta)
                                       .arg(oldZoom, 0, 'f', 4)
                                       .arg(zoom_, 0, 'f', 4)
                                       .arg(pointText(beforeZoom))
                                       .arg(pointText(afterZoom))
                                       .arg(pointText(pan_)));

        update();
        emitCoordinateUpdate();
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        stopNavigationAnimation();
        DebugLog::instance().write(QStringLiteral("keyPress key=%1 text=%2 tool=%3 lineActive=%4 points=%5")
                                       .arg(event->key())
                                       .arg(event->text())
                                       .arg(toolName(activeTool_))
                                       .arg(lineCommandActive_)
                                       .arg(pendingPoints_.size()));
        if (selectionBoxActive_ && event->key() == Qt::Key_Escape) {
            cancelSelectionBox();
            return;
        }

        if (activeTool_ == Tool::Picture && event->key() == Qt::Key_Escape) {
            cancelPicturePlacement();
            return;
        }

        if (joinActive_ &&
            (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
            applyJoin();
            return;
        }

        if (joinActive_ && event->key() == Qt::Key_Escape) {
            cancelJoinMode();
            return;
        }

        if (subdivisionActive_ &&
            (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
            applySubdivision(subdivisionSections_);
            return;
        }

        if (subdivisionActive_ && event->key() == Qt::Key_Escape) {
            cancelSubdivisionPreview();
            return;
        }

        if (activeTool_ == Tool::Rotate && event->key() == Qt::Key_Escape) {
            cancelRotate();
            return;
        }

        if (activeTool_ == Tool::Scale && event->key() == Qt::Key_Escape) {
            cancelScale();
            return;
        }

        if (activeTool_ == Tool::Scale &&
            (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
            if (scaleStep_ == 0) {
                handleScalePoint(scaleSelectionBoundsCenter());
            } else if (scaleStep_ == 1 && !scaleFactorText_.isEmpty()) {
                bool validFactor = false;
                const qreal factor = scaleFactorText_.toDouble(&validFactor);
                if (validFactor && std::isfinite(factor)) {
                    scaleFactorText_.clear();
                    if (scaleMode_ == ScaleMode::OneD) {
                        scaleUsingTypedFactor_ = true;
                        scaleTypedFactor_ = factor;
                        scaleStep_ = 2;
                        updateScalePreview(cursorWorld_);
                        publishScalePrompt();
                        update();
                    } else {
                        commitScale(factor, QPointF());
                    }
                }
            } else if (scaleStep_ == 2) {
                updateScalePreview(cursorWorld_);
                if (scalePreviewValid_) {
                    commitScale(scalePreviewFactor_, scalePreviewAxis_);
                }
            }
            event->accept();
            return;
        }

        if (activeTool_ == Tool::Scale && scaleStep_ == 1) {
            if (event->key() == Qt::Key_Backspace) {
                scaleFactorText_.chop(1);
                publishScalePrompt();
                event->accept();
                return;
            }
            QString typedText = event->text();
            if (typedText == QStringLiteral(",")) {
                typedText = QStringLiteral(".");
            }
            if (typedText.size() == 1) {
                const QChar character = typedText.front();
                const bool digit = character.isDigit();
                const bool decimal = character == QLatin1Char('.') &&
                                     !scaleFactorText_.contains(QLatin1Char('.'));
                const bool sign = character == QLatin1Char('-') &&
                                  scaleFactorText_.isEmpty();
                if (digit || decimal || sign) {
                    scaleFactorText_.append(character);
                    publishScalePrompt();
                    event->accept();
                    return;
                }
            }
        }

        if (activeTool_ == Tool::Mirror && event->key() == Qt::Key_Escape) {
            cancelMirror();
            return;
        }

        if (grabActive_ && event->key() == Qt::Key_Escape) {
            cancelGrab();
            return;
        }

        if (duplicateActive_ && event->key() == Qt::Key_Escape) {
            cancelDuplicate();
            return;
        }

        if (grabActive_ && !event->isAutoRepeat() &&
            event->modifiers() == Qt::NoModifier && event->key() == Qt::Key_B) {
            beginGrabBasePointMode();
            return;
        }

        if (activeTool_ == Tool::Select && !grabActive_ &&
            !event->isAutoRepeat() && event->modifiers() == Qt::NoModifier &&
            event->key() == Qt::Key_G) {
            beginGrab();
            return;
        }

        if (activeTool_ == Tool::Select && draggingSelected_ &&
            !event->isAutoRepeat() && event->modifiers() == Qt::NoModifier &&
            (event->key() == Qt::Key_X || event->key() == Qt::Key_Y)) {
            const DragAxisLock requestedLock = event->key() == Qt::Key_X
                                                   ? DragAxisLock::X
                                                   : DragAxisLock::Y;
            dragAxisLock_ = dragAxisLock_ == requestedLock
                                ? DragAxisLock::None
                                : requestedLock;
            currentDragSnap_ = DragSnapResult{};
            dragSnapLocked_ = false;
            if (grabActive_) {
                updateGrabPosition(draggingShapeIndices_.isEmpty()
                                       ? QVector<ObjectId>{selectedShapeIndex_}
                                       : draggingShapeIndices_);
            }
            DebugLog::instance().write(
                QStringLiteral("selection drag axis lock=%1")
                    .arg(dragAxisLockName(dragAxisLock_)));
            update();
            emitCoordinateUpdate();
            return;
        }

        if (isEraseLikeTool(activeTool_) && event->key() == Qt::Key_Escape) {
            exitEraseLikeTool();
            return;
        }

        if (activeToolController_ != nullptr) {
            const ToolInput input = makeKeyToolInput(event);
            if (activeToolController_->handleKey(input, toolContext_)) {
                update();
                emitCoordinateUpdate();
                return;
            }
        }

        if (activeTool_ == Tool::Select &&
            event->key() == Qt::Key_A &&
            event->modifiers() == Qt::NoModifier) {
            selectedShapeIndices_.clear();
            selectedShapeIndices_.reserve(shapes_.size());
            for (int index = 0; index < shapes_.size(); ++index) {
                const ObjectId objectId = shapes_.objectIdAt(index);
                if (document_.isObjectEditable(objectId)) {
                    selectedShapeIndices_.append(objectId);
                }
            }
            selectedShapeIndex_ = selectedShapeIndices_.isEmpty()
                                      ? ObjectId::invalid()
                                      : selectedShapeIndices_.back();
            draggingSelected_ = false;
            draggingShapeIndices_.clear();
            draggingControlPoint_ = false;
            controlPointIndex_ = -1;
            update();
            emitCoordinateUpdate();
            DebugLog::instance().write(
                QStringLiteral("select all count=%1")
                    .arg(selectedShapeIndices_.size()));
            return;
        }

        if (activeTool_ == Tool::Select &&
            (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) &&
            !selectedShapeIndices_.isEmpty()) {
            deleteSelectedShapes();
            return;
        }

        if (event->key() == Qt::Key_Escape) {
            pendingPoints_.clear();
            resetArcPreviewTracking();
            DebugLog::instance().write(QStringLiteral("keyPress branch=cancel-input"));

            if (activeTool_ == Tool::Line && lineCommandActive_) {
                setTool(Tool::Select);
                if (commandFinished_) {
                    commandFinished_(Tool::Select);
                }
            } else {
                lineCommandActive_ = false;
            }

            update();
            return;
        }

        QWidget::keyPressEvent(event);
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (activeTool_ != Tool::CircleTangentThree ||
            event->type() != QEvent::KeyPress) {
            return false;
        }

        QWidget *targetWidget = qobject_cast<QWidget *>(watched);
        if (targetWidget == nullptr || targetWidget->window() != window()) {
            return false;
        }

        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() != Qt::Key_Tab && keyEvent->key() != Qt::Key_Backtab) {
            return false;
        }

        const ToolInput input = makeKeyToolInput(keyEvent);
        if (activeToolController_ != nullptr &&
            activeToolController_->handleKey(input, toolContext_)) {
            keyEvent->accept();
            update();
            emitCoordinateUpdate();
            return true;
        }
        return false;
    }

private:
    void beginOrbitAt(const QPointF &screenPosition)
    {
        const ViewportNavigationPreferences preferences =
            viewportTransform_.navigationPreferences();
        Point3D depthPoint;
        if (preferences.useMouseDepthNavigate) {
            const QVector<Shape> depthShapes = visibleDepthShapes();
            const bool gpuDepthHit =
                gpuSurface_ != nullptr &&
                gpuSurface_->pickScenePoint(screenPosition,
                                            viewportTransform_,
                                            size(),
                                            depthShapes,
                                            &depthPoint);
            const bool cpuDepthHit = !gpuDepthHit &&
                curveHitTester_.hitTestVisibleDepth(document_,
                                                     screenPosition,
                                                     viewportTransform_,
                                                     size(),
                                                     &depthPoint);
            if (gpuDepthHit || cpuDepthHit) {
                viewportTransform_.setOrbitPivotPreservingView(depthPoint);
                return;
            }
        }

        if (!preferences.orbitAroundActive || !selectedShapeIndex_.isValid()) {
            return;
        }
        const int shapeIndex = objectIndex(selectedShapeIndex_);
        if (shapeIndex < 0 || shapeIndex >= shapes_.size()) {
            return;
        }
        const Shape &shape = shapes_[shapeIndex];
        bool hasBounds = false;
        qreal minimumX = 0.0;
        qreal minimumY = 0.0;
        qreal maximumX = 0.0;
        qreal maximumY = 0.0;
        const auto includePoints = [&](const QVector<QPointF> &points) {
            for (const QPointF &point : points) {
                if (!std::isfinite(point.x()) || !std::isfinite(point.y())) {
                    continue;
                }
                if (!hasBounds) {
                    minimumX = maximumX = point.x();
                    minimumY = maximumY = point.y();
                    hasBounds = true;
                } else {
                    minimumX = std::min(minimumX, point.x());
                    minimumY = std::min(minimumY, point.y());
                    maximumX = std::max(maximumX, point.x());
                    maximumY = std::max(maximumY, point.y());
                }
            }
        };
        includePoints(shape.points);
        includePoints(shape.nurbs.controlPoints);
        for (const Shape::NurbsCurve2D &component : shape.components) {
            includePoints(component.controlPoints);
        }
        if (!hasBounds) {
            return;
        }
        const QPointF center((minimumX + maximumX) * 0.5,
                             (minimumY + maximumY) * 0.5);
        viewportTransform_.setOrbitPivotPreservingView(
            workPlanePointToWorld(center, shape.workPlane, shape.workPlaneOffset));
    }

    static qreal directionDot(const Point3D &first, const Point3D &second)
    {
        return first.x * second.x + first.y * second.y + first.z * second.z;
    }

    void stopNavigationAnimation()
    {
        if (navigationAnimation_ != nullptr) {
            navigationAnimation_->stop();
        }
    }

    void animateCameraChange(const std::function<void()> &setTarget)
    {
        stopNavigationAnimation();
        const ViewportCameraState original = viewportTransform_.cameraState();
        setTarget();
        const ViewportCameraState target = viewportTransform_.cameraState();
        constexpr qreal angleTolerance = 1.0e-7;
        const bool poseChanged =
            std::abs(original.zoom - target.zoom) > 1.0e-8 ||
            std::abs(original.gridViewDistance - target.gridViewDistance) > 1.0e-8 ||
            std::hypot(original.pan.x() - target.pan.x(),
                       original.pan.y() - target.pan.y()) > 1.0e-8 ||
            std::abs(original.orbitPivot.x - target.orbitPivot.x) > 1.0e-8 ||
            std::abs(original.orbitPivot.y - target.orbitPivot.y) > 1.0e-8 ||
            std::abs(original.orbitPivot.z - target.orbitPivot.z) > 1.0e-8 ||
            1.0 - std::abs(original.orientation.dot(target.orientation)) >
                angleTolerance;

        ViewportCameraState start = original;
        start.perspective = target.perspective;
        if (!poseChanged || navigationAnimation_ == nullptr) {
            viewportTransform_.setCameraState(target);
            notifyViewStateChanged();
            update();
            emitCoordinateUpdate();
            return;
        }
        navigationAnimationStart_ = start;
        navigationAnimationEnd_ = target;
        viewportTransform_.setCameraState(start);
        navigationAnimation_->start();
    }

    QString navigationTooltip(const BlenderNavigationHit &hit) const
    {
        switch (hit.action) {
        case BlenderNavigationAction::Axis: {
            const qreal component = std::abs(hit.direction.x) > 0.5
                                        ? hit.direction.x
                                        : std::abs(hit.direction.y) > 0.5
                                              ? hit.direction.y
                                              : hit.direction.z;
            const QString axis = std::abs(hit.direction.x) > 0.5
                                     ? QStringLiteral("X")
                                     : std::abs(hit.direction.y) > 0.5
                                           ? QStringLiteral("Y")
                                           : QStringLiteral("Z");
            return QStringLiteral("Align to %1%2 view; drag to orbit")
                .arg(component >= 0.0 ? QStringLiteral("+") : QStringLiteral("-"),
                     axis);
        }
        case BlenderNavigationAction::Orbit:
            return QStringLiteral("Drag to orbit the view");
        case BlenderNavigationAction::Zoom:
            return QStringLiteral("Drag to zoom the view");
        case BlenderNavigationAction::Pan:
            return QStringLiteral("Drag to pan the view");
        case BlenderNavigationAction::Camera:
            return QStringLiteral("Camera view (no active scene camera)");
        case BlenderNavigationAction::Projection:
            return viewportTransform_.isPerspectiveEnabled()
                       ? QStringLiteral("Switch to orthographic projection")
                       : QStringLiteral("Switch to perspective projection");
        case BlenderNavigationAction::None:
            break;
        }
        return {};
    }

    void resetForDocumentReplacement()
    {
        history_.clear();
        pendingPoints_.clear();
        pendingPictureImage_ = QImage();
        pendingPictureImageData_.clear();
        pendingPicturePath_.clear();
        controllerPreviewShape_ = Shape{};
        controllerPreviewShapeVisible_ = false;
        resetArcPreviewTracking();
        selectedShapeIndices_.clear();
        selectedShapeIndex_ = ObjectId::invalid();
        controlPointIndex_ = -1;
        selectionBoxActive_ = false;
        selectionBoxMoved_ = false;
        selectionBoxAdditive_ = false;
        trimBoxSelectionActive_ = false;
        draggingSelected_ = false;
        dragGestureStarted_ = false;
        draggingShapeIndices_.clear();
        draggingControlPoint_ = false;
        dragHistoryRecorded_ = false;
        currentDragSnap_ = DragSnapResult{};
        dragSnapLocked_ = false;
        eraseStrokeActive_ = false;
        eraseCursorPressed_ = false;
        eraseCandidateShapeIndices_.clear();
        eraseStrokeScreenPath_.clear();
        eraseTargetShapeIndices_.clear();
        eraseSceneCurveCaches_.clear();
        eraseTargetCurveCaches_.clear();
        eraseGeometryCachePrepared_ = false;
        trimHoverPositionValid_ = false;
        currentSnap_ = SnapResult{};
        joinActive_ = false;
        joinShapeIndices_.clear();
        resetScaleInteraction();
        resetRotateInteraction();
        resetMirrorInteraction();
        subdivisionActive_ = false;
        subdivisionShapeIndex_ = ObjectId::invalid();
        subdivisionSections_ = 2;
        resetSubdivisionWheelTracking();
        lineCommandActive_ = false;
        activeTool_ = Tool::Select;
        repeatTool_ = Tool::Select;
    }

    void updateSnapEngineSettings()
    {
        snapEngine_.setSettings(SnapSettings{osnapEnabled_,
                                             endpointSnapEnabled_,
                                             midpointSnapEnabled_,
                                             intersectionSnapEnabled_,
                                             centerSnapEnabled_,
                                             perpendicularSnapEnabled_,
                                             tangentSnapEnabled_,
                                             nearSnapEnabled_,
                                             controlPointSnapEnabled_});
    }

    int objectIndex(ObjectId objectId) const
    {
        return document_.indexOf(objectId);
    }

    bool isJoinableShape(const Shape &shape) const
    {
        if (shape.geometryType == GeometryType::PolyCurve) {
            if (shape.components.isEmpty()) {
                return false;
            }
            for (const Shape::NurbsCurve2D &component : shape.components) {
                if (!isValidNurbsCurve(component)) {
                    return false;
                }
            }
            return true;
        }

        return shape.geometryType == GeometryType::Line ||
               shape.geometryType == GeometryType::Arc ||
               shape.geometryType == GeometryType::Bezier ||
               shape.geometryType == GeometryType::Nurbs ||
               shape.geometryType == GeometryType::Ellipse;
    }

    bool appendJoinComponents(const Shape &shape,
                              QVector<Shape::NurbsCurve2D> *components) const
    {
        if (components == nullptr || !isJoinableShape(shape)) {
            return false;
        }

        if (shape.geometryType == GeometryType::PolyCurve) {
            *components += shape.components;
            return true;
        }

        if (isValidNurbsCurve(shape.nurbs)) {
            components->append(shape.nurbs);
            return true;
        }

        if (shape.geometryType == GeometryType::Line && shape.points.size() >= 2) {
            const Shape::NurbsCurve2D line = makeDegreeOneNurbs(shape.points);
            if (isValidNurbsCurve(line)) {
                components->append(line);
                return true;
            }
        }

        return false;
    }

    bool nurbsCurveEndpoints(const Shape::NurbsCurve2D &curve,
                             QPointF *start,
                             QPointF *end) const
    {
        if (!isValidNurbsCurve(curve) || (start == nullptr && end == nullptr)) {
            return false;
        }

        const QVector<double> fullKnots = expandedKnotVector(curve);
        if (fullKnots.size() <= curve.controlPoints.size()) {
            return false;
        }

        const bool startValid = start == nullptr ||
                                evaluateNurbsPoint(curve, fullKnots[curve.degree], start);
        const bool endValid = end == nullptr ||
                              evaluateNurbsPoint(curve,
                                                 fullKnots[curve.controlPoints.size()],
                                                 end);
        return startValid && endValid;
    }

    qreal joinEndpointTolerance() const
    {
        constexpr qreal minimumTolerance = 1.0e-5;
        constexpr qreal screenTolerancePixels = 3.0;
        return std::max(minimumTolerance,
                        screenTolerancePixels / std::max(zoom_, 1.0e-9));
    }

    Shape::NurbsCurve2D reversedNurbsCurve(const Shape::NurbsCurve2D &curve) const
    {
        if (!isValidNurbsCurve(curve)) {
            return {};
        }

        Shape::NurbsCurve2D reversed = curve;
        std::reverse(reversed.controlPoints.begin(), reversed.controlPoints.end());
        if (!reversed.weights.isEmpty()) {
            std::reverse(reversed.weights.begin(), reversed.weights.end());
        }

        const QVector<double> fullKnots = expandedKnotVector(curve);
        const qreal domainStart = fullKnots[curve.degree];
        const qreal domainEnd = fullKnots[curve.controlPoints.size()];
        reversed.knots.clear();
        reversed.knots.reserve(curve.knots.size());
        for (int index = 1; index + 1 < fullKnots.size(); ++index) {
            reversed.knots.append(domainStart + domainEnd -
                                  fullKnots[fullKnots.size() - 1 - index]);
        }
        return reversed;
    }

    bool orderJoinComponents(const QVector<Shape::NurbsCurve2D> &input,
                             QVector<Shape::NurbsCurve2D> *ordered) const
    {
        if (ordered == nullptr || input.isEmpty()) {
            return false;
        }

        QVector<QPointF> starts;
        QVector<QPointF> ends;
        starts.reserve(input.size());
        ends.reserve(input.size());
        for (const Shape::NurbsCurve2D &curve : input) {
            QPointF start;
            QPointF end;
            if (!nurbsCurveEndpoints(curve, &start, &end)) {
                return false;
            }
            starts.append(start);
            ends.append(end);
        }

        const qreal tolerance = joinEndpointTolerance();
        const auto endpointsMatch = [tolerance](const QPointF &first,
                                                  const QPointF &second) {
            return std::hypot(first.x() - second.x(), first.y() - second.y()) <= tolerance;
        };

        QVector<int> componentOrder;
        QVector<bool> componentReversed;
        QVector<char> used(input.size(), false);

        const auto makeResult = [&]() {
            ordered->clear();
            ordered->reserve(componentOrder.size());
            for (int position = 0; position < componentOrder.size(); ++position) {
                const int componentIndex = componentOrder[position];
                ordered->append(componentReversed[position]
                                    ? reversedNurbsCurve(input[componentIndex])
                                    : input[componentIndex]);
            }
        };

        std::function<bool(const QPointF &)> extendChain;
        extendChain = [&](const QPointF &currentEnd) {
            if (componentOrder.size() == input.size()) {
                return true;
            }

            for (int candidate = 0; candidate < input.size(); ++candidate) {
                if (used[candidate]) {
                    continue;
                }

                if (endpointsMatch(currentEnd, starts[candidate])) {
                    used[candidate] = true;
                    componentOrder.append(candidate);
                    componentReversed.append(false);
                    if (extendChain(ends[candidate])) {
                        return true;
                    }
                    componentReversed.removeLast();
                    componentOrder.removeLast();
                    used[candidate] = false;
                }

                if (endpointsMatch(currentEnd, ends[candidate])) {
                    used[candidate] = true;
                    componentOrder.append(candidate);
                    componentReversed.append(true);
                    if (extendChain(starts[candidate])) {
                        return true;
                    }
                    componentReversed.removeLast();
                    componentOrder.removeLast();
                    used[candidate] = false;
                }
            }

            return false;
        };

        for (int first = 0; first < input.size(); ++first) {
            for (const bool reverseFirst : {false, true}) {
                std::fill(used.begin(), used.end(), false);
                componentOrder.clear();
                componentReversed.clear();
                used[first] = true;
                componentOrder.append(first);
                componentReversed.append(reverseFirst);
                const QPointF firstEnd = reverseFirst ? starts[first] : ends[first];
                if (extendChain(firstEnd)) {
                    makeResult();
                    return true;
                }
            }
        }

        return false;
    }

    bool closeJoinGaps(QVector<Shape::NurbsCurve2D> *components) const
    {
        if (components == nullptr || components->isEmpty()) {
            return false;
        }

        const qreal tolerance = joinEndpointTolerance();
        for (int index = 0; index + 1 < components->size(); ++index) {
            QPointF previousEnd;
            QPointF nextStart;
            if (!nurbsCurveEndpoints(components->at(index), nullptr, &previousEnd) ||
                !nurbsCurveEndpoints(components->at(index + 1), &nextStart, nullptr)) {
                return false;
            }

            const QPointF delta = previousEnd - nextStart;
            if (std::hypot(delta.x(), delta.y()) > tolerance) {
                return false;
            }

            // Move only the next component. That closes this seam without
            // disturbing a seam that was already closed earlier in the chain.
            // The component's degree, weights, knots, and parameter domain
            // remain unchanged.
            for (QPointF &controlPoint : (*components)[index + 1].controlPoints) {
                controlPoint += delta;
            }
        }

        return true;
    }

    bool nurbsCurvePointAtFraction(const Shape::NurbsCurve2D &curve,
                                   qreal fraction,
                                   QPointF *point) const
    {
        if (!isValidNurbsCurve(curve) || point == nullptr) {
            return false;
        }

        const QVector<double> fullKnots = expandedKnotVector(curve);
        if (fullKnots.size() <= curve.controlPoints.size()) {
            return false;
        }

        const qreal firstParameter = fullKnots[curve.degree];
        const qreal lastParameter = fullKnots[curve.controlPoints.size()];
        return evaluateNurbsPoint(curve,
                                  firstParameter +
                                      (lastParameter - firstParameter) *
                                          std::clamp(fraction, 0.0, 1.0),
                                  point);
    }

    bool joinComponentsAreContinuous(const QVector<Shape::NurbsCurve2D> &components) const
    {
        const qreal joinTolerance = joinEndpointTolerance();
        for (int index = 0; index + 1 < components.size(); ++index) {
            QPointF firstEnd;
            QPointF nextStart;
            if (!nurbsCurveEndpoints(components[index], nullptr, &firstEnd) ||
                !nurbsCurveEndpoints(components[index + 1], &nextStart, nullptr)) {
                return false;
            }

            if (std::hypot(firstEnd.x() - nextStart.x(),
                           firstEnd.y() - nextStart.y()) > joinTolerance) {
                return false;
            }
        }

        return true;
    }

    QVector<QPointF> polyCurvePoints(
        const QVector<Shape::NurbsCurve2D> &components) const
    {
        QVector<QPointF> points;
        for (int index = 0; index < components.size(); ++index) {
            QPointF start;
            QPointF end;
            if (!nurbsCurveEndpoints(components[index], &start, &end)) {
                continue;
            }
            if (index == 0) {
                points.append(start);
            }
            points.append(end);
        }
        return points;
    }

    QVector<QVector<Shape::NurbsCurve2D>> connectedCurveGroups(
        const QVector<Shape::NurbsCurve2D> &curves) const
    {
        QVector<QPointF> starts;
        QVector<QPointF> ends;
        starts.reserve(curves.size());
        ends.reserve(curves.size());
        qreal coordinateScale = 1.0;
        for (const Shape::NurbsCurve2D &curve : curves) {
            QPointF start;
            QPointF end;
            if (!nurbsCurveEndpoints(curve, &start, &end)) {
                starts.append(QPointF());
                ends.append(QPointF());
                continue;
            }
            starts.append(start);
            ends.append(end);
            coordinateScale = std::max({coordinateScale,
                                        std::abs(start.x()),
                                        std::abs(start.y()),
                                        std::abs(end.x()),
                                        std::abs(end.y())});
        }

        const qreal endpointTolerance = std::max<qreal>(1.0e-8,
                                                        coordinateScale * 1.0e-12);
        const auto endpointsMatch = [endpointTolerance](const QPointF &first,
                                                        const QPointF &second) {
            return std::hypot(first.x() - second.x(),
                              first.y() - second.y()) <= endpointTolerance;
        };
        QVector<QVector<Shape::NurbsCurve2D>> groups;
        QVector<bool> grouped(curves.size(), false);
        for (int seed = 0; seed < curves.size(); ++seed) {
            if (grouped[seed]) {
                continue;
            }

            QVector<int> connectedIndices{seed};
            grouped[seed] = true;
            for (int cursor = 0; cursor < connectedIndices.size(); ++cursor) {
                const int current = connectedIndices[cursor];
                if (!isValidNurbsCurve(curves[current])) {
                    continue;
                }
                for (int candidate = 0; candidate < curves.size(); ++candidate) {
                    if (grouped[candidate] || !isValidNurbsCurve(curves[candidate])) {
                        continue;
                    }
                    if (endpointsMatch(starts[current], starts[candidate]) ||
                        endpointsMatch(starts[current], ends[candidate]) ||
                        endpointsMatch(ends[current], starts[candidate]) ||
                        endpointsMatch(ends[current], ends[candidate])) {
                        grouped[candidate] = true;
                        connectedIndices.append(candidate);
                    }
                }
            }

            std::sort(connectedIndices.begin(), connectedIndices.end());
            QVector<Shape::NurbsCurve2D> group;
            group.reserve(connectedIndices.size());
            for (const int index : connectedIndices) {
                group.append(curves[index]);
            }
            QVector<Shape::NurbsCurve2D> ordered;
            if (group.size() > 1 && orderJoinComponents(group, &ordered)) {
                group = ordered;
            }
            groups.append(group);
        }
        return groups;
    }

    Shape polyCurveShapeForComponents(
        const Shape &source,
        const QVector<Shape::NurbsCurve2D> &components) const
    {
        Shape result = source;
        result.geometryType = GeometryType::PolyCurve;
        result.points = polyCurvePoints(components);
        result.nurbs = Shape::NurbsCurve2D{};
        result.arcMode = ArcMode::TwoPoint;
        result.arcSweep = 0.0;
        result.subdivisionParameters.clear();
        result.components = components;
        return result;
    }

    void normalizeDisconnectedPolyCurveObjects()
    {
        QVector<SceneObject> normalizedObjects;
        normalizedObjects.reserve(document_.size());
        bool changed = false;
        int splitCount = 0;
        for (const SceneObject &sceneObject : document_.objects()) {
            const Shape &source = sceneObject.geometry;
            if (source.geometryType != GeometryType::PolyCurve ||
                source.components.size() < 2) {
                normalizedObjects.append(sceneObject);
                continue;
            }

            const auto connectedGroups = connectedCurveGroups(source.components);
            QVector<QVector<Shape::NurbsCurve2D>> validGroups;
            for (const auto &group : connectedGroups) {
                QVector<Shape::NurbsCurve2D> ordered;
                if (group.size() > 1 && !orderJoinComponents(group, &ordered)) {
                    // A connected branch is not one continuous spline. Keep
                    // each branch as its own selectable curve object.
                    for (const Shape::NurbsCurve2D &curve : group) {
                        validGroups.append({curve});
                    }
                } else {
                    validGroups.append(group.size() > 1 ? ordered : group);
                }
            }

            if (validGroups.size() <= 1) {
                normalizedObjects.append(sceneObject);
                continue;
            }

            changed = true;
            ++splitCount;
            SceneObject firstPiece = sceneObject;
            firstPiece.geometry = polyCurveShapeForComponents(source, validGroups.first());
            normalizedObjects.append(firstPiece);
            for (int groupIndex = 1; groupIndex < validGroups.size(); ++groupIndex) {
                SceneObject piece = sceneObject;
                piece.id = ObjectId::invalid();
                piece.geometry = polyCurveShapeForComponents(source, validGroups[groupIndex]);
                normalizedObjects.append(piece);
            }
        }

        if (changed) {
            document_.replaceObjects(normalizedObjects);
            DebugLog::instance().write(
                QStringLiteral("disconnected PolyCurve repair splitObjects=%1 objects=%2")
                    .arg(splitCount)
                    .arg(document_.size()));
        }
    }

    void notifyJoinStatus(const QString &message = QString())
    {
        if (joinStatusUpdate_) {
            joinStatusUpdate_(message.isEmpty() ? joinStatusText() : message);
        }
    }

    bool isShapeSelected(int shapeIndex) const
    {
        const ObjectId objectId = shapes_.objectIdAt(shapeIndex);
        return objectId.isValid() && selectedShapeIndices_.contains(objectId);
    }

    bool deleteSelectedShapes()
    {
        QVector<int> indices;
        for (const ObjectId objectId : selectedShapeIndices_) {
            const int index = objectIndex(objectId);
            if (index >= 0 && index < shapes_.size() && !indices.contains(index)) {
                indices.append(index);
            }
        }

        if (indices.isEmpty()) {
            clearSelection();
            return false;
        }

        std::sort(indices.begin(), indices.end());
        const int deletedCount = indices.size();
        recordGeometryChange();

        for (auto iterator = indices.crbegin(); iterator != indices.crend(); ++iterator) {
            shapes_.removeAt(*iterator);
        }

        clearSelection();
        draggingSelected_ = false;
        draggingShapeIndices_.clear();
        draggingControlPoint_ = false;
        controlPointIndex_ = -1;
        dragHistoryRecorded_ = false;
        currentDragSnap_ = DragSnapResult{};
        dragSnapLocked_ = false;
        update();
        DebugLog::instance().write(QStringLiteral("delete selection count=%1 shapes=%2")
                                       .arg(deletedCount)
                                       .arg(shapes_.size()));
        return true;
    }

    void clearSelection()
    {
        selection_.clear();
    }

    void pruneSelectionToEditableLayers()
    {
        QVector<ObjectId> editableSelection;
        editableSelection.reserve(selectedShapeIndices_.size());
        for (const ObjectId objectId : selectedShapeIndices_) {
            if (document_.isObjectEditable(objectId)) {
                editableSelection.append(objectId);
            }
        }

        const ObjectId primaryObject = editableSelection.contains(selectedShapeIndex_)
                                           ? selectedShapeIndex_
                                           : ObjectId::invalid();
        selection_.setObjectIds(editableSelection, primaryObject);
        selectedShapeIndex_ = selection_.primaryObjectId();
        draggingSelected_ = false;
        draggingShapeIndices_.clear();
        draggingControlPoint_ = false;
        controlPointIndex_ = -1;
        dragHistoryRecorded_ = false;
        currentDragSnap_ = DragSnapResult{};
        dragSnapLocked_ = false;
    }

    void setSingleSelection(int shapeIndex)
    {
        selection_.clear();
        if (shapeIndex >= 0 && shapeIndex < shapes_.size()) {
            const ObjectId objectId = shapes_.objectIdAt(shapeIndex);
            selection_.add(objectId);
        }
    }

    void toggleShapeSelection(int shapeIndex)
    {
        if (shapeIndex < 0 || shapeIndex >= shapes_.size()) {
            return;
        }

        const ObjectId objectId = shapes_.objectIdAt(shapeIndex);
        selection_.toggle(objectId);
    }

    QRectF selectionBoundsForShape(const Shape &shape) const
    {
        QVector<QPointF> points = controlPointsForShape(shape);
        // Keep the source points in the selection bounds as well as the
        // stored NURBS CVs. This covers endpoints that are represented by
        // the shape record but are not present in a malformed/legacy CV
        // array, while the NURBS data remains the rendering source of truth.
        for (const QPointF &point : shape.points) {
            if (!points.contains(point)) {
                points.append(point);
            }
        }

        qreal minX = 0.0;
        qreal maxX = 0.0;
        qreal minY = 0.0;
        qreal maxY = 0.0;
        bool initialized = false;
        for (const QPointF &point : points) {
            if (!std::isfinite(point.x()) || !std::isfinite(point.y())) {
                continue;
            }

            const QPointF screenPoint = worldToScreen(point);
            if (!initialized) {
                minX = maxX = screenPoint.x();
                minY = maxY = screenPoint.y();
                initialized = true;
            } else {
                minX = std::min(minX, screenPoint.x());
                maxX = std::max(maxX, screenPoint.x());
                minY = std::min(minY, screenPoint.y());
                maxY = std::max(maxY, screenPoint.y());
            }
        }

        if (!initialized) {
            return {};
        }

        // A line or a point can have a zero-width bounding box. The small
        // padding keeps box selection usable at normal zoom levels and also
        // covers the visible stroke/point marker.
        return QRectF(QPointF(minX, minY), QPointF(maxX, maxY))
            .adjusted(-5.0, -5.0, 5.0, 5.0);
    }

    bool shapeMatchesSelectionBox(const Shape &shape,
                                  const QRectF &box,
                                  bool crossingSelection) const
    {
        const QRectF bounds = selectionBoundsForShape(shape).normalized();
        if (bounds.isNull()) {
            return false;
        }

        // CAD-style selection windows use containment when dragged from
        // left to right and crossing selection when dragged from right to
        // left. The crossing window includes anything that touches it.
        if (!crossingSelection) {
            return box.normalized().contains(bounds);
        }

        // QRectF::intersects() can exclude a contact that falls exactly on
        // an edge. Include the visible stroke/point tolerance and compare
        // the normalized edges inclusively so a touching curve is selected.
        constexpr qreal crossingTolerancePixels = 2.0;
        const QRectF crossingBox = box.normalized().adjusted(-crossingTolerancePixels,
                                                              -crossingTolerancePixels,
                                                              crossingTolerancePixels,
                                                              crossingTolerancePixels);
        return bounds.left() <= crossingBox.right() &&
               crossingBox.left() <= bounds.right() &&
               bounds.top() <= crossingBox.bottom() &&
               crossingBox.top() <= bounds.bottom();
    }

    void beginSelectionBox(const QPointF &screenPosition, bool additive)
    {
        selectionBoxActive_ = true;
        selectionBoxMoved_ = false;
        selectionBoxAdditive_ = additive;
        trimBoxSelectionActive_ = false;
        selectionBoxStartScreen_ = screenPosition;
        selectionBoxCurrentScreen_ = screenPosition;
        draggingSelected_ = false;
        draggingShapeIndices_.clear();
        draggingControlPoint_ = false;
        controlPointIndex_ = -1;
        dragHistoryRecorded_ = false;
        currentDragSnap_ = DragSnapResult{};
        dragSnapLocked_ = false;
        setCursor(Qt::CrossCursor);
        update();
        emitCoordinateUpdate();
        DebugLog::instance().write(QStringLiteral("selection box start additive=%1 at=%2")
                                       .arg(additive)
                                       .arg(pointText(screenPosition)));
    }

    void finishSelectionBox()
    {
        if (!selectionBoxActive_) {
            return;
        }

        const QRectF selectionBox =
            QRectF(selectionBoxStartScreen_, selectionBoxCurrentScreen_).normalized();
        const bool crossingSelection =
            selectionBoxCurrentScreen_.x() < selectionBoxStartScreen_.x();
        const bool moved = selectionBoxMoved_ ||
                           selectionBox.width() >= 3.0 ||
                           selectionBox.height() >= 3.0;
        QVector<ObjectId> boxSelection;
        if (moved) {
            for (int index = 0; index < shapes_.size(); ++index) {
                const ObjectId objectId = shapes_.objectIdAt(index);
                if (!document_.isObjectEditable(objectId)) {
                    continue;
                }
                if (!workPlaneMatches(shapes_[index].workPlane,
                                      shapes_[index].workPlaneOffset,
                                      viewportTransform_.workPlane(),
                                      viewportTransform_.workPlaneOffset())) {
                    continue;
                }
                if (shapeMatchesSelectionBox(shapes_[index],
                                              selectionBox,
                                              crossingSelection)) {
                    boxSelection.append(shapes_.objectIdAt(index));
                }
            }
        }

        if (moved) {
            if (selectionBoxAdditive_) {
                for (const ObjectId objectId : boxSelection) {
                    if (!selectedShapeIndices_.contains(objectId)) {
                        selectedShapeIndices_.append(objectId);
                    }
                }
                if (!boxSelection.isEmpty()) {
                    selectedShapeIndex_ = boxSelection.back();
                }
            } else {
                selectedShapeIndices_ = boxSelection;
                selectedShapeIndex_ = boxSelection.isEmpty()
                                          ? ObjectId::invalid()
                                          : boxSelection.back();
            }
        } else if (!selectionBoxAdditive_) {
            clearSelection();
        }

        DebugLog::instance().write(QStringLiteral("selection box finish moved=%1 crossing=%2 additive=%3 selected=%4")
                                       .arg(moved)
                                       .arg(crossingSelection)
                                       .arg(selectionBoxAdditive_)
                                       .arg(selectedShapeIndices_.size()));
        selectionBoxActive_ = false;
        selectionBoxMoved_ = false;
        selectionBoxAdditive_ = false;
        trimBoxSelectionActive_ = false;
        setCursor(joinActive_ ? Qt::CrossCursor : Qt::ArrowCursor);
        update();
        emitCoordinateUpdate();
    }

    void cancelSelectionBox()
    {
        if (!selectionBoxActive_) {
            return;
        }

        const bool cancelingTrimBox = trimBoxSelectionActive_;
        selectionBoxActive_ = false;
        selectionBoxMoved_ = false;
        selectionBoxAdditive_ = false;
        trimBoxSelectionActive_ = false;
        if (cancelingTrimBox) {
            eraseCandidateShapeIndices_.clear();
            eraseStrokeScreenPath_.clear();
            eraseTargetShapeIndices_.clear();
            eraseSceneCurveCaches_.clear();
            eraseTargetCurveCaches_.clear();
            eraseGeometryCachePrepared_ = false;
            trimHoverPositionValid_ = false;
        }
        setCursor(joinActive_ || activeTool_ == Tool::Trim
                      ? Qt::CrossCursor
                      : Qt::ArrowCursor);
        update();
        DebugLog::instance().write(QStringLiteral("selection box canceled"));
    }

    bool isSubdividableShape(const Shape &shape) const
    {
        if (shape.geometryType == GeometryType::Line) {
            return shape.points.size() >= 2 &&
                   (isValidNurbsCurve(shape.nurbs) || !shape.points.isEmpty());
        }

        return (shape.geometryType == GeometryType::Arc ||
                shape.geometryType == GeometryType::Bezier ||
                shape.geometryType == GeometryType::Nurbs ||
                shape.geometryType == GeometryType::Circle ||
                shape.geometryType == GeometryType::Ellipse) &&
               isValidNurbsCurve(shape.nurbs);
    }

    bool subdivisionCurve(const Shape &shape, Shape::NurbsCurve2D *curve) const
    {
        if (curve == nullptr || !isSubdividableShape(shape)) {
            return false;
        }

        if (isValidNurbsCurve(shape.nurbs)) {
            *curve = shape.nurbs;
            return true;
        }

        if (shape.geometryType == GeometryType::Line && shape.points.size() >= 2) {
            *curve = makeDegreeOneNurbs(shape.points);
            return isValidNurbsCurve(*curve);
        }

        return false;
    }

    QVector<double> subdivisionParametersForSections(const Shape &shape,
                                                      int sections) const
    {
        QVector<double> parameters;
        if (sections < 2) {
            return parameters;
        }

        Shape::NurbsCurve2D curve;
        if (!subdivisionCurve(shape, &curve)) {
            return parameters;
        }

        const QVector<double> fullKnots = expandedKnotVector(curve);
        if (fullKnots.size() <= curve.degree + 1 ||
            curve.controlPoints.isEmpty()) {
            return parameters;
        }

        const qreal firstParameter = fullKnots[curve.degree];
        const qreal lastParameter = fullKnots[curve.controlPoints.size()];
        if (!std::isfinite(firstParameter) || !std::isfinite(lastParameter) ||
            lastParameter <= firstParameter) {
            return parameters;
        }

        int nonZeroSpans = 0;
        for (int index = curve.degree; index < curve.controlPoints.size(); ++index) {
            if (fullKnots[index + 1] > fullKnots[index]) {
                ++nonZeroSpans;
            }
        }

        const int sampleCount = std::clamp(std::max(128, nonZeroSpans * 64), 128, 4096);
        QVector<qreal> sampleParameters;
        QVector<qreal> cumulativeLengths;
        sampleParameters.reserve(sampleCount + 1);
        cumulativeLengths.reserve(sampleCount + 1);

        QPointF previousPoint;
        if (!evaluateNurbsPoint(curve, firstParameter, &previousPoint)) {
            return parameters;
        }

        sampleParameters.append(firstParameter);
        cumulativeLengths.append(0.0);
        qreal totalLength = 0.0;
        for (int sample = 1; sample <= sampleCount; ++sample) {
            const qreal fraction = static_cast<qreal>(sample) / sampleCount;
            const qreal parameter = firstParameter +
                                    (lastParameter - firstParameter) * fraction;
            QPointF currentPoint;
            if (!evaluateNurbsPoint(curve, parameter, &currentPoint)) {
                return QVector<double>();
            }

            totalLength += std::hypot(currentPoint.x() - previousPoint.x(),
                                      currentPoint.y() - previousPoint.y());
            sampleParameters.append(parameter);
            cumulativeLengths.append(totalLength);
            previousPoint = currentPoint;
        }

        if (totalLength <= 1e-9) {
            return parameters;
        }

        parameters.reserve(sections - 1);
        for (int division = 1; division < sections; ++division) {
            const qreal targetLength = totalLength * division / sections;
            const auto upper = std::lower_bound(cumulativeLengths.cbegin(),
                                                cumulativeLengths.cend(),
                                                targetLength);
            const int upperIndex = static_cast<int>(upper - cumulativeLengths.cbegin());
            if (upperIndex <= 0) {
                parameters.append(sampleParameters.first());
                continue;
            }
            if (upperIndex >= cumulativeLengths.size()) {
                parameters.append(sampleParameters.last());
                continue;
            }

            const qreal lowerLength = cumulativeLengths[upperIndex - 1];
            const qreal upperLength = cumulativeLengths[upperIndex];
            const qreal span = upperLength - lowerLength;
            const qreal localFraction = span > 1e-12
                                            ? (targetLength - lowerLength) / span
                                            : 0.0;
            parameters.append(sampleParameters[upperIndex - 1] +
                               (sampleParameters[upperIndex] -
                                sampleParameters[upperIndex - 1]) * localFraction);
        }

        return parameters;
    }

    void resetSubdivisionWheelTracking()
    {
        subdivisionWheelAccumulator_ = 0;
        subdivisionPixelAccumulator_ = 0.0;
    }

    int subdivisionWheelStepsFromEvent(int angleDelta, int pixelDelta)
    {
        if (angleDelta != 0) {
            // Qt can split one physical wheel detent into several smooth
            // angle deltas. Blender's modal tools receive normalized wheel
            // events, so retain only the incomplete part here and emit one
            // logical step for each complete 120-unit detent.
            subdivisionPixelAccumulator_ = 0.0;
            if (subdivisionWheelAccumulator_ != 0 &&
                ((subdivisionWheelAccumulator_ > 0) != (angleDelta > 0))) {
                subdivisionWheelAccumulator_ = 0;
            }

            subdivisionWheelAccumulator_ += angleDelta;
            const int steps = subdivisionWheelAccumulator_ / 120;
            subdivisionWheelAccumulator_ -= steps * 120;
            return steps;
        }

        if (pixelDelta != 0) {
            // A pure pixel-delta stream has no platform-independent detent
            // size. Keep the same discrete behavior with a conservative
            // screen-pixel threshold for touch/high-resolution devices.
            subdivisionWheelAccumulator_ = 0;
            if (subdivisionPixelAccumulator_ != 0.0 &&
                ((subdivisionPixelAccumulator_ > 0.0) != (pixelDelta > 0))) {
                subdivisionPixelAccumulator_ = 0.0;
            }

            subdivisionPixelAccumulator_ += pixelDelta;
            constexpr qreal pixelsPerWheelStep = 40.0;
            const int magnitude = static_cast<int>(std::floor(
                std::abs(subdivisionPixelAccumulator_) / pixelsPerWheelStep));
            const int steps = subdivisionPixelAccumulator_ > 0.0 ? magnitude : -magnitude;
            subdivisionPixelAccumulator_ -= steps * pixelsPerWheelStep;
            return steps;
        }

        return 0;
    }

    void applySubdivisionWheelSteps(int steps)
    {
        if (!subdivisionActive_ || steps == 0) {
            return;
        }

        const int previousSections = subdivisionSections_;
        subdivisionSections_ = std::clamp(subdivisionSections_ + steps,
                                          2,
                                          maxSubdivisionSections);
        if (subdivisionSections_ != previousSections) {
            notifySubdivisionStatus();
            update();
            DebugLog::instance().write(
                QStringLiteral("subdivision wheel sections=%1 points=%2")
                    .arg(subdivisionSections_)
                    .arg(subdivisionSections_ - 1));
        }
    }

    void notifySubdivisionStatus()
    {
        if (subdivisionStatusUpdate_) {
            subdivisionStatusUpdate_(subdivisionStatusText());
        }
    }

    void cancelSubdivisionPreview()
    {
        if (!subdivisionActive_) {
            return;
        }

        subdivisionActive_ = false;
        subdivisionShapeIndex_ = ObjectId::invalid();
        subdivisionSections_ = 2;
        resetSubdivisionWheelTracking();
        notifySubdivisionStatus();
        update();
        DebugLog::instance().write(QStringLiteral("cancelSubdivisionPreview"));
    }

    void notifyHistoryChanged()
    {
        if (historyChanged_) {
            historyChanged_();
        }
    }

    void notifyLayersChanged()
    {
        if (layersChanged_) {
            layersChanged_();
        }
    }

    void recordLayerChange()
    {
        history_.record();
        notifyHistoryChanged();
    }

    void recordGeometryChange()
    {
        recordGeometrySnapshot(document_.snapshot());
    }

    void recordGeometrySnapshot(const Document::Snapshot &snapshot)
    {
        history_.record(snapshot);
        notifyHistoryChanged();
        QTimer::singleShot(0, this, [this]() {
            notifyLayersChanged();
        });
        DebugLog::instance().write(QStringLiteral("history record shapes=%1 undoAvailable=%2 redoCleared")
                                       .arg(shapes_.size())
                                       .arg(history_.undoCount()));
    }

    void beginDragHistory()
    {
        if (grabActive_) {
            return;
        }
        if (!dragHistoryRecorded_) {
            recordGeometryChange();
            dragHistoryRecorded_ = true;
        }
    }

    void resetGrabInteraction()
    {
        grabActive_ = false;
        grabMoved_ = false;
        grabPickingBasePoint_ = false;
        grabHasBasePoint_ = false;
        grabBasePoint_ = QPointF();
        grabCursorOffset_ = QPointF();
    }

    void resetDuplicateInteraction()
    {
        duplicateActive_ = false;
        duplicatePickingBasePoint_ = false;
        duplicateHasBasePoint_ = false;
        duplicateSourceObjects_.clear();
        duplicatePreviewShapes_.clear();
        duplicateBasePoint_ = QPointF();
        duplicateCursorOffset_ = QPointF();
        duplicateDestination_ = QPointF();
    }

    void resetInteractionAfterHistory()
    {
        pendingPoints_.clear();
        selectedShapeIndices_.clear();
        selectedShapeIndex_ = ObjectId::invalid();
        selectionBoxActive_ = false;
        selectionBoxMoved_ = false;
        selectionBoxAdditive_ = false;
        trimBoxSelectionActive_ = false;
        draggingSelected_ = false;
        draggingShapeIndices_.clear();
        dragHistoryRecorded_ = false;
        currentSnap_ = SnapResult{};
        currentDragSnap_ = DragSnapResult{};
        dragSnapLocked_ = false;
        dragAxisLock_ = DragAxisLock::None;
        resetGrabInteraction();
        draggingControlPoint_ = false;
        controlPointIndex_ = -1;
        eraseStrokeActive_ = false;
        eraseCursorPressed_ = false;
        eraseCandidateShapeIndices_.clear();
        eraseStrokeScreenPath_.clear();
        eraseTargetShapeIndices_.clear();
        eraseSceneCurveCaches_.clear();
        eraseTargetCurveCaches_.clear();
        eraseGeometryCachePrepared_ = false;
        trimHoverPositionValid_ = false;
        joinActive_ = false;
        joinShapeIndices_.clear();
        resetScaleInteraction();
        resetRotateInteraction();
        resetMirrorInteraction();
        subdivisionActive_ = false;
        subdivisionShapeIndex_ = ObjectId::invalid();
        subdivisionSections_ = 2;
        resetSubdivisionWheelTracking();
        lineCommandActive_ = false;
        setTool(Tool::Select);
        if (commandFinished_) {
            commandFinished_(Tool::Select);
        }
        update();
    }

    void finishLineCommand()
    {
        DebugLog::instance().write(QStringLiteral("finishLineCommand begin points=%1")
                                       .arg(pendingPoints_.size()));
        if (pendingPoints_.size() >= 2) {
            const Shape::NurbsCurve2D curve = makeDegreeOneNurbs(pendingPoints_);
            recordGeometryChange();
            Shape line{GeometryType::Line,
                       pendingPoints_,
                       curve,
                       ArcMode::TwoPoint,
                       0.0,
                       {},
                       {}};
            line.workPlane = viewportTransform_.workPlane();
            line.workPlaneOffset = viewportTransform_.workPlaneOffset();
            shapes_.append(line);
            DebugLog::instance().write(
                QStringLiteral("finishLineCommand committed dimension=%1 degree=%2 order=%3 rational=%4 controlPoints=%5 weights=%6 knots=%7 shapes=%8")
                    .arg(curve.dimension)
                    .arg(curve.degree)
                    .arg(curve.order)
                    .arg(curve.rational)
                    .arg(curve.controlPoints.size())
                    .arg(curve.weights.size())
                    .arg(curve.knots.size())
                    .arg(shapes_.size()));
        } else {
            DebugLog::instance().write(QStringLiteral("finishLineCommand discarded insufficient points"));
        }

        pendingPoints_.clear();
        setTool(Tool::Select);
        if (commandFinished_) {
            commandFinished_(Tool::Select);
        }
        DebugLog::instance().write(QStringLiteral("finishLineCommand end tool=%1 lineActive=%2 points=%3")
                                       .arg(toolName(activeTool_))
                                       .arg(lineCommandActive_)
                                       .arg(pendingPoints_.size()));
    }

    bool makeArcSnapGeometry(const Shape &shape,
                             QPointF *centerScreen,
                             qreal *radius,
                             qreal *startAngle,
                             qreal *sweepAngle) const
    {
        if (shape.geometryType != GeometryType::Arc || shape.points.size() < 3) {
            return false;
        }

        if (shape.arcMode == ArcMode::TwoPoint) {
            return makeCircularArcGeometry(shape.points[0],
                                           shape.points[1],
                                           shape.points[2],
                                           centerScreen,
                                           radius,
                                           startAngle,
                                           sweepAngle);
        }

        const QPointF center = worldToScreen(shape.points[0]);
        const QPointF start = worldToScreen(shape.points[1]);
        const QPointF end = worldToScreen(shape.points[2]);
        const qreal arcRadius = std::hypot(start.x() - center.x(),
                                           start.y() - center.y());
        if (arcRadius <= 1e-9) {
            return false;
        }

        constexpr qreal pi = 3.14159265358979323846;
        constexpr qreal twoPi = 2.0 * pi;
        const qreal firstAngle = std::atan2(start.y() - center.y(),
                                            start.x() - center.x());
        qreal selectedSweep = shape.arcSweep;
        if (std::abs(selectedSweep) <= 1e-9) {
            const qreal endAngle = std::atan2(end.y() - center.y(),
                                              end.x() - center.x());
            selectedSweep = endAngle - firstAngle;
            if (selectedSweep > pi) {
                selectedSweep -= twoPi;
            } else if (selectedSweep < -pi) {
                selectedSweep += twoPi;
            }
        }

        if (centerScreen != nullptr) {
            *centerScreen = center;
        }
        if (radius != nullptr) {
            *radius = arcRadius;
        }
        if (startAngle != nullptr) {
            *startAngle = firstAngle;
        }
        if (sweepAngle != nullptr) {
            *sweepAngle = selectedSweep;
        }
        return true;
    }

    bool arcAngleIsOnSweep(qreal startAngle, qreal sweepAngle, qreal angle) const
    {
        constexpr qreal twoPi = 6.28318530717958647692;
        constexpr qreal epsilon = 1e-7;
        if (std::abs(sweepAngle) >= twoPi - epsilon) {
            return true;
        }

        const auto positiveAngle = [twoPi](qreal value) {
            value = std::fmod(value, twoPi);
            if (value < 0.0) {
                value += twoPi;
            }
            return value;
        };

        if (sweepAngle >= 0.0) {
            return positiveAngle(angle - startAngle) <= sweepAngle + epsilon;
        }

        return positiveAngle(startAngle - angle) <= -sweepAngle + epsilon;
    }

    bool arcSnapPointAtFraction(const Shape &shape,
                                qreal fraction,
                                QPointF *point) const
    {
        QPointF center;
        qreal radius = 0.0;
        qreal startAngle = 0.0;
        qreal sweepAngle = 0.0;
        if (!makeArcSnapGeometry(shape,
                                 &center,
                                 &radius,
                                 &startAngle,
                                 &sweepAngle)) {
            return false;
        }

        const qreal angle = startAngle + sweepAngle * fraction;
        if (point != nullptr) {
            *point = screenToWorld(QPointF(center.x() + radius * std::cos(angle),
                                           center.y() + radius * std::sin(angle)));
        }
        return true;
    }

    Shape::NurbsCurve2D makeArcNurbsCurve(const Shape &shape) const
    {
        Shape::NurbsCurve2D curve;
        curve.dimension = 2;
        curve.degree = 2;
        curve.order = 3;
        curve.rational = true;

        QPointF centerScreen;
        qreal radius = 0.0;
        qreal startAngle = 0.0;
        qreal sweepAngle = 0.0;
        if (!makeArcSnapGeometry(shape,
                                 &centerScreen,
                                 &radius,
                                 &startAngle,
                                 &sweepAngle) ||
            radius <= 1e-9 || std::abs(sweepAngle) <= 1e-9) {
            return curve;
        }

        constexpr qreal pi = 3.14159265358979323846;
        constexpr qreal halfPi = pi / 2.0;
        const int spanCount = std::max(1, static_cast<int>(std::ceil(
            std::abs(sweepAngle) / halfPi)));
        const qreal spanSweep = sweepAngle / spanCount;

        const auto screenPointAt = [centerScreen, radius](qreal angle) {
            return QPointF(centerScreen.x() + radius * std::cos(angle),
                           centerScreen.y() + radius * std::sin(angle));
        };

        curve.controlPoints.reserve(spanCount * 2 + 1);
        curve.weights.reserve(spanCount * 2 + 1);
        for (int span = 0; span < spanCount; ++span) {
            const qreal spanStart = startAngle + spanSweep * span;
            const qreal spanEnd = spanStart + spanSweep;
            const qreal spanMiddle = (spanStart + spanEnd) * 0.5;
            const qreal middleWeight = std::cos(std::abs(spanSweep) * 0.5);

            if (span == 0) {
                curve.controlPoints.append(screenToWorld(screenPointAt(spanStart)));
                curve.weights.append(1.0);
            }

            // A circular span is an exact rational quadratic Bezier. The
            // middle CV lies outside the circle and its weight controls the
            // pull back onto the circle. Adjacent spans share their endpoint.
            const qreal middleRadius = radius / middleWeight;
            const QPointF middleScreen(
                centerScreen.x() + middleRadius * std::cos(spanMiddle),
                centerScreen.y() + middleRadius * std::sin(spanMiddle));
            curve.controlPoints.append(screenToWorld(middleScreen));
            curve.weights.append(middleWeight);
            curve.controlPoints.append(screenToWorld(screenPointAt(spanEnd)));
            curve.weights.append(1.0);
        }

        // Clamped knot vector for piecewise rational quadratic Bezier spans:
        // endpoint multiplicity 3 and internal knot multiplicity 2. The
        // first and last redundant entries are omitted, as in openNURBS.
        const qreal knotDelta = std::abs(spanSweep);
        curve.knots.reserve(curve.controlPoints.size() + curve.degree - 1);
        curve.knots.append(0.0);
        curve.knots.append(0.0);
        for (int knot = 1; knot < spanCount; ++knot) {
            const double parameter = knotDelta * knot;
            curve.knots.append(parameter);
            curve.knots.append(parameter);
        }
        const double endParameter = std::abs(sweepAngle);
        curve.knots.append(endParameter);
        curve.knots.append(endParameter);

        return curve;
    }

    QVector<SnapCandidate> snapCandidatesForShape(const Shape &shape) const
    {
        return snapEngine_.snapCandidatesForShape(shape,
                                                  viewportTransform_,
                                                  size());

        QVector<SnapCandidate> candidates;
        if (shape.points.isEmpty()) {
            return candidates;
        }

        Shape::NurbsCurve2D subdivisionCurveData;
        if (subdivisionCurve(shape, &subdivisionCurveData)) {
            for (const double parameter : shape.subdivisionParameters) {
                QPointF point;
                if (evaluateNurbsPoint(subdivisionCurveData, parameter, &point)) {
                    candidates.append(SnapCandidate{SnapType::Endpoint, point});
                }
            }
        }

        if (shape.geometryType == GeometryType::Point) {
            candidates.append(SnapCandidate{SnapType::Endpoint, shape.points.first()});
            return candidates;
        }

        if (shape.geometryType == GeometryType::PolyCurve) {
            for (const Shape::NurbsCurve2D &component : shape.components) {
                QPointF start;
                QPointF end;
                if (nurbsCurveEndpoints(component, &start, &end)) {
                    candidates.append(SnapCandidate{SnapType::Endpoint, start});
                    candidates.append(SnapCandidate{SnapType::Endpoint, end});
                }
                QPointF midpoint;
                if (nurbsCurvePointAtFraction(component, 0.5, &midpoint)) {
                    candidates.append(SnapCandidate{SnapType::Midpoint, midpoint});
                }
            }
            return candidates;
        }

        if (shape.geometryType == GeometryType::Circle) {
            candidates.append(SnapCandidate{SnapType::Center, shape.points.first()});
            return candidates;
        }

        if (shape.geometryType == GeometryType::Rectangle) {
            const QVector<QPointF> vertices = rectangleVertices(shape);
            for (const QPointF &vertex : vertices) {
                candidates.append(SnapCandidate{SnapType::Endpoint, vertex});
            }
            for (int index = 0; index < vertices.size(); ++index) {
                candidates.append(SnapCandidate{
                    SnapType::Midpoint,
                    (vertices[index] + vertices[(index + 1) % vertices.size()]) / 2.0});
            }
            return candidates;
        }

        if (shape.geometryType == GeometryType::Arc && shape.points.size() >= 3) {
            QPointF start = shape.arcMode == ArcMode::OnePoint
                                ? shape.points[1]
                                : shape.points[0];
            QPointF end = shape.arcMode == ArcMode::OnePoint
                              ? shape.points[2]
                              : shape.points[1];
            // The stored NURBS is the rendered source of truth. In
            // particular, this remains correct after a control point edit,
            // when the legacy three-point arc definition no longer describes
            // the displayed curve exactly.
            if (!nurbsCurveEndpoints(shape.nurbs, &start, &end)) {
                QPointF evaluatedEndpoint;
                if (arcSnapPointAtFraction(shape, 0.0, &evaluatedEndpoint)) {
                    start = evaluatedEndpoint;
                }
                if (arcSnapPointAtFraction(shape, 1.0, &evaluatedEndpoint)) {
                    end = evaluatedEndpoint;
                }
            }
            candidates.append(SnapCandidate{SnapType::Endpoint, start});
            candidates.append(SnapCandidate{SnapType::Endpoint, end});

            QPointF midpoint;
            if (arcSnapPointAtFraction(shape, 0.5, &midpoint)) {
                candidates.append(SnapCandidate{SnapType::Midpoint, midpoint});
            }

            QPointF center;
            if (makeArcSnapGeometry(shape, &center, nullptr, nullptr, nullptr)) {
                candidates.append(SnapCandidate{SnapType::Center, screenToWorld(center)});
            }
            return candidates;
        }

        if (shape.geometryType != GeometryType::Line) {
            return candidates;
        }

        QVector<LineSegment> segments;
        // A moved object may use any of its snap points as the source. The
        // OSnap toggles control which target types are eligible below.
        for (const QPointF &point : shape.points) {
            candidates.append(SnapCandidate{SnapType::Endpoint, point});
        }

        for (int index = 0; index + 1 < shape.points.size(); ++index) {
            const QPointF start = shape.points[index];
            const QPointF end = shape.points[index + 1];
            segments.append(LineSegment{start, end});

            candidates.append(SnapCandidate{SnapType::Midpoint, (start + end) / 2.0});
        }

        for (int first = 0; first < segments.size(); ++first) {
            for (int second = first + 1; second < segments.size(); ++second) {
                QPointF intersection;
                if (segmentIntersection(segments[first].start,
                                        segments[first].end,
                                        segments[second].start,
                                        segments[second].end,
                                        &intersection)) {
                    candidates.append(SnapCandidate{SnapType::Intersection, intersection});
                }
            }
        }

        return candidates;
    }

    QVector<SnapCandidate> snapCandidatesForScene(int excludedShapeIndex = -1) const
    {
        const QVector<int> excludedShapeIndices = excludedShapeIndex >= 0
                                                      ? QVector<int>{excludedShapeIndex}
                                                      : QVector<int>{};
        return snapCandidatesForScene(excludedShapeIndices);
    }

    QVector<SnapCandidate> snapCandidatesForScene(
        const QVector<int> &excludedShapeIndices) const
    {
        return snapEngine_.snapCandidatesForScene(document_,
                                                  excludedShapeIndices,
                                                  viewportTransform_,
                                                  size());

        QVector<SnapCandidate> candidates;
        QVector<LineSegment> segments;

        for (int shapeIndex = 0; shapeIndex < shapes_.size(); ++shapeIndex) {
            if (excludedShapeIndices.contains(shapeIndex)) {
                continue;
            }

            const Shape &shape = shapes_[shapeIndex];
            if (shape.points.isEmpty()) {
                continue;
            }

            if (endpointSnapEnabled_) {
                Shape::NurbsCurve2D subdivisionCurveData;
                if (subdivisionCurve(shape, &subdivisionCurveData)) {
                    for (const double parameter : shape.subdivisionParameters) {
                        QPointF point;
                        if (evaluateNurbsPoint(subdivisionCurveData, parameter, &point)) {
                            candidates.append(SnapCandidate{SnapType::Endpoint, point});
                        }
                    }
                }
            }

            if (shape.geometryType == GeometryType::Point) {
                if (endpointSnapEnabled_) {
                    candidates.append(SnapCandidate{SnapType::Endpoint, shape.points.first()});
                }
                continue;
            }

            if (shape.geometryType == GeometryType::PolyCurve) {
                for (const Shape::NurbsCurve2D &component : shape.components) {
                    QPointF start;
                    QPointF end;
                    if (endpointSnapEnabled_ &&
                        nurbsCurveEndpoints(component, &start, &end)) {
                        candidates.append(SnapCandidate{SnapType::Endpoint, start});
                        candidates.append(SnapCandidate{SnapType::Endpoint, end});
                    }
                    if (midpointSnapEnabled_) {
                        QPointF midpoint;
                        if (nurbsCurvePointAtFraction(component, 0.5, &midpoint)) {
                            candidates.append(SnapCandidate{SnapType::Midpoint, midpoint});
                        }
                    }
                }
                continue;
            }

            if (shape.geometryType == GeometryType::Circle) {
                if (centerSnapEnabled_) {
                    candidates.append(SnapCandidate{SnapType::Center, shape.points.first()});
                }
                continue;
            }

            if (shape.geometryType == GeometryType::Rectangle) {
                const QVector<QPointF> vertices = rectangleVertices(shape);
                if (endpointSnapEnabled_) {
                    for (const QPointF &vertex : vertices) {
                        candidates.append(SnapCandidate{SnapType::Endpoint, vertex});
                    }
                }
                for (int index = 0; index < vertices.size(); ++index) {
                    const QPointF start = vertices[index];
                    const QPointF end = vertices[(index + 1) % vertices.size()];
                    segments.append(LineSegment{start, end});
                    if (midpointSnapEnabled_) {
                        candidates.append(SnapCandidate{
                            SnapType::Midpoint,
                            (start + end) / 2.0});
                    }
                }
                continue;
            }

            if (shape.geometryType == GeometryType::Arc && shape.points.size() >= 3) {
                QPointF start = shape.arcMode == ArcMode::OnePoint
                                    ? shape.points[1]
                                    : shape.points[0];
                QPointF end = shape.arcMode == ArcMode::OnePoint
                                  ? shape.points[2]
                                  : shape.points[1];
                // Use the same NURBS data that drawShape() renders. The
                // legacy arc construction points may be stale after control
                // point editing.
                if (!nurbsCurveEndpoints(shape.nurbs, &start, &end)) {
                    QPointF evaluatedEndpoint;
                    if (arcSnapPointAtFraction(shape, 0.0, &evaluatedEndpoint)) {
                        start = evaluatedEndpoint;
                    }
                    if (arcSnapPointAtFraction(shape, 1.0, &evaluatedEndpoint)) {
                        end = evaluatedEndpoint;
                    }
                }

                if (endpointSnapEnabled_) {
                    candidates.append(SnapCandidate{SnapType::Endpoint, start});
                    candidates.append(SnapCandidate{SnapType::Endpoint, end});
                }

                if (midpointSnapEnabled_) {
                    QPointF midpoint;
                    if (arcSnapPointAtFraction(shape, 0.5, &midpoint)) {
                        candidates.append(SnapCandidate{SnapType::Midpoint, midpoint});
                    }
                }

                if (centerSnapEnabled_) {
                    QPointF center;
                    if (makeArcSnapGeometry(shape, &center, nullptr, nullptr, nullptr)) {
                        candidates.append(SnapCandidate{
                            SnapType::Center,
                            screenToWorld(center)});
                    }
                }

                continue;
            }

            if (shape.geometryType != GeometryType::Line) {
                continue;
            }

            if (endpointSnapEnabled_) {
                for (const QPointF &point : shape.points) {
                    candidates.append(SnapCandidate{SnapType::Endpoint, point});
                }
            }

            for (int index = 0; index + 1 < shape.points.size(); ++index) {
                const QPointF start = shape.points[index];
                const QPointF end = shape.points[index + 1];
                segments.append(LineSegment{start, end});

                if (midpointSnapEnabled_) {
                    candidates.append(SnapCandidate{SnapType::Midpoint, (start + end) / 2.0});
                }
            }
        }

        if (intersectionSnapEnabled_) {
            for (int first = 0; first < segments.size(); ++first) {
                for (int second = first + 1; second < segments.size(); ++second) {
                    QPointF intersection;
                    if (segmentIntersection(segments[first].start,
                                            segments[first].end,
                                            segments[second].start,
                                            segments[second].end,
                                            &intersection)) {
                        candidates.append(SnapCandidate{SnapType::Intersection, intersection});
                    }
                }
            }
        }

        return candidates;
    }

    QVector<SnapCandidate> perpendicularCandidates(const QPointF &origin,
                                                    const QPointF &cursor) const
    {
        return snapEngine_.perpendicularCandidates(document_,
                                                   origin,
                                                   cursor,
                                                   viewportTransform_,
                                                   size());

        QVector<SnapCandidate> candidates;
        if (!perpendicularSnapEnabled_) {
            return candidates;
        }

        constexpr qreal epsilon = 1e-9;

        for (const Shape &shape : shapes_) {
            if (shape.points.isEmpty()) {
                continue;
            }

            if (shape.geometryType == GeometryType::Circle && shape.points.size() >= 2) {
                const QPointF center = shape.points[0];
                const QPointF edge = shape.points[1];
                const qreal radius = std::hypot(edge.x() - center.x(),
                                                edge.y() - center.y());
                if (radius <= epsilon) {
                    continue;
                }

                const QPointF fromCenter = origin - center;
                const qreal distanceFromCenter =
                    std::hypot(fromCenter.x(), fromCenter.y());

                if (distanceFromCenter <= epsilon) {
                    // From a circle center, every radius is perpendicular to
                    // the circumference. Use the cursor direction to choose
                    // which point on the circumference to target.
                    const QPointF towardCursor = cursor - center;
                    const qreal cursorDistance =
                        std::hypot(towardCursor.x(), towardCursor.y());
                    if (cursorDistance > epsilon) {
                        candidates.append(SnapCandidate{
                            SnapType::Perpendicular,
                            center + towardCursor * (radius / cursorDistance)});
                    }
                } else {
                    const QPointF radialDirection = fromCenter / distanceFromCenter;
                    candidates.append(SnapCandidate{
                        SnapType::Perpendicular,
                        center + radialDirection * radius});
                    candidates.append(SnapCandidate{
                        SnapType::Perpendicular,
                        center - radialDirection * radius});
                }

                continue;
            }

            if (shape.geometryType == GeometryType::Arc && shape.points.size() >= 3) {
                QPointF center;
                qreal radius = 0.0;
                qreal startAngle = 0.0;
                qreal sweepAngle = 0.0;
                if (!makeArcSnapGeometry(shape,
                                         &center,
                                         &radius,
                                         &startAngle,
                                         &sweepAngle)) {
                    continue;
                }

                const QPointF originScreen = worldToScreen(origin);
                const QPointF fromCenter = originScreen - center;
                const qreal distanceFromCenter =
                    std::hypot(fromCenter.x(), fromCenter.y());

                const auto appendIfOnArc = [&](const QPointF &candidateScreen) {
                    const qreal candidateAngle =
                        std::atan2(candidateScreen.y() - center.y(),
                                   candidateScreen.x() - center.x());
                    if (arcAngleIsOnSweep(startAngle, sweepAngle, candidateAngle)) {
                        candidates.append(SnapCandidate{
                            SnapType::Perpendicular,
                            screenToWorld(candidateScreen)});
                    }
                };

                if (distanceFromCenter <= epsilon) {
                    const QPointF cursorScreen = worldToScreen(cursor);
                    const QPointF towardCursor = cursorScreen - center;
                    const qreal cursorDistance =
                        std::hypot(towardCursor.x(), towardCursor.y());
                    if (cursorDistance > epsilon) {
                        appendIfOnArc(center + towardCursor * (radius / cursorDistance));
                    }
                } else {
                    const QPointF radialDirection = fromCenter / distanceFromCenter;
                    appendIfOnArc(center + radialDirection * radius);
                    appendIfOnArc(center - radialDirection * radius);
                }

                continue;
            }

            if (shape.geometryType != GeometryType::Line) {
                continue;
            }

            for (int index = 0; index + 1 < shape.points.size(); ++index) {
                const QPointF start = shape.points[index];
                const QPointF end = shape.points[index + 1];
                const QPointF direction = end - start;
                const qreal lengthSquared = direction.x() * direction.x() +
                                            direction.y() * direction.y();
                if (lengthSquared <= epsilon) {
                    continue;
                }

                const QPointF fromStart = origin - start;
                const qreal projection =
                    (fromStart.x() * direction.x() + fromStart.y() * direction.y()) /
                    lengthSquared;
                if (projection < -epsilon || projection > 1.0 + epsilon) {
                    continue;
                }

                const QPointF foot = start + direction * std::clamp(projection, 0.0, 1.0);
                if (std::hypot(origin.x() - foot.x(), origin.y() - foot.y()) <= epsilon) {
                    continue;
                }

                candidates.append(SnapCandidate{SnapType::Perpendicular, foot});
            }
        }

        return candidates;
    }

    QVector<SnapCandidate> tangentCandidates(const QPointF &origin) const
    {
        return snapEngine_.tangentCandidates(document_,
                                              origin,
                                              viewportTransform_,
                                              size());

        QVector<SnapCandidate> candidates;
        if (!tangentSnapEnabled_) {
            return candidates;
        }

        constexpr qreal epsilon = 1e-9;

        for (const Shape &shape : shapes_) {
            if (shape.geometryType == GeometryType::Circle && shape.points.size() >= 2) {
                const QPointF center = shape.points[0];
                const QPointF edge = shape.points[1];
                const qreal radius = std::hypot(edge.x() - center.x(),
                                                edge.y() - center.y());
                if (radius <= epsilon) {
                    continue;
                }

                const QPointF fromCenter = origin - center;
                const qreal distanceFromCenter =
                    std::hypot(fromCenter.x(), fromCenter.y());
                if (distanceFromCenter < radius - epsilon) {
                    // A point inside a circle has no real tangent points.
                    continue;
                }

                if (distanceFromCenter <= epsilon) {
                    continue;
                }

                const QPointF radialDirection = fromCenter / distanceFromCenter;
                const QPointF tangentDirection(-radialDirection.y(), radialDirection.x());
                const qreal radiusRatio = radius / distanceFromCenter;
                const qreal radialDistance = radius * radiusRatio;
                const qreal tangentDistance =
                    radius * std::sqrt(std::max(0.0, 1.0 - radiusRatio * radiusRatio));

                candidates.append(SnapCandidate{
                    SnapType::Tangent,
                    center + radialDirection * radialDistance + tangentDirection * tangentDistance});

                if (tangentDistance > epsilon) {
                    candidates.append(SnapCandidate{
                        SnapType::Tangent,
                        center + radialDirection * radialDistance - tangentDirection * tangentDistance});
                }
                continue;
            }

            if (shape.geometryType != GeometryType::Arc || shape.points.size() < 3) {
                continue;
            }

            QPointF centerScreen;
            qreal radius = 0.0;
            qreal startAngle = 0.0;
            qreal sweepAngle = 0.0;
            if (!makeArcSnapGeometry(shape,
                                     &centerScreen,
                                     &radius,
                                     &startAngle,
                                     &sweepAngle)) {
                continue;
            }

            const QPointF originScreen = worldToScreen(origin);
            const QPointF fromCenter = originScreen - centerScreen;
            const qreal distanceFromCenter =
                std::hypot(fromCenter.x(), fromCenter.y());
            if (distanceFromCenter < radius - epsilon ||
                distanceFromCenter <= epsilon) {
                continue;
            }

            const QPointF radialDirection = fromCenter / distanceFromCenter;
            const QPointF tangentDirection(-radialDirection.y(), radialDirection.x());
            const qreal radiusRatio = radius / distanceFromCenter;
            const qreal radialDistance = radius * radiusRatio;
            const qreal tangentDistance =
                radius * std::sqrt(std::max(0.0, 1.0 - radiusRatio * radiusRatio));

            const auto appendIfOnArc = [&](const QPointF &candidateScreen) {
                const qreal candidateAngle =
                    std::atan2(candidateScreen.y() - centerScreen.y(),
                               candidateScreen.x() - centerScreen.x());
                if (arcAngleIsOnSweep(startAngle, sweepAngle, candidateAngle)) {
                    candidates.append(SnapCandidate{
                        SnapType::Tangent,
                        screenToWorld(candidateScreen)});
                }
            };

            appendIfOnArc(centerScreen + radialDirection * radialDistance +
                          tangentDirection * tangentDistance);
            if (tangentDistance > epsilon) {
                appendIfOnArc(centerScreen + radialDirection * radialDistance -
                              tangentDirection * tangentDistance);
            }
        }

        return candidates;
    }

    SnapResult findSnapPoint(const QPointF &rawPoint) const
    {
        const bool serviceDrawingSnapActive =
            (activeTool_ == Tool::Line && lineCommandActive_) ||
            activeTool_ == Tool::Arc || isCircleConstructionTool(activeTool_) ||
            activeTool_ == Tool::Point || activeTool_ == Tool::Rotate ||
            activeTool_ == Tool::Scale ||
            isEllipseTool(activeTool_) || isRectangleTool(activeTool_) ||
            activeTool_ == Tool::Picture ||
            activeTool_ == Tool::Mirror || isDimensionTool(activeTool_) ||
            activeTool_ == Tool::TangentFromCurve ||
            activeTool_ == Tool::PerpendicularFromCurve;
        return snapEngine_.findSnapPoint(document_,
                                         rawPoint,
                                         serviceDrawingSnapActive,
                                         pendingPoints_,
                                         viewportTransform_,
                                         size());

        SnapResult best;
        const bool drawingSnapActive =
            (activeTool_ == Tool::Line && lineCommandActive_) ||
            activeTool_ == Tool::Arc || isCircleConstructionTool(activeTool_) ||
            activeTool_ == Tool::Point || activeTool_ == Tool::Rotate;
        if (!osnapEnabled_ || !drawingSnapActive) {
            return best;
        }

        const QPointF cursorScreen = worldToScreen(rawPoint);
        constexpr qreal snapRadiusPixels = 12.0;
        qreal bestDistance = snapRadiusPixels;

        const auto consider = [&](const SnapCandidate &candidate) {
            const QPointF candidateScreen = worldToScreen(candidate.point);
            const qreal distance = std::hypot(candidateScreen.x() - cursorScreen.x(),
                                               candidateScreen.y() - cursorScreen.y());
            if (distance <= bestDistance) {
                bestDistance = distance;
                best.type = candidate.type;
                best.point = candidate.point;
            }
        };

        for (const SnapCandidate &candidate : snapCandidatesForScene()) {
            consider(candidate);
        }

        if (!pendingPoints_.isEmpty()) {
            for (const SnapCandidate &candidate :
                 perpendicularCandidates(pendingPoints_.back(), rawPoint)) {
                consider(candidate);
            }

            for (const SnapCandidate &candidate :
                 tangentCandidates(pendingPoints_.back())) {
                consider(candidate);
            }
        }

        return best;
    }

    SnapResult closestSnapCandidate(const QPointF &rawPoint,
                                    const QVector<SnapCandidate> &candidates) const
    {
        SnapResult best;
        const QPointF cursorScreen = worldToScreen(rawPoint);
        constexpr qreal snapRadiusPixels = 12.0;
        qreal bestDistance = snapRadiusPixels;
        for (const SnapCandidate &candidate : candidates) {
            const QPointF candidateScreen = worldToScreen(candidate.point);
            const qreal distance = std::hypot(candidateScreen.x() - cursorScreen.x(),
                                              candidateScreen.y() - cursorScreen.y());
            if (distance <= bestDistance) {
                bestDistance = distance;
                best.type = candidate.type;
                best.point = candidate.point;
            }
        }
        return best;
    }

    QVector<int> grabSelectedShapeIndices() const
    {
        QVector<int> selectedIndices;
        selectedIndices.reserve(draggingShapeIndices_.size());
        for (const ObjectId objectId : draggingShapeIndices_) {
            const int shapeIndex = objectIndex(objectId);
            if (shapeIndex >= 0) {
                selectedIndices.append(shapeIndex);
            }
        }
        return selectedIndices;
    }

    SnapResult findGrabBasePointSnap(const QPointF &rawPoint) const
    {
        QVector<int> excludedShapeIndices;
        const QVector<int> selectedIndices = grabSelectedShapeIndices();
        for (int shapeIndex = 0; shapeIndex < document_.size(); ++shapeIndex) {
            if (!selectedIndices.contains(shapeIndex)) {
                excludedShapeIndices.append(shapeIndex);
            }
        }
        const QVector<SnapCandidate> candidates =
            snapEngine_.snapCandidatesForScene(document_,
                                               excludedShapeIndices,
                                               viewportTransform_,
                                               size());
        return closestSnapCandidate(rawPoint, candidates);
    }

    QVector<int> duplicateSourceShapeIndices() const
    {
        QVector<int> selectedIndices;
        selectedIndices.reserve(duplicateSourceObjects_.size());
        for (const SceneObject &source : duplicateSourceObjects_) {
            const int shapeIndex = objectIndex(source.id);
            if (shapeIndex >= 0) {
                selectedIndices.append(shapeIndex);
            }
        }
        return selectedIndices;
    }

    SnapResult findDuplicateBasePointSnap(const QPointF &rawPoint) const
    {
        QVector<int> excludedShapeIndices;
        const QVector<int> selectedIndices = duplicateSourceShapeIndices();
        for (int shapeIndex = 0; shapeIndex < document_.size(); ++shapeIndex) {
            if (!selectedIndices.contains(shapeIndex)) {
                excludedShapeIndices.append(shapeIndex);
            }
        }
        const QVector<SnapCandidate> candidates =
            snapEngine_.snapCandidatesForScene(document_,
                                               excludedShapeIndices,
                                               viewportTransform_,
                                               size());
        return closestSnapCandidate(rawPoint, candidates);
    }

    SnapResult findDuplicateDestinationSnap(const QPointF &rawPoint) const
    {
        const QVector<int> selectedIndices = duplicateSourceShapeIndices();
        bool selectedLine = false;
        for (const int shapeIndex : selectedIndices) {
            if (shapeIndex >= 0 && shapeIndex < document_.size() &&
                document_[shapeIndex].geometryType == GeometryType::Line) {
                selectedLine = true;
                break;
            }
        }
        return snapEngine_.findSnapPoint(document_,
                                         rawPoint,
                                         true,
                                         QVector<QPointF>{duplicateBasePoint_},
                                         viewportTransform_,
                                         size(),
                                         selectedIndices,
                                         true,
                                         !selectedLine);
    }

    SnapResult findGrabDestinationSnap(const QPointF &rawPoint) const
    {
        const QVector<int> selectedIndices = grabSelectedShapeIndices();
        bool selectedLine = false;
        for (const int shapeIndex : selectedIndices) {
            if (shapeIndex >= 0 && shapeIndex < document_.size() &&
                document_[shapeIndex].geometryType == GeometryType::Line) {
                selectedLine = true;
                break;
            }
        }
        return snapEngine_.findSnapPoint(document_,
                                         rawPoint,
                                         true,
                                         QVector<QPointF>{grabBasePoint_},
                                         viewportTransform_,
                                         size(),
                                         selectedIndices,
                                         true,
                                         !selectedLine);
    }

    DragSnapResult findDragSnap(ObjectId selectedObjectId,
                                bool forceEnabled = false) const
    {
        return findDragSnap(QVector<ObjectId>{selectedObjectId}, forceEnabled);
    }

    DragSnapResult findDragSnap(const QVector<ObjectId> &selectedObjectIds,
                                bool forceEnabled = false,
                                bool includeNear = true) const
    {
        QVector<int> serviceSelectedShapeIndices;
        serviceSelectedShapeIndices.reserve(selectedObjectIds.size());
        for (const ObjectId objectId : selectedObjectIds) {
            const int shapeIndex = objectIndex(objectId);
            if (shapeIndex >= 0) {
                serviceSelectedShapeIndices.append(shapeIndex);
            }
        }
        return snapEngine_.findDragSnap(document_,
                                        serviceSelectedShapeIndices,
                                        viewportTransform_,
                                        size(),
                                        forceEnabled,
                                        includeNear);
    }

    DragSnapResult trackNearDragSnap(const QVector<ObjectId> &selectedObjectIds,
                                     const QPointF &sourcePoint,
                                     int targetShapeIndex,
                                     int targetComponentIndex,
                                     qreal snapRadiusPixels) const
    {
        QVector<int> serviceSelectedShapeIndices;
        serviceSelectedShapeIndices.reserve(selectedObjectIds.size());
        for (const ObjectId objectId : selectedObjectIds) {
            const int shapeIndex = objectIndex(objectId);
            if (shapeIndex >= 0) {
                serviceSelectedShapeIndices.append(shapeIndex);
            }
        }
        return snapEngine_.trackNearDragSnap(document_,
                                             serviceSelectedShapeIndices,
                                             sourcePoint,
                                             targetShapeIndex,
                                             targetComponentIndex,
                                             viewportTransform_,
                                             size(),
                                             snapRadiusPixels);
    }

    DragSnapResult findControlPointSnap(ObjectId selectedObjectId,
                                        int selectedControlPointIndex,
                                        const QPointF &controlPoint) const
    {
        return snapEngine_.findControlPointSnap(document_,
                                                objectIndex(selectedObjectId),
                                                selectedControlPointIndex,
                                                controlPoint,
                                                viewportTransform_,
                                                size());
    }

    QPointF constrainLinePoint(const QPointF &rawPoint)
    {
        currentSnap_ = findSnapPoint(rawPoint);
        const QPointF snappedOrRawPoint = currentSnap_.isValid()
                                              ? currentSnap_.point
                                              : rawPoint;

        if (isEllipseTool(activeTool_) && pendingPoints_.size() >= 2) {
            const EllipseMode mode = ellipseModeForTool(activeTool_);
            if (mode == EllipseMode::CenterAxisRadius ||
                mode == EllipseMode::AxisEndpoints) {
                const QPointF center = mode == EllipseMode::CenterAxisRadius
                                           ? pendingPoints_[0]
                                           : (pendingPoints_[0] + pendingPoints_[1]) * 0.5;
                const QPointF axis = pendingPoints_[1] - pendingPoints_[0];
                const qreal axisLength = std::hypot(axis.x(), axis.y());
                if (axisLength > 1.0e-9) {
                    const QPointF majorUnit = axis / axisLength;
                    const QPointF minorUnit(-majorUnit.y(), majorUnit.x());
                    return center + minorUnit *
                                        QPointF::dotProduct(snappedOrRawPoint - center,
                                                            minorUnit);
                }
            }
            if (mode == EllipseMode::FociPoint) {
                return snappedOrRawPoint;
            }
        }

        if (activeTool_ == Tool::RectangleThreePoint && pendingPoints_.size() >= 2) {
            const QPointF edge = pendingPoints_[1] - pendingPoints_[0];
            const qreal edgeLength = std::hypot(edge.x(), edge.y());
            if (edgeLength > 1.0e-9) {
                const QPointF edgeUnit = edge / edgeLength;
                const QPointF perpendicular(-edgeUnit.y(), edgeUnit.x());
                return pendingPoints_[1] + perpendicular *
                                                QPointF::dotProduct(
                                                    snappedOrRawPoint - pendingPoints_[1],
                                                    perpendicular);
            }
        }

        if (currentSnap_.isValid()) {
            return currentSnap_.point;
        }

        const bool drawingConstraintActive =
            (activeTool_ == Tool::Line && lineCommandActive_) ||
            (activeTool_ == Tool::LinearDimension && pendingPoints_.size() == 1) ||
            (activeTool_ == Tool::AngularDimension && !pendingPoints_.isEmpty()) ||
            activeTool_ == Tool::Arc || activeTool_ == Tool::Mirror ||
            isCircleConstructionTool(activeTool_) ||
            isPolygonTool(activeTool_) ||
            (isEllipseTool(activeTool_) &&
             ellipseModeForTool(activeTool_) != EllipseMode::Corners) ||
            (activeTool_ == Tool::RectangleThreePoint && pendingPoints_.size() == 1) ||
            activeTool_ == Tool::TangentFromCurve ||
            activeTool_ == Tool::PerpendicularFromCurve;
        if (!orthoEnabled_ || panning_ || !drawingConstraintActive || pendingPoints_.isEmpty()) {
            return rawPoint;
        }

        if (activeTool_ == Tool::Arc && arcMode_ == ArcMode::OnePoint &&
            pendingPoints_.size() >= 2) {
            return constrainOnePointArcEndpoint(rawPoint);
        }

        if (activeTool_ == Tool::Arc && arcMode_ == ArcMode::TwoPoint &&
            pendingPoints_.size() >= 2) {
            return constrainTwoPointArcThroughPoint(rawPoint);
        }

        const QPointF origin = activeTool_ == Tool::AngularDimension
                                   ? pendingPoints_.first()
                                   : pendingPoints_.back();
        const qreal deltaX = rawPoint.x() - origin.x();
        const qreal deltaY = rawPoint.y() - origin.y();

        if (std::abs(deltaX) >= std::abs(deltaY)) {
            return QPointF(rawPoint.x(), origin.y());
        }

        return QPointF(origin.x(), rawPoint.y());
    }

    QPointF constrainOnePointArcEndpoint(const QPointF &rawPoint) const
    {
        const QPointF center = worldToScreen(pendingPoints_[0]);
        const QPointF start = worldToScreen(pendingPoints_[1]);
        const qreal radius = std::hypot(start.x() - center.x(),
                                        start.y() - center.y());
        if (radius <= 1e-9) {
            return rawPoint;
        }

        constexpr qreal pi = 3.14159265358979323846;
        constexpr qreal halfPi = pi / 2.0;
        constexpr qreal twoPi = 2.0 * pi;
        const qreal startAngle = std::atan2(start.y() - center.y(),
                                            start.x() - center.x());
        const QPointF raw = worldToScreen(rawPoint);
        const qreal rawAngle = std::atan2(raw.y() - center.y(),
                                          raw.x() - center.x());

        // Use the previous constrained angle as the reference for the next
        // cursor sample. This keeps the sweep unwrapped while the cursor
        // crosses +/-180 degrees, so 270 and 360 degree quarter-turns remain
        // reachable instead of jumping back to the principal angle.
        qreal candidateSweep = rawAngle - startAngle;
        if (arcPreviewInitialized_) {
            qreal delta = rawAngle - arcPreviewPreviousAngle_;
            if (delta > pi) {
                delta -= twoPi;
            } else if (delta < -pi) {
                delta += twoPi;
            }
            candidateSweep = arcPreviewSweepAngle_ + delta;
        } else {
            if (candidateSweep > pi) {
                candidateSweep -= twoPi;
            } else if (candidateSweep < -pi) {
                candidateSweep += twoPi;
            }
        }

        const qreal snappedSweep = std::round(candidateSweep / halfPi) * halfPi;
        const qreal snappedAngle = startAngle + snappedSweep;
        const QPointF snappedScreen(center.x() + radius * std::cos(snappedAngle),
                                    center.y() + radius * std::sin(snappedAngle));
        return screenToWorld(snappedScreen);
    }

    QPointF constrainTwoPointArcThroughPoint(const QPointF &rawPoint) const
    {
        const QPointF start = worldToScreen(pendingPoints_[0]);
        const QPointF end = worldToScreen(pendingPoints_[1]);
        const QPointF chord = end - start;
        const qreal chordLength = std::hypot(chord.x(), chord.y());
        if (chordLength <= 1e-9) {
            return rawPoint;
        }

        const QPointF midpoint = (start + end) / 2.0;
        const QPointF raw = worldToScreen(rawPoint);

        // A semicircle's through point is one half-chord radius away from
        // the chord midpoint, perpendicular to the start/end chord. These
        // are snap targets, not a permanent constraint: outside the snap
        // radius the third point remains free to define any circular arc.
        const QPointF leftNormal(-chord.y() / chordLength,
                                 chord.x() / chordLength);
        const qreal halfChord = chordLength / 2.0;
        const QPointF candidates[] = {
            midpoint + leftNormal * halfChord,
            midpoint - leftNormal * halfChord};

        constexpr qreal semicircleSnapRadiusPixels = 12.0;
        qreal closestDistance = semicircleSnapRadiusPixels;
        const QPointF *closestCandidate = nullptr;
        for (const QPointF &candidate : candidates) {
            const qreal distance = std::hypot(raw.x() - candidate.x(),
                                              raw.y() - candidate.y());
            if (distance <= closestDistance) {
                closestDistance = distance;
                closestCandidate = &candidate;
            }
        }

        return closestCandidate != nullptr ? screenToWorld(*closestCandidate) : rawPoint;
    }

    void refreshCursorConstraint()
    {
        if (!cursorValid_) {
            currentSnap_ = SnapResult{};
            return;
        }

        cursorWorld_ = constrainLinePoint(rawCursorWorld_);
        lastWorldPosition_ = cursorWorld_;
    }

    qreal distanceToSegment(const QPointF &point,
                            const QPointF &start,
                            const QPointF &end) const
    {
        return curveHitTester_.distanceToSegment(point, start, end);
    }

    QVector<QPointF> rectangleVertices(const Shape &shape) const
    {
        if (shape.geometryType != GeometryType::Rectangle || shape.points.size() < 2) {
            return {};
        }

        if (shape.points.size() >= 4) {
            return {shape.points[0], shape.points[1], shape.points[2], shape.points[3]};
        }

        const QPointF first = shape.points[0];
        const QPointF second = shape.points[1];
        return {first,
                QPointF(second.x(), first.y()),
                second,
                QPointF(first.x(), second.y())};
    }

    QVector<QPointF> controlPointsForShape(const Shape &shape) const
    {
        return curveHitTester_.controlPointsForShape(shape);
    }

    QVector<int> controlPointShapeIndices() const
    {
        QVector<int> indices;
        for (const ObjectId objectId : selectedShapeIndices_) {
            const int index = objectIndex(objectId);
            if (index >= 0 && document_.isObjectVisible(objectId) &&
                document_.isObjectEditable(objectId) &&
                !indices.contains(index)) {
                indices.append(index);
            }
        }
        const int primaryIndex = objectIndex(selectedShapeIndex_);
        if (primaryIndex >= 0 && document_.isObjectVisible(selectedShapeIndex_) &&
            document_.isObjectEditable(selectedShapeIndex_) &&
            !indices.contains(primaryIndex)) {
            indices.append(primaryIndex);
        }
        return indices;
    }

    bool hitTestSelectedControlPoint(const QPointF &screenPosition,
                                     int *shapeIndex,
                                     int *controlPointIndex) const
    {
        return curveHitTester_.hitTestSelectedControlPoint(document_,
                                                           controlPointShapeIndices(),
                                                           screenPosition,
                                                           viewportTransform_,
                                                           size(),
                                                           shapeIndex,
                                                           controlPointIndex);
    }

    int hitTestShape(const QPointF &screenPosition) const
    {
        return curveHitTester_.hitTestShape(document_,
                                            screenPosition,
                                            viewportTransform_,
                                            size(),
                                            true);
    }

    bool insertNurbsKnot(QVector<HomogeneousControlPoint2D> *controlPoints,
                         QVector<double> *knots,
                         int degree,
                         qreal parameter) const
    {
        if (controlPoints == nullptr || knots == nullptr ||
            controlPoints->isEmpty() || knots->isEmpty() || degree < 1) {
            return false;
        }

        const auto sameKnot = [](qreal first, qreal second) {
            const qreal tolerance = 1.0e-9 *
                                     std::max<qreal>(1.0,
                                                     std::max(std::abs(first),
                                                              std::abs(second)));
            return std::abs(first - second) <= tolerance;
        };

        const int n = controlPoints->size() - 1;
        const int m = knots->size() - 1;
        if (m != n + degree + 1) {
            return false;
        }

        int span = degree;
        if (parameter >= knots->at(n + 1)) {
            span = n;
        } else {
            int low = degree;
            int high = n + 1;
            int middle = (low + high) / 2;
            while (parameter < knots->at(middle) || parameter >= knots->at(middle + 1)) {
                if (parameter < knots->at(middle)) {
                    high = middle;
                } else {
                    low = middle;
                }
                middle = (low + high) / 2;
            }
            span = middle;
        }

        int multiplicity = 0;
        for (const double knot : *knots) {
            if (sameKnot(knot, parameter)) {
                ++multiplicity;
            }
        }
        if (multiplicity >= degree) {
            return false;
        }

        QVector<HomogeneousControlPoint2D> insertedControlPoints(n + 2);
        for (int index = 0; index <= span - degree; ++index) {
            insertedControlPoints[index] = controlPoints->at(index);
        }
        for (int index = span - multiplicity; index <= n; ++index) {
            insertedControlPoints[index + 1] = controlPoints->at(index);
        }
        for (int index = span - degree + 1; index <= span - multiplicity; ++index) {
            const qreal denominator = knots->at(index + degree) - knots->at(index);
            if (std::abs(denominator) <= 1.0e-12) {
                return false;
            }
            const qreal alpha = (parameter - knots->at(index)) / denominator;
            insertedControlPoints[index] = blendHomogeneousControlPoints(
                controlPoints->at(index - 1),
                controlPoints->at(index),
                alpha);
        }

        QVector<double> insertedKnots(m + 2);
        for (int index = 0; index <= span; ++index) {
            insertedKnots[index] = knots->at(index);
        }
        insertedKnots[span + 1] = parameter;
        for (int index = span + 1; index <= m; ++index) {
            insertedKnots[index + 1] = knots->at(index);
        }

        *controlPoints = insertedControlPoints;
        *knots = insertedKnots;
        return true;
    }

    bool rationalBezierSpansForCurve(
        const Shape::NurbsCurve2D &curve,
        QVector<RationalBezierSpan2D> *spans) const
    {
        if (spans == nullptr || !isValidNurbsCurve(curve)) {
            return false;
        }

        spans->clear();
        QVector<HomogeneousControlPoint2D> controlPoints;
        controlPoints.reserve(curve.controlPoints.size());
        for (int index = 0; index < curve.controlPoints.size(); ++index) {
            const qreal weight = curve.rational ? curve.weights[index] : 1.0;
            controlPoints.append(HomogeneousControlPoint2D{
                curve.controlPoints[index] * weight,
                weight});
        }

        QVector<double> knots = expandedKnotVector(curve);
        const qreal domainStart = knots[curve.degree];
        const qreal domainEnd = knots[curve.controlPoints.size()];
        const auto sameKnot = [](qreal first, qreal second) {
            const qreal tolerance = 1.0e-9 *
                                     std::max<qreal>(1.0,
                                                     std::max(std::abs(first),
                                                              std::abs(second)));
            return std::abs(first - second) <= tolerance;
        };

        QVector<double> internalKnots;
        for (const double knot : knots) {
            if (knot <= domainStart || knot >= domainEnd) {
                continue;
            }
            if (internalKnots.isEmpty() || !sameKnot(internalKnots.back(), knot)) {
                internalKnots.append(knot);
            }
        }

        for (const double internalKnot : internalKnots) {
            int multiplicity = 0;
            for (const double knot : knots) {
                if (sameKnot(knot, internalKnot)) {
                    ++multiplicity;
                }
            }
            while (multiplicity < curve.degree) {
                if (!insertNurbsKnot(&controlPoints,
                                     &knots,
                                     curve.degree,
                                     internalKnot)) {
                    return false;
                }
                ++multiplicity;
            }
        }

        const qreal knotTolerance = 1.0e-10;
        for (int spanIndex = curve.degree;
             spanIndex < controlPoints.size();
             ++spanIndex) {
            if (knots[spanIndex + 1] - knots[spanIndex] <= knotTolerance) {
                continue;
            }

            RationalBezierSpan2D span;
            span.startParameter = knots[spanIndex];
            span.endParameter = knots[spanIndex + 1];
            span.controlPoints.reserve(curve.degree + 1);
            for (int controlIndex = spanIndex - curve.degree;
                 controlIndex <= spanIndex;
                 ++controlIndex) {
                span.controlPoints.append(controlPoints[controlIndex]);
            }
            spans->append(span);
        }

        return !spans->isEmpty();
    }

    bool splitRationalBezierSpan(
        const QVector<HomogeneousControlPoint2D> &source,
        qreal fraction,
        QVector<HomogeneousControlPoint2D> *left,
        QVector<HomogeneousControlPoint2D> *right) const
    {
        if (source.size() < 2 || left == nullptr || right == nullptr ||
            fraction <= 0.0 || fraction >= 1.0) {
            return false;
        }

        QVector<HomogeneousControlPoint2D> working = source;
        left->resize(source.size());
        right->resize(source.size());
        (*left)[0] = working.first();
        (*right)[source.size() - 1] = working.last();

        for (int level = 1; level < source.size(); ++level) {
            for (int index = 0; index + 1 < working.size(); ++index) {
                working[index] = blendHomogeneousControlPoints(
                    working[index],
                    working[index + 1],
                    fraction);
            }
            (*left)[level] = working.first();
            (*right)[source.size() - 1 - level] = working[source.size() - 1 - level];
        }
        return true;
    }

    bool trimNurbsCurve(const Shape::NurbsCurve2D &source,
                        qreal startParameter,
                        qreal endParameter,
                        Shape::NurbsCurve2D *trimmed) const
    {
        if (trimmed == nullptr || !isValidNurbsCurve(source)) {
            return false;
        }

        const QVector<double> fullKnots = expandedKnotVector(source);
        const qreal domainStart = fullKnots[source.degree];
        const qreal domainEnd = fullKnots[source.controlPoints.size()];
        const qreal domainTolerance =
            std::max<qreal>(1.0e-9, std::abs(domainEnd - domainStart) * 1.0e-9);
        const qreal start = std::clamp(startParameter, domainStart, domainEnd);
        const qreal end = std::clamp(endParameter, domainStart, domainEnd);
        if (end - start <= domainTolerance) {
            return false;
        }

        QVector<RationalBezierSpan2D> spans;
        if (!rationalBezierSpansForCurve(source, &spans)) {
            return false;
        }

        QVector<RationalBezierSpan2D> trimmedSpans;
        for (const RationalBezierSpan2D &span : spans) {
            const qreal overlapStart = std::max(start, span.startParameter);
            const qreal overlapEnd = std::min(end, span.endParameter);
            if (overlapEnd - overlapStart <= domainTolerance) {
                continue;
            }

            const qreal spanLength = span.endParameter - span.startParameter;
            qreal localStart = (overlapStart - span.startParameter) / spanLength;
            qreal localEnd = (overlapEnd - span.startParameter) / spanLength;
            QVector<HomogeneousControlPoint2D> working = span.controlPoints;

            if (localStart > 1.0e-10) {
                QVector<HomogeneousControlPoint2D> discarded;
                QVector<HomogeneousControlPoint2D> remaining;
                if (!splitRationalBezierSpan(working,
                                              localStart,
                                              &discarded,
                                              &remaining)) {
                    return false;
                }
                working = remaining;
                localEnd = (localEnd - localStart) / (1.0 - localStart);
            }

            if (localEnd < 1.0 - 1.0e-10) {
                QVector<HomogeneousControlPoint2D> remaining;
                QVector<HomogeneousControlPoint2D> discarded;
                if (!splitRationalBezierSpan(working,
                                              localEnd,
                                              &remaining,
                                              &discarded)) {
                    return false;
                }
                working = remaining;
            }

            RationalBezierSpan2D trimmedSpan;
            trimmedSpan.startParameter = overlapStart;
            trimmedSpan.endParameter = overlapEnd;
            trimmedSpan.controlPoints = working;
            trimmedSpans.append(trimmedSpan);
        }

        if (trimmedSpans.isEmpty()) {
            return false;
        }

        Shape::NurbsCurve2D result;
        result.dimension = source.dimension;
        result.degree = source.degree;
        result.order = source.order;
        result.rational = source.rational;

        for (int spanIndex = 0; spanIndex < trimmedSpans.size(); ++spanIndex) {
            const QVector<HomogeneousControlPoint2D> &spanPoints =
                trimmedSpans[spanIndex].controlPoints;
            const int firstPoint = spanIndex == 0 ? 0 : 1;
            for (int pointIndex = firstPoint; pointIndex < spanPoints.size(); ++pointIndex) {
                const HomogeneousControlPoint2D &point = spanPoints[pointIndex];
                if (std::abs(point.weight) <= 1.0e-12) {
                    return false;
                }
                result.controlPoints.append(point.weightedPosition / point.weight);
                result.weights.append(source.rational ? point.weight : 1.0);
            }
        }

        QVector<double> resultFullKnots;
        const auto appendRepeated = [&resultFullKnots](qreal value, int count) {
            for (int index = 0; index < count; ++index) {
                resultFullKnots.append(value);
            }
        };
        appendRepeated(trimmedSpans.first().startParameter, source.degree + 1);
        for (int spanIndex = 0; spanIndex + 1 < trimmedSpans.size(); ++spanIndex) {
            appendRepeated(trimmedSpans[spanIndex].endParameter, source.degree);
        }
        appendRepeated(trimmedSpans.last().endParameter, source.degree + 1);

        if (resultFullKnots.size() < 2) {
            return false;
        }
        result.knots = resultFullKnots;
        result.knots.removeFirst();
        result.knots.removeLast();
        if (!isValidNurbsCurve(result)) {
            return false;
        }

        *trimmed = result;
        return true;
    }

    qreal distanceToEraserStroke(const QPointF &screenPoint,
                                 const QVector<QPointF> &stroke) const
    {
        if (stroke.isEmpty()) {
            return 1.0e9;
        }
        if (stroke.size() == 1) {
            return distanceToSegment(screenPoint, stroke.first(), stroke.first());
        }

        qreal distance = 1.0e9;
        for (int index = 1; index < stroke.size(); ++index) {
            distance = std::min(distance,
                                distanceToSegment(screenPoint,
                                                  stroke[index - 1],
                                                  stroke[index]));
        }
        return distance;
    }

    bool sampleNurbsCurveForErase(const Shape::NurbsCurve2D &curve,
                                  SampledNurbsCurve2D *sampled) const
    {
        return curveSampler_.sampleNurbsCurve(curve,
                                              viewportTransform_,
                                              size(),
                                              sampled);

        // Kept below as a migration reference while erase-cache ownership
        // moves fully into the sampling service.
        if (sampled == nullptr || !isValidNurbsCurve(curve)) {
            return false;
        }

        sampled->parameters.clear();
        sampled->screenPoints.clear();
        sampled->segmentBounds.clear();
        sampled->bounds = QRectF();

        const QVector<double> fullKnots = expandedKnotVector(curve);
        const qreal domainStart = fullKnots[curve.degree];
        const qreal domainEnd = fullKnots[curve.controlPoints.size()];
        if (domainEnd <= domainStart) {
            return false;
        }

        int nonZeroSpans = 0;
        for (int index = curve.degree; index < curve.controlPoints.size(); ++index) {
            if (fullKnots[index + 1] > fullKnots[index]) {
                ++nonZeroSpans;
            }
        }

        // Use enough points for a responsive partial-erase preview while
        // avoiding the old 256-sample minimum for every curve. Degree-1
        // spans get a little more density because a long straight span still
        // needs a visible partial interval when the eraser crosses it.
        const int samplesPerSpan = curve.degree <= 1 ? 64 : 32;
        const int maxSampleCount = 2048;
        const int samplesForEachSpan =
            std::max(1, std::min(samplesPerSpan,
                                  maxSampleCount / std::max(1, nonZeroSpans)));
        sampled->parameters.reserve(nonZeroSpans * samplesForEachSpan + 1);
        sampled->screenPoints.reserve(nonZeroSpans * samplesForEachSpan + 1);
        for (int spanIndex = curve.degree;
             spanIndex < curve.controlPoints.size();
             ++spanIndex) {
            const qreal spanStart = fullKnots[spanIndex];
            const qreal spanEnd = fullKnots[spanIndex + 1];
            if (spanEnd <= spanStart) {
                continue;
            }

            for (int sample = 0; sample <= samplesForEachSpan; ++sample) {
                if (spanIndex > curve.degree && sample == 0) {
                    continue;
                }

                const qreal fraction = static_cast<qreal>(sample) /
                                       samplesForEachSpan;
                const qreal parameter = spanStart + (spanEnd - spanStart) * fraction;
                QPointF worldPoint;
                if (!evaluateNurbsPoint(curve, parameter, &worldPoint)) {
                    sampled->parameters.clear();
                    sampled->screenPoints.clear();
                    sampled->segmentBounds.clear();
                    sampled->bounds = QRectF();
                    return false;
                }
                sampled->parameters.append(parameter);
                sampled->screenPoints.append(worldToScreen(worldPoint));
            }
        }

        sampled->segmentBounds.reserve(sampled->screenPoints.size() - 1);
        sampled->bounds = QRectF(sampled->screenPoints.first(),
                                 sampled->screenPoints.first());
        for (int sample = 1; sample < sampled->screenPoints.size(); ++sample) {
            const QPointF &first = sampled->screenPoints[sample - 1];
            const QPointF &second = sampled->screenPoints[sample];
            sampled->segmentBounds.append(QRectF(first, second).normalized());
            sampled->bounds = sampled->bounds.united(QRectF(second, second));
        }

        return sampled->parameters.size() >= 2;
    }

    QVector<Shape::NurbsCurve2D> eraseIntersectionCurvesForShape(
        const Shape &shape) const
    {
        if (shape.geometryType == GeometryType::PolyCurve) {
            return shape.components;
        }

        if (isValidNurbsCurve(shape.nurbs)) {
            return {shape.nurbs};
        }

        if (shape.geometryType == GeometryType::Line && shape.points.size() >= 2) {
            return {makeDegreeOneNurbs(shape.points)};
        }

        if ((shape.geometryType == GeometryType::Bezier || shape.geometryType == GeometryType::Nurbs) &&
            shape.points.size() >= 2) {
            return {makeBezierNurbs(shape.points)};
        }

        if ((shape.geometryType == GeometryType::Rectangle ||
             shape.geometryType == GeometryType::Polygon) &&
            shape.points.size() >= 2) {
            const QVector<QPointF> vertices = shape.geometryType == GeometryType::Polygon
                                                  ? shape.points
                                                  : rectangleVertices(shape);
            const int minimumVertices = shape.geometryType == GeometryType::Polygon ? 3 : 4;
            if (vertices.size() < minimumVertices) {
                return {};
            }

            QVector<Shape::NurbsCurve2D> curves;
            curves.reserve(vertices.size());
            for (int index = 0; index < vertices.size(); ++index) {
                curves.append(makeDegreeOneNurbs(
                    {vertices[index], vertices[(index + 1) % vertices.size()]}));
            }
            return curves;
        }

        return {};
    }

    QVector<qreal> eraseIntersectionParameters(
        int sourceShapeIndex,
        int sourceComponentIndex,
        const Shape::NurbsCurve2D &sourceCurve,
        const SampledNurbsCurve2D *sourceSamplesOverride = nullptr,
        const QVector<EraseCurveSampleCache> *sceneCache = nullptr) const
    {
        QVector<qreal> parameters;
        if (sourceShapeIndex < 0 || sourceShapeIndex >= shapes_.size() ||
            !isValidNurbsCurve(sourceCurve)) {
            return parameters;
        }

        SampledNurbsCurve2D generatedSourceSamples;
        const SampledNurbsCurve2D *sourceSamples = sourceSamplesOverride;
        if (sourceSamples == nullptr) {
            if (!sampleNurbsCurveForErase(sourceCurve, &generatedSourceSamples)) {
                return parameters;
            }
            sourceSamples = &generatedSourceSamples;
        } else if (sourceSamples->screenPoints.size() < 2 ||
                   sourceSamples->parameters.size() != sourceSamples->screenPoints.size()) {
            return parameters;
        }

        const auto parameterAtIntersection = [](const QPointF &first,
                                                 const QPointF &second,
                                                 const QPointF &intersection) {
            const QPointF direction = second - first;
            const qreal lengthSquared = QPointF::dotProduct(direction, direction);
            if (lengthSquared <= 1.0e-12) {
                return 0.0;
            }
            return std::clamp(QPointF::dotProduct(intersection - first, direction) /
                                  lengthSquared,
                                  0.0,
                                  1.0);
        };

        const auto boundsOverlap = [](const QRectF &first, const QRectF &second) {
            return first.right() >= second.left() &&
                   second.right() >= first.left() &&
                   first.bottom() >= second.top() &&
                   second.bottom() >= first.top();
        };

        const auto appendUniqueParameter = [&](qreal parameter) {
            QPointF candidateWorld;
            if (!evaluateNurbsPoint(sourceCurve, parameter, &candidateWorld)) {
                parameters.append(parameter);
                return;
            }
            const QPointF candidateScreen = worldToScreen(candidateWorld);
            for (const qreal existingParameter : parameters) {
                QPointF existingWorld;
                if (evaluateNurbsPoint(sourceCurve, existingParameter, &existingWorld)) {
                    const QPointF existingScreen = worldToScreen(existingWorld);
                    if (std::hypot(candidateScreen.x() - existingScreen.x(),
                                   candidateScreen.y() - existingScreen.y()) <= 0.1) {
                        return;
                    }
                }
            }
            parameters.append(parameter);
        };

        const auto maximumSampledSegmentLength = [](const SampledNurbsCurve2D &sampled) {
            qreal maximumLength = 0.0;
            for (int index = 1; index < sampled.screenPoints.size(); ++index) {
                const QPointF delta = sampled.screenPoints[index] -
                                      sampled.screenPoints[index - 1];
                maximumLength = std::max(maximumLength,
                                         std::hypot(delta.x(), delta.y()));
            }
            return maximumLength;
        };
        const qreal sourceSampleSegmentLength = sourceCurve.degree > 1
                                                    ? maximumSampledSegmentLength(*sourceSamples)
                                                    : 0.0;

        const auto collectIntersections =
            [&](const SampledNurbsCurve2D &otherSamples,
                const Shape::NurbsCurve2D &otherCurve) {
                if (otherSamples.screenPoints.size() < 2 ||
                    otherSamples.parameters.size() != otherSamples.screenPoints.size()) {
                    return;
                }

                const bool useBounds =
                    !sourceSamples->bounds.isNull() && !otherSamples.bounds.isNull() &&
                    sourceSamples->segmentBounds.size() ==
                        sourceSamples->screenPoints.size() - 1 &&
                    otherSamples.segmentBounds.size() ==
                        otherSamples.screenPoints.size() - 1;
                qreal curvedSegmentLength = sourceSampleSegmentLength;
                if (otherCurve.degree > 1) {
                    curvedSegmentLength = std::max(
                        curvedSegmentLength,
                        maximumSampledSegmentLength(otherSamples));
                }
                const qreal tangentCandidateTolerancePixels =
                    std::max<qreal>(2.0, curvedSegmentLength * 0.05);
                if (useBounds && !boundsOverlap(sourceSamples->bounds,
                                                otherSamples.bounds.adjusted(
                                                    -tangentCandidateTolerancePixels,
                                                    -tangentCandidateTolerancePixels,
                                                    tangentCandidateTolerancePixels,
                                                    tangentCandidateTolerancePixels))) {
                    return;
                }

                // A previously trimmed endpoint can lie just off the sampled
                // chord. Recognize endpoint contacts before strict segment
                // crossings, otherwise an entire neighboring section is lost.
                for (int endpoint : {0, int(otherSamples.screenPoints.size() - 1)}) {
                    const QPointF point = otherSamples.screenPoints[endpoint];
                    qreal bestDistance = 1.0;
                    qreal bestParameter = 0.0;
                    bool found = false;
                    for (int i = 1; i < sourceSamples->screenPoints.size(); ++i) {
                        const QPointF a = sourceSamples->screenPoints[i - 1];
                        const QPointF b = sourceSamples->screenPoints[i];
                        const qreal distance = distanceToSegment(point, a, b);
                        if (distance <= bestDistance) {
                            bestDistance = distance;
                            bestParameter = sourceSamples->parameters[i - 1] +
                                (sourceSamples->parameters[i] - sourceSamples->parameters[i - 1]) *
                                parameterAtIntersection(a, b, point);
                            found = true;
                        }
                    }
                    if (found) {
                        appendUniqueParameter(bestParameter);
                    }
                }

                const qreal spatialCellSizePixels = std::max<qreal>(
                    8.0, tangentCandidateTolerancePixels * 2.0);
                constexpr qint64 maximumSpatialCellsPerSegment = 256;
                const auto spatialCellCoordinate = [=](qreal coordinate) {
                    return static_cast<int>(std::floor(coordinate /
                                                       spatialCellSizePixels));
                };
                const auto spatialCellKey = [](int x, int y) {
                    return (static_cast<quint64>(static_cast<quint32>(x)) << 32) |
                           static_cast<quint32>(y);
                };
                const auto cellCountForBounds = [=](int firstX,
                                                    int lastX,
                                                    int firstY,
                                                    int lastY) {
                    const qint64 xCount = static_cast<qint64>(lastX) - firstX + 1;
                    const qint64 yCount = static_cast<qint64>(lastY) - firstY + 1;
                    if (xCount <= 0 || yCount <= 0 ||
                        xCount > maximumSpatialCellsPerSegment / yCount) {
                        return maximumSpatialCellsPerSegment + 1;
                    }
                    return xCount * yCount;
                };

                QHash<quint64, QVector<int>> otherSegmentGrid;
                QVector<int> longOtherSegments;
                const int otherSegmentCount = otherSamples.screenPoints.size() - 1;
                if (useBounds) {
                    for (int otherSegment = 1;
                         otherSegment <= otherSegmentCount;
                         ++otherSegment) {
                        const QRectF &bounds =
                            otherSamples.segmentBounds[otherSegment - 1];
                        const int firstX = spatialCellCoordinate(bounds.left());
                        const int lastX = spatialCellCoordinate(bounds.right());
                        const int firstY = spatialCellCoordinate(bounds.top());
                        const int lastY = spatialCellCoordinate(bounds.bottom());
                        if (cellCountForBounds(firstX, lastX, firstY, lastY) >
                            maximumSpatialCellsPerSegment) {
                            longOtherSegments.append(otherSegment);
                            continue;
                        }
                        for (int cellX = firstX; cellX <= lastX; ++cellX) {
                            for (int cellY = firstY; cellY <= lastY; ++cellY) {
                                otherSegmentGrid[spatialCellKey(cellX, cellY)]
                                    .append(otherSegment);
                            }
                        }
                    }
                }

                constexpr qreal tangentContactTolerancePixels = 0.001;
                const auto squaredScreenDistance = [](const QPointF &first,
                                                       const QPointF &second) {
                    const QPointF delta = first - second;
                    return QPointF::dotProduct(delta, delta);
                };
                const auto separationSquared = [&](qreal sourceParameter,
                                                   qreal otherParameter) {
                    QPointF sourceWorld;
                    QPointF otherWorld;
                    if (!evaluateNurbsPoint(sourceCurve, sourceParameter, &sourceWorld) ||
                        !evaluateNurbsPoint(otherCurve, otherParameter, &otherWorld)) {
                        return 1.0e30;
                    }
                    return squaredScreenDistance(worldToScreen(sourceWorld),
                                                  worldToScreen(otherWorld));
                };
                const auto goldenMinimum = [](qreal low,
                                              qreal high,
                                              const auto &valueAt) {
                    constexpr qreal ratio = 0.6180339887498948482;
                    qreal first = high - (high - low) * ratio;
                    qreal second = low + (high - low) * ratio;
                    qreal firstValue = valueAt(first);
                    qreal secondValue = valueAt(second);
                    for (int iteration = 0; iteration < 20; ++iteration) {
                        if (firstValue <= secondValue) {
                            high = second;
                            second = first;
                            secondValue = firstValue;
                            first = high - (high - low) * ratio;
                            firstValue = valueAt(first);
                        } else {
                            low = first;
                            first = second;
                            firstValue = secondValue;
                            second = low + (high - low) * ratio;
                            secondValue = valueAt(second);
                        }
                    }
                    return (low + high) * 0.5;
                };

                QVector<QPair<qreal, qreal>> refinedTangentContacts;
                QVector<int> candidateGeneration(otherSegmentCount + 1, 0);
                for (int sourceSegment = 1;
                     sourceSegment < sourceSamples->screenPoints.size();
                     ++sourceSegment) {
                    const QPointF &sourceStart =
                        sourceSamples->screenPoints[sourceSegment - 1];
                    const QPointF &sourceEnd =
                        sourceSamples->screenPoints[sourceSegment];
                    QVector<int> candidateOtherSegments;
                    QRectF sourceBounds;
                    if (useBounds) {
                        sourceBounds =
                            sourceSamples->segmentBounds[sourceSegment - 1].adjusted(
                                -tangentCandidateTolerancePixels,
                                -tangentCandidateTolerancePixels,
                                tangentCandidateTolerancePixels,
                                tangentCandidateTolerancePixels);
                        const int firstX = spatialCellCoordinate(sourceBounds.left());
                        const int lastX = spatialCellCoordinate(sourceBounds.right());
                        const int firstY = spatialCellCoordinate(sourceBounds.top());
                        const int lastY = spatialCellCoordinate(sourceBounds.bottom());
                        if (cellCountForBounds(firstX, lastX, firstY, lastY) >
                            maximumSpatialCellsPerSegment) {
                            candidateOtherSegments.reserve(otherSegmentCount);
                            for (int otherSegment = 1;
                                 otherSegment <= otherSegmentCount;
                                 ++otherSegment) {
                                candidateOtherSegments.append(otherSegment);
                            }
                        } else {
                            const auto appendCandidate = [&](int otherSegment) {
                                if (candidateGeneration[otherSegment] != sourceSegment) {
                                    candidateGeneration[otherSegment] = sourceSegment;
                                    candidateOtherSegments.append(otherSegment);
                                }
                            };
                            for (const int otherSegment : longOtherSegments) {
                                appendCandidate(otherSegment);
                            }
                            for (int cellX = firstX; cellX <= lastX; ++cellX) {
                                for (int cellY = firstY; cellY <= lastY; ++cellY) {
                                    const auto cell = otherSegmentGrid.constFind(
                                        spatialCellKey(cellX, cellY));
                                    if (cell == otherSegmentGrid.cend()) {
                                        continue;
                                    }
                                    for (const int otherSegment : cell.value()) {
                                        appendCandidate(otherSegment);
                                    }
                                }
                            }
                        }
                    } else {
                        candidateOtherSegments.reserve(otherSegmentCount);
                        for (int otherSegment = 1;
                             otherSegment <= otherSegmentCount;
                             ++otherSegment) {
                            candidateOtherSegments.append(otherSegment);
                        }
                    }
                    std::sort(candidateOtherSegments.begin(), candidateOtherSegments.end());

                    for (const int otherSegment : candidateOtherSegments) {
                        if (useBounds &&
                            !boundsOverlap(sourceBounds,
                                           otherSamples.segmentBounds[otherSegment - 1])) {
                            continue;
                        }

                        const QPointF &otherStart =
                            otherSamples.screenPoints[otherSegment - 1];
                        const QPointF &otherEnd =
                            otherSamples.screenPoints[otherSegment];
                        const QPointF sourceDirection = sourceEnd - sourceStart;
                        const QPointF otherDirection = otherEnd - otherStart;
                        const qreal directionProduct =
                            std::hypot(sourceDirection.x(), sourceDirection.y()) *
                            std::hypot(otherDirection.x(), otherDirection.y());
                        const bool nearlyParallel =
                            directionProduct > 1.0e-12 &&
                            std::abs(QPointF::dotProduct(sourceDirection,
                                                        otherDirection)) /
                                    directionProduct >=
                                0.9;
                        QPointF intersection;
                        if (segmentIntersection(sourceStart,
                                                sourceEnd,
                                                otherStart,
                                                otherEnd,
                                                &intersection)) {
                            if (!nearlyParallel) {
                                const qreal localParameter = parameterAtIntersection(
                                    sourceStart,
                                    sourceEnd,
                                    intersection);
                                appendUniqueParameter(
                                    sourceSamples->parameters[sourceSegment - 1] +
                                    (sourceSamples->parameters[sourceSegment] -
                                     sourceSamples->parameters[sourceSegment - 1]) *
                                        localParameter);
                                continue;
                            }
                        }

                        if (!nearlyParallel) {
                            continue;
                        }

                        qreal closestDistance = 1.0e30;
                        QPointF closestPairMidpoint;
                        const auto considerClosestPoints = [&](const QPointF &first,
                                                               const QPointF &second) {
                            const qreal distance = std::hypot(first.x() - second.x(),
                                                              first.y() - second.y());
                            if (distance < closestDistance) {
                                closestDistance = distance;
                                closestPairMidpoint = (first + second) * 0.5;
                            }
                        };
                        const qreal sourceStartOnOther = parameterAtIntersection(
                            otherStart, otherEnd, sourceStart);
                        const QPointF sourceStartProjection =
                            otherStart + (otherEnd - otherStart) * sourceStartOnOther;
                        considerClosestPoints(sourceStart, sourceStartProjection);
                        const qreal sourceEndOnOther = parameterAtIntersection(
                            otherStart, otherEnd, sourceEnd);
                        const QPointF sourceEndProjection =
                            otherStart + (otherEnd - otherStart) * sourceEndOnOther;
                        considerClosestPoints(sourceEnd, sourceEndProjection);
                        const qreal otherStartOnSource = parameterAtIntersection(
                            sourceStart, sourceEnd, otherStart);
                        const QPointF otherStartProjection =
                            sourceStart + (sourceEnd - sourceStart) * otherStartOnSource;
                        considerClosestPoints(otherStartProjection, otherStart);
                        const qreal otherEndOnSource = parameterAtIntersection(
                            sourceStart, sourceEnd, otherEnd);
                        const QPointF otherEndProjection =
                            sourceStart + (sourceEnd - sourceStart) * otherEndOnSource;
                        considerClosestPoints(otherEndProjection, otherEnd);
                        if (closestDistance > tangentCandidateTolerancePixels) {
                            continue;
                        }
                        const bool alreadyRefined = std::any_of(
                            refinedTangentContacts.cbegin(),
                            refinedTangentContacts.cend(),
                            [&](const QPair<qreal, qreal> &contact) {
                                QPointF contactWorld;
                                if (!evaluateNurbsPoint(sourceCurve,
                                                        contact.first,
                                                        &contactWorld)) {
                                    return false;
                                }
                                const QPointF contactScreen = worldToScreen(contactWorld);
                                return std::hypot(contactScreen.x() - closestPairMidpoint.x(),
                                                  contactScreen.y() - closestPairMidpoint.y()) <=
                                       1.0;
                            });
                        if (alreadyRefined) {
                            continue;
                        }

                        constexpr int refinementNeighborSegments = 2;
                        const int sourceLowIndex =
                            std::max(0, sourceSegment - 1 - refinementNeighborSegments);
                        const int sourceHighIndex = std::min(
                            static_cast<int>(sourceSamples->parameters.size()) - 1,
                            sourceSegment + refinementNeighborSegments);
                        const int otherLowIndex =
                            std::max(0, otherSegment - 1 - refinementNeighborSegments);
                        const int otherHighIndex = std::min(
                            static_cast<int>(otherSamples.parameters.size()) - 1,
                            otherSegment + refinementNeighborSegments);
                        const qreal sourceLow =
                            sourceSamples->parameters[sourceLowIndex];
                        const qreal sourceHigh =
                            sourceSamples->parameters[sourceHighIndex];
                        const qreal otherLow = otherSamples.parameters[otherLowIndex];
                        const qreal otherHigh = otherSamples.parameters[otherHighIndex];
                        const auto nearestOtherParameter = [&](qreal sourceParameter) {
                            return goldenMinimum(
                                otherLow,
                                otherHigh,
                                [&](qreal parameter) {
                                    return separationSquared(sourceParameter, parameter);
                                });
                        };
                        const qreal sourceParameter = goldenMinimum(
                            sourceLow,
                            sourceHigh,
                            [&](qreal parameter) {
                                const qreal otherParameter =
                                    nearestOtherParameter(parameter);
                                return separationSquared(parameter, otherParameter);
                            });
                        const qreal otherParameter =
                            nearestOtherParameter(sourceParameter);
                        const qreal contactError =
                            separationSquared(sourceParameter, otherParameter);
                        if (contactError <= tangentContactTolerancePixels *
                                                tangentContactTolerancePixels) {
                            refinedTangentContacts.append(
                                qMakePair(sourceParameter, contactError));
                        }
                    }
                }

                QVector<QPair<qreal, qreal>> uniqueTangentContacts;
                for (const QPair<qreal, qreal> &candidate : refinedTangentContacts) {
                    QPointF candidateWorld;
                    if (!evaluateNurbsPoint(sourceCurve, candidate.first, &candidateWorld)) {
                        continue;
                    }
                    const QPointF candidateScreen = worldToScreen(candidateWorld);
                    int nearbyContact = -1;
                    for (int index = 0; index < uniqueTangentContacts.size(); ++index) {
                        QPointF existingWorld;
                        if (!evaluateNurbsPoint(sourceCurve,
                                               uniqueTangentContacts[index].first,
                                               &existingWorld)) {
                            continue;
                        }
                        const QPointF existingScreen = worldToScreen(existingWorld);
                        if (std::hypot(candidateScreen.x() - existingScreen.x(),
                                       candidateScreen.y() - existingScreen.y()) <= 1.0) {
                            nearbyContact = index;
                            break;
                        }
                    }
                    if (nearbyContact < 0) {
                        uniqueTangentContacts.append(candidate);
                    } else if (candidate.second <
                               uniqueTangentContacts[nearbyContact].second) {
                        uniqueTangentContacts[nearbyContact] = candidate;
                    }
                }
                for (const QPair<qreal, qreal> &contact : uniqueTangentContacts) {
                    appendUniqueParameter(contact.first);
                }
            };

        if (sceneCache != nullptr) {
            for (const EraseCurveSampleCache &other : *sceneCache) {
                if (other.shapeIndex == sourceShapeIndex &&
                    other.componentIndex == sourceComponentIndex) {
                    continue;
                }
                collectIntersections(other.sampled, other.curve);
            }
        } else {
            for (int shapeIndex = 0; shapeIndex < shapes_.size(); ++shapeIndex) {
                const QVector<Shape::NurbsCurve2D> otherCurves =
                    eraseIntersectionCurvesForShape(shapes_[shapeIndex]);
                for (int componentIndex = 0;
                     componentIndex < otherCurves.size();
                     ++componentIndex) {
                    if (shapeIndex == sourceShapeIndex &&
                        componentIndex == sourceComponentIndex) {
                        continue;
                    }

                    SampledNurbsCurve2D otherSamples;
                    if (sampleNurbsCurveForErase(otherCurves[componentIndex],
                                                 &otherSamples)) {
                        collectIntersections(otherSamples,
                                             otherCurves[componentIndex]);
                    }
                }
            }
        }

        return parameters;
    }

    QVector<int> eraseSelectionTargets() const
    {
        QVector<int> targets;
        for (const ObjectId objectId : selectedShapeIndices_) {
            const int shapeIndex = objectIndex(objectId);
            if (shapeIndex >= 0 && shapeIndex < shapes_.size() &&
                !targets.contains(shapeIndex)) {
                targets.append(shapeIndex);
            }
        }
        const int primaryIndex = objectIndex(selectedShapeIndex_);
        if (primaryIndex >= 0 && !targets.contains(primaryIndex)) {
            targets.append(primaryIndex);
        }
        return targets;
    }

    void prepareEraseGeometryCache()
    {
        eraseTargetShapeIndices_.clear();
        eraseSceneCurveCaches_.clear();
        eraseTargetCurveCaches_.clear();
        eraseGeometryCachePrepared_ = true;

        const QVector<int> selectedTargets = eraseSelectionTargets();
        if (selectedTargets.isEmpty()) {
            return;
        }

        eraseSceneCurveCaches_ = curveSampler_.sampleDocument(document_,
                                                              viewportTransform_,
                                                              size());
        if (eraseSceneCurveCaches_.isEmpty()) {
            // Compatibility fallback for malformed legacy shapes while the
            // remaining erase-intersection code finishes its migration.
            for (int shapeIndex = 0; shapeIndex < shapes_.size(); ++shapeIndex) {
                const QVector<Shape::NurbsCurve2D> curves =
                    eraseIntersectionCurvesForShape(shapes_[shapeIndex]);
                for (int componentIndex = 0;
                     componentIndex < curves.size();
                     ++componentIndex) {
                    if (!isValidNurbsCurve(curves[componentIndex])) {
                        continue;
                    }

                    EraseCurveSampleCache cache;
                    cache.shapeIndex = shapeIndex;
                    cache.componentIndex = componentIndex;
                    cache.curve = curves[componentIndex];
                    if (sampleNurbsCurveForErase(cache.curve, &cache.sampled)) {
                        eraseSceneCurveCaches_.append(cache);
                    }
                }
            }
        }

        for (const int shapeIndex : selectedTargets) {
            bool hasTargetCurve = false;
            for (const EraseCurveSampleCache &sceneCurve : eraseSceneCurveCaches_) {
                if (sceneCurve.shapeIndex != shapeIndex) {
                    continue;
                }

                EraseCurveSampleCache targetCurve = sceneCurve;
                targetCurve.intersectionParameters = eraseIntersectionParameters(
                    shapeIndex,
                    sceneCurve.componentIndex,
                    sceneCurve.curve,
                    &sceneCurve.sampled,
                    &eraseSceneCurveCaches_);
                eraseTargetCurveCaches_.append(targetCurve);
                hasTargetCurve = true;
            }
            if (hasTargetCurve) {
                eraseTargetShapeIndices_.append(shapes_.objectIdAt(shapeIndex));
            }
        }

        DebugLog::instance().write(
            QStringLiteral("erase cache prepared selectedShapes=%1 sceneCurves=%2 targetCurves=%3")
                .arg(eraseTargetShapeIndices_.size())
                .arg(eraseSceneCurveCaches_.size())
                .arg(eraseTargetCurveCaches_.size()));
    }

    qreal distanceToCachedEraseShape(const QPointF &screenPosition,
                                     int shapeIndex,
                                     int *closestComponentIndex = nullptr) const
    {
        constexpr qreal eraserRadiusPixels = 10.0;
        qreal distance = 1.0e9;
        if (closestComponentIndex != nullptr) {
            *closestComponentIndex = -1;
        }
        for (const EraseCurveSampleCache &targetCurve : eraseTargetCurveCaches_) {
            if (targetCurve.shapeIndex != shapeIndex) {
                continue;
            }

            qreal componentDistance = 1.0e9;
            for (int sample = 1;
                 sample < targetCurve.sampled.screenPoints.size();
                 ++sample) {
                if (targetCurve.sampled.segmentBounds.size() ==
                    targetCurve.sampled.screenPoints.size() - 1) {
                    const QRectF &bounds =
                        targetCurve.sampled.segmentBounds[sample - 1];
                    if (screenPosition.x() < bounds.left() - eraserRadiusPixels ||
                        screenPosition.x() > bounds.right() + eraserRadiusPixels ||
                        screenPosition.y() < bounds.top() - eraserRadiusPixels ||
                        screenPosition.y() > bounds.bottom() + eraserRadiusPixels) {
                        continue;
                    }
                }

                componentDistance = std::min(
                    componentDistance,
                    distanceToSegment(screenPosition,
                                      targetCurve.sampled.screenPoints[sample - 1],
                                      targetCurve.sampled.screenPoints[sample]));
            }
            if (componentDistance < distance) {
                distance = componentDistance;
                if (closestComponentIndex != nullptr) {
                    *closestComponentIndex = targetCurve.componentIndex;
                }
            }
        }
        return distance;
    }

    void updateTrimHover(const QPointF &screenPosition)
    {
        if (trimHoverPositionValid_ && eraseGeometryCachePrepared_ &&
            trimHoverScreenPosition_ == screenPosition) {
            return;
        }
        trimHoverScreenPosition_ = screenPosition;
        trimHoverPositionValid_ = true;

        if (!eraseGeometryCachePrepared_) {
            prepareEraseGeometryCache();
        }

        eraseStrokeScreenPath_.clear();
        eraseStrokeScreenPath_.append(screenPosition);
        eraseCandidateShapeIndices_.clear();

        constexpr qreal trimHitRadiusPixels = 10.0;
        qreal closestDistance = trimHitRadiusPixels;
        int closestShapeIndex = -1;
        int closestComponentIndex = -1;
        for (const ObjectId objectId : eraseTargetShapeIndices_) {
            const int shapeIndex = objectIndex(objectId);
            if (shapeIndex < 0) {
                continue;
            }
            int componentIndex = -1;
            const qreal distance = distanceToCachedEraseShape(screenPosition,
                                                              shapeIndex,
                                                              &componentIndex);
            const bool closer = distance < closestDistance - 1.0e-6;
            const bool tieOnActiveSelection =
                std::abs(distance - closestDistance) <= 1.0e-6 &&
                objectId == selectedShapeIndex_;
            if (closer || tieOnActiveSelection) {
                closestDistance = distance;
                closestShapeIndex = shapeIndex;
                closestComponentIndex = componentIndex;
            }
        }

        if (closestShapeIndex >= 0) {
            eraseCandidateShapeIndices_.append(shapes_.objectIdAt(closestShapeIndex));
        }
        trimHoverComponentIndex_ = closestComponentIndex;
        updateErasePreviewIntervals(true, closestShapeIndex, closestComponentIndex);
    }

    void trimAtScreenPosition(const QPointF &screenPosition)
    {
        updateTrimHover(screenPosition);
        if (eraseCandidateShapeIndices_.isEmpty()) {
            DebugLog::instance().write(
                QStringLiteral("trim click ignored no selected curve under cursor"));
            return;
        }

        applyEraseCandidates(nullptr, trimHoverComponentIndex_);
        eraseStrokeActive_ = false;
        eraseCandidateShapeIndices_.clear();
        eraseStrokeScreenPath_.clear();
        eraseTargetShapeIndices_.clear();
        eraseSceneCurveCaches_.clear();
        eraseTargetCurveCaches_.clear();
        eraseGeometryCachePrepared_ = false;
        trimHoverPositionValid_ = false;
        trimHoverComponentIndex_ = -1;
        update();
        DebugLog::instance().write(QStringLiteral("trim click applied"));
    }

    void updateTrimBoxPreview()
    {
        if (!trimBoxSelectionActive_) {
            return;
        }
        if (!eraseGeometryCachePrepared_) {
            prepareEraseGeometryCache();
        }

        const QRectF box =
            QRectF(selectionBoxStartScreen_, selectionBoxCurrentScreen_).normalized();
        const QPointF drag = selectionBoxCurrentScreen_ - selectionBoxStartScreen_;
        // Trim's window/crossing gesture follows the diagonal the user draws:
        // top-left to bottom-right contains; bottom-left to top-right crosses.
        const bool crossingSelection = drag.x() * drag.y() < 0.0;
        eraseCandidateShapeIndices_.clear();
        for (const ObjectId objectId : eraseTargetShapeIndices_) {
            const int shapeIndex = objectIndex(objectId);
            if (shapeIndex >= 0 && shapeIndex < shapes_.size() &&
                shapeMatchesSelectionBox(shapes_[shapeIndex], box, crossingSelection)) {
                eraseCandidateShapeIndices_.append(objectId);
            }
        }

        // A non-empty marker enables the existing orange trim preview; the
        // actual interval calculation and commit use the box region below.
        eraseStrokeScreenPath_.clear();
        eraseStrokeScreenPath_.append(selectionBoxStartScreen_);
        for (EraseCurveSampleCache &targetCurve : eraseTargetCurveCaches_) {
            targetCurve.previewStrokePointCount = 0;
            const ObjectId objectId = shapes_.objectIdAt(targetCurve.shapeIndex);
            if (!eraseCandidateShapeIndices_.contains(objectId)) {
                targetCurve.previewIntervals.clear();
                continue;
            }

            const QVector<ParameterInterval> hitIntervals =
                curveIntervalsInsideScreenBox(targetCurve.curve,
                                              targetCurve.sampled,
                                              box);
            targetCurve.previewIntervals = boundEraseIntervals(
                targetCurve.curve,
                hitIntervals,
                targetCurve.intersectionParameters);
        }
    }

    void finishTrimBoxSelection()
    {
        if (!selectionBoxActive_ || !trimBoxSelectionActive_) {
            return;
        }

        const QRectF box =
            QRectF(selectionBoxStartScreen_, selectionBoxCurrentScreen_).normalized();
        const QPointF drag = selectionBoxCurrentScreen_ - selectionBoxStartScreen_;
        const bool crossingSelection = drag.x() * drag.y() < 0.0;
        const bool moved = selectionBoxMoved_ || box.width() >= 3.0 ||
                           box.height() >= 3.0;
        const QPointF clickPosition = selectionBoxCurrentScreen_;
        if (!moved) {
            selectionBoxActive_ = false;
            selectionBoxMoved_ = false;
            selectionBoxAdditive_ = false;
            trimBoxSelectionActive_ = false;
            trimHoverComponentIndex_ = -1;
            eraseCandidateShapeIndices_.clear();
            eraseStrokeScreenPath_.clear();
            trimHoverPositionValid_ = false;
            trimAtScreenPosition(clickPosition);
            setCursor(Qt::CrossCursor);
            update();
            emitCoordinateUpdate();
            return;
        }

        updateTrimBoxPreview();
        const int candidateCount = eraseCandidateShapeIndices_.size();
        selectionBoxActive_ = false;
        selectionBoxMoved_ = false;
        selectionBoxAdditive_ = false;
        trimBoxSelectionActive_ = false;
        trimHoverComponentIndex_ = -1;
        if (!eraseCandidateShapeIndices_.isEmpty()) {
            applyEraseCandidates(&box);
        }

        eraseStrokeActive_ = false;
        eraseCursorPressed_ = false;
        eraseCandidateShapeIndices_.clear();
        eraseStrokeScreenPath_.clear();
        eraseTargetShapeIndices_.clear();
        eraseSceneCurveCaches_.clear();
        eraseTargetCurveCaches_.clear();
        eraseGeometryCachePrepared_ = false;
        trimHoverPositionValid_ = false;
        setCursor(Qt::CrossCursor);
        update();
        emitCoordinateUpdate();
        DebugLog::instance().write(
            QStringLiteral("trim box applied crossing=%1 candidates=%2")
                .arg(crossingSelection)
                .arg(candidateCount));
    }

    QVector<ParameterInterval> eraserIntervalsForCurve(
        const Shape::NurbsCurve2D &curve,
        const QVector<QPointF> &stroke) const
    {
        QVector<ParameterInterval> intervals;
        if (!isValidNurbsCurve(curve) || stroke.isEmpty()) {
            return intervals;
        }

        const QVector<double> fullKnots = expandedKnotVector(curve);
        const qreal domainStart = fullKnots[curve.degree];
        const qreal domainEnd = fullKnots[curve.controlPoints.size()];
        const qreal domainLength = domainEnd - domainStart;
        if (domainLength <= 1.0e-12) {
            return intervals;
        }

        int nonZeroSpans = 0;
        for (int index = curve.degree; index < curve.controlPoints.size(); ++index) {
            if (fullKnots[index + 1] > fullKnots[index]) {
                ++nonZeroSpans;
            }
        }
        const int sampleCount = std::max(256, nonZeroSpans * 128);
        constexpr qreal eraserRadiusPixels = 10.0;
        const auto insideEraser = [&](qreal parameter) {
            QPointF worldPoint;
            if (!evaluateNurbsPoint(curve, parameter, &worldPoint)) {
                return false;
            }
            return distanceToEraserStroke(worldToScreen(worldPoint), stroke) <=
                   eraserRadiusPixels;
        };

        const auto refineBoundary = [&](qreal first,
                                        qreal second,
                                        bool firstInside) {
            qreal low = first;
            qreal high = second;
            for (int iteration = 0; iteration < 30; ++iteration) {
                const qreal middle = (low + high) * 0.5;
                if (insideEraser(middle) == firstInside) {
                    low = middle;
                } else {
                    high = middle;
                }
            }
            return (low + high) * 0.5;
        };

        qreal previousParameter = domainStart;
        bool previousInside = insideEraser(previousParameter);
        qreal intervalStart = previousInside ? domainStart : -1.0;
        for (int sample = 1; sample <= sampleCount; ++sample) {
            const qreal currentParameter =
                domainStart + domainLength * sample / sampleCount;
            const bool currentInside = insideEraser(currentParameter);
            if (currentInside != previousInside) {
                const qreal boundary = refineBoundary(previousParameter,
                                                       currentParameter,
                                                       previousInside);
                if (currentInside) {
                    intervalStart = boundary;
                } else if (intervalStart >= 0.0) {
                    intervals.append(ParameterInterval{intervalStart, boundary});
                    intervalStart = -1.0;
                }
            }
            previousParameter = currentParameter;
            previousInside = currentInside;
        }

        if (intervalStart >= 0.0) {
            intervals.append(ParameterInterval{intervalStart, domainEnd});
        }

        const qreal mergeTolerance = std::max<qreal>(1.0e-9, domainLength * 1.0e-8);
        QVector<ParameterInterval> merged;
        for (const ParameterInterval &interval : intervals) {
            if (interval.end - interval.start <= mergeTolerance) {
                continue;
            }
            if (!merged.isEmpty() && interval.start <= merged.last().end + mergeTolerance) {
                merged.last().end = std::max(merged.last().end, interval.end);
            } else {
                merged.append(interval);
            }
        }
        return merged;
    }

    qreal distanceBetweenScreenSegments(const QPointF &firstStart,
                                        const QPointF &firstEnd,
                                        const QPointF &secondStart,
                                        const QPointF &secondEnd) const
    {
        return std::min({distanceToSegment(firstStart, secondStart, secondEnd),
                         distanceToSegment(firstEnd, secondStart, secondEnd),
                         distanceToSegment(secondStart, firstStart, firstEnd),
                         distanceToSegment(secondEnd, firstStart, firstEnd)});
    }

    QVector<ParameterInterval> eraserIntervalsForSampledCurveSegment(
        const SampledNurbsCurve2D &sampled,
        const QPointF &strokeStart,
        const QPointF &strokeEnd) const
    {
        QVector<ParameterInterval> intervals;
        if (sampled.parameters.size() < 2 ||
            sampled.parameters.size() != sampled.screenPoints.size()) {
            return intervals;
        }

        constexpr qreal eraserRadiusPixels = 10.0;
        const QRectF strokeBounds = QRectF(strokeStart, strokeEnd).normalized();
        const bool hasBounds = sampled.segmentBounds.size() ==
                               sampled.screenPoints.size() - 1;
        bool inside = false;
        qreal intervalStart = 0.0;
        for (int sample = 1; sample < sampled.screenPoints.size(); ++sample) {
            bool segmentInside = false;
            bool boundsMayBeNear = !hasBounds;
            if (hasBounds) {
                const QRectF &curveBounds = sampled.segmentBounds[sample - 1];
                boundsMayBeNear =
                    curveBounds.right() >= strokeBounds.left() - eraserRadiusPixels &&
                    strokeBounds.right() >= curveBounds.left() - eraserRadiusPixels &&
                    curveBounds.bottom() >= strokeBounds.top() - eraserRadiusPixels &&
                    strokeBounds.bottom() >= curveBounds.top() - eraserRadiusPixels;
            }
            if (boundsMayBeNear) {
                segmentInside =
                    distanceBetweenScreenSegments(sampled.screenPoints[sample - 1],
                                                  sampled.screenPoints[sample],
                                                  strokeStart,
                                                  strokeEnd) <= eraserRadiusPixels;
            }

            if (segmentInside && !inside) {
                intervalStart = sampled.parameters[sample - 1];
                inside = true;
            } else if (!segmentInside && inside) {
                intervals.append(ParameterInterval{intervalStart,
                                                   sampled.parameters[sample - 1]});
                inside = false;
            }
        }

        if (inside) {
            intervals.append(ParameterInterval{intervalStart,
                                               sampled.parameters.last()});
        }
        return intervals;
    }

    QVector<ParameterInterval> boundEraseIntervals(
        const Shape::NurbsCurve2D &curve,
        const QVector<ParameterInterval> &hitIntervals,
        const QVector<qreal> &intersectionParameters) const
    {
        if (hitIntervals.isEmpty() || !isValidNurbsCurve(curve)) {
            return {};
        }

        const QVector<double> fullKnots = expandedKnotVector(curve);
        const qreal domainStart = fullKnots[curve.degree];
        const qreal domainEnd = fullKnots[curve.controlPoints.size()];
        const qreal domainLength = domainEnd - domainStart;
        const qreal tolerance = std::max<qreal>(1.0e-9, domainLength * 1.0e-8);

        QVector<qreal> boundaries{domainStart, domainEnd};
        boundaries += intersectionParameters;
        std::sort(boundaries.begin(), boundaries.end());

        QVector<qreal> uniqueBoundaries;
        for (const qreal boundary : boundaries) {
            const qreal clampedBoundary = std::clamp(boundary,
                                                     domainStart,
                                                     domainEnd);
            if (uniqueBoundaries.isEmpty() ||
                clampedBoundary > uniqueBoundaries.back() + tolerance) {
                uniqueBoundaries.append(clampedBoundary);
            } else {
                uniqueBoundaries.back() =
                    std::max(uniqueBoundaries.back(), clampedBoundary);
            }
        }

        QVector<ParameterInterval> removedIntervals;
        for (const ParameterInterval &hit : hitIntervals) {
            const qreal hitMidpoint = (hit.start + hit.end) * 0.5;
            for (int boundaryIndex = 0;
                 boundaryIndex + 1 < uniqueBoundaries.size();
                 ++boundaryIndex) {
                const qreal pieceStart = uniqueBoundaries[boundaryIndex];
                const qreal pieceEnd = uniqueBoundaries[boundaryIndex + 1];
                if (pieceEnd - pieceStart <= tolerance ||
                    hit.end <= pieceStart + tolerance ||
                    hit.start >= pieceEnd - tolerance) {
                    continue;
                }

                // The eraser has a visible radius. Near a real intersection,
                // that radius can overlap both neighboring pieces even when
                // the stroke is centered on only one of them. Use the stroke
                // interval's center to choose the directly erased piece, and
                // also retain pieces whose center is fully covered by a long
                // stroke. This prevents a small brush overlap from deleting
                // past the intersection while still allowing a long drag to
                // remove several complete pieces.
                const qreal pieceMidpoint = (pieceStart + pieceEnd) * 0.5;
                const bool hitCenterIsInPiece =
                    hitMidpoint > pieceStart + tolerance &&
                    hitMidpoint < pieceEnd - tolerance;
                const bool pieceCenterIsInHit =
                    pieceMidpoint >= hit.start - tolerance &&
                    pieceMidpoint <= hit.end + tolerance;
                if (hitCenterIsInPiece || pieceCenterIsInHit) {
                    removedIntervals.append(ParameterInterval{pieceStart, pieceEnd});
                }
            }
        }

        if (removedIntervals.isEmpty()) {
            return removedIntervals;
        }

        // A closed curve's domain ends are the same geometric point, not
        // cutting boundaries. The first and last parameter pieces therefore
        // belong to one section unless another curve intersects the seam.
        QPointF firstPoint;
        QPointF lastPoint;
        const bool closed = evaluateNurbsPoint(curve, domainStart, &firstPoint) &&
                            evaluateNurbsPoint(curve, domainEnd, &lastPoint) &&
                            std::hypot(firstPoint.x() - lastPoint.x(),
                                       firstPoint.y() - lastPoint.y()) <= 1.0e-8;
        const bool seamIsIntersection = std::any_of(
            intersectionParameters.begin(), intersectionParameters.end(),
            [&](qreal parameter) {
                return std::abs(parameter - domainStart) <= tolerance ||
                       std::abs(parameter - domainEnd) <= tolerance;
            });
        if (closed && !seamIsIntersection && uniqueBoundaries.size() > 2) {
            const bool removesFirstPiece = std::any_of(
                removedIntervals.begin(), removedIntervals.end(),
                [&](const ParameterInterval &interval) {
                    return interval.start <= domainStart + tolerance &&
                           interval.end > domainStart + tolerance;
                });
            const bool removesLastPiece = std::any_of(
                removedIntervals.begin(), removedIntervals.end(),
                [&](const ParameterInterval &interval) {
                    return interval.end >= domainEnd - tolerance &&
                           interval.start < domainEnd - tolerance;
                });
            if (removesFirstPiece || removesLastPiece) {
                removedIntervals.append({domainStart, uniqueBoundaries[1]});
                removedIntervals.append({uniqueBoundaries[uniqueBoundaries.size() - 2],
                                         domainEnd});
            }
        }

        std::sort(removedIntervals.begin(),
                  removedIntervals.end(),
                  [](const ParameterInterval &first, const ParameterInterval &second) {
                      return first.start < second.start;
                  });
        QVector<ParameterInterval> merged;
        for (const ParameterInterval &interval : removedIntervals) {
            if (!merged.isEmpty() &&
                interval.start <= merged.last().end + tolerance) {
                merged.last().end = std::max(merged.last().end, interval.end);
            } else {
                merged.append(interval);
            }
        }
        return merged;
    }

    QVector<ParameterInterval> curveIntervalsInsideScreenBox(
        const Shape::NurbsCurve2D &curve,
        const SampledNurbsCurve2D &sampled,
        const QRectF &box) const
    {
        QVector<ParameterInterval> intervals;
        if (!isValidNurbsCurve(curve) || sampled.parameters.size() < 2 ||
            sampled.parameters.size() != sampled.screenPoints.size()) {
            return intervals;
        }

        const QRectF region = box.normalized();
        const auto isInside = [&](qreal parameter) {
            QPointF worldPoint;
            return evaluateNurbsPoint(curve, parameter, &worldPoint) &&
                   region.contains(worldToScreen(worldPoint));
        };
        const auto refineBoundary = [&](qreal first,
                                        qreal second,
                                        bool firstInside) {
            qreal low = first;
            qreal high = second;
            for (int iteration = 0; iteration < 32; ++iteration) {
                const qreal middle = (low + high) * 0.5;
                if (isInside(middle) == firstInside) {
                    low = middle;
                } else {
                    high = middle;
                }
            }
            return (low + high) * 0.5;
        };

        qreal intervalStart = sampled.parameters.first();
        bool previousInside = region.contains(sampled.screenPoints.first());
        bool insideInterval = previousInside;
        for (int sample = 1; sample < sampled.parameters.size(); ++sample) {
            const bool currentInside = region.contains(sampled.screenPoints[sample]);
            if (currentInside != previousInside) {
                const qreal boundary = refineBoundary(sampled.parameters[sample - 1],
                                                       sampled.parameters[sample],
                                                       previousInside);
                if (currentInside) {
                    intervalStart = boundary;
                    insideInterval = true;
                } else if (insideInterval) {
                    intervals.append({intervalStart, boundary});
                    insideInterval = false;
                }
            }
            previousInside = currentInside;
        }
        if (insideInterval) {
            intervals.append({intervalStart, sampled.parameters.last()});
        }

        const qreal domainLength = sampled.parameters.last() -
                                   sampled.parameters.first();
        const qreal tolerance = std::max<qreal>(1.0e-9, domainLength * 1.0e-8);
        intervals.erase(std::remove_if(intervals.begin(), intervals.end(),
                                       [tolerance](const ParameterInterval &interval) {
                                           return interval.end - interval.start <= tolerance;
                                       }),
                        intervals.end());
        return intervals;
    }

    void updateErasePreviewIntervals(bool reset,
                                     int onlyShapeIndex = -1,
                                     int onlyComponentIndex = -1)
    {
        if (eraseStrokeScreenPath_.isEmpty()) {
            return;
        }

        const int strokePointCount = eraseStrokeScreenPath_.size();
        for (EraseCurveSampleCache &targetCurve : eraseTargetCurveCaches_) {
            if (reset || targetCurve.previewStrokePointCount > strokePointCount) {
                targetCurve.previewIntervals.clear();
                targetCurve.previewStrokePointCount = 0;
            }
            if (!eraseCandidateShapeIndices_.contains(
                    shapes_.objectIdAt(targetCurve.shapeIndex))) {
                continue;
            }
            if (onlyShapeIndex >= 0 &&
                (targetCurve.shapeIndex != onlyShapeIndex ||
                 targetCurve.componentIndex != onlyComponentIndex)) {
                targetCurve.previewIntervals.clear();
                targetCurve.previewStrokePointCount = strokePointCount;
                continue;
            }

            QVector<ParameterInterval> newHitIntervals;
            int firstStrokeSegment = targetCurve.previewStrokePointCount;
            if (targetCurve.previewStrokePointCount == 0) {
                newHitIntervals = eraserIntervalsForSampledCurveSegment(
                    targetCurve.sampled,
                    eraseStrokeScreenPath_.first(),
                    eraseStrokeScreenPath_.first());
                firstStrokeSegment = 1;
            }

            for (int strokeSegment = firstStrokeSegment;
                 strokeSegment < strokePointCount;
                 ++strokeSegment) {
                const QVector<ParameterInterval> segmentIntervals =
                    eraserIntervalsForSampledCurveSegment(
                        targetCurve.sampled,
                        eraseStrokeScreenPath_[strokeSegment - 1],
                        eraseStrokeScreenPath_[strokeSegment]);
                for (const ParameterInterval &interval : segmentIntervals) {
                    newHitIntervals.append(interval);
                }
            }

            if (!newHitIntervals.isEmpty()) {
                for (const ParameterInterval &interval : newHitIntervals) {
                    targetCurve.previewIntervals.append(interval);
                }
                targetCurve.previewIntervals = boundEraseIntervals(
                    targetCurve.curve,
                    targetCurve.previewIntervals,
                    targetCurve.intersectionParameters);
            }
            targetCurve.previewStrokePointCount = strokePointCount;
        }
    }

    QVector<ParameterInterval> eraseIntervalsBoundedByIntersections(
        int sourceShapeIndex,
        int sourceComponentIndex,
        const Shape::NurbsCurve2D &curve,
        const QVector<QPointF> &stroke,
        const QVector<qreal> *cachedIntersectionParameters = nullptr) const
    {
        const QVector<ParameterInterval> hitIntervals =
            eraserIntervalsForCurve(curve, stroke);
        if (hitIntervals.isEmpty()) {
            return {};
        }

        const QVector<qreal> intersectionParameters =
            cachedIntersectionParameters != nullptr
                ? *cachedIntersectionParameters
                : eraseIntersectionParameters(sourceShapeIndex,
                                              sourceComponentIndex,
                                              curve);
        return boundEraseIntervals(curve, hitIntervals, intersectionParameters);
    }

    bool trimShapeAtEraserStroke(const Shape &shape,
                                 const QVector<QPointF> &stroke,
                                 QVector<Shape> *replacement,
                                 int sourceShapeIndex = -1,
                                 const QVector<EraseCurveSampleCache> *cachedTargets = nullptr,
                                 const QRectF *trimBox = nullptr,
                                 int onlyComponentIndex = -1) const
    {
        if (replacement == nullptr) {
            return false;
        }
        replacement->clear();

        if (shape.geometryType == GeometryType::Point ||
            shape.geometryType == GeometryType::Rectangle ||
            shape.geometryType == GeometryType::Polygon) {
            return true;
        }

        QVector<Shape::NurbsCurve2D> sourceCurves;
        if (shape.geometryType == GeometryType::PolyCurve) {
            sourceCurves = shape.components;
        } else if (isValidNurbsCurve(shape.nurbs)) {
            sourceCurves.append(shape.nurbs);
        } else if (shape.geometryType == GeometryType::Line && shape.points.size() >= 2) {
            sourceCurves.append(makeDegreeOneNurbs(shape.points));
        } else {
            return true;
        }

        QVector<Shape::NurbsCurve2D> remainingCurves;
        bool changed = false;
        for (int sourceComponentIndex = 0;
             sourceComponentIndex < sourceCurves.size();
             ++sourceComponentIndex) {
            const Shape::NurbsCurve2D &sourceCurve = sourceCurves[sourceComponentIndex];
            if (onlyComponentIndex >= 0 &&
                sourceComponentIndex != onlyComponentIndex) {
                remainingCurves.append(sourceCurve);
                continue;
            }
            const QVector<qreal> *cachedIntersectionParameters = nullptr;
            const EraseCurveSampleCache *cachedTargetCurve = nullptr;
            if (cachedTargets != nullptr) {
                for (const EraseCurveSampleCache &cachedTarget : *cachedTargets) {
                    if (cachedTarget.shapeIndex == sourceShapeIndex &&
                        cachedTarget.componentIndex == sourceComponentIndex) {
                        cachedTargetCurve = &cachedTarget;
                        cachedIntersectionParameters =
                            &cachedTarget.intersectionParameters;
                        break;
                    }
                }
            }
            QVector<ParameterInterval> removedIntervals;
            if (trimBox != nullptr) {
                SampledNurbsCurve2D fallbackSamples;
                const SampledNurbsCurve2D *samples =
                    cachedTargetCurve != nullptr ? &cachedTargetCurve->sampled
                                                 : &fallbackSamples;
                if (cachedTargetCurve == nullptr &&
                    !sampleNurbsCurveForErase(sourceCurve, &fallbackSamples)) {
                    remainingCurves.append(sourceCurve);
                    continue;
                }
                const QVector<ParameterInterval> hitIntervals =
                    curveIntervalsInsideScreenBox(sourceCurve, *samples, *trimBox);
                QVector<qreal> fallbackIntersections;
                const QVector<qreal> *intersections = cachedIntersectionParameters;
                if (intersections == nullptr) {
                    fallbackIntersections = eraseIntersectionParameters(
                        sourceShapeIndex, sourceComponentIndex, sourceCurve);
                    intersections = &fallbackIntersections;
                }
                removedIntervals = boundEraseIntervals(sourceCurve,
                                                       hitIntervals,
                                                       *intersections);
            } else {
                removedIntervals = eraseIntervalsBoundedByIntersections(
                    sourceShapeIndex,
                    sourceComponentIndex,
                    sourceCurve,
                    stroke,
                    cachedIntersectionParameters);
            }
            if (removedIntervals.isEmpty()) {
                remainingCurves.append(sourceCurve);
                continue;
            }

            changed = true;
            const QVector<double> fullKnots = expandedKnotVector(sourceCurve);
            const qreal domainStart = fullKnots[sourceCurve.degree];
            const qreal domainEnd = fullKnots[sourceCurve.controlPoints.size()];
            qreal keepStart = domainStart;
            const qreal tolerance =
                std::max<qreal>(1.0e-9, std::abs(domainEnd - domainStart) * 1.0e-9);
            for (const ParameterInterval &removed : removedIntervals) {
                if (removed.start - keepStart > tolerance) {
                    Shape::NurbsCurve2D kept;
                    if (!trimNurbsCurve(sourceCurve, keepStart, removed.start, &kept)) {
                        return false;
                    }
                    remainingCurves.append(kept);
                }
                keepStart = std::max(keepStart, removed.end);
            }
            if (domainEnd - keepStart > tolerance) {
                Shape::NurbsCurve2D kept;
                if (!trimNurbsCurve(sourceCurve, keepStart, domainEnd, &kept)) {
                    return false;
                }
                remainingCurves.append(kept);
            }
        }

        if (!changed) {
            return false;
        }
        if (remainingCurves.isEmpty()) {
            return true;
        }

        for (const QVector<Shape::NurbsCurve2D> &connectedCurves :
             connectedCurveGroups(remainingCurves)) {
            // Keep fragments that still meet at a closed curve's seam in one
            // selectable object, but separate disconnected trim remainders.
            Shape updated = shape;
            if (shape.geometryType == GeometryType::Line && connectedCurves.size() == 1) {
                updated.geometryType = GeometryType::Line;
                updated.nurbs = connectedCurves.first();
                updated.points = updated.nurbs.controlPoints;
                updated.subdivisionParameters.clear();
                updated.components.clear();
            } else {
                updated = polyCurveShapeForComponents(shape, connectedCurves);
            }
            replacement->append(updated);
        }
        return true;
    }

    void eraseAlongScreenSegment(const QPointF &start, const QPointF &end)
    {
        constexpr qreal eraserRadiusPixels = 10.0;
        constexpr qreal sampleSpacingPixels = 5.0;
        const qreal length = std::hypot(end.x() - start.x(), end.y() - start.y());
        const int sampleCount = std::max(
            1,
            static_cast<int>(std::ceil(length / sampleSpacingPixels)));

        int addedCandidates = 0;
        for (int sample = 0; sample <= sampleCount; ++sample) {
            const qreal fraction = static_cast<qreal>(sample) / sampleCount;
            const QPointF cursor = start + (end - start) * fraction;
            for (const ObjectId objectId : eraseTargetShapeIndices_) {
                const int shapeIndex = objectIndex(objectId);
                if (shapeIndex < 0 || shapeIndex >= shapes_.size()) {
                    continue;
                }
                if (eraseCandidateShapeIndices_.contains(objectId)) {
                    continue;
                }

                if (distanceToCachedEraseShape(cursor, shapeIndex) <= eraserRadiusPixels) {
                    eraseCandidateShapeIndices_.append(objectId);
                    ++addedCandidates;
                }
            }
        }

        if (addedCandidates > 0) {
            DebugLog::instance().write(
                QStringLiteral("erase candidates added=%1 total=%2")
                    .arg(addedCandidates)
                    .arg(eraseCandidateShapeIndices_.size()));
        }
    }

    void applyEraseCandidates(const QRectF *trimBox = nullptr,
                              int onlyComponentIndex = -1)
    {
        if (eraseCandidateShapeIndices_.isEmpty()) {
            return;
        }

        QVector<int> indices;
        for (const ObjectId objectId : eraseCandidateShapeIndices_) {
            const int index = objectIndex(objectId);
            if (index >= 0) {
                indices.append(index);
            }
        }
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());

        QVector<QPair<int, QVector<Shape>>> changes;
        for (auto iterator = indices.crbegin(); iterator != indices.crend(); ++iterator) {
            if (*iterator < 0 || *iterator >= shapes_.size()) {
                continue;
            }

            QVector<Shape> replacement;
            if (trimShapeAtEraserStroke(shapes_[*iterator],
                                        eraseStrokeScreenPath_,
                                        &replacement,
                                        *iterator,
                                        &eraseTargetCurveCaches_,
                                        trimBox,
                                        onlyComponentIndex)) {
                changes.append(qMakePair(*iterator, replacement));
            }
        }

        if (changes.isEmpty()) {
            DebugLog::instance().write(QStringLiteral("erase found no trim interval"));
            return;
        }

        recordGeometryChange();
        int removedCount = 0;
        QVector<ObjectId> removedObjectIds;
        for (const QPair<int, QVector<Shape>> &change : changes) {
            const int shapeIndex = change.first;
            if (shapeIndex < 0 || shapeIndex >= shapes_.size()) {
                continue;
            }

            const ObjectId objectId = shapes_.objectIdAt(shapeIndex);
            if (change.second.isEmpty()) {
                shapes_.removeAt(shapeIndex);
                removedObjectIds.append(objectId);
                ++removedCount;
            } else {
                const SceneObject *sourceObject = document_.object(objectId);
                const SceneObject sourceObjectCopy = sourceObject != nullptr
                                                         ? *sourceObject
                                                         : SceneObject{};
                shapes_.replace(objectId, change.second.first());
                for (int pieceIndex = 1; pieceIndex < change.second.size(); ++pieceIndex) {
                    SceneObject piece = sourceObjectCopy;
                    piece.id = ObjectId::invalid();
                    piece.geometry = change.second[pieceIndex];
                    shapes_.insertObject(shapeIndex + pieceIndex, piece);
                }
            }
        }

        const QVector<ObjectId> oldSelectedShapeIndices = selectedShapeIndices_;
        const ObjectId oldSelectedShapeIndex = selectedShapeIndex_;
        selectedShapeIndices_.clear();
        for (const ObjectId objectId : oldSelectedShapeIndices) {
            if (objectIndex(objectId) >= 0 && !removedObjectIds.contains(objectId)) {
                selectedShapeIndices_.append(objectId);
            }
        }
        selectedShapeIndex_ = objectIndex(oldSelectedShapeIndex) >= 0 &&
                                      !removedObjectIds.contains(oldSelectedShapeIndex)
                                  ? oldSelectedShapeIndex
                                  : ObjectId::invalid();
        if (!selectedShapeIndex_.isValid() && !selectedShapeIndices_.isEmpty()) {
            selectedShapeIndex_ = selectedShapeIndices_.back();
        }
        draggingSelected_ = false;
        draggingShapeIndices_.clear();
        draggingControlPoint_ = false;
        controlPointIndex_ = -1;
        currentDragSnap_ = DragSnapResult{};
        dragSnapLocked_ = false;
        dragHistoryRecorded_ = false;
        DebugLog::instance().write(QStringLiteral("erase segment applied candidates=%1 changed=%2 removed=%3 shapes=%4")
                                       .arg(indices.size())
                                       .arg(changes.size())
                                       .arg(removedCount)
                                       .arg(shapes_.size()));
        update();
    }

    void cancelEraseStroke()
    {
        eraseStrokeActive_ = false;
        eraseCursorPressed_ = false;
        eraseCandidateShapeIndices_.clear();
        eraseStrokeScreenPath_.clear();
        update();
        DebugLog::instance().write(QStringLiteral("erase stroke canceled"));
    }

    void exitEraseLikeTool()
    {
        cancelEraseStroke();
        setTool(Tool::Select);
        if (commandFinished_) {
            commandFinished_(Tool::Select);
        }
        DebugLog::instance().write(QStringLiteral("erase-like tool exited"));
    }

    void resetScaleInteraction()
    {
        scaleShapeIds_.clear();
        scaleStep_ = 0;
        scaleBaseWorld_ = QPointF();
        scaleReferenceWorld_ = QPointF();
        scaleAxisDirection_ = QPointF();
        scaleReferenceLength_ = 0.0;
        scaleUsingTypedFactor_ = false;
        scaleTypedFactor_ = 1.0;
        scaleFactorText_.clear();
        scalePreviewFactor_ = 1.0;
        scalePreviewAxis_ = QPointF();
        scalePreviewValid_ = false;
    }

    QString scalePrompt() const
    {
        if (scaleStep_ == 0) {
            return QStringLiteral("%1: click a base point, or press Enter for the selection center")
                .arg(scaleModeName(scaleMode_));
        }
        if (scaleStep_ == 1) {
            const QString numeric = scaleFactorText_.isEmpty()
                                        ? QString()
                                        : QStringLiteral("  Factor: %1").arg(scaleFactorText_);
            return QStringLiteral("%1: type a factor + Enter, or click the first reference point%2")
                .arg(scaleModeName(scaleMode_), numeric);
        }
        if (scaleUsingTypedFactor_) {
            return QStringLiteral("Scale 1D: click the scale direction  •  Factor %1")
                .arg(scaleTypedFactor_, 0, 'g', 8);
        }
        return scaleMode_ == ScaleMode::OneD
                   ? QStringLiteral("Scale 1D: click the second reference point along the first-point axis")
                   : QStringLiteral("%1: click the second reference point")
                         .arg(scaleModeName(scaleMode_));
    }

    void publishScalePrompt()
    {
        if (toolStatusUpdate_ != nullptr) {
            toolStatusUpdate_(scalePrompt());
        }
    }

    QPointF scaleSelectionBoundsCenter() const
    {
        QRectF combinedBounds;
        bool hasBounds = false;
        for (const ObjectId objectId : scaleShapeIds_) {
            const int shapeIndex = objectIndex(objectId);
            if (shapeIndex < 0) {
                continue;
            }
            const QRectF bounds = selectionBoundsForShape(shapes_[shapeIndex]);
            if (bounds.isNull()) {
                continue;
            }
            combinedBounds = hasBounds ? combinedBounds.united(bounds) : bounds;
            hasBounds = true;
        }
        return hasBounds ? screenToWorld(combinedBounds.center()) : QPointF();
    }

    void scaleShapeGeometry(Shape *shape,
                            const QPointF &base,
                            const QPointF &axisDirection,
                            qreal factor,
                            ScaleMode mode) const
    {
        if (shape == nullptr) {
            return;
        }

        if (mode == ScaleMode::OneD &&
            shape->geometryType == GeometryType::Rectangle && shape->points.size() == 2) {
            shape->points = rectangleVertices(*shape);
        }

        const auto scaledPoint = [base, axisDirection, factor, mode](const QPointF &point) {
            const QPointF offset = point - base;
            if (mode != ScaleMode::OneD) {
                return base + offset * factor;
            }
            const qreal alongAxis = QPointF::dotProduct(offset, axisDirection);
            return base + offset + axisDirection * (alongAxis * (factor - 1.0));
        };

        for (QPointF &point : shape->points) {
            point = scaledPoint(point);
        }
        for (QPointF &point : shape->nurbs.controlPoints) {
            point = scaledPoint(point);
        }
        for (Shape::NurbsCurve2D &component : shape->components) {
            for (QPointF &point : component.controlPoints) {
                point = scaledPoint(point);
            }
        }
    }

    qreal scaleFactorAtPoint(const QPointF &point) const
    {
        if (scaleReferenceLength_ <= 1.0e-12) {
            return 1.0;
        }
        const QPointF offset = point - scaleBaseWorld_;
        if (scaleMode_ == ScaleMode::OneD) {
            return QPointF::dotProduct(offset, scaleAxisDirection_) /
                   scaleReferenceLength_;
        }
        return std::hypot(offset.x(), offset.y()) / scaleReferenceLength_;
    }

    void updateScalePreview(const QPointF &point)
    {
        scalePreviewValid_ = false;
        if (activeTool_ != Tool::Scale || scaleStep_ != 2) {
            return;
        }

        if (scaleUsingTypedFactor_) {
            const QPointF direction = point - scaleBaseWorld_;
            const qreal length = std::hypot(direction.x(), direction.y());
            if (length <= 1.0e-12) {
                return;
            }
            scalePreviewAxis_ = direction / length;
            scalePreviewFactor_ = scaleTypedFactor_;
        } else {
            scalePreviewAxis_ = scaleAxisDirection_;
            scalePreviewFactor_ = scaleFactorAtPoint(point);
        }

        scalePreviewValid_ = std::isfinite(scalePreviewFactor_) &&
                             (scaleMode_ != ScaleMode::OneD ||
                              std::hypot(scalePreviewAxis_.x(),
                                         scalePreviewAxis_.y()) > 1.0e-12);
    }

    void finishScale()
    {
        resetScaleInteraction();
        currentSnap_ = SnapResult{};
        setTool(Tool::Select);
        if (commandFinished_) {
            commandFinished_(Tool::Select);
        }
        update();
    }

    void commitScale(qreal factor, const QPointF &axisDirection)
    {
        if (!std::isfinite(factor)) {
            return;
        }

        const bool changed = std::abs(factor - 1.0) > 1.0e-12;
        if (changed) {
            recordGeometryChange();
            for (const ObjectId objectId : scaleShapeIds_) {
                const int shapeIndex = objectIndex(objectId);
                if (shapeIndex >= 0 && document_.isObjectEditable(objectId)) {
                    scaleShapeGeometry(&shapes_[shapeIndex],
                                       scaleBaseWorld_,
                                       axisDirection,
                                       factor,
                                       scaleMode_);
                }
            }
            updateAssociativeDimensions(document_, curveSampler_);
            notifyLayersChanged();
        }

        DebugLog::instance().write(QStringLiteral("scale committed mode=%1 factor=%2 base=%3 objects=%4 changed=%5")
                                       .arg(scaleModeName(scaleMode_))
                                       .arg(factor, 0, 'g', 10)
                                       .arg(pointText(scaleBaseWorld_))
                                       .arg(scaleShapeIds_.size())
                                       .arg(changed));
        finishScale();
    }

    bool handleScalePoint(const QPointF &worldPoint)
    {
        if (activeTool_ != Tool::Scale || scaleShapeIds_.isEmpty()) {
            return false;
        }

        if (scaleStep_ == 0) {
            scaleBaseWorld_ = worldPoint;
            scaleStep_ = 1;
            pendingPoints_ = {scaleBaseWorld_};
            publishScalePrompt();
            update();
            return true;
        }

        if (scaleStep_ == 1) {
            const QPointF reference = worldPoint - scaleBaseWorld_;
            scaleReferenceLength_ = std::hypot(reference.x(), reference.y());
            if (scaleReferenceLength_ <= 1.0e-12) {
                return false;
            }
            scaleReferenceWorld_ = worldPoint;
            scaleAxisDirection_ = reference / scaleReferenceLength_;
            scaleUsingTypedFactor_ = false;
            scaleStep_ = 2;
            updateScalePreview(worldPoint);
            publishScalePrompt();
            update();
            return true;
        }

        updateScalePreview(worldPoint);
        if (!scalePreviewValid_) {
            return false;
        }
        commitScale(scalePreviewFactor_, scalePreviewAxis_);
        return true;
    }

    void cancelScale()
    {
        if (activeTool_ != Tool::Scale && scaleShapeIds_.isEmpty()) {
            return;
        }
        resetScaleInteraction();
        pendingPoints_.clear();
        currentSnap_ = SnapResult{};
        setTool(Tool::Select);
        if (commandFinished_) {
            commandFinished_(Tool::Select);
        }
        update();
        DebugLog::instance().write(QStringLiteral("scale canceled"));
    }

    void resetRotateInteraction()
    {
        rotateShapeIndices_.clear();
        rotateStep_ = 0;
        rotateBaseWorld_ = QPointF();
        rotateReferenceWorld_ = QPointF();
        rotatePreviewAngle_ = 0.0;
    }

    qreal rotationAngleForPoint(const QPointF &worldPoint) const
    {
        const QPointF startVector = rotateReferenceWorld_ - rotateBaseWorld_;
        const QPointF endVector = worldPoint - rotateBaseWorld_;
        if (std::hypot(startVector.x(), startVector.y()) <= 1.0e-12 ||
            std::hypot(endVector.x(), endVector.y()) <= 1.0e-12) {
            return 0.0;
        }

        constexpr qreal pi = 3.14159265358979323846;
        constexpr qreal twoPi = 2.0 * pi;
        qreal angle = std::atan2(endVector.y(), endVector.x()) -
                      std::atan2(startVector.y(), startVector.x());
        while (angle > pi) {
            angle -= twoPi;
        }
        while (angle < -pi) {
            angle += twoPi;
        }
        return angle;
    }

    QPointF rotatePointAround(const QPointF &point,
                              const QPointF &base,
                              qreal angle) const
    {
        const qreal cosine = std::cos(angle);
        const qreal sine = std::sin(angle);
        const QPointF offset = point - base;
        return base + QPointF(offset.x() * cosine - offset.y() * sine,
                              offset.x() * sine + offset.y() * cosine);
    }

    void rotateShapeGeometry(Shape *shape,
                             const QPointF &base,
                             qreal angle) const
    {
        if (shape == nullptr) {
            return;
        }

        if (shape->geometryType == GeometryType::Rectangle && shape->points.size() == 2) {
            shape->points = rectangleVertices(*shape);
        }
        for (QPointF &point : shape->points) {
            point = rotatePointAround(point, base, angle);
        }
        for (QPointF &point : shape->nurbs.controlPoints) {
            point = rotatePointAround(point, base, angle);
        }
        for (Shape::NurbsCurve2D &component : shape->components) {
            for (QPointF &point : component.controlPoints) {
                point = rotatePointAround(point, base, angle);
            }
        }
    }

    void rotateShapes(const QVector<ObjectId> &objectIds,
                      const QPointF &base,
                      qreal angle)
    {
        for (const ObjectId objectId : objectIds) {
            const int shapeIndex = objectIndex(objectId);
            if (shapeIndex >= 0) {
                rotateShapeGeometry(&shapes_[shapeIndex], base, angle);
            }
        }
    }

    bool handleRotatePoint(const QPointF &worldPoint)
    {
        if (activeTool_ != Tool::Rotate || rotateShapeIndices_.isEmpty()) {
            return false;
        }

        constexpr qreal minimumPointDistance = 1.0e-9;
        if (rotateStep_ == 0) {
            rotateBaseWorld_ = worldPoint;
            rotateStep_ = 1;
            rotatePreviewAngle_ = 0.0;
            DebugLog::instance().write(QStringLiteral("rotate center=%1")
                                           .arg(pointText(rotateBaseWorld_)));
            update();
            return true;
        }

        if (rotateStep_ == 1) {
            if (std::hypot(worldPoint.x() - rotateBaseWorld_.x(),
                           worldPoint.y() - rotateBaseWorld_.y()) <=
                minimumPointDistance) {
                DebugLog::instance().write(QStringLiteral("rotate reference ignored at center"));
                return false;
            }

            rotateReferenceWorld_ = worldPoint;
            rotateStep_ = 2;
            rotatePreviewAngle_ = 0.0;
            DebugLog::instance().write(QStringLiteral("rotate reference=%1")
                                           .arg(pointText(rotateReferenceWorld_)));
            update();
            return true;
        }

        if (std::hypot(worldPoint.x() - rotateBaseWorld_.x(),
                       worldPoint.y() - rotateBaseWorld_.y()) <=
            minimumPointDistance) {
            DebugLog::instance().write(QStringLiteral("rotate final point ignored at center"));
            return false;
        }

        const qreal angle = rotationAngleForPoint(worldPoint);
        if (std::abs(angle) > 1.0e-12) {
            recordGeometryChange();
            rotateShapes(rotateShapeIndices_, rotateBaseWorld_, angle);
        }

        DebugLog::instance().write(QStringLiteral("rotate committed shapes=%1 angleDegrees=%2 base=%3 reference=%4 final=%5")
                                       .arg(rotateShapeIndices_.size())
                                       .arg(angle * 180.0 / 3.14159265358979323846, 0, 'f', 4)
                                       .arg(pointText(rotateBaseWorld_))
                                       .arg(pointText(rotateReferenceWorld_))
                                       .arg(pointText(worldPoint)));
        resetRotateInteraction();
        setTool(Tool::Select);
        if (commandFinished_) {
            commandFinished_(Tool::Select);
        }
        update();
        return true;
    }

    void cancelRotate()
    {
        if (activeTool_ != Tool::Rotate && rotateShapeIndices_.isEmpty()) {
            return;
        }

        resetRotateInteraction();
        setTool(Tool::Select);
        if (commandFinished_) {
            commandFinished_(Tool::Select);
        }
        update();
        DebugLog::instance().write(QStringLiteral("rotate canceled"));
    }

    void resetMirrorInteraction()
    {
        mirrorShapeIndices_.clear();
    }

    bool handleMirrorPoint(const QPointF &worldPoint)
    {
        if (activeTool_ != Tool::Mirror || mirrorShapeIndices_.isEmpty()) {
            return false;
        }

        pendingPoints_.append(worldPoint);
        if (pendingPoints_.size() < 2) {
            update();
            return true;
        }

        const QPointF axisStart = pendingPoints_[0];
        const QPointF axisEnd = pendingPoints_[1];
        QVector<SceneObject> mirroredObjects;
        mirroredObjects.reserve(mirrorShapeIndices_.size());
        for (const ObjectId objectId : mirrorShapeIndices_) {
            const SceneObject *sourceObject = document_.object(objectId);
            if (sourceObject == nullptr || !document_.isObjectEditable(objectId)) {
                continue;
            }

            Shape mirroredShape;
            if (!mirrorShapeAcrossLine(sourceObject->geometry,
                                       axisStart,
                                       axisEnd,
                                       &mirroredShape)) {
                continue;
            }

            SceneObject mirroredObject;
            mirroredObject.layerId = sourceObject->layerId;
            mirroredObject.geometry = mirroredShape;
            mirroredObjects.append(mirroredObject);
        }

        if (mirroredObjects.isEmpty()) {
            DebugLog::instance().write(QStringLiteral("mirror rejected axis=%1->%2 objects=%3")
                                           .arg(pointText(axisStart), pointText(axisEnd))
                                           .arg(mirrorShapeIndices_.size()));
            pendingPoints_.removeLast();
            update();
            return false;
        }

        const int sourceCount = mirrorShapeIndices_.size();
        recordGeometryChange();
        QVector<ObjectId> mirroredIds;
        mirroredIds.reserve(mirroredObjects.size());
        for (const SceneObject &mirroredObject : mirroredObjects) {
            mirroredIds.append(shapes_.insertObject(shapes_.size(), mirroredObject));
        }

        selection_.setObjectIds(mirroredIds);
        selectedShapeIndex_ = selection_.primaryObjectId();
        pendingPoints_.clear();
        resetMirrorInteraction();
        setTool(Tool::Select);
        if (commandFinished_) {
            commandFinished_(Tool::Select);
        }
        notifyLayersChanged();
        update();
        DebugLog::instance().write(QStringLiteral("mirror committed sourceCount=%1 copyCount=%2 axis=%3->%4")
                                       .arg(sourceCount)
                                       .arg(mirroredIds.size())
                                       .arg(pointText(axisStart), pointText(axisEnd)));
        return true;
    }

    void cancelMirror()
    {
        if (activeTool_ != Tool::Mirror && mirrorShapeIndices_.isEmpty()) {
            return;
        }

        pendingPoints_.clear();
        resetMirrorInteraction();
        setTool(Tool::Select);
        if (commandFinished_) {
            commandFinished_(Tool::Select);
        }
        update();
        DebugLog::instance().write(QStringLiteral("mirror canceled"));
    }

    void translateControlPoint(ObjectId objectId,
                               int controlPointIndex,
                               const QPointF &delta)
    {
        const int shapeIndex = objectIndex(objectId);
        if (shapeIndex < 0 || shapeIndex >= shapes_.size() ||
            controlPointIndex < 0 ||
            (qFuzzyIsNull(delta.x()) && qFuzzyIsNull(delta.y()))) {
            return;
        }

        Shape &shape = shapes_[shapeIndex];
        if (shape.geometryType == GeometryType::PolyCurve) {
            int remaining = controlPointIndex;
            for (int componentIndex = 0; componentIndex < shape.components.size(); ++componentIndex) {
                Shape::NurbsCurve2D &component = shape.components[componentIndex];
                if (remaining < component.controlPoints.size()) {
                    const QPointF oldPoint = component.controlPoints[remaining];
                    component.controlPoints[remaining] += delta;
                    const qreal seamTolerance = joinEndpointTolerance();
                    if (remaining == 0 && componentIndex > 0) {
                        Shape::NurbsCurve2D &previous = shape.components[componentIndex - 1];
                        if (!previous.controlPoints.isEmpty() &&
                            std::hypot(previous.controlPoints.last().x() - oldPoint.x(),
                                       previous.controlPoints.last().y() - oldPoint.y()) <=
                                seamTolerance) {
                            previous.controlPoints.last() += delta;
                        }
                    }
                    if (remaining == component.controlPoints.size() - 1 &&
                        componentIndex + 1 < shape.components.size()) {
                        Shape::NurbsCurve2D &next = shape.components[componentIndex + 1];
                        if (!next.controlPoints.isEmpty() &&
                            std::hypot(next.controlPoints.first().x() - oldPoint.x(),
                                       next.controlPoints.first().y() - oldPoint.y()) <=
                                seamTolerance) {
                            next.controlPoints.first() += delta;
                        }
                    }
                    shape.points = polyCurvePoints(shape.components);
                    return;
                }
                remaining -= component.controlPoints.size();
            }
            return;
        }

        if (!shape.nurbs.controlPoints.isEmpty()) {
            if (controlPointIndex >= shape.nurbs.controlPoints.size()) {
                return;
            }

            const QPointF newControlPoint =
                shape.nurbs.controlPoints[controlPointIndex] + delta;
            if (!setClosedNurbsSeamControlPoint(&shape,
                                               controlPointIndex,
                                               newControlPoint)) {
                shape.nurbs.controlPoints[controlPointIndex] = newControlPoint;
            }

            // These curve types keep their source points in the same order as
            // their NURBS CVs. Keep both representations synchronized. Arc
            // and circle construction points intentionally remain unchanged;
            // their stored NURBS is the geometry being edited.
            if ((shape.geometryType == GeometryType::Line || shape.geometryType == GeometryType::Bezier ||
                 shape.geometryType == GeometryType::Nurbs) &&
                controlPointIndex < shape.points.size()) {
                shape.points[controlPointIndex] += delta;
            }
            return;
        }

        if (controlPointIndex < shape.points.size()) {
            shape.points[controlPointIndex] += delta;
        }
    }

    void translateShape(ObjectId objectId, const QPointF &delta)
    {
        const int index = objectIndex(objectId);
        if (index < 0 || index >= shapes_.size()) {
            return;
        }

        Shape &shape = shapes_[index];
        for (QPointF &point : shape.points) {
            point += delta;
        }
        for (QPointF &point : shape.nurbs.controlPoints) {
            point += delta;
        }
        for (Shape::NurbsCurve2D &component : shape.components) {
            for (QPointF &point : component.controlPoints) {
                point += delta;
            }
        }
    }

    void translateShapeGeometry(Shape &shape, const QPointF &delta) const
    {
        for (QPointF &point : shape.points) {
            point += delta;
        }
        for (QPointF &point : shape.nurbs.controlPoints) {
            point += delta;
        }
        for (Shape::NurbsCurve2D &component : shape.components) {
            for (QPointF &point : component.controlPoints) {
                point += delta;
            }
        }
    }

    void translateShapes(const QVector<ObjectId> &objectIds, const QPointF &delta)
    {
        for (const ObjectId objectId : objectIds) {
            translateShape(objectId, delta);
        }
    }

    QPointF constrainDragDelta(const QPointF &delta) const
    {
        switch (dragAxisLock_) {
        case DragAxisLock::X:
            return QPointF(delta.x(), 0.0);
        case DragAxisLock::Y:
            return QPointF(0.0, delta.y());
        case DragAxisLock::None:
            return delta;
        }

        return delta;
    }

    void updateGrabPosition(const QVector<ObjectId> &dragIndices)
    {
        if (!grabActive_ || grabPickingBasePoint_) {
            return;
        }

        if (dragSnapLocked_) {
            constexpr qreal dragSnapBreakawayPixels = 18.0;
            const QPointF cursorScreen = worldToScreen(rawCursorWorld_);
            const QPointF snapScreen = worldToScreen(dragSnapCursorWorld_);
            const qreal cursorDistanceFromSnap =
                std::hypot(cursorScreen.x() - snapScreen.x(),
                           cursorScreen.y() - snapScreen.y());
            if (cursorDistanceFromSnap <= dragSnapBreakawayPixels) {
                return;
            }
            dragSnapLocked_ = false;
            currentDragSnap_ = DragSnapResult{};
        }

        // Rebuild the preview from the saved document so axis changes and snap
        // changes never accumulate an additional incremental delta.
        document_.restoreSnapshot(grabStartSnapshot_);
        QPointF totalDelta;
        QPointF freeDestination;
        if (grabHasBasePoint_) {
            const QPointF destinationCursor = rawCursorWorld_ - grabCursorOffset_;
            currentSnap_ = findGrabDestinationSnap(destinationCursor);
            const QPointF destination = currentSnap_.isValid()
                                            ? currentSnap_.point
                                            : destinationCursor;
            cursorWorld_ = destination;
            lastWorldPosition_ = destination;
            freeDestination = destinationCursor;
            totalDelta = destination - grabBasePoint_;
        } else {
            currentSnap_ = SnapResult{};
            totalDelta = rawCursorWorld_ - grabStartWorld_;
        }
        QPointF delta = constrainDragDelta(totalDelta);
        if (grabHasBasePoint_ && currentSnap_.isValid() &&
            (!qFuzzyIsNull(delta.x() - totalDelta.x()) ||
             !qFuzzyIsNull(delta.y() - totalDelta.y()))) {
            // Do not show a snap marker for a destination the axis lock makes
            // impossible to reach. The cursor still projects onto the locked
            // axis, as it does for an ordinary constrained Grab.
            currentSnap_ = SnapResult{};
            cursorWorld_ = freeDestination;
            lastWorldPosition_ = freeDestination;
            totalDelta = freeDestination - grabBasePoint_;
            delta = constrainDragDelta(totalDelta);
        }
        if (!qFuzzyIsNull(delta.x()) || !qFuzzyIsNull(delta.y())) {
            translateShapes(dragIndices, delta);
            grabMoved_ = true;
        } else {
            grabMoved_ = false;
        }
        currentDragSnap_ = DragSnapResult{};
        if (grabHasBasePoint_ && dragAxisLock_ == DragAxisLock::None) {
            constexpr int maximumTangentRefinements = 12;
            constexpr qreal tangentSnapPrecisionPixels = 0.01;
            QPointF totalTangentCorrection;
            for (int iteration = 0; iteration < maximumTangentRefinements; ++iteration) {
                const DragSnapResult dragSnap = findDragSnap(dragIndices, true);
                if (dragSnap.type != SnapType::Tangent) {
                    break;
                }

                currentDragSnap_ = dragSnap;
                const QPointF sourceScreen = worldToScreen(dragSnap.sourcePoint);
                const QPointF targetScreen = worldToScreen(dragSnap.targetPoint);
                const qreal remainingError =
                    std::hypot(targetScreen.x() - sourceScreen.x(),
                               targetScreen.y() - sourceScreen.y());
                if (remainingError <= tangentSnapPrecisionPixels) {
                    break;
                }

                translateShapes(dragIndices, dragSnap.translation);
                totalTangentCorrection += dragSnap.translation;
                grabMoved_ = true;
            }

            const DragSnapResult finalTangentSnap = findDragSnap(dragIndices, true);
            bool preciselySnapped = finalTangentSnap.type == SnapType::Tangent;
            if (preciselySnapped) {
                const QPointF sourceScreen = worldToScreen(finalTangentSnap.sourcePoint);
                const QPointF targetScreen = worldToScreen(finalTangentSnap.targetPoint);
                preciselySnapped =
                    std::hypot(targetScreen.x() - sourceScreen.x(),
                               targetScreen.y() - sourceScreen.y()) <=
                    tangentSnapPrecisionPixels;
            }

            if (preciselySnapped) {
                currentDragSnap_ = finalTangentSnap;
                currentSnap_ = SnapResult{};
                grabMoved_ = true;
                dragSnapLocked_ = true;
                dragSnapCursorWorld_ = rawCursorWorld_;
                DebugLog::instance().write(
                    QStringLiteral("grab endpoint tangent snap source=%1 target=%2 delta=%3")
                        .arg(pointText(currentDragSnap_.sourcePoint))
                        .arg(pointText(currentDragSnap_.targetPoint))
                        .arg(pointText(currentDragSnap_.translation)));
            } else {
                if (!qFuzzyIsNull(totalTangentCorrection.x()) ||
                    !qFuzzyIsNull(totalTangentCorrection.y())) {
                    translateShapes(dragIndices, -totalTangentCorrection);
                }
                currentDragSnap_ = DragSnapResult{};
            }
        }
        lastDragWorld_ = rawCursorWorld_;
        DebugLog::instance().write(
            QStringLiteral("grab move delta=%1 cursorWorld=%2 basePoint=%3 snap=%4 axisLock=%5")
                .arg(pointText(delta))
                .arg(pointText(rawCursorWorld_))
                .arg(grabHasBasePoint_ ? pointText(grabBasePoint_) : QStringLiteral("none"))
                .arg(snapTypeName(currentSnap_.type))
                .arg(dragAxisLockName(dragAxisLock_)));
    }

    QPointF screenToWorld(const QPointF &screen) const
    {
        return viewportTransform_.screenToWorld(screen, size());
    }

    QPointF worldToScreen(const QPointF &world) const
    {
        return viewportTransform_.worldToScreen(world, size());
    }

    void drawSnapMarker(QPainter &painter,
                        SnapType type,
                        const QPointF &worldPoint)
    {
        viewportOverlay_.drawSnapMarker(painter, type, worldPoint, size());
    }

    void drawLineToolPreview(QPainter &painter)
    {
        viewportOverlay_.drawLinePreview(painter,
                                         pendingPoints_,
                                         cursorWorld_,
                                         cursorValid_,
                                         currentSnap_,
                                         size());
    }

    void drawMirrorToolPreview(QPainter &painter)
    {
        if (pendingPoints_.isEmpty() || !cursorValid_) {
            return;
        }

        const QPointF axisStart = pendingPoints_.first();
        const QPointF axisEnd = cursorWorld_;
        for (const ObjectId objectId : mirrorShapeIndices_) {
            const int shapeIndex = objectIndex(objectId);
            if (shapeIndex < 0 || shapeIndex >= shapes_.size() ||
                !document_.isObjectVisible(objectId)) {
                continue;
            }

            Shape mirroredShape;
            if (mirrorShapeAcrossLine(shapes_[shapeIndex],
                                      axisStart,
                                      axisEnd,
                                      &mirroredShape)) {
                drawShape(painter, mirroredShape, true, false, false);
                if (!subdivisionActive_ || objectId != subdivisionShapeIndex_) {
                    drawSubdivisionPoints(painter,
                                          mirroredShape,
                                          mirroredShape.subdivisionParameters,
                                          false);
                }
            }
        }
    }

    bool makeCircularArcGeometry(const QPointF &startWorld,
                                 const QPointF &endWorld,
                                 const QPointF &throughWorld,
                                 QPointF *center,
                                 qreal *radius,
                                 qreal *startAngle,
                                 qreal *sweepAngle) const
    {
        const QPointF start = worldToScreen(startWorld);
        const QPointF end = worldToScreen(endWorld);
        const QPointF through = worldToScreen(throughWorld);

        const qreal startSquared = start.x() * start.x() + start.y() * start.y();
        const qreal endSquared = end.x() * end.x() + end.y() * end.y();
        const qreal throughSquared =
            through.x() * through.x() + through.y() * through.y();
        const qreal denominator = 2.0 *
            (start.x() * (end.y() - through.y()) +
             end.x() * (through.y() - start.y()) +
             through.x() * (start.y() - end.y()));

        if (std::abs(denominator) < 1e-9) {
            return false;
        }

        const QPointF circleCenter(
            (startSquared * (end.y() - through.y()) +
             endSquared * (through.y() - start.y()) +
             throughSquared * (start.y() - end.y())) / denominator,
            (startSquared * (through.x() - end.x()) +
             endSquared * (start.x() - through.x()) +
             throughSquared * (end.x() - start.x())) / denominator);
        const qreal circleRadius = std::hypot(start.x() - circleCenter.x(),
                                              start.y() - circleCenter.y());
        if (circleRadius <= 1e-9) {
            return false;
        }

        constexpr qreal twoPi = 6.28318530717958647692;
        const auto normalizeAngle = [twoPi](qreal angle) {
            angle = std::fmod(angle, twoPi);
            if (angle < 0.0) {
                angle += twoPi;
            }
            return angle;
        };

        const qreal firstAngle = std::atan2(start.y() - circleCenter.y(),
                                            start.x() - circleCenter.x());
        const qreal secondAngle = std::atan2(end.y() - circleCenter.y(),
                                             end.x() - circleCenter.x());
        const qreal throughAngle = std::atan2(through.y() - circleCenter.y(),
                                              through.x() - circleCenter.x());
        const qreal counterClockwiseSweep = normalizeAngle(secondAngle - firstAngle);
        const qreal throughSweep = normalizeAngle(throughAngle - firstAngle);

        if (counterClockwiseSweep <= 1e-9) {
            return false;
        }

        const qreal selectedSweep = throughSweep <= counterClockwiseSweep + 1e-7
                                        ? counterClockwiseSweep
                                        : -(twoPi - counterClockwiseSweep);

        if (center != nullptr) {
            *center = circleCenter;
        }
        if (radius != nullptr) {
            *radius = circleRadius;
        }
        if (startAngle != nullptr) {
            *startAngle = firstAngle;
        }
        if (sweepAngle != nullptr) {
            *sweepAngle = selectedSweep;
        }
        return true;
    }

    void resetArcPreviewTracking()
    {
        arcPreviewInitialized_ = false;
        arcPreviewPreviousAngle_ = 0.0;
        arcPreviewSweepAngle_ = 0.0;
    }

    void initializeArcPreviewTracking()
    {
        resetArcPreviewTracking();
        if (arcMode_ != ArcMode::OnePoint || pendingPoints_.size() < 2) {
            return;
        }

        const QPointF center = worldToScreen(pendingPoints_[0]);
        const QPointF start = worldToScreen(pendingPoints_[1]);
        const qreal radius = std::hypot(start.x() - center.x(),
                                        start.y() - center.y());
        if (radius <= 1e-9) {
            return;
        }

        arcPreviewPreviousAngle_ = std::atan2(start.y() - center.y(),
                                              start.x() - center.x());
        arcPreviewInitialized_ = true;
    }

    void updateArcPreviewTracking(const QPointF &cursorWorld)
    {
        if (arcMode_ != ArcMode::OnePoint || pendingPoints_.size() < 2) {
            return;
        }

        const QPointF center = worldToScreen(pendingPoints_[0]);
        const QPointF cursor = worldToScreen(cursorWorld);
        const qreal radius = std::hypot(cursor.x() - center.x(),
                                        cursor.y() - center.y());
        if (radius <= 1e-9) {
            return;
        }

        const qreal angle = std::atan2(cursor.y() - center.y(),
                                       cursor.x() - center.x());
        if (!arcPreviewInitialized_) {
            arcPreviewPreviousAngle_ = angle;
            arcPreviewInitialized_ = true;
            return;
        }

        constexpr qreal pi = 3.14159265358979323846;
        constexpr qreal twoPi = 2.0 * pi;
        qreal delta = angle - arcPreviewPreviousAngle_;
        if (delta > pi) {
            delta -= twoPi;
        } else if (delta < -pi) {
            delta += twoPi;
        }

        arcPreviewSweepAngle_ += delta;
        arcPreviewPreviousAngle_ = angle;
    }

    void drawArcToolPreview(QPainter &painter)
    {
        viewportOverlay_.drawArcPreview(painter,
                                        pendingPoints_,
                                        arcMode_,
                                        cursorWorld_,
                                        cursorValid_,
                                        arcPreviewSweepAngle_,
                                        currentSnap_,
                                        size());
    }

    void drawCircleToolPreview(QPainter &painter)
    {
        viewportOverlay_.drawCirclePreview(painter,
                                           activeTool_,
                                           pendingPoints_,
                                           cursorWorld_,
                                           cursorValid_,
                                           currentSnap_,
                                           size());
    }

    void drawCircleTangentToolPreview(QPainter &painter)
    {
        if (controllerPreviewShapeVisible_) {
            drawShape(painter, controllerPreviewShape_, true);
        }
    }

    void drawEllipseToolPreview(QPainter &painter)
    {
        viewportOverlay_.drawEllipsePreview(painter,
                                             activeTool_,
                                             pendingPoints_,
                                             cursorWorld_,
                                             cursorValid_,
                                             currentSnap_,
                                             size());
    }

    void drawRectangleToolPreview(QPainter &painter)
    {
        viewportOverlay_.drawRectanglePreview(painter,
                                              activeTool_,
                                              pendingPoints_,
                                              cursorWorld_,
                                              cursorValid_,
                                              currentSnap_,
                                              size());
    }

    void drawPolygonToolPreview(QPainter &painter)
    {
        viewportOverlay_.drawPolygonPreview(painter,
                                            activeTool_,
                                            polygonSideCount_,
                                            pendingPoints_,
                                            cursorWorld_,
                                            cursorValid_,
                                            currentSnap_,
                                            size());
    }

    void drawControlPoints(QPainter &painter, const Shape &shape, int shapeIndex)
    {
        const WorkPlane previousPlane = viewportTransform_.workPlane();
        const qreal previousOffset = viewportTransform_.workPlaneOffset();
        viewportTransform_.setWorkPlane(shape.workPlane, shape.workPlaneOffset);
        viewportOverlay_.drawControlPoints(painter,
                                           shape,
                                           size(),
                                           shapes_.objectIdAt(shapeIndex),
                                           selectedShapeIndex_,
                                           draggingControlPoint_,
                                           controlPointIndex_);
        viewportTransform_.setWorkPlane(previousPlane, previousOffset);
    }

    void drawGrid(QPainter &painter)
    {
        viewportRenderer_.drawGrid(painter, size());
    }

    void drawOrigin(QPainter &painter)
    {
        viewportRenderer_.drawOrigin(painter, size());
    }

    QVector<double> expandedKnotVector(const Shape::NurbsCurve2D &curve) const
    {
        return expandedNurbsKnotVector(curve);
    }

    bool isValidNurbsCurve(const Shape::NurbsCurve2D &curve) const
    {
        return validateNurbsCurve(curve);
    }

    bool evaluateNurbsPoint(const Shape::NurbsCurve2D &curve,
                            qreal parameter,
                            QPointF *point) const
    {
        return classiCAD::evaluateNurbsPoint(curve, parameter, point);
    }

    void drawSubdivisionPoints(QPainter &painter,
                               const Shape &shape,
                               const QVector<double> &parameters,
                               bool preview)
    {
        const WorkPlane previousPlane = viewportTransform_.workPlane();
        const qreal previousOffset = viewportTransform_.workPlaneOffset();
        viewportTransform_.setWorkPlane(shape.workPlane, shape.workPlaneOffset);
        viewportOverlay_.drawSubdivisionPoints(painter,
                                               shape,
                                               parameters,
                                               size(),
                                               preview);
        viewportTransform_.setWorkPlane(previousPlane, previousOffset);
    }

    void drawPointToolPreview(QPainter &painter)
    {
        viewportOverlay_.drawPointPreview(painter,
                                          cursorWorld_,
                                          cursorValid_,
                                          currentSnap_,
                                          size());
    }

    void drawRotateToolPreview(QPainter &painter)
    {
        viewportOverlay_.drawRotatePreview(painter,
                                           cursorWorld_,
                                           cursorValid_,
                                           rotateStep_,
                                           rotateBaseWorld_,
                                           rotateReferenceWorld_,
                                           rotatePreviewAngle_,
                                           currentSnap_,
                                           size());
    }

    void drawScaleToolGuide(QPainter &painter)
    {
        if (!cursorValid_ || scaleStep_ != 2) {
            return;
        }

        QPointF guideEnd = cursorWorld_;
        if (scaleMode_ == ScaleMode::OneD && !scaleUsingTypedFactor_) {
            const QPointF offset = cursorWorld_ - scaleBaseWorld_;
            guideEnd = scaleBaseWorld_ +
                       scaleAxisDirection_ *
                           QPointF::dotProduct(offset, scaleAxisDirection_);
        }

        painter.save();
        painter.setPen(QPen(QColor(QStringLiteral("#8aa7c7")), 1.0, Qt::DashLine));
        painter.drawLine(worldToScreen(scaleBaseWorld_), worldToScreen(guideEnd));
        painter.setPen(QPen(QColor(QStringLiteral("#e6b85c")), 1.5));
        painter.setBrush(QColor(QStringLiteral("#282828")));
        painter.drawEllipse(worldToScreen(scaleBaseWorld_), 4.0, 4.0);
        painter.restore();
    }

    void drawEraseCandidatePreview(QPainter &painter, int shapeIndex)
    {
        viewportOverlay_.drawEraseCandidatePreview(painter,
                                                   eraseTargetCurveCaches_,
                                                   shapeIndex,
                                                   !eraseStrokeScreenPath_.isEmpty());
    }

    void drawErasePreview(QPainter &painter)
    {
        viewportOverlay_.drawErasePreview(painter,
                                          activeTool_,
                                          eraseCursorScreen_,
                                          cursorValid_,
                                          eraseCursorPressed_,
                                          eraseCandidateShapeIndices_.size());
    }

    void drawShape(QPainter &painter,
                   const Shape &shape,
                   bool preview,
                   bool selected = false,
                   bool drawPreviewPoints = true,
                   const QColor &layerColor = QColor(),
                   const QString &layerLineType = QString(),
                   qreal layerLineWeightMm = 0.0)
    {
        const WorkPlane previousPlane = viewportTransform_.workPlane();
        const qreal previousOffset = viewportTransform_.workPlaneOffset();
        viewportTransform_.setWorkPlane(shape.workPlane, shape.workPlaneOffset);
        viewportRenderer_.drawShape(painter,
                                    shape,
                                    size(),
                                    preview,
                                    selected,
                                    drawPreviewPoints,
                                    layerColor,
                                    layerLineType,
                                    layerLineWeightMm);
        viewportTransform_.setWorkPlane(previousPlane, previousOffset);
    }

    void emitCoordinateUpdate()
    {
        if (coordinateUpdate_) {
            coordinateUpdate_(coordinateText());
        }
    }

    void notifyViewStateChanged()
    {
        if (viewStateUpdate_) {
            viewStateUpdate_(viewportTransform_.workPlane(),
                             viewportTransform_.workPlaneOffset(),
                             viewportTransform_.viewPreset());
        }
    }

private:
    Shape pictureShapeForCorner(const QPointF &cursorCorner) const
    {
        Shape picture;
        picture.geometryType = GeometryType::Picture;
        picture.workPlane = viewportTransform_.workPlane();
        picture.workPlaneOffset = viewportTransform_.workPlaneOffset();
        if (pendingPictureImage_.isNull() || pendingPoints_.isEmpty()) {
            return picture;
        }
        const qreal aspectRatio = static_cast<qreal>(pendingPictureImage_.width()) /
                                  pendingPictureImage_.height();
        picture.points = makePictureFramePoints(pendingPoints_.first(),
                                                cursorCorner,
                                                aspectRatio);
        picture.pictureImage = pendingPictureImage_;
        picture.pictureImageData = pendingPictureImageData_;
        return picture;
    }

    void cancelPicturePlacement()
    {
        if (activeTool_ != Tool::Picture) {
            return;
        }
        pendingPoints_.clear();
        setTool(Tool::Select);
        if (commandFinished_) {
            commandFinished_(Tool::Select);
        }
        update();
    }

    ToolInput makeToolInput(const QMouseEvent *event,
                            const QPointF &screenPosition,
                            const QPointF &rawWorldPosition,
                            const QPointF &worldPosition) const
    {
        ToolInput input;
        input.screenPosition = screenPosition;
        input.rawWorldPosition = rawWorldPosition;
        input.worldPosition = worldPosition;
        input.viewportSize = size();
        input.snapType = currentSnap_.type;
        if (event != nullptr) {
            input.button = event->button();
            input.buttons = event->buttons();
            input.modifiers = event->modifiers();
        }
        return input;
    }

    ToolInput makeKeyToolInput(const QKeyEvent *event) const
    {
        ToolInput input;
        input.viewportSize = size();
        if (event != nullptr) {
            input.key = event->key();
            input.text = event->text();
            input.modifiers = event->modifiers();
        }
        return input;
    }

    bool makeToolShape(ToolId tool,
                       const QVector<QPointF> &points,
                       ArcMode arcMode,
                       qreal arcSweep,
                       Shape *shape) const
    {
        if (shape == nullptr || points.size() < requiredPoints(tool)) {
            return false;
        }

        Shape result{geometryTypeForTool(tool),
                     points,
                     Shape::NurbsCurve2D{},
                     arcMode,
                     arcSweep,
                     {},
                     {}};
        result.workPlane = viewportTransform_.workPlane();
        result.workPlaneOffset = viewportTransform_.workPlaneOffset();
        if (isRectangleTool(tool)) {
            result.points = makeRectanglePoints(rectangleModeForTool(tool), points);
            if (result.points.size() != 4) {
                return false;
            }
        } else if (isPolygonTool(tool)) {
            result.points = makeRegularPolygonPoints(
                polygonModeForTool(tool), points, polygonSideCount_);
            if (result.points.size() < 3) {
                return false;
            }
        } else if (isCircleConstructionTool(tool)) {
            if (tool == ToolId::CircleDiameter) {
                if (!makeCircleDefinitionFromDiameter(points[0],
                                                      points[1],
                                                      &result.points)) {
                    return false;
                }
            } else if (tool == ToolId::CircleThreePoint) {
                if (!makeCircleDefinitionFromThreePoints(points[0],
                                                         points[1],
                                                         points[2],
                                                         &result.points)) {
                    return false;
                }
            }
        }

        if (tool == Tool::Line) {
            result.nurbs = makeDegreeOneNurbs(result.points);
        } else if (tool == Tool::Arc) {
            result.nurbs = makeArcNurbsCurve(result);
        } else if (tool == Tool::Bezier || tool == Tool::Nurbs) {
            result.nurbs = makeBezierNurbs(result.points);
        } else if (isCircleConstructionTool(tool)) {
            result.nurbs = makeCircleNurbs(result.points);
            if (!isValidNurbsCurve(result.nurbs)) {
                return false;
            }
        } else if (isEllipseTool(tool)) {
            result.nurbs = makeEllipseNurbs(ellipseModeForTool(tool), points);
            if (!isValidNurbsCurve(result.nurbs)) {
                return false;
            }
            const EllipseMode mode = ellipseModeForTool(tool);
            const QPointF center = mode == EllipseMode::CenterAxisRadius
                                       ? points[0]
                                       : (points[0] + points[1]) * 0.5;
            result.points = {center};
        }

        *shape = result;
        return true;
    }

    static constexpr int maxSubdivisionSections = 10000;
    ToolId activeTool_ = ToolId::Select;
    ToolId repeatTool_ = ToolId::Select;
    ArcMode arcMode_ = ArcMode::OnePoint;
    bool arcPreviewInitialized_ = false;
    qreal arcPreviewPreviousAngle_ = 0.0;
    qreal arcPreviewSweepAngle_ = 0.0;
    bool controlPointsVisible_ = false;
    Document document_;
    SelectionModel selection_;
    History history_;
    ViewportTransform viewportTransform_;
    QVariantAnimation *navigationAnimation_ = nullptr;
    ViewportCameraState navigationAnimationStart_;
    ViewportCameraState navigationAnimationEnd_;
    QPointF navigationHoverPosition_{-1000.0, -1000.0};
    QPointF navigationPressPosition_;
    QPointF navigationLastPosition_;
    BlenderNavigationHit navigationPressHit_;
    BlenderNavigationAction navigationPressedAction_ = BlenderNavigationAction::None;
    bool navigationMoved_ = false;
    CurveSampler curveSampler_;
    CurveHitTester curveHitTester_;
    SnapEngine snapEngine_;
    ViewportRenderer viewportRenderer_;
    BlenderGridRenderer blenderGridRenderer_;
    ViewportGpuSurface *gpuSurface_ = nullptr;
    ViewportOverlay viewportOverlay_;
    BlenderGridAppearance gridAppearance_;
    ToolContext toolContext_;
    ToolRegistry toolRegistry_;
    InteractionTool *activeToolController_ = nullptr;
    ToolStatus toolStatus_;
    // Temporary source-compatibility view. Document owns the storage and
    // identity; the alias will disappear once viewport responsibilities are
    // extracted into tools and services.
    Document &shapes_;
    QVector<QPointF> pendingPoints_;
    QImage pendingPictureImage_;
    QByteArray pendingPictureImageData_;
    QString pendingPicturePath_;
    Shape controllerPreviewShape_;
    bool controllerPreviewShapeVisible_ = false;
    int polygonSideCount_ = 6;
    int polygonWheelRemainder_ = 0;
    QPointF &pan_;
    QPointF lastWorldPosition_{0.0, 0.0};
    QPointF rawCursorWorld_{0.0, 0.0};
    QPoint lastMousePosition_;
    QPointF cursorWorld_{0.0, 0.0};
    SnapResult currentSnap_;
    DragSnapResult currentDragSnap_;
    // Legacy member names remain during this incremental migration, but the
    // values are references into SelectionModel, and therefore stable object
    // IDs, never container indexes. These aliases keep the current viewport
    // implementation source-compatible while selection behavior migrates.
    QVector<ObjectId> &selectedShapeIndices_;
    ObjectId &selectedShapeIndex_;
    bool draggingSelected_ = false;
    QVector<ObjectId> draggingShapeIndices_;
    bool draggingControlPoint_ = false;
    bool dragGestureStarted_ = false;
    int &controlPointIndex_;
    bool dragHistoryRecorded_ = false;
    bool dragSnapLocked_ = false;
    quint64 dragSnapTraceSequence_ = 0;
    DragAxisLock dragAxisLock_ = DragAxisLock::None;
    bool grabActive_ = false;
    bool grabMoved_ = false;
    bool grabPickingBasePoint_ = false;
    bool grabHasBasePoint_ = false;
    Document::Snapshot grabStartSnapshot_;
    QPointF grabStartWorld_{0.0, 0.0};
    QPointF grabBasePoint_{0.0, 0.0};
    QPointF grabCursorOffset_{0.0, 0.0};
    QPointF dragSnapCursorWorld_{0.0, 0.0};
    QPointF nearDragFreeSourcePoint_{0.0, 0.0};
    bool nearDragFreeSourcePointValid_ = false;
    QPointF dragStartScreen_{0.0, 0.0};
    QPointF lastDragWorld_{0.0, 0.0};
    QPointF lastControlPointWorld_{0.0, 0.0};
    bool duplicateActive_ = false;
    bool duplicatePickingBasePoint_ = false;
    bool duplicateHasBasePoint_ = false;
    QVector<SceneObject> duplicateSourceObjects_;
    QVector<Shape> duplicatePreviewShapes_;
    QPointF duplicateBasePoint_{0.0, 0.0};
    QPointF duplicateCursorOffset_{0.0, 0.0};
    QPointF duplicateDestination_{0.0, 0.0};
    bool selectionBoxActive_ = false;
    bool selectionBoxMoved_ = false;
    bool selectionBoxAdditive_ = false;
    bool trimBoxSelectionActive_ = false;
    QPointF selectionBoxStartScreen_{0.0, 0.0};
    QPointF selectionBoxCurrentScreen_{0.0, 0.0};
    bool eraseStrokeActive_ = false;
    bool eraseCursorPressed_ = false;
    QPointF eraseCursorScreen_{0.0, 0.0};
    QPointF lastEraseScreen_{0.0, 0.0};
    QVector<QPointF> eraseStrokeScreenPath_;
    QVector<ObjectId> eraseCandidateShapeIndices_;
    QVector<ObjectId> eraseTargetShapeIndices_;
    QVector<EraseCurveSampleCache> eraseSceneCurveCaches_;
    QVector<EraseCurveSampleCache> eraseTargetCurveCaches_;
    bool eraseGeometryCachePrepared_ = false;
    bool trimHoverPositionValid_ = false;
    int trimHoverComponentIndex_ = -1;
    QPointF trimHoverScreenPosition_{0.0, 0.0};
    qreal &zoom_;
    bool panning_ = false;
    bool orbiting_ = false;
    bool panMoved_ = false;
    QPoint panStartPosition_;
    Qt::MouseButton panButton_ = Qt::MiddleButton;
    bool lineCommandActive_ = false;
    bool cursorValid_ = false;
    bool orthoEnabled_ = false;
    bool osnapEnabled_ = false;
    bool endpointSnapEnabled_ = true;
    bool midpointSnapEnabled_ = true;
    bool intersectionSnapEnabled_ = true;
    bool centerSnapEnabled_ = true;
    bool perpendicularSnapEnabled_ = false;
    bool tangentSnapEnabled_ = false;
    bool nearSnapEnabled_ = false;
    bool controlPointSnapEnabled_ = false;
    bool subdivisionActive_ = false;
    ObjectId subdivisionShapeIndex_ = ObjectId::invalid();
    int subdivisionSections_ = 2;
    int subdivisionWheelAccumulator_ = 0;
    qreal subdivisionPixelAccumulator_ = 0.0;
    bool joinActive_ = false;
    QVector<ObjectId> joinShapeIndices_;
    QVector<ObjectId> rotateShapeIndices_;
    QVector<ObjectId> scaleShapeIds_;
    QVector<ObjectId> mirrorShapeIndices_;
    int rotateStep_ = 0;
    QPointF rotateBaseWorld_{0.0, 0.0};
    QPointF rotateReferenceWorld_{0.0, 0.0};
    qreal rotatePreviewAngle_ = 0.0;
    ScaleMode scaleMode_ = ScaleMode::TwoD;
    int scaleStep_ = 0;
    QPointF scaleBaseWorld_{0.0, 0.0};
    QPointF scaleReferenceWorld_{0.0, 0.0};
    QPointF scaleAxisDirection_{0.0, 0.0};
    qreal scaleReferenceLength_ = 0.0;
    bool scaleUsingTypedFactor_ = false;
    qreal scaleTypedFactor_ = 1.0;
    QString scaleFactorText_;
    qreal scalePreviewFactor_ = 1.0;
    QPointF scalePreviewAxis_{0.0, 0.0};
    bool scalePreviewValid_ = false;
};

ViewportWidgetApi *createViewportWidget(QWidget *parent)
{
    return new ViewportWidget(parent);
}

} // namespace classiCAD
