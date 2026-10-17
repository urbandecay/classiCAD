#include "document_transaction.h"

#include <algorithm>

#include <cmath>
#include <utility>

#include <QSet>

namespace classiCAD {

namespace {

template <typename List>
bool moveListEntry(List *entries, qsizetype fromIndex, qsizetype toIndex)
{
    if (entries == nullptr || fromIndex < 0 || toIndex < 0 ||
        fromIndex >= entries->size() || toIndex >= entries->size() ||
        fromIndex == toIndex) {
        return false;
    }
    auto entry = entries->takeAt(fromIndex);
    entries->insert(toIndex, std::move(entry));
    return true;
}

QString uniqueStackName(const QString &sourceName, QSet<QString> *usedNames)
{
    const QString base = sourceName.trimmed().isEmpty()
        ? QStringLiteral("Data") : sourceName.trimmed();
    if (usedNames == nullptr || !usedNames->contains(base)) {
        if (usedNames != nullptr) usedNames->insert(base);
        return base;
    }

    QString stem = base;
    int nextSuffix = 1;
    const qsizetype dot = base.lastIndexOf(QLatin1Char('.'));
    if (dot >= 0) {
        bool parsed = false;
        const int existingSuffix = base.mid(dot + 1).toInt(&parsed);
        if (parsed && existingSuffix >= 0) {
            stem = base.left(dot);
            nextSuffix = existingSuffix + 1;
        }
    }
    for (int suffix = nextSuffix;; ++suffix) {
        const QString candidate = QStringLiteral("%1.%2")
            .arg(stem)
            .arg(suffix, 3, 10, QLatin1Char('0'));
        if (usedNames->contains(candidate)) continue;
        usedNames->insert(candidate);
        return candidate;
    }
}

bool sameModifierStack(const QVector<BlenderSourceModifier> &left,
                       const QVector<BlenderSourceModifier> &right)
{
    if (left.size() != right.size()) return false;
    for (qsizetype index = 0; index < left.size(); ++index) {
        if (left[index].name != right[index].name ||
            left[index].linkedName != right[index].linkedName ||
            left[index].linkedType != right[index].linkedType) {
            return false;
        }
    }
    return true;
}

bool sameShaderEffectStack(const QVector<BlenderSourceShaderEffect> &left,
                           const QVector<BlenderSourceShaderEffect> &right)
{
    if (left.size() != right.size()) return false;
    for (qsizetype index = 0; index < left.size(); ++index) {
        if (left[index].name != right[index].name ||
            left[index].linkedName != right[index].linkedName ||
            left[index].linkedType != right[index].linkedType) {
            return false;
        }
    }
    return true;
}

} // namespace

DocumentTransaction::DocumentTransaction(Document &document, History &history)
    : document_(document),
      history_(history),
      before_(document.snapshot())
{
}

DocumentTransaction::~DocumentTransaction()
{
    if (!finished_ && !changes_.isEmpty()) {
        rollback();
    }
}

Shape *DocumentTransaction::editGeometry(ObjectId objectId)
{
    Shape *shape = document_.mutableShape(objectId);
    if (shape == nullptr) {
        return nullptr;
    }
    changes_.addObject(objectId);
    changes_.geometryChanged = true;
    return shape;
}

ObjectId DocumentTransaction::addShape(const Shape &shape)
{
    SceneObject object;
    object.geometry = shape;
    const CollectionId activeCollection = document_.activeCollectionId();
    if (activeCollection.isValid()) {
        object.collectionIds.append(activeCollection);
    }
    return insertObject(document_.size(), object);
}

ObjectId DocumentTransaction::insertObject(int index, const SceneObject &source)
{
    SceneObject object = source;
    const ObjectId objectId = document_.insertObject(index, object);
    if (objectId.isValid()) {
        changes_.addObject(objectId);
        changes_.geometryChanged = true;
        changes_.structureChanged = true;
    }
    return objectId;
}

QVector<ObjectId> DocumentTransaction::insertObjects(
    int index, const QVector<SceneObject> &objects)
{
    QVector<ObjectId> objectIds = document_.insertObjects(index, objects);
    if (objectIds.isEmpty()) {
        return objectIds;
    }

    for (const ObjectId objectId : objectIds) {
        changes_.addObject(objectId);
    }
    changes_.geometryChanged = true;
    changes_.structureChanged = true;
    return objectIds;
}

bool DocumentTransaction::replaceGeometry(ObjectId objectId, const Shape &shape)
{
    if (!document_.replace(objectId, shape)) {
        return false;
    }
    changes_.addObject(objectId);
    changes_.geometryChanged = true;
    return true;
}

bool DocumentTransaction::setObjectPlacementTranslation(
    ObjectId objectId, const Point3D &translation)
{
    if (!document_.setObjectPlacementTranslation(objectId, translation)) {
        return false;
    }
    changes_.addObject(objectId);
    changes_.placementChanged = true;
    return true;
}

bool DocumentTransaction::setObjectInstanceTransform(
    ObjectId objectId, const AffineTransform3D &transform)
{
    if (!document_.setObjectInstanceTransform(objectId, transform)) {
        return false;
    }
    changes_.addObject(objectId);
    changes_.placementChanged = true;
    return true;
}

bool DocumentTransaction::setObjectAppearance(
    ObjectId objectId,
    const QVector4D &displayColorSceneLinear,
    const WorkbenchMaterialData &workbenchMaterial)
{
    SceneObject *object = document_.mutableObject(objectId);
    if (object == nullptr || !document_.isObjectEditable(objectId)) {
        return false;
    }

    const auto unitInterval = [](float value) {
        return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
    };
    if (!unitInterval(displayColorSceneLinear.x()) ||
        !unitInterval(displayColorSceneLinear.y()) ||
        !unitInterval(displayColorSceneLinear.z()) ||
        !unitInterval(displayColorSceneLinear.w()) ||
        !unitInterval(workbenchMaterial.baseColorSceneLinear.x()) ||
        !unitInterval(workbenchMaterial.baseColorSceneLinear.y()) ||
        !unitInterval(workbenchMaterial.baseColorSceneLinear.z()) ||
        !std::isfinite(workbenchMaterial.roughness) ||
        workbenchMaterial.roughness < 0.0 || workbenchMaterial.roughness > 1.0 ||
        !std::isfinite(workbenchMaterial.metallic) ||
        workbenchMaterial.metallic < 0.0 || workbenchMaterial.metallic > 1.0 ||
        !std::isfinite(workbenchMaterial.alpha) ||
        workbenchMaterial.alpha < 0.0 || workbenchMaterial.alpha > 1.0) {
        return false;
    }

    const WorkbenchMaterialData &currentMaterial = object->workbenchMaterial;
    const bool sameDisplayColor =
        object->displayColorSceneLinear == displayColorSceneLinear;
    const bool sameMaterial =
        object->hasWorkbenchMaterial &&
        currentMaterial.baseColorSceneLinear ==
            workbenchMaterial.baseColorSceneLinear &&
        currentMaterial.roughness == workbenchMaterial.roughness &&
        currentMaterial.metallic == workbenchMaterial.metallic &&
        currentMaterial.alpha == workbenchMaterial.alpha;
    if (sameDisplayColor && sameMaterial) {
        return false;
    }

    object->displayColorSceneLinear = displayColorSceneLinear;
    object->workbenchMaterial = workbenchMaterial;
    object->hasWorkbenchMaterial = true;
    changes_.addObject(objectId);
    changes_.objectAppearanceChanged = true;
    return true;
}

bool DocumentTransaction::appendMeshMaterialSlot(
    ObjectId objectId, const QString &materialId)
{
    SceneObject *object = document_.mutableObject(objectId);
    if (object == nullptr || materialId.isEmpty() ||
        !document_.isObjectEditable(objectId) ||
        object->geometry.geometryType != GeometryType::PolygonMesh) {
        return false;
    }
    const bool materialExists = std::any_of(
        document_.materials().cbegin(), document_.materials().cend(),
        [&materialId](const DocumentMaterial &material) {
            return material.id == materialId;
        });
    if (!materialExists) return false;

    object->geometry.polygonMesh.materialSlotIds.append(materialId);
    changes_.addObject(objectId);
    changes_.structureChanged = true;
    changes_.objectAppearanceChanged = true;
    return true;
}

bool DocumentTransaction::addMaterial(const DocumentMaterial &material)
{
    const bool alreadyPresent = std::any_of(
        document_.materials().cbegin(), document_.materials().cend(),
        [&material](const DocumentMaterial &existing) {
            return existing.id == material.id;
        });
    if (!document_.addMaterial(material)) return false;
    if (!alreadyPresent) changes_.materialsChanged = true;
    return true;
}

CollectionId DocumentTransaction::insertCollection(
    const DocumentCollection &collection)
{
    const CollectionId existingId = collection.sourceId.isEmpty()
        ? CollectionId::invalid()
        : [&]() {
              for (const DocumentCollection &candidate : document_.collections()) {
                  if (candidate.sourceId == collection.sourceId &&
                      candidate.libraryPath == collection.libraryPath) {
                      return candidate.id;
                  }
              }
              return CollectionId::invalid();
          }();
    if (existingId.isValid()) return existingId;

    const CollectionId id = document_.insertCollection(collection);
    if (!id.isValid()) return id;
    changes_.addCollection(id);
    changes_.collectionsChanged = true;
    changes_.structureChanged = true;
    for (const CollectionId parentId : collection.parentIds) {
        changes_.addCollection(parentId);
    }
    if (collection.hideViewport || collection.hideSelect)
        changes_.visibilityChanged = true;
    return id;
}

bool DocumentTransaction::setCollectionVisibility(CollectionId collectionId,
                                                   bool hideViewport,
                                                   bool hideRender)
{
    if (!document_.setCollectionVisibility(collectionId, hideViewport,
                                           hideRender)) {
        return false;
    }
    changes_.addCollection(collectionId);
    changes_.collectionsChanged = true;
    changes_.visibilityChanged = true;
    return true;
}

bool DocumentTransaction::setCollectionHideSelect(CollectionId collectionId,
                                                   bool hideSelect)
{
    if (!document_.setCollectionHideSelect(collectionId, hideSelect)) {
        return false;
    }
    changes_.addCollection(collectionId);
    changes_.collectionsChanged = true;
    changes_.visibilityChanged = true;
    return true;
}

bool DocumentTransaction::setObjectVisibilityFlags(ObjectId objectId,
                                                    bool hideViewport,
                                                    bool hideSelect)
{
    if (!document_.setObjectVisibilityFlags(objectId, hideViewport,
                                             hideSelect)) {
        return false;
    }
    changes_.addObject(objectId);
    changes_.visibilityChanged = true;
    return true;
}

bool DocumentTransaction::setObjectHiddenInViewport(ObjectId objectId,
                                                    bool hidden)
{
    if (!document_.setObjectHiddenInViewport(objectId, hidden)) return false;
    changes_.addObject(objectId);
    changes_.visibilityChanged = true;
    return true;
}

bool DocumentTransaction::setObjectRenderVisibility(ObjectId objectId,
                                                     bool hideRender)
{
    if (!document_.setObjectRenderVisibility(objectId, hideRender)) return false;
    changes_.addObject(objectId);
    changes_.visibilityChanged = true;
    return true;
}

bool DocumentTransaction::setObjectViewLayerHidden(ObjectId objectId,
                                                   const QString &viewLayerKey,
                                                   bool hidden)
{
    if (!document_.setObjectViewLayerHidden(objectId, viewLayerKey, hidden)) {
        return false;
    }
    changes_.addObject(objectId);
    changes_.visibilityChanged = true;
    return true;
}

bool DocumentTransaction::setObjectParent(ObjectId objectId,
                                          ObjectId parentObjectId)
{
    if (!document_.setObjectParent(objectId, parentObjectId)) return false;
    changes_.addObject(objectId);
    if (parentObjectId.isValid()) changes_.addObject(parentObjectId);
    changes_.structureChanged = true;
    return true;
}

bool DocumentTransaction::setObjectSourceIdentity(
    ObjectId objectId, const QString &sourceObjectId)
{
    SceneObject *object = document_.mutableObject(objectId);
    if (object == nullptr) return false;
    if (object->blenderSourceObjectId == sourceObjectId) return true;
    object->blenderSourceObjectId = sourceObjectId;
    changes_.addObject(objectId);
    changes_.structureChanged = true;
    return true;
}

bool DocumentTransaction::moveBlenderSourceDataStackEntry(
    ObjectId objectId,
    BlenderSourceDataStackKind stackKind,
    qsizetype fromIndex,
    qsizetype toIndex)
{
    SceneObject *object = document_.mutableObject(objectId);
    if (object == nullptr || !document_.isObjectEditable(objectId)) return false;

    bool moved = false;
    switch (stackKind) {
    case BlenderSourceDataStackKind::Modifier:
        if (!object->blenderSourceModifiers.isEmpty()) {
            moved = moveListEntry(&object->blenderSourceModifiers,
                                  fromIndex, toIndex);
            if (moved && object->blenderSourceModifierNames.size() ==
                             object->blenderSourceModifiers.size()) {
                moveListEntry(&object->blenderSourceModifierNames,
                              fromIndex, toIndex);
            }
        } else {
            moved = moveListEntry(&object->blenderSourceModifierNames,
                                  fromIndex, toIndex);
        }
        break;
    case BlenderSourceDataStackKind::Constraint:
        moved = moveListEntry(&object->blenderSourceConstraintNames,
                              fromIndex, toIndex);
        break;
    case BlenderSourceDataStackKind::ShaderEffect:
        moved = moveListEntry(&object->blenderSourceShaderEffects,
                              fromIndex, toIndex);
        break;
    }
    if (!moved) return false;
    changes_.addObject(objectId);
    changes_.structureChanged = true;
    return true;
}

bool DocumentTransaction::transferBlenderSourceDataStack(
    ObjectId sourceObjectId,
    ObjectId destinationObjectId,
    BlenderSourceDataStackKind stackKind,
    qsizetype sourceIndex,
    bool replaceStack)
{
    if (!sourceObjectId.isValid() || !destinationObjectId.isValid() ||
        sourceObjectId == destinationObjectId ||
        !document_.isObjectEditable(destinationObjectId)) {
        return false;
    }
    const SceneObject *source = document_.object(sourceObjectId);
    if (source == nullptr) return false;
    const SceneObject sourceSnapshot = *source;
    SceneObject *destination = document_.mutableObject(destinationObjectId);
    if (destination == nullptr) return false;

    const bool sourceIsGreasePencil =
        sourceSnapshot.blenderSourceObjectType ==
        QLatin1String("GREASE_PENCIL");
    const bool destinationIsGreasePencil =
        destination->blenderSourceObjectType ==
        QLatin1String("GREASE_PENCIL");
    if ((stackKind == BlenderSourceDataStackKind::Modifier &&
         sourceIsGreasePencil != destinationIsGreasePencil) ||
        (stackKind == BlenderSourceDataStackKind::ShaderEffect &&
         (!sourceIsGreasePencil || !destinationIsGreasePencil))) {
        return false;
    }

    bool changed = false;
    switch (stackKind) {
    case BlenderSourceDataStackKind::Modifier: {
        if (replaceStack) {
            QStringList sourceNames =
                sourceSnapshot.blenderSourceModifierNames;
            if (!sourceSnapshot.blenderSourceModifiers.isEmpty()) {
                sourceNames.clear();
                for (const BlenderSourceModifier &modifier :
                     sourceSnapshot.blenderSourceModifiers) {
                    sourceNames.append(modifier.name);
                }
            }
            changed = !sameModifierStack(
                          destination->blenderSourceModifiers,
                          sourceSnapshot.blenderSourceModifiers) ||
                destination->blenderSourceModifierNames != sourceNames;
            if (changed) {
                destination->blenderSourceModifiers =
                    sourceSnapshot.blenderSourceModifiers;
                destination->blenderSourceModifierNames = sourceNames;
            }
            break;
        }

        BlenderSourceModifier copiedModifier;
        QString sourceName;
        if (!sourceSnapshot.blenderSourceModifiers.isEmpty()) {
            if (sourceIndex < 0 || sourceIndex >=
                                      sourceSnapshot.blenderSourceModifiers.size()) {
                return false;
            }
            copiedModifier =
                sourceSnapshot.blenderSourceModifiers[sourceIndex];
            sourceName = copiedModifier.name;
        } else {
            if (sourceIndex < 0 || sourceIndex >=
                                      sourceSnapshot.blenderSourceModifierNames.size()) {
                return false;
            }
            sourceName =
                sourceSnapshot.blenderSourceModifierNames[sourceIndex];
            copiedModifier.name = sourceName;
        }
        QSet<QString> usedNames;
        for (const QString &name : destination->blenderSourceModifierNames) {
            usedNames.insert(name);
        }
        if (destination->blenderSourceModifierNames.isEmpty()) {
            for (const BlenderSourceModifier &modifier :
                 destination->blenderSourceModifiers) {
                usedNames.insert(modifier.name);
            }
        }
        copiedModifier.name = uniqueStackName(sourceName, &usedNames);
        if (destination->blenderSourceModifiers.isEmpty() &&
            !destination->blenderSourceModifierNames.isEmpty()) {
            for (const QString &name :
                 destination->blenderSourceModifierNames) {
                BlenderSourceModifier existing;
                existing.name = name;
                destination->blenderSourceModifiers.append(
                    std::move(existing));
            }
        }
        destination->blenderSourceModifiers.append(
            std::move(copiedModifier));
        destination->blenderSourceModifierNames.clear();
        for (const BlenderSourceModifier &modifier :
             destination->blenderSourceModifiers) {
            destination->blenderSourceModifierNames.append(modifier.name);
        }
        changed = true;
        break;
    }
    case BlenderSourceDataStackKind::Constraint: {
        if (replaceStack) {
            changed = destination->blenderSourceConstraintNames !=
                sourceSnapshot.blenderSourceConstraintNames;
            if (changed) {
                destination->blenderSourceConstraintNames =
                    sourceSnapshot.blenderSourceConstraintNames;
            }
            break;
        }
        if (sourceIndex < 0 || sourceIndex >=
                                  sourceSnapshot.blenderSourceConstraintNames.size()) {
            return false;
        }
        QSet<QString> usedNames;
        for (const QString &name : destination->blenderSourceConstraintNames) {
            usedNames.insert(name);
        }
        destination->blenderSourceConstraintNames.append(uniqueStackName(
            sourceSnapshot.blenderSourceConstraintNames[sourceIndex],
            &usedNames));
        changed = true;
        break;
    }
    case BlenderSourceDataStackKind::ShaderEffect: {
        if (replaceStack) {
            changed = !sameShaderEffectStack(
                destination->blenderSourceShaderEffects,
                sourceSnapshot.blenderSourceShaderEffects);
            if (changed) {
                destination->blenderSourceShaderEffects =
                    sourceSnapshot.blenderSourceShaderEffects;
            }
            break;
        }
        if (sourceIndex < 0 || sourceIndex >=
                                  sourceSnapshot.blenderSourceShaderEffects.size()) {
            return false;
        }
        BlenderSourceShaderEffect copiedEffect =
            sourceSnapshot.blenderSourceShaderEffects[sourceIndex];
        QSet<QString> usedNames;
        for (const BlenderSourceShaderEffect &effect :
             destination->blenderSourceShaderEffects) {
            usedNames.insert(effect.name);
        }
        copiedEffect.name = uniqueStackName(copiedEffect.name, &usedNames);
        destination->blenderSourceShaderEffects.append(
            std::move(copiedEffect));
        changed = true;
        break;
    }
    }

    if (!changed) return false;
    changes_.addObject(destinationObjectId);
    changes_.structureChanged = true;
    return true;
}

bool DocumentTransaction::setCollectionViewLayerState(
    CollectionId collectionId,
    const QString &viewLayerKey,
    const QStringList &sourcePath,
    const QString &stateKey,
    bool value)
{
    if (!document_.setCollectionViewLayerState(collectionId, viewLayerKey,
                                                sourcePath, stateKey, value)) {
        return false;
    }
    changes_.addCollection(collectionId);
    changes_.collectionsChanged = true;
    changes_.visibilityChanged = true;
    return true;
}

bool DocumentTransaction::ensureCollectionViewLayerState(
    CollectionId collectionId,
    const QString &viewLayerKey,
    const QStringList &sourcePath)
{
    DocumentCollection *collection =
        document_.mutableCollection(collectionId);
    if (collection == nullptr || viewLayerKey.isEmpty() || sourcePath.isEmpty() ||
        collection->sourceId.isEmpty() ||
        sourcePath.back() != collection->sourceId) {
        return false;
    }
    for (const QJsonValue &value : collection->viewLayerStates) {
        const QJsonObject state = value.toObject();
        if (state.value(QStringLiteral("view_layer_key")).toString() !=
            viewLayerKey) {
            continue;
        }
        const QJsonArray storedPath = state.value(
            QStringLiteral("path")).toArray();
        if (storedPath.size() != sourcePath.size()) continue;
        bool matches = true;
        for (int index = 0; index < sourcePath.size(); ++index) {
            if (!storedPath[index].isString() ||
                storedPath[index].toString() != sourcePath[index]) {
                matches = false;
                break;
            }
        }
        if (matches) return true;
    }
    QJsonArray storedPath;
    for (const QString &part : sourcePath) storedPath.append(part);
    collection->viewLayerStates.append(QJsonObject{
        {QStringLiteral("view_layer_key"), viewLayerKey},
        {QStringLiteral("path"), storedPath},
        {QStringLiteral("exclude"), false},
        {QStringLiteral("hide_viewport"), false},
        {QStringLiteral("hide_render"), false},
        {QStringLiteral("holdout"), false},
        {QStringLiteral("indirect_only"), false}});
    changes_.addCollection(collectionId);
    changes_.collectionsChanged = true;
    changes_.structureChanged = true;
    changes_.visibilityChanged = true;
    return true;
}

bool DocumentTransaction::setCollectionSceneNames(
    CollectionId collectionId, const QStringList &sceneNames)
{
    DocumentCollection *collection =
        document_.mutableCollection(collectionId);
    if (collection == nullptr) return false;
    QStringList uniqueNames;
    for (const QString &name : sceneNames) {
        const QString trimmed = name.trimmed();
        if (!trimmed.isEmpty() && !uniqueNames.contains(trimmed)) {
            uniqueNames.append(trimmed);
        }
    }
    if (collection->sceneMembershipExplicit &&
        collection->sourceSceneNames == uniqueNames) {
        return true;
    }
    collection->sourceSceneNames = uniqueNames;
    collection->sourceSceneName = uniqueNames.isEmpty()
        ? QString{} : uniqueNames.first();
    collection->sceneMembershipExplicit = true;
    changes_.addCollection(collectionId);
    changes_.collectionsChanged = true;
    changes_.structureChanged = true;
    changes_.visibilityChanged = true;
    return true;
}

bool DocumentTransaction::removeCollectionViewLayerStates(
    CollectionId collectionId, const QStringList &viewLayerKeys)
{
    DocumentCollection *collection =
        document_.mutableCollection(collectionId);
    if (collection == nullptr) return false;
    if (viewLayerKeys.isEmpty() || collection->viewLayerStates.isEmpty()) {
        return true;
    }
    QJsonArray retained;
    bool removed = false;
    for (const QJsonValue &value : collection->viewLayerStates) {
        const QString key = value.toObject().value(
            QStringLiteral("view_layer_key")).toString();
        if (viewLayerKeys.contains(key)) {
            removed = true;
        } else {
            retained.append(value);
        }
    }
    if (!removed) return true;
    collection->viewLayerStates = retained;
    changes_.addCollection(collectionId);
    changes_.collectionsChanged = true;
    changes_.structureChanged = true;
    changes_.visibilityChanged = true;
    return true;
}

bool DocumentTransaction::setObjectDisplayName(ObjectId objectId,
                                                const QString &name)
{
    if (!document_.setObjectDisplayName(objectId, name)) return false;
    changes_.addObject(objectId);
    changes_.structureChanged = true;
    return true;
}

bool DocumentTransaction::makeObjectLocal(ObjectId objectId)
{
    if (!document_.makeObjectLocal(objectId)) return false;
    changes_.addObject(objectId);
    changes_.structureChanged = true;
    return true;
}

bool DocumentTransaction::setObjectAssetMarked(ObjectId objectId,
                                               bool assetMarked)
{
    if (!document_.setObjectAssetMarked(objectId, assetMarked)) return false;
    changes_.addObject(objectId);
    changes_.structureChanged = true;
    return true;
}

bool DocumentTransaction::renameCollection(CollectionId collectionId,
                                            const QString &name)
{
    if (!document_.renameCollection(collectionId, name)) return false;
    changes_.addCollection(collectionId);
    changes_.collectionsChanged = true;
    changes_.structureChanged = true;
    return true;
}

bool DocumentTransaction::makeCollectionLocal(CollectionId collectionId)
{
    if (!document_.makeCollectionLocal(collectionId)) return false;
    changes_.addCollection(collectionId);
    changes_.collectionsChanged = true;
    changes_.structureChanged = true;
    return true;
}

bool DocumentTransaction::setCollectionColorTag(CollectionId collectionId,
                                                const QString &colorTag)
{
    if (!document_.setCollectionColorTag(collectionId, colorTag)) return false;
    changes_.addCollection(collectionId);
    changes_.collectionsChanged = true;
    return true;
}

bool DocumentTransaction::setCollectionAssetMarked(CollectionId collectionId,
                                                   bool assetMarked)
{
    if (!document_.setCollectionAssetMarked(collectionId, assetMarked)) {
        return false;
    }
    changes_.addCollection(collectionId);
    changes_.collectionsChanged = true;
    changes_.structureChanged = true;
    return true;
}

bool DocumentTransaction::setCollectionParents(
    CollectionId collectionId, const QVector<CollectionId> &parentIds)
{
    const DocumentCollection *collection = document_.collection(collectionId);
    if (collection == nullptr) return false;
    const QVector<CollectionId> previousParentIds = collection->parentIds;
    if (!document_.setCollectionParents(collectionId, parentIds)) return false;
    changes_.addCollection(collectionId);
    for (const CollectionId parentId : previousParentIds) {
        changes_.addCollection(parentId);
    }
    for (const CollectionId parentId : parentIds) {
        changes_.addCollection(parentId);
    }
    changes_.collectionsChanged = true;
    changes_.structureChanged = true;
    return true;
}

bool DocumentTransaction::moveCollectionRelative(
    CollectionId collectionId,
    CollectionId targetCollectionId,
    CollectionId parentCollectionId,
    bool after)
{
    if (!document_.moveCollectionRelative(collectionId, targetCollectionId,
                                          parentCollectionId, after)) {
        return false;
    }
    changes_.addCollection(collectionId);
    changes_.addCollection(targetCollectionId);
    if (parentCollectionId.isValid()) {
        changes_.addCollection(parentCollectionId);
    }
    changes_.collectionsChanged = true;
    changes_.structureChanged = true;
    return true;
}

bool DocumentTransaction::setObjectCollections(
    ObjectId objectId, const QVector<CollectionId> &collectionIds)
{
    const SceneObject *object = document_.object(objectId);
    if (object == nullptr) return false;
    const QVector<CollectionId> previousCollectionIds = object->collectionIds;
    if (!document_.setObjectCollections(objectId, collectionIds)) return false;
    changes_.addObject(objectId);
    for (const CollectionId collectionId : previousCollectionIds) {
        changes_.addCollection(collectionId);
    }
    for (const CollectionId collectionId : collectionIds) {
        changes_.addCollection(collectionId);
    }
    changes_.collectionsChanged = true;
    changes_.structureChanged = true;
    changes_.visibilityChanged = true;
    return true;
}

bool DocumentTransaction::removeCollection(CollectionId collectionId)
{
    const DocumentCollection *collection = document_.collection(collectionId);
    if (collection == nullptr) return false;
    const QVector<CollectionId> parents = collection->parentIds;
    QVector<CollectionId> children;
    for (const DocumentCollection &candidate : document_.collections()) {
        if (candidate.parentIds.contains(collectionId)) {
            children.append(candidate.id);
        }
    }
    QVector<ObjectId> linkedObjects;
    for (const SceneObject &object : document_.objects()) {
        if (object.collectionIds.contains(collectionId) ||
            object.instanceCollectionId == collectionId) {
            linkedObjects.append(object.id);
        }
    }
    if (!document_.removeCollection(collectionId)) return false;
    changes_.addCollection(collectionId);
    for (const CollectionId parentId : parents) {
        changes_.addCollection(parentId);
    }
    for (const CollectionId childId : children) {
        changes_.addCollection(childId);
    }
    for (const ObjectId objectId : linkedObjects) {
        changes_.addObject(objectId);
    }
    changes_.collectionsChanged = true;
    changes_.structureChanged = true;
    changes_.visibilityChanged = true;
    return true;
}

bool DocumentTransaction::translateObjects(const QVector<ObjectId> &objectIds,
                                           const Point3D &worldDelta)
{
    if (!document_.translateObjects(objectIds, worldDelta)) {
        return false;
    }
    for (const ObjectId objectId : objectIds) {
        if (document_.object(objectId) != nullptr) {
            changes_.addObject(objectId);
        }
    }
    changes_.placementChanged = true;
    return true;
}

bool DocumentTransaction::removeObject(ObjectId objectId)
{
    const SceneObject *sceneObject = document_.object(objectId);
    if (sceneObject == nullptr) {
        return false;
    }
    if (!document_.remove(objectId)) {
        return false;
    }
    changes_.addObject(objectId);
    changes_.geometryChanged = true;
    changes_.structureChanged = true;
    return true;
}

QVector<ObjectId> DocumentTransaction::removeObjects(
    const QVector<ObjectId> &objectIds)
{
    QVector<ObjectId> removedIds = document_.removeObjects(objectIds);
    if (removedIds.isEmpty()) {
        return removedIds;
    }
    for (const ObjectId objectId : removedIds) {
        changes_.addObject(objectId);
    }
    changes_.geometryChanged = true;
    changes_.structureChanged = true;
    return removedIds;
}

bool DocumentTransaction::setSettings(const DocumentSettings &settings)
{
    if (!document_.setSettings(settings)) {
        return false;
    }
    changes_.settingsChanged = true;
    return true;
}

void DocumentTransaction::replaceDocument(const Document::Snapshot &snapshot)
{
    document_.restoreSnapshot(snapshot);
    changes_.geometryChanged = true;
    changes_.structureChanged = true;
    changes_.visibilityChanged = true;
    changes_.settingsChanged = true;
    changes_.materialsChanged = true;
    for (const SceneObject &object : snapshot.objects) {
        changes_.addObject(object.id);
    }
}

void DocumentTransaction::replaceObjects(const QVector<SceneObject> &objects)
{
    document_.replaceObjects(objects);
    changes_.geometryChanged = true;
    changes_.structureChanged = true;
    for (const SceneObject &object : document_.objects()) {
        changes_.addObject(object.id);
    }
}

void DocumentTransaction::markAllGeometryChanged()
{
    changes_.geometryChanged = true;
    for (const SceneObject &object : document_.objects()) {
        changes_.addObject(object.id);
    }
}

void DocumentTransaction::markSelectionChanged()
{
    changes_.selectionChanged = true;
}

bool DocumentTransaction::commit()
{
    if (finished_ || changes_.isEmpty()) {
        return false;
    }
    if (changes_.affectsPersistentDocument()) {
        history_.record(before_);
        document_.applyChanges(changes_);
    }
    finished_ = true;
    return true;
}

void DocumentTransaction::rollback()
{
    if (finished_) {
        return;
    }
    document_.restoreSnapshot(before_);
    finished_ = true;
}

const DocumentChangeSet &DocumentTransaction::changes() const
{
    return changes_;
}

} // namespace classiCAD
