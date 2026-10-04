#pragma once

#include "core/document/document.h"
#include "core/document/object_id.h"

#include <QPointF>
#include <QVector>

#include <functional>

namespace classiCAD {

struct SnapResult;

// Owns the source snapshot, base-point stage, and translated preview set for
// interactive Duplicate. The viewport supplies its shared snap query.
class DuplicateTool final {
public:
    bool begin(const Document &document,
               const QVector<ObjectId> &selectedObjectIds);
    void beginInPlace();
    void chooseBasePoint(const QPointF &rawPosition,
                         const QPointF &resolvedBasePoint);
    void updatePlacement(
        const QPointF &destinationCursor,
        const SnapResult &destinationSnap,
        const std::function<void(Shape &, const QPointF &)> &translate);
    void reset();

    bool isActive() const;
    bool isPickingBasePoint() const;
    bool hasBasePoint() const;
    const QVector<SceneObject> &sourceObjects() const;
    QVector<ObjectId> sourceObjectIds() const;
    const QVector<Shape> &previewShapes() const;
    QPointF basePoint() const;
    QPointF cursorOffset() const;
    QPointF destination() const;

private:
    bool active_ = false;
    bool pickingBasePoint_ = false;
    bool hasBasePoint_ = false;
    QVector<SceneObject> sourceObjects_;
    QVector<Shape> previewShapes_;
    QPointF basePoint_;
    QPointF cursorOffset_;
    QPointF destination_;
};

} // namespace classiCAD
