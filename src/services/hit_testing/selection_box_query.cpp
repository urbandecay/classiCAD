#include "selection_box_query.h"

#include "core/geometry/shape_mapping.h"
#include "core/geometry/nurbs_surface.h"
#include "services/hit_testing/curve_hit_tester.h"
#include "services/hit_testing/projected_curve_bounds.h"
#include "services/sampling/curve_sampler.h"
#include "services/sampling/curve_sample_data.h"
#include "services/viewport/viewport_transform.h"

#include <algorithm>
#include <cmath>

namespace classiCAD {

namespace {

bool pointInside(const QRectF &rect, const QPointF &point)
{
    return rect.left() <= point.x() && point.x() <= rect.right() &&
           rect.top() <= point.y() && point.y() <= rect.bottom();
}

bool segmentIntersectsRect(const QRectF &rect,
                           const QPointF &start,
                           const QPointF &end)
{
    // Clipped samples have NaN coordinates. Reject them before Liang-Barsky
    // clipping so comparisons do not fall through and report a false hit.
    if (!std::isfinite(start.x()) || !std::isfinite(start.y()) ||
        !std::isfinite(end.x()) || !std::isfinite(end.y())) {
        return false;
    }

    const qreal dx = end.x() - start.x();
    const qreal dy = end.y() - start.y();
    const qreal p[] = {-dx, dx, -dy, dy};
    const qreal q[] = {start.x() - rect.left(),
                       rect.right() - start.x(),
                       start.y() - rect.top(),
                       rect.bottom() - start.y()};
    qreal firstFraction = 0.0;
    qreal lastFraction = 1.0;
    for (int edge = 0; edge < 4; ++edge) {
        if (std::abs(p[edge]) <= 1.0e-12) {
            if (q[edge] < 0.0) {
                return false;
            }
            continue;
        }
        const qreal fraction = q[edge] / p[edge];
        if (p[edge] < 0.0) {
            firstFraction = std::max(firstFraction, fraction);
        } else {
            lastFraction = std::min(lastFraction, fraction);
        }
        if (firstFraction > lastFraction) {
            return false;
        }
    }
    return true;
}

} // namespace

ProjectedShapeBoundsResult queryProjectedShapeBounds(
    const Shape &shape,
    const CurveHitTester &curveHitTester,
    const ViewportTransform &viewportTransform,
    const QSize &viewportSize,
    const Point3D &worldOffset)
{
    qreal minX = 0.0;
    qreal maxX = 0.0;
    qreal minY = 0.0;
    qreal maxY = 0.0;
    bool initialized = false;
    const auto includeWorldPoint = [&](const Point3D &worldPoint) {
        QPointF screenPoint;
        if (!viewportTransform.worldPointToScreen(worldPoint,
                                                  viewportSize,
                                                  &screenPoint)) {
            return;
        }
        if (!initialized) {
            minX = maxX = screenPoint.x();
            minY = maxY = screenPoint.y();
            initialized = true;
        } else {
            minX = std::min(minX, screenPoint.x());
            maxX = std::max(maxX, screenPoint.x());
            minY = std::min(minY, screenPoint.y());
            maxY = std::max(maxY, screenPoint.y());
        }
    };

    const auto includeCurveControlHull = [&](const NurbsCurve3D &curve,
                                             WorkPlaneFrame frame) {
        if (!validateNurbsCurve(curve)) {
            return;
        }
        frame.origin.x += worldOffset.x;
        frame.origin.y += worldOffset.y;
        frame.origin.z += worldOffset.z;
        for (int index = 0; index < curve.controlPoints.size(); ++index) {
            includeWorldPoint(workPlaneFramePointToWorld(
                curve.controlPoints[index],
                curve.dimension == 3 ? curve.normalCoordinates[index] : 0.0,
                frame));
        }
    };
    if (shape.geometryType == GeometryType::PolyCurve) {
        for (int componentIndex = 0;
             componentIndex < shape.components.size(); ++componentIndex) {
            includeCurveControlHull(
                shape.components[componentIndex],
                shapeComponentWorkPlaneFrame(shape, componentIndex));
        }
    } else if (validateNurbsCurve(shape.nurbs)) {
        includeCurveControlHull(shape.nurbs, shapeWorkPlaneFrame(shape));
    } else {
        WorkPlaneFrame frame = shapeWorkPlaneFrame(shape);
        frame.origin.x += worldOffset.x;
        frame.origin.y += worldOffset.y;
        frame.origin.z += worldOffset.z;
        QVector<QPointF> points = curveHitTester.controlPointsForShape(shape);
        for (const QPointF &point : shape.points) {
            if (!points.contains(point)) {
                points.append(point);
            }
        }
        for (const QPointF &point : points) {
            if (std::isfinite(point.x()) && std::isfinite(point.y())) {
                includeWorldPoint(workPlaneFramePointToWorld(point, frame));
            }
        }
    }

    for (const auto &face : shapeSurfaceFaces(shape)) {
        for (const Point3D &point : face.controlPoints) {
            includeWorldPoint({point.x + worldOffset.x,
                               point.y + worldOffset.y,
                               point.z + worldOffset.z});
        }
    }

    if (!initialized) {
        return {};
    }

    // Selection-window bounds must be the exact projected control hull with
    // no padding, so full containment at the edge remains reliable.
    return {QRectF(QPointF(minX, minY), QPointF(maxX, maxY)), true};
}

SelectionBoxGeometryResult queryCurveOrPointSelectionBox(
    const Shape &shape,
    const QRectF &box,
    bool crossingSelection,
    const CurveSampler &curveSampler,
    const ViewportTransform &viewportTransform,
    const QSize &viewportSize,
    const Point3D &worldOffset)
{
    const QRectF selectionRect = box.normalized();
    constexpr qreal crossingTolerancePixels = 2.0;
    const QRectF hitRect = crossingSelection
                               ? selectionRect.adjusted(-crossingTolerancePixels,
                                                        -crossingTolerancePixels,
                                                        crossingTolerancePixels,
                                                        crossingTolerancePixels)
                               : selectionRect;

    const QVector<Shape::NurbsCurve2D> curves =
        curveSampler.curvesForShape(shape);
    if (!curves.isEmpty()) {
        bool sampledGeometry = false;
        bool intersects = false;
        for (int componentIndex = 0; componentIndex < curves.size();
             ++componentIndex) {
            const Shape::NurbsCurve2D &curve = curves[componentIndex];
            const WorkPlaneFrame frame =
                shape.geometryType == GeometryType::PolyCurve
                    ? shapeComponentWorkPlaneFrame(shape, componentIndex)
                    : shapeWorkPlaneFrame(shape);
            QRectF controlHullBounds;
            if (projectedNurbsControlHullBounds(curve,
                                                frame,
                                                viewportTransform,
                                                viewportSize,
                                                &controlHullBounds,
                                                worldOffset) &&
                !screenBoundsOverlap(controlHullBounds, hitRect)) {
                if (!crossingSelection) {
                    return {true, false};
                }
                continue;
            }
            SampledNurbsCurve2D sampled;
            if (!curveSampler.sampleNurbsCurve(curve,
                                                frame,
                                                viewportTransform,
                                                viewportSize,
                                                &sampled,
                                                worldOffset)) {
                return {true, false};
            }

            sampledGeometry = true;
            for (const QPointF &point : sampled.screenPoints) {
                if (!crossingSelection && !pointInside(hitRect, point)) {
                    return {true, false};
                }
                if (crossingSelection && pointInside(hitRect, point)) {
                    intersects = true;
                }
            }
            if (crossingSelection) {
                for (int index = 1; index < sampled.screenPoints.size(); ++index) {
                    if (segmentIntersectsRect(hitRect,
                                              sampled.screenPoints[index - 1],
                                              sampled.screenPoints[index])) {
                        intersects = true;
                        break;
                    }
                }
            }
        }
        return {true, sampledGeometry &&
                          (crossingSelection ? intersects : true)};
    }

    if (shape.geometryType == GeometryType::Point && !shape.points.isEmpty()) {
        QPointF screenPoint;
        const WorkPlaneFrame frame = shapeWorkPlaneFrame(shape);
        const bool projects = viewportTransform.worldPointToScreen(
            [&]() {
                Point3D world = workPlaneFramePointToWorld(shape.points.first(), frame);
                world.x += worldOffset.x;
                world.y += worldOffset.y;
                world.z += worldOffset.z;
                return world;
            }(),
            viewportSize,
            &screenPoint);
        return {true, projects && pointInside(hitRect, screenPoint)};
    }

    return {};
}

} // namespace classiCAD
