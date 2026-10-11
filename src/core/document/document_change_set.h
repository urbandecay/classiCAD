#pragma once

#include "layer_id.h"
#include "collection_id.h"
#include "object_id.h"

#include <QSet>
#include <QVector>

namespace classiCAD {

// Describes the parts of a committed document edit that downstream views and
// caches need to invalidate.
struct DocumentChangeSet {
    QVector<ObjectId> objectIds;
    QVector<LayerId> layerIds;
    QVector<CollectionId> collectionIds;
    bool geometryChanged = false;
    bool placementChanged = false;
    bool structureChanged = false;
    bool layerPropertiesChanged = false;
    bool visibilityChanged = false;
    bool settingsChanged = false;
    bool objectAppearanceChanged = false;
    bool materialsChanged = false;
    bool collectionsChanged = false;
    bool selectionChanged = false;

    bool isEmpty() const
    {
        return objectIds.isEmpty() && layerIds.isEmpty() && collectionIds.isEmpty() &&
               !geometryChanged && !placementChanged && !structureChanged &&
               !layerPropertiesChanged && !visibilityChanged &&
               !settingsChanged && !objectAppearanceChanged &&
               !materialsChanged && !collectionsChanged &&
               !selectionChanged;
    }

    bool affectsPersistentDocument() const
    {
        return geometryChanged || placementChanged || structureChanged ||
               layerPropertiesChanged || visibilityChanged || settingsChanged ||
               objectAppearanceChanged || materialsChanged || collectionsChanged;
    }

    bool affectsLayerPresentation() const
    {
        return structureChanged || layerPropertiesChanged ||
               visibilityChanged || collectionsChanged || !layerIds.isEmpty() ||
               !collectionIds.isEmpty() || settingsChanged;
    }

    void addObject(ObjectId id)
    {
        if (id.isValid() && !objectIdValues_.contains(id.value())) {
            objectIdValues_.insert(id.value());
            objectIds.append(id);
        }
    }

    void addLayer(LayerId id)
    {
        if (id.isValid() && !layerIdValues_.contains(id.value())) {
            layerIdValues_.insert(id.value());
            layerIds.append(id);
        }
    }

    void addCollection(CollectionId id)
    {
        if (id.isValid() && !collectionIdValues_.contains(id.value())) {
            collectionIdValues_.insert(id.value());
            collectionIds.append(id);
        }
    }

private:
    QSet<quint64> objectIdValues_;
    QSet<quint64> layerIdValues_;
    QSet<quint64> collectionIdValues_;
};

} // namespace classiCAD
