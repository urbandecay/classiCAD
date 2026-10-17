/* SPDX-FileCopyrightText: 2008 Blender Authors
 * SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The view-layer tree ordering, repeated collection-membership rows, and
 * select/activate, Ctrl-toggle, and range-selection rules are adapted from
 * Blender 5.2.2's tree_display_view_layer.cc and outliner_select.cc.
 * Collection and object drop outcomes are adapted from outliner_dragdrop.cc.
 * Shift-recursive restriction changes are adapted from outliner_draw.cc.
 */
#include "collections_panel.h"

#include <QAbstractItemView>
#include <QAbstractItemModel>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QHash>
#include <QInputDialog>
#include <QFont>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVariant>
#include <QVBoxLayout>
#include <QRegularExpression>

#include <algorithm>

namespace classiCAD {

namespace {

constexpr int itemKindRole = Qt::UserRole + 1;
constexpr int itemIdRole = Qt::UserRole + 2;
constexpr int itemPathRole = Qt::UserRole + 3;
constexpr int itemViewLayerEditableRole = Qt::UserRole + 4;
constexpr int itemSourcePathRole = Qt::UserRole + 5;
constexpr int itemLinkedCollectionRole = Qt::UserRole + 6;
constexpr int itemNotInCollectionRole = Qt::UserRole + 7;
constexpr int itemParentCollectionRole = Qt::UserRole + 8;
constexpr int nameColumn = 0;
constexpr int viewportColumn = 1;
constexpr int renderColumn = 2;
constexpr int viewLayerViewportColumn = 3;
constexpr int selectColumn = 4;
constexpr int excludeColumn = 5;

QJsonObject viewLayerState(const DocumentCollection &collection,
                           const QString &viewLayerKey,
                           const QStringList &sourcePath)
{
    if (viewLayerKey.isEmpty() || sourcePath.isEmpty()) return {};
    for (const QJsonValue &stateValue : collection.viewLayerStates) {
        const QJsonObject state = stateValue.toObject();
        if (state.value(QStringLiteral("view_layer_key")).toString() !=
            viewLayerKey) {
            continue;
        }
        const QJsonArray storedPath = state.value(QStringLiteral("path")).toArray();
        if (storedPath.size() != sourcePath.size()) continue;
        bool matches = true;
        for (int index = 0; index < sourcePath.size(); ++index) {
            if (!storedPath[index].isString() ||
                storedPath[index].toString() != sourcePath[index]) {
                matches = false;
                break;
            }
        }
        if (matches) return state;
    }
    return {};
}

class ViewLayerCheckDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    bool editorEvent(QEvent *event,
                     QAbstractItemModel *model,
                     const QStyleOptionViewItem &option,
                     const QModelIndex &index) override
    {
        if (!index.data(itemViewLayerEditableRole).toBool()) return false;
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }
};

class OutlinerTreeWidget final : public QTreeWidget {
public:
    using ActivateObject =
        std::function<bool(QTreeWidgetItem *, Qt::KeyboardModifiers)>;
    using ApplyDrop = std::function<void(
        const QVector<OutlinerRowReference> &,
        const OutlinerRowReference &,
        bool,
        bool)>;

    explicit OutlinerTreeWidget(QWidget *parent = nullptr)
        : QTreeWidget(parent)
    {
    }

    void setActivationHandlers(ActivateObject activateObject,
                               std::function<void()> clickEmpty,
                               std::function<void()> syncSelection,
                               ApplyDrop applyDrop)
    {
        activateObject_ = std::move(activateObject);
        clickEmpty_ = std::move(clickEmpty);
        syncSelection_ = std::move(syncSelection);
        applyDrop_ = std::move(applyDrop);
    }

    Qt::KeyboardModifiers restrictionModifiersFor(
        const QTreeWidgetItem *item, int column) const
    {
        if (restrictionItem_ == item && restrictionColumn_ == column) {
            return restrictionModifiers_;
        }
        return QApplication::keyboardModifiers();
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            QTreeWidgetItem *item = itemAt(event->position().toPoint());
            const int column = columnAt(event->position().toPoint().x());
            if (item != nullptr && column >= viewportColumn &&
                column <= excludeColumn) {
                restrictionItem_ = item;
                restrictionColumn_ = column;
                restrictionModifiers_ = event->modifiers();
            }
            if (item == nullptr) {
                if (clickEmpty_) clickEmpty_();
            } else if (columnAt(event->position().toPoint().x()) == nameColumn &&
                       activateObject_ &&
                       activateObject_(item, event->modifiers())) {
                QTreeWidget::mousePressEvent(event);
                if (syncSelection_) syncSelection_();
                return;
            }
        }
        QTreeWidget::mousePressEvent(event);
        if (event->button() == Qt::LeftButton && syncSelection_) {
            syncSelection_();
        }
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        QTreeWidget::mouseReleaseEvent(event);
        if (event->button() == Qt::LeftButton) clearRestrictionPress();
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        clearRestrictionPress();
        if (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down) {
            QTreeWidget::keyPressEvent(event);
            if (activateObject_ && activateObject_(currentItem(),
                                                    event->modifiers())) {
                if (syncSelection_) syncSelection_();
            }
            return;
        }
        QTreeWidget::keyPressEvent(event);
    }

