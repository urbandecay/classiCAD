#pragma once

#include "tool.h"

#include "core/document/object_id.h"

#include <QPointF>

#include <functional>

namespace classiCAD {

class TrimTool final : public InteractionTool {
public:
    struct HoverTarget {
        ObjectId objectId = ObjectId::invalid();
        int componentIndex = -1;
        qreal distance = 0.0;

        bool isValid() const { return objectId.isValid(); }
    };

    using CurveDistanceQuery = std::function<qreal(ObjectId, int *)>;
    using BoxTargetPredicate = std::function<bool(ObjectId)>;

    ToolId id() const override;
    void begin(ToolContext &context) override;
    void cancel(ToolContext &context) override;
    ToolStatus status() const override;

    void resetInteraction();

    void beginBoxSelection(const QPointF &screenPosition);
    void updateBoxSelection(const QPointF &screenPosition);
    void clearBoxSelection();
    bool boxSelectionActive() const;
    bool boxSelectionMoved() const;
    QPointF boxStartPosition() const;
    QPointF boxCurrentPosition() const;

    void invalidateHover();
    bool hoverPositionValid() const;
    bool hoverPositionMatches(const QPointF &screenPosition) const;
    void setHoverPosition(const QPointF &screenPosition, int componentIndex);
    int hoverComponentIndex() const;
    HoverTarget updateHover(const QPointF &screenPosition,
                            const QVector<ObjectId> &targetObjectIds,
                            ObjectId primarySelection,
                            qreal hitRadius,
                            const CurveDistanceQuery &distanceQuery);
    ObjectId primaryCandidate() const;
    int selectBoxCandidates(const QVector<ObjectId> &targetObjectIds,
                            const BoxTargetPredicate &matchesBox);

    QVector<ObjectId> &candidateObjectIds();
    const QVector<ObjectId> &candidateObjectIds() const;
    void clearCandidates();
    bool addCandidate(ObjectId objectId);

    QVector<QPointF> &screenPath();
    const QVector<QPointF> &screenPath() const;
    void clearScreenPath();
    void setSinglePointScreenPath(const QPointF &screenPosition);

private:
    ToolStatus status_;
    bool boxSelectionActive_ = false;
    bool boxSelectionMoved_ = false;
    QPointF boxStartPosition_;
    QPointF boxCurrentPosition_;
    bool hoverPositionValid_ = false;
    int hoverComponentIndex_ = -1;
    QPointF hoverScreenPosition_;
    QVector<ObjectId> candidateObjectIds_;
    QVector<QPointF> screenPath_;
};

} // namespace classiCAD
