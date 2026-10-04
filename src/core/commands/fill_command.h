#pragma once

#include "core/document/document.h"
#include "core/history/document_transaction.h"

#include <QVector>

namespace classiCAD {

struct FillCommandCandidate {
    ObjectId objectId = ObjectId::invalid();
    Shape::NurbsSurface3D surface;
};

struct FillCommandPlan {
    QVector<FillCommandCandidate> candidates;
};

bool buildFillCommandPlan(const Document &document,
                          const QVector<ObjectId> &selectedObjectIds,
                          FillCommandPlan *plan);
bool applyFillCommand(DocumentTransaction &transaction,
                      const FillCommandPlan &plan,
                      int *filledCount = nullptr);

} // namespace classiCAD
