/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "layers_panel.h"

#include "layer_style_widgets.h"
#include "ui/viewport/line_type_style.h"

#include <QAbstractItemView>
#include <QColor>
#include <QFont>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>

#include <utility>

namespace classiCAD {
namespace {

enum class LayerHeaderIcon {
    Visibility,
    Freeze,
    Lock,
    Color,
    Plot,
};

QIcon makeLayerHeaderIcon(LayerHeaderIcon kind)
{
    QPixmap pixmap(16, 16);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(185, 190, 198), 1.0,
                        Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);

    switch (kind) {
    case LayerHeaderIcon::Visibility:
        painter.drawEllipse(QRectF(1.5, 4.0, 13.0, 8.0));
        painter.setBrush(QColor(185, 190, 198));
        painter.drawEllipse(QRectF(6.0, 6.0, 4.0, 4.0));
        break;
    case LayerHeaderIcon::Freeze:
        painter.drawLine(QPointF(8, 1.5), QPointF(8, 14.5));
        painter.drawLine(QPointF(2.4, 4.8), QPointF(13.6, 11.2));
        painter.drawLine(QPointF(2.4, 11.2), QPointF(13.6, 4.8));
        painter.drawLine(QPointF(8, 1.5), QPointF(6.4, 3.2));
        painter.drawLine(QPointF(8, 1.5), QPointF(9.6, 3.2));
        painter.drawLine(QPointF(8, 14.5), QPointF(6.4, 12.8));
        painter.drawLine(QPointF(8, 14.5), QPointF(9.6, 12.8));
        break;
    case LayerHeaderIcon::Lock: {
        QPainterPath shackle;
        shackle.moveTo(5, 7.5);
        shackle.lineTo(5, 5.5);
        shackle.cubicTo(5, 1.7, 11, 1.7, 11, 5.5);
        shackle.lineTo(11, 7.5);
        painter.drawPath(shackle);
        painter.drawRoundedRect(QRectF(3.5, 7, 9, 7), 1, 1);
        painter.drawLine(QPointF(8, 9.2), QPointF(8, 11.4));
        break;
    }
    case LayerHeaderIcon::Color:
        painter.setBrush(QColor(235, 235, 235));
        painter.drawRect(QRectF(3, 3, 10, 10));
        break;
    case LayerHeaderIcon::Plot:
        painter.drawRect(QRectF(4, 1.8, 8, 5.2));
        painter.drawRoundedRect(QRectF(2, 5.5, 12, 6.3), 1, 1);
        painter.drawRect(QRectF(4, 9.5, 8, 4.5));
        painter.drawLine(QPointF(5.5, 11.5), QPointF(10.5, 11.5));
        painter.drawPoint(QPointF(11.5, 7.8));
        break;
    }

    return QIcon(pixmap);
}

QTableWidgetItem *makeCheckItem(bool checked, const QString &tooltip)
{
    auto *item = new QTableWidgetItem;
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable |
                   Qt::ItemIsUserCheckable);
    item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    item->setTextAlignment(Qt::AlignCenter);
    item->setToolTip(tooltip);
    return item;
}

} // namespace

LayersPanel::LayersPanel(LayersPanelCallbacks callbacks, QWidget *parent)
    : QFrame(parent)
    , callbacks_(std::move(callbacks))
{
    setObjectName(QStringLiteral("rightPanel"));
    setMinimumWidth(240);
    createLayout();
}

