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
 * The Libraries tree and its ID-type filtering follow
 * tree_display_libraries.cc and the RNA labels from common.cc.
 * Orphan Data grouping and its zero-user/type-filter rules follow
 * tree_display_orphaned.cc and BKE_idtype.hh.
 * Sequencer strip nesting, current meta level, and duplicate-file grouping
 * follow tree_display_sequencer.cc and tree_element_seq.cc.
 * Sequencer row selection and active-strip state follow outliner_sync.cc.
 * Library Overrides Properties and Hierarchies views follow
 * tree_display_override_library_properties.cc,
 * tree_display_override_library_hierarchies.cc, tree_element_overrides.cc,
 * and space_outliner.py.
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
#include <QCheckBox>
#include <QColor>
#include <QCryptographicHash>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QHash>
#include <QInputDialog>
#include <QFont>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPolygonF>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QToolButton>
#include <QVariant>
#include <QVBoxLayout>
#include <QRegularExpression>

#include <algorithm>
#include <functional>
#include <limits>

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
constexpr int itemSourceObjectIdRole = Qt::UserRole + 11;
constexpr int itemLibraryCurrentFileRole = Qt::UserRole + 12;
constexpr int itemSequencerStripIdRole = Qt::UserRole + 13;
constexpr int itemSequencerStripActiveRole = Qt::UserRole + 14;
constexpr int itemDataApiKindRole = Qt::UserRole + 15;
constexpr int itemDataApiTypeRole = Qt::UserRole + 16;
constexpr int itemDataApiNodeRole = Qt::UserRole + 17;
constexpr int itemDataApiChildrenBuiltRole = Qt::UserRole + 18;
constexpr int itemDataApiTruncatedRole = Qt::UserRole + 19;
constexpr int itemDataApiPlaceholderRole = Qt::UserRole + 20;
constexpr int itemSceneMembershipsRole = Qt::UserRole + 21;
constexpr int itemDataStackKindRole = Qt::UserRole + 22;
constexpr int itemDataStackIndexRole = Qt::UserRole + 23;
constexpr int itemAssetMarkedRole = Qt::UserRole + 24;
constexpr int itemLinkedLibraryRole = Qt::UserRole + 25;
constexpr int itemSceneMasterRole = Qt::UserRole + 26;
constexpr int nameColumn = 0;
constexpr int viewportColumn = 1;
constexpr int renderColumn = 2;
constexpr int viewLayerViewportColumn = 3;
constexpr int holdoutColumn = 4;
constexpr int indirectOnlyColumn = 5;
constexpr int selectColumn = 6;
constexpr int excludeColumn = 7;
constexpr int modeColumn = 8;
constexpr int itemModeValueRole = Qt::UserRole + 27;

QColor collectionColorTagColor(const QString &colorTag);

enum class RestrictionGlyph {
    Enable,
    Select,
    Hide,
    Viewport,
    Render,
    Holdout,
    IndirectOnly,
    ObjectMode,
    EditMode,
};

QString outlinerObjectTypeFilterKey(QString blenderType)
{
    blenderType = blenderType.trimmed().toUpper();
    if (blenderType == QLatin1String("MESH")) {
        return QStringLiteral("MESH");
    }
    if (blenderType == QLatin1String("ARMATURE")) {
        return QStringLiteral("ARMATURE");
    }
    if (blenderType == QLatin1String("LIGHT")) {
        return QStringLiteral("LIGHT");
    }
    if (blenderType == QLatin1String("CAMERA")) {
        return QStringLiteral("CAMERA");
    }
    if (blenderType == QLatin1String("EMPTY")) {
        return QStringLiteral("EMPTY");
    }
    if (blenderType == QLatin1String("GREASE_PENCIL") ||
        blenderType == QLatin1String("GREASEPENCIL") ||
        blenderType == QLatin1String("GPENCIL")) {
        return QStringLiteral("GREASE_PENCIL");
    }
    return QStringLiteral("OTHERS");
}

QIcon paintedIcon(const std::function<void(QPainter &)> &paint)
{
    QPixmap pixmap(16, 16);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(0xB8, 0xBC, 0xC2), 1.25,
                        Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    paint(painter);
    return QIcon(pixmap);
}

QIcon collectionIcon(const QString &colorTag)
{
    QColor color = collectionColorTagColor(colorTag);
    if (!color.isValid()) color = QColor(0xA9, 0xAF, 0xB8);
    return paintedIcon([color](QPainter &painter) {
        QPainterPath folder;
        folder.moveTo(1.5, 4.5);
        folder.lineTo(6.0, 4.5);
        folder.lineTo(7.6, 6.1);
        folder.lineTo(14.5, 6.1);
        folder.lineTo(13.6, 13.2);
        folder.lineTo(1.5, 13.2);
        folder.closeSubpath();
        painter.setPen(QPen(color.darker(135), 1.0));
        painter.setBrush(color);
        painter.drawPath(folder);
    });
}

QIcon objectTypeIcon(QString blenderType)
{
    blenderType = blenderType.toUpper();
    return paintedIcon([blenderType](QPainter &painter) {
        QColor color(0xB8, 0xBC, 0xC2);
        if (blenderType == QLatin1String("MESH")) {
            color = QColor(0xE4, 0x94, 0x4C);
            QPolygonF mesh;
            mesh << QPointF(2.0, 12.7) << QPointF(7.6, 2.2)
                 << QPointF(14.0, 12.7) << QPointF(2.0, 12.7);
            painter.setPen(QPen(color, 1.4, Qt::SolidLine,
                                Qt::RoundCap, Qt::RoundJoin));
            painter.setBrush(QColor(color.red(), color.green(),
                                    color.blue(), 38));
            painter.drawPolygon(mesh);
            painter.setBrush(color);
            for (const QPointF &point : {QPointF(2.0, 12.7),
                                         QPointF(7.6, 2.2),
                                         QPointF(14.0, 12.7)}) {
                painter.drawEllipse(point, 1.45, 1.45);
            }
        } else if (blenderType == QLatin1String("CAMERA")) {
            color = QColor(0x66, 0xC8, 0xC1);
            painter.setPen(QPen(color, 1.35, Qt::SolidLine,
                                Qt::RoundCap, Qt::RoundJoin));
            painter.drawRoundedRect(QRectF(1.5, 4.2, 9.2, 7.4), 1.0, 1.0);
            painter.drawPolygon(QPolygonF{QPointF(10.7, 6.0),
                                           QPointF(14.5, 4.2),
                                           QPointF(14.5, 11.8),
                                           QPointF(10.7, 10.0)});
        } else if (blenderType == QLatin1String("LIGHT")) {
            color = QColor(0xA8, 0xD3, 0x6F);
            painter.setPen(QPen(color, 1.25, Qt::SolidLine,
                                Qt::RoundCap, Qt::RoundJoin));
            painter.drawEllipse(QRectF(4.0, 3.0, 8.0, 8.0));
            painter.drawLine(QPointF(6.3, 11.5), QPointF(9.7, 11.5));
            painter.drawLine(QPointF(6.8, 13.2), QPointF(9.2, 13.2));
            painter.drawLine(QPointF(8.0, 0.9), QPointF(8.0, 2.0));
            painter.drawLine(QPointF(1.8, 7.0), QPointF(3.0, 7.0));
            painter.drawLine(QPointF(13.0, 7.0), QPointF(14.2, 7.0));
        } else if (blenderType == QLatin1String("ARMATURE")) {
            color = QColor(0xB1, 0x8B, 0xD3);
            painter.setPen(QPen(color, 2.0, Qt::SolidLine,
                                Qt::RoundCap, Qt::RoundJoin));
            painter.drawLine(QPointF(3.0, 12.8), QPointF(12.7, 3.1));
            painter.setBrush(color);
            painter.drawEllipse(QPointF(3.0, 12.8), 2.0, 2.0);
            painter.drawEllipse(QPointF(12.7, 3.1), 2.0, 2.0);
        } else if (blenderType == QLatin1String("EMPTY")) {
            painter.setPen(QPen(color, 1.3));
            painter.drawEllipse(QRectF(3.0, 3.0, 10.0, 10.0));
            painter.drawLine(QPointF(8.0, 1.0), QPointF(8.0, 15.0));
            painter.drawLine(QPointF(1.0, 8.0), QPointF(15.0, 8.0));
        } else if (blenderType == QLatin1String("CURVE") ||
                   blenderType == QLatin1String("SURFACE") ||
                   blenderType == QLatin1String("FONT")) {
            color = QColor(0x78, 0xC6, 0xA1);
            QPainterPath curve;
            curve.moveTo(2.0, 12.7);
            curve.cubicTo(4.0, 1.5, 11.8, 14.5, 14.0, 3.2);
            painter.setPen(QPen(color, 1.6, Qt::SolidLine,
                                Qt::RoundCap, Qt::RoundJoin));
            painter.drawPath(curve);
            painter.setBrush(color);
            painter.drawEllipse(QPointF(2.0, 12.7), 1.25, 1.25);
            painter.drawEllipse(QPointF(14.0, 3.2), 1.25, 1.25);
        } else if (blenderType == QLatin1String("GREASE_PENCIL") ||
                   blenderType == QLatin1String("GPENCIL")) {
            color = QColor(0xD7, 0x83, 0xA6);
            painter.setPen(QPen(color, 2.0, Qt::SolidLine,
                                Qt::RoundCap, Qt::RoundJoin));
            painter.drawLine(QPointF(3.0, 12.7), QPointF(12.7, 3.0));
            painter.drawLine(QPointF(5.2, 14.0), QPointF(14.0, 5.2));
        } else {
            painter.setPen(QPen(color, 1.3));
            painter.setBrush(QColor(color.red(), color.green(),
                                    color.blue(), 35));
            painter.drawEllipse(QRectF(3.0, 3.0, 10.0, 10.0));
            painter.drawEllipse(QPointF(8.0, 8.0), 1.2, 1.2);
        }
    });
}

QIcon restrictionIcon(RestrictionGlyph glyph, bool enabled)
{
    const QColor active = enabled ? QColor(0xC3, 0xC8, 0xCF)
                                  : QColor(0x6C, 0x71, 0x78);
    return paintedIcon([glyph, active, enabled](QPainter &painter) {
        painter.setPen(QPen(active, 1.25, Qt::SolidLine,
                            Qt::RoundCap, Qt::RoundJoin));
        switch (glyph) {
        case RestrictionGlyph::Enable:
            painter.drawRoundedRect(QRectF(2.0, 2.0, 12.0, 12.0), 2.0, 2.0);
            painter.drawLine(QPointF(5.0, 8.0), QPointF(7.1, 10.0));
            painter.drawLine(QPointF(7.1, 10.0), QPointF(11.4, 5.8));
            break;
        case RestrictionGlyph::Select:
            painter.drawLine(QPointF(3.0, 1.5), QPointF(3.0, 13.5));
            painter.drawLine(QPointF(3.0, 1.5), QPointF(12.5, 8.5));
            painter.drawLine(QPointF(3.0, 7.2), QPointF(7.8, 7.2));
            painter.drawLine(QPointF(7.8, 7.2), QPointF(10.5, 13.3));
            break;
        case RestrictionGlyph::Hide:
            painter.drawEllipse(QRectF(1.2, 4.0, 13.6, 8.0));
            painter.drawEllipse(QPointF(8.0, 8.0), 2.0, 2.0);
            break;
        case RestrictionGlyph::Viewport:
            painter.drawRoundedRect(QRectF(1.5, 2.4, 13.0, 9.7), 1.0, 1.0);
            painter.drawLine(QPointF(5.0, 14.0), QPointF(11.0, 14.0));
            painter.drawLine(QPointF(8.0, 12.1), QPointF(8.0, 14.0));
            break;
        case RestrictionGlyph::Render:
            painter.drawRoundedRect(QRectF(1.2, 4.0, 9.6, 8.2), 1.0, 1.0);
            painter.drawPolygon(QPolygonF{QPointF(10.8, 6.0),
                                           QPointF(14.6, 4.2),
                                           QPointF(14.6, 12.0),
                                           QPointF(10.8, 10.2)});
            break;
        case RestrictionGlyph::Holdout:
            painter.drawEllipse(QRectF(2.0, 2.0, 12.0, 12.0));
            painter.drawLine(QPointF(3.0, 13.0), QPointF(13.0, 3.0));
            break;
        case RestrictionGlyph::IndirectOnly:
            painter.drawEllipse(QPointF(8.0, 8.0), 2.0, 2.0);
            painter.drawLine(QPointF(8.0, 1.0), QPointF(8.0, 3.2));
            painter.drawLine(QPointF(8.0, 12.8), QPointF(8.0, 15.0));
            painter.drawLine(QPointF(1.0, 8.0), QPointF(3.2, 8.0));
            painter.drawLine(QPointF(12.8, 8.0), QPointF(15.0, 8.0));
            break;
        case RestrictionGlyph::ObjectMode:
            painter.drawRoundedRect(QRectF(2.2, 2.2, 11.6, 11.6), 2.0, 2.0);
            break;
        case RestrictionGlyph::EditMode:
            painter.drawLine(QPointF(2.4, 12.7), QPointF(12.5, 2.5));
            painter.drawEllipse(QPointF(2.4, 12.7), 1.7, 1.7);
            painter.drawEllipse(QPointF(12.5, 2.5), 1.7, 1.7);
            break;
        }
        if (!enabled) {
            painter.drawLine(QPointF(2.0, 14.0), QPointF(14.0, 2.0));
        }
    });
}

class RestrictionIconDelegate final : public QStyledItemDelegate {
public:
    RestrictionIconDelegate(int column, RestrictionGlyph glyph,
                            QObject *parent)
        : QStyledItemDelegate(parent), column_(column), glyph_(glyph)
    {
    }

    void paint(QPainter *painter,
               const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem background(option);
        QApplication::style()->drawPrimitive(
            QStyle::PE_PanelItemViewItem, &background, painter,
            option.widget);
        const QVariant state = index.data(Qt::CheckStateRole);
        if (state.isNull()) return;
        const bool checked = state.toInt() == Qt::Checked;
        const QIcon icon = restrictionIcon(glyph_, checked);
        const QSize size(16, 16);
        const QRect rect(option.rect.center().x() - size.width() / 2,
                         option.rect.center().y() - size.height() / 2,
                         size.width(), size.height());
        icon.paint(painter, rect);
    }

    bool editorEvent(QEvent *event,
                     QAbstractItemModel *model,
                     const QStyleOptionViewItem &option,
                     const QModelIndex &index) override
    {
        if (index.data(Qt::CheckStateRole).isNull() ||
            (index.flags() & Qt::ItemIsUserCheckable) == 0) {
            return false;
        }
        if ((column_ == viewLayerViewportColumn || column_ == holdoutColumn ||
             column_ == indirectOnlyColumn || column_ == excludeColumn) &&
            !index.data(itemViewLayerEditableRole).toBool()) {
            return false;
        }
        bool activate = false;
        if (event->type() == QEvent::MouseButtonRelease) {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            activate = mouse->button() == Qt::LeftButton &&
                       option.rect.contains(mouse->position().toPoint());
        } else if (event->type() == QEvent::KeyPress) {
            const auto *key = static_cast<QKeyEvent *>(event);
            activate = key->key() == Qt::Key_Space;
        }
        if (!activate) return false;
        const Qt::CheckState next =
            index.data(Qt::CheckStateRole).toInt() == Qt::Checked
                ? Qt::Unchecked : Qt::Checked;
        return model->setData(index, next, Qt::CheckStateRole);
    }

private:
    int column_ = -1;
    RestrictionGlyph glyph_ = RestrictionGlyph::Hide;
};

bool isDataApiPointerStruct(const QTreeWidgetItem *item)
{
    if (item == nullptr ||
        item->data(nameColumn, itemDataApiKindRole).toString() !=
            QLatin1String("STRUCT")) {
        return false;
    }
    const QTreeWidgetItem *parent = item->parent();
    return parent != nullptr &&
           parent->data(nameColumn, itemDataApiKindRole).toString() ==
               QLatin1String("PROPERTY") &&
           parent->data(nameColumn, itemDataApiTypeRole).toString() ==
               QLatin1String("POINTER");
}

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

QColor collectionColorTagColor(const QString &colorTag)
{
    if (colorTag == QLatin1String("COLOR_01")) return QColor(0xE9, 0x78, 0x53);
    if (colorTag == QLatin1String("COLOR_02")) return QColor(0xD9, 0xA8, 0x38);
    if (colorTag == QLatin1String("COLOR_03")) return QColor(0x9B, 0xB7, 0x42);
    if (colorTag == QLatin1String("COLOR_04")) return QColor(0x45, 0xA8, 0x78);
    if (colorTag == QLatin1String("COLOR_05")) return QColor(0x43, 0xA8, 0xB5);
    if (colorTag == QLatin1String("COLOR_06")) return QColor(0x55, 0x83, 0xD5);
    if (colorTag == QLatin1String("COLOR_07")) return QColor(0x9A, 0x70, 0xC8);
    if (colorTag == QLatin1String("COLOR_08")) return QColor(0xD5, 0x68, 0x92);
    return {};
}

QIcon collectionColorTagIcon(const QString &colorTag)
{
    const QColor color = collectionColorTagColor(colorTag);
    if (!color.isValid()) return {};
    QPixmap pixmap(12, 12);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(color.darker(140), 1.0));
    painter.setBrush(color);
    painter.drawRoundedRect(QRectF(1.0, 1.0, 10.0, 10.0), 2.0, 2.0);
    return QIcon(pixmap);
}

QString collectionColorTagLabel(const QString &colorTag)
{
    if (colorTag == QLatin1String("NONE")) return QStringLiteral("None");
    if (colorTag.startsWith(QLatin1String("COLOR_"))) {
        return QStringLiteral("Color %1").arg(colorTag.mid(6));
    }
    return QStringLiteral("Unknown");
}

class OutlinerTreeWidget final : public QTreeWidget {
public:
    using ActivateObject =
        std::function<bool(QTreeWidgetItem *, Qt::KeyboardModifiers)>;
    using ApplyDrop = std::function<void(
        const QVector<OutlinerRowReference> &,
        const OutlinerRowReference &,
        bool,
        bool,
        OutlinerDropPosition)>;

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