    void startDrag(Qt::DropActions supportedActions) override
    {
        draggedRows_.clear();
        QSet<QString> seen;
        const auto append = [this, &seen](QTreeWidgetItem *item) {
            if (item == nullptr) return;
            const OutlinerRowReference reference = rowReference(item);
            if (reference.kind == OutlinerRowKind::SceneRoot) return;
            const QString key = reference.kind == OutlinerRowKind::Object
                ? QStringLiteral("o%1").arg(reference.objectId.value())
                : QStringLiteral("c%1").arg(reference.collectionId.value());
            if (seen.contains(key)) return;
            seen.insert(key);
            draggedRows_.append(reference);
        };
        append(currentItem());
        for (QTreeWidgetItem *item : selectedItems()) append(item);
        QTreeWidget::startDrag(supportedActions);
        draggedRows_.clear();
    }

    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (event->source() == this) {
            event->acceptProposedAction();
            return;
        }
        QTreeWidget::dragEnterEvent(event);
    }

    void dragMoveEvent(QDragMoveEvent *event) override
    {
        if (event->source() == this) {
            QTreeWidget::dragMoveEvent(event);
            if (!event->isAccepted()) event->acceptProposedAction();
            return;
        }
        QTreeWidget::dragMoveEvent(event);
    }

    void dropEvent(QDropEvent *event) override
    {
        if (event->source() != this || draggedRows_.isEmpty() || !applyDrop_) {
            event->ignore();
            return;
        }

        QTreeWidgetItem *targetItem = itemAt(event->position().toPoint());
        OutlinerRowReference target = targetItem == nullptr
            ? OutlinerRowReference{} : rowReference(targetItem);
        if (target.kind == OutlinerRowKind::Object &&
            !event->modifiers().testFlag(Qt::ShiftModifier)) {
            target.kind = target.parentCollectionId.isValid()
                ? OutlinerRowKind::Collection : OutlinerRowKind::SceneRoot;
            target.collectionId = target.parentCollectionId;
        } else if (target.kind == OutlinerRowKind::Collection &&
                   dropIndicatorPosition() != OnItem) {
            target.collectionId = target.parentCollectionId;
            target.kind = target.collectionId.isValid()
                ? OutlinerRowKind::Collection : OutlinerRowKind::SceneRoot;
        } else if (target.kind == OutlinerRowKind::Object &&
                   dropIndicatorPosition() != OnItem) {
            target.collectionId = target.parentCollectionId;
            target.kind = target.collectionId.isValid()
                ? OutlinerRowKind::Collection : OutlinerRowKind::SceneRoot;
        }

        const QVector<OutlinerRowReference> sources = draggedRows_;
        const bool link = event->modifiers().testFlag(Qt::ControlModifier);
        const bool parent = event->modifiers().testFlag(Qt::ShiftModifier) &&
                            target.kind == OutlinerRowKind::Object;
        QTimer::singleShot(0, this, [this, sources, target, link, parent]() {
            if (applyDrop_) applyDrop_(sources, target, link, parent);
        });
        event->setDropAction(Qt::MoveAction);
        event->accept();
    }

