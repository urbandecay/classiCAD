#include "subdivision_command.h"

namespace classiCAD {

bool SubdivisionCommand::apply(DocumentTransaction &transaction,
                               ObjectId objectId,
                               const QVector<double> &parameters)
{
    Shape *shape = transaction.editGeometry(objectId);
    if (shape == nullptr) {
        return false;
    }
    shape->subdivisionParameters = parameters;
    return true;
}

} // namespace classiCAD
