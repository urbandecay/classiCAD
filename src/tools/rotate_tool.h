#pragma once

#include "tool.h"
#include "core/document/object_id.h"

namespace classiCAD {

enum class RotatePointAction {
    Ignored,
    PivotCaptured,
    ReferenceCaptured,
    CommitRequested,
};

struct RotatePointResult {
    RotatePointAction action = RotatePointAction::Ignored;
    bool updateCursor = false;
    bool updateRawCursor = false;
    QPointF cursorPoint;
    WorkPlaneFrame frame;
    qreal angle = 0.0;
};

struct RotateFrameResult {
    bool handled = false;
    bool changed = false;
    bool restoreDrawingFrame = false;
    bool updateCursor = false;
    QPointF cursorPoint;
    SnapResult snapResult;
};

struct RotateKeyResult {
    bool handled = false;
    bool cancelled = false;
    bool commitRequested = false;
    bool updateCursor = false;
    bool updateRawCursor = false;
    QPointF cursorPoint;
    qreal commitAngle = 0.0;
    RotateFrameResult frame;
};

class RotateTool final : public InteractionTool {
public:
    struct InteractionState {
        QVector<ObjectId> sourceObjectIds;
        int stage = 0;
        WorkPlaneFrame frame;
        WorkPlaneFrame primaryFrame;
        WorkPlaneFrame prePivotPlaneFrame;
        WorkPlaneFrame prePivotFloorFrame;
        Point3D baseWorldPoint;
        Point3D referenceWorldPoint;
        Point3D referenceNormal;
        Point3D prePivotFloorNormal;
        QPointF basePoint{0.0, 0.0};
        QPointF referencePoint{0.0, 0.0};
        qreal previewAngle = 0.0;
        qreal accumulatedAngle = 0.0;
        qreal lastRawAngle = 0.0;
        qreal referenceAngle = 0.0;
        bool hasPreviousAngle = false;
        bool angleSnapEnabled = true;
        bool angleInputActive = false;
        bool angleInputManual = false;
        QString angleInput;
        qreal angleInputDirection = 1.0;
        bool angleInputDirectionCaptured = false;
        QPointF lastPointerPoint;
        bool perpendicularActive = false;
        bool prePivotPerpendicularActive = false;
        int axisLockKey = 0;
    };

    ToolId id() const override;
    void begin(ToolContext &context) override;
    EventResult dispatchMousePress(const ToolInput &input,
                                  ToolContext &context) override;
    EventResult dispatchKey(const ToolInput &input,
                            ToolContext &context) override;
    ToolStatus status() const override;
    void resetInteraction(bool angleSnapEnabled);
    void beginSelection(const QVector<ObjectId> &sourceObjectIds,
                        bool angleSnapEnabled);
    RotatePointResult acceptPoint(const ToolInput &input,
                                  qreal snapIncrementDegrees,
                                  qreal snapStrengthDegrees,
                                  ToolContext &context);
    bool commitAngle(qreal angle,
                     bool defaultAngleSnapEnabled,
                     ToolContext &context);
    RotateFrameResult handleAxisKey(int key,
                                    const ToolInput &input,
                                    ToolContext &context);
    RotateFrameResult togglePerpendicularPlane(const ToolInput &input,
                                               ToolContext &context);
    RotateKeyResult handleKeyInput(int key,
                                   const QString &text,
                                   Qt::KeyboardModifiers modifiers,
                                   bool autoRepeat,
                                   const ToolInput &input,
                                   qreal snapIncrementDegrees,
                                   qreal snapStrengthDegrees,
                                   ToolContext &context);
    RotateKeyResult takeLastKeyDispatchResult();
    void setAngleSnapParameters(qreal incrementDegrees,
                                qreal strengthDegrees);
    void setAngleSnapEnabled(bool enabled);
    bool toggleAngleSnap();
    qreal angleForPoint(const QPointF &worldPoint) const;
    bool updateReferencePreview(const QPointF &point,
                                const SnapResult &snap,
                                qreal snapIncrementDegrees,
                                qreal snapStrengthDegrees,
                                QPointF *cursorPoint);
    bool updatePreview(const QPointF &point,
                       const SnapResult &snap,
                       qreal snapIncrementDegrees,
                       qreal snapStrengthDegrees,
                       QPointF *cursorPoint);
    bool setTypedAngle(const QString &text, QPointF *cursorPoint);
    bool updateCursorForPreviewAngle(QPointF *cursorPoint) const;
    InteractionState &interactionState();
    const InteractionState &interactionState() const;

private:
    ToolStatus status_;
    InteractionState interactionState_;
    qreal snapIncrementDegrees_ = 15.0;
    qreal snapStrengthDegrees_ = 6.0;
    RotateKeyResult lastKeyDispatchResult_;
    bool hasLastKeyDispatchResult_ = false;
};

} // namespace classiCAD
