#pragma once

#include "core/document/object_id.h"
#include "core/document/shape.h"
#include "services/sampling/surface_tessellation_cache.h"

#include <QSize>

namespace classiCAD {

class Document;
class ViewportTransform;

class CurveHitTester final {
public:
    explicit CurveHitTester(
        const SurfaceTessellationCache *surfaceTessellationCache = nullptr);

    void setArchitecturalDimensionFont(bool enabled);
    void setSurfaceTessellationCache(
        const SurfaceTessellationCache *surfaceTessellationCache);

    qreal distanceToSegment(const QPointF &point,
                            const QPointF &start,
                            const QPointF &end) const;

    qreal distanceToShape(const QPointF &screenPosition,
                          const Shape &shape,
                          const ViewportTransform &transform,
                          const QSize &viewportSize,
                          ObjectId objectId = ObjectId::invalid(),
                          quint64 geometryRevision = 0,
                          const Point3D &worldOffset = {}) const;

    qreal distanceToNurbsCurve(const QPointF &screenPosition,
                               const Shape::NurbsCurve2D &curve,
                               const ViewportTransform &transform,
                               const QSize &viewportSize) const;
    qreal distanceToNurbsSurface(const QPointF &screenPosition,
                                 const Shape::NurbsSurface3D &surface,
                                 const ViewportTransform &transform,
                                 const QSize &viewportSize,
                                 ObjectId objectId = ObjectId::invalid(),
                                 quint64 geometryRevision = 0,
                                 int faceIndex = 0,
                                 const Point3D &worldOffset = {}) const;

    int hitTestShape(const Document &document,
                     const QPointF &screenPosition,
                     const ViewportTransform &transform,
                     const QSize &viewportSize,
                     bool editableOnly = false) const;

    // Drawing-plane inference can inspect every visible planar object while
    // ordinary selection remains scoped to the active plane.
    int hitTestShapeOnAnyWorkPlane(const Document &document,
                                   const QPointF &screenPosition,
                                   const ViewportTransform &transform,
                                   const QSize &viewportSize) const;

    bool hitTestVisibleDepth(const Document &document,
                             const QPointF &screenPosition,
                             const ViewportTransform &transform,
                             const QSize &viewportSize,
                             Point3D *worldPoint) const;

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

    const SurfaceTessellationCache *surfaceTessellationCache_ = nullptr;
    bool architecturalDimensionFont_ = false;
};

} // namespace classiCAD
