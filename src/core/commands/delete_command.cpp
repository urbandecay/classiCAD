#include "delete_command.h"

#include <QSet>

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
    QSet<quint64> seenIds;
    seenIds.reserve(objectIds.size());
    for (const ObjectId objectId : objectIds) {
        if (!objectId.isValid() || seenIds.contains(objectId.value()) ||
            !document.isObjectEditable(objectId)) {
            continue;
        }
        seenIds.insert(objectId.value());
        uniqueIds.append(objectId);
    }

    const QVector<ObjectId> removedIds = transaction.removeObjects(uniqueIds);
    if (removedIds.isEmpty()) {
        return false;
    }
    if (deletedCount != nullptr) {
        *deletedCount = removedIds.size();
    }
    return true;
}

} // namespace classiCAD
