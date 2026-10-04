#pragma once

#include "core/document/document.h"
#include "core/history/document_transaction.h"

#include <functional>

namespace classiCAD {

class TransformCommand final {
public:
    using GeometryEdit = std::function<void(Shape &)>;

    // Applies an already-previewed transform only to live, editable IDs.
    // Geometry details stay in core/geometry; this command owns edit routing.
    static bool apply(Document &document,
                      DocumentTransaction &transaction,
                      const QVector<ObjectId> &objectIds,
                      const GeometryEdit &edit,
                      int *editedCount = nullptr);
};

} // namespace classiCAD
