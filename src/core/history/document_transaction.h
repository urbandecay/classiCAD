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
    bool setObjectAppearance(ObjectId objectId,
                             const QVector4D &displayColorSceneLinear,
                             const WorkbenchMaterialData &workbenchMaterial);
    bool addMaterial(const DocumentMaterial &material);
    CollectionId insertCollection(const DocumentCollection &collection);
    bool setCollectionVisibility(CollectionId collectionId,
                                 bool hideViewport,
                                 bool hideRender);
    bool setCollectionHideSelect(CollectionId collectionId, bool hideSelect);
    bool setObjectVisibilityFlags(ObjectId objectId,
                                  bool hideViewport,
                                  bool hideSelect);
    bool setObjectDisplayName(ObjectId objectId, const QString &name);
    bool renameCollection(CollectionId collectionId, const QString &name);
    bool setCollectionParents(CollectionId collectionId,
                              const QVector<CollectionId> &parentIds);
    bool setObjectCollections(ObjectId objectId,
                              const QVector<CollectionId> &collectionIds);
    bool removeCollection(CollectionId collectionId);
    bool translateObjects(const QVector<ObjectId> &objectIds,
                          const Point3D &worldDelta);
    bool removeObject(ObjectId objectId);
    QVector<ObjectId> removeObjects(const QVector<ObjectId> &objectIds);
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
