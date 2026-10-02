#include "dimension_association.h"

#include "core/geometry/curve_evaluator.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace classiCAD {
namespace {

constexpr qreal kCaptureRadiusPixels = 10.0;
constexpr qreal kGeometryEpsilon = 1.0e-12;

qreal squaredDistance(const QPointF &first, const QPointF &second)
{
    const QPointF delta = first - second;
    return QPointF::dotProduct(delta, delta);
}

bool shapeCenter(const Shape &shape, QPointF *center)
{
    if (center == nullptr) {
        return false;
    }
    if (shape.geometryType == GeometryType::Ellipse &&
        validateNurbsCurve(shape.nurbs) && shape.nurbs.controlPoints.size() >= 5) {
        *center = (shape.nurbs.controlPoints[0] + shape.nurbs.controlPoints[4]) * 0.5;
        return true;
    }
    if ((shape.geometryType == GeometryType::Circle ||
         shape.geometryType == GeometryType::Ellipse ||
         (shape.geometryType == GeometryType::Arc &&
          shape.arcMode == ArcMode::OnePoint)) &&
        !shape.points.isEmpty()) {
        *center = shape.points.first();
        return true;
    }
    if (shape.geometryType != GeometryType::Arc || shape.points.size() < 3) {
        return false;
    }

    if (shape.arcMode != ArcMode::OnePoint) {
        const QPointF &first = shape.points[0];
        const QPointF &second = shape.points[1];
        const QPointF &third = shape.points[2];
        const qreal denominator = 2.0 *
            (first.x() * (second.y() - third.y()) +
             second.x() * (third.y() - first.y()) +
             third.x() * (first.y() - second.y()));
        if (std::abs(denominator) <= kGeometryEpsilon) {
            return false;
        }
        const qreal firstSquared = QPointF::dotProduct(first, first);
        const qreal secondSquared = QPointF::dotProduct(second, second);
        const qreal thirdSquared = QPointF::dotProduct(third, third);
        *center = QPointF(
            (firstSquared * (second.y() - third.y()) +
             secondSquared * (third.y() - first.y()) +
             thirdSquared * (first.y() - second.y())) / denominator,
            (firstSquared * (third.x() - second.x()) +
             secondSquared * (first.x() - third.x()) +
             thirdSquared * (second.x() - first.x())) / denominator);
        return true;
    }
    return false;
}

bool closestCurveParameter(const Shape::NurbsCurve2D &curve,
                           const QPointF &targetScreen,
                           const CurveSampler &sampler,
                           const ViewportTransform &transform,
                           const QSize &viewportSize,
                           qreal *fraction,
                           qreal *screenDistance)
{
    if (fraction == nullptr || screenDistance == nullptr) {
        return false;
    }
    SampledNurbsCurve2D sampled;
    if (!sampler.sampleNurbsCurve(curve, transform, viewportSize, &sampled) ||
        sampled.parameters.size() < 2) {
        return false;
    }

    qreal closestSquared = std::numeric_limits<qreal>::infinity();
    int closestSegment = -1;
    qreal closestSegmentFraction = 0.0;
    for (int index = 0; index + 1 < sampled.screenPoints.size(); ++index) {
        const QPointF start = sampled.screenPoints[index];
        const QPointF delta = sampled.screenPoints[index + 1] - start;
        const qreal deltaSquared = QPointF::dotProduct(delta, delta);
        const qreal segmentFraction = deltaSquared > kGeometryEpsilon
                                          ? std::clamp(
                                                QPointF::dotProduct(targetScreen - start,
                                                                    delta) /
                                                    deltaSquared,
                                                0.0,
                                                1.0)
                                          : 0.0;
        const QPointF projected = start + delta * segmentFraction;
        const qreal distanceSquared = squaredDistance(targetScreen, projected);
        if (distanceSquared < closestSquared) {
            closestSquared = distanceSquared;
            closestSegment = index;
            closestSegmentFraction = segmentFraction;
        }
    }
    if (closestSegment < 0) {
        return false;
    }

    qreal firstParameter = sampled.parameters[closestSegment];
    qreal secondParameter = sampled.parameters[closestSegment + 1];
    qreal parameter = firstParameter +
                      (secondParameter - firstParameter) * closestSegmentFraction;
    // Refine the polyline estimate against the actual NURBS curve, so curved
    // and rational geometry gets a stable associative location.
    qreal low = firstParameter;
    qreal high = secondParameter;
    for (int iteration = 0; iteration < 18; ++iteration) {
        const qreal left = low + (high - low) / 3.0;
        const qreal right = high - (high - low) / 3.0;
        QPointF leftWorld;
        QPointF rightWorld;
        if (!evaluateNurbsPoint(curve, left, &leftWorld) ||
            !evaluateNurbsPoint(curve, right, &rightWorld)) {
            return false;
        }
        if (squaredDistance(transform.worldToScreen(leftWorld, viewportSize),
                            targetScreen) <=
            squaredDistance(transform.worldToScreen(rightWorld, viewportSize),
                            targetScreen)) {
            high = right;
        } else {
            low = left;
        }
    }
    parameter = (low + high) * 0.5;

    qreal domainStart = 0.0;
    qreal domainEnd = 0.0;
    if (!nurbsParameterDomain(curve, &domainStart, &domainEnd)) {
        return false;
    }
    QPointF evaluatedWorld;
    if (!evaluateNurbsPoint(curve, parameter, &evaluatedWorld)) {
        return false;
    }
    *fraction = std::clamp((parameter - domainStart) / (domainEnd - domainStart),
                           0.0,
                           1.0);
    *screenDistance = std::sqrt(squaredDistance(
        transform.worldToScreen(evaluatedWorld, viewportSize), targetScreen));
    return true;
}

DimensionAnchorReference capturePointReference(ObjectId objectId,
                                                 DimensionAnchorKind kind,
                                                 int pointIndex)
{
    DimensionAnchorReference reference;
    reference.objectId = objectId;
    reference.kind = kind;
    reference.pointIndex = pointIndex;
    return reference;
}

} // namespace

