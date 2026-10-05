#pragma once

#include "core/document/document.h"
#include "core/history/document_transaction.h"

namespace classiCAD {

struct DuplicateCommandPlan {
    QVector<ObjectId> sourceObjectIds;
    QVector<SceneObject> duplicateObjects;
};

bool buildDuplicateCommandPlan(const Document &document,
                              const QVector<ObjectId> &sourceObjectIds,
                              const QVector<Shape> &duplicateGeometry,
                              DuplicateCommandPlan *plan,
                              QVector<Point3D> duplicatePlacementTranslations = {});
bool applyDuplicateCommand(const Document &document,
                           DocumentTransaction &transaction,
                           const DuplicateCommandPlan &plan,
                           QVector<ObjectId> *duplicateObjectIds);

} // namespace classiCAD
