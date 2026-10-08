#pragma once

#include "core/document/document.h"
#include "core/document/object_id.h"

#include <QVector>

#include <functional>

namespace classiCAD {

// Owns the source snapshot and translated preview for interactive Duplicate.
// The viewport supplies the transform and shared snap query.
class DuplicateTool final {
public:
    bool begin(const Document &document,
               const QVector<ObjectId> &selectedObjectIds);
    void beginMove(const Point3D &basePointWorld);
    void beginInPlace();
    void updatePlacementWorld(
        const Point3D &worldDelta,
        const std::function<void(Shape &, Point3D &, const Point3D &)> &translate);
    void reset();

    bool isActive() const;
    bool isPickingBasePoint() const;
    bool hasBasePoint() const;
    const QVector<SceneObject> &sourceObjects() const;
    QVector<ObjectId> sourceObjectIds() const;
    const QVector<Shape> &previewShapes() const;
    const QVector<Point3D> &previewPlacementTranslations() const;
    Point3D basePointWorld() const;
    Point3D worldDelta() const;

private:
    bool active_ = false;
    bool pickingBasePoint_ = false;
    bool hasBasePoint_ = false;
    QVector<SceneObject> sourceObjects_;
    QVector<Shape> previewShapes_;
    QVector<Point3D> previewPlacementTranslations_;
    Point3D basePointWorld_;
    Point3D worldDelta_;
};

} // namespace classiCAD
