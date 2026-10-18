#include "outliner_panel.h"

#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <QHash>

namespace classiCAD {
namespace {

constexpr int ObjectIdRole = Qt::UserRole;
constexpr int ContainerRowRole = Qt::UserRole + 1;

ObjectId itemObjectId(const QTreeWidgetItem *item)
{
    return item == nullptr
               ? ObjectId::invalid()
               : ObjectId::fromValue(item->data(0, ObjectIdRole).toULongLong());
}

} // namespace

OutlinerPanel::OutlinerPanel(OutlinerPanelCallbacks callbacks, QWidget *parent)
    : QFrame(parent)
    , callbacks_(std::move(callbacks))
{
    setObjectName(QStringLiteral("outlinerPanel"));
    setMinimumWidth(240);
    createLayout();
}

void OutlinerPanel::createLayout()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    auto *modeLayout = new QHBoxLayout;
    objectModeButton_ = new QPushButton(QStringLiteral("Object Mode"), this);
    objectModeButton_->setObjectName(QStringLiteral("objectModeButton"));
    objectModeButton_->setCheckable(true);
    editModeButton_ = new QPushButton(QStringLiteral("Edit Mode"), this);
    editModeButton_->setObjectName(QStringLiteral("editModeButton"));
    editModeButton_->setCheckable(true);
    modeLayout->addWidget(objectModeButton_);
    modeLayout->addWidget(editModeButton_);
    layout->addLayout(modeLayout);

    tree_ = new QTreeWidget(this);
    tree_->setObjectName(QStringLiteral("outlinerTree"));
    tree_->setHeaderHidden(true);
    tree_->setUniformRowHeights(true);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(tree_, 1);

    connect(objectModeButton_, &QPushButton::clicked, this, [this]() {
        if (callbacks_.setEditMode) callbacks_.setEditMode(false);
    });
    connect(editModeButton_, &QPushButton::clicked, this, [this]() {
        if (callbacks_.setEditMode) callbacks_.setEditMode(true);
    });
    connect(tree_, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem *current, QTreeWidgetItem *) {
                if (refreshing_ || current == nullptr || !callbacks_.selectObject) {
                    return;
                }
                const bool isContainer = current->data(0, ContainerRowRole).toBool();
                callbacks_.selectObject(itemObjectId(current), isContainer);
            });

    updateModeButtons();
}

void OutlinerPanel::refresh()
{
    if (tree_ == nullptr || !callbacks_.objectInfos) {
        return;
    }

    const QVector<ViewportObjectInfo> infos = callbacks_.objectInfos();
    refreshing_ = true;
    tree_->clear();
    QHash<quint64, QTreeWidgetItem *> containerItems;
    QHash<quint64, QTreeWidgetItem *> geometryItems;
    QHash<quint64, ObjectId> firstSelectedGeometry;
    ObjectId firstSelectedContainer = ObjectId::invalid();
    ObjectId primarySelectedContainer = ObjectId::invalid();
    ObjectId primarySelectedObject = ObjectId::invalid();
    hasSelectedObject_ = false;

    for (const ViewportObjectInfo &info : infos) {
        if (info.id == info.containerId) {
            auto *containerItem = new QTreeWidgetItem(tree_);
            containerItem->setText(0, info.containerName);
            containerItem->setData(0, ObjectIdRole,
                                   QVariant::fromValue<qulonglong>(info.containerId.value()));
            containerItem->setData(0, ContainerRowRole, true);
            containerItem->setExpanded(true);
            containerItems.insert(info.containerId.value(), containerItem);
        }
    }

    for (const ViewportObjectInfo &info : infos) {
        QTreeWidgetItem *containerItem =
            containerItems.value(info.containerId.value(), nullptr);
        if (containerItem == nullptr) {
            continue;
        }
        auto *geometryItem = new QTreeWidgetItem(containerItem);
        geometryItem->setText(0, info.geometryName);
        geometryItem->setData(0, ObjectIdRole,
                              QVariant::fromValue<qulonglong>(info.id.value()));
        geometryItem->setData(0, ContainerRowRole, false);
        geometryItems.insert(info.id.value(), geometryItem);
        if (info.selected && !firstSelectedGeometry.contains(info.containerId.value())) {
            firstSelectedGeometry.insert(info.containerId.value(), info.id);
            if (!firstSelectedContainer.isValid()) {
                firstSelectedContainer = info.containerId;
            }
            hasSelectedObject_ = true;
        }
        if (info.primarySelected) {
            primarySelectedContainer = info.containerId;
            primarySelectedObject = info.id;
            firstSelectedGeometry.insert(info.containerId.value(), info.id);
            hasSelectedObject_ = true;
        }
    }

    if (primarySelectedContainer.isValid()) {
        activeObjectId_ = primarySelectedContainer;
    } else if (firstSelectedContainer.isValid() &&
               (!editMode_ || !activeObjectId_.isValid())) {
        activeObjectId_ = firstSelectedContainer;
    }

    QTreeWidgetItem *currentItem = nullptr;
    if (editMode_) {
        ObjectId selectedId = ObjectId::invalid();
        if (primarySelectedContainer == activeObjectId_) {
            selectedId = primarySelectedObject;
        } else if (firstSelectedGeometry.contains(activeObjectId_.value())) {
            selectedId = firstSelectedGeometry.value(activeObjectId_.value());
        }
        currentItem = geometryItems.value(selectedId.value(), nullptr);
        if (currentItem == nullptr) {
            currentItem = containerItems.value(activeObjectId_.value(), nullptr);
        }
    } else if (hasSelectedObject_) {
        const ObjectId selectedContainer = primarySelectedContainer.isValid()
                                               ? primarySelectedContainer
                                               : firstSelectedContainer;
        currentItem = containerItems.value(selectedContainer.value(), nullptr);
    }
    if (currentItem != nullptr) {
        tree_->setCurrentItem(currentItem, 0,
                              QItemSelectionModel::ClearAndSelect |
                                  QItemSelectionModel::Rows);
        tree_->scrollToItem(currentItem);
    } else {
        tree_->clearSelection();
        tree_->setCurrentItem(nullptr);
    }
    updateModeButtons();
    refreshing_ = false;
}

void OutlinerPanel::setObjectState(bool editMode, ObjectId activeObjectId)
{
    editMode_ = editMode;
    activeObjectId_ = activeObjectId;
    updateModeButtons();
}

void OutlinerPanel::updateModeButtons()
{
    if (objectModeButton_ == nullptr || editModeButton_ == nullptr) {
        return;
    }
    objectModeButton_->setChecked(!editMode_);
    editModeButton_->setChecked(editMode_);
    editModeButton_->setEnabled(activeObjectId_.isValid() || hasSelectedObject_);
    editModeButton_->setToolTip(
        activeObjectId_.isValid()
            ? QStringLiteral("Edit the active object's curves, surfaces, and other geometry (Tab)")
            : QStringLiteral("Create geometry to make an object available for editing"));
    objectModeButton_->setToolTip(QStringLiteral("Leave the active object's geometry (Tab)"));
}

} // namespace classiCAD
