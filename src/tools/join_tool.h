#pragma once

#include "core/document/object_id.h"

#include <QString>
#include <QVector>

namespace classiCAD {

class ToolContext;

enum class JoinClickAction {
    Ignored,
    InvalidTarget,
    AlreadySelected,
    Added,
    ReadyToJoin,
};

enum class JoinKeyAction {
    Unhandled,
    Complete,
    Cancel,
};

enum class JoinExecutionFailure {
    None,
    Inactive,
    NeedTwoCurves,
    CurvesUnavailable,
    InvalidCurveSelection,
    DisconnectedCurves,
    CommandFailed,
};

struct JoinExecutionResult {
    JoinExecutionFailure failure = JoinExecutionFailure::None;
    bool committed = false;
    int sourceObjectCount = 0;
    int componentCount = 0;
    int lineMergeCount = 0;
    ObjectId joinedObjectId = ObjectId::invalid();
    qreal nearestEndpointGap = -1.0;
};

class JoinTool final {
public:
    void begin(const QVector<ObjectId> &preselectedObjectIds);
    void cancel();
    void complete();
    JoinClickAction handleObjectClick(ObjectId objectId, bool joinable);
    JoinKeyAction handleKey(int key) const;
    JoinExecutionResult executeJoin(qreal endpointTolerance,
                                    ToolContext &context);
    bool addObject(ObjectId objectId);
    bool contains(ObjectId objectId) const;
    bool isActive() const;
    int selectedCount() const;
    QString prompt() const;
    const QVector<ObjectId> &selectedObjectIds() const;

private:
    bool active_ = false;
    QVector<ObjectId> selectedObjectIds_;
};

} // namespace classiCAD
