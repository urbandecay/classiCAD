/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "viewport_render_frame.h"

#include "core/document/document.h"
#include "core/document/workbench_material_adapter.h"
#include "core/geometry/geometry_transform.h"

#include <QSet>

#include <algorithm>

namespace classiCAD {

namespace {

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
        const Layer *layer = document.layer(sceneObject.layerId);
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
        entry.showInFront = sceneObject.showInFront;
        entry.showWire = sceneObject.showWire;
        entry.showAllEdges = sceneObject.showAllEdges;
        entry.wireDisplay = sceneObject.wireDisplay;
        entry.blenderLooseMeshEdgeCompanion =
            sceneObject.blenderLooseMeshEdgeCompanion;
        entry.displayBoundsType = sceneObject.displayBoundsType;
        entry.hasDisplayBoundsData = sceneObject.hasDisplayBoundsData;
        entry.displayBoundsMinimumLocal = sceneObject.displayBoundsMinimumLocal;
        entry.displayBoundsMaximumLocal = sceneObject.displayBoundsMaximumLocal;
        entry.displayBoundsTransformWorld =
            sceneObject.displayBoundsTransformWorld;
        entry.reverseSurfaceOrientation =
            sceneObject.geometry.geometryType == GeometryType::NurbsSurface &&
            !sceneObject.blenderSourceVersion.isEmpty();
        if (layer != nullptr) {
            entry.layerColor = layer->color;
        }
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
        entry.layerLineType = layer->lineType;
        entry.layerLineWeightMm = layer->lineWeightMm;

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
    return frame;
}

} // namespace classiCAD
