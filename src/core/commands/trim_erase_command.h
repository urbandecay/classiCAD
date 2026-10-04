#pragma once

#include "core/document/document.h"
#include "core/history/document_transaction.h"

#include <QVector>

namespace classiCAD {

struct TrimEraseReplacement {
    ObjectId sourceObjectId = ObjectId::invalid();
    int sourceIndex = -1;
    QVector<Shape> pieces;
};

struct TrimEraseCommandResult {
    QVector<ObjectId> removedObjectIds;
    int removedCount = 0;
    int generatedPieceCount = 0;
};

// Applies already-computed Trim/Erase geometry replacements in descending
// document order so inserting extra pieces does not invalidate later indices.
class TrimEraseCommand final {
public:
    static bool apply(const Document &document,
                      DocumentTransaction &transaction,
                      QVector<TrimEraseReplacement> replacements,
                      TrimEraseCommandResult *result = nullptr);
};

} // namespace classiCAD
