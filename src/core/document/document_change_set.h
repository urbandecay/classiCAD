#pragma once

#include "layer_id.h"
#include "object_id.h"

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
        if (id.isValid() && !objectIds.contains(id)) {
            objectIds.append(id);
        }
    }

    void addLayer(LayerId id)
    {
        if (id.isValid() && !layerIds.contains(id)) {
            layerIds.append(id);
        }
    }
};

} // namespace classiCAD
