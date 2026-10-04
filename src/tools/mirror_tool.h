#pragma once

#include "tool.h"
#include "core/document/object_id.h"

namespace classiCAD {

class ToolContext;

struct MirrorCommitResult {
    bool committed = false;
    int sourceCount = 0;
    QVector<ObjectId> createdObjectIds;
    QPointF axisStart;
    QPointF axisEnd;
};

class MirrorTool final : public InteractionTool {
public:
    struct InteractionState {
        QVector<ObjectId> sourceObjectIds;
        QPointF axisStart{0.0, 0.0};
        QPointF axisEnd{0.0, 0.0};
        bool hasAxisStart = false;
        bool hasCompleteAxis = false;
    };

    ToolId id() const override;
    void begin(ToolContext &context) override;
    bool handleMousePress(const ToolInput &input, ToolContext &context) override;
    EventResult dispatchMousePress(const ToolInput &input,
                                   ToolContext &context) override;
    EventResult dispatchKey(const ToolInput &input,
                            ToolContext &context) override;
    MirrorCommitResult commitAxis(ToolContext &context);
    MirrorCommitResult takeLastCommitResult();
    void cancel(ToolContext &context) override;
    ToolStatus status() const override;
    void setSourceObjectIds(const QVector<ObjectId> &objectIds);
    void clearInteraction();
    void rejectAxisEndpoint();
    bool hasAxisStart() const;
    bool hasCompleteAxis() const;
    const QPointF &axisStart() const;
    const QPointF &axisEnd() const;
    const QVector<ObjectId> &sourceObjectIds() const;

private:
    ToolStatus status_;
    InteractionState interactionState_;
    MirrorCommitResult lastCommitResult_;
    bool hasLastCommitResult_ = false;
};

} // namespace classiCAD
