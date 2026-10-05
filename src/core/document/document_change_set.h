#pragma once

#include "layer_id.h"
#include "object_id.h"

#include <QSet>
#include <QVector>

namespace classiCAD {

// Describes the parts of a committed document edit that downstream views and
// caches need to invalidate.
struct DocumentChangeSet {
    QVector<ObjectId> objectIds;
    QVector<LayerId> layerIds;
    bool geometryChanged = false;
    bool structureChanged = false;
    bool layerPropertiesChanged = false;
    bool visibilityChanged = false;
    bool settingsChanged = false;
    bool selectionChanged = false;

    bool isEmpty() const
    {
        return objectIds.isEmpty() && layerIds.isEmpty() &&
               !geometryChanged && !structureChanged &&
               !layerPropertiesChanged && !visibilityChanged &&
               !settingsChanged && !selectionChanged;
    }

    bool affectsPersistentDocument() const
    {
        return geometryChanged || structureChanged ||
               layerPropertiesChanged || visibilityChanged || settingsChanged;
    }

    bool affectsLayerPresentation() const
    {
        return structureChanged || layerPropertiesChanged ||
               visibilityChanged || !layerIds.isEmpty();
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

private:
    QSet<quint64> objectIdValues_;
    QSet<quint64> layerIdValues_;
};

} // namespace classiCAD
