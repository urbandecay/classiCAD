#pragma once

#include "document_settings.h"
#include "layer.h"
#include "scene_object.h"

#include <QVector>

namespace classiCAD {

// Document owns persistent scene objects and their layer membership. The
// container-like shape accessors are a short-lived migration bridge for the
// viewport; the storage and identity remain owned by this class.
class Document final {
public:
    struct Snapshot {
        QVector<Layer> layers;
        QVector<SceneObject> objects;
        LayerId activeLayerId = LayerId::invalid();
        quint64 nextObjectValue = 1;
        quint64 nextLayerValue = 1;
        DocumentSettings settings;
    };

    Document();

    int size() const;
    bool isEmpty() const;
    const QVector<SceneObject> &objects() const;
    QVector<SceneObject> &objects();
    const QVector<Layer> &layers() const;
    const DocumentSettings &settings() const;
    bool setSettings(const DocumentSettings &settings);

    const SceneObject *object(ObjectId id) const;
    SceneObject *object(ObjectId id);
    const Shape *shape(ObjectId id) const;
    Shape *shape(ObjectId id);
    ObjectId objectIdAt(int index) const;
    int indexOf(ObjectId id) const;
    Shape &operator[](int index);
    const Shape &operator[](int index) const;

    ObjectId append(const Shape &shape);
    ObjectId insert(int index, const Shape &shape);
    ObjectId insertObject(int index, SceneObject object);
    bool replace(ObjectId id, const Shape &shape);
    bool remove(ObjectId id);
    bool removeAt(int index);

    // Used only while restoring legacy update sessions and by the current
    // explode implementation. New objects get IDs; existing snapshots use
    // restoreSnapshot() so IDs are preserved.
    QVector<ObjectId> replaceShapes(const QVector<Shape> &shapes);
    void replaceObjects(const QVector<SceneObject> &objects);

    LayerId activeLayerId() const;
    bool setActiveLayer(LayerId id);
    LayerId createLayer(const QString &name);
    bool removeLayer(LayerId id);
    bool renameLayer(LayerId id, const QString &name);
    bool moveLayer(LayerId id, int targetIndex);
    Layer *layer(LayerId id);
    const Layer *layer(LayerId id) const;
    bool setLayerVisible(LayerId id, bool visible);
    bool setLayerFrozen(LayerId id, bool frozen);
    bool setLayerLocked(LayerId id, bool locked);
    bool setLayerColor(LayerId id, const QColor &color);
    bool setLayerLineType(LayerId id, const QString &lineType);
    bool setLayerLineWeight(LayerId id, qreal lineWeightMm);
    bool setLayerPlotted(LayerId id, bool plotted);
    bool setLayerDescription(LayerId id, const QString &description);
    bool isLayerEditable(LayerId id) const;
    bool moveObjectToLayer(ObjectId objectId, LayerId layerId);
    bool isObjectVisible(ObjectId objectId) const;
    bool isObjectEditable(ObjectId objectId) const;

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
    ObjectId allocateObjectId();
    LayerId allocateLayerId();
    LayerId normalizedLayerId(LayerId requested) const;
    void rebuildLayerObjectIds();
    void ensureDefaultLayer();

    QVector<Layer> layers_;
    QVector<SceneObject> objects_;
    LayerId activeLayerId_ = LayerId::invalid();
    quint64 nextObjectValue_ = 1;
    quint64 nextLayerValue_ = 1;
    DocumentSettings settings_;
};

} // namespace classiCAD
