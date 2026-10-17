/* SPDX-FileCopyrightText: 2008 Blender Authors
 * SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The view-layer tree ordering, repeated collection-membership rows, and
 * select/activate, Ctrl-toggle, and range-selection rules are adapted from
 * Blender 5.2.2's tree_display_view_layer.cc and outliner_select.cc.
 * Collection and object drop outcomes are adapted from outliner_dragdrop.cc.
 * Shift-recursive restriction changes are adapted from outliner_draw.cc.
 * Name, type, and state filtering is adapted from outliner_tree.cc and
 * space_outliner.py.
 * Active-row scrolling is adapted from outliner_draw.cc and outliner_edit.cc.
 * The Scenes tree mode is adapted from tree_display_scenes.cc and the
 * scene/object tree elements in tree_element_id_scene.cc and
 * tree_element_scene_objects.cc.
 * Generic object contents follow tree_element_id_object.cc, with Animation
 * rows following tree_element_anim_data.cc, tree_element_driver.cc, and
 * tree_element_nla.cc.
 */
#include "collections_panel.h"

#include <QAbstractItemView>
#include <QAbstractItemModel>
#include <QAction>
#include <QActionGroup>
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
#include <QToolButton>
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
constexpr int itemViewLayerKeyRole = Qt::UserRole + 9;
constexpr int itemSceneKeyRole = Qt::UserRole + 10;
constexpr int nameColumn = 0;
constexpr int viewportColumn = 1;
constexpr int renderColumn = 2;
constexpr int viewLayerViewportColumn = 3;
constexpr int holdoutColumn = 4;
constexpr int indirectOnlyColumn = 5;
constexpr int selectColumn = 6;
constexpr int excludeColumn = 7;

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
            if (reference.kind != OutlinerRowKind::Object &&
                reference.kind != OutlinerRowKind::Collection) return;
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
        if (target.kind == OutlinerRowKind::ObjectContent) {
            event->ignore();
            return;
        }
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
        "Filter Outliner names with * and ? wildcards. Exact and case-sensitive matching are in Filter."));
    auto *filterRow = new QHBoxLayout;
    filterRow->setContentsMargins(0, 0, 0, 0);
    filterRow->addWidget(filterEdit_, 1);
    filterButton_ = new QToolButton(this);
    filterButton_->setText(QStringLiteral("Filter"));
    filterButton_->setToolTip(QStringLiteral(
        "Filter collections and objects by name, type, and state."));
    filterButton_->setPopupMode(QToolButton::InstantPopup);
    filterRow->addWidget(filterButton_);
    displayModeCombo_ = new QComboBox(this);
    displayModeCombo_->addItem(QStringLiteral("View Layer"));
    displayModeCombo_->addItem(QStringLiteral("Scenes"));
    displayModeCombo_->setToolTip(QStringLiteral(
        "Choose the Blender Outliner tree display."));
    filterRow->addWidget(displayModeCombo_);
    sceneCombo_ = new QComboBox(this);
    sceneCombo_->setToolTip(QStringLiteral("Active Blender scene"));
    filterRow->addWidget(sceneCombo_);
    viewLayerCombo_ = new QComboBox(this);
    viewLayerCombo_->setToolTip(QStringLiteral("Active Blender view layer"));
    filterRow->addWidget(viewLayerCombo_);
    layout->addLayout(filterRow);
    auto *outlinerTree = new OutlinerTreeWidget(this);
    tree_ = outlinerTree;
    tree_->setColumnCount(8);
    tree_->setHeaderLabels({QStringLiteral("Name"),
                            QStringLiteral("Viewport"),
                            QStringLiteral("Render"),
                            QStringLiteral("Layer View"),
                            QStringLiteral("Holdout"),
                            QStringLiteral("Indirect Only"),
                            QStringLiteral("Select"),
                            QStringLiteral("Exclude")});
    tree_->headerItem()->setToolTip(viewportColumn,
                                    QStringLiteral("Hide globally in the viewport. Shift-click applies to descendants; Ctrl-click on a collection isolates it."));
    tree_->headerItem()->setToolTip(renderColumn,
                                    QStringLiteral("Hide globally in renders. Shift-click applies to descendants; Ctrl-click on a collection isolates it."));
    tree_->headerItem()->setToolTip(viewLayerViewportColumn,
                                    QStringLiteral("Hide in the active Blender view layer. Shift-click applies to descendants; Ctrl-click on a collection isolates it."));
    tree_->headerItem()->setToolTip(holdoutColumn,
                                    QStringLiteral("Make this collection a holdout in the active view layer. Shift-click applies to descendants; Ctrl-click isolates it."));
    tree_->headerItem()->setToolTip(indirectOnlyColumn,
                                    QStringLiteral("Cycles only: make this collection contribute indirectly. Shift-click applies to descendants; Ctrl-click isolates it."));
    tree_->headerItem()->setToolTip(selectColumn,
                                    QStringLiteral("Prevent selection. Shift-click applies to descendants; Ctrl-click on a collection isolates it."));
    tree_->headerItem()->setToolTip(excludeColumn,
                                    QStringLiteral("Exclude this collection path from the active view layer. Shift-click applies to descendants; Ctrl-click isolates this path."));
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
    tree_->header()->setSectionResizeMode(holdoutColumn,
                                          QHeaderView::ResizeToContents);
    tree_->header()->setSectionResizeMode(indirectOnlyColumn,
                                          QHeaderView::ResizeToContents);
    tree_->header()->setSectionResizeMode(excludeColumn,
                                          QHeaderView::ResizeToContents);
    tree_->setItemDelegateForColumn(viewLayerViewportColumn,
                                    new ViewLayerCheckDelegate(tree_));
    tree_->setItemDelegateForColumn(holdoutColumn,
                                    new ViewLayerCheckDelegate(tree_));
    tree_->setItemDelegateForColumn(indirectOnlyColumn,
                                    new ViewLayerCheckDelegate(tree_));
    tree_->setItemDelegateForColumn(excludeColumn,
                                    new ViewLayerCheckDelegate(tree_));
    QSettings outlinerSettings;
    showEnableColumn_ = outlinerSettings.value(
        QStringLiteral("outliner/showEnableColumn"), true).toBool();
    showSelectColumn_ = outlinerSettings.value(
        QStringLiteral("outliner/showSelectColumn"), false).toBool();
    showHideColumn_ = outlinerSettings.value(
        QStringLiteral("outliner/showHideColumn"), true).toBool();
    showViewportColumn_ = outlinerSettings.value(
        QStringLiteral("outliner/showViewportColumn"), false).toBool();
    showRenderColumn_ = outlinerSettings.value(
        QStringLiteral("outliner/showRenderColumn"), true).toBool();
    showHoldoutColumn_ = outlinerSettings.value(
        QStringLiteral("outliner/showHoldoutColumn"), false).toBool();
    showIndirectOnlyColumn_ = outlinerSettings.value(
        QStringLiteral("outliner/showIndirectOnlyColumn"), false).toBool();
    showCollections_ = outlinerSettings.value(
        QStringLiteral("outliner/filterCollections"), true).toBool();
    showObjects_ = outlinerSettings.value(
        QStringLiteral("outliner/filterObjects"), true).toBool();
    showObjectContents_ = outlinerSettings.value(
        QStringLiteral("outliner/filterObjectContents"), true).toBool();
    showObjectChildren_ = outlinerSettings.value(
        QStringLiteral("outliner/filterObjectChildren"), true).toBool();
    showAllViewLayers_ = outlinerSettings.value(
        QStringLiteral("outliner/filterAllViewLayers"), false).toBool();
    scrollToActive_ = outlinerSettings.value(
        QStringLiteral("outliner/scrollToActive"), true).toBool();
    scenesDisplayMode_ = outlinerSettings.value(
        QStringLiteral("outliner/displayMode"),
        QStringLiteral("VIEW_LAYER")).toString() ==
        QLatin1String("SCENES");
    displayModeCombo_->setCurrentIndex(scenesDisplayMode_ ? 1 : 0);
    objectStateFilter_ = static_cast<OutlinerObjectStateFilter>(
        qBound(0, outlinerSettings.value(
                    QStringLiteral("outliner/filterObjectState"), 0).toInt(),
               static_cast<int>(OutlinerObjectStateFilter::Selectable)));
    invertObjectStateFilter_ = outlinerSettings.value(
        QStringLiteral("outliner/filterInvertState"), false).toBool();
    exactNameMatch_ = outlinerSettings.value(
        QStringLiteral("outliner/filterExactName"), false).toBool();
    caseSensitiveNameFilter_ = outlinerSettings.value(
        QStringLiteral("outliner/filterCaseSensitive"), false).toBool();
    const QVector<QPair<QString, QString>> objectTypes{
        {QStringLiteral("MESH"), QStringLiteral("Meshes")},
        {QStringLiteral("ARMATURE"), QStringLiteral("Armatures")},
        {QStringLiteral("LIGHT"), QStringLiteral("Lights")},
        {QStringLiteral("CAMERA"), QStringLiteral("Cameras")},
        {QStringLiteral("GREASE_PENCIL"), QStringLiteral("Grease Pencil")},
        {QStringLiteral("EMPTY"), QStringLiteral("Empties")},
        {QStringLiteral("OTHER"), QStringLiteral("Others")}};
    for (const auto &type : objectTypes) {
        objectTypeFilters_.insert(
            type.first,
            outlinerSettings.value(
                QStringLiteral("outliner/filterObjectType/%1").arg(type.first),
                true).toBool());
    }

    auto *filterMenu = new QMenu(filterButton_);
    filterButton_->setMenu(filterMenu);
    QAction *allViewLayersAction = filterMenu->addAction(
        QStringLiteral("All View Layers"));
    allViewLayersAction->setCheckable(true);
    allViewLayersAction->setChecked(showAllViewLayers_);
    allViewLayersAction->setEnabled(!scenesDisplayMode_);
    connect(allViewLayersAction, &QAction::toggled, this,
            [this](bool visible) {
        showAllViewLayers_ = visible;
        QSettings settings;
        settings.setValue(QStringLiteral("outliner/filterAllViewLayers"),
                          visible);
        refresh();
    });
    connect(displayModeCombo_, &QComboBox::activated, this,
            [this, allViewLayersAction](int index) {
        scenesDisplayMode_ = index == 1;
        QSettings settings;
        settings.setValue(QStringLiteral("outliner/displayMode"),
                          scenesDisplayMode_ ? QStringLiteral("SCENES")
                                             : QStringLiteral("VIEW_LAYER"));
        allViewLayersAction->setEnabled(!scenesDisplayMode_);
        refresh();
    });
    QAction *collectionsAction = filterMenu->addAction(
        QStringLiteral("Collections"));
    collectionsAction->setCheckable(true);
    collectionsAction->setChecked(showCollections_);
    connect(collectionsAction, &QAction::toggled, this, [this](bool visible) {
        showCollections_ = visible;
        QSettings settings;
        settings.setValue(QStringLiteral("outliner/filterCollections"), visible);
        refresh();
    });
    QAction *objectsAction = filterMenu->addAction(QStringLiteral("Objects"));
    objectsAction->setCheckable(true);
    objectsAction->setChecked(showObjects_);
    QAction *contentsAction = filterMenu->addAction(
        QStringLiteral("Object Contents"));
    contentsAction->setCheckable(true);
    contentsAction->setChecked(showObjectContents_);
    contentsAction->setEnabled(showObjects_);
    connect(contentsAction, &QAction::toggled, this, [this](bool visible) {
        showObjectContents_ = visible;
        QSettings settings;
        settings.setValue(QStringLiteral("outliner/filterObjectContents"),
                          visible);
        refresh();
    });
    connect(objectsAction, &QAction::toggled, this,
            [this, contentsAction](bool visible) {
        showObjects_ = visible;
        contentsAction->setEnabled(visible);
        QSettings settings;
        settings.setValue(QStringLiteral("outliner/filterObjects"), visible);
        refresh();
    });
    QAction *childrenAction = filterMenu->addAction(
        QStringLiteral("Object Children"));
    childrenAction->setCheckable(true);
    childrenAction->setChecked(showObjectChildren_);
    connect(childrenAction, &QAction::toggled, this, [this](bool visible) {
        showObjectChildren_ = visible;
        QSettings settings;
        settings.setValue(QStringLiteral("outliner/filterObjectChildren"),
                          visible);
        refresh();
    });
    filterMenu->addSeparator();
    QAction *scrollToActiveAction = filterMenu->addAction(
        QStringLiteral("Scroll to Active"));
    scrollToActiveAction->setCheckable(true);
    scrollToActiveAction->setChecked(scrollToActive_);
    connect(scrollToActiveAction, &QAction::toggled, this,
            [this](bool enabled) {
        scrollToActive_ = enabled;
        QSettings settings;
        settings.setValue(QStringLiteral("outliner/scrollToActive"), enabled);
        syncSelection();
    });
    filterMenu->addSeparator();
    filterMenu->addSection(QStringLiteral("Object State"));
    auto *stateGroup = new QActionGroup(filterMenu);
    stateGroup->setExclusive(true);
    const QVector<QPair<OutlinerObjectStateFilter, QString>> states{
        {OutlinerObjectStateFilter::All, QStringLiteral("All")},
        {OutlinerObjectStateFilter::Visible, QStringLiteral("Visible")},
        {OutlinerObjectStateFilter::Hidden, QStringLiteral("Hidden")},
        {OutlinerObjectStateFilter::Selected, QStringLiteral("Selected")},
        {OutlinerObjectStateFilter::Active, QStringLiteral("Active")},
        {OutlinerObjectStateFilter::Selectable, QStringLiteral("Selectable")}};
    for (const auto &state : states) {
        QAction *action = filterMenu->addAction(state.second);
        action->setCheckable(true);
        action->setData(static_cast<int>(state.first));
        action->setChecked(objectStateFilter_ == state.first);
        stateGroup->addAction(action);
    }
    connect(stateGroup, &QActionGroup::triggered, this,
            [this](QAction *action) {
        objectStateFilter_ = static_cast<OutlinerObjectStateFilter>(
            action->data().toInt());
        QSettings settings;
        settings.setValue(QStringLiteral("outliner/filterObjectState"),
                          static_cast<int>(objectStateFilter_));
        applyNameFilter(filterEdit_->text());
    });
    QAction *invertStateAction = filterMenu->addAction(
        QStringLiteral("Invert State Filter"));
    invertStateAction->setCheckable(true);
    invertStateAction->setChecked(invertObjectStateFilter_);
    invertStateAction->setEnabled(
        objectStateFilter_ != OutlinerObjectStateFilter::All);
    connect(invertStateAction, &QAction::toggled, this, [this](bool invert) {
        invertObjectStateFilter_ = invert;
        QSettings settings;
        settings.setValue(QStringLiteral("outliner/filterInvertState"),
                          invert);
        applyNameFilter(filterEdit_->text());
    });
    filterMenu->addSeparator();
    filterMenu->addSection(QStringLiteral("Object Types"));
    for (const auto &type : objectTypes) {
        QAction *action = filterMenu->addAction(type.second);
        action->setCheckable(true);
        action->setChecked(objectTypeFilters_.value(type.first, true));
        connect(action, &QAction::toggled, this,
                [this, key = type.first](bool enabled) {
            objectTypeFilters_.insert(key, enabled);
            QSettings settings;
            settings.setValue(
                QStringLiteral("outliner/filterObjectType/%1").arg(key),
                enabled);
            applyNameFilter(filterEdit_->text());
        });
    }
    filterMenu->addSeparator();
    QAction *exactMatchAction = filterMenu->addAction(
        QStringLiteral("Exact Name Match"));
    exactMatchAction->setCheckable(true);
    exactMatchAction->setChecked(exactNameMatch_);
    connect(exactMatchAction, &QAction::toggled, this,
            [this](bool enabled) {
        exactNameMatch_ = enabled;
        QSettings settings;
        settings.setValue(QStringLiteral("outliner/filterExactName"), enabled);
        applyNameFilter(filterEdit_->text());
    });
    QAction *caseSensitiveAction = filterMenu->addAction(
        QStringLiteral("Case Sensitive"));
    caseSensitiveAction->setCheckable(true);
    caseSensitiveAction->setChecked(caseSensitiveNameFilter_);
    connect(caseSensitiveAction, &QAction::toggled, this,
            [this](bool enabled) {
        caseSensitiveNameFilter_ = enabled;
        QSettings settings;
        settings.setValue(QStringLiteral("outliner/filterCaseSensitive"),
                          enabled);
        applyNameFilter(filterEdit_->text());
    });
    connect(stateGroup, &QActionGroup::triggered, this,
            [invertStateAction](QAction *action) {
        invertStateAction->setEnabled(
            action->data().toInt() !=
            static_cast<int>(OutlinerObjectStateFilter::All));
    });
    tree_->header()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree_->header(), &QHeaderView::customContextMenuRequested,
            this, [this](const QPoint &position) {
        QMenu menu(this);
        menu.addSection(QStringLiteral("Restriction Toggles"));
        const QString activeViewLayerKey = callbacks_.activeViewLayerKey
            ? callbacks_.activeViewLayerKey() : QString{};
        const auto addColumnToggle =
            [this, &menu, &activeViewLayerKey](
                const QString &label, bool checked, int column,
                const QString &settingsKey, bool viewLayerSpecific) {
            QAction *action = menu.addAction(label);
            action->setCheckable(true);
            action->setChecked(checked);
            if (viewLayerSpecific && activeViewLayerKey.isEmpty()) {
                action->setEnabled(false);
            }
            connect(action, &QAction::toggled, this,
                    [this, column, settingsKey, viewLayerSpecific](bool visible) {
                switch (column) {
                case viewportColumn: showViewportColumn_ = visible; break;
                case renderColumn: showRenderColumn_ = visible; break;
                case viewLayerViewportColumn: showHideColumn_ = visible; break;
                case holdoutColumn: showHoldoutColumn_ = visible; break;
                case indirectOnlyColumn:
                    showIndirectOnlyColumn_ = visible;
                    break;
                case selectColumn: showSelectColumn_ = visible; break;
                case excludeColumn: showEnableColumn_ = visible; break;
                default: break;
                }
                QSettings settings;
                settings.setValue(settingsKey, visible);
                const bool unavailable = viewLayerSpecific &&
                    (!callbacks_.activeViewLayerKey ||
                     callbacks_.activeViewLayerKey().isEmpty());
                tree_->setColumnHidden(column, !visible || unavailable);
            });
        };
        addColumnToggle(QStringLiteral("Enable / Exclude"),
                        showEnableColumn_, excludeColumn,
                        QStringLiteral("outliner/showEnableColumn"), true);
        addColumnToggle(QStringLiteral("Select"), showSelectColumn_,
                        selectColumn,
                        QStringLiteral("outliner/showSelectColumn"), false);
        addColumnToggle(QStringLiteral("Hide in View Layer"),
                        showHideColumn_, viewLayerViewportColumn,
                        QStringLiteral("outliner/showHideColumn"), true);
        addColumnToggle(QStringLiteral("Viewport"), showViewportColumn_,
                        viewportColumn,
                        QStringLiteral("outliner/showViewportColumn"), false);
        addColumnToggle(QStringLiteral("Render"), showRenderColumn_,
                        renderColumn,
                        QStringLiteral("outliner/showRenderColumn"), false);
        addColumnToggle(QStringLiteral("Holdout"), showHoldoutColumn_,
                        holdoutColumn,
                        QStringLiteral("outliner/showHoldoutColumn"), true);
        addColumnToggle(QStringLiteral("Indirect Only (Cycles)"),
                        showIndirectOnlyColumn_, indirectOnlyColumn,
                        QStringLiteral("outliner/showIndirectOnlyColumn"), true);
        menu.exec(tree_->header()->mapToGlobal(position));
    });
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
    connect(sceneCombo_, &QComboBox::activated, this, [this](int index) {
        if (index < 0 || !callbacks_.setActiveScene) return;
        const QString key = sceneCombo_->itemData(index).toString();
        if (!key.isEmpty()) callbacks_.setActiveScene(key);
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
             column != holdoutColumn && column != indirectOnlyColumn &&
             column != selectColumn && column != excludeColumn)) {
            return;
        }
        const OutlinerRowKind kind = static_cast<OutlinerRowKind>(
            item->data(nameColumn, itemKindRole).toInt());
        const bool hidden = item->checkState(column) != Qt::Checked;
        const QString stateKey = column == viewLayerViewportColumn
            ? QStringLiteral("hide_viewport")
            : column == excludeColumn
                ? QStringLiteral("exclude")
                : column == holdoutColumn
                    ? QStringLiteral("holdout")
                    : QStringLiteral("indirect_only");
        const bool stateValue =
            column == holdoutColumn || column == indirectOnlyColumn
                ? !hidden : hidden;
        const quint64 value =
            item->data(nameColumn, itemIdRole).toULongLong();
        QString viewLayerKey = item->data(
            nameColumn, itemViewLayerKeyRole).toString();
        if (viewLayerKey.isEmpty() && callbacks_.activeViewLayerKey) {
            viewLayerKey = callbacks_.activeViewLayerKey();
        }
        const QStringList sourcePath =
            item->data(nameColumn, itemSourcePathRole).toStringList();
        const Qt::KeyboardModifiers modifiers =
            static_cast<OutlinerTreeWidget *>(tree_)
                ->restrictionModifiersFor(item, column);
        const bool recursive = modifiers.testFlag(Qt::ShiftModifier);
        const bool isolate = kind == OutlinerRowKind::Collection &&
            !recursive && modifiers.testFlag(Qt::ControlModifier);
        // Applying a transaction notifies MainWindow synchronously, and its
        // observer rebuilds this tree. Defer the write until Qt has finished
        // dispatching itemChanged so the active QTreeWidgetItem is not deleted
        // from inside its own check-state update.
        QTimer::singleShot(0, this, [this, kind, column, hidden, value,
                                     stateKey, stateValue, viewLayerKey,
                                     sourcePath, recursive, isolate]() {
            if (kind == OutlinerRowKind::Collection) {
                const CollectionId id = CollectionId::fromValue(value);
                if (column == viewportColumn && callbacks_.setViewportVisibility) {
                    callbacks_.setViewportVisibility(id, hidden, recursive,
                                                     isolate);
                } else if (column == renderColumn &&
                           callbacks_.setCollectionRenderVisibility) {
                    callbacks_.setCollectionRenderVisibility(id, hidden,
                                                             recursive,
                                                             isolate);
                } else if ((column == viewLayerViewportColumn ||
                            column == excludeColumn ||
                            column == holdoutColumn ||
                            column == indirectOnlyColumn) &&
                           callbacks_.setCollectionViewLayerState) {
                    callbacks_.setCollectionViewLayerState(
                        id, viewLayerKey, sourcePath, stateKey, stateValue,
                        recursive, isolate);
                } else if (column == selectColumn &&
                           callbacks_.setCollectionSelectability) {
                    callbacks_.setCollectionSelectability(id, hidden,
                                                          recursive, isolate);
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
        if (!exactNameMatch_) {
            if (!glob.startsWith(QLatin1Char('*')))
                glob.prepend(QLatin1Char('*'));
            if (!glob.endsWith(QLatin1Char('*')))
                glob.append(QLatin1Char('*'));
        }
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
            expression,
            caseSensitiveNameFilter_
                ? QRegularExpression::NoPatternOption
                : QRegularExpression::CaseInsensitiveOption);
    }

    QSet<quint64> selectedValues;
    if (callbacks_.selectedObjectIds) {
        for (const ObjectId id : callbacks_.selectedObjectIds()) {
            if (id.isValid()) selectedValues.insert(id.value());
        }
    }
    const ObjectId activeId = callbacks_.activeObjectId
        ? callbacks_.activeObjectId() : ObjectId::invalid();
    const QString activeViewLayerKey = callbacks_.activeViewLayerKey
        ? callbacks_.activeViewLayerKey() : QString{};
    const auto objectMatchesFilter = [this, &selectedValues, activeId,
                                      activeViewLayerKey](
                                         const QTreeWidgetItem *item) {
        if (item == nullptr) return false;
        const quint64 objectValue = item->data(
            nameColumn, itemIdRole).toULongLong();
        const OutlinerObjectEntry entry = objectEntries_.value(objectValue);
        if (!entry.id.isValid()) return false;
        const QString sourceType = entry.blenderType.toUpper();
        QString typeKey = QStringLiteral("OTHER");
        if (sourceType == QLatin1String("MESH")) {
            typeKey = QStringLiteral("MESH");
        } else if (sourceType == QLatin1String("ARMATURE")) {
            typeKey = QStringLiteral("ARMATURE");
        } else if (sourceType == QLatin1String("LIGHT")) {
            typeKey = QStringLiteral("LIGHT");
        } else if (sourceType == QLatin1String("CAMERA")) {
            typeKey = QStringLiteral("CAMERA");
        } else if (sourceType == QLatin1String("GREASE_PENCIL") ||
                   sourceType == QLatin1String("GPENCIL")) {
            typeKey = QStringLiteral("GREASE_PENCIL");
        } else if (sourceType == QLatin1String("EMPTY")) {
            typeKey = QStringLiteral("EMPTY");
        }
        if (!objectTypeFilters_.value(typeKey, true)) return false;

        const QString rowViewLayerKey = item->data(
            nameColumn, itemViewLayerKeyRole).toString();
        bool visible = entry.visible;
        bool selectable = entry.selectable;
        const bool hasLayerVisibilityCallback = !rowViewLayerKey.isEmpty() &&
            static_cast<bool>(callbacks_.objectVisibleInViewLayer);
        const bool hasLayerSelectableCallback = !rowViewLayerKey.isEmpty() &&
            static_cast<bool>(callbacks_.objectSelectableInViewLayer);
        if (hasLayerVisibilityCallback) {
            visible = callbacks_.objectVisibleInViewLayer(
                entry.id, rowViewLayerKey);
        } else if (!rowViewLayerKey.isEmpty() &&
                   rowViewLayerKey != activeViewLayerKey) {
            visible = !entry.hideViewport &&
                !entry.viewLayerHidden.value(rowViewLayerKey).toBool();
        }
        if (hasLayerSelectableCallback) {
            selectable = callbacks_.objectSelectableInViewLayer(
                entry.id, rowViewLayerKey);
        } else if (!rowViewLayerKey.isEmpty() &&
                   rowViewLayerKey != activeViewLayerKey) {
            selectable = visible && !entry.hideSelect;
        }
        if (!rowViewLayerKey.isEmpty() &&
            (!hasLayerVisibilityCallback || !hasLayerSelectableCallback)) {
            for (QTreeWidgetItem *ancestor = item->parent();
                 ancestor != nullptr; ancestor = ancestor->parent()) {
                if (ancestor->data(nameColumn, itemKindRole).toInt() !=
                    static_cast<int>(OutlinerRowKind::Collection)) {
                    continue;
                }
                const bool globallyVisible =
                    ancestor->checkState(viewportColumn) == Qt::Checked;
                const bool layerVisible =
                    ancestor->checkState(viewLayerViewportColumn) == Qt::Checked &&
                    ancestor->checkState(excludeColumn) == Qt::Checked;
                if (!hasLayerVisibilityCallback) {
                    visible = visible && globallyVisible && layerVisible;
                }
                if (!hasLayerSelectableCallback) {
                    selectable = selectable && visible &&
                        ancestor->checkState(selectColumn) == Qt::Checked;
                }
            }
        }

        bool matchesState = true;
        switch (objectStateFilter_) {
        case OutlinerObjectStateFilter::Visible:
            matchesState = visible;
            break;
        case OutlinerObjectStateFilter::Hidden:
            matchesState = !visible;
            break;
        case OutlinerObjectStateFilter::Selected:
            matchesState = selectedValues.contains(objectValue);
            break;
        case OutlinerObjectStateFilter::Active:
            matchesState = activeId.isValid() &&
                           activeId.value() == objectValue;
            break;
        case OutlinerObjectStateFilter::Selectable:
            matchesState = selectable;
            break;
        case OutlinerObjectStateFilter::All:
            break;
        }
        if (invertObjectStateFilter_ &&
            objectStateFilter_ != OutlinerObjectStateFilter::All) {
            matchesState = !matchesState;
        }
        return matchesState;
    };

    const auto filterSubtree = [&](const auto &self,
                                   QTreeWidgetItem *item) -> bool {
        if (item == nullptr) return false;
        bool childMatches = false;
        for (int index = 0; index < item->childCount(); ++index) {
            childMatches = self(self, item->child(index)) || childMatches;
        }
        bool eligible = true;
        const OutlinerRowKind kind = static_cast<OutlinerRowKind>(
            item->data(nameColumn, itemKindRole).toInt());
        if (kind == OutlinerRowKind::Collection) {
            eligible = showCollections_;
        } else if (kind == OutlinerRowKind::Object) {
            eligible = showObjects_ && objectMatchesFilter(item);
        }
        const bool nameMatches = text.isEmpty() ||
            matcher.match(item->text(nameColumn)).hasMatch();
        const bool visible = (eligible && nameMatches) || childMatches;
        item->setHidden(!visible);
        if ((!text.isEmpty() || objectStateFilter_ !=
                                    OutlinerObjectStateFilter::All) &&
            childMatches) {
            item->setExpanded(true);
        }
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
    QStringList expandedPaths;
    const QString scope = callbacks_.stateScopeKey
        ? callbacks_.stateScopeKey() : QString{};
    const QString scopeHash = QString::fromLatin1(
        QCryptographicHash::hash(scope.toUtf8(),
                                 QCryptographicHash::Sha256)
            .toHex());
    const QString settingsKey = scenesDisplayMode_
        ? QStringLiteral("outliner/%1/scenesExpandedPaths").arg(scopeHash)
        : QStringLiteral("outliner/%1/expandedPaths").arg(scopeHash);
    bool hasSavedState = false;
    if (scenesDisplayMode_) {
        QSettings settings;
        if (!scope.isEmpty() && settings.contains(settingsKey)) {
            expandedPaths = settings.value(settingsKey).toStringList();
            hasSavedState = true;
        }
    } else {
        hasSavedState = callbacks_.loadExpandedTreePaths &&
        callbacks_.loadExpandedTreePaths(&expandedPaths);
        if (!hasSavedState && !scope.isEmpty()) {
            QSettings settings;
            if (settings.contains(settingsKey)) {
                expandedPaths = settings.value(settingsKey).toStringList();
                hasSavedState = true;
            }
        }
    }
    if (!hasSavedState) return;
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
    expandedPaths.sort(Qt::CaseSensitive);
    expandedPaths.removeDuplicates();
    if (scenesDisplayMode_) {
        const QString scope = callbacks_.stateScopeKey
            ? callbacks_.stateScopeKey() : QString{};
        if (scope.isEmpty()) return;
        const QString scopeHash = QString::fromLatin1(
            QCryptographicHash::hash(scope.toUtf8(),
                                     QCryptographicHash::Sha256)
                .toHex());
        QSettings settings;
        settings.setValue(
            QStringLiteral("outliner/%1/scenesExpandedPaths").arg(scopeHash),
            expandedPaths);
    } else if (callbacks_.saveExpandedTreePaths) {
        callbacks_.saveExpandedTreePaths(expandedPaths);
    } else {
        const QString scope = callbacks_.stateScopeKey
            ? callbacks_.stateScopeKey() : QString{};
        if (scope.isEmpty()) return;
        const QString scopeHash = QString::fromLatin1(
            QCryptographicHash::hash(scope.toUtf8(),
                                     QCryptographicHash::Sha256)
                .toHex());
        QSettings settings;
        settings.setValue(
            QStringLiteral("outliner/%1/expandedPaths").arg(scopeHash),
            expandedPaths);
    }
}

bool CollectionsPanel::activateObjectRow(
    QTreeWidgetItem *item, Qt::KeyboardModifiers modifiers)
{
    if (refreshing_ || item == nullptr) return false;
    const OutlinerRowKind kind = static_cast<OutlinerRowKind>(
        item->data(nameColumn, itemKindRole).toInt());
    if (kind == OutlinerRowKind::SceneRoot) {
        const QString sceneKey = item->data(
            nameColumn, itemSceneKeyRole).toString();
        if (!sceneKey.isEmpty() && callbacks_.setActiveScene) {
            QTimer::singleShot(0, this, [this, sceneKey]() {
                if (callbacks_.setActiveScene) {
                    callbacks_.setActiveScene(sceneKey);
                }
            });
            return true;
        }
        const QString viewLayerKey = item->data(
            nameColumn, itemViewLayerKeyRole).toString();
        if (viewLayerKey.isEmpty()) return false;
        if (callbacks_.setActiveViewLayer) {
            QTimer::singleShot(0, this, [this, viewLayerKey]() {
                if (callbacks_.setActiveViewLayer) {
                    callbacks_.setActiveViewLayer(viewLayerKey);
                }
            });
        }
        return true;
    }
    if (kind != OutlinerRowKind::Object) return false;
    const ObjectId clickedId = ObjectId::fromValue(
        item->data(nameColumn, itemIdRole).toULongLong());
    const QString rowViewLayerKey = item->data(
        nameColumn, itemViewLayerKeyRole).toString();
    const QString activeViewLayerKey = callbacks_.activeViewLayerKey
        ? callbacks_.activeViewLayerKey() : QString{};
    if (!rowViewLayerKey.isEmpty() &&
        rowViewLayerKey != activeViewLayerKey &&
        callbacks_.setActiveViewLayer && clickedId.isValid()) {
        const QString rowPath = item->data(nameColumn, itemPathRole).toString();
        QTimer::singleShot(0, this,
                           [this, clickedId, rowViewLayerKey, rowPath,
                            modifiers]() {
            if (!callbacks_.setActiveViewLayer ||
                !callbacks_.activeViewLayerKey) {
                return;
            }
            if (callbacks_.activeViewLayerKey() != rowViewLayerKey) {
                callbacks_.setActiveViewLayer(rowViewLayerKey);
            }
            const QVector<QTreeWidgetItem *> rows =
                objectRows_.value(clickedId.value());
            for (QTreeWidgetItem *row : rows) {
                if (row != nullptr &&
                    row->data(nameColumn, itemViewLayerKeyRole).toString() ==
                        rowViewLayerKey &&
                    row->data(nameColumn, itemPathRole).toString() == rowPath) {
                    activateObjectRow(row, modifiers);
                    return;
                }
            }
        });
        return true;
    }
    if (!callbacks_.setObjectSelection) return true;
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
    if (kind == OutlinerRowKind::ObjectContent) return;
    QMenu menu(this);

    if (kind == OutlinerRowKind::SceneRoot &&
        item->data(nameColumn, itemViewLayerKeyRole).toString().isEmpty() &&
        callbacks_.createCollection) {
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
    const QString &activeViewLayerKey,
    const QString &rowViewLayerKey)
{
    if (!entry.collection.id.isValid() ||
        ancestors.contains(entry.collection.id.value())) {
        return;
    }
    sourcePath.append(entry.collection.sourceId);
    const QJsonObject layerState = viewLayerState(
        entry.collection, activeViewLayerKey, sourcePath);
    if (!activeViewLayerKey.isEmpty() && layerState.isEmpty()) return;

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
    item->setData(nameColumn, itemSourcePathRole,
                  QVariant::fromValue(sourcePath));
    item->setData(nameColumn, itemViewLayerKeyRole, rowViewLayerKey);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable |
                   Qt::ItemIsSelectable | Qt::ItemIsDragEnabled |
                   Qt::ItemIsDropEnabled);
    item->setCheckState(viewportColumn, entry.collection.hideViewport
                               ? Qt::Unchecked : Qt::Checked);
    item->setCheckState(renderColumn, entry.collection.hideRender
                             ? Qt::Unchecked : Qt::Checked);
    item->setCheckState(selectColumn, entry.collection.hideSelect
                               ? Qt::Unchecked : Qt::Checked);
    if (!layerState.isEmpty()) {
        item->setData(viewLayerViewportColumn, itemViewLayerEditableRole, true);
        item->setData(holdoutColumn, itemViewLayerEditableRole, true);
        item->setData(indirectOnlyColumn, itemViewLayerEditableRole, true);
        item->setData(excludeColumn, itemViewLayerEditableRole, true);
        item->setCheckState(viewLayerViewportColumn,
            layerState.value(QStringLiteral("hide_viewport")).toBool()
                ? Qt::Unchecked : Qt::Checked);
        item->setCheckState(holdoutColumn,
            layerState.value(QStringLiteral("holdout")).toBool()
                ? Qt::Checked : Qt::Unchecked);
        item->setCheckState(indirectOnlyColumn,
            layerState.value(QStringLiteral("indirect_only")).toBool()
                ? Qt::Checked : Qt::Unchecked);
        item->setCheckState(excludeColumn,
            layerState.value(QStringLiteral("exclude")).toBool()
                ? Qt::Unchecked : Qt::Checked);
    }

    bool excludedFromViewLayer = false;
    for (qsizetype prefixSize = 1;
         prefixSize <= sourcePath.size() && !activeViewLayerKey.isEmpty();
         ++prefixSize) {
        const QStringList prefix = sourcePath.mid(0, prefixSize);
        const auto pathCollection = std::find_if(
            entries.cbegin(), entries.cend(), [&prefix](
                const CollectionPanelEntry &candidate) {
                return candidate.collection.sourceId == prefix.back();
            });
        if (pathCollection != entries.cend() &&
            viewLayerState(pathCollection->collection, activeViewLayerKey,
                           prefix).value(QStringLiteral("exclude")).toBool()) {
            excludedFromViewLayer = true;
            break;
        }
    }

    ancestors.insert(entry.collection.id.value());
    for (const CollectionPanelEntry &candidate : entries) {
        if (candidate.collection.parentIds.contains(entry.collection.id)) {
            appendCollection(tree, candidate, entries, objects, item, ancestors,
                             path, sourcePath, activeViewLayerKey,
                             rowViewLayerKey);
        }
    }
    if (!excludedFromViewLayer) {
        for (const OutlinerObjectEntry &object : objects) {
            if (!object.collectionIds.contains(entry.collection.id)) continue;
            appendObject(item, object, path, entry.collection.id,
                         rowViewLayerKey);
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
    item->setData(nameColumn, itemViewLayerKeyRole, activeViewLayerKey);
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
    if (showObjectContents_) {
        const auto appendContentRow =
            [&objectPath, &activeViewLayerKey](
                QTreeWidgetItem *contentParent,
                const QString &name, const QString &suffix,
                const QString &description) -> QTreeWidgetItem * {
            if (contentParent == nullptr || name.isEmpty()) return nullptr;
            auto *content = new QTreeWidgetItem(contentParent);
            content->setText(nameColumn, name);
            content->setData(nameColumn, itemKindRole,
                             static_cast<int>(OutlinerRowKind::ObjectContent));
            content->setData(nameColumn, itemViewLayerKeyRole,
                             activeViewLayerKey);
            content->setData(nameColumn, itemPathRole,
                             objectPath.join(QLatin1Char('/')) + suffix);
            content->setToolTip(nameColumn, description);
            content->setFlags(Qt::ItemIsEnabled);
            return content;
        };
        if (entry.hasAnimationData) {
            QTreeWidgetItem *animation = appendContentRow(
                item, QStringLiteral("Animation"), QStringLiteral("/animation"),
                QStringLiteral("Blender object animation data"));
            appendContentRow(animation, entry.animationActionName,
                             QStringLiteral("/animation/action"),
                             QStringLiteral("Blender active action data block"));
            if (entry.hasDrivers) {
                QTreeWidgetItem *drivers = appendContentRow(
                    animation, QStringLiteral("Drivers"),
                    QStringLiteral("/animation/drivers"),
                    QStringLiteral("Blender animation drivers"));
                for (qsizetype index = 0;
                     index < entry.driverTargetNames.size(); ++index) {
                    appendContentRow(
                        drivers, entry.driverTargetNames[index],
                        QStringLiteral("/animation/driver-target-%1").arg(index),
                        QStringLiteral("Blender driver target data block"));
                }
            }
            if (!entry.nlaTracks.isEmpty()) {
                QTreeWidgetItem *tracks = appendContentRow(
                    animation, QStringLiteral("NLA Tracks"),
                    QStringLiteral("/animation/nla-tracks"),
                    QStringLiteral("Blender non-linear animation tracks"));
                for (qsizetype trackIndex = 0;
                     trackIndex < entry.nlaTracks.size(); ++trackIndex) {
                    const OutlinerNlaTrackEntry &sourceTrack =
                        entry.nlaTracks[trackIndex];
                    QTreeWidgetItem *track = appendContentRow(
                        tracks, sourceTrack.name,
                        QStringLiteral("/animation/nla-track-%1").arg(trackIndex),
                        QStringLiteral("Blender NLA track"));
                    for (qsizetype actionIndex = 0;
                         actionIndex < sourceTrack.actionNames.size();
                         ++actionIndex) {
                        appendContentRow(
                            track, sourceTrack.actionNames[actionIndex],
                            QStringLiteral("/animation/nla-track-%1/action-%2")
                                .arg(trackIndex).arg(actionIndex),
                            QStringLiteral("Blender NLA action data block"));
                    }
                }
            }
        }
        appendContentRow(item, entry.dataBlockName, QStringLiteral("/data"),
                         QStringLiteral("Blender object data block"));
        for (qsizetype index = 0; index < entry.materialNames.size(); ++index) {
            appendContentRow(
                item,
                entry.materialNames[index],
                QStringLiteral("/material-%1").arg(index),
                QStringLiteral("Blender material data block"));
        }
        const auto appendNameGroup = [&appendContentRow](
            QTreeWidgetItem *objectItem, const QString &groupName,
            const QString &groupPath, const QString &description,
            const QStringList &names, const QString &itemPathPrefix) {
            if (names.isEmpty()) return;
            QTreeWidgetItem *group = appendContentRow(
                objectItem, groupName, groupPath, description);
            for (qsizetype index = 0; index < names.size(); ++index) {
                appendContentRow(
                    group, names[index],
                    QStringLiteral("%1-%2").arg(itemPathPrefix).arg(index),
                    description);
            }
        };
        appendNameGroup(item, QStringLiteral("Constraints"),
                        QStringLiteral("/constraints"),
                        QStringLiteral("Blender object constraints"),
                        entry.constraintNames, QStringLiteral("/constraint"));
        appendNameGroup(item, QStringLiteral("Modifiers"),
                        QStringLiteral("/modifiers"),
                        QStringLiteral("Blender object modifiers"),
                        entry.modifierNames, QStringLiteral("/modifier"));
        appendNameGroup(item, QStringLiteral("Vertex Groups"),
                        QStringLiteral("/vertex-groups"),
                        QStringLiteral("Blender mesh vertex groups"),
                        entry.vertexGroupNames, QStringLiteral("/vertex-group"));
    }
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
            const QString viewLayerKey = parentRow->data(
                nameColumn, itemViewLayerKeyRole).toString();
            QTreeWidgetItem *collectionRow = parentRow->parent();
            while (collectionRow != nullptr &&
                   collectionRow->data(nameColumn, itemKindRole).toInt() ==
                       static_cast<int>(OutlinerRowKind::Object)) {
                collectionRow = collectionRow->parent();
            }
            if (collectionRow == nullptr) continue;

            QTreeWidgetItem *matchingChildRow = nullptr;
            bool childPresentInViewLayer = false;
            for (QTreeWidgetItem *childRow : childRows) {
                if (childRow == nullptr || childRow == parentRow ||
                    childRow->data(nameColumn, itemViewLayerKeyRole)
                            .toString() != viewLayerKey) continue;
                childPresentInViewLayer = true;
                if (childRow->parent() == collectionRow) {
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
            if (!childPresentInViewLayer) continue;

            const QStringList parentPath = parentRow
                ->data(nameColumn, itemPathRole).toString()
                .split(QLatin1Char('/'), Qt::SkipEmptyParts);
            QTreeWidgetItem *notInCollectionRow = appendObject(
                parentRow, childEntry, parentPath,
                CollectionId::invalid(), viewLayerKey);
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
    const QString treeStateContextKey =
        (callbacks_.treeStateContextKey
             ? callbacks_.treeStateContextKey() : QString{}) +
        (scenesDisplayMode_ ? QStringLiteral("/Scenes")
                            : QStringLiteral("/ViewLayer"));
    const bool treeStateContextChanged =
        treeStateContextKey != currentTreeStateContextKey_;
    currentTreeStateContextKey_ = treeStateContextKey;
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
    const QVector<CollectionPanelEntry> allEntries = callbacks_.entries();
    const QVector<OutlinerObjectEntry> allObjects = callbacks_.objects
        ? callbacks_.objects() : QVector<OutlinerObjectEntry>{};
    const QVector<OutlinerSceneEntry> scenes = callbacks_.scenes
        ? callbacks_.scenes() : QVector<OutlinerSceneEntry>{};
    const bool scenesMode = scenesDisplayMode_ && !scenes.isEmpty();
    displayModeCombo_->setEnabled(!scenes.isEmpty());
    QVector<OutlinerObjectEntry> displayObjects;
    if (showObjects_) displayObjects = allObjects;
    const QVector<OutlinerViewLayerEntry> viewLayers = callbacks_.viewLayers
        ? callbacks_.viewLayers() : QVector<OutlinerViewLayerEntry>{};
    const QString activeSceneKey = callbacks_.activeSceneKey
        ? callbacks_.activeSceneKey() : QString{};
    const QString activeViewLayerKey = callbacks_.activeViewLayerKey
        ? callbacks_.activeViewLayerKey() : QString{};
    OutlinerSceneEntry activeScene;
    activeScene.key = activeSceneKey;
    for (const OutlinerSceneEntry &scene : scenes) {
        if (scene.key == activeSceneKey) {
            activeScene = scene;
            break;
        }
    }
    const auto entriesForScene = [&allEntries, activeSceneKey](
                                    const OutlinerSceneEntry &scene) {
        QVector<CollectionPanelEntry> entries;
        QSet<quint64> includedIds;
        for (const CollectionPanelEntry &entry : allEntries) {
            const bool belongs = entry.collection.sourceSceneNames.isEmpty()
                ? scene.key == activeSceneKey
                : entry.collection.sourceSceneNames.contains(scene.displayName);
            if (belongs) includedIds.insert(entry.collection.id.value());
        }
        for (const CollectionPanelEntry &entry : allEntries) {
            if (!includedIds.contains(entry.collection.id.value())) continue;
            DocumentCollection collection = entry.collection;
            collection.parentIds.erase(
                std::remove_if(collection.parentIds.begin(),
                               collection.parentIds.end(),
                               [&includedIds](CollectionId parentId) {
                                   return !includedIds.contains(parentId.value());
                               }),
                collection.parentIds.end());
            entries.append({std::move(collection)});
        }
        return entries;
    };
    const auto objectsForScene = [&allObjects, activeSceneKey](
                                    const OutlinerSceneEntry &scene) {
        QVector<OutlinerObjectEntry> objects;
        for (const OutlinerObjectEntry &object : allObjects) {
            if (object.sceneKeys.contains(scene.key) ||
                (scene.key == activeSceneKey && object.sceneKeys.isEmpty())) {
                objects.append(object);
            }
        }
        return objects;
    };
    const QVector<CollectionPanelEntry> entries = entriesForScene(activeScene);
    const QVector<OutlinerObjectEntry> activeSceneObjects =
        objectsForScene(activeScene);
    if (!scenesMode && showObjects_) displayObjects = activeSceneObjects;
    {
        const QSignalBlocker blocker(sceneCombo_);
        sceneCombo_->clear();
        int activeIndex = -1;
        for (const OutlinerSceneEntry &scene : scenes) {
            sceneCombo_->addItem(scene.displayName, scene.key);
            if (scene.key == activeSceneKey) {
                activeIndex = sceneCombo_->count() - 1;
            }
        }
        sceneCombo_->setCurrentIndex(activeIndex);
        sceneCombo_->setVisible(!scenesMode && scenes.size() > 1);
    }
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
        viewLayerCombo_->setVisible(!scenesMode && viewLayers.size() > 1);
    }
    tree_->setColumnHidden(viewportColumn, !showViewportColumn_);
    tree_->setColumnHidden(renderColumn, !showRenderColumn_);
    tree_->setColumnHidden(selectColumn, !showSelectColumn_);
    tree_->setColumnHidden(viewLayerViewportColumn,
                           scenesMode || activeViewLayerKey.isEmpty() ||
                               !showHideColumn_);
    tree_->setColumnHidden(holdoutColumn,
                           scenesMode || activeViewLayerKey.isEmpty() ||
                               !showHoldoutColumn_);
    tree_->setColumnHidden(indirectOnlyColumn,
                           scenesMode || activeViewLayerKey.isEmpty() ||
                               !showIndirectOnlyColumn_);
    tree_->setColumnHidden(excludeColumn,
                           scenesMode || activeViewLayerKey.isEmpty() ||
                               !showEnableColumn_);
    QVector<QTreeWidgetItem *> sceneRoots;
    const auto populateSceneRoot = [this](
                                       QTreeWidgetItem *root,
                                       const QVector<CollectionPanelEntry> &entries,
                                       const QVector<OutlinerObjectEntry> &objects,
                                       const QString &viewLayerKey,
                                       const QString &rootPath) {
        if (showCollections_) {
            for (const CollectionPanelEntry &entry : entries) {
                bool hasKnownParent = false;
                for (const CollectionId parentId : entry.collection.parentIds) {
                    const auto parent = std::find_if(
                        entries.cbegin(), entries.cend(),
                        [parentId](const CollectionPanelEntry &candidate) {
                            return candidate.collection.id == parentId;
                        });
                    hasKnownParent = hasKnownParent ||
                                     parent != entries.cend();
                }
                if (!hasKnownParent) {
                    appendCollection(tree_, entry, entries, objects, root, {},
                                     {rootPath}, {}, viewLayerKey,
                                     viewLayerKey);
                }
            }
            for (const OutlinerObjectEntry &object : objects) {
                if (object.collectionIds.isEmpty()) {
                    appendObject(root, object, {rootPath,
                                                QStringLiteral("root")},
                                 CollectionId::invalid(), viewLayerKey);
                }
            }
        } else {
            for (const OutlinerObjectEntry &object : objects) {
                appendObject(root, object, {rootPath,
                                            QStringLiteral("root")},
                             CollectionId::invalid(), viewLayerKey);
            }
        }
    };
    if (scenesMode) {
        for (const OutlinerSceneEntry &scene : scenes) {
            const QVector<CollectionPanelEntry> sceneEntries =
                entriesForScene(scene);
            const QVector<OutlinerObjectEntry> sceneObjects = showObjects_
                ? objectsForScene(scene) : QVector<OutlinerObjectEntry>{};
            const QString rootPath = QStringLiteral("scenes/%1").arg(scene.key);
            auto *sceneRoot = new QTreeWidgetItem(tree_);
            sceneRoot->setText(nameColumn, scene.displayName);
            sceneRoot->setData(nameColumn, itemKindRole,
                               static_cast<int>(OutlinerRowKind::SceneRoot));
            sceneRoot->setData(nameColumn, itemSceneKeyRole, scene.key);
            sceneRoot->setData(nameColumn, itemViewLayerKeyRole,
                               scene.activeViewLayerKey);
            sceneRoot->setData(nameColumn, itemPathRole, rootPath);
            sceneRoot->setToolTip(nameColumn,
                                  QStringLiteral("Click to activate this scene"));
            sceneRoot->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            QFont sceneFont = sceneRoot->font(nameColumn);
            sceneFont.setBold(scene.key == activeSceneKey);
            sceneRoot->setFont(nameColumn, sceneFont);
            sceneRoots.append(sceneRoot);

            if (!scene.viewLayers.isEmpty()) {
                auto *layersGroup = new QTreeWidgetItem(sceneRoot);
                layersGroup->setText(nameColumn,
                                     QStringLiteral("View Layers"));
                layersGroup->setData(nameColumn, itemKindRole,
                                     static_cast<int>(OutlinerRowKind::SceneGroup));
                layersGroup->setData(nameColumn, itemPathRole,
                                     rootPath + QStringLiteral("/view-layers"));
                layersGroup->setFlags(Qt::ItemIsEnabled);
                for (const OutlinerViewLayerEntry &viewLayer :
                     scene.viewLayers) {
                    auto *layerRow = new QTreeWidgetItem(layersGroup);
                    layerRow->setText(nameColumn, viewLayer.displayName);
                    layerRow->setData(nameColumn, itemKindRole,
                                      static_cast<int>(OutlinerRowKind::SceneRoot));
                    layerRow->setData(nameColumn, itemViewLayerKeyRole,
                                      viewLayer.key);
                    layerRow->setData(nameColumn, itemPathRole,
                        rootPath + QStringLiteral("/view-layers/%1")
                                       .arg(viewLayer.key));
                    layerRow->setToolTip(
                        nameColumn,
                        QStringLiteral("Click to activate this view layer"));
                    layerRow->setFlags(Qt::ItemIsEnabled |
                                       Qt::ItemIsSelectable);
                    QFont layerFont = layerRow->font(nameColumn);
                    layerFont.setBold(viewLayer.key == activeViewLayerKey);
                    layerRow->setFont(nameColumn, layerFont);
                }
            }

            if (showCollections_) {
                auto *collectionsGroup = new QTreeWidgetItem(sceneRoot);
                collectionsGroup->setText(nameColumn,
                                          QStringLiteral("Collections"));
                collectionsGroup->setData(nameColumn, itemKindRole,
                    static_cast<int>(OutlinerRowKind::SceneGroup));
                collectionsGroup->setData(nameColumn, itemPathRole,
                    rootPath + QStringLiteral("/collections"));
                collectionsGroup->setFlags(Qt::ItemIsEnabled);
                for (const CollectionPanelEntry &entry : sceneEntries) {
                    bool hasKnownParent = false;
                    for (const CollectionId parentId :
                         entry.collection.parentIds) {
                        const auto parent = std::find_if(
                            sceneEntries.cbegin(), sceneEntries.cend(),
                            [parentId](const CollectionPanelEntry &candidate) {
                                return candidate.collection.id == parentId;
                            });
                        hasKnownParent = hasKnownParent ||
                                         parent != sceneEntries.cend();
                    }
                    if (!hasKnownParent) {
                        appendCollection(
                            tree_, entry, sceneEntries, sceneObjects,
                            collectionsGroup, {},
                            {rootPath, QStringLiteral("collections")}, {},
                            {}, scene.activeViewLayerKey);
                    }
                }
            }

            if (showObjects_) {
                auto *objectsGroup = new QTreeWidgetItem(sceneRoot);
                objectsGroup->setText(nameColumn, QStringLiteral("Objects"));
                objectsGroup->setData(nameColumn, itemKindRole,
                    static_cast<int>(OutlinerRowKind::SceneGroup));
                objectsGroup->setData(nameColumn, itemPathRole,
                    rootPath + QStringLiteral("/objects"));
                objectsGroup->setFlags(Qt::ItemIsEnabled);
                for (const OutlinerObjectEntry &object : sceneObjects) {
                    appendObject(objectsGroup, object,
                                 {rootPath, QStringLiteral("objects")},
                                 CollectionId::invalid(),
                                 scene.activeViewLayerKey);
                }
            }
        }
    } else if (showAllViewLayers_ && !viewLayers.isEmpty()) {
        for (const OutlinerViewLayerEntry &viewLayer : viewLayers) {
            auto *viewLayerRoot = new QTreeWidgetItem(tree_);
            viewLayerRoot->setText(nameColumn, viewLayer.displayName);
            viewLayerRoot->setData(nameColumn, itemKindRole,
                                   static_cast<int>(OutlinerRowKind::SceneRoot));
            viewLayerRoot->setData(nameColumn, itemViewLayerKeyRole,
                                   viewLayer.key);
            const QString rootPath = QStringLiteral("view-layer/%1")
                                         .arg(viewLayer.key);
            viewLayerRoot->setData(nameColumn, itemPathRole, rootPath);
            viewLayerRoot->setToolTip(nameColumn,
                                      QStringLiteral("Click to activate this view layer"));
            viewLayerRoot->setFlags(viewLayerRoot->flags() |
                                    Qt::ItemIsDropEnabled |
                                    Qt::ItemIsSelectable);
            QFont rootFont = viewLayerRoot->font(nameColumn);
            rootFont.setBold(viewLayer.key == activeViewLayerKey);
            viewLayerRoot->setFont(nameColumn, rootFont);
            sceneRoots.append(viewLayerRoot);
            populateSceneRoot(viewLayerRoot, entries, displayObjects,
                              viewLayer.key, rootPath);
        }
    } else {
        auto *sceneRoot = new QTreeWidgetItem(tree_);
        sceneRoot->setText(nameColumn, QStringLiteral("Scene Collection"));
        sceneRoot->setData(nameColumn, itemKindRole,
                           static_cast<int>(OutlinerRowKind::SceneRoot));
        sceneRoot->setData(nameColumn, itemPathRole, QStringLiteral("scene"));
        sceneRoot->setFlags((sceneRoot->flags() & ~Qt::ItemIsSelectable) |
                            Qt::ItemIsDropEnabled);
        sceneRoots.append(sceneRoot);
        populateSceneRoot(sceneRoot, entries, displayObjects,
                          activeViewLayerKey,
                          QStringLiteral("scene"));
    }
    if (showObjectChildren_) {
        buildObjectParentHierarchy(displayObjects);
    }
    if (!hasRefreshed_ || stateScopeChanged || treeStateContextChanged) {
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
        QVector<QTreeWidgetItem *> pending = sceneRoots;
        while (!pending.isEmpty()) {
            QTreeWidgetItem *item = pending.takeLast();
            const QString path = item->data(nameColumn, itemPathRole).toString();
            const auto saved = expandedPaths.constFind(path);
            if (saved != expandedPaths.cend()) {
                item->setExpanded(saved.value());
            } else if (item->data(nameColumn, itemKindRole).toInt() ==
                           static_cast<int>(OutlinerRowKind::Collection) ||
                       item->data(nameColumn, itemKindRole).toInt() ==
                           static_cast<int>(OutlinerRowKind::SceneGroup)) {
                item->setExpanded(true);
            } else if (scenesMode &&
                       item->data(nameColumn, itemSceneKeyRole).isValid()) {
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
    const QString activeViewLayerKey = callbacks_.activeViewLayerKey
        ? callbacks_.activeViewLayerKey() : QString{};

    refreshing_ = true;
    const QSignalBlocker blocker(tree_);
    QTreeWidgetItem *activeItem = nullptr;
    QTreeWidgetItem *activeLayerItem = nullptr;
    QTreeWidgetItem *currentActiveItem = nullptr;
    QTreeWidgetItem *anyActiveItem = nullptr;
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
                if (activeId.isValid() && value == activeId.value()) {
                    if (currentItem == item) {
                        currentActiveItem = item;
                    }
                    if (anyActiveItem == nullptr) {
                        anyActiveItem = item;
                    }
                    if (item->data(nameColumn, itemViewLayerKeyRole).toString() ==
                            activeViewLayerKey &&
                        activeLayerItem == nullptr) {
                        activeLayerItem = item;
                    }
                }
            }
            for (int childIndex = 0; childIndex < item->childCount();
                 ++childIndex) {
                pending.append(item->child(childIndex));
            }
        }
    }
    activeItem = activeLayerItem != nullptr ? activeLayerItem
        : (currentActiveItem != nullptr ? currentActiveItem : anyActiveItem);
    if (activeItem != nullptr) {
        tree_->setCurrentItem(activeItem, nameColumn,
                              QItemSelectionModel::NoUpdate);
    }
    refreshing_ = false;
    applyNameFilter(activeFilterText_);
    if (scrollToActive_ && activeItem != nullptr && !activeItem->isHidden()) {
        const QRect activeRect = tree_->visualItemRect(activeItem);
        if (!activeRect.isValid() ||
            !tree_->viewport()->rect().intersects(activeRect)) {
            for (QTreeWidgetItem *ancestor = activeItem->parent();
                 ancestor != nullptr; ancestor = ancestor->parent()) {
                if (!ancestor->isHidden()) ancestor->setExpanded(true);
            }
            tree_->scrollToItem(activeItem, QAbstractItemView::PositionAtCenter);
        }
    }
}

} // namespace classiCAD
