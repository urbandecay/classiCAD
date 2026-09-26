#pragma once

#include "core/document/object_id.h"
#include "core/model.h"
#include "services/hit_testing/curve_hit_tester.h"
#include "services/viewport/viewport_transform.h"

#include <QPainter>
#include <QSize>

namespace classiCAD {

class ViewportRenderer {
public:
    ViewportRenderer(const ViewportTransform &transform,
                     const CurveHitTester &curveHitTester);

    void drawGrid(QPainter &painter, const QSize &viewportSize) const;
    void drawOrigin(QPainter &painter, const QSize &viewportSize) const;
    void drawShape(QPainter &painter,
                   const Shape &shape,
                   const QSize &viewportSize,
                   bool preview,
                   bool selected = false,
                   bool drawPreviewPoints = true) const;
    void setSmoothCurveDisplay(bool enabled);
    void drawNurbsCurve(QPainter &painter,
                        const Shape::NurbsCurve2D &curve,
                        const QSize &viewportSize) const;
    void drawControlPoints(QPainter &painter,
                           const Shape &shape,
                           const QSize &viewportSize,
                           ObjectId shapeObjectId,
                           ObjectId selectedObjectId,
                           bool draggingControlPoint,
                           int activeControlPointIndex) const;
    void drawSubdivisionPoints(QPainter &painter,
                               const Shape &shape,
                               const QVector<double> &parameters,
                               const QSize &viewportSize,
                               bool preview) const;

    void drawCircularArc(QPainter &painter,
                         const QPointF &start,
                         const QPointF &end,
                         const QPointF &through,
                         const QSize &viewportSize) const;
    void drawCenterArcWithSweep(QPainter &painter,
                                const QPointF &centerWorld,
                                const QPointF &startWorld,
                                qreal sweepAngle,
                                const QSize &viewportSize) const;

private:
    QPointF worldToScreen(const QPointF &world,
                          const QSize &viewportSize) const;
    QPointF screenToWorld(const QPointF &screen,
                          const QSize &viewportSize) const;
    bool isValidNurbsCurve(const Shape::NurbsCurve2D &curve) const;
    bool evaluateNurbsPoint(const Shape::NurbsCurve2D &curve,
                            qreal parameter,
                            QPointF *point) const;
    bool subdivisionCurve(const Shape &shape,
                          Shape::NurbsCurve2D *curve) const;
    QVector<QPointF> rectangleVertices(const Shape &shape) const;
    bool makeCircularArcGeometry(const QPointF &startWorld,
                                 const QPointF &endWorld,
                                 const QPointF &throughWorld,
                                 const QSize &viewportSize,
                                 QPointF *center,
                                 qreal *radius,
                                 qreal *startAngle,
                                 qreal *sweepAngle) const;
    void drawCenterArc(QPainter &painter,
                       const QPointF &centerWorld,
                       const QPointF &startWorld,
                       const QPointF &endWorld,
                       const QSize &viewportSize) const;

    const ViewportTransform &transform_;
    const CurveHitTester &curveHitTester_;
    bool smoothCurveDisplay_ = true;
};

} // namespace classiCAD
