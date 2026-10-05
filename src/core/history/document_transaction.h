#pragma once

#include "core/document/document.h"
#include "history.h"

#include <functional>

namespace classiCAD {

// A single reversible document operation. The transaction records the initial
// snapshot, reports affected IDs/categories, and restores that snapshot if a
// caller exits without committing.
class DocumentTransaction final {
public:
    DocumentTransaction(Document &document, History &history);
    ~DocumentTransaction();

    DocumentTransaction(const DocumentTransaction &) = delete;
    DocumentTransaction &operator=(const DocumentTransaction &) = delete;

    Shape *editGeometry(ObjectId objectId);
    ObjectId addShape(const Shape &shape);
    ObjectId insertObject(int index, const SceneObject &object);
    QVector<ObjectId> insertObjects(int index,
                                    const QVector<SceneObject> &objects);
    bool replaceGeometry(ObjectId objectId, const Shape &shape);
    bool setObjectPlacementTranslation(ObjectId objectId,
                                       const Point3D &translation);
    bool translateObjects(const QVector<ObjectId> &objectIds,
                          const Point3D &worldDelta);
    bool removeObject(ObjectId objectId);
    QVector<ObjectId> removeObjects(const QVector<ObjectId> &objectIds);
    bool moveObjectToLayer(ObjectId objectId, LayerId layerId);
    bool editLayer(LayerId layerId,
                   const std::function<void(Layer &)> &edit,
                   bool visibilityChanged = false);
    bool setSettings(const DocumentSettings &settings);
    void replaceDocument(const Document::Snapshot &snapshot);
    void replaceObjects(const QVector<SceneObject> &objects);
    void markAllGeometryChanged();
    void markSelectionChanged();

    bool commit();
    void rollback();
    const DocumentChangeSet &changes() const;

private:
    Document &document_;
    History &history_;
    Document::Snapshot before_;
    DocumentChangeSet changes_;
    bool finished_ = false;
};

} // namespace classiCAD
