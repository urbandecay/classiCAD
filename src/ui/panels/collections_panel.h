/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "core/document/document_collection.h"
#include "core/document/object_id.h"

#include <QFrame>
#include <QPoint>
#include <QSet>
#include <QStringList>
#include <QVector>

#include <functional>

class QTreeWidget;
class QTreeWidgetItem;

namespace classiCAD {

struct CollectionPanelEntry {
    DocumentCollection collection;
};

struct OutlinerObjectEntry {
    ObjectId id = ObjectId::invalid();
    QString name;
    QVector<CollectionId> collectionIds;
    bool hideViewport = false;
    bool hideSelect = false;
};

struct CollectionsPanelCallbacks {
    std::function<QVector<CollectionPanelEntry>()> entries;
    std::function<QVector<OutlinerObjectEntry>()> objects;
    std::function<QVector<ObjectId>()> selectedObjectIds;
    std::function<ObjectId()> activeObjectId;
    std::function<bool(ObjectId)> canSelectObject;
    std::function<void(const QVector<ObjectId> &, ObjectId)> setObjectSelection;
    std::function<CollectionId(const QString &, CollectionId)> createCollection;
    std::function<void(CollectionId, const QString &)> renameCollection;
    std::function<void(CollectionId)> removeCollection;
    std::function<void(const QVector<ObjectId> &, CollectionId)> linkObjects;
    std::function<void(ObjectId, const QString &)> renameObject;
    std::function<void(ObjectId, CollectionId)> unlinkObject;
    std::function<void(CollectionId, bool)> setViewportVisibility;
    std::function<void(CollectionId, bool)> setCollectionSelectability;
    std::function<void(ObjectId, bool)> setObjectViewportVisibility;
    std::function<void(ObjectId, bool)> setObjectSelectability;
};

// Displays linked collections and scene objects with Blender-style selection
// and viewport/selectability controls.
class CollectionsPanel final : public QFrame {
public:
    explicit CollectionsPanel(CollectionsPanelCallbacks callbacks,
                               QWidget *parent = nullptr);
    void refresh();
    void syncSelection();

private:
    enum class ItemKind { Collection, Object, SceneRoot };

    void createLayout();
    void showContextMenu(const QPoint &position);
    bool activateObjectRow(QTreeWidgetItem *item,
                           Qt::KeyboardModifiers modifiers);
    void appendCollection(QTreeWidget *tree,
                         const CollectionPanelEntry &entry,
                         const QVector<CollectionPanelEntry> &entries,
                         const QVector<OutlinerObjectEntry> &objects,
                         QTreeWidgetItem *parent,
                         QSet<quint64> ancestors,
                         QStringList path);
    QTreeWidgetItem *appendObject(QTreeWidgetItem *parent,
                                  const OutlinerObjectEntry &entry,
                                  const QStringList &path);

    CollectionsPanelCallbacks callbacks_;
    QTreeWidget *tree_ = nullptr;
    QTreeWidgetItem *rangeAnchorItem_ = nullptr;
    bool refreshing_ = false;
    bool hasRefreshed_ = false;
};

} // namespace classiCAD
