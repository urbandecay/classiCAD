#pragma once

#include "core/geometry/nurbs_curve.h"
#include "core/geometry/work_plane.h"

#include <QRectF>
#include <QSize>

namespace classiCAD {

class ViewportTransform;

// Returns false when the control hull cannot be projected safely. Callers
// should then keep the existing exact or sampled query as a fallback.
bool projectedNurbsControlHullBounds(
    const NurbsCurve2D &curve,
    const WorkPlaneFrame &workPlaneFrame,
    const ViewportTransform &viewportTransform,
    const QSize &viewportSize,
    QRectF *screenBounds,
    const Point3D &worldOffset = {});

bool screenBoundsOverlap(const QRectF &first, const QRectF &second);

} // namespace classiCAD