private:
    void clearRestrictionPress()
    {
        restrictionItem_ = nullptr;
        restrictionColumn_ = -1;
        restrictionModifiers_ = Qt::NoModifier;
    }

    static OutlinerRowReference rowReference(const QTreeWidgetItem *item)
    {
        OutlinerRowReference reference;
        if (item == nullptr) return reference;
        reference.kind = static_cast<OutlinerRowKind>(
            item->data(nameColumn, itemKindRole).toInt());
        const quint64 value = item->data(nameColumn, itemIdRole).toULongLong();
        if (reference.kind == OutlinerRowKind::Object) {
            reference.objectId = ObjectId::fromValue(value);
            reference.linkedCollectionId = CollectionId::fromValue(
                item->data(nameColumn, itemLinkedCollectionRole).toULongLong());
            reference.syntheticParentRow =
                item->data(nameColumn, itemNotInCollectionRole).toBool();
            QTreeWidgetItem *ancestor = item->parent();
            while (ancestor != nullptr &&
                   ancestor->data(nameColumn, itemKindRole).toInt() ==
                       static_cast<int>(OutlinerRowKind::Object)) {
                ancestor = ancestor->parent();
            }
            if (ancestor != nullptr &&
                ancestor->data(nameColumn, itemKindRole).toInt() ==
                    static_cast<int>(OutlinerRowKind::Collection)) {
                reference.parentCollectionId = CollectionId::fromValue(
                    ancestor->data(nameColumn, itemIdRole).toULongLong());
            }
        } else if (reference.kind == OutlinerRowKind::Collection) {
            reference.collectionId = CollectionId::fromValue(value);
            reference.parentCollectionId = CollectionId::fromValue(
                item->data(nameColumn, itemParentCollectionRole).toULongLong());
        }
        return reference;
    }

    ActivateObject activateObject_;
    std::function<void()> clickEmpty_;
    std::function<void()> syncSelection_;
    ApplyDrop applyDrop_;
    QVector<OutlinerRowReference> draggedRows_;
    const QTreeWidgetItem *restrictionItem_ = nullptr;
    int restrictionColumn_ = -1;
    Qt::KeyboardModifiers restrictionModifiers_ = Qt::NoModifier;
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
    filterEdit_ = new QLineEdit(this);
    filterEdit_->setClearButtonEnabled(true);
    filterEdit_->setPlaceholderText(QStringLiteral("Filter objects and collections"));
    filterEdit_->setToolTip(QStringLiteral(
        "Case-insensitive name filter. Use * and ? as wildcards."));
    auto *filterRow = new QHBoxLayout;
    filterRow->setContentsMargins(0, 0, 0, 0);
    filterRow->addWidget(filterEdit_, 1);
    viewLayerCombo_ = new QComboBox(this);
    viewLayerCombo_->setToolTip(QStringLiteral("Active Blender view layer"));
    filterRow->addWidget(viewLayerCombo_);
    layout->addLayout(filterRow);
    auto *outlinerTree = new OutlinerTreeWidget(this);
    tree_ = outlinerTree;
    tree_->setColumnCount(6);
    tree_->setHeaderLabels({QStringLiteral("Name"),
                            QStringLiteral("Viewport"),
                            QStringLiteral("Render"),
                            QStringLiteral("Layer View"),
                            QStringLiteral("Select"),
                            QStringLiteral("Exclude")});
    tree_->headerItem()->setToolTip(viewportColumn,
                                    QStringLiteral("Hide globally in the viewport. Shift-click applies to descendants."));
    tree_->headerItem()->setToolTip(renderColumn,
                                    QStringLiteral("Hide globally in renders. Shift-click applies to descendants."));
    tree_->headerItem()->setToolTip(viewLayerViewportColumn,
                                    QStringLiteral("Hide in the active Blender view layer. Shift-click applies to descendants."));
    tree_->headerItem()->setToolTip(selectColumn,
                                    QStringLiteral("Prevent selection. Shift-click applies to descendants."));
    tree_->headerItem()->setToolTip(excludeColumn,
                                    QStringLiteral("Exclude this collection path from the active view layer. Shift-click applies to descendants."));
    tree_->setRootIsDecorated(true);
    tree_->setAlternatingRowColors(true);
    tree_->setUniformRowHeights(true);
    tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tree_->setDragEnabled(true);
    tree_->setAcceptDrops(true);
    tree_->setDropIndicatorShown(true);
    tree_->setDragDropMode(QAbstractItemView::InternalMove);
    tree_->header()->setSectionResizeMode(nameColumn, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(viewportColumn,
                                          QHeaderView::ResizeToContents);
    tree_->header()->setSectionResizeMode(renderColumn,
                                          QHeaderView::ResizeToContents);
    tree_->header()->setSectionResizeMode(selectColumn,
                                          QHeaderView::ResizeToContents);
    tree_->header()->setSectionResizeMode(viewLayerViewportColumn,
                                          QHeaderView::ResizeToContents);
    tree_->header()->setSectionResizeMode(excludeColumn,
                                          QHeaderView::ResizeToContents);
    tree_->setItemDelegateForColumn(viewLayerViewportColumn,
                                    new ViewLayerCheckDelegate(tree_));
    tree_->setItemDelegateForColumn(excludeColumn,
                                    new ViewLayerCheckDelegate(tree_));
    tree_->setToolTip(QStringLiteral(
        "Scene objects and collection memberships. Click to select and activate; Ctrl-click toggles selection. The restriction columns control viewport visibility and selection."));
    layout->addWidget(tree_);
    connect(filterEdit_, &QLineEdit::textChanged, this,
            [this](const QString &text) { applyNameFilter(text); });
    connect(viewLayerCombo_, &QComboBox::activated, this, [this](int index) {
        if (index < 0 || !callbacks_.setActiveViewLayer) return;
        const QString key = viewLayerCombo_->itemData(index).toString();
        if (!key.isEmpty()) callbacks_.setActiveViewLayer(key);
    });
    connect(tree_, &QTreeWidget::itemExpanded, this,
            [this](QTreeWidgetItem *) { saveTreeState(); });
    connect(tree_, &QTreeWidget::itemCollapsed, this,
            [this](QTreeWidgetItem *) { saveTreeState(); });

    outlinerTree->setActivationHandlers(
        [this](QTreeWidgetItem *item, Qt::KeyboardModifiers modifiers) {
            return activateObjectRow(item, modifiers);
        },
        [this]() {
            if (callbacks_.setObjectSelection) {
                callbacks_.setObjectSelection({}, ObjectId::invalid());
            }
            rangeAnchorItem_ = nullptr;
        },
        [this]() { syncSelection(); },
        [this](const QVector<OutlinerRowReference> &sources,
               const OutlinerRowReference &target,
               bool link,
               bool parent) {
            if (callbacks_.dropRows) {
                callbacks_.dropRows(sources, target, link, parent);
            }
        });
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree_, &QTreeWidget::customContextMenuRequested, this,
            [this](const QPoint &position) { showContextMenu(position); });

    connect(tree_, &QTreeWidget::itemChanged, this,
            [this](QTreeWidgetItem *item, int column) {
        if (refreshing_ || item == nullptr ||
            (column != viewportColumn && column != renderColumn &&
             column != viewLayerViewportColumn &&
             column != selectColumn && column != excludeColumn)) {
            return;
        }
        const OutlinerRowKind kind = static_cast<OutlinerRowKind>(
            item->data(nameColumn, itemKindRole).toInt());
        const bool hidden = item->checkState(column) != Qt::Checked;
        const quint64 value =
            item->data(nameColumn, itemIdRole).toULongLong();
        const QString viewLayerKey = callbacks_.activeViewLayerKey
            ? callbacks_.activeViewLayerKey() : QString{};
        const QStringList sourcePath =
            item->data(nameColumn, itemSourcePathRole).toStringList();
        const bool recursive =
            static_cast<OutlinerTreeWidget *>(tree_)
                ->restrictionModifiersFor(item, column)
                .testFlag(Qt::ShiftModifier);
        // Applying a transaction notifies MainWindow synchronously, and its
        // observer rebuilds this tree. Defer the write until Qt has finished
        // dispatching itemChanged so the active QTreeWidgetItem is not deleted
        // from inside its own check-state update.
        QTimer::singleShot(0, this, [this, kind, column, hidden, value,
                                     viewLayerKey, sourcePath, recursive]() {
            if (kind == OutlinerRowKind::Collection) {
                const CollectionId id = CollectionId::fromValue(value);
                if (column == viewportColumn && callbacks_.setViewportVisibility) {
                    callbacks_.setViewportVisibility(id, hidden, recursive);
                } else if (column == renderColumn &&
                           callbacks_.setCollectionRenderVisibility) {
                    callbacks_.setCollectionRenderVisibility(id, hidden,
                                                             recursive);
                } else if (column == viewLayerViewportColumn &&
                           callbacks_.setCollectionViewLayerState) {
                    callbacks_.setCollectionViewLayerState(
                        id, viewLayerKey, sourcePath,
                        QStringLiteral("hide_viewport"), hidden, recursive);
                } else if (column == selectColumn &&
                           callbacks_.setCollectionSelectability) {
                    callbacks_.setCollectionSelectability(id, hidden,
                                                          recursive);
                } else if (column == excludeColumn &&
                           callbacks_.setCollectionViewLayerState) {
                    callbacks_.setCollectionViewLayerState(
                        id, viewLayerKey, sourcePath,
                        QStringLiteral("exclude"), hidden, recursive);
                }
            } else if (kind == OutlinerRowKind::Object) {
                const ObjectId id = ObjectId::fromValue(value);
                if (column == viewportColumn &&
                    callbacks_.setObjectViewportVisibility) {
                    callbacks_.setObjectViewportVisibility(id, hidden,
                                                           recursive);
                } else if (column == renderColumn &&
                           callbacks_.setObjectRenderVisibility) {
                    callbacks_.setObjectRenderVisibility(id, hidden,
                                                         recursive);
                } else if (column == viewLayerViewportColumn &&
                           callbacks_.setObjectViewLayerHidden) {
                    callbacks_.setObjectViewLayerHidden(id, viewLayerKey,
                                                        hidden, recursive);
                } else if (column == selectColumn &&
                           callbacks_.setObjectSelectability) {
                    callbacks_.setObjectSelectability(id, hidden, recursive);
                }
            }
            refresh();
        });
    });

}

