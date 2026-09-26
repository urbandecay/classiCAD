#pragma once

#include "core/document/document.h"
#include "core/model.h"
#include "services/sampling/curve_sampler.h"
#include "services/viewport/viewport_transform.h"

namespace classiCAD {

DimensionAnchorReference captureDimensionAnchor(
    const Document &document,
    const QPointF &point,
    SnapType snapType,
    const CurveSampler &curveSampler,
    const ViewportTransform &transform,
    const QSize &viewportSize);

bool resolveDimensionAnchor(const Document &document,
                            const DimensionAnchorReference &anchor,
                            const CurveSampler &curveSampler,
                            QPointF *point);

void updateAssociativeDimensions(Document &document,
                                 const CurveSampler &curveSampler);

} // namespace classiCAD