DimensionAnchorReference captureDimensionAnchor(
    const Document &document,
    const QPointF &point,
    SnapType snapType,
    const CurveSampler &curveSampler,
    const ViewportTransform &transform,
    const QSize &viewportSize)
{
    DimensionAnchorReference none;
    const QPointF targetScreen = transform.worldToScreen(point, viewportSize);
    const qreal maximumDistanceSquared = kCaptureRadiusPixels *
                                        kCaptureRadiusPixels;

    if (snapType == SnapType::Center) {
        qreal bestDistanceSquared = maximumDistanceSquared;
        DimensionAnchorReference best;
        for (int index = 0; index < document.size(); ++index) {
            const ObjectId objectId = document.objectIdAt(index);
            const Shape &shape = document[index];
            if (isDimensionGeometryType(shape.geometryType) ||
                !document.isObjectVisible(objectId) ||
                !workPlaneMatches(shapeWorkPlaneFrame(shape),
                                  transform.workPlaneFrame())) {
                continue;
            }
            QPointF center;
            if (!shapeCenter(shape, &center)) {
                continue;
            }
            const qreal distanceSquared = squaredDistance(
                targetScreen, transform.worldToScreen(center, viewportSize));
            if (distanceSquared <= bestDistanceSquared) {
                bestDistanceSquared = distanceSquared;
                best = capturePointReference(objectId, DimensionAnchorKind::Center, -1);
            }
        }
        if (best.objectId.isValid()) {
            return best;
        }
    }

    if (snapType == SnapType::ControlPoint) {
        qreal bestDistanceSquared = maximumDistanceSquared;
        DimensionAnchorReference best;
        for (int index = 0; index < document.size(); ++index) {
            const ObjectId objectId = document.objectIdAt(index);
            const Shape &shape = document[index];
            if (isDimensionGeometryType(shape.geometryType) ||
                !document.isObjectVisible(objectId) ||
                !workPlaneMatches(shapeWorkPlaneFrame(shape),
                                  transform.workPlaneFrame())) {
                continue;
            }
            const auto considerCurve = [&](const Shape::NurbsCurve2D &curve,
                                           int componentIndex) {
                for (int pointIndex = 0;
                     pointIndex < curve.controlPoints.size();
                     ++pointIndex) {
                    const qreal distanceSquared = squaredDistance(
                        targetScreen,
                        transform.worldToScreen(curve.controlPoints[pointIndex],
                                                viewportSize));
                    if (distanceSquared <= bestDistanceSquared) {
                        bestDistanceSquared = distanceSquared;
                        best = capturePointReference(objectId,
                                                     DimensionAnchorKind::ControlPoint,
                                                     pointIndex);
                        best.componentIndex = componentIndex;
                    }
                }
            };
            if (shape.geometryType == GeometryType::PolyCurve) {
                for (int component = 0; component < shape.components.size(); ++component) {
                    considerCurve(shape.components[component], component);
                }
            } else if (validateNurbsCurve(shape.nurbs)) {
                considerCurve(shape.nurbs, -1);
            }
        }
        if (best.objectId.isValid()) {
            return best;
        }
    }

    if (snapType == SnapType::Endpoint || snapType == SnapType::ControlPoint ||
        snapType == SnapType::None) {
        qreal bestDistanceSquared = maximumDistanceSquared;
        DimensionAnchorReference best;
        for (int index = 0; index < document.size(); ++index) {
            const ObjectId objectId = document.objectIdAt(index);
            const Shape &shape = document[index];
            if (shape.geometryType != GeometryType::Point || shape.points.isEmpty() ||
                !document.isObjectVisible(objectId) ||
                !workPlaneMatches(shapeWorkPlaneFrame(shape),
                                  transform.workPlaneFrame())) {
                continue;
            }
            const qreal distanceSquared = squaredDistance(
                targetScreen, transform.worldToScreen(shape.points.first(), viewportSize));
            if (distanceSquared <= bestDistanceSquared) {
                bestDistanceSquared = distanceSquared;
                best = capturePointReference(objectId, DimensionAnchorKind::ShapePoint, 0);
            }
        }
        if (best.objectId.isValid()) {
            return best;
        }
    }

    qreal bestDistance = kCaptureRadiusPixels;
    DimensionAnchorReference best;
    for (int shapeIndex = 0; shapeIndex < document.size(); ++shapeIndex) {
        const ObjectId objectId = document.objectIdAt(shapeIndex);
        const Shape &shape = document[shapeIndex];
        if (isDimensionGeometryType(shape.geometryType) ||
            !document.isObjectVisible(objectId) ||
            !workPlaneMatches(shapeWorkPlaneFrame(shape),
                              transform.workPlaneFrame())) {
            continue;
        }
        const QVector<Shape::NurbsCurve2D> curves = curveSampler.curvesForShape(shape);
        for (int componentIndex = 0; componentIndex < curves.size(); ++componentIndex) {
            qreal parameterFraction = 0.0;
            qreal distance = 0.0;
            if (!closestCurveParameter(curves[componentIndex],
                                       targetScreen,
                                       curveSampler,
                                       transform,
                                       viewportSize,
                                       &parameterFraction,
                                       &distance) ||
                distance > bestDistance) {
                continue;
            }
            bestDistance = distance;
            best.objectId = objectId;
            best.kind = DimensionAnchorKind::CurveParameter;
            best.componentIndex = shape.geometryType == GeometryType::PolyCurve ||
                                          shape.geometryType == GeometryType::Rectangle ||
                                          shape.geometryType == GeometryType::Polygon
                                      ? componentIndex
                                      : -1;
            best.parameterFraction = parameterFraction;
        }
    }
    return best.objectId.isValid() ? best : none;
}

