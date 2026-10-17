/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "viewport_render_frame.h"

#include "core/document/document.h"
#include "core/document/workbench_material_adapter.h"
#include "core/geometry/geometry_transform.h"
#include "core/geometry/shape_mapping.h"

#include <QSet>

#include <algorithm>

namespace classiCAD {

namespace {

QString activeImportedSceneCameraObjectId(const Document &document,
                                          bool *sceneFound)
{
    if (sceneFound != nullptr) *sceneFound = false;
    const QString activeLayer = document.settings().activeViewLayerKey;
    for (const QJsonValue &sceneValue :
         document.settings().importedBlenderScenes) {
        const QJsonObject scene = sceneValue.toObject();
        bool matchesActiveLayer = activeLayer.isEmpty();
        if (!activeLayer.isEmpty()) {
            for (const QJsonValue &layerValue :
                 scene.value(QStringLiteral("view_layers")).toArray()) {
                if (layerValue.toObject().value(QStringLiteral("key"))
                        .toString() == activeLayer) {
                    matchesActiveLayer = true;
                    break;
                }
            }
        }
        if (!matchesActiveLayer) continue;
        if (sceneFound != nullptr) *sceneFound = true;
        return scene.value(QStringLiteral("active_camera")).toObject()
            .value(QStringLiteral("object_id")).toString();
    }
    return {};
}

bool remapCameraBackgroundPicture(Shape *shape,
                                  const ViewportTransform &camera,
                                  const QSize &viewportSize)
{
    if (shape == nullptr ||
        shape->geometryType != GeometryType::Picture ||
        !shape->pictureCameraViewOnly ||
        shape->pictureCameraLocalCorners.size() != 4 ||
        !camera.cameraState().cameraView || viewportSize.isEmpty()) {
        return false;
    }
    const Point3D cameraPosition = camera.cameraPosition(viewportSize);
    Point3D forward = camera.viewDirection();
    Point3D up = camera.viewUp();
    QVector3D forwardVector(float(forward.x), float(forward.y),
                            float(forward.z));
    QVector3D upVector(float(up.x), float(up.y), float(up.z));
    if (forwardVector.lengthSquared() <= 1.0e-16f ||
        upVector.lengthSquared() <= 1.0e-16f) {
        return false;
    }
    forwardVector.normalize();
    upVector.normalize();
    QVector3D rightVector =
        QVector3D::crossProduct(forwardVector, upVector);
    if (rightVector.lengthSquared() <= 1.0e-16f) return false;
    rightVector.normalize();

    QVector<Point3D> worldCorners;
    worldCorners.reserve(4);
    const qreal sourceLens =
        shape->pictureCameraSourceFocalLengthMillimeters;
    const qreal currentLens = camera.cameraPreferences().focalLengthMillimeters;
    const qreal frameScale = camera.isPerspectiveEnabled() &&
        std::isfinite(sourceLens) && sourceLens > 0.0 &&
        std::isfinite(currentLens) && currentLens > 0.0
            ? sourceLens / currentLens
            : 1.0;
    for (const Point3D &local : shape->pictureCameraLocalCorners) {
        const QVector3D world =
            QVector3D(float(cameraPosition.x), float(cameraPosition.y),
                      float(cameraPosition.z)) +
            rightVector * float(local.x * frameScale) +
            upVector * float(local.y * frameScale) +
            forwardVector * float(-local.z);
        if (!std::isfinite(world.x()) || !std::isfinite(world.y()) ||
            !std::isfinite(world.z())) {
            return false;
        }
        worldCorners.append({world.x(), world.y(), world.z()});
    }
    const QVector3D edgeX(float(worldCorners[1].x - worldCorners[0].x),
                          float(worldCorners[1].y - worldCorners[0].y),
                          float(worldCorners[1].z - worldCorners[0].z));
    const QVector3D edgeY(float(worldCorners[0].x - worldCorners[3].x),
                          float(worldCorners[0].y - worldCorners[3].y),
                          float(worldCorners[0].z - worldCorners[3].z));
    const QVector3D normal = QVector3D::crossProduct(edgeX, edgeY);
    const WorkPlaneFrame frame = makeWorkPlaneFrameFromNormal(
        worldCorners[0], {normal.x(), normal.y(), normal.z()},
        {edgeX.x(), edgeX.y(), edgeX.z()});
    if (!isValidWorkPlaneFrame(frame)) return false;
    shape->workPlaneFrame = frame;
    shape->points.clear();
    shape->points.reserve(worldCorners.size());
    for (const Point3D &corner : worldCorners) {
        shape->points.append(worldPointToWorkPlaneFrame(corner, frame));
    }
    return true;
}

AffineTransform3D transformPreviewLinear(
    const ViewportRenderTransform &preview)
{
    if (preview.kind == ViewportRenderTransformKind::Scale) {
        return affineLinearTransformFromPointMap(
            [&preview](const Point3D &point) {
                return scalePointInFrame(
                    point, preview.scaleBasePoint, preview.scaleAxis,
                    preview.scaleFactor,
                    preview.scaleMode == ScaleMode::OneD,
                    preview.scaleSurfaceFrame);
            });
    }
    if (preview.kind == ViewportRenderTransformKind::Rotate) {
        return affineLinearTransformFromPointMap(
            [&preview](const Point3D &point) {
                return rotatePointAboutAxis(
                    point, preview.rotatePivot, preview.rotateAxis,
                    preview.rotateAngle);
            });
    }
    return identityAffineTransform3D;
}

bool applyCollectionInstanceTransform(ViewportRenderObject *proxy,
                                      const AffineTransform3D &transform)
{
    if (proxy == nullptr || !isValidAffineTransform(transform, true)) {
        return false;
    }
    if (proxy->hasDisplayBoundsData) {
        proxy->displayBoundsTransformWorld = multiplyAffineTransforms(
            transform, proxy->displayBoundsTransformWorld);
    }
    if (proxy->shape.geometryType == GeometryType::PolygonMesh) {
        proxy->meshObjectTransform = multiplyAffineTransforms(
            transform, proxy->meshObjectTransform);
        return isValidAffineTransform(proxy->meshObjectTransform);
    }
    if (proxy->placementTranslation.x != 0.0 ||
        proxy->placementTranslation.y != 0.0 ||
        proxy->placementTranslation.z != 0.0) {
        if (!bakeShapePlacementTranslation(&proxy->shape,
                                           proxy->placementTranslation)) {
            return false;
        }
        proxy->placementTranslation = {};
    }
    if (!transformShapeGeometryAffine(&proxy->shape, transform)) {
        return false;
    }
    if (!proxy->blenderSurfaceWireVerticesWorld.isEmpty()) {
        for (Point3D &point : proxy->blenderSurfaceWireVerticesWorld) {
            point = transformAffinePoint(transform, point);
        }
    }
    const double determinant =
        transform[0] * (transform[5] * transform[10] -
                        transform[6] * transform[9]) -
        transform[1] * (transform[4] * transform[10] -
                        transform[6] * transform[8]) +
        transform[2] * (transform[4] * transform[9] -
                        transform[5] * transform[8]);
    if (determinant < 0.0) proxy->reverseSurfaceOrientation =
        !proxy->reverseSurfaceOrientation;
    return true;
}

} // namespace

const ViewportRenderObject *ViewportRenderFrame::find(
    ObjectId objectId) const noexcept
{
    if (!objectId.isValid()) {
        return nullptr;
    }
    const auto found = objectIndices.constFind(objectId.value());
    if (found == objectIndices.cend() || *found < 0 ||
        *found >= objects.size()) {
        return nullptr;
    }
    return &objects[*found];
}

ViewportRenderFrame buildViewportRenderFrame(
    const Document &document,
    const ViewportTransform &camera,
    const QSize &viewportSize,
    const ViewportRenderFrameInput &input)
{
    ViewportRenderFrame frame;
    frame.camera = camera;
    frame.viewportSize = viewportSize;
    frame.activeToolPreview = input.activeToolPreview;
    frame.objects.reserve(document.size());
    frame.objectIndices.reserve(document.size());

    QSet<quint64> selectedIds;
    selectedIds.reserve(input.selectedObjectIds.size() +
                        input.highlightedObjectIds.size() + 1);
    for (const ObjectId id : input.selectedObjectIds) {
        if (id.isValid()) {
            selectedIds.insert(id.value());
        }
    }
    if (input.primarySelectedObjectId.isValid()) {
        selectedIds.insert(input.primarySelectedObjectId.value());
    }
    for (const ObjectId id : input.highlightedObjectIds) {
        if (id.isValid()) {
            selectedIds.insert(id.value());
        }
    }

    QSet<quint64> transformedIds;
    if (input.transformPreview.kind != ViewportRenderTransformKind::None) {
        transformedIds.reserve(input.transformPreview.objectIds.size());
        for (const ObjectId id : input.transformPreview.objectIds) {
            if (id.isValid()) {
                transformedIds.insert(id.value());
            }
        }
    }

    QHash<QString, ObjectId> blenderObjectIdsBySourceId;
    blenderObjectIdsBySourceId.reserve(document.size());
    for (const SceneObject &sceneObject : document.objects()) {
        if (!sceneObject.blenderLooseMeshEdgeCompanion &&
            !sceneObject.blenderSourceObjectId.isEmpty()) {
            blenderObjectIdsBySourceId.insert(
                sceneObject.blenderSourceObjectId, sceneObject.id);
        }
    }

    bool activeSceneCameraFound = false;
    const QString activeSceneCameraId = activeImportedSceneCameraObjectId(
        document, &activeSceneCameraFound);

    QSet<quint64> hiddenPreviewIds;
    if (!input.activeToolPreview.shapes.isEmpty()) {
        hiddenPreviewIds.reserve(input.activeToolPreview.hiddenObjectIds.size());
        for (const ObjectId id : input.activeToolPreview.hiddenObjectIds) {
            if (id.isValid()) {
                hiddenPreviewIds.insert(id.value());
            }
        }
    }

    int objectIndex = 0;
    for (const SceneObject &sceneObject : document.objects()) {
        if (camera.cameraState().cameraView &&
            sceneObject.geometry.pictureCameraViewOnly &&
            activeSceneCameraFound &&
            sceneObject.blenderSourceObjectId != activeSceneCameraId) {
            ++objectIndex;
            continue;
        }
        const bool visible = input.visibility ==
                ViewportRenderFrameInput::Visibility::Render
            ? document.isObjectRenderable(sceneObject.id)
            : document.isObjectVisible(sceneObject.id);
        if (!visible ||
            hiddenPreviewIds.contains(sceneObject.id.value())) {
            ++objectIndex;
            continue;
        }

        ViewportRenderObject entry;
        entry.shape = sceneObject.geometry;
        const bool cameraBackgroundRemapped = remapCameraBackgroundPicture(
            &entry.shape, camera, viewportSize);
        if (cameraBackgroundRemapped) {
            // Its world-space shape changes with the current camera frame,
            // while the document geometry revision does not. Build matching
            // depth geometry for this snapshot instead of reusing stale data.
            entry.cacheable = false;
        }
        // Resolve imported Blender material slots into this render snapshot.
        // The inline slot array is only a transient viewport bridge; the
        // document's material datablocks remain the persisted source of truth.
        if (entry.shape.geometryType == GeometryType::PolygonMesh &&
            !entry.shape.polygonMesh.materialSlotIds.isEmpty()) {
            entry.shape.polygonMesh.materialSlots.clear();
            entry.shape.polygonMesh.materialSlots.reserve(
                entry.shape.polygonMesh.materialSlotIds.size());
            for (const QString &materialId :
                 entry.shape.polygonMesh.materialSlotIds) {
                PolygonMeshMaterialSlot slot;
                const auto found = std::find_if(
                    document.materials().cbegin(), document.materials().cend(),
                    [&materialId](const DocumentMaterial &material) {
                        return material.id == materialId;
                    });
                if (found != document.materials().cend()) {
                    slot.name = found->name;
                    slot.libraryPath = found->libraryPath;
                    slot.baseColorSceneLinear = found->baseColorSceneLinear;
                    slot.roughness = found->roughness;
                    slot.metallic = found->metallic;
                    slot.alpha = found->alpha;
                }
                entry.shape.polygonMesh.materialSlots.append(std::move(slot));
            }
        }
        entry.placementTranslation = sceneObject.placementTranslation;
        entry.meshObjectTransform = sceneObject.meshObjectTransform;
        entry.objectId = sceneObject.id;
        entry.displayName = sceneObject.displayName;
        entry.libraryPath = sceneObject.libraryPath;
        entry.workbenchRandomLibraryPath =
            sceneObject.workbenchRandomLibraryPath;
        entry.blenderSourceObjectType = sceneObject.blenderSourceObjectType;
        if (sceneObject.geometry.geometryType == GeometryType::NurbsSurface &&
            !sceneObject.blenderSurfaceWireGeometryFingerprint.isEmpty() &&
            sceneObject.blenderSurfaceWireGeometryFingerprint ==
                nurbsSurfaceGeometryFingerprint(
                    sceneObject.geometry.nurbsSurface)) {
            entry.blenderSurfaceWireVerticesWorld =
                sceneObject.blenderSurfaceWireVerticesWorld;
        }
        entry.objectIndex = objectIndex++;
        entry.geometryRevision = document.objectGeometryRevision(entry.objectId);
        entry.selected = selectedIds.contains(entry.objectId.value());
        entry.active = input.primarySelectedObjectId == entry.objectId;
        entry.showInFront = sceneObject.showInFront;
        entry.hideSurfacePick = sceneObject.hideSurfacePick;
        entry.showWire = sceneObject.showWire;
        entry.showAllEdges = sceneObject.showAllEdges;
        entry.wireDisplay = sceneObject.wireDisplay;
        entry.blenderLooseMeshEdgeCompanion =
            sceneObject.blenderLooseMeshEdgeCompanion;
        entry.blenderLooseMeshEdgeSourceIndices =
            sceneObject.blenderLooseMeshEdgeSourceIndices;
        if (sceneObject.blenderLooseMeshEdgeCompanion &&
            !sceneObject.blenderSourceObjectId.isEmpty()) {
            const ObjectId sourceObjectId = blenderObjectIdsBySourceId.value(
                sceneObject.blenderSourceObjectId, ObjectId::invalid());
            entry.blenderLooseMeshEdgeSourceSelected =
                sourceObjectId.isValid() &&
                selectedIds.contains(sourceObjectId.value());
            entry.blenderLooseMeshEdgeSourceActive =
                sourceObjectId.isValid() &&
                input.primarySelectedObjectId == sourceObjectId;
            entry.active = entry.active ||
                           entry.blenderLooseMeshEdgeSourceActive;
            entry.blenderLooseMeshEdgeSourceTransforming =
                sourceObjectId.isValid() &&
                transformedIds.contains(sourceObjectId.value());
            if (sourceObjectId.isValid()) {
                const QSet<int> selectedSourceEdges =
                    input.blenderMeshEdgeSelections.value(
                        sourceObjectId.value());
                for (int componentIndex = 0;
                     componentIndex <
                         sceneObject.blenderLooseMeshEdgeSourceIndices.size();
                     ++componentIndex) {
                    if (selectedSourceEdges.contains(
                            sceneObject.blenderLooseMeshEdgeSourceIndices[
                                componentIndex])) {
                        entry.blenderLooseMeshEdgeSelectedComponents.insert(
                            componentIndex);
                    }
                }
            }
        }
        entry.displayBoundsType = sceneObject.displayBoundsType;
        entry.hasDisplayBoundsData = sceneObject.hasDisplayBoundsData;
        entry.displayBoundsMinimumLocal = sceneObject.displayBoundsMinimumLocal;
        entry.displayBoundsMaximumLocal = sceneObject.displayBoundsMaximumLocal;
        entry.displayBoundsTransformWorld =
            sceneObject.displayBoundsTransformWorld;
        entry.reverseSurfaceOrientation =
            sceneObject.geometry.geometryType == GeometryType::NurbsSurface &&
            !sceneObject.blenderSourceVersion.isEmpty();
        entry.displayColorSceneLinear = sceneObject.displayColorSceneLinear;
        entry.workbenchMaterial = sceneObject.workbenchMaterial;
        entry.hasWorkbenchMaterial = sceneObject.hasWorkbenchMaterial;
        if (sceneObject.geometry.geometryType == GeometryType::NurbsSolid &&
            sceneObject.geometry.nurbsSolid.boundaryFaces.isEmpty()) {
            for (auto faceIt = sceneObject.nurbsSolidFaceMaterialIds.cbegin();
                 faceIt != sceneObject.nurbsSolidFaceMaterialIds.cend(); ++faceIt) {
                const auto materialIt = std::find_if(
                    document.materials().cbegin(), document.materials().cend(),
                    [&faceIt](const DocumentMaterial &material) {
                        return material.id == faceIt.value();
                    });
                if (materialIt != document.materials().cend()) {
                    entry.nurbsSolidFaceMaterials.insert(
                        faceIt.key(),
                        workbenchMaterialDataFromDocumentMaterial(*materialIt));
                }
            }
        }
        if (sceneObject.geometry.geometryType == GeometryType::PolygonMesh) {
            const PolygonMesh3D &mesh = sceneObject.geometry.polygonMesh;
            if (!mesh.materialSlotIds.isEmpty()) {
                entry.meshMaterials.reserve(mesh.materialSlotIds.size());
                for (const QString &slotId : mesh.materialSlotIds) {
                    WorkbenchMaterialData slotData;
                    const auto material = std::find_if(
                        document.materials().cbegin(),
                        document.materials().cend(),
                        [&slotId](const DocumentMaterial &candidate) {
                            return candidate.id == slotId;
                        });
                    if (material != document.materials().cend()) {
                        slotData =
                            workbenchMaterialDataFromDocumentMaterial(*material);
                    }
                    entry.meshMaterials.append(slotData);
                }
            } else {
                entry.meshMaterials.reserve(mesh.materialSlots.size());
                for (const PolygonMeshMaterialSlot &slot : mesh.materialSlots) {
                    WorkbenchMaterialData slotData;
                    if (slot.baseColorSceneLinear.size() >= 3) {
                        slotData.baseColorSceneLinear = QVector3D(
                            slot.baseColorSceneLinear[0],
                            slot.baseColorSceneLinear[1],
                            slot.baseColorSceneLinear[2]);
                    }
                    slotData.roughness = slot.roughness;
                    slotData.metallic = slot.metallic;
                    slotData.alpha = slot.alpha;
                    entry.meshMaterials.append(slotData);
                }
            }
        }
        const bool transformObject =
            transformedIds.contains(entry.objectId.value());
        if (transformObject &&
            input.transformPreview.kind == ViewportRenderTransformKind::Scale) {
            entry.scalePreview = true;
            entry.cacheable = false;
            const ViewportRenderTransform &preview = input.transformPreview;
            if (bakeShapePlacementTranslation(
                    &entry.shape, entry.placementTranslation)) {
                entry.placementTranslation = {};
            }
            const auto componentTargets =
                preview.controlPointIndices.constFind(entry.objectId.value());
            if (componentTargets != preview.controlPointIndices.cend()) {
                transformShapeControlPoints(
                    &entry.shape, componentTargets.value(),
                    [&preview](const Point3D &point) {
                        return scalePointInFrame(
                            point, preview.scaleBasePoint,
                            preview.scaleAxis, preview.scaleFactor,
                            preview.scaleMode == ScaleMode::OneD,
                            preview.scaleSurfaceFrame);
                    });
            } else if (sceneObject.instanceCollectionId.isValid()) {
                transformShapeControlPoints(
                    &entry.shape, QSet<int>{0},
                    [&preview](const Point3D &point) {
                        return scalePointInFrame(
                            point, preview.scaleBasePoint,
                            preview.scaleAxis, preview.scaleFactor,
                            preview.scaleMode == ScaleMode::OneD,
                            preview.scaleSurfaceFrame);
                    });
            } else {
                scaleShapeGeometry(&entry.shape,
                                   preview.scaleBasePoint,
                                   preview.scaleAxis,
                                   preview.scaleFactor,
                                   preview.scaleMode == ScaleMode::OneD,
                                   preview.scaleSurfaceFrame);
            }
        } else if (transformObject &&
                   input.transformPreview.kind == ViewportRenderTransformKind::Rotate) {
            entry.rotatePreview = true;
            entry.cacheable = false;
            const ViewportRenderTransform &preview = input.transformPreview;
            if (bakeShapePlacementTranslation(
                    &entry.shape, entry.placementTranslation)) {
                entry.placementTranslation = {};
            }
            const auto componentTargets =
                preview.controlPointIndices.constFind(entry.objectId.value());
            if (componentTargets != preview.controlPointIndices.cend()) {
                transformShapeControlPoints(
                    &entry.shape, componentTargets.value(),
                    [&preview](const Point3D &point) {
                        return rotatePointAboutAxis(
                            point, preview.rotatePivot,
                            preview.rotateAxis, preview.rotateAngle);
                    });
            } else if (sceneObject.instanceCollectionId.isValid()) {
                transformShapeControlPoints(
                    &entry.shape, QSet<int>{0},
                    [&preview](const Point3D &point) {
                        return rotatePointAboutAxis(
                            point, preview.rotatePivot,
                            preview.rotateAxis, preview.rotateAngle);
                    });
            } else {
                rotateShapeGeometry(&entry.shape,
                                    preview.rotatePivot,
                                    preview.rotateAxis,
                                    preview.rotateAngle);
            }
        }

        frame.objectIndices.insert(entry.objectId.value(), frame.objects.size());
        frame.objects.append(std::move(entry));
    }
    // Blender collection instances draw the linked collection at the Empty's
    // transform. Keep ordinary source entries for their own scene membership,
    // then append uncached draw proxies that pick as the owning Empty.
    QHash<quint64, int> sourceEntryByObjectId;
    sourceEntryByObjectId.reserve(frame.objects.size());
    for (int index = 0; index < frame.objects.size(); ++index) {
        const ViewportRenderObject &entry = frame.objects[index];
        sourceEntryByObjectId.insert(entry.objectId.value(), index);
    }

    QHash<quint64, DocumentCollection> collectionsById;
    collectionsById.reserve(document.collections().size());
    for (const DocumentCollection &collection : document.collections()) {
        collectionsById.insert(collection.id.value(), collection);
    }

    const auto instanceObjectOrigin = [](const ViewportRenderObject &entry) {
        Point3D origin;
        if (!entry.shape.points.isEmpty()) {
            origin = shapePointToWorld(entry.shape, entry.shape.points.first());
        }
        origin.x += entry.placementTranslation.x;
        origin.y += entry.placementTranslation.y;
        origin.z += entry.placementTranslation.z;
        return origin;
    };
        const auto collectionInstanceTransform =
            [&input, &transformedIds](const SceneObject &object) {
                AffineTransform3D linear = object.instanceTransform;
                if (transformedIds.contains(object.id.value()) &&
                    (input.transformPreview.kind ==
                         ViewportRenderTransformKind::Rotate ||
                     input.transformPreview.kind ==
                         ViewportRenderTransformKind::Scale)) {
                    linear = multiplyAffineTransforms(
                        transformPreviewLinear(input.transformPreview), linear);
                }
                return linear;
            };
        const auto makeInstanceTransform = [](
            const Point3D &origin, const Point3D &collectionOffset,
            const AffineTransform3D &linear) {
            const Point3D negativeOffset{-collectionOffset.x,
                                         -collectionOffset.y,
                                         -collectionOffset.z};
            return multiplyAffineTransforms(
                multiplyAffineTransforms(affineTranslation(origin), linear),
                affineTranslation(negativeOffset));
        };

    for (const SceneObject &instanceObject : document.objects()) {
        if (!instanceObject.instanceCollectionId.isValid()) continue;
        const auto instanceEntryIndex = sourceEntryByObjectId.constFind(
            instanceObject.id.value());
        if (instanceEntryIndex == sourceEntryByObjectId.cend()) continue;
        const auto target = collectionsById.constFind(
            instanceObject.instanceCollectionId.value());
        if (target == collectionsById.cend()) continue;
        const ViewportRenderObject &instanceEntry =
            frame.objects[instanceEntryIndex.value()];
        const Point3D instanceOrigin = instanceObjectOrigin(instanceEntry);
        const bool instanceSelected = instanceEntry.selected;
        const bool instanceActive = instanceEntry.active;
        QSet<quint64> collectionPath;
        QSet<quint64> emittedObjectIds;
        int emittedCount = 0;
        const auto appendCollectionInstance =
            [&](const auto &self, CollectionId collectionId,
                const AffineTransform3D &collectionTransform) -> void {
                if (emittedCount >= 20000 || collectionPath.size() >= 64 ||
                    collectionPath.contains(collectionId.value())) {
                    return;
                }
                collectionPath.insert(collectionId.value());
                QSet<quint64> hierarchy{collectionId.value()};
                for (qsizetype collectionIndex = 0;
                     collectionIndex < document.collections().size();
                     ++collectionIndex) {
                    bool added = false;
                    for (const DocumentCollection &candidate :
                         document.collections()) {
                        if (hierarchy.contains(candidate.id.value())) continue;
                        for (const CollectionId parentId : candidate.parentIds) {
                            if (!hierarchy.contains(parentId.value())) continue;
                            hierarchy.insert(candidate.id.value());
                            added = true;
                            break;
                        }
                    }
                    if (!added) break;
                }

                for (const SceneObject &sourceObject : document.objects()) {
                    if (emittedCount >= 20000 ||
                        emittedObjectIds.contains(sourceObject.id.value())) {
                        continue;
                    }
                    bool belongs = false;
                    for (const CollectionId membership :
                         sourceObject.collectionIds) {
                        if (hierarchy.contains(membership.value())) {
                            belongs = true;
                            break;
                        }
                    }
                    if (!belongs) continue;
                    const auto sourceIndex = sourceEntryByObjectId.constFind(
                        sourceObject.id.value());
                    if (sourceIndex == sourceEntryByObjectId.cend()) continue;
                    emittedObjectIds.insert(sourceObject.id.value());
                    const ViewportRenderObject sourceEntry =
                        frame.objects[sourceIndex.value()];
                    if (sourceObject.instanceCollectionId.isValid()) {
                        const auto nestedTarget = collectionsById.constFind(
                            sourceObject.instanceCollectionId.value());
                        if (nestedTarget == collectionsById.cend()) continue;
                        const Point3D nestedOrigin =
                            instanceObjectOrigin(sourceEntry);
                        const AffineTransform3D nestedTransform =
                            makeInstanceTransform(
                                nestedOrigin, nestedTarget->instanceOffset,
                                collectionInstanceTransform(sourceObject));
                        self(self, sourceObject.instanceCollectionId,
                             multiplyAffineTransforms(collectionTransform,
                                                      nestedTransform));
                        continue;
                    }
                    ViewportRenderObject proxy = sourceEntry;
                    proxy.objectId = instanceObject.id;
                    proxy.objectIndex = objectIndex++;
                    if (!applyCollectionInstanceTransform(
                            &proxy, collectionTransform)) {
                        continue;
                    }
                    proxy.cacheable = false;
                    proxy.preparedDepthGeometry.reset();
                    proxy.preparedGeometryRevision = 0;
                    proxy.preparedGeometryOffset = {};
                    proxy.selected = instanceSelected;
                    proxy.active = instanceActive;
                    proxy.scalePreview = false;
                    proxy.rotatePreview = false;
                    proxy.blenderLooseMeshEdgeSourceSelected = false;
                    proxy.blenderLooseMeshEdgeSourceActive = false;
                    proxy.blenderLooseMeshEdgeSourceTransforming = false;
                    frame.objects.append(std::move(proxy));
                    ++emittedCount;
                }
                collectionPath.remove(collectionId.value());
            };

        const AffineTransform3D initialTransform = makeInstanceTransform(
            instanceOrigin, target->instanceOffset,
            collectionInstanceTransform(instanceObject));
        appendCollectionInstance(appendCollectionInstance,
                                 instanceObject.instanceCollectionId,
                                 initialTransform);
    }
    return frame;
}

} // namespace classiCAD
