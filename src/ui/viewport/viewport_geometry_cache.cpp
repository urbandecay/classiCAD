/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "viewport_geometry_cache.h"

#include "viewport_render_frame.h"

#include <QSet>

namespace classiCAD {

void ViewportGeometryCache::prepareFrame(
    ViewportRenderFrame *frame,
    const SurfaceTessellationCache *surfaceCache)
{
    if (frame == nullptr) {
        return;
    }

    QSet<quint64> visibleCacheableIds;
    visibleCacheableIds.reserve(frame->objects.size());
    for (ViewportRenderObject &object : frame->objects) {
        if (!object.cacheable || !object.objectId.isValid() ||
            object.geometryRevision == 0) {
            object.preparedGeometryRevision = 0;
            object.preparedGeometryOffset = {};
            object.preparedDepthGeometry =
                QSharedPointer<ViewportDepthGeometry>::create(
                    buildViewportDepthGeometry(object, surfaceCache));
            continue;
        }

        const quint64 id = object.objectId.value();
        visibleCacheableIds.insert(id);
        auto cached = entries_.find(id);
        if (cached != entries_.end() &&
            cached->geometryRevision != object.geometryRevision &&
            cached->sourceShape.geometryType == object.shape.geometryType &&
            (object.shape.geometryType == GeometryType::NurbsSurface ||
             object.shape.geometryType == GeometryType::NurbsSolid)) {
            const bool solid = object.shape.geometryType == GeometryType::NurbsSolid;
            const Point3D &before = cached->sourceShape.nurbsSolid.displacement;
            const Point3D &after = object.shape.nurbsSolid.displacement;
            Point3D offset;
            if ((!solid || (before.x == after.x && before.y == after.y &&
                            before.z == after.z)) &&
                SurfaceTessellationCache::translationOffset(
                    shapeBaseSurface(cached->sourceShape),
                    shapeBaseSurface(object.shape), &offset)) {
                cached->geometryRevision = object.geometryRevision;
                cached->offset = offset;
            }
        }
        if (cached == entries_.end() ||
            cached->geometryRevision != object.geometryRevision) {
            Entry entry;
            entry.geometryRevision = object.geometryRevision;
            entry.preparedGeometryRevision = object.geometryRevision;
            entry.sourceShape = object.shape;
            entry.geometry =
                QSharedPointer<ViewportDepthGeometry>::create(
                    buildViewportDepthGeometry(object, surfaceCache));
            cached = entries_.insert(id, std::move(entry));
        }
        object.preparedDepthGeometry = cached->geometry;
        object.preparedGeometryRevision = cached->preparedGeometryRevision;
        object.preparedGeometryOffset = {
            cached->offset.x + object.placementTranslation.x,
            cached->offset.y + object.placementTranslation.y,
            cached->offset.z + object.placementTranslation.z};
    }

    for (auto cached = entries_.begin(); cached != entries_.end();) {
        if (!visibleCacheableIds.contains(cached.key())) {
            cached = entries_.erase(cached);
        } else {
            ++cached;
        }
    }
}

void ViewportGeometryCache::clear()
{
    entries_.clear();
}

} // namespace classiCAD