void CollectionsPanel::applyNameFilter(const QString &text)
{
    if (activeFilterText_.isEmpty() && !text.isEmpty()) {
        expandedBeforeFilter_.clear();
        QVector<QTreeWidgetItem *> pending;
        for (int index = 0; index < tree_->topLevelItemCount(); ++index) {
            pending.append(tree_->topLevelItem(index));
        }
        while (!pending.isEmpty()) {
            QTreeWidgetItem *item = pending.takeLast();
            const QString path = item->data(nameColumn, itemPathRole).toString();
            if (!path.isEmpty()) {
                expandedBeforeFilter_.insert(path, item->isExpanded());
            }
            for (int index = 0; index < item->childCount(); ++index) {
                pending.append(item->child(index));
            }
        }
    }

    activeFilterText_ = text;
    QRegularExpression matcher;
    if (!text.isEmpty()) {
        QString glob = text;
        if (!glob.startsWith(QLatin1Char('*'))) glob.prepend(QLatin1Char('*'));
        if (!glob.endsWith(QLatin1Char('*'))) glob.append(QLatin1Char('*'));
        QString expression = QStringLiteral("^");
        for (const QChar character : glob) {
            if (character == QLatin1Char('*')) {
                expression.append(QStringLiteral(".*"));
            } else if (character == QLatin1Char('?')) {
                expression.append(QLatin1Char('.'));
            } else {
                expression.append(QRegularExpression::escape(QString(character)));
            }
        }
        expression.append(QLatin1Char('$'));
        matcher = QRegularExpression(
            expression, QRegularExpression::CaseInsensitiveOption);
    }

    const auto filterSubtree = [&](const auto &self,
                                   QTreeWidgetItem *item) -> bool {
        if (item == nullptr) return false;
        bool childMatches = false;
        for (int index = 0; index < item->childCount(); ++index) {
            childMatches = self(self, item->child(index)) || childMatches;
        }
        const bool matches = text.isEmpty() ||
            matcher.match(item->text(nameColumn)).hasMatch();
        const bool visible = matches || childMatches;
        item->setHidden(!visible);
        if (!text.isEmpty() && childMatches) item->setExpanded(true);
        return visible;
    };

    for (int index = 0; index < tree_->topLevelItemCount(); ++index) {
        filterSubtree(filterSubtree, tree_->topLevelItem(index));
    }

    if (text.isEmpty()) {
        restoringTreeState_ = true;
        QVector<QTreeWidgetItem *> pending;
        for (int index = 0; index < tree_->topLevelItemCount(); ++index) {
            pending.append(tree_->topLevelItem(index));
        }
        while (!pending.isEmpty()) {
            QTreeWidgetItem *item = pending.takeLast();
            const QString path = item->data(nameColumn, itemPathRole).toString();
            const auto saved = expandedBeforeFilter_.constFind(path);
            if (saved != expandedBeforeFilter_.cend()) {
                item->setExpanded(saved.value());
            }
            for (int index = 0; index < item->childCount(); ++index) {
                pending.append(item->child(index));
            }
        }
        restoringTreeState_ = false;
        expandedBeforeFilter_.clear();
        saveTreeState();
    }
}

