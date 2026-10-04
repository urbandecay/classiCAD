#pragma once

#include "shape_creation_tool.h"

#include <QPointF>
#include <QString>
#include <Qt>
#include <QVector>

namespace classiCAD {

struct DocumentSettings;

enum class ArcTextInputMode {
    None,
    Radius,
    Angle,
    ChordLength,
    Sagitta,
};

enum class ArcKeyAction {
    Unhandled,
    BeginTextInput,
    ChangeTextInput,
    ApplyTextInput,
    CancelArc,
    ToggleAngleSnap,
    TogglePlaneLock,
    TogglePerpendicularPlane,
    FinishArc,
};

struct ArcKeyResult {
    ArcKeyAction action = ArcKeyAction::Unhandled;
    bool handled = false;
};

struct ArcTextInputUpdate {
    ArcTextInputMode inputMode = ArcTextInputMode::None;
    bool hasLengthValue = false;
    qreal lengthValue = 0.0;
    bool updateCursor = false;
    bool updateRawCursor = false;
    QPointF cursorPoint;
    bool clearGeometrySnap = false;
};

struct ArcCommitResult {
    bool committed = false;
    ArcMode mode = ArcMode::OnePoint;
    qreal radius = 0.0;
    qreal sweep = 0.0;
    qreal chordLength = 0.0;
    qreal sagitta = 0.0;
};

enum class ArcInputStage {
    FirstPoint,
    SecondPoint,
    Complete,
};

struct ArcClickResult {
    bool handled = false;
    bool chordEndpointCaptured = false;
    bool cancelled = false;
    ArcCommitResult commit;
};

struct ArcEndpointConstraintResult {
    QPointF point;
    SnapResult snapResult;
    bool updateRawCursor = false;
    QPointF rawCursorPoint;
};

struct ArcChordLengthResult {
    bool applied = false;
    QPointF cursorPoint;
};

struct ArcAxisKeyResult {
    bool handled = false;
    bool refreshCursor = false;
    bool drawingFrameChanged = false;
    bool drawingPlaneLocked = false;
    WorkPlaneFrame drawingFrame;
};

struct ArcPlaneToggleResult {
    bool changed = false;
    bool refreshCursor = false;
    bool forceCursorRefresh = false;
    bool rebaseOnePointPreview = false;
};

class ArcTool final : public ShapeCreationTool {
public:
    struct RadiusInputUpdate {
        bool accepted = false;
        bool appendStartPoint = false;
        QPointF startPoint;
        bool updateCursor = false;
        QPointF cursorPoint;
    };

    struct InteractionState {
        ArcMode mode = ArcMode::OnePoint;
        bool previewInitialized = false;
        qreal previewPreviousAngle = 0.0;
        qreal previewSweepAngle = 0.0;
        bool angleValueLocked = false;
        qreal previewStartAngle = 0.0;
        bool angleSnapEnabled = true;
        bool perpendicularPlaneActive = false;
        bool planeLocked = false;
        WorkPlaneFrame lockedFrame;
        bool lockedFrameValid = false;
        ArcTextInputMode textInputMode = ArcTextInputMode::None;
        QString textInput;
        WorkPlaneFrame referenceFrame;
        WorkPlaneFrame inputFrame;
        WorkPlaneFrame axisBaseFrame;
        Point3D referenceNormal;
        Point3D firstPointWorld;
        Point3D secondPointWorld;
        Point3D resolvedChordPointWorld;
        bool referenceFrameValid = false;
        bool inputFrameValid = false;
        bool axisBaseFrameValid = false;
        bool chordWorldPointsValid = false;
        bool resolvedChordPointValid = false;
        int axisConstraintKey = 0;
        int planeNormalLockKey = 0;
        int verticalOverrideAxis = 0;
        Point3D twoPointPerpendicularNormal;
        bool twoPointPerpendicularNormalValid = false;
        bool wasVertical = false;
        QVector<QPointF> inputPoints;
    };

    ArcTool();

    InteractionState &interactionState();
    const InteractionState &interactionState() const;
    ArcMode mode() const;
    void setMode(ArcMode mode);
    const QVector<QPointF> &inputPoints() const;
    ArcInputStage inputStage() const;
    void setInputPoints(const QVector<QPointF> &points);
    void appendInputPoint(const QPointF &point);
    bool setInputPoint(int index, const QPointF &point);
    void clearInputPoints();
    void resetInputState();
    bool captureReferenceForFirstPoint(const QPointF &localPoint,
                                       const WorkPlaneFrame &frame);
    bool beginTextInput(ArcTextInputMode mode,
                        const QString &initialText,
                        bool hasPendingPoints);
    void clearTextInput();
    void appendTextInput(const QString &text);
    void backspaceTextInput();
    ArcTextInputMode textInputMode() const;
    const QString &textInput() const;
    ArcKeyResult handleKeyInput(int key,
                                const QString &text,
                                Qt::KeyboardModifiers modifiers,
                                bool autoRepeat);
    ArcTextInputUpdate applyTextInput(const DocumentSettings &settings,
                                     const QPointF &cursorPoint,
                                     bool geometrySnapActive);
    ArcCommitResult commitAt(const QPointF &cursorPoint,
                             bool geometrySnapActive,
                             ToolContext &context);
    ArcClickResult handleClick(const ToolInput &input,
                               bool worldPositionValid,
                               ToolContext &context);
    ArcEndpointConstraintResult constrainChordEndpoint(
        const ToolInput &input,
        ToolContext &context);
    ArcChordLengthResult applyChordLength(qreal chordLength,
                                          const ToolInput &input,
                                          ToolContext &context);
    bool restoreChordReferenceFrame(ToolContext &context);
    bool updateChordWorkPlane(
        ToolContext &context,
        const Point3D *previewEndpointWorld = nullptr);
    ArcAxisKeyResult handleAxisKey(int key,
                                   const ToolInput &input,
                                   ToolContext &context);
    ArcPlaneToggleResult togglePerpendicularPlane(
        const QPointF &cursorPoint,
        ToolContext &context);
    void rebaseOnePointPreview(const QPointF &cursorPoint);
    bool toggleAngleSnap();
    bool togglePlaneLock(const WorkPlaneFrame &currentFrame,
                         bool hasPendingPoints);
    RadiusInputUpdate applyRadiusInput(qreal radius,
                                      const QVector<QPointF> &points,
                                      const QPointF &cursorPoint,
                                      bool geometrySnapActive);
    bool applyAngleInput(qreal degrees,
                         const QVector<QPointF> &points,
                         QPointF *cursorPoint);
    bool applySagittaInput(qreal sagitta,
                           const QVector<QPointF> &points,
                           const QPointF &cursorPoint,
                           QPointF *updatedCursorPoint) const;
    void resetPreviewTracking();
    void initializePreviewTracking();
    void updatePreviewTracking(const QPointF &cursorPoint,
                               bool geometrySnapActive);
    QPointF constrainOnePointEndpoint(const QPointF &rawPoint) const;
    QPointF constrainTwoPointThroughPoint(const QPointF &cursorPoint,
                                          bool geometrySnap,
                                          bool altModifier) const;
    static qreal snapPreviewAngle(qreal angle);

protected:
    bool buildShape(const ToolContext &context, Shape *shape) const override;

private:
    bool resolveChordLengthEndpoint(qreal chordLength,
                                    const Point3D &cursorWorldPoint,
                                    bool axisConstraintActive,
                                    const Point3D &axisDirection,
                                    Point3D *endpointWorldPoint);
    InteractionState interactionState_;
};

} // namespace classiCAD
