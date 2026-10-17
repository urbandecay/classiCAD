/* SPDX-FileCopyrightText: 2008 Blender Authors
 * SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The view-layer tree ordering, repeated collection-membership rows, and
 * select/activate, Ctrl-toggle, and range-selection rules are adapted from
 * Blender 5.2.2's tree_display_view_layer.cc and outliner_select.cc.
 */
#include "collections_panel.h"

#include <QAbstractItemView>
#include <QAction>
#include <QHash>
#include <QInputDialog>
#include <QFont>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVariant>
#include <QVBoxLayout>

#include <algorithm>

namespace classiCAD {

namespace {

constexpr int itemKindRole = Qt::UserRole + 1;
constexpr int itemIdRole = Qt::UserRole + 2;
constexpr int itemPathRole = Qt::UserRole + 3;
constexpr int nameColumn = 0;
constexpr int viewportColumn = 1;
constexpr int selectColumn = 2;

class OutlinerTreeWidget final : public QTreeWidget {
public:
    using ActivateObject =
        std::function<bool(QTreeWidgetItem *, Qt::KeyboardModifiers)>;

    explicit OutlinerTreeWidget(QWidget *parent = nullptr)
        : QTreeWidget(parent)
    {
    }

    void setActivationHandlers(ActivateObject activateObject,
                               std::function<void()> clickEmpty)
    {
        activateObject_ = std::move(activateObject);
        clickEmpty_ = std::move(clickEmpty);
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            QTreeWidgetItem *item = itemAt(event->position().toPoint());
            if (item == nullptr) {
                if (clickEmpty_) clickEmpty_();
            } else if (columnAt(event->position().toPoint().x()) == nameColumn &&
                       activateObject_ &&
                       activateObject_(item, event->modifiers())) {
                event->accept();
                return;
            }
        }
        QTreeWidget::mousePressEvent(event);
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down) {
            QTreeWidget::keyPressEvent(event);
            if (activateObject_ && activateObject_(currentItem(),
                                                    event->modifiers())) {
                event->accept();
                return;
            }
            return;
        }
        QTreeWidget::keyPressEvent(event);
    }

private:
    ActivateObject activateObject_;
    std::function<void()> clickEmpty_;
};

} // namespace

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
    auto *outlinerTree = new OutlinerTreeWidget(this);
    tree_ = outlinerTree;
    tree_->setColumnCount(3);
    tree_->setHeaderLabels({QStringLiteral("Name"),
                            QStringLiteral("Viewport"),
                            QStringLiteral("Select")});
    tree_->setRootIsDecorated(true);
    tree_->setAlternatingRowColors(true);
    tree_->setUniformRowHeights(true);
    tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tree_->header()->setSectionResizeMode(nameColumn, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(viewportColumn,
                                          QHeaderView::ResizeToContents);
    tree_->header()->setSectionResizeMode(selectColumn,
                                          QHeaderView::ResizeToContents);
    tree_->setToolTip(QStringLiteral(
        "Scene objects and collection memberships. Click to select and activate; Ctrl-click toggles selection. The restriction columns control viewport visibility and selection."));
    layout->addWidget(tree_);

    outlinerTree->setActivationHandlers(
        [this](QTreeWidgetItem *item, Qt::KeyboardModifiers modifiers) {
            return activateObjectRow(item, modifiers);
        },
        [this]() {
            if (callbacks_.setObjectSelection) {
                callbacks_.setObjectSelection({}, ObjectId::invalid());
            }
            rangeAnchorItem_ = nullptr;
        });
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree_, &QTreeWidget::customContextMenuRequested, this,
            [this](const QPoint &position) { showContextMenu(position); });

    connect(tree_, &QTreeWidget::itemChanged, this,
            [this](QTreeWidgetItem *item, int column) {
        if (refreshing_ || item == nullptr ||
            (column != viewportColumn && column != selectColumn)) {
            return;
        }
        const ItemKind kind = static_cast<ItemKind>(
            item->data(nameColumn, itemKindRole).toInt());
        const bool hidden = item->checkState(column) != Qt::Checked;
        const quint64 value =
            item->data(nameColumn, itemIdRole).toULongLong();
        // Applying a transaction notifies MainWindow synchronously, and its
        // observer rebuilds this tree. Defer the write until Qt has finished
        // dispatching itemChanged so the active QTreeWidgetItem is not deleted
        // from inside its own check-state update.
        QTimer::singleShot(0, this, [this, kind, column, hidden, value]() {
            if (kind == ItemKind::Collection) {
                const CollectionId id = CollectionId::fromValue(value);
                if (column == viewportColumn && callbacks_.setViewportVisibility) {
                    callbacks_.setViewportVisibility(id, hidden);
                } else if (column == selectColumn &&
                           callbacks_.setCollectionSelectability) {
                    callbacks_.setCollectionSelectability(id, hidden);
                }
            } else if (kind == ItemKind::Object) {
                const ObjectId id = ObjectId::fromValue(value);
                if (column == viewportColumn &&
                    callbacks_.setObjectViewportVisibility) {
                    callbacks_.setObjectViewportVisibility(id, hidden);
                } else if (column == selectColumn &&
                           callbacks_.setObjectSelectability) {
                    callbacks_.setObjectSelectability(id, hidden);
                }
            }
            refresh();
        });
    });

}