void CollectionsPanel::restoreTreeState()
{
    if (tree_ == nullptr) return;
    const QString scope = callbacks_.stateScopeKey
        ? callbacks_.stateScopeKey() : QString{};
    if (scope.isEmpty()) return;
    const QString scopeHash = QString::fromLatin1(
        QCryptographicHash::hash(scope.toUtf8(), QCryptographicHash::Sha256)
            .toHex());
    QSettings settings;
    const QString settingsKey =
        QStringLiteral("outliner/%1/expandedPaths").arg(scopeHash);
    if (!settings.contains(settingsKey)) return;
    const QStringList expandedPaths =
        settings.value(settingsKey).toStringList();
    const QSet<QString> expanded(expandedPaths.cbegin(), expandedPaths.cend());

    restoringTreeState_ = true;
    QVector<QTreeWidgetItem *> pending;
    for (int index = 0; index < tree_->topLevelItemCount(); ++index) {
        pending.append(tree_->topLevelItem(index));
    }
    while (!pending.isEmpty()) {
        QTreeWidgetItem *item = pending.takeLast();
        const QString path = item->data(nameColumn, itemPathRole).toString();
        if (!path.isEmpty()) item->setExpanded(expanded.contains(path));
        for (int index = 0; index < item->childCount(); ++index) {
            pending.append(item->child(index));
        }
    }
    restoringTreeState_ = false;
}

