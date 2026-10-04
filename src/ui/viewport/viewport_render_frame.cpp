/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "viewport_render_frame.h"

#include "core/document/document.h"
#include "core/geometry/geometry_transform.h"

#include <QSet>

namespace classiCAD {

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

    int objectIndex = 0;
    for (const SceneObject &sceneObject : document.objects()) {
        const Layer *layer = document.layer(sceneObject.layerId);
        if (layer == nullptr || !layer->visible || layer->frozen) {
            ++objectIndex;
            continue;
        }

        ViewportRenderObject entry;
        entry.shape = sceneObject.geometry;
        entry.objectId = sceneObject.id;
        entry.objectIndex = objectIndex++;
        entry.geometryRevision = document.objectGeometryRevision(entry.objectId);
        entry.selected = selectedIds.contains(entry.objectId.value());
        entry.layerColor = layer->color;
        entry.layerLineType = layer->lineType;
        entry.layerLineWeightMm = layer->lineWeightMm;

        const bool transformObject =
            transformedIds.contains(entry.objectId.value());
        if (transformObject &&
            input.transformPreview.kind == ViewportRenderTransformKind::Scale) {
            entry.scalePreview = true;
            entry.cacheable = false;
            const ViewportRenderTransform &preview = input.transformPreview;
            scaleShapeGeometry(&entry.shape,
                               preview.scaleBasePoint,
                               preview.scaleAxis,
                               preview.scaleFactor,
                               preview.scaleMode == ScaleMode::OneD,
                               preview.scaleSurfaceFrame);
        } else if (transformObject &&
                   input.transformPreview.kind == ViewportRenderTransformKind::Rotate) {
            entry.rotatePreview = true;
            entry.cacheable = false;
            const ViewportRenderTransform &preview = input.transformPreview;
            rotateShapeGeometry(&entry.shape,
                                preview.rotatePivot,
                                preview.rotateAxis,
                                preview.rotateAngle);
        }

        frame.objectIndices.insert(entry.objectId.value(), frame.objects.size());
        frame.objects.append(std::move(entry));
    }
    return frame;
}

} // namespace classiCAD
