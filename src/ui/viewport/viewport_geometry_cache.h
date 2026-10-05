/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "services/sampling/surface_tessellation_cache.h"
#include "viewport_depth_geometry.h"

#include <QHash>

namespace classiCAD {

struct ViewportRenderFrame;

// Keeps camera-independent world samples for the currently visible objects.
// The render frame owns shared immutable handles so drawing and depth picking
// consume the same prepared geometry for a given object revision.
class ViewportGeometryCache final {
public:
    void prepareFrame(ViewportRenderFrame *frame,
                      const SurfaceTessellationCache *surfaceCache);
    void clear();
    int size() const noexcept { return entries_.size(); }

private:
    struct Entry {
        quint64 geometryRevision = 0;
        quint64 preparedGeometryRevision = 0;
        Shape sourceShape;
        Point3D offset;
        QSharedPointer<const ViewportDepthGeometry> geometry;
    };

    QHash<quint64, Entry> entries_;
};

} // namespace classiCAD
