#pragma once

#include "core/document/document.h"
#include "core/history/document_transaction.h"

#include <QHash>
#include <QSet>

namespace classiCAD {

class MirrorCommand final {
public:
    static bool apply(const Document &document,
                      DocumentTransaction &transaction,
                      const QVector<ObjectId> &sourceObjectIds,
                      const QPointF &axisStart,
                      const QPointF &axisEnd,
                      QVector<ObjectId> *createdObjectIds = nullptr);

    // Edit Mode Mirror transforms only the selected control vertices. The
    // axis is defined in axisFrame, and placement translation is baked once.
    static bool applyControlPoints(
        Document &document,
        DocumentTransaction &transaction,
        const QVector<ObjectId> &sourceObjectIds,
        const QHash<quint64, QSet<int>> &controlPointIndices,
        const QPointF &axisStart,
        const QPointF &axisEnd,
        const WorkPlaneFrame &axisFrame,
        int *editedObjectCount = nullptr);

    // Blender-style Edit Mode mirror across a global axis plane through the
    // selection pivot. The selected control vertices are transformed in place.
    static bool applyControlPointsAcrossGlobalAxisPlane(
        Document &document,
        DocumentTransaction &transaction,
        const QVector<ObjectId> &sourceObjectIds,
        const QHash<quint64, QSet<int>> &controlPointIndices,
        const Point3D &pivot,
        int axis,
        int *editedObjectCount = nullptr);
};

} // namespace classiCAD