void LayersPanel::createLayout()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    auto *layersBox = new QGroupBox(QStringLiteral("Layers"), this);
    auto *layersLayout = new QVBoxLayout(layersBox);
    auto *actionsLayout = new QHBoxLayout;

    addButton_ = new QPushButton(QStringLiteral("+"), layersBox);
    addButton_->setToolTip(QStringLiteral("Add layer"));
    removeButton_ = new QPushButton(QStringLiteral("-"), layersBox);
    removeButton_->setToolTip(QStringLiteral("Remove selected empty layer"));
    activateButton_ = new QPushButton(QStringLiteral("✓"), layersBox);
    activateButton_->setToolTip(QStringLiteral("Set selected layer as current"));
    moveUpButton_ = new QPushButton(QStringLiteral("↑"), layersBox);
    moveUpButton_->setToolTip(QStringLiteral("Move layer up"));
    moveDownButton_ = new QPushButton(QStringLiteral("↓"), layersBox);
    moveDownButton_->setToolTip(QStringLiteral("Move layer down"));
    for (QPushButton *button : {addButton_, removeButton_, activateButton_,
                                moveUpButton_, moveDownButton_}) {
        button->setFixedWidth(34);
        actionsLayout->addWidget(button);
    }
    actionsLayout->addStretch(1);
    layersLayout->addLayout(actionsLayout);

    filter_ = new QLineEdit(layersBox);
    filter_->setObjectName(QStringLiteral("layerFilter"));
    filter_->setPlaceholderText(QStringLiteral("Filter layers"));
    layersLayout->addWidget(filter_);

    table_ = new QTableWidget(0, 10, layersBox);
    table_->setObjectName(QStringLiteral("layerTable"));
    QFont headerFont = table_->horizontalHeader()->font();
    headerFont.setBold(false);
    headerFont.setWeight(QFont::Normal);
    table_->horizontalHeader()->setFont(headerFont);
    table_->setHorizontalHeaderLabels(
        {QString(), QStringLiteral("Name"), QString(), QString(), QString(), QString(),
         QStringLiteral("Linetype"), QStringLiteral("Lineweight"), QString(),
         QStringLiteral("Description")});
    const auto setIconHeader = [this](int column, LayerHeaderIcon icon,
                                      const QString &tooltip) {
        QTableWidgetItem *header = table_->horizontalHeaderItem(column);
        header->setIcon(makeLayerHeaderIcon(icon));
        header->setTextAlignment(Qt::AlignCenter);
        header->setToolTip(tooltip);
    };
    setIconHeader(2, LayerHeaderIcon::Visibility, QStringLiteral("Visibility"));
    setIconHeader(3, LayerHeaderIcon::Freeze, QStringLiteral("Freeze / thaw"));
    setIconHeader(4, LayerHeaderIcon::Lock, QStringLiteral("Lock / unlock"));
    setIconHeader(5, LayerHeaderIcon::Color, QStringLiteral("Layer color"));
    setIconHeader(8, LayerHeaderIcon::Plot, QStringLiteral("Plot / do not plot"));
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setStretchLastSection(false);
    table_->horizontalHeader()->setMinimumSectionSize(20);
    for (int column = 0; column < 10; ++column) {
        table_->horizontalHeader()->setSectionResizeMode(column, QHeaderView::Fixed);
    }
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    table_->horizontalHeader()->setSectionResizeMode(9, QHeaderView::Stretch);
    const int widths[] = {22, 110, 22, 22, 22, 22, 68, 72, 22, 75};
    for (int column = 0; column < 10; ++column) {
        table_->setColumnWidth(column, widths[column]);
    }
    table_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setShowGrid(false);
    table_->setMinimumHeight(190);
    layersLayout->addWidget(table_, 1);

    moveObjectsButton_ = new QPushButton(QStringLiteral("Move Selected Here"), layersBox);
    moveObjectsButton_->setToolTip(
        QStringLiteral("Move the selected geometry to this layer"));
    layersLayout->addWidget(moveObjectsButton_);
    layout->addWidget(layersBox, 1);

    connect(table_, &QTableWidget::currentCellChanged, this,
            [this](int, int, int, int) { updateControls(); });
    connect(table_, &QTableWidget::itemChanged, this,
            [this](QTableWidgetItem *item) {
                if (refreshing_ || item == nullptr) {
                    return;
                }
                const LayerId id = LayerId::fromValue(
                    item->data(Qt::UserRole).toULongLong());
                const bool enabled = item->checkState() == Qt::Checked;
                switch (item->column()) {
                case 2:
                    if (callbacks_.setVisible) callbacks_.setVisible(id, enabled);
                    break;
                case 3:
                    if (callbacks_.setFrozen) callbacks_.setFrozen(id, enabled);
                    break;
                case 4:
                    if (callbacks_.setLocked) callbacks_.setLocked(id, enabled);
                    break;
                case 8:
                    if (callbacks_.setPlotted) callbacks_.setPlotted(id, enabled);
                    break;
                default:
                    break;
                }
            });
    connect(table_, &QTableWidget::cellClicked, this,
            [this](int row, int column) {
                table_->setCurrentCell(row, column);
                const LayerId id = layerIdAtRow(row);
                if (column == 0 && callbacks_.activateLayer) {
                    callbacks_.activateLayer(id);
                } else if (column == 5 && callbacks_.chooseColor) {
                    callbacks_.chooseColor(id);
                }
            });
    connect(table_, &QTableWidget::cellDoubleClicked, this,
            [this](int row, int column) {
                const LayerId id = layerIdAtRow(row);
                if (column == 1) {
                    table_->setCurrentCell(row, column);
                    if (callbacks_.renameLayer) callbacks_.renameLayer(id);
                } else if (column == 9 && callbacks_.editDescription) {
                    callbacks_.editDescription(id);
                }
            });
    connect(filter_, &QLineEdit::textChanged, this,
            [this](const QString &) { refresh(); });
    connect(addButton_, &QPushButton::clicked, this, [this]() {
        if (callbacks_.addLayer) callbacks_.addLayer();
    });
    connect(removeButton_, &QPushButton::clicked, this, [this]() {
        if (callbacks_.removeLayer) callbacks_.removeLayer(selectedLayerId());
    });
    connect(moveUpButton_, &QPushButton::clicked, this,
            [this]() { moveSelectedLayer(-1); });
    connect(moveDownButton_, &QPushButton::clicked, this,
            [this]() { moveSelectedLayer(1); });
    connect(activateButton_, &QPushButton::clicked, this, [this]() {
        if (callbacks_.activateLayer) callbacks_.activateLayer(selectedLayerId());
    });
    connect(moveObjectsButton_, &QPushButton::clicked, this, [this]() {
        if (callbacks_.moveSelectedObjects) {
            callbacks_.moveSelectedObjects(selectedLayerId());
        }
    });
}

