#pragma once

#include "core/document/dimension_anchor.h"
#include "services/snapping/snap_types.h"

#include <QPointF>
#include <QSize>

namespace classiCAD {

class Document;
class CurveSampler;
class ViewportTransform;

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
