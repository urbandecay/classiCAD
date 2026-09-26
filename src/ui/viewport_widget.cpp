#include "viewport_widget_api.h"
#include "../core/document/document.h"
#include "../core/document/selection_model.h"
#include "../core/debug_log.h"
#include "../core/geometry/curve_evaluator.h"
#include "../core/geometry/geometry_transform.h"
#include "../core/history/history.h"
#include "../core/serialization/document_serializer.h"
#include "../services/hit_testing/curve_hit_tester.h"
#include "../services/sampling/curve_sampler.h"
#include "../services/snapping/snap_engine.h"
#include "../services/viewport/viewport_transform.h"
#include "../tools/tool_context.h"
#include "../tools/tool_input.h"
#include "../tools/tool_registry.h"
#include "input_helpers.h"
#include "viewport/viewport_overlay.h"
#include "viewport/viewport_renderer.h"

#include <QAction>
#include <QApplication>
#include <QCursor>
#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPixmap>
#include <QSettings>
#include <QTextStream>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <functional>

namespace classiCAD {

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
            shapes_.append(shape);
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
            update();
        });
        toolContext_.setStatusPublisher([this](const ToolStatus &status) {
            toolStatus_ = status;
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
        setCursor(Qt::CrossCursor);
        DebugLog::instance().write(QStringLiteral("viewport constructed"));
    }

    void setTool(ToolId tool)
    {
        DebugLog::instance().write(QStringLiteral("setTool requested=%1 previous=%2")
                                       .arg(toolName(tool), toolName(activeTool_)));
        if (grabActive_ && tool != Tool::Select) {
            cancelGrab();
        }
        const Tool previousTool = activeTool_;
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
        if (tool != Tool::Mirror) {
            resetMirrorInteraction();
        }
        activeTool_ = tool;
        activeToolController_ = toolRegistry_.find(tool);
        pendingPoints_.clear();
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
        }

        if (tool != Tool::Select) {
            repeatTool_ = tool;
            if (!isEraseLikeTool(tool) && tool != Tool::Rotate && tool != Tool::Mirror) {
                selectedShapeIndices_.clear();
                selectedShapeIndex_ = ObjectId::invalid();
            }
            selectionBoxActive_ = false;
            selectionBoxMoved_ = false;
            selectionBoxAdditive_ = false;
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

        DebugLog::instance().write(QStringLiteral("setTool applied=%1 lineCommandActive=%2 cursorValid=%3 cursorWorld=%4")
                                       .arg(toolName(activeTool_))
                                       .arg(lineCommandActive_)
                                       .arg(cursorValid_)
                                       .arg(pointText(cursorWorld_)));
        update();
    }

    ViewportCommandResult executeCommand(ViewportCommand command,
                                         int argument = 0) override
    {
        switch (command) {
        case ViewportCommand::Undo: {
            const bool accepted = history_.canUndo();
            undo();
            return {accepted, 0};
        }
        case ViewportCommand::Redo: {
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
        case ViewportCommand::BeginMirror:
            return {beginMirror(), 0};
        }

        return {};
    }

    QVector<ViewportLayerInfo> layerInfos() const override
    {
        QVector<ViewportLayerInfo> infos;
        infos.reserve(document_.layers().size());
        for (const Layer &layer : document_.layers()) {
            infos.append(ViewportLayerInfo{layer.id,
                                           layer.name,
                                           layer.visible,
                                           layer.locked,
                                           layer.id == document_.activeLayerId(),
                                           static_cast<int>(layer.objectIds.size())});
        }
        return infos;
    }

    ViewportLayerCommandResult executeLayerCommand(
        const ViewportLayerCommandRequest &request) override
    {
        const auto hasEditableAlternative = [this](LayerId excludedLayer) {
            for (const Layer &layer : document_.layers()) {
                if (layer.id != excludedLayer && layer.visible && !layer.locked) {
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
                      bool near)
    {
        endpointSnapEnabled_ = endpoint;
        midpointSnapEnabled_ = midpoint;
        intersectionSnapEnabled_ = intersection;
        centerSnapEnabled_ = center;
        perpendicularSnapEnabled_ = perpendicular;
        tangentSnapEnabled_ = tangent;
        nearSnapEnabled_ = near;
        updateSnapEngineSettings();
        refreshCursorConstraint();
        DebugLog::instance().write(QStringLiteral("setSnapModes endpoint=%1 midpoint=%2 intersection=%3 center=%4 perpendicular=%5 tangent=%6 near=%7 snap=%8")
                                       .arg(endpointSnapEnabled_)
                                       .arg(midpointSnapEnabled_)
                                       .arg(intersectionSnapEnabled_)
                                       .arg(centerSnapEnabled_)
                                       .arg(perpendicularSnapEnabled_)
                                       .arg(tangentSnapEnabled_)
                                       .arg(nearSnapEnabled_)
                                       .arg(snapTypeName(currentSnap_.type)));
        update();
    }

    QString coordinateText() const
    {
        return QStringLiteral("X %1   Y %2   Zoom %3%")
            .arg(lastWorldPosition_.x(), 0, 'f', 2)
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
        // Version 3 adds the document/layer record while retaining the
        // legacy shape array as a compatibility aid for older tooling.
        root.insert(QStringLiteral("version"), 3);
        root.insert(QStringLiteral("zoom"), zoom_);
        root.insert(QStringLiteral("pan"), pointToJson(pan_));
        root.insert(QStringLiteral("document"), documentToJson(document_));

        QJsonArray shapes;
        for (const Shape &shape : shapes_) {
            shapes.append(shapeToJson(shape));
        }
        root.insert(QStringLiteral("shapes"), shapes);

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

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.fillRect(rect(), QColor(QStringLiteral("#282828")));

        drawGrid(painter);
        drawOrigin(painter);

        for (int index = 0; index < shapes_.size(); ++index) {
            const ObjectId objectId = shapes_.objectIdAt(index);
            if (!document_.isObjectVisible(objectId)) {
                continue;
            }
            const bool selected = selectedShapeIndices_.contains(objectId) ||
                                  objectId == selectedShapeIndex_ ||
                                  joinShapeIndices_.contains(objectId);
            const bool rotatePreview = activeTool_ == Tool::Rotate &&
                                        rotateStep_ == 2 &&
                                        rotateShapeIndices_.contains(objectId);
            if (rotatePreview) {
                Shape previewShape = shapes_[index];
                rotateShapeGeometry(&previewShape,
                                    rotateBaseWorld_,
                                    rotatePreviewAngle_);
                drawShape(painter, previewShape, false, true);
                if (!subdivisionActive_ || objectId != subdivisionShapeIndex_) {
                    drawSubdivisionPoints(painter,
                                          previewShape,
                                          previewShape.subdivisionParameters,
                                          false);
                }
            } else {
                drawShape(painter, shapes_[index], false, selected);
                if (!subdivisionActive_ || objectId != subdivisionShapeIndex_) {
                    drawSubdivisionPoints(painter,
                                          shapes_[index],
                                          shapes_[index].subdivisionParameters,
                                          false);
                }
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
                    if (activeTool_ == Tool::Rotate && rotateStep_ == 2 &&
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

        if (activeTool_ == Tool::TangentFromCurve) {
            drawLineToolPreview(painter);
            if (!pendingPoints_.isEmpty()) {
                drawSnapMarker(painter, SnapType::Tangent, pendingPoints_.first());
            }
        } else if (activeTool_ == Tool::Line && lineCommandActive_) {
            drawLineToolPreview(painter);
        } else if (activeTool_ == Tool::Mirror) {
            drawMirrorToolPreview(painter);
            drawLineToolPreview(painter);
        } else if (activeTool_ == Tool::Arc) {
            drawArcToolPreview(painter);
        } else if (activeTool_ == Tool::Circle && !pendingPoints_.isEmpty()) {
            drawCircleToolPreview(painter);
        } else if (activeTool_ == Tool::Rectangle && !pendingPoints_.isEmpty()) {
            drawRectangleToolPreview(painter);
        } else if (activeTool_ == Tool::Point) {
            drawPointToolPreview(painter);
        } else if (activeTool_ == Tool::Rotate) {
            drawRotateToolPreview(painter);
        } else if (isEraseLikeTool(activeTool_)) {
            drawErasePreview(painter);
        } else if (!pendingPoints_.isEmpty()) {
            drawShape(painter,
                      Shape{geometryTypeForTool(activeTool_),
                            pendingPoints_,
                            Shape::NurbsCurve2D{},
                            ArcMode::TwoPoint,
                            0.0,
                            {},
                            {}},
                      true);
        }

        if (grabActive_ && currentSnap_.isValid()) {
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
                                         grabHasBasePoint_);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        const QPointF screenPosition = eventPosition(event);
        const QPointF rawWorldPosition = screenToWorld(screenPosition);
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

        if (event->button() == Qt::RightButton && activeTool_ == Tool::Mirror) {
            cancelMirror();
            return;
        }

        if (event->button() == panButton_ ||
            (event->button() == Qt::LeftButton && event->modifiers().testFlag(Qt::AltModifier))) {
            panning_ = true;
            panMoved_ = false;
            panStartPosition_ = screenPosition.toPoint();
            lastMousePosition_ = screenPosition.toPoint();
            DebugLog::instance().write(QStringLiteral("mousePress branch=start-pan at=%1")
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
            eraseCursorPressed_ = true;
            rawCursorWorld_ = rawWorldPosition;
            cursorWorld_ = rawWorldPosition;
            lastWorldPosition_ = rawWorldPosition;
            cursorValid_ = true;
            eraseCursorScreen_ = screenPosition;
            trimAtScreenPosition(screenPosition);
            setCursor(Qt::CrossCursor);
            emitCoordinateUpdate();
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
            if (activeTool_ == Tool::Rectangle && completedShape.points.size() >= 2) {
                const QPointF first = completedShape.points[0];
                const QPointF second = completedShape.points[1];
                completedShape.points = {first,
                                         QPointF(second.x(), first.y()),
                                         second,
                                         QPointF(first.x(), second.y())};
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
        eraseCursorScreen_ = screenPosition;
        rawCursorWorld_ = screenToWorld(screenPosition);
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
        const bool circlePreviewActive = activeTool_ == Tool::Circle && !pendingPoints_.isEmpty();
        const bool rectanglePreviewActive =
            activeTool_ == Tool::Rectangle && !pendingPoints_.isEmpty();
        const bool arcPreviewActive = activeTool_ == Tool::Arc;
        const bool mirrorPreviewActive = activeTool_ == Tool::Mirror;

        if (!panning_ && activeTool_ == Tool::Arc && arcMode_ == ArcMode::OnePoint &&
            pendingPoints_.size() >= 2) {
            updateArcPreviewTracking(cursorWorld_);
        }

        if (panning_) {
            const QPoint current = screenPosition.toPoint();
            const QPoint delta = current - lastMousePosition_;
            const QPoint totalPanDelta = current - panStartPosition_;
            if (std::hypot(totalPanDelta.x(), totalPanDelta.y()) >= 3.0) {
                panMoved_ = true;
            }
            pan_ += QPointF(delta.x() / zoom_, -delta.y() / zoom_);
            lastMousePosition_ = current;
        }

        if (selectionBoxActive_) {
            selectionBoxCurrentScreen_ = screenPosition;
            const QPointF totalDelta = screenPosition - selectionBoxStartScreen_;
            if (std::hypot(totalDelta.x(), totalDelta.y()) >= 3.0) {
                selectionBoxMoved_ = true;
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
            const QPointF delta = rawCursorWorld_ - lastControlPointWorld_;
            if (!qFuzzyIsNull(delta.x()) || !qFuzzyIsNull(delta.y())) {
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
                        currentDragSnap_ = findControlPointSnap(
                            selectedShapeIndex_,
                            controlPointIndex_,
                            controlPoints[controlPointIndex_]);
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
                DebugLog::instance().write(
                    QStringLiteral("control point drag shape=%1 index=%2 delta=%3 world=%4")
                        .arg(selectedIndex)
                        .arg(controlPointIndex_)
                        .arg(pointText(delta))
                        .arg(pointText(rawCursorWorld_)));
            }
        } else if (draggingSelected_ && dragGestureStarted_ && selectedIndex >= 0) {
            const QVector<ObjectId> dragIndices = draggingShapeIndices_.isEmpty()
                                                     ? QVector<ObjectId>{selectedShapeIndex_}
                                                     : draggingShapeIndices_;
            if (grabActive_) {
                updateGrabPosition(dragIndices);
            } else {
                const bool groupDrag = dragIndices.size() > 1;
                const QPointF rawDelta = rawCursorWorld_ - lastDragWorld_;
                const QPointF delta = constrainDragDelta(rawDelta);
                if (!qFuzzyIsNull(rawDelta.x()) || !qFuzzyIsNull(rawDelta.y())) {
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
                        constexpr qreal dragSnapBreakawayPixels = 18.0;
                        const qreal cursorDistanceFromSnap =
                            std::hypot(screenPosition.x() - worldToScreen(dragSnapCursorWorld_).x(),
                                       screenPosition.y() - worldToScreen(dragSnapCursorWorld_).y());

                        if (dragSnapLocked_ && cursorDistanceFromSnap <= dragSnapBreakawayPixels) {
                            // Keep the geometry attached while the cursor is still near the
                            // snap point. This prevents a one-pixel mouse move from
                            // repeatedly attaching and detaching the line.
                            DebugLog::instance().write(
                                QStringLiteral("selection drag snap-hold shape=%1 cursorDistance=%2 breakaway=%3")
                                    .arg(selectedIndex)
                                    .arg(cursorDistanceFromSnap, 0, 'f', 2)
                                    .arg(dragSnapBreakawayPixels, 0, 'f', 2));
                        } else {
                            if (dragSnapLocked_) {
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
                                currentDragSnap_ = groupDrag
                                                        ? findDragSnap(dragIndices)
                                                        : findDragSnap(selectedShapeIndex_);
                                if (currentDragSnap_.isValid()) {
                                    translateShapes(dragIndices, currentDragSnap_.translation);
                                    dragSnapLocked_ = true;
                                    dragSnapCursorWorld_ = rawCursorWorld_;
                                }
                            }
                        }
                    }

                    lastDragWorld_ = rawCursorWorld_;

                    DebugLog::instance().write(
                        QStringLiteral("selection drag shape=%1 delta=%2 cursorWorld=%3 snap=%4 snapSource=%5 snapTarget=%6 snapTranslation=%7 axisLock=%8")
                            .arg(selectedIndex)
                            .arg(pointText(delta))
                            .arg(pointText(rawCursorWorld_))
                            .arg(snapTypeName(currentDragSnap_.type))
                            .arg(pointText(currentDragSnap_.sourcePoint))
                            .arg(pointText(currentDragSnap_.targetPoint))
                            .arg(pointText(currentDragSnap_.translation))
                            .arg(dragAxisLockName(dragAxisLock_)));
                    if (groupDrag) {
                        DebugLog::instance().write(
                            QStringLiteral("group selection drag count=%1")
                                .arg(dragIndices.size()));
                    }
                }
            }
        }

        if (pointPreviewActive || lineCommandActive_ || arcPreviewActive || circlePreviewActive ||
            rectanglePreviewActive || mirrorPreviewActive || panning_ || draggingSelected_ ||
            draggingControlPoint_) {
            update();
        }

        if (pointPreviewActive || lineCommandActive_ || arcPreviewActive || circlePreviewActive ||
            rectanglePreviewActive || mirrorPreviewActive || activeTool_ == Tool::Erase || panning_ ||
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

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        const bool repeatToolOnRelease =
            panning_ && event->button() == panButton_ && !panMoved_ &&
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
        if (panning_ && (event->button() == panButton_ || event->button() == Qt::LeftButton)) {
            panning_ = false;
            setCursor(activeTool_ == Tool::Select ? Qt::ArrowCursor : Qt::CrossCursor);
            DebugLog::instance().write(QStringLiteral("mouseRelease branch=end-pan"));
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
            finishSelectionBox();
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

        const QPointF screenPosition = eventPosition(event);
        const QPointF beforeZoom = screenToWorld(screenPosition);
        const qreal oldZoom = zoom_;
        const qreal factor = event->angleDelta().y() > 0 ? 1.12 : 1.0 / 1.12;
        viewportTransform_.zoomAt(screenPosition, factor, size(), 0.15, 12.0);

        const QPointF afterZoom = screenToWorld(screenPosition);

        DebugLog::instance().write(QStringLiteral("wheel screen=%1 deltaY=%2 zoom=%3->%4 worldBefore=%5 worldAfter=%6 pan=%7")
                                       .arg(pointText(screenPosition))
                                       .arg(event->angleDelta().y())
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

        if (activeTool_ == Tool::Mirror && event->key() == Qt::Key_Escape) {
            cancelMirror();
            return;
        }

        if (grabActive_ && event->key() == Qt::Key_Escape) {
            cancelGrab();
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

private:
    void updateSnapEngineSettings()
    {
        snapEngine_.setSettings(SnapSettings{osnapEnabled_,
                                             endpointSnapEnabled_,
                                             midpointSnapEnabled_,
                                             intersectionSnapEnabled_,
                                             centerSnapEnabled_,
                                             perpendicularSnapEnabled_,
                                             tangentSnapEnabled_,
                                             nearSnapEnabled_});
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
               shape.geometryType == GeometryType::Nurbs;
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
        setCursor(joinActive_ ? Qt::CrossCursor : Qt::ArrowCursor);
        update();
        emitCoordinateUpdate();
    }

    void cancelSelectionBox()
    {
        if (!selectionBoxActive_) {
            return;
        }

        selectionBoxActive_ = false;
        selectionBoxMoved_ = false;
        selectionBoxAdditive_ = false;
        setCursor(joinActive_ ? Qt::CrossCursor : Qt::ArrowCursor);
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
                shape.geometryType == GeometryType::Circle) &&
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

    void resetInteractionAfterHistory()
    {
        pendingPoints_.clear();
        selectedShapeIndices_.clear();
        selectedShapeIndex_ = ObjectId::invalid();
        selectionBoxActive_ = false;
        selectionBoxMoved_ = false;
        selectionBoxAdditive_ = false;
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
            shapes_.append(Shape{GeometryType::Line,
                                 pendingPoints_,
                                 curve,
                                 ArcMode::TwoPoint,
                                 0.0,
                                 {},
                                 {}});
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
            activeTool_ == Tool::Arc || activeTool_ == Tool::Circle ||
            activeTool_ == Tool::Point || activeTool_ == Tool::Rotate ||
            activeTool_ == Tool::Mirror ||
            activeTool_ == Tool::TangentFromCurve;
        return snapEngine_.findSnapPoint(document_,
                                         rawPoint,
                                         serviceDrawingSnapActive,
                                         pendingPoints_,
                                         viewportTransform_,
                                         size());

        SnapResult best;
        const bool drawingSnapActive =
            (activeTool_ == Tool::Line && lineCommandActive_) ||
            activeTool_ == Tool::Arc || activeTool_ == Tool::Circle ||
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

    SnapResult findGrabDestinationSnap(const QPointF &rawPoint) const
    {
        return snapEngine_.findSnapPoint(document_,
                                         rawPoint,
                                         true,
                                         QVector<QPointF>{grabBasePoint_},
                                         viewportTransform_,
                                         size(),
                                         grabSelectedShapeIndices(),
                                         true);
    }

    DragSnapResult findDragSnap(ObjectId selectedObjectId) const
    {
        return findDragSnap(QVector<ObjectId>{selectedObjectId});
    }

    DragSnapResult findDragSnap(const QVector<ObjectId> &selectedObjectIds) const
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
                                        size());

        DragSnapResult best;
        if (!osnapEnabled_ || selectedObjectIds.isEmpty()) {
            return best;
        }

        QVector<SnapCandidate> sourceCandidates;
        QVector<int> selectedShapeIndices;
        for (const ObjectId objectId : selectedObjectIds) {
            const int shapeIndex = objectIndex(objectId);
            if (shapeIndex < 0) {
                continue;
            }
            selectedShapeIndices.append(shapeIndex);
            sourceCandidates += snapCandidatesForShape(shapes_[shapeIndex]);
        }
        if (sourceCandidates.isEmpty()) {
            return best;
        }

        const QVector<SnapCandidate> targetCandidates =
            snapCandidatesForScene(selectedShapeIndices);
        constexpr qreal snapRadiusPixels = 12.0;
        qreal bestDistance = snapRadiusPixels;

        for (const SnapCandidate &source : sourceCandidates) {
            const QPointF sourceScreen = worldToScreen(source.point);
            for (const SnapCandidate &target : targetCandidates) {
                const QPointF targetScreen = worldToScreen(target.point);
                const qreal distance = std::hypot(targetScreen.x() - sourceScreen.x(),
                                                   targetScreen.y() - sourceScreen.y());
                if (distance <= bestDistance) {
                    bestDistance = distance;
                    best.type = source.type;
                    best.sourcePoint = source.point;
                    best.targetPoint = target.point;
                    best.translation = target.point - source.point;
                }
            }
        }

        return best;
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
        if (currentSnap_.isValid()) {
            return currentSnap_.point;
        }

        const bool drawingConstraintActive =
            (activeTool_ == Tool::Line && lineCommandActive_) ||
            activeTool_ == Tool::Arc || activeTool_ == Tool::Mirror ||
            activeTool_ == Tool::TangentFromCurve;
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

        const QPointF origin = pendingPoints_.back();
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

        if (shape.geometryType == GeometryType::Rectangle && shape.points.size() >= 2) {
            const QVector<QPointF> vertices = rectangleVertices(shape);
            if (vertices.size() < 4) {
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

        const auto collectIntersections =
            [&](const SampledNurbsCurve2D &otherSamples) {
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
                if (useBounds && !boundsOverlap(sourceSamples->bounds,
                                                otherSamples.bounds.adjusted(-1, -1, 1, 1))) {
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
                        parameters.append(bestParameter);
                    }
                }

                for (int sourceSegment = 1;
                     sourceSegment < sourceSamples->screenPoints.size();
                     ++sourceSegment) {
                    const QPointF &sourceStart =
                        sourceSamples->screenPoints[sourceSegment - 1];
                    const QPointF &sourceEnd =
                        sourceSamples->screenPoints[sourceSegment];
                    for (int otherSegment = 1;
                         otherSegment < otherSamples.screenPoints.size();
                         ++otherSegment) {
                        if (useBounds &&
                            !boundsOverlap(
                                sourceSamples->segmentBounds[sourceSegment - 1],
                                otherSamples.segmentBounds[otherSegment - 1])) {
                            continue;
                        }

                        QPointF intersection;
                        if (!segmentIntersection(
                                sourceStart,
                                sourceEnd,
                                otherSamples.screenPoints[otherSegment - 1],
                                otherSamples.screenPoints[otherSegment],
                                &intersection)) {
                            continue;
                        }

                        const qreal localParameter = parameterAtIntersection(
                            sourceStart,
                            sourceEnd,
                            intersection);
                        parameters.append(
                            sourceSamples->parameters[sourceSegment - 1] +
                            (sourceSamples->parameters[sourceSegment] -
                             sourceSamples->parameters[sourceSegment - 1]) *
                                localParameter);
                    }
                }
            };

        if (sceneCache != nullptr) {
            for (const EraseCurveSampleCache &other : *sceneCache) {
                if (other.shapeIndex == sourceShapeIndex &&
                    other.componentIndex == sourceComponentIndex) {
                    continue;
                }
                collectIntersections(other.sampled);
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
                        collectIntersections(otherSamples);
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
                                     int shapeIndex) const
    {
        constexpr qreal eraserRadiusPixels = 10.0;
        qreal distance = 1.0e9;
        for (const EraseCurveSampleCache &targetCurve : eraseTargetCurveCaches_) {
            if (targetCurve.shapeIndex != shapeIndex) {
                continue;
            }

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

                distance = std::min(
                    distance,
                    distanceToSegment(screenPosition,
                                      targetCurve.sampled.screenPoints[sample - 1],
                                      targetCurve.sampled.screenPoints[sample]));
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
        for (const ObjectId objectId : eraseTargetShapeIndices_) {
            const int shapeIndex = objectIndex(objectId);
            if (shapeIndex < 0) {
                continue;
            }
            const qreal distance = distanceToCachedEraseShape(screenPosition,
                                                              shapeIndex);
            const bool closer = distance < closestDistance - 1.0e-6;
            const bool tieOnActiveSelection =
                std::abs(distance - closestDistance) <= 1.0e-6 &&
                objectId == selectedShapeIndex_;
            if (closer || tieOnActiveSelection) {
                closestDistance = distance;
                closestShapeIndex = shapeIndex;
            }
        }

        if (closestShapeIndex >= 0) {
            eraseCandidateShapeIndices_.append(shapes_.objectIdAt(closestShapeIndex));
        }
        updateErasePreviewIntervals(true);
    }

    void trimAtScreenPosition(const QPointF &screenPosition)
    {
        updateTrimHover(screenPosition);
        if (eraseCandidateShapeIndices_.isEmpty()) {
            DebugLog::instance().write(
                QStringLiteral("trim click ignored no selected curve under cursor"));
            return;
        }

        applyEraseCandidates();
        eraseStrokeActive_ = false;
        eraseCandidateShapeIndices_.clear();
        eraseStrokeScreenPath_.clear();
        eraseTargetShapeIndices_.clear();
        eraseSceneCurveCaches_.clear();
        eraseTargetCurveCaches_.clear();
        eraseGeometryCachePrepared_ = false;
        trimHoverPositionValid_ = false;
        update();
        DebugLog::instance().write(QStringLiteral("trim click applied"));
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

    void updateErasePreviewIntervals(bool reset)
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
                                 const QVector<EraseCurveSampleCache> *cachedTargets = nullptr) const
    {
        if (replacement == nullptr) {
            return false;
        }
        replacement->clear();

        if (shape.geometryType == GeometryType::Point || shape.geometryType == GeometryType::Rectangle) {
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
            const QVector<qreal> *cachedIntersectionParameters = nullptr;
            if (cachedTargets != nullptr) {
                for (const EraseCurveSampleCache &cachedTarget : *cachedTargets) {
                    if (cachedTarget.shapeIndex == sourceShapeIndex &&
                        cachedTarget.componentIndex == sourceComponentIndex) {
                        cachedIntersectionParameters =
                            &cachedTarget.intersectionParameters;
                        break;
                    }
                }
            }
            const QVector<ParameterInterval> removedIntervals =
                eraseIntervalsBoundedByIntersections(sourceShapeIndex,
                                                     sourceComponentIndex,
                                                     sourceCurve,
                                                     stroke,
                                                     cachedIntersectionParameters);
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

        if (shape.geometryType == GeometryType::Line && remainingCurves.size() == 1) {
            Shape updated = shape;
            updated.nurbs = remainingCurves.first();
            updated.points = updated.nurbs.controlPoints;
            updated.subdivisionParameters.clear();
            replacement->append(updated);
            return true;
        }

        Shape updated{GeometryType::PolyCurve,
                      polyCurvePoints(remainingCurves),
                      Shape::NurbsCurve2D{},
                      ArcMode::TwoPoint,
                      0.0,
                      {},
                      remainingCurves};
        replacement->append(updated);
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

    void applyEraseCandidates()
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
                                        &eraseTargetCurveCaches_)) {
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
                shapes_.replace(objectId, change.second.first());
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

            shape.nurbs.controlPoints[controlPointIndex] += delta;

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
        dragSnapLocked_ = false;
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
                                           pendingPoints_,
                                           cursorWorld_,
                                           cursorValid_,
                                           currentSnap_,
                                           size());
    }

    void drawRectangleToolPreview(QPainter &painter)
    {
        viewportOverlay_.drawRectanglePreview(painter,
                                              pendingPoints_,
                                              cursorWorld_,
                                              cursorValid_,
                                              currentSnap_,
                                              size());
    }

    void drawControlPoints(QPainter &painter, const Shape &shape, int shapeIndex)
    {
        viewportOverlay_.drawControlPoints(painter,
                                           shape,
                                           size(),
                                           shapes_.objectIdAt(shapeIndex),
                                           selectedShapeIndex_,
                                           draggingControlPoint_,
                                           controlPointIndex_);
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
        viewportOverlay_.drawSubdivisionPoints(painter,
                                               shape,
                                               parameters,
                                               size(),
                                               preview);
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
                   bool drawPreviewPoints = true)
    {
        viewportRenderer_.drawShape(painter,
                                    shape,
                                    size(),
                                    preview,
                                    selected,
                                    drawPreviewPoints);
    }

    void emitCoordinateUpdate()
    {
        if (coordinateUpdate_) {
            coordinateUpdate_(coordinateText());
        }
    }

private:
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
        if (tool == Tool::Rectangle && result.points.size() >= 2) {
            const QPointF first = result.points[0];
            const QPointF second = result.points[1];
            result.points = {first,
                             QPointF(second.x(), first.y()),
                             second,
                             QPointF(first.x(), second.y())};
        }

        if (tool == Tool::Line) {
            result.nurbs = makeDegreeOneNurbs(result.points);
        } else if (tool == Tool::Arc) {
            result.nurbs = makeArcNurbsCurve(result);
        } else if (tool == Tool::Bezier || tool == Tool::Nurbs) {
            result.nurbs = makeBezierNurbs(result.points);
        } else if (tool == Tool::Circle) {
            result.nurbs = makeCircleNurbs(result.points);
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
    CurveSampler curveSampler_;
    CurveHitTester curveHitTester_;
    SnapEngine snapEngine_;
    ViewportRenderer viewportRenderer_;
    ViewportOverlay viewportOverlay_;
    ToolContext toolContext_;
    ToolRegistry toolRegistry_;
    InteractionTool *activeToolController_ = nullptr;
    ToolStatus toolStatus_;
    // Temporary source-compatibility view. Document owns the storage and
    // identity; the alias will disappear once viewport responsibilities are
    // extracted into tools and services.
    Document &shapes_;
    QVector<QPointF> pendingPoints_;
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
    QPointF dragStartScreen_{0.0, 0.0};
    QPointF lastDragWorld_{0.0, 0.0};
    QPointF lastControlPointWorld_{0.0, 0.0};
    bool selectionBoxActive_ = false;
    bool selectionBoxMoved_ = false;
    bool selectionBoxAdditive_ = false;
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
    QPointF trimHoverScreenPosition_{0.0, 0.0};
    qreal &zoom_;
    bool panning_ = false;
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
    bool subdivisionActive_ = false;
    ObjectId subdivisionShapeIndex_ = ObjectId::invalid();
    int subdivisionSections_ = 2;
    int subdivisionWheelAccumulator_ = 0;
    qreal subdivisionPixelAccumulator_ = 0.0;
    bool joinActive_ = false;
    QVector<ObjectId> joinShapeIndices_;
    QVector<ObjectId> rotateShapeIndices_;
    QVector<ObjectId> mirrorShapeIndices_;
    int rotateStep_ = 0;
    QPointF rotateBaseWorld_{0.0, 0.0};
    QPointF rotateReferenceWorld_{0.0, 0.0};
    qreal rotatePreviewAngle_ = 0.0;
};

ViewportWidgetApi *createViewportWidget(QWidget *parent)
{
    return new ViewportWidget(parent);
}

} // namespace classiCAD