LayerId LayersPanel::selectedLayerId() const
{
    return layerIdAtRow(table_ == nullptr ? -1 : table_->currentRow());
}

int LayersPanel::currentRow() const noexcept
{
    return table_ == nullptr ? -1 : table_->currentRow();
}

LayerId LayersPanel::layerIdAtRow(int row) const
{
    if (table_ == nullptr || row < 0 || row >= table_->rowCount()) {
        return LayerId::invalid();
    }
    const QTableWidgetItem *nameItem = table_->item(row, 1);
    return nameItem == nullptr
               ? LayerId::invalid()
               : LayerId::fromValue(nameItem->data(Qt::UserRole).toULongLong());
}

void LayersPanel::moveSelectedLayer(int delta)
{
    const int row = currentRow();
    const int targetIndex = row + delta;
    if (row < 0 || targetIndex < 0 || targetIndex >= table_->rowCount()) {
        return;
    }
    if (callbacks_.moveLayer) {
        callbacks_.moveLayer(selectedLayerId(), targetIndex);
    }
}

void LayersPanel::updateControls()
{
    if (!callbacks_.layerInfos) {
        return;
    }
    const QVector<ViewportLayerInfo> infos = callbacks_.layerInfos();
    const LayerId id = selectedLayerId();
    ViewportLayerInfo selectedInfo;
    bool found = false;
    for (const ViewportLayerInfo &info : infos) {
        if (info.id == id) {
            selectedInfo = info;
            found = true;
            break;
        }
    }

    activateButton_->setEnabled(found && !selectedInfo.active);
    removeButton_->setEnabled(found && table_->rowCount() > 1);
    moveUpButton_->setEnabled(found && currentRow() > 0);
    moveDownButton_->setEnabled(found && currentRow() + 1 < table_->rowCount());
    moveObjectsButton_->setEnabled(found && selectedInfo.visible &&
                                   !selectedInfo.frozen && !selectedInfo.locked);
}