bool resolveDimensionAnchor(const Document &document,
                            const DimensionAnchorReference &anchor,
                            const CurveSampler &curveSampler,
                            QPointF *point)
{
    if (point == nullptr || !anchor.objectId.isValid()) {
        return false;
    }
    const Shape *shape = document.shape(anchor.objectId);
    if (shape == nullptr || isDimensionGeometryType(shape->geometryType)) {
        return false;
    }

    switch (anchor.kind) {
    case DimensionAnchorKind::None:
        return false;
    case DimensionAnchorKind::CurveParameter: {
        const QVector<Shape::NurbsCurve2D> curves =
            curveSampler.curvesForShape(*shape);
        const int curveIndex = shape->geometryType == GeometryType::PolyCurve ||
                                       shape->geometryType == GeometryType::Rectangle ||
                                       shape->geometryType == GeometryType::Polygon
                                   ? anchor.componentIndex
                                   : 0;
        if (curveIndex < 0 || curveIndex >= curves.size()) {
            return false;
        }
        const Shape::NurbsCurve2D &curve = curves[curveIndex];
        qreal domainStart = 0.0;
        qreal domainEnd = 0.0;
        if (!nurbsParameterDomain(curve, &domainStart, &domainEnd)) {
            return false;
        }
        return evaluateNurbsPoint(curve,
                                  domainStart +
                                      (domainEnd - domainStart) *
                                          anchor.parameterFraction,
                                  point);
    }
    case DimensionAnchorKind::ControlPoint: {
        const Shape::NurbsCurve2D *curve = nullptr;
        if (shape->geometryType == GeometryType::PolyCurve) {
            if (anchor.componentIndex >= 0 &&
                anchor.componentIndex < shape->components.size()) {
                curve = &shape->components[anchor.componentIndex];
            }
        } else if (validateNurbsCurve(shape->nurbs)) {
            curve = &shape->nurbs;
        }
        if (curve == nullptr || anchor.pointIndex < 0 ||
            anchor.pointIndex >= curve->controlPoints.size()) {
            return false;
        }
        *point = curve->controlPoints[anchor.pointIndex];
        return true;
    }
    case DimensionAnchorKind::ShapePoint:
        if (anchor.pointIndex < 0 || anchor.pointIndex >= shape->points.size()) {
            return false;
        }
        *point = shape->points[anchor.pointIndex];
        return true;
    case DimensionAnchorKind::Center:
        return shapeCenter(*shape, point);
    }
    return false;
}

