/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "services/viewport/viewport_transform.h"
#include "viewport_render_frame.h"
#include "viewport_shading.h"

#include <QImage>
#include <QPainter>

#include <functional>

namespace classiCAD {

// Software Workbench screen-space effects for the QPainter fallback. The color
// image is shaded first; this pass resolves a scene-wide depth/normal/object-ID
// buffer from NURBS display triangles and imported polygon-mesh triangles.
void applyRasterWorkbenchEffects(QImage *colorImage,
                                 const QVector<ViewportRenderObject> &objects,
                                 const ViewportTransform &transform,
                                 const QSize &viewportSize,
                                 qreal devicePixelRatio,
                                 const ViewportShadingSettings &settings);

// Render surface objects into temporary color layers, use their display
// tessellation to depth-test opaque coverage, then accumulate visible partial
// alpha layers and resolve once into the QPainter scene image. This is a
// software fallback compositor, not Blender's weighted OIT implementation.
bool compositeRasterWorkbenchSurfaces(
    QImage *colorImage,
    const QVector<ViewportRenderObject> &objects,
    const ViewportTransform &transform,
    const QSize &viewportSize,
    qreal devicePixelRatio,
    bool antialias,
    const std::function<void(QPainter &, const ViewportRenderObject &)> &drawObject);

} // namespace classiCAD