bool CollectionsPanel::activateObjectRow(
    QTreeWidgetItem *item, Qt::KeyboardModifiers modifiers)
{
    if (refreshing_ || item == nullptr ||
        item->data(nameColumn, itemKindRole).toInt() !=
            static_cast<int>(ItemKind::Object)) return false;
    if (!callbacks_.setObjectSelection) return true;
    const ObjectId clickedId = ObjectId::fromValue(
        item->data(nameColumn, itemIdRole).toULongLong());
    if (!clickedId.isValid() ||
        (callbacks_.canSelectObject &&
         !callbacks_.canSelectObject(clickedId))) {
        return true;
    }

    // Collection links can produce several tree rows for one object. Keep
    // the particular row that was activated as the current row even though
    // selection itself is shared by the canonical object ID.
    tree_->setCurrentItem(item, nameColumn, QItemSelectionModel::NoUpdate);

    const bool extend = modifiers.testFlag(Qt::ControlModifier);
    const bool useRange = modifiers.testFlag(Qt::ShiftModifier);
    QVector<ObjectId> selectedIds = callbacks_.selectedObjectIds
        ? callbacks_.selectedObjectIds() : QVector<ObjectId>{};
    ObjectId activeId = callbacks_.activeObjectId
        ? callbacks_.activeObjectId() : ObjectId::invalid();

    if (useRange && rangeAnchorItem_ != nullptr) {
        QVector<QTreeWidgetItem *> visibleObjectRows;
        const auto appendVisibleObjects = [&](const auto &self,
                                               QTreeWidgetItem *parent) -> void {
            if (parent == nullptr) return;
            if (parent->data(nameColumn, itemKindRole).toInt() ==
                static_cast<int>(ItemKind::Object)) {
                visibleObjectRows.append(parent);
            }
            if (!parent->isExpanded()) return;
            for (int childIndex = 0; childIndex < parent->childCount();
                 ++childIndex) {
                self(self, parent->child(childIndex));
            }
        };
        for (int topLevelIndex = 0;
             topLevelIndex < tree_->topLevelItemCount(); ++topLevelIndex) {
            appendVisibleObjects(appendVisibleObjects,
                                 tree_->topLevelItem(topLevelIndex));
        }
        const int anchorIndex = visibleObjectRows.indexOf(rangeAnchorItem_);
        const int clickedIndex = visibleObjectRows.indexOf(item);
        if (anchorIndex >= 0 && clickedIndex >= 0) {
            if (!extend) selectedIds.clear();
            const int first = qMin(anchorIndex, clickedIndex);
            const int last = qMax(anchorIndex, clickedIndex);
            for (int index = first; index <= last; ++index) {
                const ObjectId id = ObjectId::fromValue(
                    visibleObjectRows[index]
                        ->data(nameColumn, itemIdRole).toULongLong());
                if (id.isValid() && !selectedIds.contains(id) &&
                    (!callbacks_.canSelectObject ||
                     callbacks_.canSelectObject(id))) {
                    selectedIds.append(id);
                }
            }
            activeId = clickedId;
            callbacks_.setObjectSelection(selectedIds, activeId);
            return true;
        }
    }

    if (extend) {
        if (selectedIds.contains(clickedId)) {
            selectedIds.removeAll(clickedId);
            if (activeId == clickedId) {
                activeId = selectedIds.isEmpty() ? ObjectId::invalid()
                                                  : selectedIds.back();
            }
        } else {
            selectedIds.append(clickedId);
            activeId = clickedId;
        }
    } else {
        selectedIds = {clickedId};
        activeId = clickedId;
    }
    if (!useRange) rangeAnchorItem_ = item;
    callbacks_.setObjectSelection(selectedIds, activeId);
    return true;
}

