#pragma once

#include "collection_id.h"
#include <QString>
#include <QStringList>
#include <QVector>

namespace classiCAD {
struct DocumentCollection {
    CollectionId id = CollectionId::invalid();
    QString name;
    QString sourceId;
    QString sourceSceneName;
    QStringList sourceSceneNames;
    QString sourceSceneLibraryPath;
    QString libraryPath;
    QVector<CollectionId> parentIds;
    bool hideViewport = false;
    bool hideRender = false;
};
} // namespace classiCAD
