#pragma once

#include "services/viewport/viewport_transform.h"

#include "core/document/document.h"
#include "core/model.h"

namespace classiCAD {

struct SnapSettings {
    bool enabled = false;
    bool endpoint = true;
    bool midpoint = true;
    bool intersection = true;
    bool center = true;
    bool perpendicular = false;
    bool tangent = false;
};

class SnapEngine final {
public:
    void setSettings(const SnapSettings &settings);
    const SnapSettings &settings() const;

    QVector<SnapCandidate> snapCandidatesForShape(
        const Shape &shape,
        const ViewportTransform &transform,
        const QSize &viewportSize) const;
    QVector<SnapCandidate> snapCandidatesForScene(
        const Document &document,
        const QVector<int> &excludedShapeIndices,
        const ViewportTransform &transform,
        const QSize &viewportSize) const;

    QVector<SnapCandidate> perpendicularCandidates(
        const Document &document,
        const QPointF &origin,
        const QPointF &cursor,
        const ViewportTransform &transform,
        const QSize &viewportSize,
        const QVector<int> &excludedShapeIndices = {}) const;
    QVector<SnapCandidate> tangentCandidates(
        const Document &document,
        const QPointF &origin,
        const ViewportTransform &transform,
        const QSize &viewportSize,
        const QVector<int> &excludedShapeIndices = {}) const;

    SnapResult findSnapPoint(const Document &document,
                             const QPointF &rawPoint,
                             bool drawingSnapActive,
                             const QVector<QPointF> &pendingPoints,
                             const ViewportTransform &transform,
                             const QSize &viewportSize,
                             const QVector<int> &excludedShapeIndices = {},
                             bool forceEnabled = false) const;

    DragSnapResult findDragSnap(const Document &document,
                                const QVector<int> &selectedShapeIndices,
                                const ViewportTransform &transform,
                                const QSize &viewportSize) const;
    DragSnapResult findControlPointSnap(
        const Document &document,
        int selectedShapeIndex,
        int selectedControlPointIndex,
        const QPointF &controlPoint,
        const ViewportTransform &transform,
        const QSize &viewportSize) const;

private:
    QVector<QPointF> rectangleVertices(const Shape &shape) const;
    bool subdivisionCurve(const Shape &shape, Shape::NurbsCurve2D *curve) const;
    bool nurbsCurveEndpoints(const Shape::NurbsCurve2D &curve,
                             QPointF *start,
                             QPointF *end) const;
    bool nurbsCurvePointAtFraction(const Shape::NurbsCurve2D &curve,
                                   qreal fraction,
                                   QPointF *point) const;
    bool makeCircularArcGeometry(const QPointF &startWorld,
                                 const QPointF &endWorld,
                                 const QPointF &throughWorld,
                                 const ViewportTransform &transform,
                                 const QSize &viewportSize,
                                 QPointF *center,
                                 qreal *radius,
                                 qreal *startAngle,
                                 qreal *sweepAngle) const;
    bool makeArcSnapGeometry(const Shape &shape,
                             const ViewportTransform &transform,
                             const QSize &viewportSize,
                             QPointF *centerScreen,
                             qreal *radius,
                             qreal *startAngle,
                             qreal *sweepAngle) const;
    bool arcAngleIsOnSweep(qreal startAngle,
                           qreal sweepAngle,
                           qreal angle) const;
    bool arcSnapPointAtFraction(const Shape &shape,
                                qreal fraction,
                                const ViewportTransform &transform,
                                const QSize &viewportSize,
                                QPointF *point) const;

    SnapSettings settings_;
};

} // namespace classiCAD