void CollectionsPanel::showContextMenu(const QPoint &position)
{
    QTreeWidgetItem *item = tree_ != nullptr ? tree_->itemAt(position) : nullptr;
    if (item == nullptr) return;
    const ItemKind kind = static_cast<ItemKind>(
        item->data(nameColumn, itemKindRole).toInt());
    QMenu menu(this);

    if (kind == ItemKind::SceneRoot && callbacks_.createCollection) {
        QAction *create = menu.addAction(QStringLiteral("New Collection"));
        connect(create, &QAction::triggered, this, [this]() {
            bool accepted = false;
            const QString name = QInputDialog::getText(
                this, QStringLiteral("New Collection"),
                QStringLiteral("Name"), QLineEdit::Normal, {}, &accepted);
            if (accepted && !name.trimmed().isEmpty()) {
                callbacks_.createCollection(name, CollectionId::invalid());
            }
        });
    } else if (kind == ItemKind::Collection) {
        const CollectionId collectionId = CollectionId::fromValue(
            item->data(nameColumn, itemIdRole).toULongLong());
        if (callbacks_.createCollection) {
            QAction *create = menu.addAction(QStringLiteral("New Collection"));
            connect(create, &QAction::triggered, this, [this, collectionId]() {
                bool accepted = false;
                const QString name = QInputDialog::getText(
                    this, QStringLiteral("New Collection"),
                    QStringLiteral("Name"), QLineEdit::Normal, {}, &accepted);
                if (accepted && !name.trimmed().isEmpty()) {
                    callbacks_.createCollection(name, collectionId);
                }
            });
        }
        if (callbacks_.renameCollection) {
            QAction *rename = menu.addAction(QStringLiteral("Rename"));
            connect(rename, &QAction::triggered, this, [this, item, collectionId]() {
                bool accepted = false;
                const QString name = QInputDialog::getText(
                    this, QStringLiteral("Rename Collection"),
                    QStringLiteral("Name"), QLineEdit::Normal,
                    item->text(nameColumn), &accepted);
                if (accepted && !name.trimmed().isEmpty()) {
                    callbacks_.renameCollection(collectionId, name);
                }
            });
        }
        if (callbacks_.setObjectSelection) {
            QAction *selectObjects = menu.addAction(
                QStringLiteral("Select Objects"));
            connect(selectObjects, &QAction::triggered, this,
                    [this, item]() {
                QVector<ObjectId> objectIds;
                QSet<quint64> seen;
                QVector<QTreeWidgetItem *> pending{item};
                while (!pending.isEmpty()) {
                    QTreeWidgetItem *current = pending.takeLast();
                    if (current->data(nameColumn, itemKindRole).toInt() ==
                        static_cast<int>(ItemKind::Object)) {
                        const ObjectId id = ObjectId::fromValue(
                            current->data(nameColumn, itemIdRole)
                                .toULongLong());
                        if (id.isValid() && !seen.contains(id.value()) &&
                            (!callbacks_.canSelectObject ||
                             callbacks_.canSelectObject(id))) {
                            seen.insert(id.value());
                            objectIds.append(id);
                        }
                    }
                    for (int childIndex = 0;
                         childIndex < current->childCount(); ++childIndex) {
                        pending.append(current->child(childIndex));
                    }
                }
                callbacks_.setObjectSelection(
                    objectIds, objectIds.isEmpty() ? ObjectId::invalid()
                                                   : objectIds.back());
            });
        }
        const QVector<ObjectId> selectedIds = callbacks_.selectedObjectIds
            ? callbacks_.selectedObjectIds() : QVector<ObjectId>{};
        if (callbacks_.linkObjects && !selectedIds.isEmpty()) {
            QAction *link = menu.addAction(
                QStringLiteral("Link Selected Objects"));
            connect(link, &QAction::triggered, this,
                    [this, selectedIds, collectionId]() {
                callbacks_.linkObjects(selectedIds, collectionId);
            });
        }
        if (callbacks_.removeCollection) {
            menu.addSeparator();
            QAction *remove = menu.addAction(
                QStringLiteral("Remove Collection"));
            connect(remove, &QAction::triggered, this,
                    [this, collectionId]() {
                if (QMessageBox::question(
                        this, QStringLiteral("Remove Collection"),
                        QStringLiteral("Remove this collection? Objects linked only here will become unlinked.")) ==
                    QMessageBox::Yes) {
                    callbacks_.removeCollection(collectionId);
                }
            });
        }
    } else if (kind == ItemKind::Object) {
        const ObjectId objectId = ObjectId::fromValue(
            item->data(nameColumn, itemIdRole).toULongLong());
        if (callbacks_.setObjectSelection) {
            QAction *select = menu.addAction(QStringLiteral("Select"));
            connect(select, &QAction::triggered, this, [this, objectId]() {
                if (callbacks_.canSelectObject &&
                    !callbacks_.canSelectObject(objectId)) return;
                callbacks_.setObjectSelection({objectId}, objectId);
            });
        }
        if (callbacks_.renameObject) {
            QAction *rename = menu.addAction(QStringLiteral("Rename"));
            connect(rename, &QAction::triggered, this, [this, item, objectId]() {
                bool accepted = false;
                const QString name = QInputDialog::getText(
                    this, QStringLiteral("Rename Object"),
                    QStringLiteral("Name"), QLineEdit::Normal,
                    item->text(nameColumn), &accepted);
                if (accepted && !name.trimmed().isEmpty()) {
                    callbacks_.renameObject(objectId, name);
                }
            });
        }
        if (callbacks_.unlinkObject && item->parent() != nullptr &&
            item->parent()->data(nameColumn, itemKindRole).toInt() ==
                static_cast<int>(ItemKind::Collection)) {
            const CollectionId collectionId = CollectionId::fromValue(
                item->parent()->data(nameColumn, itemIdRole).toULongLong());
            QAction *unlink = menu.addAction(
                QStringLiteral("Unlink from Collection"));
            connect(unlink, &QAction::triggered, this,
                    [this, objectId, collectionId]() {
                callbacks_.unlinkObject(objectId, collectionId);
            });
        }
    }

    if (!menu.actions().isEmpty()) {
        menu.exec(tree_->viewport()->mapToGlobal(position));
    }
}

