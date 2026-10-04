#pragma once

#include "core/document/document.h"
#include "core/history/document_transaction.h"

namespace classiCAD {

class MirrorCommand final {
public:
    static bool apply(const Document &document,
                      DocumentTransaction &transaction,
                      const QVector<ObjectId> &sourceObjectIds,
                      const QPointF &axisStart,
                      const QPointF &axisEnd,
                      QVector<ObjectId> *createdObjectIds = nullptr);
};

} // namespace classiCAD
