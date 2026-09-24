#pragma once

#include "services/viewport/viewport_transform.h"

#include "core/document/document.h"
#include "core/model.h"

namespace classiCAD {

class CurveHitTester final {
public:
    qreal distanceToSegment(const QPointF &point,
                            const QPointF &start,
                            const QPointF &end) const;

    qreal distanceToShape(const QPointF &screenPosition,
                          const Shape &shape,
                          const ViewportTransform &transform,
                          const QSize &viewportSize) const;

    qreal distanceToNurbsCurve(const QPointF &screenPosition,
                               const Shape::NurbsCurve2D &curve,
                               const ViewportTransform &transform,
                               const QSize &viewportSize) const;

    int hitTestShape(const Document &document,
                     const QPointF &screenPosition,
                     const ViewportTransform &transform,
                     const QSize &viewportSize,
                     bool editableOnly = false) const;

    QVector<QPointF> controlPointsForShape(const Shape &shape) const;

    bool hitTestSelectedControlPoint(
        const Document &document,
        const QVector<int> &selectedShapeIndices,
        const QPointF &screenPosition,
        const ViewportTransform &transform,
        const QSize &viewportSize,
        int *shapeIndex,
        int *controlPointIndex) const;

private:
    QVector<QPointF> rectangleVertices(const Shape &shape) const;
    qreal distanceToArc(const QPointF &screenPosition,
                        const Shape &shape,
                        const ViewportTransform &transform,
                        const QSize &viewportSize) const;
    qreal distanceToCubicCurve(const QPointF &screenPosition,
                               const Shape &shape,
                               const ViewportTransform &transform,
                               const QSize &viewportSize) const;
    bool makeCircularArcGeometry(const QPointF &startWorld,
                                 const QPointF &endWorld,
                                 const QPointF &throughWorld,
                                 const ViewportTransform &transform,
                                 const QSize &viewportSize,
                                 QPointF *center,
                                 qreal *radius,
                                 qreal *startAngle,
                                 qreal *sweepAngle) const;
    bool arcAngleIsOnSweep(qreal startAngle,
                           qreal sweepAngle,
                           qreal angle) const;
};

} // namespace classiCAD
