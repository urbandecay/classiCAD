#pragma once

#include "core/document/object_id.h"
#include "core/document/scene_object.h"
#include "core/history/document_transaction.h"

namespace classiCAD {

struct JoinCommandPlan {
    QVector<ObjectId> sourceObjectIds;
    SceneObject joinedObject;
    int insertionIndex = -1;
};

bool buildJoinCommandPlan(
    const QVector<Shape::NurbsCurve2D> &components,
    const QVector<WorkPlaneFrame> &componentFrames,
    bool mixedPlanes,
    const WorkPlaneFrame &joinFrame,
    WorkPlane joinWorkPlane,
    qreal joinWorkPlaneOffset,
    LayerId joinedLayerId,
    const QVector<ObjectId> &sourceObjectIds,
    int insertionIndex,
    JoinCommandPlan *plan);
bool applyJoinCommand(const Document &document,
                      DocumentTransaction &transaction,
                      const JoinCommandPlan &plan,
                      ObjectId *joinedObjectId);

} // namespace classiCAD
