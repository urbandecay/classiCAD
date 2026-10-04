#pragma once

#include "core/document/document.h"
#include "core/document/object_id.h"
#include "core/document/scene_object.h"
#include "core/history/document_transaction.h"

#include <QVector>

namespace classiCAD {

struct ExplodeCommandPlan {
    QVector<SceneObject> replacementObjects;
    QVector<int> selectedObjectIndices;
    int sourceObjectCount = 0;
    int outputComponentCount = 0;
};

bool buildExplodeCommandPlan(const Document &document,
                             const QVector<ObjectId> &selectedObjectIds,
                             ExplodeCommandPlan *plan);
bool applyExplodeCommand(DocumentTransaction &transaction,
                         const ExplodeCommandPlan &plan);

} // namespace classiCAD
