#include "transform_command.h"

namespace classiCAD {

bool TransformCommand::apply(Document &document,
                             DocumentTransaction &transaction,
                             const QVector<ObjectId> &objectIds,
                             const GeometryEdit &edit,
                             int *editedCount)
{
    if (editedCount != nullptr) {
        *editedCount = 0;
    }
    if (!edit) {
        return false;
    }

    int count = 0;
    for (const ObjectId objectId : objectIds) {
        if (!document.isObjectEditable(objectId)) {
            continue;
        }
        Shape *shape = transaction.editGeometry(objectId);
        if (shape == nullptr) {
            continue;
        }
        edit(*shape);
        ++count;
    }
    if (editedCount != nullptr) {
        *editedCount = count;
    }
    return count > 0;
}

} // namespace classiCAD
