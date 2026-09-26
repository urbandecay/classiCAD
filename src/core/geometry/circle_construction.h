#pragma once

#include <QPointF>
#include <QVector>

namespace classiCAD {

// Circle geometry is stored as {center, point-on-circumference}.
bool makeCircleDefinitionFromDiameter(const QPointF &firstEndpoint,
                                      const QPointF &secondEndpoint,
                                      QVector<QPointF> *definition);
bool makeCircleDefinitionFromThreePoints(const QPointF &first,
                                         const QPointF &second,
                                         const QPointF &third,
                                         QVector<QPointF> *definition);

} // namespace classiCAD
