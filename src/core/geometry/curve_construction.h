#pragma once

#include "core/document/shape.h"
#include "core/geometry/construction_modes.h"

namespace classiCAD {

Shape::NurbsCurve2D makeDegreeOneNurbs(const QVector<QPointF> &points);
Shape::NurbsCurve2D makeBezierNurbs(const QVector<QPointF> &points);
Shape::NurbsCurve2D makeCircleNurbs(const QVector<QPointF> &points);
Shape::NurbsCurve2D makeEllipseNurbs(EllipseMode mode,
                                     const QVector<QPointF> &points);
QVector<QPointF> makeRectanglePoints(RectangleMode mode,
                                     const QVector<QPointF> &points);
QVector<QPointF> makeRegularPolygonPoints(PolygonMode mode,
                                          const QVector<QPointF> &points,
                                          int sides);
QVector<QPointF> makePictureFramePoints(const QPointF &firstCorner,
                                        const QPointF &cursorCorner,
                                        qreal imageAspectRatio);

} // namespace classiCAD
