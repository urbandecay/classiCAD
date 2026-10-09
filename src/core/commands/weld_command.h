#pragma once

#include "core/document/document.h"
#include "core/document/object_id.h"
#include "core/document/scene_object.h"
#include "core/history/document_transaction.h"

#include <QString>
#include <QVector>

namespace classiCAD {

struct WeldCommandReplacement {
    ObjectId objectId = ObjectId::invalid();
    Shape geometry;
};

struct WeldCommandPlan {
    QVector<WeldCommandReplacement> replacements;
    int intersectionCount = 0;
    int splitCurveCount = 0;
    QString failureMessage;
};

// Splits selected planar NURBS curves at interior, transverse intersections,
// including crossings between spans of one multi-segment polyline. Each source
// object stays separate; resulting pieces share exact endpoint coordinates at
// each welded intersection. When focusedObjectIds is nonempty, only crossings
// involving one of those objects are planned.
bool buildWeldCommandPlan(const Document &document,
                          const QVector<ObjectId> &selectedObjectIds,
                          WeldCommandPlan *plan,
                          const QVector<ObjectId> &focusedObjectIds = {});
bool applyWeldCommand(DocumentTransaction &transaction,
                      const WeldCommandPlan &plan);

} // namespace classiCAD
