/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "core/document/document_collection.h"

#include <QFrame>
#include <QSet>
#include <QVector>

#include <functional>

class QTreeWidget;
class QTreeWidgetItem;

namespace classiCAD {

struct CollectionPanelEntry {
    DocumentCollection collection;
    int objectCount = 0;
};

struct CollectionsPanelCallbacks {
    std::function<QVector<CollectionPanelEntry>()> entries;
    std::function<void(CollectionId, bool)> setViewportVisibility;
};

// Displays Blender-style collection links and view-layer visibility.
class CollectionsPanel final : public QFrame {
public:
    explicit CollectionsPanel(CollectionsPanelCallbacks callbacks,
                               QWidget *parent = nullptr);
    void refresh();

private:
    void createLayout();
    void appendCollection(QTreeWidget *tree,
                         const CollectionPanelEntry &entry,
                         const QVector<CollectionPanelEntry> &entries,
                         QTreeWidgetItem *parent,
                         QSet<quint64> ancestors);

    CollectionsPanelCallbacks callbacks_;
    QTreeWidget *tree_ = nullptr;
    bool refreshing_ = false;
};

} // namespace classiCAD
