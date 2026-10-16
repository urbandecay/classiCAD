#pragma once

#include "collection_id.h"
#include "object_id.h"

#include <QSet>
#include <QVector>

namespace classiCAD {

// Describes the parts of a committed document edit that downstream views and
// caches need to invalidate.
struct DocumentChangeSet {
    QVector<ObjectId> objectIds;
    QVector<CollectionId> collectionIds;
    bool geometryChanged = false;
    bool placementChanged = false;
    bool structureChanged = false;
    bool visibilityChanged = false;
    bool settingsChanged = false;
    bool objectAppearanceChanged = false;
    bool materialsChanged = false;
    bool collectionsChanged = false;
    bool selectionChanged = false;

    bool isEmpty() const
    {
        return objectIds.isEmpty() && collectionIds.isEmpty() &&
               !geometryChanged && !placementChanged && !structureChanged &&
               !visibilityChanged &&
               !settingsChanged && !objectAppearanceChanged &&
               !materialsChanged && !collectionsChanged &&
               !selectionChanged;
    }

    bool affectsPersistentDocument() const
    {
        return geometryChanged || placementChanged || structureChanged ||
               visibilityChanged || settingsChanged ||
               objectAppearanceChanged || materialsChanged || collectionsChanged;
    }

    bool affectsScenePresentation() const
    {
        return structureChanged || visibilityChanged || collectionsChanged ||
               !collectionIds.isEmpty() || settingsChanged;
    }

    void addObject(ObjectId id)
    {
        if (id.isValid() && !objectIdValues_.contains(id.value())) {
            objectIdValues_.insert(id.value());
            objectIds.append(id);
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
    QSet<quint64> collectionIdValues_;
};

} // namespace classiCAD
