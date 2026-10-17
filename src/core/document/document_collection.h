#pragma once

#include "collection_id.h"
#include "core/geometry/work_plane.h"
#include <QString>
#include <QJsonArray>
#include <QStringList>
#include <QVector>

namespace classiCAD {
struct CollectionParentOrder {
    CollectionId parentId = CollectionId::invalid();
    int order = 0;
};

struct DocumentCollection {
    CollectionId id = CollectionId::invalid();
    QString name;
    QString sourceId;
    QString sourceSceneName;
    QStringList sourceSceneNames;
    // True means sourceSceneNames is an exact scene-membership list, including
    // an empty list for an unlinked/orphan collection. False keeps the legacy
    // active-scene fallback for older native documents.
    bool sceneMembershipExplicit = false;
    QString sourceSceneLibraryPath;
    QString libraryPath;
    // Blender Collection.instance_offset is subtracted from linked contents
    // when this collection is instanced by an object.
    Point3D instanceOffset;
    QVector<CollectionId> parentIds;
    QVector<CollectionParentOrder> parentOrders;
    int rootOrder = -1;
    bool hideViewport = false;
    bool hideRender = false;
    bool hideSelect = false;
    QJsonArray viewLayerStates;
};
} // namespace classiCAD
