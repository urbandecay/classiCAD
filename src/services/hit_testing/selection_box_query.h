#pragma once

#include "core/document/shape.h"

#include <QRectF>
#include <QSize>

namespace classiCAD {

class CurveSampler;
class CurveHitTester;
class ViewportTransform;

struct SelectionBoxGeometryResult {
    bool applies = false;
    bool matches = false;
};

struct ProjectedShapeBoundsResult {
    QRectF bounds;
    bool hasProjectedPoints = false;
};

ProjectedShapeBoundsResult queryProjectedShapeBounds(
    const Shape &shape,
    const CurveHitTester &curveHitTester,
    const ViewportTransform &viewportTransform,
    const QSize &viewportSize,
    const Point3D &worldOffset = {});

// Evaluates selection against sampled curve geometry or a point object's
// projected location. Other object types stay with the viewport's projected
// bounds fallback.
SelectionBoxGeometryResult queryCurveOrPointSelectionBox(
    const Shape &shape,
    const QRectF &box,
    bool crossingSelection,
    const CurveSampler &curveSampler,
    const ViewportTransform &viewportTransform,
    const QSize &viewportSize,
    const Point3D &worldOffset = {});

} // namespace classiCAD
