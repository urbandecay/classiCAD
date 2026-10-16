#pragma once

#include "tool.h"
#include "core/document/object_id.h"

#include <QHash>
#include <QSet>

namespace classiCAD {

class ToolContext;

struct MirrorCommitResult {
    bool committed = false;
    bool componentEdit = false;
    bool globalAxisMirror = false;
    int sourceCount = 0;
    int controlPointCount = 0;
    int globalAxis = -1;
    Point3D pivot;
    QVector<ObjectId> createdObjectIds;
    QPointF axisStart;
    QPointF axisEnd;
};

class MirrorTool final : public InteractionTool {
public:
    struct InteractionState {
        QVector<ObjectId> sourceObjectIds;
        QHash<quint64, QSet<int>> controlPointIndices;
        WorkPlaneFrame axisFrame;
        QPointF axisStart{0.0, 0.0};
        QPointF axisEnd{0.0, 0.0};
        bool hasAxisStart = false;
        bool hasCompleteAxis = false;
        bool globalAxisMirror = false;
        int globalAxis = -1;
        Point3D pivot;
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
    void setComponentControlPointTargets(
        const QHash<quint64, QSet<int>> &controlPointIndices);
    void beginGlobalAxisComponentMirror(const Point3D &pivot,
                                        ToolContext &context);
    void clearInteraction();
    void rejectAxisEndpoint();
    bool hasAxisStart() const;
    bool hasCompleteAxis() const;
    const QPointF &axisStart() const;
    const QPointF &axisEnd() const;
    const WorkPlaneFrame &axisFrame() const;
    bool isComponentEdit() const;
    bool isGlobalAxisMirror() const;
    int globalAxis() const;
    const Point3D &mirrorPivot() const;
    const QHash<quint64, QSet<int>> &controlPointIndices() const;
    const QVector<ObjectId> &sourceObjectIds() const;

private:
    MirrorCommitResult commitGlobalAxis(ToolContext &context);

    ToolStatus status_;
    InteractionState interactionState_;
    MirrorCommitResult lastCommitResult_;
    bool hasLastCommitResult_ = false;
};

} // namespace classiCAD
