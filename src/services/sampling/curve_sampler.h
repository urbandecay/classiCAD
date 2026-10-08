#pragma once

#include "core/document/shape.h"
#include "curve_sample_data.h"

#include <QSize>

namespace classiCAD {

class Document;
class ViewportTransform;

class CurveSampler final {
public:
    bool sampleNurbsCurve(const Shape::NurbsCurve2D &curve,
                          const ViewportTransform &transform,
                          const QSize &viewportSize,
                          SampledNurbsCurve2D *sampled,
                          const Point3D &worldOffset = {}) const;
    bool sampleNurbsCurve(const Shape::NurbsCurve2D &curve,
                          const WorkPlaneFrame &workPlaneFrame,
                          const ViewportTransform &transform,
                          const QSize &viewportSize,
                          SampledNurbsCurve2D *sampled,
                          const Point3D &worldOffset = {}) const;

    QVector<Shape::NurbsCurve2D> curvesForShape(const Shape &shape) const;
    QVector<EraseCurveSampleCache> sampleDocument(
        const Document &document,
        const ViewportTransform &transform,
        const QSize &viewportSize) const;
};

} // namespace classiCAD
