#pragma once

#include "core/document/document.h"
#include "core/document/object_id.h"

#include <QPointF>
#include <QVector>

namespace classiCAD {

// Owns the staged state and rollback snapshot for the viewport's Grab gesture.
// Snap queries and transient geometry mutation remain behind viewport services.
class GrabTool final {
public:
    bool begin(Document &document,
               const QVector<ObjectId> &selectedObjectIds,
               const QPointF &startWorldPosition);
    bool enterBasePointMode(Document &document);
    void restoreSourceGeometry(Document &document) const;
    void acceptBasePoint(const QPointF &basePoint,
                         const QPointF &cursorOffset);
    void setMoved(bool moved);
    void reset();

    bool isActive() const;
    bool moved() const;
    bool isPickingBasePoint() const;
    bool hasBasePoint() const;
    const QVector<ObjectId> &objectIds() const;
    const Document::Snapshot &startSnapshot() const;
    QPointF startWorldPosition() const;
    QPointF basePoint() const;
    QPointF cursorOffset() const;

private:
    bool active_ = false;
    bool moved_ = false;
    bool pickingBasePoint_ = false;
    bool hasBasePoint_ = false;
    QVector<ObjectId> objectIds_;
    QVector<Shape> sourceGeometry_;
    Document::Snapshot startSnapshot_;
    QPointF startWorldPosition_;
    QPointF basePoint_;
    QPointF cursorOffset_;
};

} // namespace classiCAD
