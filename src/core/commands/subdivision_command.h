#pragma once

#include "core/document/object_id.h"
#include "core/history/document_transaction.h"

#include <QVector>

namespace classiCAD {

class SubdivisionCommand final {
public:
    static bool apply(DocumentTransaction &transaction,
                      ObjectId objectId,
                      const QVector<double> &parameters);
};

} // namespace classiCAD
