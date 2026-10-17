/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "core/document/document_collection.h"
#include "core/document/object_id.h"

#include <QFrame>
#include <QHash>
#include <QJsonObject>
#include <QPoint>
#include <QSet>
#include <QStringList>
#include <QVector>

#include <functional>

class QTreeWidget;
class QTreeWidgetItem;
class QLineEdit;
class QComboBox;
class QToolButton;

namespace classiCAD {

struct CollectionPanelEntry {
    DocumentCollection collection;
};

struct OutlinerObjectEntry {
    ObjectId id = ObjectId::invalid();
    QString name;
    QString blenderType;
    QString dataBlockName;
    QStringList materialNames;
    QStringList modifierNames;
    QStringList constraintNames;
    QStringList vertexGroupNames;
    QVector<CollectionId> collectionIds;
    bool hideViewport = false;
    bool hideRender = false;
    bool hideSelect = false;
    bool visible = true;
    bool selectable = true;
    QJsonObject viewLayerHidden;
    ObjectId parentObjectId = ObjectId::invalid();
    QStringList sceneKeys;
};

enum class OutlinerRowKind {
    Collection,
    Object,
    SceneRoot,
    SceneGroup,
    ObjectContent
};
enum class OutlinerObjectStateFilter {
    All,
    Visible,
    Hidden,
    Selected,
    Active,
    Selectable,
};

struct OutlinerRowReference {
    OutlinerRowKind kind = OutlinerRowKind::SceneRoot;
    ObjectId objectId = ObjectId::invalid();
    CollectionId collectionId = CollectionId::invalid();
    CollectionId linkedCollectionId = CollectionId::invalid();
    CollectionId parentCollectionId = CollectionId::invalid();
    bool syntheticParentRow = false;
};

struct OutlinerViewLayerEntry {
    QString key;
    QString displayName;
};

struct OutlinerSceneEntry {
    QString key;
    QString displayName;
    QVector<OutlinerViewLayerEntry> viewLayers;
    QString activeViewLayerKey;
};

struct CollectionsPanelCallbacks {
    std::function<QVector<CollectionPanelEntry>()> entries;
    std::function<QVector<OutlinerObjectEntry>()> objects;
    std::function<QVector<OutlinerSceneEntry>()> scenes;
    std::function<QString()> activeSceneKey;
    std::function<void(const QString &)> setActiveScene;
    std::function<QVector<OutlinerViewLayerEntry>()> viewLayers;
    std::function<QString()> activeViewLayerKey;
    std::function<void(const QString &)> setActiveViewLayer;
    std::function<QVector<ObjectId>()> selectedObjectIds;
    std::function<ObjectId()> activeObjectId;
    std::function<bool(ObjectId)> canSelectObject;
    std::function<bool(ObjectId, const QString &)> objectVisibleInViewLayer;
    std::function<bool(ObjectId, const QString &)> objectSelectableInViewLayer;
    std::function<void(const QVector<ObjectId> &, ObjectId)> setObjectSelection;
    std::function<CollectionId(const QString &, CollectionId)> createCollection;
    std::function<void(CollectionId, const QString &)> renameCollection;
    std::function<void(CollectionId)> removeCollection;
    std::function<void(const QVector<ObjectId> &, CollectionId)> linkObjects;
    std::function<void(ObjectId, const QString &)> renameObject;
    std::function<void(ObjectId, CollectionId)> unlinkObject;
    std::function<void(CollectionId, bool, bool, bool)> setViewportVisibility;
    std::function<void(CollectionId, bool, bool, bool)>
        setCollectionRenderVisibility;
    std::function<void(CollectionId, bool, bool, bool)>
        setCollectionSelectability;
    std::function<void(ObjectId, bool, bool)> setObjectViewportVisibility;
    std::function<void(ObjectId, bool, bool)> setObjectRenderVisibility;
    std::function<void(ObjectId, bool, bool)> setObjectSelectability;
    std::function<void(ObjectId, const QString &, bool, bool)>
        setObjectViewLayerHidden;
    std::function<void(CollectionId,
                       const QString &,
                       const QStringList &,
                       const QString &,
                       bool,
                       bool,
                       bool)> setCollectionViewLayerState;
    std::function<void(const QVector<OutlinerRowReference> &,
                       const OutlinerRowReference &,
                       bool,
                       bool)> dropRows;
    std::function<QString()> stateScopeKey;
    std::function<QString()> treeStateContextKey;
    std::function<bool(QStringList *)> loadExpandedTreePaths;
    std::function<void(const QStringList &)> saveExpandedTreePaths;
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
    void createLayout();
    void applyNameFilter(const QString &text);
    void restoreTreeState();
    void saveTreeState();
    void showContextMenu(const QPoint &position);
    bool activateObjectRow(QTreeWidgetItem *item,
                           Qt::KeyboardModifiers modifiers);
    void appendCollection(QTreeWidget *tree,
                         const CollectionPanelEntry &entry,
                         const QVector<CollectionPanelEntry> &entries,
                         const QVector<OutlinerObjectEntry> &objects,
                         QTreeWidgetItem *parent,
                         QSet<quint64> ancestors,
                         QStringList path,
                         QStringList sourcePath,
                         const QString &activeViewLayerKey,
                         const QString &rowViewLayerKey);
    QTreeWidgetItem *appendObject(QTreeWidgetItem *parent,
                                  const OutlinerObjectEntry &entry,
                                  const QStringList &path,
                                  CollectionId linkedCollectionId,
                                  const QString &activeViewLayerKey);
    void buildObjectParentHierarchy(
        const QVector<OutlinerObjectEntry> &objects);

    CollectionsPanelCallbacks callbacks_;
    QTreeWidget *tree_ = nullptr;
    QLineEdit *filterEdit_ = nullptr;
    QToolButton *filterButton_ = nullptr;
    QComboBox *displayModeCombo_ = nullptr;
    QComboBox *sceneCombo_ = nullptr;
    QComboBox *viewLayerCombo_ = nullptr;
    QTreeWidgetItem *rangeAnchorItem_ = nullptr;
    QHash<quint64, QVector<QTreeWidgetItem *>> objectRows_;
    QHash<quint64, OutlinerObjectEntry> objectEntries_;
    QHash<QString, bool> expandedBeforeFilter_;
    QString activeFilterText_;
    QHash<QString, bool> objectTypeFilters_;
    QString currentStateScopeKey_;
    QString currentTreeStateContextKey_;
    OutlinerObjectStateFilter objectStateFilter_ =
        OutlinerObjectStateFilter::All;
    bool showCollections_ = true;
    bool showObjects_ = true;
    bool showObjectContents_ = true;
    bool showObjectChildren_ = true;
    bool showAllViewLayers_ = false;
    bool scrollToActive_ = true;
    bool scenesDisplayMode_ = false;
    bool invertObjectStateFilter_ = false;
    bool exactNameMatch_ = false;
    bool caseSensitiveNameFilter_ = false;
    bool showEnableColumn_ = true;
    bool showSelectColumn_ = false;
    bool showHideColumn_ = true;
    bool showViewportColumn_ = false;
    bool showRenderColumn_ = true;
    bool showHoldoutColumn_ = false;
    bool showIndirectOnlyColumn_ = false;
    bool refreshing_ = false;
    bool restoringTreeState_ = false;
    bool hasRefreshed_ = false;
};

} // namespace classiCAD