void CollectionsPanel::saveTreeState()
{
    if (tree_ == nullptr || refreshing_ || restoringTreeState_ ||
        !activeFilterText_.isEmpty()) {
        return;
    }
    const QString scope = callbacks_.stateScopeKey
        ? callbacks_.stateScopeKey() : QString{};
    if (scope.isEmpty()) return;
    const QString scopeHash = QString::fromLatin1(
        QCryptographicHash::hash(scope.toUtf8(), QCryptographicHash::Sha256)
            .toHex());
    QStringList expandedPaths;
    QVector<QTreeWidgetItem *> pending;
    for (int index = 0; index < tree_->topLevelItemCount(); ++index) {
        pending.append(tree_->topLevelItem(index));
    }
    while (!pending.isEmpty()) {
        QTreeWidgetItem *item = pending.takeLast();
        if (item->isExpanded()) {
            const QString path = item->data(nameColumn, itemPathRole).toString();
            if (!path.isEmpty()) expandedPaths.append(path);
        }
        for (int index = 0; index < item->childCount(); ++index) {
            pending.append(item->child(index));
        }
    }
    QSettings settings;
    settings.setValue(QStringLiteral("outliner/%1/expandedPaths").arg(scopeHash),
                      expandedPaths);
}

bool CollectionsPanel::activateObjectRow(
    QTreeWidgetItem *item, Qt::KeyboardModifiers modifiers)
{
    if (refreshing_ || item == nullptr ||
        item->data(nameColumn, itemKindRole).toInt() !=
            static_cast<int>(OutlinerRowKind::Object)) return false;
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
                static_cast<int>(OutlinerRowKind::Object)) {
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
    const OutlinerRowKind kind = static_cast<OutlinerRowKind>(
        item->data(nameColumn, itemKindRole).toInt());
    QMenu menu(this);

    if (kind == OutlinerRowKind::SceneRoot && callbacks_.createCollection) {
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
    } else if (kind == OutlinerRowKind::Collection) {
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
                        static_cast<int>(OutlinerRowKind::Object)) {
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
    } else if (kind == OutlinerRowKind::Object) {
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
        const CollectionId linkedCollectionId = CollectionId::fromValue(
            item->data(nameColumn, itemLinkedCollectionRole).toULongLong());
        if (callbacks_.unlinkObject && linkedCollectionId.isValid()) {
            QAction *unlink = menu.addAction(
                QStringLiteral("Unlink from Collection"));
            connect(unlink, &QAction::triggered, this,
                    [this, objectId, linkedCollectionId]() {
                callbacks_.unlinkObject(objectId, linkedCollectionId);
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
    QStringList path,
    QStringList sourcePath,
    const QString &activeViewLayerKey)
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
                  static_cast<int>(OutlinerRowKind::Collection));
    item->setData(nameColumn, itemIdRole,
                  QVariant::fromValue<qulonglong>(entry.collection.id.value()));
    item->setData(nameColumn, itemParentCollectionRole,
                  QVariant::fromValue<qulonglong>(
                      parent != nullptr &&
                              parent->data(nameColumn, itemKindRole).toInt() ==
                                  static_cast<int>(OutlinerRowKind::Collection)
                          ? parent->data(nameColumn, itemIdRole).toULongLong()
                          : 0));
    path.append(QStringLiteral("c%1").arg(entry.collection.id.value()));
    item->setData(nameColumn, itemPathRole, path.join(QLatin1Char('/')));
    sourcePath.append(entry.collection.sourceId);
    item->setData(nameColumn, itemSourcePathRole,
                  QVariant::fromValue(sourcePath));
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable |
                   Qt::ItemIsSelectable | Qt::ItemIsDragEnabled |
                   Qt::ItemIsDropEnabled);
    item->setCheckState(viewportColumn, entry.collection.hideViewport
                               ? Qt::Unchecked : Qt::Checked);
    item->setCheckState(renderColumn, entry.collection.hideRender
                             ? Qt::Unchecked : Qt::Checked);
    item->setCheckState(selectColumn, entry.collection.hideSelect
                               ? Qt::Unchecked : Qt::Checked);
    const QJsonObject layerState = viewLayerState(
        entry.collection, activeViewLayerKey, sourcePath);
    if (!layerState.isEmpty()) {
        item->setData(viewLayerViewportColumn, itemViewLayerEditableRole, true);
        item->setData(excludeColumn, itemViewLayerEditableRole, true);
        item->setCheckState(viewLayerViewportColumn,
            layerState.value(QStringLiteral("hide_viewport")).toBool()
                ? Qt::Unchecked : Qt::Checked);
        item->setCheckState(excludeColumn,
            layerState.value(QStringLiteral("exclude")).toBool()
                ? Qt::Unchecked : Qt::Checked);
    }

    ancestors.insert(entry.collection.id.value());
    for (const CollectionPanelEntry &candidate : entries) {
        if (candidate.collection.parentIds.contains(entry.collection.id)) {
            appendCollection(tree, candidate, entries, objects, item, ancestors,
                             path, sourcePath, activeViewLayerKey);
        }
    }
    for (const OutlinerObjectEntry &object : objects) {
        if (object.collectionIds.contains(entry.collection.id)) {
            appendObject(item, object, path, entry.collection.id,
                         activeViewLayerKey);
        }
    }
}

QTreeWidgetItem *CollectionsPanel::appendObject(
    QTreeWidgetItem *parent,
    const OutlinerObjectEntry &entry,
    const QStringList &path,
    CollectionId linkedCollectionId,
    const QString &activeViewLayerKey)
{
    if (parent == nullptr || !entry.id.isValid()) return nullptr;
    auto *item = new QTreeWidgetItem(parent);
    item->setText(nameColumn, entry.name);
    item->setData(nameColumn, itemKindRole,
                  static_cast<int>(OutlinerRowKind::Object));
    item->setData(nameColumn, itemIdRole,
                  QVariant::fromValue<qulonglong>(entry.id.value()));
    item->setData(nameColumn, itemLinkedCollectionRole,
                  QVariant::fromValue<qulonglong>(
                      linkedCollectionId.isValid()
                          ? linkedCollectionId.value() : 0));
    QStringList objectPath = path;
    objectPath.append(QStringLiteral("o%1").arg(entry.id.value()));
    item->setData(nameColumn, itemPathRole,
                  objectPath.join(QLatin1Char('/')));
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable |
                   Qt::ItemIsSelectable | Qt::ItemIsDragEnabled |
                   Qt::ItemIsDropEnabled);
    item->setCheckState(viewportColumn, entry.hideViewport
                               ? Qt::Unchecked : Qt::Checked);
    item->setCheckState(renderColumn, entry.hideRender
                               ? Qt::Unchecked : Qt::Checked);
    item->setCheckState(selectColumn, entry.hideSelect
                               ? Qt::Unchecked : Qt::Checked);
    if (!activeViewLayerKey.isEmpty()) {
        item->setData(viewLayerViewportColumn, itemViewLayerEditableRole, true);
        item->setCheckState(viewLayerViewportColumn,
            entry.viewLayerHidden.value(activeViewLayerKey).toBool()
                ? Qt::Unchecked : Qt::Checked);
    }
    objectRows_[entry.id.value()].append(item);
    objectEntries_.insert(entry.id.value(), entry);
    return item;
}