    void setAllowParentingWithoutModifier(bool allow)
    {
        allowParentingWithoutModifier_ = allow;
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
        QTreeWidgetItem *current = currentItem();
        const OutlinerRowReference currentReference = rowReference(current);
        if (currentReference.kind != OutlinerRowKind::Object &&
            currentReference.kind != OutlinerRowKind::Collection &&
            currentReference.kind != OutlinerRowKind::Material &&
            currentReference.kind != OutlinerRowKind::DataStackEntry &&
            currentReference.kind != OutlinerRowKind::DataStackGroup) {
            QTreeWidget::startDrag(supportedActions);
            return;
        }
        const bool draggingDataStack =
            currentReference.kind == OutlinerRowKind::DataStackEntry ||
            currentReference.kind == OutlinerRowKind::DataStackGroup;
        QSet<const QTreeWidgetItem *> selectedRows;
        if (current != nullptr) selectedRows.insert(current);
        if (!draggingDataStack) {
            for (QTreeWidgetItem *item : selectedItems()) {
                if (rowReference(item).kind == currentReference.kind) {
                    selectedRows.insert(item);
                }
            }
        }
        const QSet<const QTreeWidgetItem *> selectedCollectionRows =
            currentReference.kind == OutlinerRowKind::Collection
                ? selectedRows : QSet<const QTreeWidgetItem *>{};
        QSet<QString> seen;
        const auto append = [this, &seen, &selectedRows,
                             &selectedCollectionRows,
                             &currentReference](QTreeWidgetItem *item) {
            if (item == nullptr) return;
            if (!selectedRows.contains(item)) return;
            const OutlinerRowReference reference = rowReference(item);
            if (reference.kind != currentReference.kind) return;
            if (reference.kind == OutlinerRowKind::Collection) {
                for (QTreeWidgetItem *ancestor = item->parent();
                     ancestor != nullptr; ancestor = ancestor->parent()) {
                    if (selectedCollectionRows.contains(ancestor) &&
                        rowReference(ancestor).kind ==
                            OutlinerRowKind::Collection) {
                        return;
                    }
                }
            }
            const QString key =
                (reference.kind == OutlinerRowKind::DataStackEntry ||
                 reference.kind == OutlinerRowKind::DataStackGroup)
                ? QStringLiteral("s%1:%2:%3")
                      .arg(reference.objectId.value())
                      .arg(static_cast<int>(reference.dataStackKind))
                      .arg(reference.dataStackIndex)
                : reference.kind == OutlinerRowKind::Object
                ? QStringLiteral("o%1").arg(reference.objectId.value())
                : reference.kind == OutlinerRowKind::Collection
                    ? QStringLiteral("c%1").arg(reference.collectionId.value())
                    : QStringLiteral("m%1").arg(reference.materialId);
            if (seen.contains(key)) return;
            seen.insert(key);
            draggedRows_.append(reference);
        };
        std::function<void(QTreeWidgetItem *)> visit =
            [&append, &visit](QTreeWidgetItem *item) {
                append(item);
                for (int index = 0; index < item->childCount(); ++index) {
                    visit(item->child(index));
                }
            };
        for (int index = 0; index < topLevelItemCount(); ++index) {
            visit(topLevelItem(index));
        }
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
        OutlinerDropPosition position = OutlinerDropPosition::Into;
        const bool materialDrop =
            draggedRows_.first().kind == OutlinerRowKind::Material;
        const bool shiftHeld =
            event->modifiers().testFlag(Qt::ShiftModifier);
        const bool draggedObject =
            draggedRows_.first().kind == OutlinerRowKind::Object;
        const bool parentActionAllowed = draggedObject &&
            (allowParentingWithoutModifier_ || shiftHeld);
        const QString targetPath = targetItem == nullptr
            ? QString{}
            : targetItem->data(nameColumn, itemPathRole).toString();
        if (draggedRows_.first().kind == OutlinerRowKind::DataStackEntry ||
            draggedRows_.first().kind == OutlinerRowKind::DataStackGroup) {
            const OutlinerRowReference &source = draggedRows_.first();
            if (draggedRows_.size() != 1 || targetItem == nullptr ||
                !source.objectId.isValid()) {
                event->ignore();
                return;
            }

            const bool sourceIsEntry =
                source.kind == OutlinerRowKind::DataStackEntry;
            const bool sameObjectEntryDrop = sourceIsEntry &&
                target.kind == OutlinerRowKind::DataStackEntry &&
                source.objectId == target.objectId &&
                source.dataStackKind == target.dataStackKind;
            if (sameObjectEntryDrop) {
                const QRect targetRect = visualItemRect(targetItem);
                if (targetRect.height() <= 0) {
                    event->ignore();
                    return;
                }
                const int rowY =
                    event->position().toPoint().y() - targetRect.top();
                if (rowY < targetRect.height() / 4) {
                    position = OutlinerDropPosition::Before;
                } else if (rowY > (targetRect.height() * 3) / 4) {
                    position = OutlinerDropPosition::After;
                } else {
                    event->ignore();
                    return;
                }
                const QVector<OutlinerRowReference> sources = draggedRows_;
                QTimer::singleShot(0, this,
                    [this, sources, target, position]() {
                        if (applyDrop_) {
                            applyDrop_(sources, target, false, false, position);
                        }
                    });
                event->setDropAction(Qt::MoveAction);
                event->accept();
                return;
            }

            const bool targetIsObject = target.kind == OutlinerRowKind::Object;
            const bool targetIsStackRow =
                target.kind == OutlinerRowKind::DataStackGroup ||
                target.kind == OutlinerRowKind::DataStackEntry;
            const bool targetStackMatches = targetIsStackRow &&
                target.dataStackKind == source.dataStackKind;
            const bool targetIsValid = targetIsObject || targetStackMatches;
            if (!targetIsValid || !target.objectId.isValid() ||
                target.objectId == source.objectId ||
                (!sourceIsEntry &&
                 target.kind == OutlinerRowKind::DataStackEntry)) {
                event->ignore();
                return;
            }

            const QVector<OutlinerRowReference> sources = draggedRows_;
            const bool linkStack =
                source.kind == OutlinerRowKind::DataStackGroup;
            QTimer::singleShot(0, this,
                [this, sources, target, linkStack]() {
                    if (applyDrop_) {
                        applyDrop_(sources, target, linkStack, false,
                                   OutlinerDropPosition::Into);
                    }
                });
            event->setDropAction(Qt::CopyAction);
            event->accept();
            return;
        }
        const bool clearParentDrop =
            parentActionAllowed &&
            (((target.kind == OutlinerRowKind::SceneRoot &&
               target.sceneKey.isEmpty()) ||
              (target.kind == OutlinerRowKind::Collection && shiftHeld)) ||
             (target.kind == OutlinerRowKind::ObjectContent &&
              (targetPath.endsWith(QStringLiteral("/modifiers")) ||
               targetPath.endsWith(QStringLiteral("/constraints")))));
        if (target.kind == OutlinerRowKind::ObjectContent &&
            !clearParentDrop) {
            event->ignore();
            return;
        }
        if (target.kind == OutlinerRowKind::SceneRoot &&
            !target.sceneKey.isEmpty() &&
            draggedRows_.first().kind != OutlinerRowKind::Object) {
            event->ignore();
            return;
        }
        if (target.kind == OutlinerRowKind::Collection &&
            draggedRows_.first().kind == OutlinerRowKind::Collection) {
            const QRect targetRect = visualItemRect(targetItem);
            const int rowY = event->position().toPoint().y() - targetRect.top();
            if (targetRect.height() > 0 &&
                rowY < targetRect.height() / 4) {
                position = OutlinerDropPosition::Before;
            } else if (targetRect.height() > 0 &&
                       rowY > (targetRect.height() * 3) / 4) {
                position = OutlinerDropPosition::After;
            }
            if (position == OutlinerDropPosition::Before &&
                targetRect.height() > 0 && rowY < targetRect.height() / 4 &&
                targetItem->isExpanded() && targetItem->childCount() > 0) {
                const OutlinerRowReference firstChild =
                    rowReference(targetItem->child(0));
                if (firstChild.kind == OutlinerRowKind::Collection) {
                    target = firstChild;
                } else {
                    position = OutlinerDropPosition::Into;
                }
            }
        }
        if (target.kind == OutlinerRowKind::Object && !materialDrop &&
            !parentActionAllowed) {
            target.kind = target.parentCollectionId.isValid()
                ? OutlinerRowKind::Collection : OutlinerRowKind::SceneRoot;
            target.collectionId = target.parentCollectionId;
        }
        if (materialDrop && target.kind != OutlinerRowKind::Object) {
            event->ignore();
            return;
        }
        if (shiftHeld && !materialDrop &&
            (draggedRows_.first().kind != OutlinerRowKind::Object ||
             (target.kind != OutlinerRowKind::Object && !clearParentDrop))) {
            event->ignore();
            return;
        }
        if (target.kind == OutlinerRowKind::Collection &&
            std::any_of(draggedRows_.cbegin(), draggedRows_.cend(),
                        [&target](const OutlinerRowReference &source) {
                return source.kind == OutlinerRowKind::Collection &&
                       source.collectionId == target.collectionId;
            })) {
            event->ignore();
            return;
        }

        const QVector<OutlinerRowReference> sources = draggedRows_;
        const bool link = event->modifiers().testFlag(Qt::ControlModifier);
        const bool parent = parentActionAllowed &&
            (target.kind == OutlinerRowKind::Object || clearParentDrop);
        QTimer::singleShot(0, this,
                           [this, sources, target, link, parent, position]() {
            if (applyDrop_) applyDrop_(sources, target, link, parent, position);
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
        if (reference.kind == OutlinerRowKind::SceneRoot) {
            reference.sceneKey = item->data(nameColumn, itemSceneKeyRole)
                                     .toString();
        }
        if (reference.kind == OutlinerRowKind::Material) {
            reference.materialId = item->data(nameColumn, itemIdRole).toString();
        }
        if (reference.kind == OutlinerRowKind::DataStackEntry ||
            reference.kind == OutlinerRowKind::DataStackGroup) {
            reference.objectId = ObjectId::fromValue(value);
            reference.dataStackKind = static_cast<BlenderSourceDataStackKind>(
                item->data(nameColumn, itemDataStackKindRole).toInt());
            reference.dataStackIndex = item->data(
                nameColumn, itemDataStackIndexRole).toLongLong();
        } else if (reference.kind == OutlinerRowKind::ObjectContent) {
            reference.objectId = ObjectId::fromValue(value);
        }
        if (reference.kind == OutlinerRowKind::Object) {
            reference.objectId = ObjectId::fromValue(value);
            reference.linkedCollectionId = CollectionId::fromValue(
                item->data(nameColumn, itemLinkedCollectionRole).toULongLong());
            reference.syntheticParentRow =
                item->data(nameColumn, itemNotInCollectionRole).toBool();
            reference.sceneMembershipKeys = item->data(
                nameColumn, itemSceneMembershipsRole).toStringList();
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
            ancestor = item->parent();
            while (ancestor != nullptr) {
                if (ancestor->data(nameColumn, itemKindRole).toInt() ==
                    static_cast<int>(OutlinerRowKind::SceneRoot)) {
                    reference.sceneKey = ancestor->data(
                        nameColumn, itemSceneKeyRole).toString();
                    if (!reference.sceneKey.isEmpty()) break;
                }
                ancestor = ancestor->parent();
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
    bool allowParentingWithoutModifier_ = false;
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
    filterEdit_->setPlaceholderText(QStringLiteral("Filter Outliner names"));
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
    displayModeCombo_->addItem(QStringLiteral("Sequence"));
    displayModeCombo_->addItem(QStringLiteral("Data API"));
    displayModeCombo_->addItem(QStringLiteral("Libraries"));
    displayModeCombo_->addItem(QStringLiteral("Orphan Data"));
    displayModeCombo_->addItem(QStringLiteral("Library Overrides"));
    displayModeCombo_->setToolTip(QStringLiteral(
        "Choose the Blender Outliner tree display."));
    filterRow->addWidget(displayModeCombo_);
    libraryOverrideViewCombo_ = new QComboBox(this);
    libraryOverrideViewCombo_->addItem(QStringLiteral("Properties"),
                                       QStringLiteral("PROPERTIES"));
    libraryOverrideViewCombo_->addItem(QStringLiteral("Hierarchies"),
                                       QStringLiteral("HIERARCHIES"));
    libraryOverrideViewCombo_->setToolTip(QStringLiteral(
        "Choose the Blender Library Overrides view."));
    filterRow->addWidget(libraryOverrideViewCombo_);
    idTypeCombo_ = new QComboBox(this);
    idTypeCombo_->setToolTip(QStringLiteral(
        "Filter Blender data-block rows by ID type."));
    idTypeCombo_->addItem(QStringLiteral("All ID Types"), QString{});
    idTypeCombo_->setVisible(false);
    filterRow->addWidget(idTypeCombo_);
    sceneCombo_ = new QComboBox(this);
    sceneCombo_->setToolTip(QStringLiteral("Active Blender scene"));
    filterRow->addWidget(sceneCombo_);
    viewLayerCombo_ = new QComboBox(this);
    viewLayerCombo_->setToolTip(QStringLiteral("Active Blender view layer"));
    filterRow->addWidget(viewLayerCombo_);
    layout->addLayout(filterRow);
    auto *keyingSetRow = new QHBoxLayout;
    keyingSetRow->setContentsMargins(0, 0, 0, 0);
    dataApiKeyingSetCombo_ = new QComboBox(this);
    dataApiKeyingSetCombo_->setToolTip(QStringLiteral(
        "Active Blender Keying Set for RNA properties"));
    keyingSetRow->addWidget(dataApiKeyingSetCombo_, 1);
    addKeyingSetPathsButton_ = new QToolButton(this);
    addKeyingSetPathsButton_->setText(QStringLiteral("+"));
    addKeyingSetPathsButton_->setToolTip(QStringLiteral(
        "Add selected animatable properties to the active Keying Set"));
    keyingSetRow->addWidget(addKeyingSetPathsButton_);
    removeKeyingSetPathsButton_ = new QToolButton(this);
    removeKeyingSetPathsButton_->setText(QStringLiteral("−"));
    removeKeyingSetPathsButton_->setToolTip(QStringLiteral(
        "Remove selected animatable properties from the active Keying Set"));
    keyingSetRow->addWidget(removeKeyingSetPathsButton_);
    addDataApiDriverButton_ = new QToolButton(this);
    addDataApiDriverButton_->setText(QStringLiteral("D+"));
    addDataApiDriverButton_->setToolTip(QStringLiteral(
        "Add driver paths for selected animatable properties. classiCAD stores the paths but does not evaluate Blender drivers."));
    keyingSetRow->addWidget(addDataApiDriverButton_);
    removeDataApiDriverButton_ = new QToolButton(this);
    removeDataApiDriverButton_->setText(QStringLiteral("D−"));
    removeDataApiDriverButton_->setToolTip(QStringLiteral(
        "Remove driver paths for selected animatable properties. classiCAD stores the paths but does not evaluate Blender drivers."));
    keyingSetRow->addWidget(removeDataApiDriverButton_);
    layout->addLayout(keyingSetRow);
    auto *outlinerTree = new OutlinerTreeWidget(this);
    tree_ = outlinerTree;
    tree_->setColumnCount(9);
    tree_->setHeaderLabels({QStringLiteral("Name"),
                            QStringLiteral("Viewport"),
                            QStringLiteral("Render"),
                            QStringLiteral("Layer View"),
                            QStringLiteral("Holdout"),
                            QStringLiteral("Indirect Only"),
                            QStringLiteral("Select"),
                            QStringLiteral("Exclude"),
                            QStringLiteral("Mode")});
    tree_->headerItem()->setToolTip(viewportColumn,
                                    QStringLiteral("Hide globally in the viewport. Shift-click applies to descendants; Ctrl-click on a collection isolates it."));
    tree_->headerItem()->setToolTip(renderColumn,
                                    QStringLiteral("Hide globally in renders. Shift-click applies to descendants; Ctrl-click on a collection isolates it."));
    tree_->headerItem()->setToolTip(viewLayerViewportColumn,
                                    QStringLiteral("Hide in the active Blender view layer, or hide the object in the current viewport when no Blender view layer is active. Shift-click applies to descendants; Ctrl-click on a collection isolates it."));
    tree_->headerItem()->setToolTip(holdoutColumn,
                                    QStringLiteral("Make this collection a holdout in the active view layer. Shift-click applies to descendants; Ctrl-click isolates it."));
    tree_->headerItem()->setToolTip(indirectOnlyColumn,
                                    QStringLiteral("Cycles only: make this collection contribute indirectly. Shift-click applies to descendants; Ctrl-click isolates it."));
    tree_->headerItem()->setToolTip(selectColumn,
                                    QStringLiteral("Prevent selection. Shift-click applies to descendants; Ctrl-click on a collection isolates it."));
    tree_->headerItem()->setToolTip(excludeColumn,
                                    QStringLiteral("Exclude this collection path from the active view layer. Shift-click applies to descendants; Ctrl-click isolates this path."));
    tree_->headerItem()->setToolTip(modeColumn,
                                    QStringLiteral("Switch the active object between Object Mode and Edit Mode."));
    const QVector<QPair<int, RestrictionGlyph>> restrictionColumns{
        {viewportColumn, RestrictionGlyph::Viewport},
        {renderColumn, RestrictionGlyph::Render},
        {viewLayerViewportColumn, RestrictionGlyph::Hide},
        {holdoutColumn, RestrictionGlyph::Holdout},
        {indirectOnlyColumn, RestrictionGlyph::IndirectOnly},
        {selectColumn, RestrictionGlyph::Select},
        {excludeColumn, RestrictionGlyph::Enable}};
    for (const auto &column : restrictionColumns) {
        tree_->headerItem()->setText(column.first, QString{});
        tree_->headerItem()->setIcon(
            column.first, restrictionIcon(column.second, true));
    }
    tree_->headerItem()->setText(modeColumn, QString{});
    tree_->headerItem()->setIcon(
        modeColumn, restrictionIcon(RestrictionGlyph::ObjectMode, true));
    tree_->setRootIsDecorated(true);
    tree_->setAlternatingRowColors(true);
    tree_->setUniformRowHeights(true);
    tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tree_->setDragEnabled(true);
    tree_->setAcceptDrops(true);
    tree_->setDropIndicatorShown(true);
    tree_->setDragDropMode(QAbstractItemView::DragDrop);
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
    tree_->header()->setSectionResizeMode(modeColumn,
                                          QHeaderView::ResizeToContents);
    for (const auto &column : restrictionColumns) {
        tree_->setItemDelegateForColumn(
            column.first,
            new RestrictionIconDelegate(column.first, column.second, tree_));
    }
    tree_->header()->setMinimumSectionSize(22);
    const QVector<int> blenderColumnOrder{
        nameColumn, modeColumn, excludeColumn, selectColumn,
        viewLayerViewportColumn, viewportColumn, renderColumn,
        holdoutColumn, indirectOnlyColumn};
    for (int visualIndex = 0; visualIndex < blenderColumnOrder.size();
         ++visualIndex) {
        const int currentVisualIndex = tree_->header()->visualIndex(
            blenderColumnOrder[visualIndex]);
        if (currentVisualIndex != visualIndex) {
            tree_->header()->moveSection(currentVisualIndex, visualIndex);
        }
    }
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
    sortAlphabetically_ = outlinerSettings.value(
        QStringLiteral("outliner/sortAlphabetically"), true).toBool();
    syncSelectionEnabled_ = outlinerSettings.value(
        QStringLiteral("outliner/syncSelection"), true).toBool();
    showModeColumn_ = outlinerSettings.value(
        QStringLiteral("outliner/showModeColumn"), true).toBool();
    scenesDisplayMode_ = outlinerSettings.value(
        QStringLiteral("outliner/displayMode"),
        QStringLiteral("VIEW_LAYER")).toString() ==
        QLatin1String("SCENES");
    librariesDisplayMode_ = outlinerSettings.value(
        QStringLiteral("outliner/displayMode"),
        QStringLiteral("VIEW_LAYER")).toString() ==
        QLatin1String("LIBRARIES");
    orphanDataDisplayMode_ = outlinerSettings.value(
        QStringLiteral("outliner/displayMode"),
        QStringLiteral("VIEW_LAYER")).toString() ==
        QLatin1String("ORPHAN_DATA");
    sequencerDisplayMode_ = outlinerSettings.value(
        QStringLiteral("outliner/displayMode"),
        QStringLiteral("VIEW_LAYER")).toString() ==
        QLatin1String("SEQUENCE");
    dataApiDisplayMode_ = outlinerSettings.value(
        QStringLiteral("outliner/displayMode"),
        QStringLiteral("VIEW_LAYER")).toString() ==
        QLatin1String("DATA_API");
    libraryOverridesDisplayMode_ = outlinerSettings.value(
        QStringLiteral("outliner/displayMode"),
        QStringLiteral("VIEW_LAYER")).toString() ==
        QLatin1String("LIBRARY_OVERRIDES");
    overrideHierarchiesView_ = outlinerSettings.value(
        QStringLiteral("outliner/libraryOverrideViewMode"),
        QStringLiteral("PROPERTIES")).toString() ==
        QLatin1String("HIERARCHIES");
    showSystemOverrides_ = outlinerSettings.value(
        QStringLiteral("outliner/showSystemOverrides"), false).toBool();
    idTypeFilter_ = outlinerSettings.value(
        QStringLiteral("outliner/idTypeFilter"),
        outlinerSettings.value(
            QStringLiteral("outliner/libraries/typeFilter"))).toString();
    displayModeCombo_->setCurrentIndex(
        libraryOverridesDisplayMode_ ? 6
            : (orphanDataDisplayMode_ ? 5
            : (librariesDisplayMode_ ? 4
                : (dataApiDisplayMode_ ? 3
                    : (sequencerDisplayMode_ ? 2
                        : (scenesDisplayMode_ ? 1 : 0))))));
    libraryOverrideViewCombo_->setCurrentIndex(
        overrideHierarchiesView_ ? 1 : 0);
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
        {QStringLiteral("OTHERS"), QStringLiteral("Others")}};
    for (const auto &type : objectTypes) {
        const QString oldOthersKey = QStringLiteral(
            "outliner/filterObjectType/OTHER");
        const QVariant defaultValue = type.first == QLatin1String("OTHERS")
            ? outlinerSettings.value(oldOthersKey, true)
            : QVariant(true);
        objectTypeFilters_.insert(
            type.first,
            outlinerSettings.value(
                QStringLiteral("outliner/filterObjectType/%1").arg(type.first),
                defaultValue).toBool());
    }

    auto *filterMenu = new QMenu(filterButton_);
    filterButton_->setMenu(filterMenu);
    QMenu *restrictionMenu = filterMenu->addMenu(
        QStringLiteral("Restriction Toggles"));
    restrictionToggleMenuAction_ = restrictionMenu->menuAction();
    const auto addRestrictionMenuAction =
        [this, restrictionMenu](
            const QString &label, const QString &settingsKey, int column,
            RestrictionGlyph glyph, bool checked, bool viewLayerSpecific) {
        QAction *action = restrictionMenu->addAction(
            restrictionIcon(glyph, checked), label);
        action->setCheckable(true);
        action->setChecked(checked);
        action->setProperty("outlinerViewLayerSpecific", viewLayerSpecific);
        connect(action, &QAction::toggled, this,
                [this, settingsKey, column, viewLayerSpecific](bool visible) {
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
        return action;
    };
    connect(restrictionMenu, &QMenu::aboutToShow, this,
            [this, restrictionMenu]() {
        const bool hasActiveViewLayer = callbacks_.activeViewLayerKey &&
            !callbacks_.activeViewLayerKey().isEmpty();
        for (QAction *action : restrictionMenu->actions()) {
            if (!action->isCheckable() ||
                !action->property("outlinerViewLayerSpecific").toBool()) {
                continue;
            }
            action->setEnabled(hasActiveViewLayer);
        }
    });
    viewLayerOnlyRestrictionActions_.append(addRestrictionMenuAction(
        QStringLiteral("Enable / Exclude"),
        QStringLiteral("outliner/showEnableColumn"), excludeColumn,
        RestrictionGlyph::Enable, showEnableColumn_, true));
    addRestrictionMenuAction(QStringLiteral("Select"),
        QStringLiteral("outliner/showSelectColumn"), selectColumn,
        RestrictionGlyph::Select, showSelectColumn_, false);
    addRestrictionMenuAction(QStringLiteral("Hide"),
        QStringLiteral("outliner/showHideColumn"), viewLayerViewportColumn,
        RestrictionGlyph::Hide, showHideColumn_, false);
    addRestrictionMenuAction(QStringLiteral("Viewport"),
        QStringLiteral("outliner/showViewportColumn"), viewportColumn,
        RestrictionGlyph::Viewport, showViewportColumn_, false);
    addRestrictionMenuAction(QStringLiteral("Render"),
        QStringLiteral("outliner/showRenderColumn"), renderColumn,
        RestrictionGlyph::Render, showRenderColumn_, false);
    viewLayerOnlyRestrictionActions_.append(addRestrictionMenuAction(
        QStringLiteral("Holdout"),
        QStringLiteral("outliner/showHoldoutColumn"), holdoutColumn,
        RestrictionGlyph::Holdout, showHoldoutColumn_, true));
    viewLayerOnlyRestrictionActions_.append(addRestrictionMenuAction(
        QStringLiteral("Indirect Only (Cycles)"),
        QStringLiteral("outliner/showIndirectOnlyColumn"), indirectOnlyColumn,
        RestrictionGlyph::IndirectOnly, showIndirectOnlyColumn_, true));

    restrictionToggleSeparatorAction_ = filterMenu->addSeparator();
    sortAlphabeticallyAction_ = filterMenu->addAction(
        QStringLiteral("Sort Alphabetically"));
    sortAlphabeticallyAction_->setCheckable(true);
    sortAlphabeticallyAction_->setChecked(sortAlphabetically_);
    sortAlphabeticallyAction_->setEnabled(!dataApiDisplayMode_);
    connect(sortAlphabeticallyAction_, &QAction::toggled, this,
            [this](bool enabled) {
        sortAlphabetically_ = enabled;
        QSettings settings;
        settings.setValue(QStringLiteral("outliner/sortAlphabetically"),
                          enabled);
        refresh();
    });
    syncSelectionAction_ = filterMenu->addAction(
        QStringLiteral("Sync Selection"));
    syncSelectionAction_->setCheckable(true);
    syncSelectionAction_->setChecked(syncSelectionEnabled_);
    syncSelectionAction_->setEnabled(!libraryOverridesDisplayMode_);
    showModeColumnAction_ = filterMenu->addAction(
        QStringLiteral("Show Mode Column"));
    showModeColumnAction_->setCheckable(true);
    showModeColumnAction_->setChecked(showModeColumn_);
    showModeColumnAction_->setEnabled(!libraryOverridesDisplayMode_);
    connect(showModeColumnAction_, &QAction::toggled, this, [this](bool show) {
        showModeColumn_ = show;
        QSettings settings;
        settings.setValue(QStringLiteral("outliner/showModeColumn"), show);
        tree_->setColumnHidden(modeColumn, !show);
    });
    connect(syncSelectionAction_, &QAction::toggled, this,
            [this](bool enabled) {
        syncSelectionEnabled_ = enabled;
        QSettings settings;
        settings.setValue(QStringLiteral("outliner/syncSelection"), enabled);
        if (scrollToActiveAction_ != nullptr) {
            scrollToActiveAction_->setEnabled(enabled);
        }
        if (!enabled) return;
        if (callbacks_.setObjectSelection && tree_ != nullptr &&
            !sequencerDisplayMode_ && !dataApiDisplayMode_ &&
            !librariesDisplayMode_ && !orphanDataDisplayMode_ &&
            !libraryOverridesDisplayMode_) {
            QVector<ObjectId> selectedIds;
            for (QTreeWidgetItem *item : tree_->selectedItems()) {
                if (item->data(nameColumn, itemKindRole).toInt() !=
                    static_cast<int>(OutlinerRowKind::Object)) {
                    continue;
                }
                const ObjectId id = ObjectId::fromValue(
                    item->data(nameColumn, itemIdRole).toULongLong());
                if (id.isValid() && !selectedIds.contains(id)) {
                    selectedIds.append(id);
                }
            }
            QTreeWidgetItem *currentItem = tree_->currentItem();
            ObjectId activeId = currentItem != nullptr &&
                    currentItem->data(nameColumn, itemKindRole).toInt() ==
                        static_cast<int>(OutlinerRowKind::Object)
                ? ObjectId::fromValue(currentItem->data(
                      nameColumn, itemIdRole).toULongLong())
                : ObjectId::invalid();
            if (!selectedIds.contains(activeId)) {
                activeId = selectedIds.isEmpty() ? ObjectId::invalid()
                                                 : selectedIds.back();
            }
            callbacks_.setObjectSelection(selectedIds, activeId);
        } else if (sequencerDisplayMode_ &&
                   callbacks_.setSequencerSelection) {
            QStringList selectedIds;
            QString activeId;
            for (QTreeWidgetItem *item : tree_->selectedItems()) {
                if (item->data(nameColumn, itemKindRole).toInt() !=
                    static_cast<int>(OutlinerRowKind::SequencerStrip)) {
                    continue;
                }
                const QString id = item->data(
                    nameColumn, itemSequencerStripIdRole).toString();
                if (!id.isEmpty() && !selectedIds.contains(id)) {
                    selectedIds.append(id);
                }
                if (item == tree_->currentItem()) activeId = id;
            }
            callbacks_.setSequencerSelection(selectedIds, activeId);
        }
        syncSelection();
    });

    viewLayerFilterActions_.append(filterMenu->addSeparator());
    QAction *allViewLayersAction = filterMenu->addAction(
        QStringLiteral("All View Layers"));
    viewLayerFilterActions_.append(allViewLayersAction);
    allViewLayersAction->setCheckable(true);
    allViewLayersAction->setChecked(showAllViewLayers_);
    allViewLayersAction->setEnabled(
        !scenesDisplayMode_ && !librariesDisplayMode_ &&
        !orphanDataDisplayMode_ && !sequencerDisplayMode_ &&
        !dataApiDisplayMode_ && !libraryOverridesDisplayMode_);
    connect(allViewLayersAction, &QAction::toggled, this,
            [this](bool visible) {
        showAllViewLayers_ = visible;
        QSettings settings;
        settings.setValue(QStringLiteral("outliner/filterAllViewLayers"),
                          visible);
        refresh();
    });
    viewLayerFilterActions_.append(
        filterMenu->addSection(QStringLiteral("Filter")));
    connect(displayModeCombo_, &QComboBox::activated, this,
            [this, allViewLayersAction](int index) {
        scenesDisplayMode_ = index == 1;
        sequencerDisplayMode_ = index == 2;
        dataApiDisplayMode_ = index == 3;
        librariesDisplayMode_ = index == 4;
        orphanDataDisplayMode_ = index == 5;
        libraryOverridesDisplayMode_ = index == 6;
        QString displayModeKey = QStringLiteral("VIEW_LAYER");
        if (scenesDisplayMode_) {
            displayModeKey = QStringLiteral("SCENES");
        } else if (sequencerDisplayMode_) {
            displayModeKey = QStringLiteral("SEQUENCE");
        } else if (dataApiDisplayMode_) {
            displayModeKey = QStringLiteral("DATA_API");
        } else if (librariesDisplayMode_) {
            displayModeKey = QStringLiteral("LIBRARIES");
        } else if (orphanDataDisplayMode_) {
            displayModeKey = QStringLiteral("ORPHAN_DATA");
        } else if (libraryOverridesDisplayMode_) {
            displayModeKey = QStringLiteral("LIBRARY_OVERRIDES");
        }
        QSettings settings;
        settings.setValue(QStringLiteral("outliner/displayMode"),
                          displayModeKey);
        allViewLayersAction->setEnabled(
            !scenesDisplayMode_ && !librariesDisplayMode_ &&
            !orphanDataDisplayMode_ && !sequencerDisplayMode_ &&
            !dataApiDisplayMode_ && !libraryOverridesDisplayMode_);
        sortAlphabeticallyAction_->setEnabled(!dataApiDisplayMode_);
        syncSelectionAction_->setEnabled(!libraryOverridesDisplayMode_);
        showModeColumnAction_->setEnabled(!libraryOverridesDisplayMode_);
        refresh();
    });
    connect(libraryOverrideViewCombo_, &QComboBox::activated, this,
            [this](int index) {
        overrideHierarchiesView_ = index == 1;
        QSettings settings;
        settings.setValue(QStringLiteral("outliner/libraryOverrideViewMode"),
                          overrideHierarchiesView_
                              ? QStringLiteral("HIERARCHIES")
                              : QStringLiteral("PROPERTIES"));
        refresh();
    });
    systemOverridesAction_ = filterMenu->addAction(
        QStringLiteral("System Overrides"));
    systemOverridesAction_->setCheckable(true);
    systemOverridesAction_->setChecked(showSystemOverrides_);
    systemOverridesAction_->setToolTip(QStringLiteral(
        "Show Blender's system-generated library override properties."));
    connect(systemOverridesAction_, &QAction::toggled, this,
            [this](bool visible) {
        showSystemOverrides_ = visible;
        QSettings settings;
        settings.setValue(QStringLiteral("outliner/showSystemOverrides"), visible);
        refresh();
    });
    connect(idTypeCombo_, &QComboBox::activated, this,
            [this](int index) {
        idTypeFilter_ = idTypeCombo_->itemData(index).toString();
        QSettings settings;
        settings.setValue(QStringLiteral("outliner/idTypeFilter"),
                          idTypeFilter_);
        refresh();
    });
    QAction *collectionsAction = filterMenu->addAction(
        QStringLiteral("Collections"));
    viewLayerFilterActions_.append(collectionsAction);
    collectionsAction->setCheckable(true);
    collectionsAction->setChecked(showCollections_);
    connect(collectionsAction, &QAction::toggled, this, [this](bool visible) {
        showCollections_ = visible;
        QSettings settings;
        settings.setValue(QStringLiteral("outliner/filterCollections"), visible);
        refresh();
    });
    QAction *objectsAction = filterMenu->addAction(QStringLiteral("Objects"));
    viewLayerFilterActions_.append(objectsAction);
    objectsAction->setCheckable(true);
    objectsAction->setChecked(showObjects_);
    QAction *contentsAction = filterMenu->addAction(
        QStringLiteral("Object Contents"));
    viewLayerFilterActions_.append(contentsAction);
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
        if (objectChildrenAction_ != nullptr) {
            objectChildrenAction_->setEnabled(visible);
        }
        for (QAction *action : objectTypeFilterActions_) {
            if (action != nullptr) action->setEnabled(visible);
        }
        QSettings settings;
        settings.setValue(QStringLiteral("outliner/filterObjects"), visible);
        refresh();
    });
    objectChildrenAction_ = filterMenu->addAction(
        QStringLiteral("Object Children"));
    viewLayerFilterActions_.append(objectChildrenAction_);
    objectChildrenAction_->setCheckable(true);
    objectChildrenAction_->setChecked(showObjectChildren_);
    objectChildrenAction_->setEnabled(showObjects_);
    connect(objectChildrenAction_, &QAction::toggled, this, [this](bool visible) {
        showObjectChildren_ = visible;
        QSettings settings;
        settings.setValue(QStringLiteral("outliner/filterObjectChildren"),
                          visible);
        refresh();
    });
    viewLayerFilterActions_.append(filterMenu->addSeparator());
    scrollToActiveAction_ = filterMenu->addAction(
        QStringLiteral("Scroll to Active"));
    scrollToActiveAction_->setCheckable(true);
    scrollToActiveAction_->setChecked(scrollToActive_);
    scrollToActiveAction_->setEnabled(syncSelectionEnabled_);
    connect(scrollToActiveAction_, &QAction::toggled, this,
            [this](bool enabled) {
        scrollToActive_ = enabled;
        QSettings settings;
        settings.setValue(QStringLiteral("outliner/scrollToActive"), enabled);
        syncSelection();
    });
    viewLayerFilterActions_.append(filterMenu->addSeparator());
    viewLayerFilterActions_.append(
        filterMenu->addSection(QStringLiteral("Object State")));
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
        viewLayerFilterActions_.append(action);
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
    viewLayerFilterActions_.append(invertStateAction);
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
    viewLayerFilterActions_.append(filterMenu->addSeparator());
    objectTypeFilterSection_ = filterMenu->addSection(
        QStringLiteral("Object Types"));
    viewLayerFilterActions_.append(objectTypeFilterSection_);
    for (const auto &type : objectTypes) {
        QAction *action = filterMenu->addAction(type.second);
        action->setCheckable(true);
        action->setChecked(objectTypeFilters_.value(type.first, true));
        objectTypeFilterActions_.insert(type.first, action);
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
    viewLayerFilterActions_.append(filterMenu->addSeparator());
    searchFilterActions_.append(
        filterMenu->addSection(QStringLiteral("Search")));
    QAction *exactMatchAction = filterMenu->addAction(
        QStringLiteral("Exact Match"));
    searchFilterActions_.append(exactMatchAction);
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
    searchFilterActions_.append(caseSensitiveAction);
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
        addColumnToggle(QStringLiteral("Hide"),
                        showHideColumn_, viewLayerViewportColumn,
                        QStringLiteral("outliner/showHideColumn"), false);
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
    connect(dataApiKeyingSetCombo_, &QComboBox::activated, this,
            [this](int index) {
        if (index < 0 || !callbacks_.setActiveKeyingSet) return;
        callbacks_.setActiveKeyingSet(
            dataApiKeyingSetCombo_->itemData(index).toString());
        refresh();
    });
    connect(addKeyingSetPathsButton_, &QToolButton::clicked, this,
            [this]() { editSelectedDataApiKeyingSetPaths(true); });
    connect(removeKeyingSetPathsButton_, &QToolButton::clicked, this,
            [this]() { editSelectedDataApiKeyingSetPaths(false); });
    connect(addDataApiDriverButton_, &QToolButton::clicked, this,
            [this]() { editSelectedDataApiDriverPaths(true); });
    connect(removeDataApiDriverButton_, &QToolButton::clicked, this,
            [this]() { editSelectedDataApiDriverPaths(false); });
    connect(sceneCombo_, &QComboBox::activated, this, [this](int index) {
        if (index < 0 || !callbacks_.setActiveScene) return;
        const QString key = sceneCombo_->itemData(index).toString();
        if (!key.isEmpty()) callbacks_.setActiveScene(key);
    });
    connect(tree_, &QTreeWidget::itemExpanded, this,
            [this](QTreeWidgetItem *item) {
                populateDataApiChildren(item);
                saveTreeState();
            });
    connect(tree_, &QTreeWidget::itemCollapsed, this,
            [this](QTreeWidgetItem *) { saveTreeState(); });
    connect(tree_, &QTreeWidget::itemClicked, this,
            [this](QTreeWidgetItem *item, int column) {
        if (column != modeColumn || item == nullptr ||
            !callbacks_.setObjectInteractionMode ||
            item->data(nameColumn, itemKindRole).toInt() !=
                static_cast<int>(OutlinerRowKind::Object)) {
            return;
        }
        const ObjectId id = ObjectId::fromValue(
            item->data(nameColumn, itemIdRole).toULongLong());
        if (!id.isValid()) return;
        const int currentMode = item->data(
            modeColumn, itemModeValueRole).toInt();
        callbacks_.setObjectInteractionMode(
            id, currentMode == 1 ? 0 : 1);
    });

    outlinerTree->setActivationHandlers(
        [this](QTreeWidgetItem *item, Qt::KeyboardModifiers modifiers) {
            return activateObjectRow(item, modifiers);
        },
        [this]() {
            if (syncSelectionEnabled_ && !librariesDisplayMode_ &&
                !orphanDataDisplayMode_ &&
                !sequencerDisplayMode_ &&
                !dataApiDisplayMode_ &&
                callbacks_.setObjectSelection) {
                callbacks_.setObjectSelection({}, ObjectId::invalid());
            }
            if (syncSelectionEnabled_ && sequencerDisplayMode_ &&
                callbacks_.setSequencerSelection) {
                QTimer::singleShot(0, this, [this]() {
                    if (callbacks_.setSequencerSelection) {
                        callbacks_.setSequencerSelection({}, {});
                    }
                });
            }
            rangeAnchorItem_ = nullptr;
        },
        [this]() { syncSelection(); },
        [this](const QVector<OutlinerRowReference> &sources,
               const OutlinerRowReference &target,
               bool link,
               bool parent,
               OutlinerDropPosition position) {
            if (callbacks_.dropRows) {
                callbacks_.dropRows(sources, target, link, parent, position);
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
                } else if (column == viewLayerViewportColumn) {
                    if (!viewLayerKey.isEmpty() &&
                        callbacks_.setObjectViewLayerHidden) {
                        callbacks_.setObjectViewLayerHidden(
                            id, viewLayerKey, hidden, recursive);
                    } else if (viewLayerKey.isEmpty() &&
                               callbacks_.setObjectHiddenInViewport) {
                        callbacks_.setObjectHiddenInViewport(
                            id, hidden, recursive);
                    }
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
    const bool viewLayerMode = !scenesDisplayMode_ && !sequencerDisplayMode_ &&
        !dataApiDisplayMode_ && !librariesDisplayMode_ &&
        !orphanDataDisplayMode_ && !libraryOverridesDisplayMode_;
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
                                      activeViewLayerKey, viewLayerMode](
                                         const QTreeWidgetItem *item) {
        if (item == nullptr) return false;
        const quint64 objectValue = item->data(
            nameColumn, itemIdRole).toULongLong();
        const OutlinerRowKind kind = static_cast<OutlinerRowKind>(
            item->data(nameColumn, itemKindRole).toInt());
        const OutlinerObjectEntry entry = kind == OutlinerRowKind::SourceObject
            ? sourceOnlyEntries_.value(item->data(
                  nameColumn, itemSourceObjectIdRole).toString())
            : objectEntries_.value(objectValue);
        if (kind == OutlinerRowKind::SourceObject) {
            if (!entry.outlinerOnly || entry.sourceObjectId.isEmpty()) {
                return false;
            }
        } else if (!entry.id.isValid()) {
            return false;
        }
        if (!viewLayerMode) return true;
        const QString typeKey =
            outlinerObjectTypeFilterKey(entry.blenderType);
        if (!objectTypeFilters_.value(typeKey, true)) return false;

        const QString rowViewLayerKey = item->data(
            nameColumn, itemViewLayerKeyRole).toString();
        bool visible = entry.visible;
        bool selectable = entry.selectable;
        const bool hasLayerVisibilityCallback = entry.id.isValid() &&
            !rowViewLayerKey.isEmpty() &&
            static_cast<bool>(callbacks_.objectVisibleInViewLayer);
        const bool hasLayerSelectableCallback = entry.id.isValid() &&
            !rowViewLayerKey.isEmpty() &&
            static_cast<bool>(callbacks_.objectSelectableInViewLayer);
        if (hasLayerVisibilityCallback) {
            visible = callbacks_.objectVisibleInViewLayer(
                entry.id, rowViewLayerKey);
        } else if (!rowViewLayerKey.isEmpty() &&
                   rowViewLayerKey != activeViewLayerKey) {
            const bool hiddenInLayer = entry.viewLayerHidden.contains(
                rowViewLayerKey)
                ? entry.viewLayerHidden.value(rowViewLayerKey).toBool()
                : entry.hiddenInViewport;
            visible = !entry.hideViewport &&
                !hiddenInLayer;
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
            matchesState = entry.id.isValid() &&
                           selectedValues.contains(objectValue);
            break;
        case OutlinerObjectStateFilter::Active:
            matchesState = entry.id.isValid() && activeId.isValid() &&
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
        const bool nameMatches = text.isEmpty() ||
            matcher.match(item->text(nameColumn)).hasMatch();
        const bool searchChildren = text.isEmpty() ||
            !dataApiDisplayMode_ || item->isExpanded() || nameMatches;
        bool childMatches = false;
        if (searchChildren) {
            for (int index = 0; index < item->childCount(); ++index) {
                childMatches = self(self, item->child(index)) || childMatches;
            }
        }
        bool eligible = true;
        const OutlinerRowKind kind = static_cast<OutlinerRowKind>(
            item->data(nameColumn, itemKindRole).toInt());
        if (viewLayerMode && kind == OutlinerRowKind::Collection) {
            eligible = showCollections_;
        } else if (viewLayerMode &&
                   (kind == OutlinerRowKind::Object ||
                    kind == OutlinerRowKind::SourceObject)) {
            eligible = showObjects_ && objectMatchesFilter(item);
        }
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
    QString expansionMode = QStringLiteral("expandedPaths");
    if (scenesDisplayMode_) {
        expansionMode = QStringLiteral("scenesExpandedPaths");
    } else if (sequencerDisplayMode_) {
        expansionMode = QStringLiteral("sequenceExpandedPaths");
    } else if (dataApiDisplayMode_) {
        expansionMode = QStringLiteral("dataApiExpandedPaths");
    } else if (libraryOverridesDisplayMode_) {
        expansionMode = QStringLiteral("libraryOverrides%1ExpandedPaths")
            .arg(overrideHierarchiesView_
                     ? QStringLiteral("Hierarchies")
                     : QStringLiteral("Properties"));
    } else if (librariesDisplayMode_) {
        expansionMode = QStringLiteral("librariesExpandedPaths");
    } else if (orphanDataDisplayMode_) {
        expansionMode = QStringLiteral("orphanDataExpandedPaths");
    }
    const QString settingsKey = QStringLiteral("outliner/%1/%2")
                                    .arg(scopeHash, expansionMode);
    bool hasSavedState = false;
    if (scenesDisplayMode_ || librariesDisplayMode_ ||
        orphanDataDisplayMode_ || sequencerDisplayMode_ ||
        dataApiDisplayMode_ || libraryOverridesDisplayMode_) {
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
        if (!path.isEmpty()) {
            const bool pointerStruct =
                dataApiDisplayMode_ && isDataApiPointerStruct(item);
            const bool explicitlyCollapsed = pointerStruct &&
                dataApiCollapsedPaths_.contains(path);
            item->setExpanded(!explicitlyCollapsed &&
                (expanded.contains(path) || pointerStruct));
        }
        for (int index = 0; index < item->childCount(); ++index) {
            pending.append(item->child(index));
        }
    }
    restoringTreeState_ = false;
}

void CollectionsPanel::populateDataApiChildren(QTreeWidgetItem *item)
{
    if (item == nullptr ||
        item->data(nameColumn, itemKindRole).toInt() !=
            static_cast<int>(OutlinerRowKind::DataApi) ||
        item->data(nameColumn, itemDataApiChildrenBuiltRole).toBool()) {
        return;
    }
    const QJsonObject source = item->data(
        nameColumn, itemDataApiNodeRole).toJsonObject();
    item->setData(nameColumn, itemDataApiChildrenBuiltRole, true);
    while (item->childCount() > 0 &&
           item->child(0)->data(nameColumn,
                                itemDataApiPlaceholderRole).toBool()) {
        delete item->takeChild(0);
    }

    for (const QJsonValue &childValue : source.value(
             QStringLiteral("children")).toArray()) {
        if (!childValue.isObject()) continue;
        const QJsonObject child = childValue.toObject();
        const QString kind = child.value(
            QStringLiteral("kind")).toString();
        const QString key = child.value(
            QStringLiteral("key")).toString();
        const QString path = item->data(nameColumn, itemPathRole).toString() +
            QLatin1Char('/') + kind.toLower() + QLatin1Char(':') + key;
        auto *row = new QTreeWidgetItem;
        row->setText(nameColumn, child.value(
            QStringLiteral("name")).toString());
        row->setData(nameColumn, itemKindRole,
                     static_cast<int>(OutlinerRowKind::DataApi));
        row->setData(nameColumn, itemPathRole, path);
        row->setData(nameColumn, itemDataApiKindRole, kind);
        row->setData(nameColumn, itemDataApiNodeRole,
                     QVariant::fromValue(child));
        const QString type = child.value(
            QStringLiteral("type")).toString();
        row->setData(nameColumn, itemDataApiTypeRole, type);
        row->setToolTip(
            nameColumn,
            type.isEmpty()
                ? QStringLiteral("Blender RNA %1").arg(kind)
                : QStringLiteral("Blender RNA %1 (%2)").arg(kind, type));
        row->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);

        const bool hasChildren = !child.value(
            QStringLiteral("children")).toArray().isEmpty();
        if (hasChildren) {
            auto *placeholder = new QTreeWidgetItem(row);
            placeholder->setData(nameColumn, itemDataApiPlaceholderRole, true);
            placeholder->setFlags(Qt::NoItemFlags);
        }
        const bool pointerStruct =
            kind == QLatin1String("STRUCT") &&
            item->data(nameColumn, itemDataApiKindRole).toString() ==
                QLatin1String("PROPERTY") &&
            item->data(nameColumn, itemDataApiTypeRole).toString() ==
                QLatin1String("POINTER");
        item->addChild(row);
        if (hasChildren && !dataApiCollapsedPaths_.contains(path) &&
            (dataApiExpandedPaths_.contains(path) || pointerStruct)) {
            row->setExpanded(true);
        }
    }

    if (item->data(nameColumn, itemDataApiTruncatedRole).toBool()) {
        auto *truncatedRow = new QTreeWidgetItem(item);
        truncatedRow->setText(
            nameColumn,
            QStringLiteral("Additional RNA data omitted"));
        truncatedRow->setData(nameColumn, itemKindRole,
                              static_cast<int>(OutlinerRowKind::DataApi));
        truncatedRow->setData(nameColumn, itemPathRole,
                              QStringLiteral("data-api/truncated"));
        truncatedRow->setToolTip(
            nameColumn,
            QStringLiteral("The 20,000-row or 32-level capture limit was reached"));
        truncatedRow->setFlags(Qt::ItemIsEnabled);
    }
}

void CollectionsPanel::updateDataApiAnimationControls()
{
    const bool visible = dataApiDisplayMode_;
    dataApiKeyingSetCombo_->setVisible(visible);
    addKeyingSetPathsButton_->setVisible(visible);
    removeKeyingSetPathsButton_->setVisible(visible);
    addDataApiDriverButton_->setVisible(visible);
    removeDataApiDriverButton_->setVisible(visible);
    addKeyingSetPathsButton_->setEnabled(
        visible && static_cast<bool>(callbacks_.editKeyingSetPaths));
    addDataApiDriverButton_->setEnabled(
        visible && static_cast<bool>(callbacks_.editDataApiDriverPaths));
    removeDataApiDriverButton_->setEnabled(
        visible && static_cast<bool>(callbacks_.editDataApiDriverPaths));

    const QJsonObject scene = callbacks_.activeSceneRecord
        ? callbacks_.activeSceneRecord() : QJsonObject{};
    const QString activeKey = scene.value(
        QStringLiteral("active_keying_set_key")).toString();
    const QSignalBlocker blocker(dataApiKeyingSetCombo_);
    dataApiKeyingSetCombo_->clear();
    for (const QJsonValue &keyingSetValue : scene.value(
             QStringLiteral("keying_sets")).toArray()) {
        if (!keyingSetValue.isObject()) continue;
        const QJsonObject keyingSet = keyingSetValue.toObject();
        const QString key = keyingSet.value(QStringLiteral("key")).toString();
        if (key.isEmpty()) continue;
        QString name = keyingSet.value(QStringLiteral("name")).toString();
        if (name.isEmpty()) name = QStringLiteral("Keying Set");
        dataApiKeyingSetCombo_->addItem(name, key);
    }
    if (activeKey.isEmpty()) {
        dataApiKeyingSetCombo_->insertItem(
            0, QStringLiteral("No Keying Set Active"), QString{});
    } else if (dataApiKeyingSetCombo_->count() == 0) {
        dataApiKeyingSetCombo_->addItem(
            QStringLiteral("No Keying Set Active"), QString{});
    }
    int activeIndex = dataApiKeyingSetCombo_->findData(activeKey);
    if (activeIndex < 0) activeIndex = 0;
    dataApiKeyingSetCombo_->setCurrentIndex(activeIndex);
    dataApiKeyingSetCombo_->setEnabled(
        visible && dataApiKeyingSetCombo_->count() > 1 &&
        static_cast<bool>(callbacks_.setActiveKeyingSet));
    removeKeyingSetPathsButton_->setEnabled(
        visible && static_cast<bool>(callbacks_.editKeyingSetPaths));
}

void CollectionsPanel::editSelectedDataApiKeyingSetPaths(bool add)
{
    if (!dataApiDisplayMode_ || tree_ == nullptr ||
        !callbacks_.editKeyingSetPaths) {
        return;
    }
    QJsonArray paths;
    QSet<QString> uniquePaths;
    for (QTreeWidgetItem *item : tree_->selectedItems()) {
        if (item == nullptr || item->data(nameColumn, itemKindRole).toInt() !=
                static_cast<int>(OutlinerRowKind::DataApi)) {
            continue;
        }
        const QJsonObject node = item->data(
            nameColumn, itemDataApiNodeRole).toJsonObject();
        const QJsonValue animationPathValue = node.value(
            QStringLiteral("animation_path"));
        if (!animationPathValue.isObject()) continue;
        QJsonObject path = animationPathValue.toObject();
        const QJsonValue idType = path.value(QStringLiteral("id_type"));
        const QJsonValue idName = path.value(QStringLiteral("id_name"));
        const QJsonValue dataPath = path.value(QStringLiteral("data_path"));
        const QJsonValue arrayIndex = path.value(
            QStringLiteral("array_index"));
        const QJsonValue entireArray = path.value(
            QStringLiteral("use_entire_array"));
        if (!idType.isString() || idType.toString().isEmpty() ||
            !idName.isString() || !dataPath.isString() ||
            dataPath.toString().isEmpty() || !arrayIndex.isDouble() ||
            arrayIndex.toInt(-1) < 0 || arrayIndex.toInt(-1) > 32766 ||
            !entireArray.isBool()) {
            continue;
        }
        path.insert(QStringLiteral("group"), QString{});
        path.insert(QStringLiteral("group_method"),
                    QStringLiteral("KEYINGSET"));
        const QString identity = path.value(QStringLiteral("id_type")).toString() +
            QLatin1Char('\x1f') +
            path.value(QStringLiteral("id_name")).toString() +
            QLatin1Char('\x1f') +
            path.value(QStringLiteral("data_path")).toString() +
            QLatin1Char('\x1f') +
            QString::number(path.value(QStringLiteral("array_index")).toInt()) +
            QLatin1Char('\x1f') +
            (path.value(QStringLiteral("use_entire_array")).toBool()
                 ? QStringLiteral("1") : QStringLiteral("0"));
        if (uniquePaths.contains(identity)) continue;
        uniquePaths.insert(identity);
        paths.append(path);
    }
    const QString currentKey = dataApiKeyingSetCombo_->currentData().toString();
    callbacks_.editKeyingSetPaths(currentKey, paths, add);
    refresh();
}

void CollectionsPanel::editSelectedDataApiDriverPaths(bool add)
{
    if (!dataApiDisplayMode_ || tree_ == nullptr ||
        !callbacks_.editDataApiDriverPaths) {
        return;
    }
    QJsonArray paths;
    QSet<QString> uniquePaths;
    const auto appendPath = [&paths, &uniquePaths](QJsonObject path) {
        const QJsonValue idType = path.value(QStringLiteral("id_type"));
        const QJsonValue idName = path.value(QStringLiteral("id_name"));
        const QJsonValue dataPath = path.value(QStringLiteral("data_path"));
        const QJsonValue arrayIndex = path.value(QStringLiteral("array_index"));
        if (!idType.isString() || idType.toString().isEmpty() ||
            !idName.isString() || idName.toString().isEmpty() ||
            !dataPath.isString() || dataPath.toString().isEmpty() ||
            !arrayIndex.isDouble() || arrayIndex.toInt(-1) < 0 ||
            arrayIndex.toInt(-1) > 32766) {
            return;
        }
        path.insert(QStringLiteral("use_entire_array"), false);
        const QString identity = idType.toString() + QLatin1Char('\x1f') +
            idName.toString() + QLatin1Char('\x1f') + dataPath.toString() +
            QLatin1Char('\x1f') + QString::number(arrayIndex.toInt());
        if (uniquePaths.contains(identity)) return;
        uniquePaths.insert(identity);
        paths.append(path);
    };
    for (QTreeWidgetItem *item : tree_->selectedItems()) {
        if (item == nullptr || item->data(nameColumn, itemKindRole).toInt() !=
                static_cast<int>(OutlinerRowKind::DataApi)) {
            continue;
        }
        const QJsonObject node = item->data(
            nameColumn, itemDataApiNodeRole).toJsonObject();
        const QJsonValue animationPathValue = node.value(
            QStringLiteral("animation_path"));
        if (!animationPathValue.isObject()) continue;
        const QJsonObject animationPath = animationPathValue.toObject();
        if (animationPath.value(QStringLiteral("use_entire_array")).toBool()) {
            bool addedComponent = false;
            for (const QJsonValue &childValue : node.value(
                     QStringLiteral("children")).toArray()) {
                if (!childValue.isObject()) continue;
                const QJsonObject child = childValue.toObject();
                if (child.value(QStringLiteral("kind")).toString() !=
                    QLatin1String("ARRAY_ELEM")) {
                    continue;
                }
                const QJsonValue componentPathValue = child.value(
                    QStringLiteral("animation_path"));
                if (!componentPathValue.isObject()) continue;
                appendPath(componentPathValue.toObject());
                addedComponent = true;
            }
            if (!addedComponent) continue;
        } else {
            appendPath(animationPath);
        }
    }
    if (paths.isEmpty()) return;
    callbacks_.editDataApiDriverPaths(paths, add);
    refresh();
}

void CollectionsPanel::saveTreeState()
{
    if (tree_ == nullptr || refreshing_ || restoringTreeState_ ||
        !activeFilterText_.isEmpty()) {
        return;
    }
    QStringList expandedPaths;
    QStringList collapsedDataApiPaths;
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
        if (dataApiDisplayMode_ && isDataApiPointerStruct(item) &&
            !item->isExpanded()) {
            const QString path = item->data(nameColumn, itemPathRole).toString();
            if (!path.isEmpty()) collapsedDataApiPaths.append(path);
        }
        for (int index = 0; index < item->childCount(); ++index) {
            pending.append(item->child(index));
        }
    }
    expandedPaths.sort(Qt::CaseSensitive);
    expandedPaths.removeDuplicates();
    collapsedDataApiPaths.sort(Qt::CaseSensitive);
    collapsedDataApiPaths.removeDuplicates();
    if (dataApiDisplayMode_) {
        dataApiExpandedPaths_ = QSet<QString>(expandedPaths.cbegin(),
                                               expandedPaths.cend());
        dataApiCollapsedPaths_ = QSet<QString>(
            collapsedDataApiPaths.cbegin(), collapsedDataApiPaths.cend());
    }
    if (scenesDisplayMode_ || librariesDisplayMode_ ||
        orphanDataDisplayMode_ || sequencerDisplayMode_ ||
        dataApiDisplayMode_ || libraryOverridesDisplayMode_) {
        const QString scope = callbacks_.stateScopeKey
            ? callbacks_.stateScopeKey() : QString{};
        if (scope.isEmpty()) return;
        const QString scopeHash = QString::fromLatin1(
            QCryptographicHash::hash(scope.toUtf8(),
                                     QCryptographicHash::Sha256)
                .toHex());
        QSettings settings;
        QString modeKey = QStringLiteral("orphanData");
        if (scenesDisplayMode_) {
            modeKey = QStringLiteral("scenes");
        } else if (sequencerDisplayMode_) {
            modeKey = QStringLiteral("sequence");
        } else if (dataApiDisplayMode_) {
            modeKey = QStringLiteral("dataApi");
        } else if (libraryOverridesDisplayMode_) {
            modeKey = QStringLiteral("libraryOverrides%1")
                .arg(overrideHierarchiesView_
                         ? QStringLiteral("Hierarchies")
                         : QStringLiteral("Properties"));
        } else if (librariesDisplayMode_) {
            modeKey = QStringLiteral("libraries");
        }
        settings.setValue(
            QStringLiteral("outliner/%1/%2ExpandedPaths")
                .arg(scopeHash, modeKey),
            expandedPaths);
        if (dataApiDisplayMode_) {
            settings.setValue(
                QStringLiteral("outliner/%1/dataApiCollapsedPaths")
                    .arg(scopeHash),
                collapsedDataApiPaths);
        }
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
    if (kind == OutlinerRowKind::SequencerStrip) {
        return sequencerDisplayMode_;
    }
    if (kind == OutlinerRowKind::Collection) {
        const CollectionId clickedId = CollectionId::fromValue(
            item->data(nameColumn, itemIdRole).toULongLong());
        const QString rowViewLayerKey = item->data(
            nameColumn, itemViewLayerKeyRole).toString();
        const QStringList sourcePath = item->data(
            nameColumn, itemSourcePathRole).toStringList();
        if (!clickedId.isValid() || sourcePath.isEmpty() ||
            !callbacks_.setActiveCollection) {
            return false;
        }
        QTimer::singleShot(0, this,
                           [this, clickedId, rowViewLayerKey, sourcePath]() {
            if (callbacks_.setActiveViewLayer &&
                callbacks_.activeViewLayerKey &&
                !rowViewLayerKey.isEmpty() &&
                callbacks_.activeViewLayerKey() != rowViewLayerKey) {
                callbacks_.setActiveViewLayer(rowViewLayerKey);
            }
            if (callbacks_.setActiveCollection) {
                callbacks_.setActiveCollection(clickedId, rowViewLayerKey,
                                              sourcePath);
            }
        });
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
    if (!syncSelectionEnabled_) {
        tree_->setCurrentItem(item, nameColumn,
                              QItemSelectionModel::NoUpdate);
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
    if (item != nullptr && tree_ != nullptr) {
        if (!item->isSelected()) {
            tree_->clearSelection();
            item->setSelected(true);
        }
        tree_->setCurrentItem(item, nameColumn,
                              QItemSelectionModel::NoUpdate);
        rangeAnchorItem_ = item;

        const OutlinerRowKind contextRowKind =
            static_cast<OutlinerRowKind>(
                item->data(nameColumn, itemKindRole).toInt());
        if (syncSelectionEnabled_ &&
            contextRowKind == OutlinerRowKind::Object &&
            callbacks_.setObjectSelection) {
            QVector<ObjectId> selectedObjectIds;
            for (QTreeWidgetItem *selected : tree_->selectedItems()) {
                if (selected->data(nameColumn, itemKindRole).toInt() !=
                    static_cast<int>(OutlinerRowKind::Object)) {
                    continue;
                }
                const ObjectId selectedId = ObjectId::fromValue(
                    selected->data(nameColumn, itemIdRole).toULongLong());
                if (selectedId.isValid() &&
                    !selectedObjectIds.contains(selectedId)) {
                    selectedObjectIds.append(selectedId);
                }
            }
            const ObjectId activeObjectId = ObjectId::fromValue(
                item->data(nameColumn, itemIdRole).toULongLong());
            callbacks_.setObjectSelection(selectedObjectIds,
                                           activeObjectId);
        } else {
            activateObjectRow(item, Qt::NoModifier);
        }
    }
    const auto createCollection = [this](CollectionId parentId) {
        if (!callbacks_.createCollection) return;
        QSet<QString> existingNames;
        if (callbacks_.entries) {
            for (const CollectionPanelEntry &entry : callbacks_.entries()) {
                existingNames.insert(entry.collection.name);
            }
        }
        QString name = QStringLiteral("Collection");
        for (int suffix = 1; existingNames.contains(name); ++suffix) {
            name = QStringLiteral("Collection.%1")
                       .arg(suffix, 3, 10, QLatin1Char('0'));
        }
        callbacks_.createCollection(name, parentId);
    };
    const auto collectionParentFromSelection = [this](
        const QString &viewLayerKey) {
        CollectionId parentId = CollectionId::invalid();
        int selectedCollectionCount = 0;
        if (tree_ != nullptr) {
            for (QTreeWidgetItem *selected : tree_->selectedItems()) {
                if (selected->data(nameColumn, itemKindRole).toInt() !=
                    static_cast<int>(OutlinerRowKind::Collection)) {
                    continue;
                }
                const QString selectedViewLayerKey = selected->data(
                    nameColumn, itemViewLayerKeyRole).toString();
                if (!viewLayerKey.isEmpty() &&
                    selectedViewLayerKey != viewLayerKey) {
                    continue;
                }
                ++selectedCollectionCount;
                if (selectedCollectionCount > 1) {
                    return qMakePair(false, CollectionId::invalid());
                }
                parentId = CollectionId::fromValue(selected->data(
                    nameColumn, itemIdRole).toULongLong());
            }
        }
        return qMakePair(true, parentId);
    };
    const auto addNewCollectionAction = [this, &createCollection,
                                         &collectionParentFromSelection](
        QMenu &menu, const QString &viewLayerKey) {
        if (!callbacks_.createCollection) return;
        QAction *create = menu.addAction(QStringLiteral("New Collection"));
        const auto selection = collectionParentFromSelection(viewLayerKey);
        create->setEnabled(selection.first);
        if (!selection.first) {
            create->setToolTip(QStringLiteral(
                "Select no more than one collection to create a nested collection"));
            return;
        }
        connect(create, &QAction::triggered, this,
                [createCollection, parentId = selection.second]() {
            createCollection(parentId);
        });
    };
    const auto activeCollectionId = [this]() {
        if (!callbacks_.activeCollectionSourcePath || !callbacks_.entries) {
            return CollectionId::invalid();
        }
        const QStringList sourcePath =
            callbacks_.activeCollectionSourcePath();
        if (sourcePath.isEmpty()) return CollectionId::invalid();
        for (const CollectionPanelEntry &entry : callbacks_.entries()) {
            if (entry.collection.sourceId == sourcePath.last()) {
                return entry.collection.id;
            }
        }
        return CollectionId::invalid();
    };
    const auto addPasteDataBlocksAction = [this, activeCollectionId](
        QMenu &menu, CollectionId parentId) {
        const bool hasCollectionClipboard =
            callbacks_.hasOutlinerCollectionClipboard &&
            callbacks_.hasOutlinerCollectionClipboard();
        const bool hasObjectClipboard = callbacks_.hasOutlinerObjectClipboard &&
                                        callbacks_.hasOutlinerObjectClipboard();
        if (!callbacks_.pasteOutlinerCollection &&
            !callbacks_.pasteOutlinerObjects) {
            return;
        }
        QAction *paste = menu.addAction(QStringLiteral("Paste Data-Blocks"));
        paste->setEnabled(hasCollectionClipboard || hasObjectClipboard);
        connect(paste, &QAction::triggered, this,
                [this, parentId, activeCollectionId]() {
            const CollectionId destinationParentId = parentId.isValid()
                ? parentId : activeCollectionId();
            if (callbacks_.hasOutlinerCollectionClipboard &&
                callbacks_.hasOutlinerCollectionClipboard() &&
                callbacks_.pasteOutlinerCollection) {
                callbacks_.pasteOutlinerCollection(destinationParentId);
            } else if (callbacks_.hasOutlinerObjectClipboard &&
                       callbacks_.hasOutlinerObjectClipboard() &&
                       callbacks_.pasteOutlinerObjects) {
                callbacks_.pasteOutlinerObjects();
            }
        });
    };
    if (item == nullptr) {
        if (tree_ == nullptr) return;
        QMenu menu(this);
        const QString activeViewLayerKey = callbacks_.activeViewLayerKey
            ? callbacks_.activeViewLayerKey() : QString{};
        addNewCollectionAction(menu, activeViewLayerKey);
        addPasteDataBlocksAction(menu, CollectionId::invalid());
        menu.addSeparator();
        QMenu *viewMenu = menu.addMenu(QStringLiteral("View"));
        const auto treeRows = [this]() {
            QVector<QPair<QTreeWidgetItem *, int>> rows;
            if (tree_ == nullptr) return rows;
            std::function<void(QTreeWidgetItem *, int)> appendRows =
                [&rows, &appendRows](QTreeWidgetItem *parent, int depth) {
                    rows.append({parent, depth});
                    for (int childIndex = 0;
                         childIndex < parent->childCount(); ++childIndex) {
                        appendRows(parent->child(childIndex), depth + 1);
                    }
                };
            for (int index = 0; index < tree_->topLevelItemCount(); ++index) {
                appendRows(tree_->topLevelItem(index), 1);
            }
            return rows;
        };
        const ObjectId activeObjectId = callbacks_.activeObjectId
            ? callbacks_.activeObjectId() : ObjectId::invalid();
        QAction *showActive = viewMenu->addAction(
            QStringLiteral("Show Active"));
        showActive->setEnabled(activeObjectId.isValid());
        connect(showActive, &QAction::triggered, this,
                [this, activeObjectId]() {
            if (!activeObjectId.isValid() || tree_ == nullptr) return;
            QVector<QTreeWidgetItem *> pending;
            for (int index = 0; index < tree_->topLevelItemCount(); ++index) {
                pending.append(tree_->topLevelItem(index));
            }
            while (!pending.isEmpty()) {
                QTreeWidgetItem *candidate = pending.takeLast();
                if (candidate->data(nameColumn, itemKindRole).toInt() ==
                        static_cast<int>(OutlinerRowKind::Object) &&
                    ObjectId::fromValue(candidate->data(
                        nameColumn, itemIdRole).toULongLong()) ==
                        activeObjectId) {
                    for (QTreeWidgetItem *ancestor = candidate->parent();
                         ancestor != nullptr; ancestor = ancestor->parent()) {
                        if (!ancestor->isHidden()) ancestor->setExpanded(true);
                    }
                    tree_->scrollToItem(candidate,
                                        QAbstractItemView::PositionAtCenter);
                    tree_->setCurrentItem(candidate, nameColumn,
                                          QItemSelectionModel::NoUpdate);
                    return;
                }
                for (int childIndex = 0;
                     childIndex < candidate->childCount(); ++childIndex) {
                    pending.append(candidate->child(childIndex));
                }
            }
        });
        viewMenu->addSeparator();

        QAction *expandedToggle = viewMenu->addAction(
            QStringLiteral("Expand/Collapse All"));
        expandedToggle->setEnabled(!dataApiDisplayMode_);
        connect(expandedToggle, &QAction::triggered, this,
                [this, treeRows]() {
            if (tree_ == nullptr || dataApiDisplayMode_) return;
            const QVector<QPair<QTreeWidgetItem *, int>> rows = treeRows();
            const bool anyCollapsed = std::any_of(
                rows.cbegin(), rows.cend(), [](const auto &row) {
                    return row.first->childCount() > 0 &&
                           !row.first->isExpanded();
                });
            {
                const QSignalBlocker blocker(tree_);
                if (anyCollapsed) {
                    tree_->expandAll();
                } else {
                    tree_->collapseAll();
                }
            }
            saveTreeState();
        });

        QAction *showHierarchy = viewMenu->addAction(
            QStringLiteral("Show Object Hierarchy"));
        showHierarchy->setEnabled(!dataApiDisplayMode_);
        connect(showHierarchy, &QAction::triggered, this,
                [this, treeRows]() {
            if (tree_ == nullptr || dataApiDisplayMode_) return;
            const QString activeSceneKey = callbacks_.activeSceneKey
                ? callbacks_.activeSceneKey() : QString{};
            const QString activeViewLayerKey = callbacks_.activeViewLayerKey
                ? callbacks_.activeViewLayerKey() : QString{};
            const QVector<QPair<QTreeWidgetItem *, int>> rows = treeRows();
            const auto containsObject = [](QTreeWidgetItem *root) {
                QVector<QTreeWidgetItem *> pending;
                for (int index = 0; index < root->childCount(); ++index) {
                    pending.append(root->child(index));
                }
                while (!pending.isEmpty()) {
                    QTreeWidgetItem *candidate = pending.takeLast();
                    if (candidate->data(nameColumn, itemKindRole).toInt() ==
                        static_cast<int>(OutlinerRowKind::Object)) {
                        return true;
                    }
                    for (int index = 0; index < candidate->childCount(); ++index) {
                        pending.append(candidate->child(index));
                    }
                }
                return false;
            };
            const QSignalBlocker blocker(tree_);
            for (const auto &row : rows) {
                QTreeWidgetItem *candidate = row.first;
                const OutlinerRowKind kind = static_cast<OutlinerRowKind>(
                    candidate->data(nameColumn, itemKindRole).toInt());
                const QString sceneKey = candidate->data(
                    nameColumn, itemSceneKeyRole).toString();
                const QString viewLayerKey = candidate->data(
                    nameColumn, itemViewLayerKeyRole).toString();
                if (kind == OutlinerRowKind::SceneRoot && !sceneKey.isEmpty()) {
                    candidate->setExpanded(sceneKey == activeSceneKey);
                } else if (kind == OutlinerRowKind::SceneRoot &&
                           !viewLayerKey.isEmpty()) {
                    candidate->setExpanded(
                        viewLayerKey == activeViewLayerKey);
                } else if (kind == OutlinerRowKind::SceneGroup &&
                           candidate->text(nameColumn) ==
                               QLatin1String("View Layers")) {
                    candidate->setExpanded(!activeSceneKey.isEmpty() &&
                        candidate->parent() != nullptr &&
                        candidate->parent()->data(
                            nameColumn, itemSceneKeyRole).toString() ==
                            activeSceneKey);
                } else if (kind == OutlinerRowKind::Collection ||
                           kind == OutlinerRowKind::Object ||
                           kind == OutlinerRowKind::SceneGroup ||
                           kind == OutlinerRowKind::SceneRoot) {
                    candidate->setExpanded(containsObject(candidate));
                } else {
                    candidate->setExpanded(false);
                }
            }
            saveTreeState();
        });

        const auto setOneLevel = [this](bool open) {
            if (tree_ == nullptr || dataApiDisplayMode_) return;
            QVector<QPair<QTreeWidgetItem *, int>> rows;
            std::function<void(QTreeWidgetItem *, int)> appendRows =
                [&rows, &appendRows](QTreeWidgetItem *parent, int depth) {
                    rows.append({parent, depth});
                    for (int childIndex = 0;
                         childIndex < parent->childCount(); ++childIndex) {
                        appendRows(parent->child(childIndex), depth + 1);
                    }
                };
            int maximumDepth = 0;
            int firstCollapsedDepth = 0;
            for (int index = 0; index < tree_->topLevelItemCount(); ++index) {
                appendRows(tree_->topLevelItem(index), 1);
            }
            for (const auto &row : rows) {
                maximumDepth = qMax(maximumDepth, row.second);
                if (firstCollapsedDepth == 0 &&
                    row.first->childCount() > 0 &&
                    !row.first->isExpanded()) {
                    firstCollapsedDepth = row.second;
                }
            }
            int targetDepth = firstCollapsedDepth;
            if (!open && targetDepth == 0) targetDepth = maximumDepth;
            if (!open && targetDepth > 0) --targetDepth;
            if (targetDepth == 0 && open) return;
            const QSignalBlocker blocker(tree_);
            for (const auto &row : rows) {
                if ((open && row.second <= targetDepth) ||
                    (!open && row.second >= targetDepth)) {
                    row.first->setExpanded(open);
                }
            }
            saveTreeState();
        };
        QAction *showOneLevel = viewMenu->addAction(
            QStringLiteral("Show One Level"));
        showOneLevel->setEnabled(!dataApiDisplayMode_);
        connect(showOneLevel, &QAction::triggered, this,
                [setOneLevel]() { setOneLevel(true); });
        QAction *hideOneLevel = viewMenu->addAction(
            QStringLiteral("Hide One Level"));
        hideOneLevel->setEnabled(!dataApiDisplayMode_);
        connect(hideOneLevel, &QAction::triggered, this,
                [setOneLevel]() { setOneLevel(false); });
        menu.exec(tree_->viewport()->mapToGlobal(position));
        return;
    }
    const OutlinerRowKind kind = static_cast<OutlinerRowKind>(
        item->data(nameColumn, itemKindRole).toInt());
    if (kind == OutlinerRowKind::ObjectContent ||
        kind == OutlinerRowKind::SceneGroup ||
        kind == OutlinerRowKind::Library ||
        kind == OutlinerRowKind::SequencerStrip ||
        kind == OutlinerRowKind::DataApi) return;
    QMenu menu(this);
    if (kind == OutlinerRowKind::SceneRoot &&
        item->data(nameColumn, itemViewLayerKeyRole).toString().isEmpty() &&
        callbacks_.createCollection) {
        const QString activeViewLayerKey = callbacks_.activeViewLayerKey
            ? callbacks_.activeViewLayerKey() : QString{};
        addNewCollectionAction(menu, activeViewLayerKey);
        addPasteDataBlocksAction(menu, CollectionId::invalid());
    } else if (kind == OutlinerRowKind::Collection) {
        const CollectionId collectionId = CollectionId::fromValue(
            item->data(nameColumn, itemIdRole).toULongLong());
        const QString viewLayerKey = item->data(
            nameColumn, itemViewLayerKeyRole).toString();
        const QStringList sourcePath = item->data(
            nameColumn, itemSourcePathRole).toStringList();
        const quint64 parentValue = item->data(
            nameColumn, itemParentCollectionRole).toULongLong();
        const OutlinerCollectionContext activeCollectionContext{
            collectionId,
            parentValue == 0 ? CollectionId::invalid()
                             : CollectionId::fromValue(parentValue),
            viewLayerKey,
            sourcePath};
        QVector<OutlinerCollectionContext> collectionTargets;
        QSet<QString> collectionTargetKeys;
        bool canEnableViewport = false;
        bool canDisableViewport = false;
        bool canEnableRender = false;
        bool canDisableRender = false;
        bool hasLayerStateInSelection = false;
        bool canShowInViewLayer = false;
        bool canHideInViewLayer = false;
        bool canSetExclude = false;
        bool canClearExclude = false;
        bool canSetHoldout = false;
        bool canClearHoldout = false;
        bool canSetIndirectOnly = false;
        bool canClearIndirectOnly = false;
        const auto appendCollectionTarget =
            [this, &collectionTargets, &collectionTargetKeys,
             &canEnableViewport, &canDisableViewport,
             &canEnableRender, &canDisableRender,
             &hasLayerStateInSelection, &canShowInViewLayer,
             &canHideInViewLayer, &canSetExclude, &canClearExclude,
             &canSetHoldout, &canClearHoldout, &canSetIndirectOnly,
             &canClearIndirectOnly, item, viewLayerKey](
                QTreeWidgetItem *candidate) {
                if (candidate == nullptr ||
                    candidate->data(nameColumn, itemKindRole).toInt() !=
                        static_cast<int>(OutlinerRowKind::Collection)) {
                    return;
                }
                const QString candidateViewLayerKey = candidate->data(
                    nameColumn, itemViewLayerKeyRole).toString();
                if (candidateViewLayerKey != viewLayerKey) return;
                const CollectionId candidateId = CollectionId::fromValue(
                    candidate->data(nameColumn, itemIdRole).toULongLong());
                if (!candidateId.isValid()) return;
                const bool globallyHidden =
                    candidate->checkState(viewportColumn) != Qt::Checked;
                const bool hiddenInRender =
                    candidate->checkState(renderColumn) != Qt::Checked;
                if (candidate == item) {
                    canEnableViewport = globallyHidden;
                    canDisableViewport = !globallyHidden;
                    canEnableRender = hiddenInRender;
                    canDisableRender = !hiddenInRender;
                }
                const bool hasLayerState =
                    candidate->data(viewLayerViewportColumn,
                                    itemViewLayerEditableRole).toBool();
                if (hasLayerState) {
                    hasLayerStateInSelection = true;
                    const bool hiddenInViewLayer =
                        candidate->checkState(viewLayerViewportColumn) !=
                        Qt::Checked;
                    canShowInViewLayer = canShowInViewLayer ||
                                         hiddenInViewLayer;
                    canHideInViewLayer = canHideInViewLayer ||
                                         !hiddenInViewLayer;
                    const bool excluded =
                        candidate->checkState(excludeColumn) != Qt::Checked;
                    canSetExclude = canSetExclude || !excluded;
                    canClearExclude = canClearExclude || excluded;
                    const bool holdout =
                        candidate->checkState(holdoutColumn) == Qt::Checked;
                    canSetHoldout = canSetHoldout || !holdout;
                    canClearHoldout = canClearHoldout || holdout;
                    const bool indirectOnly =
                        candidate->checkState(indirectOnlyColumn) == Qt::Checked;
                    canSetIndirectOnly = canSetIndirectOnly || !indirectOnly;
                    canClearIndirectOnly = canClearIndirectOnly || indirectOnly;
                }
                OutlinerCollectionContext context;
                context.collectionId = candidateId;
                const quint64 parentValue = candidate->data(
                    nameColumn, itemParentCollectionRole).toULongLong();
                context.parentCollectionId = parentValue == 0
                    ? CollectionId::invalid()
                    : CollectionId::fromValue(parentValue);
                context.viewLayerKey = candidateViewLayerKey;
                context.sourcePath = candidate->data(
                    nameColumn, itemSourcePathRole).toStringList();
                const QString key = QString::number(candidateId.value()) +
                    QLatin1Char('\x1f') + candidateViewLayerKey +
                    QLatin1Char('\x1f') +
                    context.sourcePath.join(QLatin1Char('\x1e'));
                if (collectionTargetKeys.contains(key)) return;
                collectionTargetKeys.insert(key);
                collectionTargets.append(context);
            };
        if (item->isSelected()) {
            for (QTreeWidgetItem *selected : tree_->selectedItems()) {
                appendCollectionTarget(selected);
            }
        }
        if (collectionTargets.isEmpty()) appendCollectionTarget(item);
        QVector<CollectionId> selectedCollectionIds;
        for (const OutlinerCollectionContext &target : collectionTargets) {
            if (!target.collectionId.isValid() ||
                selectedCollectionIds.contains(target.collectionId)) {
                continue;
            }
            selectedCollectionIds.append(target.collectionId);
        }
        QVector<OutlinerCollectionContext> objectSelectionTargets;
        for (const OutlinerCollectionContext &target : collectionTargets) {
            if (!viewLayerKey.isEmpty() &&
                target.viewLayerKey != viewLayerKey) {
                continue;
            }
            objectSelectionTargets.append(target);
        }
        QVector<CollectionId> linkedCollectionIds;
        bool canMarkCollectionAsset = false;
        bool canClearCollectionAsset = false;
        QVector<QTreeWidgetItem *> collectionTargetRows;
        if (item->isSelected()) {
            for (QTreeWidgetItem *selected : tree_->selectedItems()) {
                if (selected->data(nameColumn, itemKindRole).toInt() ==
                    static_cast<int>(OutlinerRowKind::Collection)) {
                    collectionTargetRows.append(selected);
                }
            }
        } else {
            collectionTargetRows.append(item);
        }
        for (QTreeWidgetItem *candidate : collectionTargetRows) {
            const CollectionId candidateId = CollectionId::fromValue(
                candidate->data(nameColumn, itemIdRole).toULongLong());
            if (!selectedCollectionIds.contains(candidateId)) continue;
            const bool linked = candidate->data(
                nameColumn, itemLinkedLibraryRole).toBool();
            if (linked && !linkedCollectionIds.contains(candidateId)) {
                linkedCollectionIds.append(candidateId);
            }
            if (linked || candidate->data(
                    nameColumn, itemSceneMasterRole).toBool()) {
                continue;
            }
            if (candidate->data(nameColumn, itemAssetMarkedRole).toBool()) {
                canClearCollectionAsset = true;
            } else {
                canMarkCollectionAsset = true;
            }
        }
        QVector<CollectionId> topLevelCollectionIds;
        QHash<quint64, CollectionId> topLevelCollectionParentIds;
        for (const OutlinerCollectionContext &target : collectionTargets) {
            const bool hasSelectedAncestor = std::any_of(
                collectionTargets.cbegin(), collectionTargets.cend(),
                [&target](const OutlinerCollectionContext &candidate) {
                    if (candidate.viewLayerKey != target.viewLayerKey ||
                        candidate.sourcePath.size() >=
                            target.sourcePath.size()) {
                        return false;
                    }
                    for (qsizetype index = 0;
                         index < candidate.sourcePath.size(); ++index) {
                        if (candidate.sourcePath[index] !=
                            target.sourcePath[index]) {
                            return false;
                        }
                    }
                    return true;
                });
            if (!hasSelectedAncestor &&
                !topLevelCollectionIds.contains(target.collectionId)) {
                topLevelCollectionIds.append(target.collectionId);
                topLevelCollectionParentIds.insert(
                    target.collectionId.value(), target.parentCollectionId);
            }
        }
        if (callbacks_.createCollection) {
            addNewCollectionAction(menu, viewLayerKey);
        }
        if (callbacks_.copyOutlinerCollection) {
            QAction *copy = menu.addAction(QStringLiteral("Copy"));
            connect(copy, &QAction::triggered, this,
                    [this, topLevelCollectionIds]() {
                callbacks_.copyOutlinerCollection(topLevelCollectionIds);
            });
        }
        if (callbacks_.pasteOutlinerCollection) {
            QAction *paste = menu.addAction(QStringLiteral("Paste"));
            paste->setEnabled(callbacks_.hasOutlinerCollectionClipboard &&
                               callbacks_.hasOutlinerCollectionClipboard());
            connect(paste, &QAction::triggered, this,
                    [this, collectionId]() {
                callbacks_.pasteOutlinerCollection(collectionId);
            });
        }
        if (callbacks_.duplicateCollection) {
            const CollectionId parentId =
                activeCollectionContext.parentCollectionId;
            QAction *duplicate = menu.addAction(
                QStringLiteral("Duplicate Collection"));
            connect(duplicate, &QAction::triggered, this,
                    [this, topLevelCollectionIds, parentId,
                     topLevelCollectionParentIds]() {
                callbacks_.duplicateCollection(topLevelCollectionIds,
                                               parentId,
                                               topLevelCollectionParentIds,
                                               false, false);
            });
            QAction *duplicateLinked = menu.addAction(
                QStringLiteral("Duplicate Linked"));
            connect(duplicateLinked, &QAction::triggered, this,
                    [this, topLevelCollectionIds, parentId,
                     topLevelCollectionParentIds]() {
                callbacks_.duplicateCollection(topLevelCollectionIds, parentId,
                                               topLevelCollectionParentIds,
                                               true, false);
            });
        }
        if (callbacks_.instanceCollectionsToScene) {
            QAction *instance = menu.addAction(
                QStringLiteral("Instance to Scene"));
            instance->setEnabled(!topLevelCollectionIds.isEmpty());
            connect(instance, &QAction::triggered, this,
                    [this, topLevelCollectionIds,
                     activeCollectionContext]() {
                callbacks_.instanceCollectionsToScene(
                    topLevelCollectionIds, activeCollectionContext);
            });
        }
        if (scenesDisplayMode_ && callbacks_.linkCollectionsToActiveScene) {
            QAction *linkToScene = menu.addAction(
                QStringLiteral("Link to Scene"));
            connect(linkToScene, &QAction::triggered, this,
                    [this, topLevelCollectionIds]() {
                callbacks_.linkCollectionsToActiveScene(
                    topLevelCollectionIds);
            });
        }
        if (callbacks_.unlinkCollections) {
            QAction *unlink = menu.addAction(QStringLiteral("Unlink"));
            connect(unlink, &QAction::triggered, this,
                    [this, collectionTargets]() {
                callbacks_.unlinkCollections(collectionTargets);
            });
        }
        const bool hasViewLayerPath = !viewLayerKey.isEmpty() &&
                                      !sourcePath.isEmpty();
        if ((hasViewLayerPath &&
             callbacks_.setCollectionsViewLayerState) ||
            callbacks_.setCollectionsViewportEnabled ||
            callbacks_.setCollectionsRenderVisibility) {
            menu.addSeparator();
            QMenu *visibility = menu.addMenu(QStringLiteral("Visibility"));
            if (hasViewLayerPath &&
                callbacks_.setCollectionsViewLayerState) {
                QAction *isolate = visibility->addAction(
                    QStringLiteral("Isolate"));
                connect(isolate, &QAction::triggered, this,
                        [this, collectionTargets]() {
                    callbacks_.setCollectionsViewLayerState(
                        collectionTargets,
                        QStringLiteral("hide_viewport"), true, false, true);
                });
                visibility->addSeparator();

                QAction *show = visibility->addAction(
                    QStringLiteral("Show"));
                show->setEnabled(!hasLayerStateInSelection ||
                                 canShowInViewLayer);
                connect(show, &QAction::triggered, this,
                        [this, collectionTargets]() {
                    callbacks_.setCollectionsViewLayerState(
                        collectionTargets,
                        QStringLiteral("hide_viewport"), false, false, false);
                });
                QAction *showInside = visibility->addAction(
                    QStringLiteral("Show All Inside"));
                connect(showInside, &QAction::triggered, this,
                        [this, collectionTargets]() {
                    callbacks_.setCollectionsViewLayerState(
                        collectionTargets,
                        QStringLiteral("hide_viewport"), false, true, false);
                });
                QAction *hide = visibility->addAction(
                    QStringLiteral("Hide"));
                hide->setEnabled(!hasLayerStateInSelection ||
                                 canHideInViewLayer);
                connect(hide, &QAction::triggered, this,
                        [this, collectionTargets]() {
                    callbacks_.setCollectionsViewLayerState(
                        collectionTargets,
                        QStringLiteral("hide_viewport"), true, false, false);
                });
                QAction *hideInside = visibility->addAction(
                    QStringLiteral("Hide All Inside"));
                connect(hideInside, &QAction::triggered, this,
                        [this, collectionTargets]() {
                    callbacks_.setCollectionsViewLayerState(
                        collectionTargets,
                        QStringLiteral("hide_viewport"), true, true, false);
                });
            }

            if (callbacks_.setCollectionsViewportEnabled) {
                if (hasViewLayerPath &&
                    callbacks_.setCollectionsViewLayerState) {
                    visibility->addSeparator();
                }
                QAction *enableViewport = visibility->addAction(
                    QStringLiteral("Enable in Viewports"));
                enableViewport->setEnabled(canEnableViewport);
                connect(enableViewport, &QAction::triggered, this,
                        [this, collectionTargets]() {
                    callbacks_.setCollectionsViewportEnabled(
                        collectionTargets, true);
                });
                QAction *disableViewport = visibility->addAction(
                    QStringLiteral("Disable in Viewports"));
                disableViewport->setEnabled(canDisableViewport);
                connect(disableViewport, &QAction::triggered, this,
                        [this, collectionTargets]() {
                    callbacks_.setCollectionsViewportEnabled(
                        collectionTargets, false);
                });
            }

            if (callbacks_.setCollectionsRenderVisibility) {
                visibility->addSeparator();
                QAction *enableRender = visibility->addAction(
                    QStringLiteral("Enable in Render"));
                enableRender->setEnabled(canEnableRender);
                connect(enableRender, &QAction::triggered, this,
                        [this, collectionTargets]() {
                    callbacks_.setCollectionsRenderVisibility(
                        collectionTargets, false);
                });
                QAction *disableRender = visibility->addAction(
                    QStringLiteral("Disable in Render"));
                disableRender->setEnabled(canDisableRender);
                connect(disableRender, &QAction::triggered, this,
                        [this, collectionTargets]() {
                    callbacks_.setCollectionsRenderVisibility(
                        collectionTargets, true);
                });
            }
        }
        if (hasViewLayerPath && !scenesDisplayMode_ &&
            callbacks_.setCollectionsViewLayerState) {
            menu.addSeparator();
            QMenu *viewLayer = menu.addMenu(QStringLiteral("View Layer"));
            const auto addViewLayerStateActions =
                [this, viewLayer, collectionTargets](
                    const QString &label, const QString &stateKey,
                    bool canSet, bool canClear) {
                    QAction *set = viewLayer->addAction(
                        QStringLiteral("Set %1").arg(label));
                    set->setEnabled(canSet);
                    connect(set, &QAction::triggered, this,
                            [this, collectionTargets,
                             stateKey]() {
                        callbacks_.setCollectionsViewLayerState(
                            collectionTargets, stateKey,
                            true, false, false);
                    });
                    QAction *clear = viewLayer->addAction(
                        QStringLiteral("Clear %1").arg(label));
                    clear->setEnabled(canClear);
                    connect(clear, &QAction::triggered, this,
                            [this, collectionTargets,
                             stateKey]() {
                        callbacks_.setCollectionsViewLayerState(
                            collectionTargets, stateKey,
                            false, false, false);
                    });
                };
            addViewLayerStateActions(QStringLiteral("Exclude"),
                                     QStringLiteral("exclude"),
                                     canSetExclude, canClearExclude);
            addViewLayerStateActions(QStringLiteral("Holdout"),
                                     QStringLiteral("holdout"),
                                     canSetHoldout, canClearHoldout);
            addViewLayerStateActions(QStringLiteral("Indirect Only"),
                                     QStringLiteral("indirect_only"),
                                     canSetIndirectOnly,
                                     canClearIndirectOnly);
            menu.addSeparator();
        }
        if (callbacks_.setCollectionColorTag &&
            !selectedCollectionIds.isEmpty()) {
            QMenu *colorTagMenu = menu.addMenu(
                QStringLiteral("Color Tag"));
            const QVector<QPair<QString, QString>> colorTags{
                {QStringLiteral("NONE"), QStringLiteral("None")},
                {QStringLiteral("COLOR_01"), QStringLiteral("Color 01")},
                {QStringLiteral("COLOR_02"), QStringLiteral("Color 02")},
                {QStringLiteral("COLOR_03"), QStringLiteral("Color 03")},
                {QStringLiteral("COLOR_04"), QStringLiteral("Color 04")},
                {QStringLiteral("COLOR_05"), QStringLiteral("Color 05")},
                {QStringLiteral("COLOR_06"), QStringLiteral("Color 06")},
                {QStringLiteral("COLOR_07"), QStringLiteral("Color 07")},
                {QStringLiteral("COLOR_08"), QStringLiteral("Color 08")}};
            for (const auto &[colorTag, label] : colorTags) {
                QAction *action = colorTagMenu->addAction(
                    collectionColorTagIcon(colorTag), label);
                connect(action, &QAction::triggered, this,
                        [this, selectedCollectionIds, colorTag]() {
                    callbacks_.setCollectionColorTag(
                        selectedCollectionIds, colorTag);
                });
            }
        }
        if (callbacks_.setCollectionsAssetMarked &&
            !selectedCollectionIds.isEmpty()) {
            QMenu *assetMenu = menu.addMenu(QStringLiteral("Asset"));
            QAction *mark = assetMenu->addAction(
                QStringLiteral("Mark as Asset"));
            mark->setEnabled(canMarkCollectionAsset);
            connect(mark, &QAction::triggered, this,
                    [this, selectedCollectionIds]() {
                callbacks_.setCollectionsAssetMarked(
                    selectedCollectionIds, true);
            });
            QAction *clear = assetMenu->addAction(
                QStringLiteral("Clear Asset"));
            clear->setEnabled(canClearCollectionAsset);
            connect(clear, &QAction::triggered, this,
                    [this, selectedCollectionIds]() {
                callbacks_.setCollectionsAssetMarked(
                    selectedCollectionIds, false);
            });
        }
        if (callbacks_.renameCollection) {
            QAction *rename = menu.addAction(QStringLiteral("Rename"));
            rename->setEnabled(
                !item->data(nameColumn, itemLinkedLibraryRole).toBool() &&
                !item->data(nameColumn, itemSceneMasterRole).toBool());
            const QString originalName = item->text(nameColumn);
            connect(rename, &QAction::triggered, this,
                    [this, originalName, collectionId]() {
                bool accepted = false;
                const QString name = QInputDialog::getText(
                    this, QStringLiteral("Rename Collection"),
                    QStringLiteral("Name"), QLineEdit::Normal,
                    originalName, &accepted);
                if (accepted && !name.trimmed().isEmpty()) {
                    callbacks_.renameCollection(collectionId, name);
                }
            });
        }
        if (callbacks_.makeCollectionsLocal &&
            !linkedCollectionIds.isEmpty()) {
            QAction *makeLocal = menu.addAction(QStringLiteral("Make Local"));
            connect(makeLocal, &QAction::triggered, this,
                    [this, linkedCollectionIds]() {
                callbacks_.makeCollectionsLocal(linkedCollectionIds);
            });
        }
        if (callbacks_.setCollectionsObjectSelection &&
            !objectSelectionTargets.isEmpty()) {
            QAction *selectObjects = menu.addAction(
                QStringLiteral("Select Objects"));
            connect(selectObjects, &QAction::triggered, this,
                    [this, objectSelectionTargets]() {
                callbacks_.setCollectionsObjectSelection(
                    objectSelectionTargets, true);
            });
            QAction *deselectObjects = menu.addAction(
                QStringLiteral("Deselect Objects"));
            connect(deselectObjects, &QAction::triggered, this,
                    [this, objectSelectionTargets]() {
                callbacks_.setCollectionsObjectSelection(
                    objectSelectionTargets, false);
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
        if (callbacks_.removeCollection ||
            callbacks_.removeCollectionHierarchy) {
            menu.addSeparator();
            if (callbacks_.removeCollection) {
                QAction *remove = menu.addAction(QStringLiteral("Delete"));
                connect(remove, &QAction::triggered, this,
                        [this, selectedCollectionIds]() {
                    callbacks_.removeCollection(selectedCollectionIds);
                });
            }
            if (callbacks_.removeCollectionHierarchy) {
                QAction *removeHierarchy = menu.addAction(
                    QStringLiteral("Delete Hierarchy"));
                connect(removeHierarchy, &QAction::triggered, this,
                        [this, topLevelCollectionIds]() {
                    callbacks_.removeCollectionHierarchy(
                        topLevelCollectionIds);
                });
            }
        }
    } else if (kind == OutlinerRowKind::Object) {
        const ObjectId objectId = ObjectId::fromValue(
            item->data(nameColumn, itemIdRole).toULongLong());
        QVector<ObjectId> objectTargetIds;
        QVector<ObjectId> linkedObjectIds;
        bool canMarkObjectAsset = false;
        bool canClearObjectAsset = false;
        QVector<QTreeWidgetItem *> objectTargetRows;
        if (item->isSelected()) {
            for (QTreeWidgetItem *selected : tree_->selectedItems()) {
                if (selected->data(nameColumn, itemKindRole).toInt() ==
                    static_cast<int>(OutlinerRowKind::Object)) {
                    objectTargetRows.append(selected);
                }
            }
        } else {
            objectTargetRows.append(item);
        }
        for (QTreeWidgetItem *candidate : objectTargetRows) {
            const ObjectId candidateId = ObjectId::fromValue(
                candidate->data(nameColumn, itemIdRole).toULongLong());
            if (!candidateId.isValid() || objectTargetIds.contains(candidateId)) {
                continue;
            }
            objectTargetIds.append(candidateId);
            const bool linked = candidate->data(
                nameColumn, itemLinkedLibraryRole).toBool();
            if (linked) linkedObjectIds.append(candidateId);
            const bool assetMarked = candidate->data(
                nameColumn, itemAssetMarkedRole).toBool();
            if (!linked && assetMarked) canClearObjectAsset = true;
            if (!linked && !assetMarked) canMarkObjectAsset = true;
        }
        if (callbacks_.copyOutlinerObject && objectId.isValid()) {
            QAction *copy = menu.addAction(QStringLiteral("Copy"));
            connect(copy, &QAction::triggered, this,
                    [this, objectId]() {
                callbacks_.copyOutlinerObject(objectId);
            });
        }
        if (callbacks_.pasteOutlinerObjects &&
            callbacks_.hasOutlinerObjectClipboard &&
            callbacks_.hasOutlinerObjectClipboard()) {
            QAction *paste = menu.addAction(QStringLiteral("Paste"));
            connect(paste, &QAction::triggered, this, [this]() {
                callbacks_.pasteOutlinerObjects();
            });
        }
        if ((callbacks_.copyOutlinerObject && objectId.isValid()) ||
            (callbacks_.pasteOutlinerObjects &&
             callbacks_.hasOutlinerObjectClipboard &&
             callbacks_.hasOutlinerObjectClipboard())) {
            menu.addSeparator();
        }
        if (callbacks_.deleteObjectSelection &&
            !objectTargetIds.isEmpty()) {
            QAction *deleteObjects = menu.addAction(QStringLiteral("Delete"));
            connect(deleteObjects, &QAction::triggered, this,
                    [this, objectTargetIds]() {
                callbacks_.deleteObjectSelection(objectTargetIds, false);
            });
            QAction *deleteHierarchy = menu.addAction(
                QStringLiteral("Delete Hierarchy"));
            connect(deleteHierarchy, &QAction::triggered, this,
                    [this, objectTargetIds]() {
                callbacks_.deleteObjectSelection(objectTargetIds, true);
            });
            menu.addSeparator();
        }
        if (callbacks_.selectOutlinerObjects &&
            !objectTargetIds.isEmpty()) {
            QAction *select = menu.addAction(QStringLiteral("Select"));
            connect(select, &QAction::triggered, this,
                    [this, objectTargetIds]() {
                callbacks_.selectOutlinerObjects(objectTargetIds);
            });
        }
        if (callbacks_.selectObjectHierarchy) {
            QAction *selectHierarchy = menu.addAction(
                QStringLiteral("Select Hierarchy"));
            connect(selectHierarchy, &QAction::triggered, this,
                    [this, objectTargetIds]() {
                callbacks_.selectObjectHierarchy(objectTargetIds);
            });
        }
        if (callbacks_.deselectSelectedObjects) {
            QAction *deselect = menu.addAction(QStringLiteral("Deselect"));
            connect(deselect, &QAction::triggered, this,
                    [this, objectTargetIds]() {
                callbacks_.deselectSelectedObjects(objectTargetIds);
            });
        }
        if (callbacks_.renameObject) {
            QAction *rename = menu.addAction(QStringLiteral("Rename"));
            rename->setEnabled(
                !item->data(nameColumn, itemLinkedLibraryRole).toBool());
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
        if (callbacks_.makeObjectsLocal && !linkedObjectIds.isEmpty()) {
            QAction *makeLocal = menu.addAction(QStringLiteral("Make Local"));
            connect(makeLocal, &QAction::triggered, this,
                    [this, linkedObjectIds]() {
                callbacks_.makeObjectsLocal(linkedObjectIds);
            });
        }
        if (callbacks_.setObjectsAssetMarked &&
            !objectTargetIds.isEmpty()) {
            QMenu *assetMenu = menu.addMenu(QStringLiteral("Asset"));
            QAction *mark = assetMenu->addAction(
                QStringLiteral("Mark as Asset"));
            mark->setEnabled(canMarkObjectAsset);
            connect(mark, &QAction::triggered, this,
                    [this, objectTargetIds]() {
                callbacks_.setObjectsAssetMarked(objectTargetIds, true);
            });
            QAction *clear = assetMenu->addAction(
                QStringLiteral("Clear Asset"));
            clear->setEnabled(canClearObjectAsset);
            connect(clear, &QAction::triggered, this,
                    [this, objectTargetIds]() {
                callbacks_.setObjectsAssetMarked(objectTargetIds, false);
            });
        }
        QVector<OutlinerObjectCollectionLink> unlinkTargets;
        const QString clickedViewLayerKey = item->data(
            nameColumn, itemViewLayerKeyRole).toString();
        for (QTreeWidgetItem *candidate : objectTargetRows) {
            if (candidate->data(nameColumn, itemViewLayerKeyRole).toString() !=
                clickedViewLayerKey) {
                continue;
            }
            const ObjectId candidateObjectId = ObjectId::fromValue(
                candidate->data(nameColumn, itemIdRole).toULongLong());
            const CollectionId candidateCollectionId = CollectionId::fromValue(
                candidate->data(nameColumn,
                                itemLinkedCollectionRole).toULongLong());
            if (!candidateObjectId.isValid() ||
                !candidateCollectionId.isValid()) {
                continue;
            }
            const bool alreadyIncluded = std::any_of(
                unlinkTargets.cbegin(), unlinkTargets.cend(),
                [candidateObjectId, candidateCollectionId](
                    const OutlinerObjectCollectionLink &target) {
                    return target.objectId == candidateObjectId &&
                           target.collectionId == candidateCollectionId;
                });
            if (!alreadyIncluded) {
                unlinkTargets.append(
                    {candidateObjectId, candidateCollectionId});
            }
        }
        if (callbacks_.unlinkObjects && !unlinkTargets.isEmpty()) {
            QAction *unlink = menu.addAction(
                QStringLiteral("Unlink from Collection"));
            connect(unlink, &QAction::triggered, this,
                    [this, unlinkTargets]() {
                callbacks_.unlinkObjects(unlinkTargets);
            });
        }
        if (callbacks_.createCollection) {
            addNewCollectionAction(menu, clickedViewLayerKey);
        }
        const bool hasLinkedObjectDataTarget =
            callbacks_.hasLinkedObjectData &&
            std::any_of(objectTargetIds.cbegin(), objectTargetIds.cend(),
                        [this](ObjectId id) {
                return callbacks_.hasLinkedObjectData(id);
            });
        if (!objectTargetIds.isEmpty() && hasLinkedObjectDataTarget &&
            callbacks_.selectLinkedObjectData) {
            QMenu *idDataMenu = menu.addMenu(QStringLiteral("ID Data"));
            QAction *selectLinked = idDataMenu->addAction(
                QStringLiteral("Select Linked"));
            connect(selectLinked, &QAction::triggered, this,
                    [this, objectTargetIds]() {
                callbacks_.selectLinkedObjectData(objectTargetIds);
            });
        }
    } else if (kind == OutlinerRowKind::Material) {
        const QString materialId = item->data(
            nameColumn, itemIdRole).toString();
        QStringList materialTargetIds;
        QVector<QTreeWidgetItem *> materialTargetRows;
        if (item->isSelected()) {
            for (QTreeWidgetItem *selected : tree_->selectedItems()) {
                if (selected->data(nameColumn, itemKindRole).toInt() ==
                    static_cast<int>(OutlinerRowKind::Material)) {
                    materialTargetRows.append(selected);
                }
            }
        } else {
            materialTargetRows.append(item);
        }
        for (QTreeWidgetItem *candidate : materialTargetRows) {
            const QString candidateId = candidate->data(
                nameColumn, itemIdRole).toString();
            if (!candidateId.isEmpty() &&
                !materialTargetIds.contains(candidateId)) {
                materialTargetIds.append(candidateId);
            }
        }
        if (!materialId.isEmpty() && callbacks_.hasLinkedMaterial &&
            callbacks_.hasLinkedMaterial(materialId) &&
            callbacks_.selectLinkedMaterial) {
            QMenu *idDataMenu = menu.addMenu(QStringLiteral("ID Data"));
            QAction *selectLinked = idDataMenu->addAction(
                QStringLiteral("Select Linked"));
            ObjectId clickedObjectId = ObjectId::invalid();
            for (QTreeWidgetItem *ancestor = item->parent();
                 ancestor != nullptr; ancestor = ancestor->parent()) {
                if (ancestor->data(nameColumn, itemKindRole).toInt() ==
                    static_cast<int>(OutlinerRowKind::Object)) {
                    clickedObjectId = ObjectId::fromValue(
                        ancestor->data(nameColumn, itemIdRole).toULongLong());
                    break;
                }
            }
            connect(selectLinked, &QAction::triggered, this,
                    [this, materialTargetIds, clickedObjectId]() {
                callbacks_.selectLinkedMaterial(
                    materialTargetIds, clickedObjectId);
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
    item->setIcon(nameColumn, collectionIcon(entry.collection.colorTag));
    if (entry.collection.colorTag != QLatin1String("NONE")) {
        item->setToolTip(nameColumn,
                         QStringLiteral("Color tag: %1")
                             .arg(collectionColorTagLabel(
                                 entry.collection.colorTag)));
    }
    item->setData(nameColumn, itemKindRole,
                  static_cast<int>(OutlinerRowKind::Collection));
    item->setData(nameColumn, itemIdRole,
                  QVariant::fromValue<qulonglong>(entry.collection.id.value()));
    item->setData(nameColumn, itemAssetMarkedRole,
                  entry.collection.assetMarked);
    item->setData(nameColumn, itemLinkedLibraryRole,
                  !entry.collection.libraryPath.isEmpty());
    item->setData(nameColumn, itemSceneMasterRole,
                  entry.collection.isSceneMaster);
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
    const QString currentViewLayerKey = callbacks_.activeViewLayerKey
        ? callbacks_.activeViewLayerKey() : QString{};
    if (rowViewLayerKey == currentViewLayerKey &&
        callbacks_.activeCollectionSourcePath &&
        sourcePath == callbacks_.activeCollectionSourcePath()) {
        QFont activeFont = item->font(nameColumn);
        activeFont.setBold(true);
        item->setFont(nameColumn, activeFont);
        QStringList tooltip{QStringLiteral("Active collection")};
        if (entry.collection.colorTag != QLatin1String("NONE")) {
            tooltip.append(QStringLiteral("Color tag: %1")
                               .arg(collectionColorTagLabel(
                                   entry.collection.colorTag)));
        }
        item->setToolTip(nameColumn, tooltip.join(QLatin1Char('\n')));
    }
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
    QVector<int> childEntryIndices;
    for (int index = 0; index < entries.size(); ++index) {
        if (entries[index].collection.parentIds.contains(entry.collection.id)) {
            childEntryIndices.append(index);
        }
    }
    const auto childOrder = [&entry, &entries](int index) {
        const QVector<CollectionParentOrder> &orders =
            entries[index].collection.parentOrders;
        const auto found = std::find_if(
            orders.cbegin(), orders.cend(), [&entry](
                const CollectionParentOrder &order) {
                return order.parentId == entry.collection.id;
            });
        return found == orders.cend()
            ? std::numeric_limits<int>::max() : found->order;
    };
    std::stable_sort(childEntryIndices.begin(), childEntryIndices.end(),
                     [&childOrder](int left, int right) {
        return childOrder(left) < childOrder(right);
    });
    for (const int childIndex : childEntryIndices) {
        appendCollection(tree, entries[childIndex], entries, objects, item,
                         ancestors, path, sourcePath, activeViewLayerKey,
                         rowViewLayerKey);
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
    const QString &activeViewLayerKey,
    bool includeContents)
{
    if (parent == nullptr || (!entry.id.isValid() && !entry.outlinerOnly)) {
        return nullptr;
    }
    auto *item = new QTreeWidgetItem(parent);
    item->setText(nameColumn, entry.name);
    item->setIcon(nameColumn, objectTypeIcon(
        entry.blenderType.isEmpty() ? QStringLiteral("CURVE")
                                   : entry.blenderType));
    item->setData(nameColumn, itemKindRole,
                  static_cast<int>(entry.outlinerOnly
                      ? OutlinerRowKind::SourceObject
                      : OutlinerRowKind::Object));
    if (entry.outlinerOnly) {
        item->setData(nameColumn, itemSourceObjectIdRole,
                      entry.sourceObjectId);
        item->setToolTip(nameColumn, entry.outlinerDescription);
    } else {
        item->setData(nameColumn, itemIdRole,
                      QVariant::fromValue<qulonglong>(entry.id.value()));
        item->setData(nameColumn, itemAssetMarkedRole, entry.assetMarked);
        item->setData(nameColumn, itemLinkedLibraryRole,
                      !entry.libraryPath.isEmpty());
    }
    item->setData(nameColumn, itemLinkedCollectionRole,
                  QVariant::fromValue<qulonglong>(
                      linkedCollectionId.isValid()
                          ? linkedCollectionId.value() : 0));
    item->setData(nameColumn, itemSceneMembershipsRole, entry.sceneKeys);
    item->setData(nameColumn, itemViewLayerKeyRole, activeViewLayerKey);
    QStringList objectPath = path;
    if (entry.outlinerOnly) {
        const QByteArray rowDigest = QCryptographicHash::hash(
            entry.sourceObjectId.toUtf8(), QCryptographicHash::Sha256).toHex();
        objectPath.append(QStringLiteral("b%1")
                               .arg(QString::fromLatin1(rowDigest.left(20))));
    } else {
        objectPath.append(QStringLiteral("o%1").arg(entry.id.value()));
    }
    item->setData(nameColumn, itemPathRole,
                  objectPath.join(QLatin1Char('/')));
    if (entry.outlinerOnly) {
        item->setFlags(Qt::ItemIsEnabled);
        sourceOnlyEntries_.insert(entry.sourceObjectId, entry);
    } else {
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable |
                       Qt::ItemIsSelectable | Qt::ItemIsDragEnabled |
                       Qt::ItemIsDropEnabled);
        item->setCheckState(viewportColumn, entry.hideViewport
                                   ? Qt::Unchecked : Qt::Checked);
        item->setCheckState(renderColumn, entry.hideRender
                                   ? Qt::Unchecked : Qt::Checked);
        item->setCheckState(selectColumn, entry.hideSelect
                                   ? Qt::Unchecked : Qt::Checked);
        item->setData(viewLayerViewportColumn, itemViewLayerEditableRole, true);
        if (!activeViewLayerKey.isEmpty()) {
            const bool hiddenInLayer = entry.viewLayerHidden.contains(
                activeViewLayerKey)
                ? entry.viewLayerHidden.value(activeViewLayerKey).toBool()
                : entry.hiddenInViewport;
            item->setCheckState(viewLayerViewportColumn,
                hiddenInLayer ? Qt::Unchecked : Qt::Checked);
        } else {
            item->setCheckState(viewLayerViewportColumn,
                entry.hiddenInViewport ? Qt::Unchecked : Qt::Checked);
        }
        objectRows_[entry.id.value()].append(item);
        objectEntries_.insert(entry.id.value(), entry);
    }
    const ObjectId activeObjectId = callbacks_.activeObjectId
        ? callbacks_.activeObjectId() : ObjectId::invalid();
    const int interactionMode = callbacks_.interactionMode
        ? callbacks_.interactionMode() : 0;
    const bool editMode = entry.id.isValid() && entry.id == activeObjectId &&
                          interactionMode == 1;
    item->setData(modeColumn, itemModeValueRole, editMode ? 1 : 0);
    item->setIcon(modeColumn, restrictionIcon(
        editMode ? RestrictionGlyph::EditMode : RestrictionGlyph::ObjectMode,
        true));
    item->setToolTip(modeColumn,
        editMode ? QStringLiteral("Edit Mode; click to switch to Object Mode")
                 : QStringLiteral("Object Mode; click to switch to Edit Mode"));
    if (!entry.hierarchyIdentity.isEmpty()) {
        hierarchyEntries_.insert(entry.hierarchyIdentity, entry);
        hierarchyRows_[entry.hierarchyIdentity].append(item);
    }
    if ((showObjectContents_ || scenesDisplayMode_) && includeContents) {
        const auto appendContentRow =
            [&objectPath, &activeViewLayerKey, &entry](
                QTreeWidgetItem *contentParent,
                const QString &name, const QString &suffix,
                const QString &description) -> QTreeWidgetItem * {
            if (contentParent == nullptr || name.isEmpty()) return nullptr;
            auto *content = new QTreeWidgetItem(contentParent);
            content->setText(nameColumn, name);
            content->setData(nameColumn, itemKindRole,
                             static_cast<int>(OutlinerRowKind::ObjectContent));
            if (entry.id.isValid()) {
                content->setData(nameColumn, itemIdRole,
                    QVariant::fromValue<qulonglong>(entry.id.value()));
            }
            content->setData(nameColumn, itemViewLayerKeyRole,
                             activeViewLayerKey);
            content->setData(nameColumn, itemPathRole,
                             objectPath.join(QLatin1Char('/')) + suffix);
            content->setToolTip(nameColumn, description);
            content->setFlags(Qt::ItemIsEnabled);
            return content;
        };
        const auto appendDataStackGroup =
            [&appendContentRow, &entry](
                QTreeWidgetItem *parent, const QString &name,
                const QString &suffix, const QString &description,
                BlenderSourceDataStackKind stackKind) -> QTreeWidgetItem * {
            QTreeWidgetItem *row = appendContentRow(
                parent, name, suffix, description);
            if (row != nullptr && entry.id.isValid() && !entry.outlinerOnly) {
                row->setData(nameColumn, itemKindRole,
                    static_cast<int>(OutlinerRowKind::DataStackGroup));
                row->setData(nameColumn, itemDataStackKindRole,
                             static_cast<int>(stackKind));
                row->setData(nameColumn, itemDataStackIndexRole,
                             QVariant::fromValue<qlonglong>(-1));
                row->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable |
                              Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);
            }
            return row;
        };
        const auto appendDataStackEntry =
            [&appendContentRow, &entry](
                QTreeWidgetItem *parent, const QString &name,
                const QString &suffix, const QString &description,
                BlenderSourceDataStackKind stackKind,
                qsizetype index) -> QTreeWidgetItem * {
            QTreeWidgetItem *row = appendContentRow(
                parent, name, suffix, description);
            if (row != nullptr && entry.id.isValid() && !entry.outlinerOnly) {
                row->setData(nameColumn, itemKindRole,
                    static_cast<int>(OutlinerRowKind::DataStackEntry));
                row->setData(nameColumn, itemIdRole,
                    QVariant::fromValue<qulonglong>(entry.id.value()));
                row->setData(nameColumn, itemDataStackKindRole,
                             static_cast<int>(stackKind));
                row->setData(nameColumn, itemDataStackIndexRole,
                             QVariant::fromValue<qlonglong>(index));
                row->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable |
                              Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);
            }
            return row;
        };
        const auto appendMaterialRow = [&appendContentRow](
            QTreeWidgetItem *parent, const OutlinerMaterialEntry &material,
            const QString &path, const QString &description) {
            QTreeWidgetItem *materialRow = appendContentRow(
                parent, material.name, path, description);
            if (materialRow != nullptr && !material.materialId.isEmpty()) {
                materialRow->setData(nameColumn, itemKindRole,
                    static_cast<int>(OutlinerRowKind::Material));
                materialRow->setData(nameColumn, itemIdRole,
                                     material.materialId);
                materialRow->setFlags(Qt::ItemIsEnabled |
                                      Qt::ItemIsSelectable |
                                      Qt::ItemIsDragEnabled);
            }
            const OutlinerAnimationEntry &source = material.animation;
            if (!source.hasAnimationData) return;
            QTreeWidgetItem *animation = appendContentRow(
                materialRow, QStringLiteral("Animation"),
                path + QStringLiteral("/animation"),
                QStringLiteral("Blender material animation data"));
            QTreeWidgetItem *action = appendContentRow(
                animation, source.actionName,
                path + QStringLiteral("/animation/action"),
                QStringLiteral("Blender active action data block"));
            appendContentRow(
                action, source.actionSlotName,
                path + QStringLiteral("/animation/action/slot"),
                QStringLiteral("Blender assigned action slot"));
            if (source.hasDrivers) {
                QTreeWidgetItem *drivers = appendContentRow(
                    animation, QStringLiteral("Drivers"),
                    path + QStringLiteral("/animation/drivers"),
                    QStringLiteral("Blender material animation drivers"));
                for (qsizetype index = 0;
                     index < source.driverTargetNames.size(); ++index) {
                    appendContentRow(
                        drivers, source.driverTargetNames[index],
                        path + QStringLiteral("/animation/driver-target-%1")
                                   .arg(index),
                        QStringLiteral("Blender driver target data block"));
                }
            }
            if (!source.nlaTracks.isEmpty()) {
                QTreeWidgetItem *tracks = appendContentRow(
                    animation, QStringLiteral("NLA Tracks"),
                    path + QStringLiteral("/animation/nla-tracks"),
                    QStringLiteral("Blender material NLA tracks"));
                for (qsizetype trackIndex = 0;
                     trackIndex < source.nlaTracks.size(); ++trackIndex) {
                    const OutlinerNlaTrackEntry &sourceTrack =
                        source.nlaTracks[trackIndex];
                    QTreeWidgetItem *track = appendContentRow(
                        tracks, sourceTrack.name,
                        path + QStringLiteral("/animation/nla-track-%1")
                                   .arg(trackIndex),
                        QStringLiteral("Blender NLA track"));
                    for (qsizetype actionIndex = 0;
                         actionIndex < sourceTrack.actionNames.size();
                         ++actionIndex) {
                        appendContentRow(
                            track, sourceTrack.actionNames[actionIndex],
                            path + QStringLiteral(
                                       "/animation/nla-track-%1/action-%2")
                                       .arg(trackIndex).arg(actionIndex),
                            QStringLiteral("Blender NLA action data block"));
                    }
                }
            }
        };
        const auto appendNamedTree = [&appendContentRow] (
            const auto &self, QTreeWidgetItem *parent,
            const QJsonArray &nodes, const QString &path,
            const QString &description, bool includeConstraints) -> void {
            for (qsizetype index = 0; index < nodes.size(); ++index) {
                const QJsonObject node = nodes[index].toObject();
                const QString nodePath = path + QStringLiteral("/item-%1")
                    .arg(index);
                QTreeWidgetItem *row = appendContentRow(
                    parent, node.value(QStringLiteral("name")).toString(),
                    nodePath, description);
                if (row == nullptr) continue;
                if (includeConstraints) {
                    const QJsonArray constraints = node.value(
                        QStringLiteral("constraints")).toArray();
                    if (!constraints.isEmpty()) {
                        QTreeWidgetItem *constraintGroup = appendContentRow(
                            row, QStringLiteral("Constraints"),
                            nodePath + QStringLiteral("/constraints"),
                            QStringLiteral("Blender pose-bone constraints"));
                        for (qsizetype constraintIndex = 0;
                             constraintIndex < constraints.size();
                             ++constraintIndex) {
                            appendContentRow(
                                constraintGroup,
                                constraints[constraintIndex].toString(),
                                nodePath + QStringLiteral("/constraint-%1")
                                    .arg(constraintIndex),
                                QStringLiteral("Blender pose-bone constraint"));
                        }
                    }
                }
                self(self, row, node.value(QStringLiteral("children")).toArray(),
                     nodePath, description, includeConstraints);
            }
        };
        if (entry.hasAnimationData) {
            QTreeWidgetItem *animation = appendContentRow(
                item, QStringLiteral("Animation"), QStringLiteral("/animation"),
                QStringLiteral("Blender object animation data"));
            QTreeWidgetItem *action = appendContentRow(
                animation, entry.animationActionName,
                QStringLiteral("/animation/action"),
                QStringLiteral("Blender active action data block"));
            appendContentRow(
                action, entry.animationActionSlotName,
                QStringLiteral("/animation/action/slot"),
                QStringLiteral("Blender assigned action slot"));
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
        if (entry.hasPose) {
            QTreeWidgetItem *pose = appendContentRow(
                item, QStringLiteral("Pose"), QStringLiteral("/pose"),
                QStringLiteral("Blender armature pose"));
            if (entry.showPoseChannels) {
                appendNamedTree(
                    appendNamedTree, pose, entry.poseBoneTree,
                    QStringLiteral("/pose/bones"),
                    QStringLiteral("Blender pose bone"), true);
            }
        }
        QTreeWidgetItem *dataBlock = appendContentRow(
            item, entry.dataBlockName, QStringLiteral("/data"),
            QStringLiteral("Blender object data block"));
        if (dataBlock != nullptr) {
            if (entry.dataHasAnimationData) {
                QTreeWidgetItem *animation = appendContentRow(
                    dataBlock, QStringLiteral("Animation"),
                    QStringLiteral("/data/animation"),
                    QStringLiteral("Blender data-block animation data"));
                QTreeWidgetItem *action = appendContentRow(
                    animation, entry.dataAnimationActionName,
                    QStringLiteral("/data/animation/action"),
                    QStringLiteral("Blender data-block active action"));
                appendContentRow(
                    action, entry.dataAnimationActionSlotName,
                    QStringLiteral("/data/animation/action/slot"),
                    QStringLiteral("Blender assigned data-block action slot"));
                if (entry.dataHasDrivers) {
                    QTreeWidgetItem *drivers = appendContentRow(
                        animation, QStringLiteral("Drivers"),
                        QStringLiteral("/data/animation/drivers"),
                        QStringLiteral("Blender data-block animation drivers"));
                    for (qsizetype index = 0;
                         index < entry.dataDriverTargetNames.size(); ++index) {
                        appendContentRow(
                            drivers, entry.dataDriverTargetNames[index],
                            QStringLiteral("/data/animation/driver-target-%1")
                                .arg(index),
                            QStringLiteral("Blender driver target data block"));
                    }
                }
                if (!entry.dataNlaTracks.isEmpty()) {
                    QTreeWidgetItem *tracks = appendContentRow(
                        animation, QStringLiteral("NLA Tracks"),
                        QStringLiteral("/data/animation/nla-tracks"),
                        QStringLiteral("Blender data-block NLA tracks"));
                    for (qsizetype trackIndex = 0;
                         trackIndex < entry.dataNlaTracks.size(); ++trackIndex) {
                        const OutlinerNlaTrackEntry &sourceTrack =
                            entry.dataNlaTracks[trackIndex];
                        QTreeWidgetItem *track = appendContentRow(
                            tracks, sourceTrack.name,
                            QStringLiteral("/data/animation/nla-track-%1")
                                .arg(trackIndex),
                            QStringLiteral("Blender data-block NLA track"));
                        for (qsizetype actionIndex = 0;
                             actionIndex < sourceTrack.actionNames.size();
                             ++actionIndex) {
                            appendContentRow(
                                track, sourceTrack.actionNames[actionIndex],
                                QStringLiteral(
                                    "/data/animation/nla-track-%1/action-%2")
                                    .arg(trackIndex).arg(actionIndex),
                                QStringLiteral("Blender NLA action data block"));
                        }
                    }
                }
            }
            appendNamedTree(
                appendNamedTree, dataBlock, entry.greasePencilLayerTree,
                QStringLiteral("/data/grease-pencil-layers"),
                QStringLiteral("Blender Grease Pencil layer or group"), false);
            appendNamedTree(
                appendNamedTree, dataBlock, entry.armatureBoneTree,
                QStringLiteral("/data/bones"),
                QStringLiteral("Blender armature bone"), false);
            if (!entry.boneCollectionTree.isEmpty()) {
                QTreeWidgetItem *boneCollections = appendContentRow(
                    dataBlock, QStringLiteral("Bone Collections"),
                    QStringLiteral("/data/bone-collections"),
                    QStringLiteral("Blender armature bone collections"));
                appendNamedTree(
                    appendNamedTree, boneCollections,
                    entry.boneCollectionTree,
                    QStringLiteral("/data/bone-collections/items"),
                    QStringLiteral("Blender bone collection"), false);
            }
            if (!entry.shapeKeyDataName.isEmpty()) {
                QTreeWidgetItem *shapeKeyData = appendContentRow(
                    dataBlock, entry.shapeKeyDataName,
                    QStringLiteral("/data/shape-keys"),
                    QStringLiteral("Blender shape-key data block"));
                for (qsizetype index = 0;
                     index < entry.shapeKeyNames.size(); ++index) {
                    appendContentRow(
                        shapeKeyData, entry.shapeKeyNames[index],
                        QStringLiteral("/data/shape-key-%1").arg(index),
                        QStringLiteral("Blender shape key"));
                }
            }
            for (qsizetype index = 0;
                 index < entry.dataMaterialNames.size(); ++index) {
                const QString path =
                    QStringLiteral("/data/material-%1").arg(index);
                if (index < entry.dataMaterialEntries.size()) {
                    appendMaterialRow(
                        dataBlock, entry.dataMaterialEntries[index], path,
                        QStringLiteral("Blender data-block material"));
                } else {
                    appendContentRow(
                        dataBlock, entry.dataMaterialNames[index], path,
                        QStringLiteral("Blender data-block material"));
                }
            }
        }
        for (qsizetype index = 0; index < entry.materialNames.size(); ++index) {
            const QString path = QStringLiteral("/material-%1").arg(index);
            if (index < entry.materialEntries.size()) {
                appendMaterialRow(
                    item, entry.materialEntries[index], path,
                    QStringLiteral("Blender material data block"));
            } else {
                appendContentRow(
                    item, entry.materialNames[index], path,
                    QStringLiteral("Blender material data block"));
            }
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
        const auto appendDataStackNameGroup =
            [&appendDataStackGroup, &appendDataStackEntry](
                QTreeWidgetItem *objectItem, const QString &groupName,
                const QString &groupPath, const QString &description,
                const QStringList &names, const QString &itemPathPrefix,
                BlenderSourceDataStackKind stackKind) {
            if (names.isEmpty()) return;
            QTreeWidgetItem *group = appendDataStackGroup(
                objectItem, groupName, groupPath, description, stackKind);
            for (qsizetype index = 0; index < names.size(); ++index) {
                appendDataStackEntry(
                    group, names[index],
                    QStringLiteral("%1-%2").arg(itemPathPrefix).arg(index),
                    description, stackKind, index);
            }
        };
        appendDataStackNameGroup(
            item, QStringLiteral("Constraints"),
            QStringLiteral("/constraints"),
            QStringLiteral("Blender object constraints"),
            entry.constraintNames, QStringLiteral("/constraint"),
            BlenderSourceDataStackKind::Constraint);
        if (entry.modifiers.isEmpty()) {
            appendDataStackNameGroup(
                item, QStringLiteral("Modifiers"),
                QStringLiteral("/modifiers"),
                QStringLiteral("Blender object modifiers"),
                entry.modifierNames, QStringLiteral("/modifier"),
                BlenderSourceDataStackKind::Modifier);
        } else {
            QTreeWidgetItem *modifiers = appendDataStackGroup(
                item, QStringLiteral("Modifiers"), QStringLiteral("/modifiers"),
                QStringLiteral("Blender object modifiers"),
                BlenderSourceDataStackKind::Modifier);
            for (qsizetype index = 0; index < entry.modifiers.size(); ++index) {
                const OutlinerModifierEntry &sourceModifier = entry.modifiers[index];
                QTreeWidgetItem *modifier = appendDataStackEntry(
                    modifiers, sourceModifier.name,
                    QStringLiteral("/modifier-%1").arg(index),
                    QStringLiteral("Blender object modifier"),
                    BlenderSourceDataStackKind::Modifier, index);
                if (sourceModifier.linkedName.isEmpty()) continue;
                const QString linkedDescription =
                    sourceModifier.linkedType == QLatin1String("node_tree")
                    ? QStringLiteral("Blender modifier node group")
                    : sourceModifier.linkedType == QLatin1String("particle_system")
                        ? QStringLiteral("Blender particle system")
                        : QStringLiteral("Blender linked modifier object");
                appendContentRow(modifier, sourceModifier.linkedName,
                                 QStringLiteral("/modifier-%1/linked").arg(index),
                                 linkedDescription);
            }
        }
        if (!entry.shaderEffects.isEmpty()) {
            QTreeWidgetItem *effects = appendDataStackGroup(
                item, QStringLiteral("Effects"), QStringLiteral("/effects"),
                QStringLiteral("Blender Grease Pencil shader effects"),
                BlenderSourceDataStackKind::ShaderEffect);
            for (qsizetype index = 0; index < entry.shaderEffects.size(); ++index) {
                const OutlinerModifierEntry &sourceEffect = entry.shaderEffects[index];
                QTreeWidgetItem *effect = appendDataStackEntry(
                    effects, sourceEffect.name,
                    QStringLiteral("/effect-%1").arg(index),
                    QStringLiteral("Blender Grease Pencil shader effect"),
                    BlenderSourceDataStackKind::ShaderEffect, index);
                if (!sourceEffect.linkedName.isEmpty()) {
                    appendContentRow(
                        effect, sourceEffect.linkedName,
                        QStringLiteral("/effect-%1/linked").arg(index),
                        QStringLiteral("Blender object linked to a Grease Pencil Swirl effect"));
                }
            }
        }
        appendNameGroup(item, QStringLiteral("Vertex Groups"),
                        QStringLiteral("/vertex-groups"),
                        QStringLiteral("Blender mesh vertex groups"),
                        entry.vertexGroupNames, QStringLiteral("/vertex-group"));
        if (!entry.instancedCollectionName.isEmpty()) {
            appendContentRow(
                item, entry.instancedCollectionName,
                QStringLiteral("/instance-collection"),
                QStringLiteral("Blender collection instanced by this object"));
        }
    }
    return item;
}

void CollectionsPanel::buildObjectParentHierarchy(
    const QVector<OutlinerObjectEntry> &objects)
{
    QSet<QString> visited;
    QSet<QString> visiting;
    QSet<QString> cyclicParentLinks;
    QStringList parentBeforeChild;
    const auto appendWithParents = [&](const auto &self,
                                       const QString &identity) -> void {
        if (identity.isEmpty() || visited.contains(identity)) return;
        const OutlinerObjectEntry entry = hierarchyEntries_.value(identity);
        if (entry.hierarchyIdentity.isEmpty()) return;
        visiting.insert(identity);
        const QString &parentIdentity = entry.parentHierarchyIdentity;
        if (!parentIdentity.isEmpty() &&
            hierarchyEntries_.contains(parentIdentity)) {
            if (visiting.contains(parentIdentity)) {
                cyclicParentLinks.insert(identity);
            } else {
                self(self, parentIdentity);
            }
        }
        visiting.remove(identity);
        visited.insert(identity);
        parentBeforeChild.append(identity);
    };
    for (const OutlinerObjectEntry &entry : objects) {
        appendWithParents(appendWithParents, entry.hierarchyIdentity);
    }

    for (const QString &childIdentity : parentBeforeChild) {
        const OutlinerObjectEntry childEntry =
            hierarchyEntries_.value(childIdentity);
        const QString parentIdentity = cyclicParentLinks.contains(childIdentity)
            ? QString{} : childEntry.parentHierarchyIdentity;
        if (parentIdentity.isEmpty()) continue;

        const QVector<QTreeWidgetItem *> parentRows =
            hierarchyRows_.value(parentIdentity);
        const QVector<QTreeWidgetItem *> childRows =
            hierarchyRows_.value(childIdentity);
        for (QTreeWidgetItem *parentRow : parentRows) {
            if (parentRow == nullptr) continue;
            const QString viewLayerKey = parentRow->data(
                nameColumn, itemViewLayerKeyRole).toString();
            QTreeWidgetItem *collectionRow = parentRow->parent();
            while (collectionRow != nullptr) {
                const OutlinerRowKind kind = static_cast<OutlinerRowKind>(
                    collectionRow->data(nameColumn, itemKindRole).toInt());
                if (kind != OutlinerRowKind::Object &&
                    kind != OutlinerRowKind::SourceObject) break;
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
                CollectionId::invalid(), viewLayerKey, false);
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
    QString treeModeContext = QStringLiteral("/ViewLayer");
    if (scenesDisplayMode_) {
        treeModeContext = QStringLiteral("/Scenes");
    } else if (sequencerDisplayMode_) {
        treeModeContext = QStringLiteral("/Sequence");
    } else if (dataApiDisplayMode_) {
        treeModeContext = QStringLiteral("/DataAPI");
    } else if (libraryOverridesDisplayMode_) {
        treeModeContext = QStringLiteral("/LibraryOverrides/%1")
            .arg(overrideHierarchiesView_
                     ? QStringLiteral("Hierarchies")
                     : QStringLiteral("Properties"));
    } else if (librariesDisplayMode_) {
        treeModeContext = QStringLiteral("/Libraries/%1").arg(idTypeFilter_);
    } else if (orphanDataDisplayMode_) {
        treeModeContext = QStringLiteral("/OrphanData/%1").arg(idTypeFilter_);
    }
    const QString treeStateContextKey =
        (callbacks_.treeStateContextKey
             ? callbacks_.treeStateContextKey() : QString{}) + treeModeContext;
    const bool treeStateContextChanged =
        treeStateContextKey != currentTreeStateContextKey_;
    currentTreeStateContextKey_ = treeStateContextKey;
    QHash<QString, bool> expandedPaths;
    QSet<QString> outlinerSelectedPaths;
    QString outlinerCurrentPath;
    const int scrollPosition = tree_->verticalScrollBar()->value();
    for (int topLevelIndex = 0;
         topLevelIndex < tree_->topLevelItemCount(); ++topLevelIndex) {
        QVector<QTreeWidgetItem *> pending{tree_->topLevelItem(topLevelIndex)};
        while (!pending.isEmpty()) {
            QTreeWidgetItem *item = pending.takeLast();
            const QString path = item->data(nameColumn, itemPathRole).toString();
            if (!path.isEmpty()) expandedPaths.insert(path, item->isExpanded());
            if (!syncSelectionEnabled_ && item->isSelected() && !path.isEmpty()) {
                outlinerSelectedPaths.insert(path);
            }
            if (!syncSelectionEnabled_ && item == tree_->currentItem()) {
                outlinerCurrentPath = path;
            }
            for (int childIndex = 0; childIndex < item->childCount();
                 ++childIndex) {
                pending.append(item->child(childIndex));
            }
        }
    }
    refreshing_ = true;
    tree_->setSortingEnabled(false);
    tree_->clear();
    rangeAnchorItem_ = nullptr;
    objectRows_.clear();
    objectEntries_.clear();
    hierarchyRows_.clear();
    hierarchyEntries_.clear();
    sourceOnlyEntries_.clear();
    const QVector<CollectionPanelEntry> allEntries = callbacks_.entries();
    const QVector<OutlinerObjectEntry> allObjects = callbacks_.objects
        ? callbacks_.objects() : QVector<OutlinerObjectEntry>{};
    const QVector<OutlinerSceneEntry> scenes = callbacks_.scenes
        ? callbacks_.scenes() : QVector<OutlinerSceneEntry>{};
    const QVector<OutlinerLibraryEntry> libraries = callbacks_.libraries
        ? callbacks_.libraries() : QVector<OutlinerLibraryEntry>{};
    const QVector<OutlinerOrphanGroup> orphanGroups = callbacks_.orphanGroups
        ? callbacks_.orphanGroups() : QVector<OutlinerOrphanGroup>{};
    const OutlinerSequenceTree sequence = callbacks_.sequencer
        ? callbacks_.sequencer() : OutlinerSequenceTree{};
    const QJsonObject dataApi = callbacks_.dataApi
        ? callbacks_.dataApi() : QJsonObject{};
    const QJsonObject libraryOverrides = callbacks_.libraryOverrides
        ? callbacks_.libraryOverrides() : QJsonObject{};
    const bool librariesMode = librariesDisplayMode_;
    const bool orphanDataMode = orphanDataDisplayMode_;
    const bool sequencerMode = sequencerDisplayMode_;
    const bool dataApiMode = dataApiDisplayMode_;
    const bool libraryOverridesMode = libraryOverridesDisplayMode_;
    const bool overrideHierarchiesMode = libraryOverridesMode &&
                                          overrideHierarchiesView_;
    const bool overridePropertiesMode = libraryOverridesMode &&
                                         !overrideHierarchiesView_;
    const bool viewLayerMode = !scenesDisplayMode_ && !sequencerMode &&
        !dataApiMode && !librariesMode && !orphanDataMode &&
        !libraryOverridesMode;
    for (QAction *action : viewLayerFilterActions_) {
        if (action != nullptr) action->setVisible(viewLayerMode);
    }
    const QJsonObject objectTypeAvailability = dataApi.value(
        QStringLiteral("object_type_filter_availability")).toObject();
    QSet<QString> availableObjectTypeFilters;
    if (!objectTypeAvailability.isEmpty()) {
        for (auto it = objectTypeAvailability.constBegin();
             it != objectTypeAvailability.constEnd(); ++it) {
            if (it.value().toBool()) availableObjectTypeFilters.insert(it.key());
        }
    } else {
        for (const OutlinerObjectEntry &object : allObjects) {
            availableObjectTypeFilters.insert(
                outlinerObjectTypeFilterKey(object.blenderType));
        }
    }
    // Blender always provides the Empty filter, even when the file has no
    // Empty objects or Empty data blocks.
    availableObjectTypeFilters.insert(QStringLiteral("EMPTY"));
    for (auto action = objectTypeFilterActions_.cbegin();
         action != objectTypeFilterActions_.cend(); ++action) {
        if (action.value() == nullptr) continue;
        action.value()->setVisible(
            viewLayerMode &&
            availableObjectTypeFilters.contains(action.key()));
        action.value()->setEnabled(showObjects_ && viewLayerMode);
    }
    if (objectTypeFilterSection_ != nullptr) {
        objectTypeFilterSection_->setVisible(viewLayerMode);
    }
    updateDataApiAnimationControls();
    const bool scenesMode = scenesDisplayMode_ && !scenes.isEmpty();
    static_cast<OutlinerTreeWidget *>(tree_)->setAllowParentingWithoutModifier(
        scenesMode || !showCollections_);
    if (!dataApiMode) {
        dataApiExpandedPaths_.clear();
        dataApiCollapsedPaths_.clear();
    } else if (!stateScopeKey.isEmpty()) {
        dataApiExpandedPaths_.clear();
        dataApiCollapsedPaths_.clear();
        const QString scopeHash = QString::fromLatin1(
            QCryptographicHash::hash(stateScopeKey.toUtf8(),
                                     QCryptographicHash::Sha256)
                .toHex());
        QSettings settings;
        const QStringList expandedDataApiPaths = settings.value(
            QStringLiteral("outliner/%1/dataApiExpandedPaths")
                .arg(scopeHash)).toStringList();
        const QStringList collapsedPaths = settings.value(
            QStringLiteral("outliner/%1/dataApiCollapsedPaths")
                .arg(scopeHash)).toStringList();
        dataApiExpandedPaths_ = QSet<QString>(
            expandedDataApiPaths.cbegin(), expandedDataApiPaths.cend());
        dataApiCollapsedPaths_ = QSet<QString>(collapsedPaths.cbegin(),
                                                collapsedPaths.cend());
    }
    libraryOverrideViewCombo_->setVisible(libraryOverridesMode);
    libraryOverrideViewCombo_->setEnabled(libraryOverridesMode);
    const bool hasOverrideLibraries = libraryOverrides.value(
        QStringLiteral("has_libraries")).toBool();
    systemOverridesAction_->setVisible(
        overridePropertiesMode && hasOverrideLibraries);
    systemOverridesAction_->setEnabled(
        overridePropertiesMode && hasOverrideLibraries);
    systemOverridesAction_->setChecked(showSystemOverrides_);
    filterEdit_->setVisible(!overrideHierarchiesMode);
    filterEdit_->setEnabled(!overrideHierarchiesMode);
    if (overrideHierarchiesMode && !filterEdit_->text().isEmpty()) {
        const QSignalBlocker filterBlocker(filterEdit_);
        filterEdit_->clear();
        activeFilterText_.clear();
        expandedBeforeFilter_.clear();
    }
    filterButton_->setVisible(viewLayerMode || scenesDisplayMode_ ||
                              libraryOverridesMode);
    if (restrictionToggleMenuAction_ != nullptr) {
        restrictionToggleMenuAction_->setVisible(
            viewLayerMode || scenesDisplayMode_);
    }
    if (restrictionToggleSeparatorAction_ != nullptr) {
        restrictionToggleSeparatorAction_->setVisible(
            viewLayerMode || scenesDisplayMode_);
    }
    for (QAction *action : viewLayerOnlyRestrictionActions_) {
        if (action != nullptr) action->setVisible(viewLayerMode);
    }
    for (QAction *action : searchFilterActions_) {
        if (action != nullptr) {
            action->setVisible(!overrideHierarchiesMode);
        }
    }
    if (sortAlphabeticallyAction_ != nullptr) {
        sortAlphabeticallyAction_->setVisible(!dataApiMode);
        sortAlphabeticallyAction_->setEnabled(!dataApiMode);
    }
    if (syncSelectionAction_ != nullptr) {
        syncSelectionAction_->setVisible(!libraryOverridesMode);
        syncSelectionAction_->setEnabled(!libraryOverridesMode);
    }
    if (showModeColumnAction_ != nullptr) {
        showModeColumnAction_->setVisible(!libraryOverridesMode);
        showModeColumnAction_->setEnabled(!libraryOverridesMode);
    }
    if (scrollToActiveAction_ != nullptr) {
        scrollToActiveAction_->setVisible(!libraryOverridesMode);
        scrollToActiveAction_->setEnabled(
            syncSelectionEnabled_ && !libraryOverridesMode);
    }
    {
        const QSignalBlocker blocker(idTypeCombo_);
        idTypeCombo_->clear();
        idTypeCombo_->addItem(QStringLiteral("All ID Types"), QString{});
        QSet<QString> addedTypes;
        for (const OutlinerLibraryEntry &library : libraries) {
            for (const OutlinerLibraryDataGroup &group : library.dataGroups) {
                if (group.displayName.isEmpty() ||
                    addedTypes.contains(group.displayName)) {
                    continue;
                }
                addedTypes.insert(group.displayName);
                idTypeCombo_->addItem(group.displayName, group.displayName);
            }
        }
        for (const OutlinerOrphanGroup &group : orphanGroups) {
            if (group.displayName.isEmpty() ||
                addedTypes.contains(group.displayName)) {
                continue;
            }
            addedTypes.insert(group.displayName);
            idTypeCombo_->addItem(group.displayName, group.displayName);
        }
        int typeIndex = idTypeCombo_->findData(idTypeFilter_);
        if (typeIndex < 0) {
            idTypeFilter_.clear();
            typeIndex = 0;
            QSettings settings;
            settings.setValue(QStringLiteral("outliner/idTypeFilter"),
                              QString{});
        }
        idTypeCombo_->setCurrentIndex(typeIndex);
        idTypeCombo_->setVisible(librariesMode || orphanDataMode);
        idTypeCombo_->setEnabled((librariesMode || orphanDataMode) &&
                                 idTypeCombo_->count() > 1);
    }
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
            const bool belongs = entry.collection.sceneMembershipExplicit
                ? entry.collection.sourceSceneNames.contains(scene.displayName)
                : (entry.collection.sourceSceneNames.isEmpty()
                       ? scene.key == activeSceneKey
                       : entry.collection.sourceSceneNames.contains(
                             scene.displayName));
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
        sceneCombo_->setVisible(
            !scenesMode && !librariesMode && !orphanDataMode &&
            !sequencerMode && !dataApiMode && !libraryOverridesMode &&
            scenes.size() > 1);
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
        viewLayerCombo_->setVisible(
            !scenesMode && !librariesMode && !orphanDataMode &&
            !sequencerMode && !dataApiMode && !libraryOverridesMode &&
            viewLayers.size() > 1);
    }
    tree_->headerItem()->setText(
        viewportColumn,
        overridePropertiesMode ? QStringLiteral("Value / Operation")
                               : QStringLiteral("Viewport"));
    tree_->headerItem()->setToolTip(
        viewportColumn,
        overridePropertiesMode
            ? QStringLiteral("Read-only value and operation from Blender's library override")
            : QStringLiteral("Hide globally in the viewport. Shift-click applies to descendants; Ctrl-click on a collection isolates it."));
    tree_->setColumnHidden(
        viewportColumn,
        dataApiMode ||
            (overridePropertiesMode
                 ? false
                 : (librariesMode || orphanDataMode || sequencerMode ||
                    overrideHierarchiesMode || !showViewportColumn_)));
    tree_->setColumnHidden(renderColumn,
                           librariesMode || orphanDataMode || sequencerMode ||
                           dataApiMode || libraryOverridesMode ||
                               !showRenderColumn_);
    tree_->setColumnHidden(selectColumn,
                           librariesMode || orphanDataMode || sequencerMode ||
                               dataApiMode || libraryOverridesMode ||
                               !showSelectColumn_);
    tree_->setColumnHidden(viewLayerViewportColumn,
                           librariesMode || orphanDataMode || sequencerMode ||
                               dataApiMode || libraryOverridesMode ||
                               !showHideColumn_);
    tree_->setColumnHidden(holdoutColumn,
                           librariesMode || orphanDataMode || sequencerMode ||
                               dataApiMode || libraryOverridesMode || scenesMode ||
                               activeViewLayerKey.isEmpty() ||
                               !showHoldoutColumn_);
    tree_->setColumnHidden(indirectOnlyColumn,
                           librariesMode || orphanDataMode || sequencerMode ||
                               dataApiMode || libraryOverridesMode || scenesMode ||
                               activeViewLayerKey.isEmpty() ||
                               !showIndirectOnlyColumn_);
    tree_->setColumnHidden(excludeColumn,
                           librariesMode || orphanDataMode || sequencerMode ||
                               dataApiMode || libraryOverridesMode || scenesMode ||
                               activeViewLayerKey.isEmpty() ||
                               !showEnableColumn_);
    tree_->setColumnHidden(modeColumn,
                           libraryOverridesMode || orphanDataMode ||
                               sequencerMode || dataApiMode ||
                               !showModeColumn_);
    QVector<QTreeWidgetItem *> sceneRoots;
    const auto topLevelCollectionIndices = [](
        const QVector<CollectionPanelEntry> &entries) {
        QVector<int> roots;
        for (int index = 0; index < entries.size(); ++index) {
            const DocumentCollection &collection = entries[index].collection;
            bool hasKnownParent = false;
            for (const CollectionId parentId : collection.parentIds) {
                hasKnownParent = hasKnownParent || std::any_of(
                    entries.cbegin(), entries.cend(),
                    [parentId](const CollectionPanelEntry &candidate) {
                        return candidate.collection.id == parentId;
                    });
            }
            if (!hasKnownParent) roots.append(index);
        }
        const auto orderFor = [&entries](int index) {
            const int explicitOrder = entries[index].collection.rootOrder;
            return explicitOrder >= 0 ? explicitOrder : index;
        };
        std::stable_sort(roots.begin(), roots.end(),
                         [&orderFor](int left, int right) {
            return orderFor(left) < orderFor(right);
        });
        return roots;
    };
    const auto populateSceneRoot = [this, &topLevelCollectionIndices](
                                       QTreeWidgetItem *root,
                                       const QVector<CollectionPanelEntry> &entries,
                                       const QVector<OutlinerObjectEntry> &objects,
                                       const QString &viewLayerKey,
                                       const QString &rootPath) {
        if (showCollections_) {
            for (const int rootIndex : topLevelCollectionIndices(entries)) {
                const CollectionPanelEntry &entry = entries[rootIndex];
                if (entry.collection.id.isValid()) {
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
    if (dataApiMode) {
        const QJsonObject rootNode = dataApi.value(
            QStringLiteral("root")).toObject();
        const QString rootName = rootNode.value(
            QStringLiteral("name")).toString(QStringLiteral("RNA"));
        auto *root = new QTreeWidgetItem(tree_);
        root->setText(nameColumn, rootName);
        root->setData(nameColumn, itemKindRole,
                      static_cast<int>(OutlinerRowKind::DataApi));
        root->setData(nameColumn, itemPathRole,
                      QStringLiteral("data-api/main"));
        root->setData(nameColumn, itemDataApiKindRole,
                      rootNode.value(QStringLiteral("kind")).toString());
        root->setData(nameColumn, itemDataApiTypeRole,
                      rootNode.value(QStringLiteral("type")).toString());
        root->setData(nameColumn, itemDataApiNodeRole,
                      QVariant::fromValue(rootNode));
        root->setData(nameColumn, itemDataApiTruncatedRole,
                      dataApi.value(QStringLiteral("truncated")).toBool());
        root->setToolTip(
            nameColumn,
            QStringLiteral("Blender RNA Data API captured at import; selected property paths can be added to the active Keying Set"));
        root->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        auto *placeholder = new QTreeWidgetItem(root);
        placeholder->setData(nameColumn, itemDataApiPlaceholderRole, true);
        placeholder->setFlags(Qt::NoItemFlags);
        sceneRoots.append(root);
    } else if (libraryOverridesMode) {
        if (overridePropertiesMode) {
            const QJsonObject properties = libraryOverrides.value(
                QStringLiteral("properties")).toObject();
            const QJsonArray groups = properties.value(
                QStringLiteral("groups")).toArray();
            for (qsizetype groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
                const QJsonObject sourceGroup = groups[groupIndex].toObject();
                auto *groupRow = new QTreeWidgetItem(tree_);
                groupRow->setText(nameColumn, sourceGroup.value(
                    QStringLiteral("name")).toString());
                groupRow->setData(nameColumn, itemKindRole,
                                  static_cast<int>(OutlinerRowKind::SceneGroup));
                const QString groupPath = QStringLiteral("overrides/properties/type-%1")
                                              .arg(groupIndex);
                groupRow->setData(nameColumn, itemPathRole, groupPath);
                groupRow->setToolTip(
                    nameColumn,
                    QStringLiteral("Blender library overrides of this data-block type"));
                groupRow->setFlags(Qt::ItemIsEnabled);
                bool hasVisibleItems = false;
                const QJsonArray items = sourceGroup.value(
                    QStringLiteral("items")).toArray();
                for (qsizetype itemIndex = 0; itemIndex < items.size(); ++itemIndex) {
                    const QJsonObject sourceItem = items[itemIndex].toObject();
                    QVector<QJsonObject> visibleProperties;
                    for (const QJsonValue &propertyValue : sourceItem.value(
                             QStringLiteral("properties")).toArray()) {
                        if (!propertyValue.isObject()) continue;
                        const QJsonObject property = propertyValue.toObject();
                        if (!showSystemOverrides_ &&
                            property.value(QStringLiteral("system")).toBool()) {
                            continue;
                        }
                        visibleProperties.append(property);
                    }
                    if (visibleProperties.isEmpty()) continue;
                    hasVisibleItems = true;
                    auto *idRow = new QTreeWidgetItem(groupRow);
                    const QString id = sourceItem.value(
                        QStringLiteral("id")).toString();
                    const QString idPath = groupPath + QStringLiteral("/id-%1")
                                               .arg(id);
                    idRow->setText(nameColumn, sourceItem.value(
                        QStringLiteral("name")).toString());
                    idRow->setData(nameColumn, itemKindRole,
                                   static_cast<int>(OutlinerRowKind::ObjectContent));
                    idRow->setData(nameColumn, itemPathRole, idPath);
                    QString description = QStringLiteral(
                        "Read-only Blender library override for %1")
                        .arg(sourceItem.value(QStringLiteral("reference_name"))
                                 .toString(QStringLiteral("an unavailable reference")));
                    const QString libraryPath = sourceItem.value(
                        QStringLiteral("reference_library_path")).toString();
                    if (!libraryPath.isEmpty()) {
                        description += QStringLiteral(" from %1").arg(libraryPath);
                    }
                    idRow->setToolTip(nameColumn, description);
                    idRow->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);

                    QHash<QString, QTreeWidgetItem *> pathRows;
                    for (qsizetype propertyIndex = 0;
                         propertyIndex < visibleProperties.size(); ++propertyIndex) {
                        const QJsonObject property = visibleProperties[propertyIndex];
                        QJsonArray segments = property.value(
                            QStringLiteral("segments")).toArray();
                        if (segments.isEmpty()) {
                            QJsonObject fallbackSegment;
                            fallbackSegment.insert(QStringLiteral("key"),
                                QStringLiteral("property:%1").arg(
                                    property.value(QStringLiteral("path")).toString()));
                            fallbackSegment.insert(QStringLiteral("name"),
                                property.value(QStringLiteral("name")).toString());
                            fallbackSegment.insert(QStringLiteral("kind"),
                                                   QStringLiteral("PROPERTY"));
                            segments.append(fallbackSegment);
                        }
                        QTreeWidgetItem *parent = idRow;
                        QString relativePath;
                        QTreeWidgetItem *propertyRow = nullptr;
                        for (qsizetype segmentIndex = 0;
                             segmentIndex < segments.size(); ++segmentIndex) {
                            const QJsonObject segment = segments[segmentIndex].toObject();
                            const QString key = segment.value(
                                QStringLiteral("key")).toString();
                            relativePath += QLatin1Char('/') + key;
                            propertyRow = pathRows.value(relativePath, nullptr);
                            if (propertyRow == nullptr) {
                                propertyRow = new QTreeWidgetItem(parent);
                                propertyRow->setText(nameColumn, segment.value(
                                    QStringLiteral("name")).toString());
                                propertyRow->setData(
                                    nameColumn, itemKindRole,
                                    static_cast<int>(OutlinerRowKind::ObjectContent));
                                propertyRow->setData(
                                    nameColumn, itemPathRole,
                                    idPath + relativePath);
                                propertyRow->setFlags(Qt::ItemIsEnabled |
                                                      Qt::ItemIsSelectable);
                                pathRows.insert(relativePath, propertyRow);
                            }
                            parent = propertyRow;
                        }
                        if (propertyRow == nullptr) continue;
                        const QString operationPath = property.value(
                            QStringLiteral("path")).toString();
                        const bool pathValid = property.value(
                            QStringLiteral("valid")).toBool();
                        propertyRow->setText(viewportColumn, property.value(
                            QStringLiteral("value")).toString());
                        QStringList operationLabels;
                        for (const QJsonValue &operationValue : property.value(
                                 QStringLiteral("operations")).toArray()) {
                            const QJsonObject operation = operationValue.toObject();
                            const QString label = operation.value(
                                QStringLiteral("label")).toString();
                            if (!label.isEmpty() && !operationLabels.contains(label)) {
                                operationLabels.append(label);
                            }
                        }
                        QString tooltip = QStringLiteral("Blender RNA path: %1")
                                              .arg(operationPath);
                        if (!pathValid) {
                            tooltip += QStringLiteral("\nThis override path no longer resolves in the imported data.");
                        }
                        if (!operationLabels.isEmpty()) {
                            tooltip += QStringLiteral("\n%1")
                                           .arg(operationLabels.join(QStringLiteral("; ")));
                            if (propertyRow->text(viewportColumn).isEmpty()) {
                                propertyRow->setText(viewportColumn,
                                    operationLabels.join(QStringLiteral("; ")));
                            }
                        }
                        propertyRow->setToolTip(nameColumn, tooltip);
                    }
                }
                if (!hasVisibleItems) {
                    delete groupRow;
                } else {
                    sceneRoots.append(groupRow);
                    groupRow->setExpanded(true);
                }
            }
            if (properties.value(QStringLiteral("truncated")).toBool()) {
                auto *truncatedRow = new QTreeWidgetItem(tree_);
                truncatedRow->setText(nameColumn,
                    QStringLiteral("Additional override data omitted"));
                truncatedRow->setData(nameColumn, itemKindRole,
                    static_cast<int>(OutlinerRowKind::ObjectContent));
                truncatedRow->setData(nameColumn, itemPathRole,
                    QStringLiteral("overrides/properties/truncated"));
                truncatedRow->setToolTip(nameColumn,
                    QStringLiteral("The bounded Blender override extraction limit was reached"));
                truncatedRow->setFlags(Qt::ItemIsEnabled);
            }
        } else {
            const QJsonArray hierarchies = libraryOverrides.value(
                QStringLiteral("hierarchies")).toArray();
            const auto appendHierarchyNode = [this](
                const auto &self, const QJsonObject &source,
                QTreeWidgetItem *parent, const QString &parentPath) -> void {
                auto *row = new QTreeWidgetItem(parent);
                row->setText(nameColumn, source.value(
                    QStringLiteral("name")).toString());
                row->setData(nameColumn, itemKindRole,
                    static_cast<int>(OutlinerRowKind::ObjectContent));
                const QString id = source.value(QStringLiteral("id")).toString();
                const QString path = parentPath + QStringLiteral("/id-%1").arg(id);
                row->setData(nameColumn, itemPathRole, path);
                row->setToolTip(nameColumn,
                    QStringLiteral("Blender %1 in this library override hierarchy")
                        .arg(source.value(QStringLiteral("type")).toString()));
                row->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
                for (const QJsonValue &childValue : source.value(
                         QStringLiteral("children")).toArray()) {
                    if (childValue.isObject()) {
                        self(self, childValue.toObject(), row, path);
                    }
                }
            };
            for (qsizetype libraryIndex = 0;
                 libraryIndex < hierarchies.size(); ++libraryIndex) {
                const QJsonObject sourceLibrary = hierarchies[libraryIndex].toObject();
                auto *libraryRow = new QTreeWidgetItem(tree_);
                libraryRow->setText(nameColumn, sourceLibrary.value(
                    QStringLiteral("name")).toString());
                libraryRow->setData(nameColumn, itemKindRole,
                    static_cast<int>(OutlinerRowKind::Library));
                libraryRow->setData(nameColumn, itemLibraryCurrentFileRole,
                    sourceLibrary.value(QStringLiteral("current_file")).toBool());
                const QString libraryPath = QStringLiteral("overrides/hierarchies/%1")
                    .arg(sourceLibrary.value(QStringLiteral("id")).toString());
                libraryRow->setData(nameColumn, itemPathRole, libraryPath);
                libraryRow->setToolTip(nameColumn,
                    sourceLibrary.value(QStringLiteral("current_file")).toBool()
                        ? QStringLiteral("Library override hierarchies in the current Blender file")
                        : QStringLiteral("Library override hierarchies from this Blender library"));
                libraryRow->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
                sceneRoots.append(libraryRow);
                const QJsonArray groups = sourceLibrary.value(
                    QStringLiteral("groups")).toArray();
                for (qsizetype groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
                    const QJsonObject sourceGroup = groups[groupIndex].toObject();
                    auto *groupRow = new QTreeWidgetItem(libraryRow);
                    groupRow->setText(nameColumn, sourceGroup.value(
                        QStringLiteral("name")).toString());
                    groupRow->setData(nameColumn, itemKindRole,
                        static_cast<int>(OutlinerRowKind::SceneGroup));
                    const QString groupPath = libraryPath + QStringLiteral("/type-%1")
                                                  .arg(groupIndex);
                    groupRow->setData(nameColumn, itemPathRole, groupPath);
                    groupRow->setToolTip(nameColumn,
                        QStringLiteral("Root Blender IDs of this type"));
                    groupRow->setFlags(Qt::ItemIsEnabled);
                    const QJsonArray roots = sourceGroup.value(
                        QStringLiteral("items")).toArray();
                    for (const QJsonValue &rootValue : roots) {
                        if (rootValue.isObject()) {
                            appendHierarchyNode(appendHierarchyNode,
                                rootValue.toObject(), groupRow, groupPath);
                        }
                    }
                }
                if (sourceLibrary.value(QStringLiteral("current_file")).toBool() &&
                    groups.isEmpty()) {
                    auto *emptyRow = new QTreeWidgetItem(libraryRow);
                    emptyRow->setText(nameColumn,
                        QStringLiteral("No Library Overrides"));
                    emptyRow->setData(nameColumn, itemKindRole,
                        static_cast<int>(OutlinerRowKind::ObjectContent));
                    emptyRow->setData(nameColumn, itemPathRole,
                        libraryPath + QStringLiteral("/empty"));
                    emptyRow->setFlags(Qt::ItemIsEnabled);
                }
                if (sourceLibrary.value(QStringLiteral("truncated")).toBool()) {
                    auto *truncatedRow = new QTreeWidgetItem(libraryRow);
                    truncatedRow->setText(nameColumn,
                        QStringLiteral("Additional hierarchy rows omitted"));
                    truncatedRow->setData(nameColumn, itemKindRole,
                        static_cast<int>(OutlinerRowKind::ObjectContent));
                    truncatedRow->setData(nameColumn, itemPathRole,
                        libraryPath + QStringLiteral("/truncated"));
                    truncatedRow->setFlags(Qt::ItemIsEnabled);
                }
            }
        }
        if (libraryOverrides.value(QStringLiteral("truncated")).toBool()) {
            auto *truncatedRow = new QTreeWidgetItem(tree_);
            truncatedRow->setText(nameColumn,
                QStringLiteral("Additional override rows omitted"));
            truncatedRow->setData(nameColumn, itemKindRole,
                static_cast<int>(OutlinerRowKind::ObjectContent));
            truncatedRow->setData(nameColumn, itemPathRole,
                QStringLiteral("overrides/truncated"));
            truncatedRow->setToolTip(nameColumn,
                QStringLiteral("The 20,000-row Blender override extraction limit was reached"));
            truncatedRow->setFlags(Qt::ItemIsEnabled);
        }
    } else if (librariesMode) {
        const auto appendLibraryContentRow = [this](
            QTreeWidgetItem *parent, const QString &name,
            const QString &path, const QString &description)
                -> QTreeWidgetItem * {
            if (parent == nullptr || name.isEmpty()) return nullptr;
            auto *row = new QTreeWidgetItem(parent);
            row->setText(nameColumn, name);
            row->setData(nameColumn, itemKindRole,
                static_cast<int>(OutlinerRowKind::ObjectContent));
            row->setData(nameColumn, itemPathRole, path);
            row->setToolTip(nameColumn, description);
            row->setFlags(Qt::ItemIsEnabled);
            return row;
        };
        const auto appendLibraryAnimation = [&appendLibraryContentRow](
            QTreeWidgetItem *parent, const OutlinerAnimationEntry &animation,
            const QString &path) {
            if (!animation.hasAnimationData) return;
            QTreeWidgetItem *animationRow = appendLibraryContentRow(
                parent, QStringLiteral("Animation"), path,
                QStringLiteral("Blender data-block animation data"));
            const QString animationPath = path;
            if (!animation.actionName.isEmpty()) {
                QTreeWidgetItem *actionRow = appendLibraryContentRow(
                    animationRow, animation.actionName,
                    animationPath + QStringLiteral("/action"),
                    QStringLiteral("Blender active Action data block"));
                appendLibraryContentRow(
                    actionRow, animation.actionSlotName,
                    animationPath + QStringLiteral("/action/slot"),
                    QStringLiteral("Blender assigned Action slot"));
            }
            if (animation.hasDrivers) {
                QTreeWidgetItem *driversRow = appendLibraryContentRow(
                    animationRow, QStringLiteral("Drivers"),
                    animationPath + QStringLiteral("/drivers"),
                    QStringLiteral("Blender animation drivers"));
                for (qsizetype index = 0;
                     index < animation.driverTargetNames.size(); ++index) {
                    appendLibraryContentRow(
                        driversRow, animation.driverTargetNames[index],
                        animationPath + QStringLiteral("/driver-target-%1")
                            .arg(index),
                        QStringLiteral("Blender driver target data block"));
                }
            }
            if (!animation.nlaTracks.isEmpty()) {
                QTreeWidgetItem *tracksRow = appendLibraryContentRow(
                    animationRow, QStringLiteral("NLA Tracks"),
                    animationPath + QStringLiteral("/nla-tracks"),
                    QStringLiteral("Blender non-linear animation tracks"));
                for (qsizetype trackIndex = 0;
                     trackIndex < animation.nlaTracks.size(); ++trackIndex) {
                    const OutlinerNlaTrackEntry &track =
                        animation.nlaTracks[trackIndex];
                    const QString trackPath = animationPath +
                        QStringLiteral("/nla-track-%1").arg(trackIndex);
                    QTreeWidgetItem *trackRow = appendLibraryContentRow(
                        tracksRow, track.name, trackPath,
                        QStringLiteral("Blender NLA track"));
                    for (qsizetype actionIndex = 0;
                         actionIndex < track.actionNames.size(); ++actionIndex) {
                        appendLibraryContentRow(
                            trackRow, track.actionNames[actionIndex],
                            trackPath + QStringLiteral("/action-%1")
                                .arg(actionIndex),
                            QStringLiteral("Blender NLA Action data block"));
                    }
                }
            }
        };
        const auto appendLibraryNameTree = [&appendLibraryContentRow](
            const auto &self, QTreeWidgetItem *parent,
            const QJsonArray &nodes, const QString &path,
            const QString &description) -> void {
            for (qsizetype index = 0; index < nodes.size(); ++index) {
                const QJsonObject node = nodes[index].toObject();
                const QString nodePath = path + QStringLiteral("/item-%1")
                    .arg(index);
                QTreeWidgetItem *row = appendLibraryContentRow(
                    parent, node.value(QStringLiteral("name")).toString(),
                    nodePath, description);
                self(self, row, node.value(
                    QStringLiteral("children")).toArray(), nodePath,
                    description);
            }
        };
        const auto appendLibraryObjectTree = [&appendLibraryContentRow](
            const auto &self, QTreeWidgetItem *parent,
            const QJsonArray &nodes, const QString &path,
            const auto &appendReferencedIdChildren) -> void {
            const bool parentObjectIsPoseMode = std::any_of(
                nodes.cbegin(), nodes.cend(), [](const QJsonValue &value) {
                    return value.toObject().value(
                        QStringLiteral("kind")).toString() ==
                        QLatin1String("POSE");
                });
            for (qsizetype index = 0; index < nodes.size(); ++index) {
                const QJsonObject node = nodes[index].toObject();
                const QString nodePath = path + QStringLiteral("/item-%1")
                    .arg(index);
                const QString kind = node.value(
                    QStringLiteral("kind")).toString();
                QString description;
                if (kind == QLatin1String("POSE")) {
                    description = QStringLiteral("Blender object pose");
                } else if (kind == QLatin1String("POSE_BONE")) {
                    description = QStringLiteral("Blender pose bone");
                } else if (kind == QLatin1String("DATA_BLOCK")) {
                    description = QStringLiteral("Blender object data block");
                } else if (kind == QLatin1String("MATERIAL")) {
                    description = QStringLiteral("Blender object material slot");
                } else if (kind == QLatin1String("CONSTRAINTS")) {
                    description = QStringLiteral("Blender object constraints");
                } else if (kind == QLatin1String("CONSTRAINT")) {
                    description = QStringLiteral("Blender object constraint");
                } else if (kind == QLatin1String("MODIFIERS")) {
                    description = QStringLiteral("Blender object modifiers");
                } else if (kind == QLatin1String("MODIFIER")) {
                    description = QStringLiteral("Blender object modifier");
                } else if (kind == QLatin1String("EFFECTS")) {
                    description = QStringLiteral("Blender Grease Pencil effects");
                } else if (kind == QLatin1String("EFFECT")) {
                    description = QStringLiteral("Blender Grease Pencil effect");
                } else if (kind == QLatin1String("VERTEX_GROUPS")) {
                    description = QStringLiteral("Blender object vertex groups");
                } else if (kind == QLatin1String("VERTEX_GROUP")) {
                    description = QStringLiteral("Blender object vertex group");
                } else if (kind == QLatin1String("COLLECTION_INSTANCE")) {
                    description = QStringLiteral("Blender instanced collection");
                }
                QTreeWidgetItem *row = appendLibraryContentRow(
                    parent, node.value(QStringLiteral("name")).toString(),
                    nodePath, description);
                if (kind == QLatin1String("DATA_BLOCK") ||
                    kind == QLatin1String("MATERIAL")) {
                    appendReferencedIdChildren(
                        row, node, nodePath, parentObjectIsPoseMode);
                }
                self(self, row, node.value(
                    QStringLiteral("children")).toArray(), nodePath,
                    appendReferencedIdChildren);
            }
        };
        const auto appendLibraryCollectionTree = [
            &appendLibraryContentRow, &appendLibraryObjectTree](
            const auto &self, QTreeWidgetItem *parent,
            const QJsonArray &nodes, const QString &path,
            const auto &appendReferencedIdChildren) -> void {
            for (qsizetype index = 0; index < nodes.size(); ++index) {
                const QJsonObject node = nodes[index].toObject();
                const QString nodePath = path + QStringLiteral("/item-%1")
                    .arg(index);
                const QString kind = node.value(
                    QStringLiteral("kind")).toString();
                const bool isCollection = kind == QLatin1String("COLLECTION");
                QTreeWidgetItem *row = appendLibraryContentRow(
                    parent, node.value(QStringLiteral("name")).toString(),
                    nodePath,
                    isCollection ? QStringLiteral("Blender child collection")
                                 : QStringLiteral("Object in Blender collection"));
                if (isCollection) {
                    self(self, row, node.value(
                        QStringLiteral("children")).toArray(), nodePath,
                        appendReferencedIdChildren);
                } else {
                    appendReferencedIdChildren(row, node, nodePath, false);
                    appendLibraryObjectTree(
                        appendLibraryObjectTree, row,
                        node.value(QStringLiteral("children")).toArray(),
                        nodePath + QStringLiteral("/object-contents"),
                        appendReferencedIdChildren);
                }
            }
        };
        QHash<QString, QTreeWidgetItem *> libraryRows;
        for (const OutlinerLibraryEntry &library : libraries) {
            auto *row = new QTreeWidgetItem;
            row->setText(nameColumn, library.displayName);
            row->setData(nameColumn, itemKindRole,
                         static_cast<int>(OutlinerRowKind::Library));
            row->setData(nameColumn, itemLibraryCurrentFileRole,
                         library.currentFile);
            const QString path = QStringLiteral("libraries/%1")
                                     .arg(library.id);
            row->setData(nameColumn, itemPathRole, path);
            row->setToolTip(
                nameColumn,
                library.currentFile
                    ? QStringLiteral("Blender current file data blocks")
                    : QStringLiteral("Linked Blender library: %1")
                          .arg(library.displayName));
            libraryRows.insert(library.id, row);
        }
        for (const OutlinerLibraryEntry &library : libraries) {
            qsizetype linkedAnimationRowsRemaining = 20000;
            bool linkedAnimationRowsTruncated = false;
            qsizetype linkedDataRowsRemaining = 20000;
            bool linkedDataRowsTruncated = false;
            const auto findLibraryDataGroup = [&library](
                const QString &displayName) -> const OutlinerLibraryDataGroup * {
                for (const OutlinerLibraryDataGroup &group : library.dataGroups) {
                    if (group.displayName == displayName) return &group;
                }
                return nullptr;
            };
            const auto resolveLibraryDataGroupName = [&library](
                const QString &dataBlockName, const QString &preferredName) {
                if (!preferredName.isEmpty()) return preferredName;
                QString resolvedName;
                for (const OutlinerLibraryDataGroup &group : library.dataGroups) {
                    if (!group.dataBlockNames.contains(dataBlockName)) continue;
                    if (!resolvedName.isEmpty()) return QString();
                    resolvedName = group.displayName;
                }
                return resolvedName;
            };
            const auto countLibraryAnimationRows = [](
                const OutlinerAnimationEntry &animation) {
                if (!animation.hasAnimationData) return qsizetype{0};
                qsizetype count = 1;
                if (!animation.actionName.isEmpty()) {
                    count += 1 + (animation.actionSlotName.isEmpty() ? 0 : 1);
                }
                if (animation.hasDrivers) {
                    count += 1 + animation.driverTargetNames.size();
                }
                if (!animation.nlaTracks.isEmpty()) {
                    ++count;
                    for (const OutlinerNlaTrackEntry &track : animation.nlaTracks) {
                        count += 1 + track.actionNames.size();
                    }
                }
                return count;
            };
            const auto countLibraryTreeRows = [](const auto &self,
                                                  const QJsonArray &nodes)
                    -> qsizetype {
                qsizetype count = 0;
                for (const QJsonValue &value : nodes) {
                    const QJsonObject node = value.toObject();
                    ++count;
                    count += self(self, node.value(
                        QStringLiteral("children")).toArray());
                }
                return count;
            };
            const auto countLibraryCollectionRows = [
                &countLibraryTreeRows](const auto &self,
                    const QJsonArray &nodes) -> qsizetype {
                qsizetype count = 0;
                for (const QJsonValue &value : nodes) {
                    const QJsonObject node = value.toObject();
                    ++count;
                    const QJsonArray children = node.value(
                        QStringLiteral("children")).toArray();
                    if (node.value(QStringLiteral("kind")).toString() ==
                        QLatin1String("COLLECTION")) {
                        count += self(self, children);
                    } else {
                        count += countLibraryTreeRows(
                            countLibraryTreeRows, children);
                    }
                }
                return count;
            };
            const auto countLibrarySceneObjectRows = [
                &countLibraryTreeRows](const auto &self,
                    const QJsonArray &nodes) -> qsizetype {
                qsizetype count = 0;
                for (const QJsonValue &value : nodes) {
                    const QJsonObject node = value.toObject();
                    ++count;
                    count += countLibraryTreeRows(
                        countLibraryTreeRows,
                        node.value(QStringLiteral("object_contents")).toArray());
                    count += self(self, node.value(
                        QStringLiteral("children")).toArray());
                }
                return count;
            };
            const auto countLibrarySceneTreeRows = [
                &countLibraryCollectionRows,
                &countLibrarySceneObjectRows](const QJsonObject &tree) {
                if (tree.isEmpty()) return qsizetype{0};
                qsizetype count =
                    (tree.value(QStringLiteral("has_view_layers_group")).toBool()
                        ? 1 + tree.value(QStringLiteral("view_layer_names"))
                                  .toArray().size() : 0) +
                    (!tree.value(QStringLiteral("world_name")).toString().isEmpty()
                        ? 1 : 0);
                if (tree.value(QStringLiteral(
                        "has_scene_collection_group")).toBool()) {
                    ++count;
                    count += countLibraryCollectionRows(
                        countLibraryCollectionRows,
                        tree.value(QStringLiteral("collection_tree")).toArray());
                }
                if (tree.value(QStringLiteral("has_objects_group")).toBool()) {
                    ++count;
                    count += countLibrarySceneObjectRows(
                        countLibrarySceneObjectRows,
                        tree.value(QStringLiteral("object_tree")).toArray());
                }
                return count;
            };
            const auto countLibraryDataBlockRows = [
                &findLibraryDataGroup, &countLibraryTreeRows,
                &countLibraryCollectionRows, &countLibrarySceneTreeRows](
                const OutlinerLibraryDataGroup::DataBlockTree &tree) {
                qsizetype count = countLibraryTreeRows(
                    countLibraryTreeRows, tree.objectTree);
                count += countLibraryCollectionRows(
                    countLibraryCollectionRows, tree.collectionTree);
                count += countLibrarySceneTreeRows(tree.sceneTree);
                count += countLibraryTreeRows(
                    countLibraryTreeRows, tree.armatureBoneTree);
                count += countLibraryTreeRows(
                    countLibraryTreeRows, tree.greasePencilLayerTree);
                if (!tree.boneCollectionTree.isEmpty()) {
                    ++count;
                    count += countLibraryTreeRows(
                        countLibraryTreeRows, tree.boneCollectionTree);
                }
                if (!tree.shapeKeyDataName.isEmpty()) {
                    count += 1 + tree.shapeKeyNames.size();
                }
                count += tree.materialNames.size();
                if (!tree.textureImageName.isEmpty()) ++count;
                const OutlinerLibraryDataGroup *textures =
                    findLibraryDataGroup(QStringLiteral("Textures"));
                for (const QString &textureName : tree.lineStyleTextureNames) {
                    ++count;
                    if (textures == nullptr) continue;
                    const auto textureTree =
                        textures->dataBlockTreesByName.constFind(textureName);
                    if (textureTree != textures->dataBlockTreesByName.cend() &&
                        !textureTree->textureImageName.isEmpty()) {
                        ++count;
                    }
                }
                return count;
            };
            const auto appendLibraryAnimationByGroup = [
                &findLibraryDataGroup, &appendLibraryAnimation,
                &countLibraryAnimationRows, &linkedAnimationRowsRemaining,
                &linkedAnimationRowsTruncated](
                QTreeWidgetItem *parent, const QString &groupName,
                const QString &dataBlockName, const QString &path) {
                const OutlinerLibraryDataGroup *group =
                    findLibraryDataGroup(groupName);
                if (group == nullptr) return;
                const auto animation =
                    group->animationsByDataBlockName.constFind(dataBlockName);
                if (animation != group->animationsByDataBlockName.cend()) {
                    const qsizetype rowCount =
                        countLibraryAnimationRows(animation.value());
                    if (rowCount > linkedAnimationRowsRemaining) {
                        linkedAnimationRowsTruncated = true;
                        return;
                    }
                    linkedAnimationRowsRemaining -= rowCount;
                    appendLibraryAnimation(parent, animation.value(), path);
                }
            };
            const auto appendLibraryTextureImage = [
                &findLibraryDataGroup, &appendLibraryContentRow,
                &appendLibraryAnimationByGroup](QTreeWidgetItem *parent,
                    const QString &textureName, const QString &texturePath) {
                const OutlinerLibraryDataGroup *texturesGroup =
                    findLibraryDataGroup(QStringLiteral("Textures"));
                if (texturesGroup == nullptr) return;
                const auto textureTree =
                    texturesGroup->dataBlockTreesByName.constFind(textureName);
                if (textureTree == texturesGroup->dataBlockTreesByName.cend() ||
                    textureTree->textureImageName.isEmpty()) return;
                const QString imagePath = texturePath + QStringLiteral("/image");
                QTreeWidgetItem *imageRow = appendLibraryContentRow(
                    parent, textureTree->textureImageName, imagePath,
                    QStringLiteral("Blender Texture image ID"));
                appendLibraryAnimationByGroup(
                    imageRow, QStringLiteral("Images"),
                    textureTree->textureImageName,
                    imagePath + QStringLiteral("/animation"));
            };
            const auto appendLibraryTextureId = [
                &appendLibraryContentRow, &appendLibraryAnimationByGroup,
                &appendLibraryTextureImage](QTreeWidgetItem *parent,
                    const QString &textureName, const QString &path) {
                QTreeWidgetItem *textureRow = appendLibraryContentRow(
                    parent, textureName, path,
                    QStringLiteral("Blender Line Style texture ID"));
                appendLibraryAnimationByGroup(
                    textureRow, QStringLiteral("Textures"), textureName,
                    path + QStringLiteral("/animation"));
                appendLibraryTextureImage(textureRow, textureName, path);
            };
            std::function<void(QTreeWidgetItem *, const QJsonObject &,
                               const QString &, bool)>
                appendReferencedIdChildren;
            appendReferencedIdChildren = [
                &findLibraryDataGroup, &resolveLibraryDataGroupName,
                &appendLibraryAnimationByGroup,
                &countLibraryDataBlockRows, &linkedDataRowsRemaining,
                &linkedDataRowsTruncated,
                &appendLibraryObjectTree, &appendLibraryCollectionTree,
                &appendLibraryNameTree, &appendLibraryContentRow,
                &appendLibraryTextureImage, &appendLibraryTextureId,
                &appendReferencedIdChildren](QTreeWidgetItem *parent,
                    const QJsonObject &node, const QString &path,
                    bool parentObjectIsPoseMode) {
                const QString kind = node.value(
                    QStringLiteral("kind")).toString();
                const QString name = node.value(
                    QStringLiteral("name")).toString();
                QString groupName = node.value(
                    QStringLiteral("id_group_name")).toString();
                if (groupName.isEmpty() && kind == QLatin1String("OBJECT")) {
                    groupName = QStringLiteral("Objects");
                } else {
                    groupName = resolveLibraryDataGroupName(name, groupName);
                }
                appendLibraryAnimationByGroup(
                    parent, groupName, name, path + QStringLiteral("/animation"));
                if (kind != QLatin1String("DATA_BLOCK")) return;

                const OutlinerLibraryDataGroup *group =
                    findLibraryDataGroup(groupName);
                if (group == nullptr) return;
                const auto dataBlockTree =
                    group->dataBlockTreesByName.constFind(name);
                if (dataBlockTree == group->dataBlockTreesByName.cend()) return;
                const qsizetype dataRows =
                    countLibraryDataBlockRows(dataBlockTree.value());
                if (dataRows > linkedDataRowsRemaining) {
                    linkedDataRowsTruncated = true;
                    return;
                }
                linkedDataRowsRemaining -= dataRows;

                appendLibraryObjectTree(
                    appendLibraryObjectTree, parent,
                    dataBlockTree->objectTree,
                    path + QStringLiteral("/object-tree"),
                    appendReferencedIdChildren);
                if (groupName == QLatin1String("Textures")) {
                    appendLibraryTextureImage(parent, name, path);
                }
                for (qsizetype textureIndex = 0;
                     textureIndex < dataBlockTree->lineStyleTextureNames.size();
                     ++textureIndex) {
                    appendLibraryTextureId(
                        parent,
                        dataBlockTree->lineStyleTextureNames[textureIndex],
                        path + QStringLiteral("/line-style-texture-%1")
                            .arg(textureIndex));
                }
                appendLibraryCollectionTree(
                    appendLibraryCollectionTree, parent,
                    dataBlockTree->collectionTree,
                    path + QStringLiteral("/collection-tree"),
                    appendReferencedIdChildren);
                if (!parentObjectIsPoseMode) {
                    appendLibraryNameTree(
                        appendLibraryNameTree, parent,
                        dataBlockTree->armatureBoneTree,
                        path + QStringLiteral("/bones"),
                        QStringLiteral("Blender armature bone"));
                }
                appendLibraryNameTree(
                    appendLibraryNameTree, parent,
                    dataBlockTree->greasePencilLayerTree,
                    path + QStringLiteral("/grease-pencil"),
                    QStringLiteral("Blender Grease Pencil layer or group"));
                if (!dataBlockTree->boneCollectionTree.isEmpty()) {
                    const QString boneCollectionPath = path +
                        QStringLiteral("/bone-collections");
                    QTreeWidgetItem *boneCollectionsRow =
                        appendLibraryContentRow(
                            parent, QStringLiteral("Bone Collections"),
                            boneCollectionPath,
                            QStringLiteral("Blender armature bone collections"));
                    appendLibraryNameTree(
                        appendLibraryNameTree, boneCollectionsRow,
                        dataBlockTree->boneCollectionTree,
                        boneCollectionPath + QStringLiteral("/items"),
                        QStringLiteral("Blender bone collection"));
                }
                if (!dataBlockTree->shapeKeyDataName.isEmpty()) {
                    const QString shapeKeyPath = path +
                        QStringLiteral("/shape-keys");
                    QTreeWidgetItem *shapeKeysRow = appendLibraryContentRow(
                        parent, dataBlockTree->shapeKeyDataName, shapeKeyPath,
                        QStringLiteral("Blender shape-key data block"));
                    for (qsizetype keyIndex = 0;
                         keyIndex < dataBlockTree->shapeKeyNames.size();
                         ++keyIndex) {
                        appendLibraryContentRow(
                            shapeKeysRow, dataBlockTree->shapeKeyNames[keyIndex],
                            shapeKeyPath + QStringLiteral("/key-%1").arg(keyIndex),
                            QStringLiteral("Blender shape key"));
                    }
                }
                for (qsizetype materialIndex = 0;
                     materialIndex < dataBlockTree->materialNames.size();
                     ++materialIndex) {
                    const QString &materialName =
                        dataBlockTree->materialNames[materialIndex];
                    const QString materialPath = path +
                        QStringLiteral("/material-%1").arg(materialIndex);
                    QTreeWidgetItem *materialRow = appendLibraryContentRow(
                        parent, materialName, materialPath,
                        QStringLiteral("Blender data-block material ID"));
                    appendLibraryAnimationByGroup(
                        materialRow, QStringLiteral("Materials"), materialName,
                        materialPath + QStringLiteral("/animation"));
                }
            };
            std::function<void(QTreeWidgetItem *, const QJsonArray &,
                               const QString &)> appendLibrarySceneObjectTree;
            appendLibrarySceneObjectTree = [
                &appendLibraryContentRow, &appendReferencedIdChildren,
                &appendLibraryObjectTree,
                &appendLibrarySceneObjectTree](QTreeWidgetItem *parent,
                    const QJsonArray &nodes, const QString &path) {
                for (qsizetype index = 0; index < nodes.size(); ++index) {
                    const QJsonObject node = nodes[index].toObject();
                    const QString objectPath = path +
                        QStringLiteral("/item-%1").arg(index);
                    QTreeWidgetItem *objectRow = appendLibraryContentRow(
                        parent, node.value(QStringLiteral("name")).toString(),
                        objectPath, QStringLiteral("Object in Blender Scene"));
                    appendReferencedIdChildren(objectRow, node, objectPath, false);
                    appendLibraryObjectTree(
                        appendLibraryObjectTree, objectRow,
                        node.value(QStringLiteral("object_contents")).toArray(),
                        objectPath + QStringLiteral("/object-contents"),
                        appendReferencedIdChildren);
                    appendLibrarySceneObjectTree(
                        objectRow, node.value(QStringLiteral("children")).toArray(),
                        objectPath + QStringLiteral("/children"));
                }
            };
            const auto appendLibrarySceneTree = [
                &appendLibraryContentRow, &appendLibraryAnimationByGroup,
                &appendLibraryCollectionTree,
                &appendLibrarySceneObjectTree,
                &appendReferencedIdChildren](QTreeWidgetItem *parent,
                    const QJsonObject &sceneTree, const QString &path) {
                if (sceneTree.value(QStringLiteral(
                        "has_view_layers_group")).toBool()) {
                    const QString viewLayersPath = path +
                        QStringLiteral("/view-layers");
                    QTreeWidgetItem *viewLayersRow = appendLibraryContentRow(
                        parent, QStringLiteral("View Layers"), viewLayersPath,
                        QStringLiteral("Blender Scene view layers"));
                    const QJsonArray names = sceneTree.value(
                        QStringLiteral("view_layer_names")).toArray();
                    for (qsizetype index = 0; index < names.size(); ++index) {
                        appendLibraryContentRow(
                            viewLayersRow, names[index].toString(),
                            viewLayersPath + QStringLiteral("/item-%1").arg(index),
                            QStringLiteral("Blender View Layer"));
                    }
                }
                const QString worldName = sceneTree.value(
                    QStringLiteral("world_name")).toString();
                if (!worldName.isEmpty()) {
                    const QString worldPath = path + QStringLiteral("/world");
                    QTreeWidgetItem *worldRow = appendLibraryContentRow(
                        parent, worldName, worldPath,
                        QStringLiteral("Blender Scene World ID"));
                    appendLibraryAnimationByGroup(
                        worldRow, sceneTree.value(
                            QStringLiteral("world_group_name")).toString(),
                        worldName, worldPath + QStringLiteral("/animation"));
                }
                if (sceneTree.value(QStringLiteral(
                        "has_scene_collection_group")).toBool()) {
                    const QString collectionPath = path +
                        QStringLiteral("/scene-collection");
                    QTreeWidgetItem *collectionRow = appendLibraryContentRow(
                        parent, QStringLiteral("Scene Collection"),
                        collectionPath,
                        QStringLiteral("Blender Scene collection hierarchy"));
                    appendLibraryCollectionTree(
                        appendLibraryCollectionTree, collectionRow,
                        sceneTree.value(QStringLiteral(
                            "collection_tree")).toArray(),
                        collectionPath + QStringLiteral("/items"),
                        appendReferencedIdChildren);
                }
                if (sceneTree.value(QStringLiteral("has_objects_group")).toBool()) {
                    const QString objectsPath = path + QStringLiteral("/objects");
                    QTreeWidgetItem *objectsRow = appendLibraryContentRow(
                        parent, QStringLiteral("Objects"), objectsPath,
                        QStringLiteral("Blender Scene objects"));
                    appendLibrarySceneObjectTree(
                        objectsRow, sceneTree.value(
                            QStringLiteral("object_tree")).toArray(),
                        objectsPath + QStringLiteral("/items"));
                }
            };
            QTreeWidgetItem *row = libraryRows.value(library.id);
            QTreeWidgetItem *parent = libraryRows.value(library.parentId);
            if (library.currentFile || parent == nullptr || parent == row) {
                tree_->addTopLevelItem(row);
                sceneRoots.append(row);
            } else {
                parent->addChild(row);
            }
            const QString libraryPath = QStringLiteral("libraries/%1")
                                            .arg(library.id);
            for (qsizetype groupIndex = 0;
                 groupIndex < library.dataGroups.size(); ++groupIndex) {
                const OutlinerLibraryDataGroup &sourceGroup =
                    library.dataGroups[groupIndex];
                if (sourceGroup.dataBlockNames.isEmpty()) continue;
                if (!idTypeFilter_.isEmpty() &&
                    sourceGroup.displayName != idTypeFilter_) {
                    continue;
                }
                QTreeWidgetItem *dataParent = row;
                QString dataPath = libraryPath;
                if (idTypeFilter_.isEmpty()) {
                    auto *groupRow = new QTreeWidgetItem(row);
                    groupRow->setText(nameColumn, sourceGroup.displayName);
                    groupRow->setData(nameColumn, itemKindRole,
                        static_cast<int>(OutlinerRowKind::SceneGroup));
                    dataPath += QStringLiteral("/type-%1").arg(groupIndex);
                    groupRow->setData(nameColumn, itemPathRole, dataPath);
                    groupRow->setToolTip(
                        nameColumn,
                        QStringLiteral("Blender data blocks of this ID type"));
                    groupRow->setFlags(Qt::ItemIsEnabled);
                    dataParent = groupRow;
                } else {
                    dataPath += QStringLiteral("/filtered-type-%1")
                                    .arg(groupIndex);
                }
                for (qsizetype dataIndex = 0;
                     dataIndex < sourceGroup.dataBlockNames.size(); ++dataIndex) {
                    const QString &dataBlockName =
                        sourceGroup.dataBlockNames[dataIndex];
                    auto *dataRow = new QTreeWidgetItem(dataParent);
                    dataRow->setText(nameColumn, dataBlockName);
                    dataRow->setData(nameColumn, itemKindRole,
                        static_cast<int>(OutlinerRowKind::ObjectContent));
                    dataRow->setData(
                        nameColumn, itemPathRole,
                        dataPath + QStringLiteral("/item-%1").arg(dataIndex));
                    dataRow->setToolTip(
                        nameColumn,
                        QStringLiteral("Blender data block"));
                    dataRow->setFlags(Qt::ItemIsEnabled |
                                      Qt::ItemIsSelectable);
                    if (!idTypeFilter_.isEmpty()) {
                        const bool isSceneId = sourceGroup.displayName ==
                            QLatin1String("Scenes");
                        const QVector<OutlinerActionSlotEntry> actionSlots =
                            sourceGroup.actionSlotsByDataBlockName.value(
                                dataBlockName);
                        for (qsizetype slotIndex = 0;
                             slotIndex < actionSlots.size(); ++slotIndex) {
                            const OutlinerActionSlotEntry &slot =
                                actionSlots[slotIndex];
                            auto *slotRow = new QTreeWidgetItem(dataRow);
                            slotRow->setText(nameColumn, slot.displayName);
                            slotRow->setData(nameColumn, itemKindRole,
                                static_cast<int>(OutlinerRowKind::ObjectContent));
                            slotRow->setData(
                                nameColumn, itemPathRole,
                                dataPath + QStringLiteral("/item-%1/slot-%2")
                                               .arg(dataIndex).arg(slotIndex));
                            slotRow->setToolTip(
                                nameColumn,
                                QStringLiteral("Blender Action slot: %1")
                                    .arg(slot.identifier));
                            slotRow->setFlags(Qt::ItemIsEnabled |
                                              Qt::ItemIsSelectable);
                        }
                        const auto animation =
                            sourceGroup.animationsByDataBlockName.constFind(
                                dataBlockName);
                        if (!isSceneId && animation !=
                            sourceGroup.animationsByDataBlockName.cend()) {
                            appendLibraryAnimation(
                                dataRow, animation.value(),
                                dataPath + QStringLiteral("/item-%1/animation")
                                    .arg(dataIndex));
                        }
                        const auto dataBlockTree =
                            sourceGroup.dataBlockTreesByName.constFind(
                                dataBlockName);
                        if (dataBlockTree !=
                            sourceGroup.dataBlockTreesByName.cend()) {
                            const QString treePath = dataPath +
                                QStringLiteral("/item-%1").arg(dataIndex);
                            appendLibraryObjectTree(
                                appendLibraryObjectTree, dataRow,
                                dataBlockTree->objectTree,
                                treePath + QStringLiteral("/object-tree"),
                                appendReferencedIdChildren);
                            if (sourceGroup.displayName ==
                                QLatin1String("Textures")) {
                                appendLibraryTextureImage(
                                    dataRow, dataBlockName, treePath);
                            }
                            for (qsizetype textureIndex = 0;
                                 textureIndex < dataBlockTree->lineStyleTextureNames.size();
                                 ++textureIndex) {
                                appendLibraryTextureId(
                                    dataRow,
                                    dataBlockTree->lineStyleTextureNames[textureIndex],
                                    treePath + QStringLiteral("/line-style-texture-%1")
                                        .arg(textureIndex));
                            }
                            appendLibraryCollectionTree(
                                appendLibraryCollectionTree, dataRow,
                                dataBlockTree->collectionTree,
                                treePath + QStringLiteral("/collection-tree"),
                                appendReferencedIdChildren);
                            appendLibraryNameTree(
                                appendLibraryNameTree, dataRow,
                                dataBlockTree->armatureBoneTree,
                                treePath + QStringLiteral("/bones"),
                                QStringLiteral("Blender armature bone"));
                            appendLibraryNameTree(
                                appendLibraryNameTree, dataRow,
                                dataBlockTree->greasePencilLayerTree,
                                treePath + QStringLiteral("/grease-pencil"),
                                QStringLiteral("Blender Grease Pencil layer or group"));
                            if (!dataBlockTree->boneCollectionTree.isEmpty()) {
                                const QString boneCollectionPath = treePath +
                                    QStringLiteral("/bone-collections");
                                QTreeWidgetItem *boneCollectionsRow =
                                    appendLibraryContentRow(
                                        dataRow,
                                        QStringLiteral("Bone Collections"),
                                        boneCollectionPath,
                                        QStringLiteral("Blender armature bone collections"));
                                appendLibraryNameTree(
                                    appendLibraryNameTree,
                                    boneCollectionsRow,
                                    dataBlockTree->boneCollectionTree,
                                    boneCollectionPath + QStringLiteral("/items"),
                                    QStringLiteral("Blender bone collection"));
                            }
                            if (!dataBlockTree->shapeKeyDataName.isEmpty()) {
                                const QString shapeKeyPath = treePath +
                                    QStringLiteral("/shape-keys");
                                QTreeWidgetItem *shapeKeysRow =
                                    appendLibraryContentRow(
                                        dataRow,
                                        dataBlockTree->shapeKeyDataName,
                                        shapeKeyPath,
                                        QStringLiteral("Blender shape-key data block"));
                                for (qsizetype keyIndex = 0;
                                     keyIndex < dataBlockTree->shapeKeyNames.size();
                                     ++keyIndex) {
                                    appendLibraryContentRow(
                                        shapeKeysRow,
                                        dataBlockTree->shapeKeyNames[keyIndex],
                                        shapeKeyPath +
                                            QStringLiteral("/key-%1").arg(keyIndex),
                                        QStringLiteral("Blender shape key"));
                                }
                            }
                            for (qsizetype materialIndex = 0;
                                 materialIndex < dataBlockTree->materialNames.size();
                                 ++materialIndex) {
                                const QString &materialName =
                                    dataBlockTree->materialNames[materialIndex];
                                const QString materialPath = treePath +
                                    QStringLiteral("/material-%1")
                                        .arg(materialIndex);
                                QTreeWidgetItem *materialRow =
                                    appendLibraryContentRow(
                                    dataRow,
                                    materialName, materialPath,
                                    QStringLiteral("Blender data-block material ID"));
                                appendLibraryAnimationByGroup(
                                    materialRow, QStringLiteral("Materials"),
                                    materialName,
                                    materialPath + QStringLiteral("/animation"));
                            }
                            if (isSceneId) {
                                appendLibrarySceneTree(
                                    dataRow, dataBlockTree->sceneTree,
                                    treePath + QStringLiteral("/scene"));
                            }
                        }
                        if (isSceneId && animation !=
                            sourceGroup.animationsByDataBlockName.cend()) {
                            appendLibraryAnimation(
                                dataRow, animation.value(),
                                dataPath + QStringLiteral("/item-%1/animation")
                                    .arg(dataIndex));
                        }
                    }
                }
            }
            if (linkedAnimationRowsTruncated) {
                auto *truncatedRow = new QTreeWidgetItem(row);
                truncatedRow->setText(
                    nameColumn,
                    QStringLiteral("Additional linked Animation rows omitted"));
                truncatedRow->setData(nameColumn, itemKindRole,
                    static_cast<int>(OutlinerRowKind::ObjectContent));
                truncatedRow->setData(
                    nameColumn, itemPathRole,
                    libraryPath + QStringLiteral("/linked-animation-truncated"));
                truncatedRow->setToolTip(
                    nameColumn,
                    QStringLiteral("The 20,000-row linked Animation expansion limit was reached"));
                truncatedRow->setFlags(Qt::ItemIsEnabled);
            }
            if (linkedDataRowsTruncated) {
                auto *truncatedRow = new QTreeWidgetItem(row);
                truncatedRow->setText(
                    nameColumn,
                    QStringLiteral("Additional linked ID rows omitted"));
                truncatedRow->setData(nameColumn, itemKindRole,
                    static_cast<int>(OutlinerRowKind::ObjectContent));
                truncatedRow->setData(
                    nameColumn, itemPathRole,
                    libraryPath + QStringLiteral("/linked-data-truncated"));
                truncatedRow->setToolTip(
                    nameColumn,
                    QStringLiteral("The 20,000-row linked ID expansion limit was reached"));
                truncatedRow->setFlags(Qt::ItemIsEnabled);
            }
            if (library.truncated) {
                auto *truncatedRow = new QTreeWidgetItem(row);
                truncatedRow->setText(
                    nameColumn,
                    QStringLiteral("Additional Blender rows omitted"));
                truncatedRow->setData(nameColumn, itemKindRole,
                    static_cast<int>(OutlinerRowKind::ObjectContent));
                truncatedRow->setData(
                    nameColumn, itemPathRole,
                    libraryPath + QStringLiteral("/truncated"));
                truncatedRow->setToolTip(
                    nameColumn,
                    QStringLiteral("A 20,000 data-block, Action-slot, or animation-row limit was reached"));
                truncatedRow->setFlags(Qt::ItemIsEnabled);
            }
        }
        for (const OutlinerLibraryEntry &library : libraries) {
            if (!library.currentFile) continue;
            QTreeWidgetItem *currentFileRow = libraryRows.value(library.id);
            if (currentFileRow != nullptr) currentFileRow->setExpanded(true);
        }
    } else if (orphanDataMode) {
        for (qsizetype groupIndex = 0;
             groupIndex < orphanGroups.size(); ++groupIndex) {
            const OutlinerOrphanGroup &sourceGroup = orphanGroups[groupIndex];
            if (!idTypeFilter_.isEmpty() &&
                sourceGroup.displayName != idTypeFilter_) {
                continue;
            }
            const QString groupPath = QStringLiteral("orphans/type-%1")
                                          .arg(groupIndex);
            QTreeWidgetItem *dataParent = nullptr;
            QString dataPath = groupPath;
            if (idTypeFilter_.isEmpty()) {
                auto *groupRow = new QTreeWidgetItem(tree_);
                groupRow->setText(nameColumn, sourceGroup.displayName);
                groupRow->setData(nameColumn, itemKindRole,
                    static_cast<int>(OutlinerRowKind::SceneGroup));
                groupRow->setData(nameColumn, itemPathRole, groupPath);
                groupRow->setToolTip(
                    nameColumn,
                    QStringLiteral("Blender data blocks with no users"));
                groupRow->setFlags(Qt::ItemIsEnabled);
                sceneRoots.append(groupRow);
                dataParent = groupRow;
            }
            for (qsizetype dataIndex = 0;
                 dataIndex < sourceGroup.dataBlockNames.size(); ++dataIndex) {
                auto *dataRow = dataParent != nullptr
                    ? new QTreeWidgetItem(dataParent)
                    : new QTreeWidgetItem(tree_);
                dataRow->setText(nameColumn,
                                 sourceGroup.dataBlockNames[dataIndex]);
                dataRow->setData(nameColumn, itemKindRole,
                    static_cast<int>(OutlinerRowKind::ObjectContent));
                dataRow->setData(
                    nameColumn, itemPathRole,
                    dataPath + QStringLiteral("/item-%1").arg(dataIndex));
                dataRow->setToolTip(
                    nameColumn,
                    QStringLiteral("Blender orphan data block (zero users)"));
                dataRow->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            }
            if (sourceGroup.truncated) {
                auto *truncatedRow = dataParent != nullptr
                    ? new QTreeWidgetItem(dataParent)
                    : new QTreeWidgetItem(tree_);
                truncatedRow->setText(
                    nameColumn,
                    QStringLiteral("Additional orphan data blocks omitted"));
                truncatedRow->setData(nameColumn, itemKindRole,
                    static_cast<int>(OutlinerRowKind::ObjectContent));
                truncatedRow->setData(
                    nameColumn, itemPathRole,
                    dataPath + QStringLiteral("/truncated"));
                truncatedRow->setToolTip(
                    nameColumn,
                    QStringLiteral("The 20,000-row extraction limit was reached"));
                truncatedRow->setFlags(Qt::ItemIsEnabled);
            }
        }
    } else if (sequencerMode) {
        const auto appendSequenceRows = [this, &sceneRoots, &sequence](
            const auto &self,
            const QVector<OutlinerSequenceStrip> &sourceRows,
            QTreeWidgetItem *parent,
            const QString &parentPath) -> void {
            for (qsizetype index = 0; index < sourceRows.size(); ++index) {
                const OutlinerSequenceStrip &source = sourceRows[index];
                const bool duplicateGroup =
                    source.kind == QLatin1String("DUPLICATE");
                auto *row = parent != nullptr
                    ? new QTreeWidgetItem(parent)
                    : new QTreeWidgetItem(tree_);
                row->setText(nameColumn, source.name);
                row->setData(nameColumn, itemKindRole,
                    static_cast<int>(duplicateGroup
                        ? OutlinerRowKind::SceneGroup
                        : OutlinerRowKind::SequencerStrip));
                const QString rowPath = parentPath +
                    QStringLiteral("/%1-%2")
                        .arg(duplicateGroup ? QStringLiteral("duplicate")
                                            : QStringLiteral("strip"))
                        .arg(index);
                row->setData(nameColumn, itemPathRole, rowPath);
                QString description = duplicateGroup
                    ? QStringLiteral("Blender strips using the same media file")
                    : QStringLiteral("Blender %1 strip").arg(source.type);
                if (!sequence.sceneName.isEmpty()) {
                    description += QStringLiteral(" in %1")
                                       .arg(sequence.sceneName);
                }
                if (!sequence.workspaceName.isEmpty()) {
                    description += QStringLiteral(" (%1 workspace)")
                                       .arg(sequence.workspaceName);
                }
                row->setToolTip(nameColumn, description);
                row->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
                row->setSelected(!duplicateGroup && source.selected);
                if (!source.sourceId.isEmpty()) {
                    row->setData(nameColumn, itemSequencerStripIdRole,
                                 source.sourceId);
                    row->setData(nameColumn, itemSequencerStripActiveRole,
                                 source.active);
                    QFont font = row->font(nameColumn);
                    font.setBold(source.active);
                    row->setFont(nameColumn, font);
                }
                if (parent == nullptr) sceneRoots.append(row);

                if (duplicateGroup || source.isMeta) {
                    self(self, source.children, row, rowPath);
                } else if (!source.isEffect) {
                    auto *dataRow = new QTreeWidgetItem(row);
                    dataRow->setText(nameColumn, source.dataDirectory);
                    dataRow->setData(nameColumn, itemKindRole,
                        static_cast<int>(OutlinerRowKind::ObjectContent));
                    dataRow->setData(nameColumn, itemPathRole,
                                     rowPath + QStringLiteral("/data"));
                    dataRow->setToolTip(
                        nameColumn,
                        QStringLiteral("Blender strip media directory"));
                    dataRow->setFlags(Qt::ItemIsEnabled);
                }
            }
        };
        appendSequenceRows(appendSequenceRows, sequence.strips, nullptr,
                           QStringLiteral("sequence"));
        if (sequence.truncated) {
            auto *truncatedRow = new QTreeWidgetItem(tree_);
            truncatedRow->setText(
                nameColumn,
                QStringLiteral("Additional sequencer strips omitted"));
            truncatedRow->setData(nameColumn, itemKindRole,
                static_cast<int>(OutlinerRowKind::ObjectContent));
            truncatedRow->setData(nameColumn, itemPathRole,
                                  QStringLiteral("sequence/truncated"));
            truncatedRow->setToolTip(
                nameColumn,
                QStringLiteral("The 10,000-strip extraction limit was reached"));
            truncatedRow->setFlags(Qt::ItemIsEnabled);
        }
    } else if (scenesMode) {
        const auto appendSceneContentRow = [this](
            QTreeWidgetItem *parent, const QString &name,
            const QString &path, const QString &viewLayerKey,
            const QString &description) -> QTreeWidgetItem * {
            if (parent == nullptr || name.isEmpty()) return nullptr;
            auto *row = new QTreeWidgetItem(parent);
            row->setText(nameColumn, name);
            row->setData(nameColumn, itemKindRole,
                         static_cast<int>(OutlinerRowKind::ObjectContent));
            row->setData(nameColumn, itemViewLayerKeyRole, viewLayerKey);
            row->setData(nameColumn, itemPathRole, path);
            row->setToolTip(nameColumn, description);
            row->setFlags(Qt::ItemIsEnabled);
            return row;
        };
        const auto appendAnimationData = [&appendSceneContentRow](
            QTreeWidgetItem *parent, const OutlinerAnimationEntry &animation,
            const QString &path, const QString &viewLayerKey,
            const QString &description) {
            if (!animation.hasAnimationData) return;
            auto *animationRow = appendSceneContentRow(
                parent, QStringLiteral("Animation"), path, viewLayerKey,
                description);
            auto *actionRow = appendSceneContentRow(
                animationRow, animation.actionName,
                path + QStringLiteral("/action"), viewLayerKey,
                QStringLiteral("Blender active action data block"));
            appendSceneContentRow(
                actionRow, animation.actionSlotName,
                path + QStringLiteral("/action/slot"), viewLayerKey,
                QStringLiteral("Blender assigned action slot"));
            if (animation.hasDrivers) {
                auto *driversRow = appendSceneContentRow(
                    animationRow, QStringLiteral("Drivers"),
                    path + QStringLiteral("/drivers"), viewLayerKey,
                    QStringLiteral("Blender animation drivers"));
                for (qsizetype index = 0;
                     index < animation.driverTargetNames.size(); ++index) {
                    appendSceneContentRow(
                        driversRow, animation.driverTargetNames[index],
                        path + QStringLiteral("/driver-target-%1").arg(index),
                        viewLayerKey,
                        QStringLiteral("Blender driver target data block"));
                }
            }
            if (!animation.nlaTracks.isEmpty()) {
                auto *tracksRow = appendSceneContentRow(
                    animationRow, QStringLiteral("NLA Tracks"),
                    path + QStringLiteral("/nla-tracks"), viewLayerKey,
                    QStringLiteral("Blender non-linear animation tracks"));
                for (qsizetype trackIndex = 0;
                     trackIndex < animation.nlaTracks.size(); ++trackIndex) {
                    const OutlinerNlaTrackEntry &sourceTrack =
                        animation.nlaTracks[trackIndex];
                    auto *trackRow = appendSceneContentRow(
                        tracksRow, sourceTrack.name,
                        path + QStringLiteral("/nla-track-%1").arg(trackIndex),
                        viewLayerKey, QStringLiteral("Blender NLA track"));
                    for (qsizetype actionIndex = 0;
                         actionIndex < sourceTrack.actionNames.size();
                         ++actionIndex) {
                        appendSceneContentRow(
                            trackRow, sourceTrack.actionNames[actionIndex],
                            path + QStringLiteral("/nla-track-%1/action-%2")
                                .arg(trackIndex).arg(actionIndex),
                            viewLayerKey,
                            QStringLiteral("Blender NLA action data block"));
                    }
                }
            }
        };
        for (const OutlinerSceneEntry &scene : scenes) {
            const QVector<CollectionPanelEntry> sceneEntries =
                entriesForScene(scene);
            const QVector<OutlinerObjectEntry> sceneObjects =
                objectsForScene(scene);
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
            sceneRoot->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable |
                                Qt::ItemIsDropEnabled);
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

            if (!scene.worldName.isEmpty()) {
                QTreeWidgetItem *worldRow = appendSceneContentRow(
                    sceneRoot, scene.worldName,
                    rootPath + QStringLiteral("/world"),
                    scene.activeViewLayerKey,
                    QStringLiteral("Blender scene World data block"));
                appendAnimationData(
                    worldRow, scene.worldAnimation,
                    rootPath + QStringLiteral("/world/animation"),
                    scene.activeViewLayerKey,
                    QStringLiteral("Blender World animation data"));
            }

            auto *collectionsGroup = new QTreeWidgetItem(sceneRoot);
            collectionsGroup->setText(nameColumn,
                                      QStringLiteral("Collections"));
            collectionsGroup->setData(nameColumn, itemKindRole,
                static_cast<int>(OutlinerRowKind::SceneGroup));
            collectionsGroup->setData(nameColumn, itemPathRole,
                rootPath + QStringLiteral("/collections"));
            collectionsGroup->setFlags(Qt::ItemIsEnabled);
            for (const int rootIndex :
                 topLevelCollectionIndices(sceneEntries)) {
                appendCollection(
                    tree_, sceneEntries[rootIndex], sceneEntries,
                    sceneObjects, collectionsGroup, {},
                    {rootPath, QStringLiteral("collections")}, {},
                    {}, scene.activeViewLayerKey);
            }

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
            appendAnimationData(
                sceneRoot, scene.animationData,
                rootPath + QStringLiteral("/animation"),
                scene.activeViewLayerKey,
                QStringLiteral("Blender scene animation data"));
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
        sceneRoot->setIcon(nameColumn, collectionIcon(QStringLiteral("NONE")));
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
    if (showObjectChildren_ && !librariesMode && !orphanDataMode &&
        !sequencerMode && !dataApiMode && !libraryOverridesMode) {
        buildObjectParentHierarchy(displayObjects);
    }
    if (sortAlphabetically_) {
        tree_->sortItems(nameColumn, Qt::AscendingOrder);
    }
    if (!hasRefreshed_ || stateScopeChanged || treeStateContextChanged) {
        if (!dataApiMode) tree_->expandAll();
        for (const QVector<QTreeWidgetItem *> &rows : objectRows_) {
            for (QTreeWidgetItem *row : rows) {
                if (row != nullptr && row->childCount() > 0) {
                    row->setExpanded(false);
                }
            }
        }
        if (librariesMode) {
            QVector<QTreeWidgetItem *> pending;
            for (int index = 0; index < tree_->topLevelItemCount(); ++index) {
                pending.append(tree_->topLevelItem(index));
            }
            while (!pending.isEmpty()) {
                QTreeWidgetItem *item = pending.takeLast();
                if (item->data(nameColumn, itemKindRole).toInt() ==
                    static_cast<int>(OutlinerRowKind::Library)) {
                    item->setExpanded(item->data(
                        nameColumn, itemLibraryCurrentFileRole).toBool());
                }
                for (int index = 0; index < item->childCount(); ++index) {
                    pending.append(item->child(index));
                }
            }
        } else if (sequencerMode) {
            QVector<QTreeWidgetItem *> pending;
            for (int index = 0; index < tree_->topLevelItemCount(); ++index) {
                pending.append(tree_->topLevelItem(index));
            }
            while (!pending.isEmpty()) {
                QTreeWidgetItem *item = pending.takeLast();
                const int kind = item->data(nameColumn, itemKindRole).toInt();
                if (kind == static_cast<int>(OutlinerRowKind::SequencerStrip) ||
                    kind == static_cast<int>(OutlinerRowKind::SceneGroup)) {
                    item->setExpanded(false);
                }
                for (int index = 0; index < item->childCount(); ++index) {
                    pending.append(item->child(index));
                }
            }
        } else if (dataApiMode) {
            for (int index = 0; index < tree_->topLevelItemCount(); ++index) {
                tree_->topLevelItem(index)->setExpanded(true);
            }
        } else if (libraryOverridesMode) {
            QVector<QTreeWidgetItem *> pending;
            for (int index = 0; index < tree_->topLevelItemCount(); ++index) {
                pending.append(tree_->topLevelItem(index));
            }
            while (!pending.isEmpty()) {
                QTreeWidgetItem *item = pending.takeLast();
                const int kind = item->data(nameColumn, itemKindRole).toInt();
                if (kind == static_cast<int>(OutlinerRowKind::Library)) {
                    item->setExpanded(
                        overrideHierarchiesMode && item->data(
                            nameColumn, itemLibraryCurrentFileRole).toBool());
                } else if (kind == static_cast<int>(OutlinerRowKind::SceneGroup)) {
                    item->setExpanded(overridePropertiesMode);
                } else {
                    item->setExpanded(false);
                }
                for (int index = 0; index < item->childCount(); ++index) {
                    pending.append(item->child(index));
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
            } else if (dataApiMode) {
                const bool pointerStruct = isDataApiPointerStruct(item);
                const bool explicitlyCollapsed = pointerStruct &&
                    dataApiCollapsedPaths_.contains(path);
                item->setExpanded(!explicitlyCollapsed &&
                    (path == QStringLiteral("data-api/main") ||
                     dataApiExpandedPaths_.contains(path) ||
                     pointerStruct));
            } else if (libraryOverridesMode) {
                const int kind = item->data(nameColumn, itemKindRole).toInt();
                if (kind == static_cast<int>(OutlinerRowKind::Library)) {
                    item->setExpanded(overrideHierarchiesMode && item->data(
                        nameColumn, itemLibraryCurrentFileRole).toBool());
                } else if (kind == static_cast<int>(OutlinerRowKind::SceneGroup)) {
                    item->setExpanded(overridePropertiesMode);
                } else {
                    item->setExpanded(false);
                }
            } else if (sequencerMode &&
                       (item->data(nameColumn, itemKindRole).toInt() ==
                            static_cast<int>(OutlinerRowKind::SequencerStrip) ||
                        item->data(nameColumn, itemKindRole).toInt() ==
                            static_cast<int>(OutlinerRowKind::SceneGroup))) {
                item->setExpanded(false);
            } else if (item->data(nameColumn, itemKindRole).toInt() ==
                           static_cast<int>(OutlinerRowKind::Collection) ||
                       item->data(nameColumn, itemKindRole).toInt() ==
                           static_cast<int>(OutlinerRowKind::SceneGroup)) {
                item->setExpanded(true);
            } else if (librariesMode &&
                       item->data(nameColumn, itemKindRole).toInt() ==
                           static_cast<int>(OutlinerRowKind::Library) &&
                       item->data(nameColumn,
                                  itemLibraryCurrentFileRole).toBool()) {
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
    if (!syncSelectionEnabled_ &&
        (!outlinerSelectedPaths.isEmpty() || !outlinerCurrentPath.isEmpty())) {
        QVector<QTreeWidgetItem *> pending;
        for (int index = 0; index < tree_->topLevelItemCount(); ++index) {
            pending.append(tree_->topLevelItem(index));
        }
        while (!pending.isEmpty()) {
            QTreeWidgetItem *item = pending.takeLast();
            const QString path = item->data(nameColumn, itemPathRole).toString();
            if (!path.isEmpty()) {
                item->setSelected(outlinerSelectedPaths.contains(path));
                if (path == outlinerCurrentPath) {
                    tree_->setCurrentItem(item, nameColumn,
                                          QItemSelectionModel::NoUpdate);
                }
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
    const ObjectId activeObjectId = callbacks_.activeObjectId
        ? callbacks_.activeObjectId() : ObjectId::invalid();
    const int activeInteractionMode = callbacks_.interactionMode
        ? callbacks_.interactionMode() : 0;
    for (auto rows = objectRows_.cbegin(); rows != objectRows_.cend(); ++rows) {
        const bool editMode = activeInteractionMode == 1 &&
                              activeObjectId.isValid() &&
                              rows.key() == activeObjectId.value();
        for (QTreeWidgetItem *item : rows.value()) {
            if (item == nullptr) continue;
            item->setData(modeColumn, itemModeValueRole,
                          editMode ? 1 : 0);
            item->setIcon(modeColumn, restrictionIcon(
                editMode ? RestrictionGlyph::EditMode
                         : RestrictionGlyph::ObjectMode,
                true));
            item->setToolTip(
                modeColumn,
                editMode
                    ? QStringLiteral("Edit Mode; click to switch to Object Mode")
                    : QStringLiteral("Object Mode; click to switch to Edit Mode"));
        }
    }
    if (!syncSelectionEnabled_) return;
    if (sequencerDisplayMode_) {
        QStringList selectedStripIds;
        QString activeStripId;
        QTreeWidgetItem *currentItem = tree_->currentItem();
        if (currentItem != nullptr &&
            currentItem->data(nameColumn, itemKindRole).toInt() !=
                static_cast<int>(OutlinerRowKind::SequencerStrip)) {
            return;
        }
        QVector<QTreeWidgetItem *> pending;
        for (int index = 0; index < tree_->topLevelItemCount(); ++index) {
            pending.append(tree_->topLevelItem(index));
        }
        while (!pending.isEmpty()) {
            QTreeWidgetItem *item = pending.takeLast();
            if (item->data(nameColumn, itemKindRole).toInt() ==
                static_cast<int>(OutlinerRowKind::SequencerStrip)) {
                const QString stripId = item->data(
                    nameColumn, itemSequencerStripIdRole).toString();
                if (!stripId.isEmpty()) {
                    if (item->isSelected()) selectedStripIds.append(stripId);
                    if (item == currentItem ||
                        (activeStripId.isEmpty() && item->data(
                            nameColumn, itemSequencerStripActiveRole).toBool())) {
                        activeStripId = stripId;
                    }
                }
            }
            for (int childIndex = 0; childIndex < item->childCount();
                 ++childIndex) {
                pending.append(item->child(childIndex));
            }
        }
        if (callbacks_.setSequencerSelection) {
            callbacks_.setSequencerSelection(selectedStripIds, activeStripId);
        }
        return;
    }
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