void CollectionsPanel::appendCollection(
    QTreeWidget *tree,
    const CollectionPanelEntry &entry,
    const QVector<CollectionPanelEntry> &entries,
    const QVector<OutlinerObjectEntry> &objects,
    QTreeWidgetItem *parent,
    QSet<quint64> ancestors,
    QStringList path)
{
    if (!entry.collection.id.isValid() ||
        ancestors.contains(entry.collection.id.value())) {
        return;
    }
    auto *item = parent == nullptr
        ? new QTreeWidgetItem(tree)
        : new QTreeWidgetItem(parent);
    item->setText(nameColumn, entry.collection.name);
    item->setData(nameColumn, itemKindRole,
                  static_cast<int>(ItemKind::Collection));
    item->setData(nameColumn, itemIdRole,
                  QVariant::fromValue<qulonglong>(entry.collection.id.value()));
    path.append(QStringLiteral("c%1").arg(entry.collection.id.value()));
    item->setData(nameColumn, itemPathRole, path.join(QLatin1Char('/')));
    item->setFlags((item->flags() | Qt::ItemIsUserCheckable) &
                   ~Qt::ItemIsSelectable);
    item->setCheckState(viewportColumn, entry.collection.hideViewport
                               ? Qt::Unchecked : Qt::Checked);
    item->setCheckState(selectColumn, entry.collection.hideSelect
                               ? Qt::Unchecked : Qt::Checked);

    ancestors.insert(entry.collection.id.value());
    for (const CollectionPanelEntry &candidate : entries) {
        if (candidate.collection.parentIds.contains(entry.collection.id)) {
            appendCollection(tree, candidate, entries, objects, item, ancestors,
                             path);
        }
    }
    for (const OutlinerObjectEntry &object : objects) {
        if (object.collectionIds.contains(entry.collection.id)) {
            appendObject(item, object, path);
        }
    }
}

