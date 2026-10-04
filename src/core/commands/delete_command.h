#pragma once

#include "core/document/document.h"
#include "core/document/object_id.h"
#include "core/history/document_transaction.h"

#include <QVector>

namespace classiCAD {

class DeleteCommand final {
public:
    // IDs must be live objects from the transaction's document. The caller
    // owns rollback and history commit for the containing edit operation.
    static bool apply(const Document &document,
                      DocumentTransaction &transaction,
                      const QVector<ObjectId> &objectIds,
                      int *deletedCount = nullptr);
};

} // namespace classiCAD
