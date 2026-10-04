#pragma once

#include "tool.h"

#include "core/document/object_id.h"

#include <QPointF>

#include <functional>

namespace classiCAD {

class EraseTool final : public InteractionTool {
public:
    using ScreenDistanceQuery = std::function<qreal(const QPointF &, ObjectId)>;

    ToolId id() const override;
    void begin(ToolContext &context) override;
    void cancel(ToolContext &context) override;
    ToolStatus status() const override;

    void resetInteraction();
    void beginStroke(const QPointF &screenPosition);
    void finishStroke();
    void cancelStroke();
    bool strokeActive() const;
    bool cursorPressed() const;
    void setCursorPressed(bool pressed);
    QPointF cursorScreenPosition() const;
    void setCursorScreenPosition(const QPointF &screenPosition);
    QPointF lastScreenPosition() const;
    void setLastScreenPosition(const QPointF &screenPosition);
    void appendStrokeScreenPosition(const QPointF &screenPosition);
    int collectCandidatesAlongSegment(
        const QPointF &start,
        const QPointF &end,
        const QVector<ObjectId> &targetObjectIds,
        const ScreenDistanceQuery &distanceQuery,
        qreal hitRadiusPixels = 10.0,
        qreal sampleSpacingPixels = 5.0);

    QVector<QPointF> &screenPath();
    const QVector<QPointF> &screenPath() const;
    void clearCandidates();
    bool addCandidate(ObjectId objectId);
    QVector<ObjectId> &candidateObjectIds();
    const QVector<ObjectId> &candidateObjectIds() const;

private:
    ToolStatus status_;
    bool strokeActive_ = false;
    bool cursorPressed_ = false;
    QPointF cursorScreenPosition_;
    QPointF lastScreenPosition_;
    QVector<QPointF> screenPath_;
    QVector<ObjectId> candidateObjectIds_;
};

} // namespace classiCAD
