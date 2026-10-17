#pragma once

#include "collection_id.h"
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
    QString sourceSceneLibraryPath;
    QString libraryPath;
    QVector<CollectionId> parentIds;
    QVector<CollectionParentOrder> parentOrders;
    bool hideViewport = false;
    bool hideRender = false;
    bool hideSelect = false;
    QJsonArray viewLayerStates;
};
} // namespace classiCAD
