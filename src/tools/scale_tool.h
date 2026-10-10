#pragma once

#include "tool.h"
#include "core/document/object_id.h"

#include <QHash>
#include <QSet>
#include <QString>

namespace classiCAD {

enum class ScalePointAction {
    Ignored,
    BasePointCaptured,
    ReferenceCaptured,
    CommitRequested,
};

struct ScalePointResult {
    ScalePointAction action = ScalePointAction::Ignored;
    QPointF point;
    qreal factor = 1.0;
    QPointF axisDirection;
};

struct ScaleDispatchResult {
    ScalePointResult point;
    bool cancelled = false;
    ScaleMode mode = ScaleMode::TwoD;
    QPointF basePoint;
    int sourceCount = 0;
    bool commitAttempted = false;
    bool committed = false;
};

struct ScaleKeyDispatchResult {
    bool handled = false;
    bool cancelled = false;
    bool promptChanged = false;
    bool redrawRequested = false;
    ScalePointResult point;
    ScaleMode mode = ScaleMode::TwoD;
    qreal factor = 1.0;
    QPointF basePoint;
    int sourceCount = 0;
    bool commitAttempted = false;
    bool committed = false;
};

class ScaleTool final : public InteractionTool {
public:
    struct InteractionState {
        QVector<ObjectId> sourceObjectIds;
        QHash<quint64, QSet<int>> controlPointIndices;
        ScaleMode mode = ScaleMode::TwoD;
        int stage = 0;
        QPointF basePoint{0.0, 0.0};
        QPointF referencePoint{0.0, 0.0};
        QPointF axisDirection{0.0, 0.0};
        qreal referenceLength = 0.0;
        bool usingTypedFactor = false;
        qreal typedFactor = 1.0;
        QString factorText;
        qreal previewFactor = 1.0;
        QPointF previewAxis{0.0, 0.0};
        bool previewValid = false;
    };

    ToolId id() const override;
    void begin(ToolContext &context) override;
    EventResult dispatchMousePress(const ToolInput &input,
                                  ToolContext &context) override;
    EventResult dispatchKey(const ToolInput &input,
                            ToolContext &context) override;
    ToolStatus status() const override;
    QString prompt() const;
    void beginSelection(
        const QVector<ObjectId> &sourceObjectIds,
        ScaleMode mode,
        const QHash<quint64, QSet<int>> &controlPointIndices = {});
    void resetInteraction();
    ScalePointResult acceptPoint(const QPointF &point);
    bool commitScale(qreal factor,
                     const QPointF &axisDirection,
                     ToolContext &context);
    bool updatePreview(const QPointF &point);
    bool appendFactorCharacter(QChar character);
    void backspaceFactorInput();
    bool acceptFactorInput(qreal *factor);
    ScaleDispatchResult takeLastDispatchResult();
    ScaleKeyDispatchResult takeLastKeyDispatchResult();
    const InteractionState &interactionState() const;

private:
    ToolStatus status_;
    InteractionState interactionState_;
    ScaleDispatchResult lastDispatchResult_;
    bool hasLastDispatchResult_ = false;
    ScaleKeyDispatchResult lastKeyDispatchResult_;
    bool hasLastKeyDispatchResult_ = false;
};

} // namespace classiCAD
