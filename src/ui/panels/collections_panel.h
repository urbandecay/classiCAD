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
class QComboBox;

namespace classiCAD {

struct CollectionPanelEntry {
    DocumentCollection collection;
    int objectCount = 0;
};

struct BlenderViewLayerChoice {
    QString key;
    QString sceneName;
    QString viewLayerName;
};

struct CollectionsPanelCallbacks {
    std::function<QVector<CollectionPanelEntry>()> entries;
    std::function<void(CollectionId, bool)> setViewportVisibility;
    std::function<QVector<BlenderViewLayerChoice>()> viewLayers;
    std::function<QString()> activeViewLayerKey;
    std::function<void(const QString &)> setActiveViewLayerKey;
};

// Displays Blender-style collection links independently from CAD layers.
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
    QComboBox *viewLayerCombo_ = nullptr;
    QTreeWidget *tree_ = nullptr;
    bool refreshing_ = false;
};

} // namespace classiCAD
