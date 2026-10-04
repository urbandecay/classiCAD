#pragma once

#include "core/document/shape.h"
#include "core/geometry/curve_geometry_data.h"

#include <QRectF>
#include <QVector>

namespace classiCAD {

// Screen-projected samples and their bounds for one curve query/render pass.
struct SampledNurbsCurve2D {
    QVector<qreal> parameters;
    QVector<QPointF> screenPoints;
    QVector<QRectF> segmentBounds;
    QRectF bounds;
};

// Transient sampling and erase-query data. This is deliberately outside the
// persistent Shape record and is rebuilt by the sampling service.
struct EraseCurveSampleCache {
    int shapeIndex = -1;
    int componentIndex = -1;
    WorkPlaneFrame workPlaneFrame;
    Shape::NurbsCurve2D curve;
    SampledNurbsCurve2D sampled;
    QVector<qreal> intersectionParameters;
    QVector<ObjectId> intersectionObjectIds;
    QVector<ParameterInterval> previewIntervals;
    int previewStrokePointCount = 0;
};

} // namespace classiCAD
