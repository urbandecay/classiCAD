/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "core/document/document_collection.h"
#include "core/document/object_id.h"
#include "core/document/scene_object.h"

#include <QFrame>
#include <QHash>
#include <QJsonArray>
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
class QCheckBox;

namespace classiCAD {

struct CollectionPanelEntry {
    DocumentCollection collection;
};

struct OutlinerNlaTrackEntry {
    QString name;
    QStringList actionNames;
};

struct OutlinerAnimationEntry {
    bool hasAnimationData = false;
    QString actionName;
    QString actionSlotName;
    bool hasDrivers = false;
    QStringList driverTargetNames;
    QVector<OutlinerNlaTrackEntry> nlaTracks;
};

struct OutlinerMaterialEntry {
    QString name;
    // Stable imported material identity. Empty for display-only material rows.
    QString materialId;
    OutlinerAnimationEntry animation;
};

struct OutlinerModifierEntry {
    QString name;
    QString linkedName;
    QString linkedType;
};

struct OutlinerObjectEntry {
    ObjectId id = ObjectId::invalid();
    // Source-only Blender object rows have no editable CAD scene object.
    bool outlinerOnly = false;
    QString sourceObjectId;
    QString hierarchyIdentity;
    QString parentHierarchyIdentity;
    QString outlinerDescription;
    QString instancedCollectionName;
    bool hasPose = false;
    bool showPoseChannels = false;
    QJsonArray poseBoneTree;
    QJsonArray armatureBoneTree;
    QJsonArray boneCollectionTree;
    QJsonArray greasePencilLayerTree;
    QString name;
    QString blenderType;
    QString dataBlockName;
    QString shapeKeyDataName;
    QStringList shapeKeyNames;
    QStringList dataMaterialNames;
    QVector<OutlinerMaterialEntry> dataMaterialEntries;
    bool dataHasAnimationData = false;
    QString dataAnimationActionName;
    QString dataAnimationActionSlotName;
    bool dataHasDrivers = false;
    QStringList dataDriverTargetNames;
    QVector<OutlinerNlaTrackEntry> dataNlaTracks;
    QStringList materialNames;
    QVector<OutlinerMaterialEntry> materialEntries;
    QStringList modifierNames;
    QVector<OutlinerModifierEntry> modifiers;
    QVector<OutlinerModifierEntry> shaderEffects;
    QStringList constraintNames;
    QStringList vertexGroupNames;
    bool hasAnimationData = false;
    QString animationActionName;
    QString animationActionSlotName;
    bool hasDrivers = false;
    QStringList driverTargetNames;
    QVector<OutlinerNlaTrackEntry> nlaTracks;
    QVector<CollectionId> collectionIds;
    bool hideViewport = false;
    bool hideRender = false;
    bool hideSelect = false;
    bool visible = true;
    bool selectable = true;
    QJsonObject viewLayerHidden;
    QStringList sceneKeys;
};

enum class OutlinerRowKind {
    Collection,
    Library,
    SequencerStrip,
    DataApi,
    Object,
    SceneRoot,
    SceneGroup,
    ObjectContent,
    SourceObject,
    Material,
    DataStackEntry,
    DataStackGroup
};
enum class OutlinerDropPosition {
    Into,
    Before,
    After,
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
    QString sceneKey;
    QStringList sceneMembershipKeys;
    QString materialId;
    BlenderSourceDataStackKind dataStackKind =
        BlenderSourceDataStackKind::Modifier;
    qsizetype dataStackIndex = -1;
    bool syntheticParentRow = false;
};

struct OutlinerCollectionContext {
    CollectionId collectionId = CollectionId::invalid();
    CollectionId parentCollectionId = CollectionId::invalid();
    QString viewLayerKey;
    QStringList sourcePath;
};

struct OutlinerViewLayerEntry {
    QString key;
    QString displayName;
};

struct OutlinerSceneEntry {
    QString key;
    QString displayName;
    QString worldName;
    OutlinerAnimationEntry worldAnimation;
    OutlinerAnimationEntry animationData;
    QVector<OutlinerViewLayerEntry> viewLayers;
    QString activeViewLayerKey;
};

struct OutlinerActionSlotEntry {
    QString displayName;
    QString identifier;
};

struct OutlinerLibraryDataGroup {
    QString displayName;
    QStringList dataBlockNames;
    QHash<QString, OutlinerAnimationEntry> animationsByDataBlockName;
    QHash<QString, QVector<OutlinerActionSlotEntry>> actionSlotsByDataBlockName;
    struct DataBlockTree {
        QString shapeKeyDataName;
        QStringList shapeKeyNames;
        QStringList materialNames;
        QJsonArray armatureBoneTree;
        QJsonArray boneCollectionTree;
        QJsonArray greasePencilLayerTree;
        QJsonArray collectionTree;
        QJsonArray objectTree;
        QJsonObject sceneTree;
        QString textureImageName;
        QStringList lineStyleTextureNames;
    };
    QHash<QString, DataBlockTree> dataBlockTreesByName;
};

struct OutlinerLibraryEntry {
    QString id;
    QString displayName;
    QString parentId;
    bool currentFile = false;
    bool truncated = false;
    QVector<OutlinerLibraryDataGroup> dataGroups;
};

struct OutlinerOrphanGroup {
    QString displayName;
    QStringList dataBlockNames;
    bool truncated = false;
};

struct OutlinerSequenceStrip {
    QString sourceId;
    QString kind;
    QString name;
    QString type;
    QString mediaFilename;
    QString dataDirectory;
    bool isMeta = false;
    bool isEffect = false;
    bool selected = false;
    bool active = false;
    QVector<OutlinerSequenceStrip> children;
};

struct OutlinerSequenceTree {
    QString workspaceName;
    QString sceneName;
    bool truncated = false;
    QVector<OutlinerSequenceStrip> strips;
};

struct CollectionsPanelCallbacks {
    std::function<QVector<CollectionPanelEntry>()> entries;
    std::function<QVector<OutlinerObjectEntry>()> objects;
    std::function<QVector<OutlinerSceneEntry>()> scenes;
    std::function<QVector<OutlinerLibraryEntry>()> libraries;
    std::function<QVector<OutlinerOrphanGroup>()> orphanGroups;
    std::function<OutlinerSequenceTree()> sequencer;
    std::function<void(const QStringList &, const QString &)> setSequencerSelection;
    std::function<QJsonObject()> dataApi;
    std::function<QJsonObject()> libraryOverrides;
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
    std::function<void(ObjectId)> selectObjectHierarchy;
    std::function<void()> deselectSelectedObjects;
    std::function<bool()> hasOutlinerObjectClipboard;
    std::function<void(ObjectId)> copyOutlinerObject;
    std::function<void()> pasteOutlinerObjects;
    std::function<bool()> hasOutlinerCollectionClipboard;
    std::function<void(const QVector<CollectionId> &)> copyOutlinerCollection;
    std::function<void(CollectionId)> pasteOutlinerCollection;
    std::function<void(ObjectId, bool)> deleteObjectSelection;
    std::function<void(const QVector<CollectionId> &, bool)>
        setCollectionsObjectSelection;
    std::function<CollectionId(const QString &, CollectionId)> createCollection;
    std::function<void(const QVector<CollectionId> &, CollectionId, bool, bool)>
        duplicateCollection;
    std::function<void(const QVector<CollectionId> &)>
        linkCollectionsToActiveScene;
    std::function<void(const QVector<CollectionId> &)>
        instanceCollectionsToScene;
    std::function<void(const QVector<OutlinerCollectionContext> &)>
        unlinkCollections;
    std::function<void(CollectionId, const QString &)> renameCollection;
    std::function<void(const QVector<CollectionId> &)> removeCollection;
    std::function<void(const QVector<ObjectId> &, CollectionId)> linkObjects;
    std::function<void(ObjectId, const QString &)> renameObject;
    std::function<void(ObjectId, CollectionId)> unlinkObject;
    std::function<void(const QVector<CollectionId> &)>
        removeCollectionHierarchy;
    std::function<void(CollectionId, bool, bool, bool)> setViewportVisibility;
    std::function<void(const QVector<OutlinerCollectionContext> &, bool)>
        setCollectionsViewportEnabled;
    std::function<void(CollectionId, bool, bool, bool)>
        setCollectionRenderVisibility;
    std::function<void(const QVector<OutlinerCollectionContext> &, bool)>
        setCollectionsRenderVisibility;
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
    std::function<void(const QVector<OutlinerCollectionContext> &,
                       const QString &,
                       bool,
                       bool,
                       bool)> setCollectionsViewLayerState;
    std::function<void(const QVector<OutlinerRowReference> &,
                       const OutlinerRowReference &,
                       bool,
                       bool,
                       OutlinerDropPosition)> dropRows;
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
    void populateDataApiChildren(QTreeWidgetItem *item);
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
                                  const QString &activeViewLayerKey,
                                  bool includeContents = true);
    void buildObjectParentHierarchy(
        const QVector<OutlinerObjectEntry> &objects);