QTreeWidgetItem *CollectionsPanel::appendObject(
    QTreeWidgetItem *parent,
    const OutlinerObjectEntry &entry,
    const QStringList &path)
{
    if (parent == nullptr || !entry.id.isValid()) return nullptr;
    auto *item = new QTreeWidgetItem(parent);
    item->setText(nameColumn, entry.name);
    item->setData(nameColumn, itemKindRole,
                  static_cast<int>(ItemKind::Object));
    item->setData(nameColumn, itemIdRole,
                  QVariant::fromValue<qulonglong>(entry.id.value()));
    QStringList objectPath = path;
    objectPath.append(QStringLiteral("o%1").arg(entry.id.value()));
    item->setData(nameColumn, itemPathRole,
                  objectPath.join(QLatin1Char('/')));
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable |
                   Qt::ItemIsSelectable);
    item->setCheckState(viewportColumn, entry.hideViewport
                               ? Qt::Unchecked : Qt::Checked);
    item->setCheckState(selectColumn, entry.hideSelect
                               ? Qt::Unchecked : Qt::Checked);
    return item;
}

void CollectionsPanel::refresh()
{
    if (tree_ == nullptr || !callbacks_.entries) return;
    QHash<QString, bool> expandedPaths;
    const int scrollPosition = tree_->verticalScrollBar()->value();
    for (int topLevelIndex = 0;
         topLevelIndex < tree_->topLevelItemCount(); ++topLevelIndex) {
        QVector<QTreeWidgetItem *> pending{tree_->topLevelItem(topLevelIndex)};
        while (!pending.isEmpty()) {
            QTreeWidgetItem *item = pending.takeLast();
            const QString path = item->data(nameColumn, itemPathRole).toString();
            if (!path.isEmpty()) expandedPaths.insert(path, item->isExpanded());
            for (int childIndex = 0; childIndex < item->childCount();
                 ++childIndex) {
                pending.append(item->child(childIndex));
            }
        }
    }
    refreshing_ = true;
    tree_->clear();
    rangeAnchorItem_ = nullptr;
    const QVector<CollectionPanelEntry> entries = callbacks_.entries();
    const QVector<OutlinerObjectEntry> objects = callbacks_.objects
        ? callbacks_.objects() : QVector<OutlinerObjectEntry>{};
    auto *sceneRoot = new QTreeWidgetItem(tree_);
    sceneRoot->setText(nameColumn, QStringLiteral("Scene Collection"));
    sceneRoot->setData(nameColumn, itemKindRole,
                       static_cast<int>(ItemKind::SceneRoot));
    sceneRoot->setData(nameColumn, itemPathRole, QStringLiteral("scene"));
    sceneRoot->setFlags(sceneRoot->flags() & ~Qt::ItemIsSelectable);
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
            appendCollection(tree_, entry, entries, objects, sceneRoot, {},
                             {QStringLiteral("scene")});
        }
    }
    for (const OutlinerObjectEntry &object : objects) {
        if (object.collectionIds.isEmpty()) {
            appendObject(sceneRoot, object,
                         {QStringLiteral("scene"), QStringLiteral("root")});
        }
    }
    if (!hasRefreshed_) {
        tree_->expandAll();
        hasRefreshed_ = true;
    } else {
        QVector<QTreeWidgetItem *> pending{sceneRoot};
        while (!pending.isEmpty()) {
            QTreeWidgetItem *item = pending.takeLast();
            const QString path = item->data(nameColumn, itemPathRole).toString();
            const auto saved = expandedPaths.constFind(path);
            if (saved != expandedPaths.cend()) {
                item->setExpanded(saved.value());
            } else if (item->data(nameColumn, itemKindRole).toInt() ==
                       static_cast<int>(ItemKind::Collection)) {
                item->setExpanded(true);
            }
            for (int childIndex = 0; childIndex < item->childCount();
                 ++childIndex) {
                pending.append(item->child(childIndex));
            }
        }
    }
    refreshing_ = false;
    tree_->verticalScrollBar()->setValue(scrollPosition);
    syncSelection();
}

