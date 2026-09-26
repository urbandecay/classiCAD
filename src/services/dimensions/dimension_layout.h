#pragma once

#include "core/model.h"
#include "services/viewport/viewport_transform.h"

#include <QLineF>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QVector>

namespace classiCAD {

enum class DimensionFontStyle {
    Standard,
    Architectural,
};

// Screen-space drawing data for persistent dimension annotations. Keeping
// this shared lets rendering and hit-testing agree exactly on the visible
// dimension lines, arrows, and label area.
struct DimensionScreenLayout {
    QVector<QLineF> lines;
    QVector<QPolygonF> arrowHeads;
    QPointF labelCenter;
    QRectF labelBounds;
    QString label;
    bool valid = false;
};

DimensionScreenLayout buildDimensionScreenLayout(const Shape &shape,
                                                 const ViewportTransform &transform,
                                                 const QSize &viewportSize,
                                                 DimensionFontStyle fontStyle =
                                                     DimensionFontStyle::Standard);
qreal distanceToDimensionLayout(const QPointF &screenPosition,
                                const DimensionScreenLayout &layout);

} // namespace classiCAD