    CollectionsPanelCallbacks callbacks_;
    QTreeWidget *tree_ = nullptr;
    QLineEdit *filterEdit_ = nullptr;
    QToolButton *filterButton_ = nullptr;
    QComboBox *displayModeCombo_ = nullptr;
    QComboBox *libraryOverrideViewCombo_ = nullptr;
    QComboBox *idTypeCombo_ = nullptr;
    QCheckBox *showSystemOverridesCheck_ = nullptr;
    QComboBox *sceneCombo_ = nullptr;
    QComboBox *viewLayerCombo_ = nullptr;
    QTreeWidgetItem *rangeAnchorItem_ = nullptr;
    QHash<quint64, QVector<QTreeWidgetItem *>> objectRows_;
    QHash<quint64, OutlinerObjectEntry> objectEntries_;
    QHash<QString, QVector<QTreeWidgetItem *>> hierarchyRows_;
    QHash<QString, OutlinerObjectEntry> hierarchyEntries_;
    QHash<QString, OutlinerObjectEntry> sourceOnlyEntries_;
    QHash<QString, bool> expandedBeforeFilter_;
    QSet<QString> dataApiExpandedPaths_;
    QSet<QString> dataApiCollapsedPaths_;
    QString activeFilterText_;
    QString idTypeFilter_;
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
    bool librariesDisplayMode_ = false;
    bool orphanDataDisplayMode_ = false;
    bool sequencerDisplayMode_ = false;
    bool dataApiDisplayMode_ = false;
    bool libraryOverridesDisplayMode_ = false;
    bool overrideHierarchiesView_ = false;
    bool showSystemOverrides_ = false;
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