void CollectionsPanel::syncSelection()
{
    if (tree_ == nullptr) return;
    const QVector<ObjectId> selectedIds = callbacks_.selectedObjectIds
        ? callbacks_.selectedObjectIds() : QVector<ObjectId>{};
    QSet<quint64> selectedValues;
    for (const ObjectId id : selectedIds) {
        if (id.isValid()) selectedValues.insert(id.value());
    }
    const ObjectId activeId = callbacks_.activeObjectId
        ? callbacks_.activeObjectId() : ObjectId::invalid();

    refreshing_ = true;
    const QSignalBlocker blocker(tree_);
    QTreeWidgetItem *activeItem = nullptr;
    QTreeWidgetItem *currentItem = tree_->currentItem();
    for (int topLevelIndex = 0;
         topLevelIndex < tree_->topLevelItemCount(); ++topLevelIndex) {
        QTreeWidgetItem *topLevel = tree_->topLevelItem(topLevelIndex);
        QVector<QTreeWidgetItem *> pending{topLevel};
        while (!pending.isEmpty()) {
            QTreeWidgetItem *item = pending.takeLast();
            if (item->data(nameColumn, itemKindRole).toInt() ==
                static_cast<int>(ItemKind::Object)) {
                const quint64 value =
                    item->data(nameColumn, itemIdRole).toULongLong();
                item->setSelected(selectedValues.contains(value));
                QFont font = item->font(nameColumn);
                font.setBold(activeId.isValid() && value == activeId.value());
                item->setFont(nameColumn, font);
                if (activeId.isValid() && value == activeId.value() &&
                    currentItem == item) {
                    activeItem = item;
                } else if (activeItem == nullptr && activeId.isValid() &&
                           value == activeId.value()) {
                    activeItem = item;
                }
            }
            for (int childIndex = 0; childIndex < item->childCount();
                 ++childIndex) {
                pending.append(item->child(childIndex));
            }
        }
    }
    if (activeItem != nullptr) {
        tree_->setCurrentItem(activeItem, nameColumn,
                              QItemSelectionModel::NoUpdate);
    }
    refreshing_ = false;
}

} // namespace classiCAD
