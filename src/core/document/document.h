#pragma once

#include "document_settings.h"
#include "document_change_set.h"
#include "document_collection.h"
#include "scene_object.h"

#include <QHash>
#include <QStringList>
#include <QVector>

#include <functional>

namespace classiCAD {

// Document owns persistent scene objects. Blender collections and view-layer
// state are modeled independently from geometry storage.
class Document final {
public:
    struct RuntimeRevisions {
        quint64 epoch = 1;
        quint64 geometry = 1;
        quint64 placement = 1;
        quint64 structure = 1;
        quint64 collection = 1;
        quint64 visibility = 1;
        quint64 settings = 1;
    };

    struct Snapshot {
        QVector<DocumentCollection> collections;
        QVector<SceneObject> objects;
        QVector<DocumentMaterial> materials;
        quint64 nextObjectValue = 1;
        quint64 nextCollectionValue = 1;
        DocumentSettings settings;
    };

    Document();

    int size() const;
    bool isEmpty() const;
    const QVector<SceneObject> &objects() const;
    const QVector<DocumentMaterial> &materials() const;
    const QVector<DocumentCollection> &collections() const;
    const DocumentSettings &settings() const;
    bool setSettings(const DocumentSettings &settings);
    // Outliner expansion belongs to saved workspace state, not geometry undo
    // snapshots. It is serialized with the project outside document history.
    const QHash<QString, QStringList> &outlinerExpandedPathsByScene() const;
    bool setOutlinerExpandedPathsByScene(
        const QHash<QString, QStringList> &expandedPathsByScene);

    const SceneObject *object(ObjectId id) const;
    const Shape *shape(ObjectId id) const;
    ObjectId objectIdAt(int index) const;
    int indexOf(ObjectId id) const;
    quint64 objectGeometryRevision(ObjectId id) const;
    const RuntimeRevisions &runtimeRevisions() const;
    void invalidateAllGeometry();
    void applyChanges(const DocumentChangeSet &changes);
    void replaceWith(const Document &document);
    const Shape &operator[](int index) const;
    // The callback returns true only when it changes geometry. Document then
    // advances the document and per-object geometry revisions automatically.
    bool mutateGeometry(ObjectId id,
                        const std::function<bool(Shape &)> &edit);
    bool setObjectPlacementTranslation(ObjectId id,
                                       const Point3D &translation);
    bool setObjectPlacementTranslations(const QVector<ObjectId> &ids,
                                       const QVector<Point3D> &translations);
    bool translateObjects(const QVector<ObjectId> &ids,
                          const Point3D &worldDelta);

    ObjectId append(const Shape &shape);
    ObjectId insert(int index, const Shape &shape);
    ObjectId insertObject(int index, SceneObject object);
    QVector<ObjectId> insertObjects(int index,
                                    const QVector<SceneObject> &objects);
    bool replace(ObjectId id, const Shape &shape);
    bool remove(ObjectId id);
    bool removeAt(int index);
    QVector<ObjectId> removeObjects(const QVector<ObjectId> &objectIds);

    // Used only while restoring legacy update sessions and by the current
    // explode implementation. New objects get IDs; existing snapshots use
    // restoreSnapshot() so IDs are preserved.
    QVector<ObjectId> replaceShapes(const QVector<Shape> &shapes);
    void replaceObjects(const QVector<SceneObject> &objects);

    const DocumentCollection *collection(CollectionId id) const;
    CollectionId createCollection(const QString &name);
    CollectionId insertCollection(DocumentCollection collection);
    bool removeCollection(CollectionId id);
    bool renameCollection(CollectionId id, const QString &name);
    bool setCollectionParents(CollectionId id, const QVector<CollectionId> &parentIds);
    bool moveCollectionRelative(CollectionId id,
                                CollectionId targetId,
                                CollectionId parentId,
                                bool after);
    bool setCollectionVisibility(CollectionId id, bool hideViewport, bool hideRender);
    bool setCollectionHideSelect(CollectionId id, bool hideSelect);
    bool setObjectCollections(ObjectId objectId, const QVector<CollectionId> &collectionIds);
    bool setObjectVisibilityFlags(ObjectId objectId,
                                  bool hideViewport,
                                  bool hideSelect);
    bool setObjectRenderVisibility(ObjectId objectId, bool hideRender);
    bool setObjectViewLayerHidden(ObjectId objectId,
                                  const QString &viewLayerKey,
                                  bool hidden);
    bool setObjectParent(ObjectId objectId, ObjectId parentObjectId);
    bool setCollectionViewLayerState(CollectionId collectionId,
                                     const QString &viewLayerKey,
                                     const QStringList &sourcePath,
                                     const QString &stateKey,
                                     bool value);
    bool setObjectDisplayName(ObjectId objectId, const QString &name);
    bool isObjectVisible(ObjectId objectId) const;
    bool isObjectVisible(ObjectId objectId,
                         const QString &viewLayerKey) const;
    // Render visibility follows Blender's hide_render/excluded collection
    // state without applying viewport-only hide flags.
    bool isObjectRenderable(ObjectId objectId) const;
    bool isObjectEditable(ObjectId objectId) const;
    bool isObjectEditable(ObjectId objectId,
                          const QString &viewLayerKey) const;

    Snapshot snapshot() const;
    void restoreSnapshot(const Snapshot &snapshot);

    // Compatibility assignment for tests and the old viewport migration
    // path. It deliberately creates fresh identities for the supplied scene.
    Document &operator=(const QVector<Shape> &shapes);

    class ConstShapeIterator {
    public:
        using Underlying = QVector<SceneObject>::const_iterator;

        ConstShapeIterator() = default;
        explicit ConstShapeIterator(Underlying iterator)
            : iterator_(iterator)
        {
        }

        const Shape &operator*() const
        {
            return iterator_->geometry;
        }

        ConstShapeIterator &operator++()
        {
            ++iterator_;
            return *this;
        }

        bool operator!=(const ConstShapeIterator &other) const
        {
            return iterator_ != other.iterator_;
        }

    private:
        Underlying iterator_;
    };

    ConstShapeIterator begin() const;
    ConstShapeIterator end() const;

private:
    friend class DocumentTransaction;

    bool addMaterial(const DocumentMaterial &material);

    ObjectId allocateObjectId();
    CollectionId allocateCollectionId();
    void rebuildObjectIndex();
    void bumpRevision(quint64 *revision);
    void noteGeometryChange(ObjectId id);
    SceneObject *mutableObject(ObjectId id);
    Shape *mutableShape(ObjectId id);
    DocumentCollection *mutableCollection(CollectionId id);

    QVector<DocumentCollection> collections_;
    QVector<SceneObject> objects_;
    QVector<DocumentMaterial> materials_;
    QHash<QString, QStringList> outlinerExpandedPathsByScene_;
    QHash<quint64, int> objectIndices_;
    QHash<quint64, quint64> objectGeometryRevisions_;
    quint64 nextObjectValue_ = 1;
    quint64 nextCollectionValue_ = 1;
    DocumentSettings settings_;
    RuntimeRevisions revisions_;
    quint64 revisionClock_ = 1;
};

} // namespace classiCAD
