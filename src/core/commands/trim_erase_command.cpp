#include "trim_erase_command.h"

#include <algorithm>
#include <utility>

namespace classiCAD {

bool TrimEraseCommand::apply(const Document &document,
                             DocumentTransaction &transaction,
                             QVector<TrimEraseReplacement> replacements,
                             TrimEraseCommandResult *result)
{
    if (result != nullptr) {
        *result = TrimEraseCommandResult{};
    }
    if (replacements.isEmpty()) {
        return false;
    }

    std::stable_sort(replacements.begin(),
                     replacements.end(),
                     [](const TrimEraseReplacement &first,
                        const TrimEraseReplacement &second) {
                         return first.sourceIndex > second.sourceIndex;
                     });

    TrimEraseCommandResult applied;
    for (const TrimEraseReplacement &replacement : replacements) {
        if (!replacement.sourceObjectId.isValid() ||
            replacement.sourceIndex < 0 ||
            replacement.sourceIndex >= document.size() ||
            document.objectIdAt(replacement.sourceIndex) !=
                replacement.sourceObjectId) {
            transaction.rollback();
            return false;
        }

        const SceneObject *sourceObject =
            document.object(replacement.sourceObjectId);
        if (sourceObject == nullptr) {
            transaction.rollback();
            return false;
        }
        const SceneObject sourceCopy = *sourceObject;
        applied.generatedPieceCount += replacement.pieces.size();
        if (replacement.pieces.isEmpty()) {
            if (!transaction.removeObject(replacement.sourceObjectId)) {
                transaction.rollback();
                return false;
            }
            applied.removedObjectIds.append(replacement.sourceObjectId);
            ++applied.removedCount;
            continue;
        }

        if (!transaction.replaceGeometry(replacement.sourceObjectId,
                                         replacement.pieces.first())) {
            transaction.rollback();
            return false;
        }
        for (int pieceIndex = 1;
             pieceIndex < replacement.pieces.size();
             ++pieceIndex) {
            SceneObject piece = sourceCopy;
            piece.id = ObjectId::invalid();
            piece.geometry = replacement.pieces[pieceIndex];
            if (!transaction.insertObject(replacement.sourceIndex + pieceIndex,
                                          piece).isValid()) {
                transaction.rollback();
                return false;
            }
        }
    }

    if (result != nullptr) {
        *result = std::move(applied);
    }
    return true;
}

} // namespace classiCAD
