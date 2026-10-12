/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "services/viewport/viewport_transform.h"
#include "viewport_render_frame.h"
#include "viewport_shading.h"

#include <QImage>

namespace classiCAD {

// Software Workbench screen-space effects for the QPainter fallback. The color
// image is shaded first; this pass resolves a scene-wide depth/normal/object-ID
// buffer from NURBS display triangles and imported polygon-mesh triangles.
void applyRasterWorkbenchCavity(QImage *colorImage,
                                const QVector<ViewportRenderObject> &objects,
                                const ViewportTransform &transform,
                                const QSize &viewportSize,
                                qreal devicePixelRatio,
                                const ViewportShadingSettings &settings);

} // namespace classiCAD
