#pragma once

#include "core/document/shape.h"
#include "core/geometry/curve_geometry_data.h"
#include "core/document/object_id.h"
#include "services/erase/curve_erase_query.h"
#include "services/sampling/curve_sample_data.h"

#include <QRectF>
#include <QSize>

#include <functional>

namespace classiCAD {

class CurveSampler;
class ViewportTransform;

struct TrimEraseComponentQueryResult {
    int componentIndex = -1;
    WorkPlaneFrame workPlaneFrame;
    QVector<qreal> intersectionParameters;
    QVector<ObjectId> intersectingObjectIds;
    QVector<ParameterInterval> removedIntervals;
    bool usedCachedIntersections = false;
};

struct TrimEraseShapeQueryResult {
    bool changed = false;
    bool erasedWholeObject = false;
    QVector<Shape> replacementShapes;
    QVector<TrimEraseComponentQueryResult> components;
};

using EraseIntersectionResolver = std::function<
    EraseIntersectionParameterResult(int componentIndex,
                                     const NurbsCurve2D &curve,
                                     const WorkPlaneFrame &workPlaneFrame)>;

// Computes erase/trim intervals from screen input and returns exact surviving
// NURBS geometry. Scene candidate gathering stays with the caller and is
// supplied lazily through intersectionResolver.
bool calculateTrimEraseReplacement(
    const Shape &sourceShape,
    int sourceShapeIndex,
    const QVector<QPointF> &screenStroke,
    const QRectF *trimBox,
    bool wholeObjectOnIntersectionFreeErase,
    int onlyComponentIndex,
    const QVector<EraseCurveSampleCache> *cachedTargets,
    qreal endpointTolerance,
    const CurveSampler &curveSampler,
    const ViewportTransform &viewportTransform,
    const QSize &viewportSize,
    const EraseIntersectionResolver &intersectionResolver,
    TrimEraseShapeQueryResult *result);

} // namespace classiCAD