void CollectionsPanel::buildObjectParentHierarchy(
    const QVector<OutlinerObjectEntry> &objects)
{
    QSet<quint64> visited;
    QSet<quint64> visiting;
    QSet<quint64> cyclicParentLinks;
    QVector<ObjectId> parentBeforeChild;
    const auto appendWithParents = [&](const auto &self,
                                       ObjectId objectId) -> void {
        if (!objectId.isValid() || visited.contains(objectId.value())) return;
        const OutlinerObjectEntry entry =
            objectEntries_.value(objectId.value());
        if (!entry.id.isValid()) return;
        visiting.insert(objectId.value());
        const ObjectId parentId = entry.parentObjectId;
        if (parentId.isValid() && objectEntries_.contains(parentId.value())) {
            if (visiting.contains(parentId.value())) {
                cyclicParentLinks.insert(objectId.value());
            } else {
                self(self, parentId);
            }
        }
        visiting.remove(objectId.value());
        visited.insert(objectId.value());
        parentBeforeChild.append(objectId);
    };
    for (const OutlinerObjectEntry &entry : objects) {
        appendWithParents(appendWithParents, entry.id);
    }

    const QString activeViewLayerKey = callbacks_.activeViewLayerKey
        ? callbacks_.activeViewLayerKey() : QString{};
    for (const ObjectId childId : parentBeforeChild) {
        const OutlinerObjectEntry childEntry =
            objectEntries_.value(childId.value());
        const ObjectId parentId = cyclicParentLinks.contains(childId.value())
            ? ObjectId::invalid() : childEntry.parentObjectId;
        if (!parentId.isValid()) continue;

        const QVector<QTreeWidgetItem *> parentRows =
            objectRows_.value(parentId.value());
        const QVector<QTreeWidgetItem *> childRows =
            objectRows_.value(childId.value());
        for (QTreeWidgetItem *parentRow : parentRows) {
            if (parentRow == nullptr) continue;
            QTreeWidgetItem *collectionRow = parentRow->parent();
            while (collectionRow != nullptr &&
                   collectionRow->data(nameColumn, itemKindRole).toInt() ==
                       static_cast<int>(OutlinerRowKind::Object)) {
                collectionRow = collectionRow->parent();
            }
            if (collectionRow == nullptr) continue;

            QTreeWidgetItem *matchingChildRow = nullptr;
            for (QTreeWidgetItem *childRow : childRows) {
                if (childRow != nullptr && childRow != parentRow &&
                    childRow->parent() == collectionRow) {
                    matchingChildRow = childRow;
                    break;
                }
            }
            if (matchingChildRow != nullptr) {
                const int childIndex =
                    collectionRow->indexOfChild(matchingChildRow);
                if (childIndex >= 0) {
                    collectionRow->takeChild(childIndex);
                    parentRow->addChild(matchingChildRow);
                }
                continue;
            }

            const QStringList parentPath = parentRow
                ->data(nameColumn, itemPathRole).toString()
                .split(QLatin1Char('/'), Qt::SkipEmptyParts);
            QTreeWidgetItem *notInCollectionRow = appendObject(
                parentRow, childEntry, parentPath,
                CollectionId::invalid(), activeViewLayerKey);
            if (notInCollectionRow != nullptr) {
                notInCollectionRow->setData(nameColumn,
                                            itemNotInCollectionRole, true);
                notInCollectionRow->setToolTip(
                    nameColumn,
                    QStringLiteral("Parented object; not linked to this collection"));
                QFont font = notInCollectionRow->font(nameColumn);
                font.setItalic(true);
                notInCollectionRow->setFont(nameColumn, font);
            }
        }
    }
}