void updateAssociativeDimensions(Document &document,
                                 const CurveSampler &curveSampler)
{
    for (int index = 0; index < document.size(); ++index) {
        Shape &dimension = document[index];
        if (!isDimensionGeometryType(dimension.geometryType) ||
            dimension.dimensionAnchors.isEmpty()) {
            continue;
        }
        for (int anchorIndex = 0;
             anchorIndex < dimension.dimensionAnchors.size() &&
             anchorIndex < dimension.points.size();
             ++anchorIndex) {
            QPointF resolvedPoint;
            if (resolveDimensionAnchor(document,
                                       dimension.dimensionAnchors[anchorIndex],
                                       curveSampler,
                                       &resolvedPoint)) {
                dimension.points[anchorIndex] = resolvedPoint;
            }
        }
        if (dimension.geometryType == GeometryType::LinearDimension &&
            dimension.dimensionOffsetValid && dimension.points.size() >= 3) {
            const QPointF delta = dimension.points[1] - dimension.points[0];
            const qreal length = std::hypot(delta.x(), delta.y());
            if (length > kGeometryEpsilon) {
                const QPointF normal(-delta.y() / length, delta.x() / length);
                const QPointF midpoint =
                    (dimension.points[0] + dimension.points[1]) * 0.5;
                dimension.points[2] = midpoint + normal * dimension.dimensionOffset;
            }
        }
    }
}

} // namespace classiCAD
