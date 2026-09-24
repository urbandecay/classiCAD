#pragma once

#include "services/viewport/viewport_transform.h"

#include "core/document/document.h"
#include "core/model.h"

namespace classiCAD {

class CurveSampler final {
public:
    bool sampleNurbsCurve(const Shape::NurbsCurve2D &curve,
                          const ViewportTransform &transform,
                          const QSize &viewportSize,
                          SampledNurbsCurve2D *sampled) const;

    QVector<Shape::NurbsCurve2D> curvesForShape(const Shape &shape) const;
    QVector<EraseCurveSampleCache> sampleDocument(
        const Document &document,
        const ViewportTransform &transform,
        const QSize &viewportSize) const;
};

} // namespace classiCAD
