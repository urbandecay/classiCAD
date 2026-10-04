#include "delete_command.h"

namespace classiCAD {

bool DeleteCommand::apply(const Document &document,
                          DocumentTransaction &transaction,
                          const QVector<ObjectId> &objectIds,
                          int *deletedCount)
{
    if (deletedCount != nullptr) {
        *deletedCount = 0;
    }
    QVector<ObjectId> uniqueIds;
    uniqueIds.reserve(objectIds.size());
    for (const ObjectId objectId : objectIds) {
        if (!objectId.isValid() || uniqueIds.contains(objectId) ||
            !document.isObjectEditable(objectId)) {
            continue;
        }
        uniqueIds.append(objectId);
    }

    for (const ObjectId objectId : uniqueIds) {
        if (!transaction.removeObject(objectId)) {
            return false;
        }
    }
    if (deletedCount != nullptr) {
        *deletedCount = uniqueIds.size();
    }
    return !uniqueIds.isEmpty();
}

} // namespace classiCAD
