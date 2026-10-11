/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "collections_panel.h"

#include <QHeaderView>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <algorithm>

namespace classiCAD {

CollectionsPanel::CollectionsPanel(CollectionsPanelCallbacks callbacks,
                                   QWidget *parent)
    : QFrame(parent), callbacks_(std::move(callbacks))
{
    createLayout();
    refresh();
}

void CollectionsPanel::createLayout()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    tree_ = new QTreeWidget(this);
    tree_->setColumnCount(2);
    tree_->setHeaderLabels({QStringLiteral("Collection"),
                            QStringLiteral("Viewport")});
    tree_->setRootIsDecorated(true);
    tree_->setAlternatingRowColors(true);
    tree_->setUniformRowHeights(true);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tree_->setColumnWidth(1, 72);
    tree_->setToolTip(QStringLiteral(
        "Collection links are independent of CAD layer assignment."));
    layout->addWidget(tree_);

    connect(tree_, &QTreeWidget::itemChanged, this,
            [this](QTreeWidgetItem *item, int column) {
        if (refreshing_ || item == nullptr ||
            column != 1 || !callbacks_.setViewportVisibility) {
            return;
        }
        const CollectionId id = CollectionId::fromValue(
            item->data(0, Qt::UserRole).toULongLong());
        if (!id.isValid()) return;
        callbacks_.setViewportVisibility(
            id, item->checkState(1) != Qt::Checked);
        refresh();
    });
}

void CollectionsPanel::appendCollection(
    QTreeWidget *tree,
    const CollectionPanelEntry &entry,
    const QVector<CollectionPanelEntry> &entries,
    QTreeWidgetItem *parent,
    QSet<quint64> ancestors)
{
    if (!entry.collection.id.isValid() ||
        ancestors.contains(entry.collection.id.value())) {
        return;
    }
    auto *item = parent == nullptr
        ? new QTreeWidgetItem(tree)
        : new QTreeWidgetItem(parent);
    item->setText(0, entry.objectCount > 0
        ? QStringLiteral("%1 (%2 objects)")
              .arg(entry.collection.name).arg(entry.objectCount)
        : entry.collection.name);
    item->setData(0, Qt::UserRole,
                  QVariant::fromValue<qulonglong>(entry.collection.id.value()));
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(1, entry.collection.hideViewport
                               ? Qt::Unchecked : Qt::Checked);

    ancestors.insert(entry.collection.id.value());
    for (const CollectionPanelEntry &candidate : entries) {
        if (candidate.collection.parentIds.contains(entry.collection.id)) {
            appendCollection(tree, candidate, entries, item, ancestors);
        }
    }
}

void CollectionsPanel::refresh()
{
    if (tree_ == nullptr || !callbacks_.entries) return;
    refreshing_ = true;
    tree_->clear();
    const QVector<CollectionPanelEntry> entries = callbacks_.entries();
    for (const CollectionPanelEntry &entry : entries) {
        bool hasKnownParent = false;
        for (const CollectionId parentId : entry.collection.parentIds) {
            const auto parent = std::find_if(
                entries.cbegin(), entries.cend(), [parentId](const CollectionPanelEntry &candidate) {
                    return candidate.collection.id == parentId;
                });
            hasKnownParent = hasKnownParent || parent != entries.cend();
        }
        if (!hasKnownParent) {
            appendCollection(tree_, entry, entries, nullptr, {});
        }
    }
    tree_->expandAll();
    refreshing_ = false;
}

} // namespace classiCAD