void LayersPanel::refresh()
{
    if (table_ == nullptr || !callbacks_.layerInfos) {
        return;
    }

    const LayerId previousSelection = selectedLayerId();
    const QVector<ViewportLayerInfo> infos = callbacks_.layerInfos();
    refreshing_ = true;
    {
        const QSignalBlocker blocker(table_);
        table_->setRowCount(infos.size());
        int activeRow = -1;
        int previousRow = -1;
        const QString filter = filter_->text().trimmed();

        for (int row = 0; row < infos.size(); ++row) {
            const ViewportLayerInfo &info = infos[row];
            if (info.id == previousSelection) previousRow = row;
            if (info.active) activeRow = row;

            auto *currentItem = new QTableWidgetItem(
                info.active ? QStringLiteral("✓") : QString());
            currentItem->setTextAlignment(Qt::AlignCenter);
            currentItem->setToolTip(info.active ? QStringLiteral("Current layer")
                                                : QStringLiteral("Click to make current"));
            auto *nameItem = new QTableWidgetItem(info.name);
            nameItem->setToolTip(QStringLiteral("%1 object%2 — double-click to rename")
                                     .arg(info.objectCount)
                                     .arg(info.objectCount == 1 ? QString()
                                                                : QStringLiteral("s")));
            if (info.active) {
                QFont activeFont = nameItem->font();
                activeFont.setBold(true);
                nameItem->setFont(activeFont);
            }

            auto *visibleItem = makeCheckItem(info.visible,
                                              QStringLiteral("Layer visibility"));
            auto *frozenItem = makeCheckItem(info.frozen,
                                             QStringLiteral("Freeze layer"));
            auto *lockedItem = makeCheckItem(info.locked,
                                             QStringLiteral("Lock layer"));
            auto *plottedItem = makeCheckItem(info.plotted,
                                              QStringLiteral("Plot this layer"));

            QPixmap colorSwatch(12, 12);
            colorSwatch.fill(Qt::transparent);
            {
                QPainter painter(&colorSwatch);
                painter.fillRect(QRect(1, 1, 10, 10), info.color);
                painter.setPen(QColor(80, 80, 80));
                painter.drawRect(QRect(0, 0, 11, 11));
            }
            auto *colorItem = new QTableWidgetItem;
            colorItem->setIcon(QIcon(colorSwatch));
            colorItem->setTextAlignment(Qt::AlignCenter);
            colorItem->setToolTip(QStringLiteral("%1 — click to change layer color")
                                      .arg(info.color.name().toUpper()));

            auto *lineTypeCombo = new LayerLineTypeComboBox(table_);
            populateLayerLineTypeCombo(lineTypeCombo, false);
            const QString rowLineType = canonicalLayerLineTypeName(info.lineType);
            int lineTypeIndex = lineTypeCombo->findData(rowLineType);
            if (lineTypeIndex < 0) {
                lineTypeCombo->addItem(makeLayerLineTypeIcon(info.lineType),
                                       info.lineType, info.lineType);
                lineTypeIndex = lineTypeCombo->count() - 1;
            }
            lineTypeCombo->setCurrentIndex(lineTypeIndex);
            lineTypeCombo->setToolTip(QStringLiteral("Linetype: %1").arg(info.lineType));

            auto *lineWeightCombo = new QComboBox(table_);
            const QList<QPair<QString, qreal>> lineWeights{
                {QStringLiteral("Default"), 0.0},
                {QStringLiteral("0.13 mm"), 0.13},
                {QStringLiteral("0.18 mm"), 0.18},
                {QStringLiteral("0.25 mm"), 0.25},
                {QStringLiteral("0.35 mm"), 0.35},
                {QStringLiteral("0.50 mm"), 0.50},
                {QStringLiteral("0.70 mm"), 0.70},
                {QStringLiteral("1.00 mm"), 1.00},
                {QStringLiteral("1.40 mm"), 1.40},
                {QStringLiteral("2.00 mm"), 2.00},
                {QStringLiteral("2.11 mm"), 2.11}};
            for (const auto &weight : lineWeights) {
                lineWeightCombo->addItem(weight.first, weight.second);
            }
            for (int index = 0; index < lineWeightCombo->count(); ++index) {
                if (qFuzzyCompare(lineWeightCombo->itemData(index).toDouble() + 1.0,
                                  info.lineWeightMm + 1.0)) {
                    lineWeightCombo->setCurrentIndex(index);
                    break;
                }
            }
            lineWeightCombo->setToolTip(QStringLiteral("Layer lineweight"));

            auto *descriptionItem = new QTableWidgetItem(info.description);
            descriptionItem->setToolTip(
                info.description.isEmpty()
                    ? QStringLiteral("Double-click to edit description")
                    : QStringLiteral("%1\nDouble-click to edit description")
                          .arg(info.description));

            const QList<QTableWidgetItem *> rowItems{
                currentItem, nameItem, visibleItem, frozenItem, lockedItem,
                colorItem, nullptr, nullptr, plottedItem, descriptionItem};
            for (int column = 0; column < rowItems.size(); ++column) {
                QTableWidgetItem *item = rowItems[column];
                if (item != nullptr) {
                    item->setData(Qt::UserRole,
                                  QVariant::fromValue<qulonglong>(info.id.value()));
                    table_->setItem(row, column, item);
                }
            }
            table_->setCellWidget(row, 6, lineTypeCombo);
            table_->setCellWidget(row, 7, lineWeightCombo);

            connect(lineTypeCombo, qOverload<int>(&QComboBox::activated),
                    this, [this, id = info.id, lineTypeCombo](int index) {
                        if (index < 0) return;
                        const QString lineType = lineTypeCombo->itemData(index).toString();
                        QTimer::singleShot(0, this, [this, id, lineType]() {
                            if (callbacks_.setLineType) callbacks_.setLineType(id, lineType);
                        });
                    });
            connect(lineWeightCombo, &QComboBox::activated,
                    this, [this, id = info.id, lineWeightCombo](int) {
                        const qreal weight = lineWeightCombo->currentData().toDouble();
                        QTimer::singleShot(0, this, [this, id, weight]() {
                            if (callbacks_.setLineWeight) {
                                callbacks_.setLineWeight(id, weight);
                            }
                        });
                    });
            table_->setRowHeight(row, 24);
            const bool matches = filter.isEmpty() ||
                                 info.name.contains(filter, Qt::CaseInsensitive);
            table_->setRowHidden(row, !matches);
        }

        int targetRow = previousRow;
        if (targetRow < 0 || table_->isRowHidden(targetRow)) targetRow = activeRow;
        if (targetRow >= 0 && table_->isRowHidden(targetRow)) {
            targetRow = -1;
            for (int row = 0; row < table_->rowCount(); ++row) {
                if (!table_->isRowHidden(row)) {
                    targetRow = row;
                    break;
                }
            }
        }
        if (targetRow >= 0) {
            table_->setCurrentCell(targetRow, 1);
        } else {
            table_->setCurrentCell(-1, 0);
        }
    }
    refreshing_ = false;
    updateControls();
    if (callbacks_.layersRefreshed) {
        callbacks_.layersRefreshed(infos);
    }
}

} // namespace classiCAD
