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
            object.preparedDepthGeometry =
                QSharedPointer<ViewportDepthGeometry>::create(
                    buildViewportDepthGeometry(object, surfaceCache));
            continue;
        }

        const quint64 id = object.objectId.value();
        visibleCacheableIds.insert(id);
        auto cached = entries_.find(id);
        if (cached == entries_.end() ||
            cached->geometryRevision != object.geometryRevision) {
            Entry entry;
            entry.geometryRevision = object.geometryRevision;
            entry.geometry =
                QSharedPointer<ViewportDepthGeometry>::create(
                    buildViewportDepthGeometry(object, surfaceCache));
            cached = entries_.insert(id, std::move(entry));
        }
        object.preparedDepthGeometry = cached->geometry;
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