void CollectionsPanel::refresh()
{
    if (tree_ == nullptr || !callbacks_.entries) return;
    const QString stateScopeKey = callbacks_.stateScopeKey
        ? callbacks_.stateScopeKey() : QString{};
    const bool stateScopeChanged = stateScopeKey != currentStateScopeKey_;
    currentStateScopeKey_ = stateScopeKey;
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
    objectRows_.clear();
    objectEntries_.clear();
    const QVector<CollectionPanelEntry> entries = callbacks_.entries();
    const QVector<OutlinerObjectEntry> objects = callbacks_.objects
        ? callbacks_.objects() : QVector<OutlinerObjectEntry>{};
    const QVector<OutlinerViewLayerEntry> viewLayers = callbacks_.viewLayers
        ? callbacks_.viewLayers() : QVector<OutlinerViewLayerEntry>{};
    const QString activeViewLayerKey = callbacks_.activeViewLayerKey
        ? callbacks_.activeViewLayerKey() : QString{};
    {
        const QSignalBlocker blocker(viewLayerCombo_);
        viewLayerCombo_->clear();
        int activeIndex = -1;
        for (const OutlinerViewLayerEntry &viewLayer : viewLayers) {
            viewLayerCombo_->addItem(viewLayer.displayName, viewLayer.key);
            if (viewLayer.key == activeViewLayerKey) {
                activeIndex = viewLayerCombo_->count() - 1;
            }
        }
        viewLayerCombo_->setCurrentIndex(activeIndex);
        viewLayerCombo_->setVisible(viewLayers.size() > 1);
    }
    tree_->setColumnHidden(viewLayerViewportColumn,
                           activeViewLayerKey.isEmpty());
    tree_->setColumnHidden(excludeColumn, activeViewLayerKey.isEmpty());
    auto *sceneRoot = new QTreeWidgetItem(tree_);
    sceneRoot->setText(nameColumn, QStringLiteral("Scene Collection"));
    sceneRoot->setData(nameColumn, itemKindRole,
                       static_cast<int>(OutlinerRowKind::SceneRoot));
    sceneRoot->setData(nameColumn, itemPathRole, QStringLiteral("scene"));
    sceneRoot->setFlags((sceneRoot->flags() & ~Qt::ItemIsSelectable) |
                        Qt::ItemIsDropEnabled);
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
                             {QStringLiteral("scene")}, {},
                             activeViewLayerKey);
        }
    }
    for (const OutlinerObjectEntry &object : objects) {
        if (object.collectionIds.isEmpty()) {
            appendObject(sceneRoot, object,
                         {QStringLiteral("scene"), QStringLiteral("root")},
                         CollectionId::invalid(), activeViewLayerKey);
        }
    }
    buildObjectParentHierarchy(objects);
    if (!hasRefreshed_ || stateScopeChanged) {
        tree_->expandAll();
        for (const QVector<QTreeWidgetItem *> &rows : objectRows_) {
            for (QTreeWidgetItem *row : rows) {
                if (row != nullptr && row->childCount() > 0) {
                    row->setExpanded(false);
                }
            }
        }
        restoreTreeState();
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
                       static_cast<int>(OutlinerRowKind::Collection)) {
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
    if (filterEdit_ != nullptr && !filterEdit_->text().isEmpty()) {
        applyNameFilter(filterEdit_->text());
    }
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
                static_cast<int>(OutlinerRowKind::Object)) {
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
