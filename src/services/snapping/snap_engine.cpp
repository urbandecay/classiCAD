#include "snap_engine.h"

#include "core/document/document.h"
#include "core/geometry/arc_curve_factory.h"
#include "core/geometry/curve_construction.h"
#include "core/geometry/curve_evaluator.h"
#include "core/geometry/curve_geometry_data.h"
#include "core/geometry/planar_geometry.h"
#include "core/geometry/shape_mapping.h"
#include "services/hit_testing/projected_curve_bounds.h"
#include "services/viewport/viewport_transform.h"
#include <QDataStream>
#include <QHash>
#include <QIODevice>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace classiCAD {
namespace {

// Retain insertion order so equal-distance snaps resolve exactly as before.
class ScreenCandidateIndex {
public:
    void append(const QPointF &point, int index)
    {
        if (!std::isfinite(point.x()) || !std::isfinite(point.y())) return;
        all_.append(index);
        if (!indexable(point)) {
            distant_.append(index);
            return;
        }
        cells_[cell(point)].append(index);
    }
    QVector<int> nearby(const QPointF &point) const
    {
        if (!indexable(point)) return all_;
        QVector<int> result = distant_;
        const auto center = cell(point);
        for (qint64 x = center.first - 1; x <= center.first + 1; ++x) {
            for (qint64 y = center.second - 1; y <= center.second + 1; ++y) {
                const auto found = cells_.constFind(qMakePair(x, y));
                if (found != cells_.cend()) result += *found;
            }
        }
        std::sort(result.begin(), result.end());
        return result;
    }
private:
    static bool indexable(const QPointF &point)
    {
        return std::isfinite(point.x()) && std::isfinite(point.y()) &&
               std::abs(point.x()) < 1.0e15 && std::abs(point.y()) < 1.0e15;
    }
    static QPair<qint64, qint64> cell(const QPointF &point)
    {
        return {qint64(std::floor(point.x() / 12.0)),
                qint64(std::floor(point.y() / 12.0))};
    }
    QHash<QPair<qint64, qint64>, QVector<int>> cells_;
    QVector<int> all_;
    QVector<int> distant_;
};

QPointF ellipseCenter(const Shape &shape)
{
    if (validateNurbsCurve(shape.nurbs) && shape.nurbs.controlPoints.size() >= 5) {
        return (shape.nurbs.controlPoints[0] + shape.nurbs.controlPoints[4]) * 0.5;
    }
    return shape.points.isEmpty() ? QPointF{} : shape.points.first();
}

bool workPlaneFramesAreCoplanar(const WorkPlaneFrame &first,
                                const WorkPlaneFrame &second)
{
    if (!isValidWorkPlaneFrame(first) || !isValidWorkPlaneFrame(second)) {
        return false;
    }

    constexpr qreal angularTolerance = 1.0e-8;
    if (std::abs(std::abs(first.normal.x * second.normal.x +
                          first.normal.y * second.normal.y +
                          first.normal.z * second.normal.z) - 1.0) >
        angularTolerance) {
        return false;
    }

    const qreal coordinateScale = std::max<qreal>(
        {1.0, std::abs(first.origin.x), std::abs(first.origin.y),
         std::abs(first.origin.z), std::abs(second.origin.x),
         std::abs(second.origin.y), std::abs(second.origin.z)});
    const qreal distanceTolerance = std::max<qreal>(
        1.0e-7,
        std::numeric_limits<qreal>::epsilon() * coordinateScale * 64.0);
    return std::abs(signedDistanceFromWorkPlaneFrame(first.origin, second)) <=
           distanceTolerance;
}

QPointF mapPointBetweenFrames(const QPointF &point,
                              const WorkPlaneFrame &source,
                              const WorkPlaneFrame &destination)
{
    return worldPointToWorkPlaneFrame(
        workPlaneFramePointToWorld(point, source), destination);
}

Point3D candidateWorldPoint(const SnapCandidate &candidate,
                            const WorkPlaneFrame &fallbackFrame)
{
    return candidate.hasWorldPoint
               ? candidate.worldPoint
               : workPlaneFramePointToWorld(candidate.point, fallbackFrame);
}

qreal dotPoint3D(const Point3D &first, const Point3D &second)
{
    return first.x * second.x + first.y * second.y + first.z * second.z;
}

Point3D subtractPoint3D(const Point3D &first, const Point3D &second)
{
    return {first.x - second.x, first.y - second.y, first.z - second.z};
}

QVector<Point3D> nurbsSurfaceCorners(const NurbsSurface3D &surface,
                                     const Point3D &worldOffset = {})
{
    if (!surface.trimLoops.isEmpty()) {
        const NurbsSurfaceTrimLoop &outerLoop = surface.trimLoops.first();
        const QVector<double> knots = expandedNurbsKnotVector(outerLoop.curve);
        QVector<Point3D> trimVertices;
        qreal curveStart = 0.0;
        qreal curveEnd = 0.0;
        if (!nurbsParameterDomain(outerLoop.curve, &curveStart, &curveEnd)) {
            return {};
        }
        qreal previousParameter = std::numeric_limits<qreal>::quiet_NaN();
        for (int knotIndex = outerLoop.curve.degree;
             knotIndex <= outerLoop.curve.controlPoints.size();
             ++knotIndex) {
            const qreal parameter = knots[knotIndex];
            if (parameter < curveStart || parameter > curveEnd ||
                (std::isfinite(previousParameter) &&
                 std::abs(parameter - previousParameter) <= 1.0e-12)) {
                continue;
            }
            previousParameter = parameter;
            QPointF uv;
            Point3D point;
            if (evaluateNurbsPoint(outerLoop.curve, parameter, &uv) &&
                evaluateNurbsSurfacePoint(surface, uv.x(), uv.y(), &point)) {
                const Point3D placedPoint{point.x + worldOffset.x,
                                          point.y + worldOffset.y,
                                          point.z + worldOffset.z};
                bool isDuplicate = false;
                for (const Point3D &previous : trimVertices) {
                    const qreal distance = std::hypot(
                        placedPoint.x - previous.x,
                        std::hypot(placedPoint.y - previous.y,
                                   placedPoint.z - previous.z));
                    if (distance <= 1.0e-9) {
                        isDuplicate = true;
                        break;
                    }
                }
                if (isDuplicate) {
                    continue;
                }
                trimVertices.append(placedPoint);
            }
        }
        return trimVertices;
    }

    qreal uStart = 0.0;
    qreal uEnd = 0.0;
    qreal vStart = 0.0;
    qreal vEnd = 0.0;
    if (!nurbsSurfaceParameterDomains(surface, &uStart, &uEnd,
                                      &vStart, &vEnd)) {
        return {};
    }

    QVector<Point3D> corners;
    corners.reserve(4);
    for (const qreal u : {uStart, uEnd}) {
        for (const qreal v : {vStart, vEnd}) {
            Point3D point;
            if (!evaluateNurbsSurfacePoint(surface, u, v, &point)) {
                return {};
            }
            corners.append({point.x + worldOffset.x,
                            point.y + worldOffset.y,
                            point.z + worldOffset.z});
        }
    }
    return corners;
}

void mapCurveBetweenFrames(Shape::NurbsCurve2D *curve,
                           const WorkPlaneFrame &source,
                           const WorkPlaneFrame &destination)
{
    if (curve == nullptr) {
        return;
    }
    for (QPointF &point : curve->controlPoints) {
        point = mapPointBetweenFrames(point, source, destination);
    }
}

Shape mapShapeBetweenFrames(const Shape &shape,
                            const WorkPlaneFrame &destination)
{
    Shape mapped = shape;
    const WorkPlaneFrame source = shapeWorkPlaneFrame(shape);
    for (QPointF &point : mapped.points) {
        point = mapPointBetweenFrames(point, source, destination);
    }
    mapCurveBetweenFrames(&mapped.nurbs, source, destination);
    for (int index = 0; index < mapped.components.size(); ++index) {
        const WorkPlaneFrame componentSource =
            shapeComponentWorkPlaneFrame(shape, index);
        mapCurveBetweenFrames(&mapped.components[index],
                              componentSource,
                              destination);
        if (mapped.componentWorkPlaneFrames.size() == mapped.components.size()) {
            mapped.componentWorkPlaneFrames[index] = destination;
        }
    }
    const qreal normalAlignment = source.normal.x * destination.normal.x +
                                  source.normal.y * destination.normal.y +
                                  source.normal.z * destination.normal.z;
    if (normalAlignment < 0.0) {
        mapped.arcSweep = -mapped.arcSweep;
    }
    mapped.workPlaneFrame = destination;
    return mapped;
}

} // namespace

void SnapEngine::setSettings(const SnapSettings &settings)
{
    settings_ = settings;
}

const SnapSettings &SnapEngine::settings() const
{
    return settings_;
}

void SnapEngine::setOcclusionPlaneQuery(OcclusionPlaneQuery query)
{
    occlusionPlaneQuery_ = std::move(query);
}

SnapEngine::OcclusionPlane SnapEngine::occlusionPlaneAt(
    const QPointF &screenPosition,
    const QVector<int> &excludedShapeIndices,
    OcclusionPlaneCache *cache) const
{
    const QPoint cacheKey(qRound(screenPosition.x()),
                          qRound(screenPosition.y()));
    if (cache != nullptr) {
        const auto cached = cache->constFind(cacheKey);
        if (cached != cache->cend()) {
            return *cached;
        }
    }
    OcclusionPlane plane;
    plane.valid = occlusionPlaneQuery_ &&
                  occlusionPlaneQuery_(screenPosition,
                                       excludedShapeIndices,
                                       &plane.point,
                                       &plane.normal);
    if (cache != nullptr) {
        cache->insert(cacheKey, plane);
    }
    return plane;
}

bool SnapEngine::pointPassesOcclusionPlane(
    const Point3D &worldPoint,
    const OcclusionPlane &plane,
    const ViewportTransform &transform) const
{
    if (!plane.valid) {
        return true;
    }

    const qreal normalLength = std::sqrt(dotPoint3D(plane.normal,
                                                    plane.normal));
    if (!std::isfinite(normalLength) || normalLength <= 1.0e-15) {
        return true;
    }
    Point3D normal{plane.normal.x / normalLength,
                   plane.normal.y / normalLength,
                   plane.normal.z / normalLength};
    const Point3D viewDirection = transform.viewDirection();
    if (dotPoint3D(normal, viewDirection) < 0.0) {
        normal = {-normal.x, -normal.y, -normal.z};
    }

    // Blender clips snap candidates against the visible surface's plane,
    // offset by a depth-scaled epsilon. Keep the same half-space rule while
    // accounting for double precision coordinates in CAD-sized scenes.
    const qreal signedDistance = dotPoint3D(
        subtractPoint3D(worldPoint, plane.point), normal);
    const qreal worldDepth =
        transform.worldDirectionToView(worldPoint).towardCamera;
    const qreal planeDepth =
        transform.worldDirectionToView(plane.point).towardCamera;
    const qreal coordinateScale = std::max<qreal>(
        {1.0, std::abs(worldPoint.x), std::abs(worldPoint.y),
         std::abs(worldPoint.z), std::abs(plane.point.x),
         std::abs(plane.point.y), std::abs(plane.point.z)});
    const qreal tolerance = std::max<qreal>(
        {1.0e-7,
         std::max(std::abs(worldDepth), std::abs(planeDepth)) * 1.0e-5,
         std::numeric_limits<qreal>::epsilon() * coordinateScale * 64.0});
    return std::isfinite(signedDistance) && signedDistance >= -tolerance;
}

QVector<QPointF> SnapEngine::rectangleVertices(const Shape &shape) const
{
    if (shape.geometryType == GeometryType::Rectangle &&
        validateNurbsCurve(shape.nurbs) && shape.nurbs.controlPoints.size() == 5) {
        return shape.nurbs.controlPoints.mid(0, 4);
    }
    if (shape.geometryType != GeometryType::Rectangle || shape.points.size() < 2) {
        return {};
    }
    if (shape.points.size() >= 4) {
        return {shape.points[0], shape.points[1], shape.points[2], shape.points[3]};
    }
    const QPointF first = shape.points[0];
    const QPointF second = shape.points[1];
    return {first,
            QPointF(second.x(), first.y()),
            second,
            QPointF(first.x(), second.y())};
}

bool SnapEngine::subdivisionCurve(const Shape &shape,
                                  Shape::NurbsCurve2D *curve) const
{
    if (curve == nullptr) {
        return false;
    }
    if (validateNurbsCurve(shape.nurbs)) {
        *curve = shape.nurbs;
        return true;
    }
    if (shape.geometryType == GeometryType::Line && shape.points.size() >= 2) {
        *curve = makeDegreeOneNurbs(shape.points);
        return validateNurbsCurve(*curve);
    }
    if ((shape.geometryType == GeometryType::Bezier ||
         shape.geometryType == GeometryType::Nurbs) &&
        shape.points.size() >= 2) {
        *curve = makeBezierNurbs(shape.points);
        return validateNurbsCurve(*curve);
    }
    return false;
}

bool SnapEngine::nurbsCurveEndpoints(const Shape::NurbsCurve2D &curve,
                                     QPointF *start,
                                     QPointF *end) const
{
    if (!validateNurbsCurve(curve) || (start == nullptr && end == nullptr)) {
        return false;
    }
    qreal firstParameter = 0.0;
    qreal lastParameter = 0.0;
    if (!nurbsParameterDomain(curve, &firstParameter, &lastParameter)) {
        return false;
    }
    return (start == nullptr || evaluateNurbsPoint(curve, firstParameter, start)) &&
           (end == nullptr || evaluateNurbsPoint(curve, lastParameter, end));
}

bool SnapEngine::nurbsCurvePointAtFraction(const Shape::NurbsCurve2D &curve,
                                           qreal fraction,
                                           QPointF *point) const
{
    if (point == nullptr || !validateNurbsCurve(curve)) {
        return false;
    }
    qreal firstParameter = 0.0;
    qreal lastParameter = 0.0;
    if (!nurbsParameterDomain(curve, &firstParameter, &lastParameter)) {
        return false;
    }
    return evaluateNurbsPoint(curve,
                              firstParameter +
                                  (lastParameter - firstParameter) *
                                      std::clamp(fraction, 0.0, 1.0),
                              point);
}

bool SnapEngine::nearestPointOnNurbsCurve(const Shape::NurbsCurve2D &curve,
                                          const QPointF &cursorScreen,
                                          const ViewportTransform &transform,
                                          const QSize &viewportSize,
                                          QPointF *nearestPoint,
                                          qreal *distanceSquared) const
{
    if (nearestPoint == nullptr || distanceSquared == nullptr ||
        !validateNurbsCurve(curve)) {
        return false;
    }

    const QVector<double> fullKnots = expandedNurbsKnotVector(curve);
    constexpr int samplesPerSpan = 32;
    qreal bestSegmentDistanceSquared = std::numeric_limits<qreal>::infinity();
    qreal bestParameterLow = 0.0;
    qreal bestParameterHigh = 0.0;
    qreal bestParameter = 0.0;
    bool foundSegment = false;

    for (int spanIndex = curve.degree;
         spanIndex < curve.controlPoints.size();
         ++spanIndex) {
        const qreal spanStart = fullKnots[spanIndex];
        const qreal spanEnd = fullKnots[spanIndex + 1];
        if (spanEnd <= spanStart) {
            continue;
        }

        QPointF previousWorld;
        if (!evaluateNurbsPoint(curve, spanStart, &previousWorld)) {
            continue;
        }
        QPointF previousScreen = transform.worldToScreen(previousWorld, viewportSize);
        qreal previousParameter = spanStart;

        for (int sample = 1; sample <= samplesPerSpan; ++sample) {
            const qreal fraction = static_cast<qreal>(sample) / samplesPerSpan;
            const qreal parameter = spanStart + (spanEnd - spanStart) * fraction;
            QPointF currentWorld;
            if (!evaluateNurbsPoint(curve, parameter, &currentWorld)) {
                continue;
            }
            const QPointF currentScreen = transform.worldToScreen(currentWorld, viewportSize);
            const QPointF segment = currentScreen - previousScreen;
            const qreal segmentLengthSquared = QPointF::dotProduct(segment, segment);
            const qreal projection = segmentLengthSquared <= 1.0e-18
                                         ? 0.0
                                         : std::clamp(
                                               QPointF::dotProduct(cursorScreen - previousScreen,
                                                                   segment) /
                                                   segmentLengthSquared,
                                               0.0,
                                               1.0);
            const QPointF projectedScreen = previousScreen + segment * projection;
            const QPointF screenDifference = projectedScreen - cursorScreen;
            const qreal projectedDistanceSquared =
                QPointF::dotProduct(screenDifference, screenDifference);
            if (projectedDistanceSquared < bestSegmentDistanceSquared) {
                bestSegmentDistanceSquared = projectedDistanceSquared;
                bestParameterLow = previousParameter;
                bestParameterHigh = parameter;
                bestParameter = previousParameter +
                                (parameter - previousParameter) * projection;
                foundSegment = true;
            }

            previousScreen = currentScreen;
            previousParameter = parameter;
        }
    }

    if (!foundSegment) {
        return false;
    }

    const auto distanceAtParameter = [&](qreal parameter, QPointF *worldPoint) {
        QPointF evaluatedPoint;
        if (!evaluateNurbsPoint(curve, parameter, &evaluatedPoint)) {
            return std::numeric_limits<qreal>::infinity();
        }
        const QPointF screenDifference =
            transform.worldToScreen(evaluatedPoint, viewportSize) - cursorScreen;
        if (worldPoint != nullptr) {
            *worldPoint = evaluatedPoint;
        }
        return QPointF::dotProduct(screenDifference, screenDifference);
    };

    qreal bestCurveDistanceSquared = distanceAtParameter(bestParameter, nearestPoint);
    qreal low = bestParameterLow;
    qreal high = bestParameterHigh;
    constexpr qreal goldenRatioConjugate = 0.6180339887498948482;
    qreal firstParameter = high - goldenRatioConjugate * (high - low);
    qreal secondParameter = low + goldenRatioConjugate * (high - low);
    qreal firstDistanceSquared = distanceAtParameter(firstParameter, nullptr);
    qreal secondDistanceSquared = distanceAtParameter(secondParameter, nullptr);

    for (int iteration = 0; iteration < 16; ++iteration) {
        if (firstDistanceSquared <= secondDistanceSquared) {
            high = secondParameter;
            secondParameter = firstParameter;
            secondDistanceSquared = firstDistanceSquared;
            firstParameter = high - goldenRatioConjugate * (high - low);
            firstDistanceSquared = distanceAtParameter(firstParameter, nullptr);
        } else {
            low = firstParameter;
            firstParameter = secondParameter;
            firstDistanceSquared = secondDistanceSquared;
            secondParameter = low + goldenRatioConjugate * (high - low);
            secondDistanceSquared = distanceAtParameter(secondParameter, nullptr);
        }
    }

    for (const qreal candidateParameter : {low, high, firstParameter, secondParameter}) {
        QPointF candidatePoint;
        const qreal candidateDistanceSquared =
            distanceAtParameter(candidateParameter, &candidatePoint);
        if (candidateDistanceSquared < bestCurveDistanceSquared) {
            bestCurveDistanceSquared = candidateDistanceSquared;
            *nearestPoint = candidatePoint;
        }
    }

    *distanceSquared = bestCurveDistanceSquared;
    return std::isfinite(bestCurveDistanceSquared);
}

QVector<SnapCandidate> SnapEngine::tangentCandidatesForNurbsCurve(
    const Shape::NurbsCurve2D &curve,
    const QPointF &originWorld,
    const ViewportTransform &transform,
    const QSize &viewportSize) const
{
    QVector<SnapCandidate> candidates;
    if (curve.degree < 2 || !validateNurbsCurve(curve)) {
        return candidates;
    }

    const QVector<double> fullKnots = expandedNurbsKnotVector(curve);
    constexpr qreal rootTolerance = 1.0e-8;
    const int samplesPerSpan = std::clamp(curve.degree * 32, 64, 512);
    const auto tangentCondition = [&](qreal parameter,
                                      qreal spanStart,
                                      qreal spanEnd,
                                      qreal *condition) {
        if (condition == nullptr) {
            return false;
        }
        qreal evaluationParameter = parameter;
        if (evaluationParameter <= spanStart) {
            evaluationParameter = std::nextafter(spanStart, spanEnd);
        } else if (evaluationParameter >= spanEnd) {
            evaluationParameter = std::nextafter(spanEnd, spanStart);
        }

        QPointF curvePoint;
        QPointF derivative;
        if (!evaluateNurbsPoint(curve, evaluationParameter, &curvePoint) ||
            !evaluateNurbsDerivative(curve, evaluationParameter, &derivative)) {
            return false;
        }

        const QPointF toCurve = curvePoint - originWorld;
        const qreal chordLength = std::hypot(toCurve.x(), toCurve.y());
        const qreal derivativeLength = std::hypot(derivative.x(), derivative.y());
        if (chordLength <= 1.0e-12 || derivativeLength <= 1.0e-12) {
            return false;
        }

        // A normalized cross product is the sine of the angle between the
        // candidate line and the curve tangent. It is scale-independent and
        // gives the root finder a stable residual across zoom levels and
        // differently sized curves.
        *condition = crossProduct(toCurve / chordLength,
                                  derivative / derivativeLength);
        return std::isfinite(*condition);
    };

    for (int spanIndex = curve.degree;
         spanIndex < curve.controlPoints.size();
         ++spanIndex) {
        const qreal spanStart = fullKnots[spanIndex];
        const qreal spanEnd = fullKnots[spanIndex + 1];
        if (spanEnd <= spanStart) {
            continue;
        }

        const auto appendRoot = [&](qreal parameter) {
            qreal evaluationParameter = parameter;
            if (evaluationParameter <= spanStart) {
                evaluationParameter = std::nextafter(spanStart, spanEnd);
            } else if (evaluationParameter >= spanEnd) {
                evaluationParameter = std::nextafter(spanEnd, spanStart);
            }
            QPointF worldPoint;
            qreal residual = 0.0;
            if (!evaluateNurbsPoint(curve, evaluationParameter, &worldPoint) ||
                !tangentCondition(evaluationParameter,
                                  spanStart,
                                  spanEnd,
                                  &residual) ||
                std::abs(residual) > rootTolerance) {
                return;
            }
            const QPointF screenPoint = transform.worldToScreen(worldPoint, viewportSize);
            const QPointF originScreen = transform.worldToScreen(originWorld, viewportSize);
            if (std::hypot(screenPoint.x() - originScreen.x(),
                           screenPoint.y() - originScreen.y()) <= 1.0e-6) {
                return;
            }
            for (const SnapCandidate &candidate : candidates) {
                const QPointF candidateScreen = transform.worldToScreen(candidate.point,
                                                                         viewportSize);
                if (std::hypot(candidateScreen.x() - screenPoint.x(),
                               candidateScreen.y() - screenPoint.y()) <= 0.05) {
                    return;
                }
            }
            candidates.append({SnapType::Tangent, worldPoint});
        };

        QVector<qreal> parameters(samplesPerSpan + 1);
        QVector<qreal> conditions(samplesPerSpan + 1, 0.0);
        QVector<bool> valid(samplesPerSpan + 1, false);
        for (int sample = 0; sample <= samplesPerSpan; ++sample) {
            const qreal fraction = static_cast<qreal>(sample) / samplesPerSpan;
            parameters[sample] = spanStart + (spanEnd - spanStart) * fraction;
            valid[sample] = tangentCondition(parameters[sample],
                                             spanStart,
                                             spanEnd,
                                             &conditions[sample]);
            // Check both ends of every knot span explicitly. A tangent at an
            // arc seam or trimmed endpoint may not produce a sign change.
            if (valid[sample] && std::abs(conditions[sample]) <= rootTolerance) {
                appendRoot(parameters[sample]);
            }
        }

        for (int sample = 1; sample <= samplesPerSpan; ++sample) {
            if (valid[sample - 1] && valid[sample] &&
                ((conditions[sample - 1] < 0.0 && conditions[sample] > 0.0) ||
                 (conditions[sample - 1] > 0.0 && conditions[sample] < 0.0))) {
                qreal low = parameters[sample - 1];
                qreal high = parameters[sample];
                qreal lowCondition = conditions[sample - 1];
                for (int iteration = 0; iteration < 64; ++iteration) {
                    const qreal middle = (low + high) * 0.5;
                    qreal middleCondition = 0.0;
                    if (!tangentCondition(middle,
                                          spanStart,
                                          spanEnd,
                                          &middleCondition)) {
                        break;
                    }
                    if ((lowCondition < 0.0 && middleCondition < 0.0) ||
                        (lowCondition > 0.0 && middleCondition > 0.0)) {
                        low = middle;
                        lowCondition = middleCondition;
                    } else {
                        high = middle;
                    }
                    if (high - low <= 1.0e-13 *
                                           std::max(1.0, std::abs((low + high) * 0.5))) {
                        break;
                    }
                }
                appendRoot((low + high) * 0.5);
            }
        }

        // Also catch an even-multiplicity/touching root, where the tangent
        // residual reaches zero without changing sign between samples.
        for (int sample = 1; sample < samplesPerSpan; ++sample) {
            if (!valid[sample - 1] || !valid[sample] || !valid[sample + 1] ||
                conditions[sample - 1] * conditions[sample] <= 0.0 ||
                conditions[sample] * conditions[sample + 1] <= 0.0 ||
                std::abs(conditions[sample]) > std::abs(conditions[sample - 1]) ||
                std::abs(conditions[sample]) > std::abs(conditions[sample + 1])) {
                continue;
            }

            qreal low = parameters[sample - 1];
            qreal high = parameters[sample + 1];
            constexpr qreal goldenRatioConjugate = 0.6180339887498948482;
            qreal first = high - goldenRatioConjugate * (high - low);
            qreal second = low + goldenRatioConjugate * (high - low);
            qreal firstCondition = 0.0;
            qreal secondCondition = 0.0;
            if (!tangentCondition(first, spanStart, spanEnd, &firstCondition) ||
                !tangentCondition(second, spanStart, spanEnd, &secondCondition)) {
                continue;
            }
            for (int iteration = 0; iteration < 48; ++iteration) {
                if (std::abs(firstCondition) <= std::abs(secondCondition)) {
                    high = second;
                    second = first;
                    secondCondition = firstCondition;
                    first = high - goldenRatioConjugate * (high - low);
                    if (!tangentCondition(first,
                                          spanStart,
                                          spanEnd,
                                          &firstCondition)) {
                        break;
                    }
                } else {
                    low = first;
                    first = second;
                    firstCondition = secondCondition;
                    second = low + goldenRatioConjugate * (high - low);
                    if (!tangentCondition(second,
                                          spanStart,
                                          spanEnd,
                                          &secondCondition)) {
                        break;
                    }
                }
            }
            const qreal root = (low + high) * 0.5;
            qreal residual = 0.0;
            if (tangentCondition(root, spanStart, spanEnd, &residual) &&
                std::abs(residual) <= rootTolerance) {
                appendRoot(root);
            }
        }
    }
    return candidates;
}

QVector<SnapCandidate> SnapEngine::perpendicularCandidatesForNurbsCurve(
    const Shape::NurbsCurve2D &curve,
    const QPointF &originWorld) const
{
    QVector<SnapCandidate> candidates;
    if (!validateNurbsCurve(curve)) {
        return candidates;
    }

    const QVector<double> fullKnots = expandedNurbsKnotVector(curve);
    const int samplesPerSpan = std::clamp(curve.degree * 32, 64, 512);
    const auto perpendicularCondition = [&](qreal parameter,
                                            qreal spanStart,
                                            qreal spanEnd,
                                            qreal *condition) {
        if (condition == nullptr) {
            return false;
        }
        qreal evaluationParameter = parameter;
        if (evaluationParameter <= spanStart) {
            evaluationParameter = std::nextafter(spanStart, spanEnd);
        } else if (evaluationParameter >= spanEnd) {
            evaluationParameter = std::nextafter(spanEnd, spanStart);
        }

        QPointF curvePoint;
        QPointF derivative;
        if (!evaluateNurbsPoint(curve, evaluationParameter, &curvePoint) ||
            !evaluateNurbsDerivative(curve, evaluationParameter, &derivative)) {
            return false;
        }
        const QPointF toCurve = curvePoint - originWorld;
        const qreal chordLength = std::hypot(toCurve.x(), toCurve.y());
        const qreal derivativeLength = std::hypot(derivative.x(), derivative.y());
        if (chordLength <= 1.0e-12 || derivativeLength <= 1.0e-12) {
            return false;
        }
        *condition = QPointF::dotProduct(toCurve / chordLength,
                                         derivative / derivativeLength);
        return std::isfinite(*condition);
    };

    for (int spanIndex = curve.degree;
         spanIndex < curve.controlPoints.size();
         ++spanIndex) {
        const qreal spanStart = fullKnots[spanIndex];
        const qreal spanEnd = fullKnots[spanIndex + 1];
        if (spanEnd <= spanStart) {
            continue;
        }

        const auto appendRoot = [&](qreal parameter) {
            qreal evaluationParameter = parameter;
            if (evaluationParameter <= spanStart) {
                evaluationParameter = std::nextafter(spanStart, spanEnd);
            } else if (evaluationParameter >= spanEnd) {
                evaluationParameter = std::nextafter(spanEnd, spanStart);
            }
            QPointF worldPoint;
            qreal residual = 0.0;
            if (!evaluateNurbsPoint(curve, evaluationParameter, &worldPoint) ||
                !perpendicularCondition(evaluationParameter,
                                        spanStart,
                                        spanEnd,
                                        &residual) ||
                std::abs(residual) > 1.0e-8 ||
                std::hypot(worldPoint.x() - originWorld.x(),
                           worldPoint.y() - originWorld.y()) <= 1.0e-9) {
                return;
            }
            for (const SnapCandidate &candidate : candidates) {
                if (std::hypot(candidate.point.x() - worldPoint.x(),
                               candidate.point.y() - worldPoint.y()) <= 1.0e-7) {
                    return;
                }
            }
            candidates.append({SnapType::Perpendicular, worldPoint});
        };

        QVector<qreal> parameters(samplesPerSpan + 1);
        QVector<qreal> conditions(samplesPerSpan + 1, 0.0);
        QVector<bool> valid(samplesPerSpan + 1, false);
        for (int sample = 0; sample <= samplesPerSpan; ++sample) {
            const qreal fraction = static_cast<qreal>(sample) / samplesPerSpan;
            parameters[sample] = spanStart + (spanEnd - spanStart) * fraction;
            valid[sample] = perpendicularCondition(parameters[sample],
                                                   spanStart,
                                                   spanEnd,
                                                   &conditions[sample]);
            if (valid[sample] && std::abs(conditions[sample]) <= 1.0e-9) {
                appendRoot(parameters[sample]);
            }
        }

        for (int sample = 1; sample <= samplesPerSpan; ++sample) {
            if (!valid[sample - 1] || !valid[sample] ||
                !((conditions[sample - 1] < 0.0 && conditions[sample] > 0.0) ||
                  (conditions[sample - 1] > 0.0 && conditions[sample] < 0.0))) {
                continue;
            }
            qreal low = parameters[sample - 1];
            qreal high = parameters[sample];
            qreal lowCondition = conditions[sample - 1];
            for (int iteration = 0; iteration < 64; ++iteration) {
                const qreal middle = (low + high) * 0.5;
                qreal middleCondition = 0.0;
                if (!perpendicularCondition(middle,
                                            spanStart,
                                            spanEnd,
                                            &middleCondition)) {
                    break;
                }
                if ((lowCondition < 0.0 && middleCondition < 0.0) ||
                    (lowCondition > 0.0 && middleCondition > 0.0)) {
                    low = middle;
                    lowCondition = middleCondition;
                } else {
                    high = middle;
                }
                if (high - low <= 1.0e-13 *
                                       std::max(1.0, std::abs((low + high) * 0.5))) {
                    break;
                }
            }
            appendRoot((low + high) * 0.5);
        }
    }
    return candidates;
}

bool SnapEngine::makeArcSnapGeometry(const Shape &shape,
                                    QPointF *center,
                                    qreal *radius,
                                    qreal *startAngle,
                                    qreal *sweepAngle) const
{
    if (shape.geometryType != GeometryType::Arc || shape.points.size() < 3) {
        return false;
    }
    if (shape.arcMode != ArcMode::OnePoint) {
        CircularArc2D arc;
        if (!makeCircularArcThroughPoint(shape.points[0],
                                         shape.points[1],
                                         shape.points[2],
                                         &arc)) {
            return false;
        }
        if (center != nullptr) *center = arc.center;
        if (radius != nullptr) *radius = arc.radius;
        if (startAngle != nullptr) *startAngle = arc.startAngle;
        if (sweepAngle != nullptr) *sweepAngle = arc.sweepAngle;
        return true;
    }

    const QPointF arcCenter = shape.points[0];
    const QPointF start = shape.points[1];
    const QPointF end = shape.points[2];
    const qreal arcRadius = std::hypot(start.x() - arcCenter.x(),
                                       start.y() - arcCenter.y());
    if (arcRadius <= 1.0e-9) {
        return false;
    }

    constexpr qreal pi = 3.14159265358979323846;
    constexpr qreal twoPi = 2.0 * pi;
    const qreal firstAngle = std::atan2(start.y() - arcCenter.y(),
                                        start.x() - arcCenter.x());
    qreal selectedSweep = shape.arcSweep;
    if (std::abs(selectedSweep) <= 1.0e-9) {
        const qreal endAngle = std::atan2(end.y() - arcCenter.y(),
                                          end.x() - arcCenter.x());
        selectedSweep = endAngle - firstAngle;
        if (selectedSweep > pi) {
            selectedSweep -= twoPi;
        } else if (selectedSweep < -pi) {
            selectedSweep += twoPi;
        }
    }

    if (center != nullptr) *center = arcCenter;
    if (radius != nullptr) {
        *radius = arcRadius;
    }
    if (startAngle != nullptr) {
        *startAngle = firstAngle;
    }
    if (sweepAngle != nullptr) {
        *sweepAngle = selectedSweep;
    }
    return true;
}

bool SnapEngine::arcAngleIsOnSweep(qreal startAngle,
                                   qreal sweepAngle,
                                   qreal angle) const
{
    constexpr qreal twoPi = 6.28318530717958647692;
    constexpr qreal epsilon = 1.0e-7;
    if (std::abs(sweepAngle) >= twoPi - epsilon) {
        return true;
    }
    const auto positiveAngle = [twoPi](qreal value) {
        value = std::fmod(value, twoPi);
        return value < 0.0 ? value + twoPi : value;
    };
    if (sweepAngle >= 0.0) {
        return positiveAngle(angle - startAngle) <= sweepAngle + epsilon;
    }
    return positiveAngle(startAngle - angle) <= -sweepAngle + epsilon;
}

bool SnapEngine::arcSnapPointAtFraction(const Shape &shape,
                                        qreal fraction,
                                        QPointF *point) const
{
    QPointF center;
    qreal radius = 0.0;
    qreal startAngle = 0.0;
    qreal sweepAngle = 0.0;
    if (!makeArcSnapGeometry(shape, &center, &radius, &startAngle, &sweepAngle)) {
        return false;
    }
    const qreal angle = startAngle + sweepAngle * fraction;
    if (point != nullptr) {
        *point = center + QPointF(radius * std::cos(angle),
                                  radius * std::sin(angle));
    }
    return true;
}

QVector<SnapCandidate> SnapEngine::snapCandidatesForShape(
    const Shape &shape,
    const ViewportTransform &transform,
    const QSize &viewportSize,
    const Point3D &worldOffset) const
{
    Q_UNUSED(transform);
    Q_UNUSED(viewportSize);
    QVector<SnapCandidate> candidates;
    if (isDimensionGeometryType(shape.geometryType)) {
        return candidates;
    }
    if (shape.points.isEmpty() && !validateNurbsCurve(shape.nurbs) &&
        shape.geometryType != GeometryType::NurbsSurface &&
        shape.geometryType != GeometryType::NurbsSolid &&
        shape.components.isEmpty()) {
        return candidates;
    }

    Shape::NurbsCurve2D subdivisionCurveData;
    if (subdivisionCurve(shape, &subdivisionCurveData)) {
        for (const double parameter : shape.subdivisionParameters) {
            QPointF point;
            if (evaluateNurbsPoint(subdivisionCurveData, parameter, &point)) {
                candidates.append({SnapType::Endpoint, point});
            }
        }
    }

    if (shape.geometryType == GeometryType::NurbsSurface ||
        shape.geometryType == GeometryType::NurbsSolid) {
        for (const auto &face : shapeSurfaceFaces(shape)) {
            for (const Point3D &point : nurbsSurfaceCorners(face, worldOffset)) {
                SnapCandidate candidate{SnapType::Endpoint,
                                        QPointF(point.x, point.y)};
                candidate.worldPoint = point;
                candidate.hasWorldPoint = true;
                candidates.append(candidate);
            }
        }
        return candidates;
    }

    if (shape.geometryType == GeometryType::Point) {
        candidates.append({SnapType::Endpoint, shape.points.first()});
        return candidates;
    }
    if (shape.geometryType == GeometryType::PolyCurve) {
        for (int componentIndex = 0;
             componentIndex < shape.components.size();
             ++componentIndex) {
            const Shape::NurbsCurve2D &component =
                shape.components[componentIndex];
            const WorkPlaneFrame frame =
                shapeComponentWorkPlaneFrame(shape, componentIndex);
            const auto appendComponentCandidate = [&](SnapType type,
                                                       const QPointF &point) {
                SnapCandidate candidate{type, point};
                candidate.componentIndex = componentIndex;
                candidate.worldPoint = workPlaneFramePointToWorld(point, frame);
                candidate.hasWorldPoint = true;
                candidates.append(candidate);
            };
            QPointF start;
            QPointF end;
            if (nurbsCurveEndpoints(component, &start, &end)) {
                appendComponentCandidate(SnapType::Endpoint, start);
                appendComponentCandidate(SnapType::Endpoint, end);
            }
            QPointF midpoint;
            if (nurbsCurvePointAtFraction(component, 0.5, &midpoint)) {
                appendComponentCandidate(SnapType::Midpoint, midpoint);
            }
        }
        return candidates;
    }
    if (shape.geometryType == GeometryType::Bezier ||
        shape.geometryType == GeometryType::Nurbs) {
        Shape::NurbsCurve2D curve;
        if (subdivisionCurve(shape, &curve)) {
            QPointF start;
            QPointF end;
            if (nurbsCurveEndpoints(curve, &start, &end)) {
                candidates.append({SnapType::Endpoint, start});
                candidates.append({SnapType::Endpoint, end});
            }
            QPointF midpoint;
            if (nurbsCurvePointAtFraction(curve, 0.5, &midpoint)) {
                candidates.append({SnapType::Midpoint, midpoint});
            }
        }
        return candidates;
    }
    if (shape.geometryType == GeometryType::Ellipse) {
        if (!shape.points.isEmpty() || validateNurbsCurve(shape.nurbs)) {
            candidates.append({SnapType::Center, ellipseCenter(shape)});
        }
        if (validateNurbsCurve(shape.nurbs) && shape.nurbs.controlPoints.size() >= 9) {
            for (const int controlPointIndex : {0, 2, 4, 6}) {
                candidates.append({SnapType::Endpoint,
                                   shape.nurbs.controlPoints[controlPointIndex]});
            }
        }
        return candidates;
    }
    if (shape.geometryType == GeometryType::Circle) {
        if (!shape.points.isEmpty()) {
            candidates.append({SnapType::Center, shape.points.first()});
        }
        return candidates;
    }
    if (shape.geometryType == GeometryType::Rectangle ||
        shape.geometryType == GeometryType::Polygon ||
        shape.geometryType == GeometryType::Picture) {
        const QVector<QPointF> vertices =
            shape.geometryType == GeometryType::Polygon
                ? polygonVerticesForShape(shape)
                : shape.geometryType == GeometryType::Picture
                      ? pictureFrameCorners(shape)
                      : rectangleVertices(shape);
        for (const QPointF &vertex : vertices) {
            candidates.append({SnapType::Endpoint, vertex});
        }
        for (int index = 0; index < vertices.size(); ++index) {
            candidates.append({SnapType::Midpoint,
                               (vertices[index] + vertices[(index + 1) % vertices.size()]) /
                                   2.0});
        }
        return candidates;
    }
    if (shape.geometryType == GeometryType::Arc) {
        QPointF start;
        QPointF end;
        if (shape.points.size() >= 3) {
            start = shape.arcMode == ArcMode::OnePoint ? shape.points[1]
                                                        : shape.points[0];
            end = shape.arcMode == ArcMode::OnePoint ? shape.points[2]
                                                      : shape.points[1];
            if (!nurbsCurveEndpoints(shape.nurbs, &start, &end)) {
                QPointF evaluatedEndpoint;
                if (arcSnapPointAtFraction(shape, 0.0, &evaluatedEndpoint)) {
                    start = evaluatedEndpoint;
                }
                if (arcSnapPointAtFraction(shape, 1.0, &evaluatedEndpoint)) {
                    end = evaluatedEndpoint;
                }
            }
        } else if (!nurbsCurveEndpoints(shape.nurbs, &start, &end)) {
            return candidates;
        }
        candidates.append({SnapType::Endpoint, start});
        candidates.append({SnapType::Endpoint, end});
        QPointF midpoint;
        if (arcSnapPointAtFraction(shape, 0.5, &midpoint)) {
            candidates.append({SnapType::Midpoint, midpoint});
        }
        QPointF center;
        if (makeArcSnapGeometry(shape,
                                &center,
                                nullptr,
                                nullptr,
                                nullptr)) {
            candidates.append({SnapType::Center, center});
        }
        return candidates;
    }
    if (shape.geometryType != GeometryType::Line) {
        return candidates;
    }

    QVector<LineSegment> segments;
    if (shape.points.isEmpty()) {
        QPointF start;
        QPointF end;
        if (nurbsCurveEndpoints(shape.nurbs, &start, &end)) {
            candidates.append({SnapType::Endpoint, start});
            candidates.append({SnapType::Endpoint, end});
        }
        return candidates;
    }

    for (const QPointF &point : shape.points) {
        candidates.append({SnapType::Endpoint, point});
    }
    for (int index = 0; index + 1 < shape.points.size(); ++index) {
        const QPointF start = shape.points[index];
        const QPointF end = shape.points[index + 1];
        segments.append({start, end});
        candidates.append({SnapType::Midpoint, (start + end) / 2.0});
    }
    for (int first = 0; first < segments.size(); ++first) {
        for (int second = first + 1; second < segments.size(); ++second) {
            QPointF intersection;
            if (segmentIntersection(segments[first].start,
                                    segments[first].end,
                                    segments[second].start,
                                    segments[second].end,
                                    &intersection)) {
                candidates.append({SnapType::Intersection, intersection});
            }
        }
    }
    return candidates;
}

QVector<SnapCandidate> SnapEngine::edgeCenterCandidatesForShape(
    const Shape &shape) const
{
    QVector<SnapCandidate> candidates;
    if (shape.geometryType == GeometryType::Point ||
        isDimensionGeometryType(shape.geometryType)) {
        return candidates;
    }
    if (shape.geometryType == GeometryType::Rectangle ||
        shape.geometryType == GeometryType::Polygon ||
        shape.geometryType == GeometryType::Picture) {
        const QVector<QPointF> vertices =
            shape.geometryType == GeometryType::Polygon
                ? polygonVerticesForShape(shape)
                : shape.geometryType == GeometryType::Picture
                      ? pictureFrameCorners(shape)
                      : rectangleVertices(shape);
        for (int index = 0; index < vertices.size(); ++index) {
            candidates.append({SnapType::Midpoint,
                               (vertices[index] +
                                vertices[(index + 1) % vertices.size()]) * 0.5});
        }
        return candidates;
    }
    if (shape.geometryType == GeometryType::Line && shape.points.size() >= 2) {
        for (int index = 0; index + 1 < shape.points.size(); ++index) {
            candidates.append({SnapType::Midpoint,
                               (shape.points[index] + shape.points[index + 1]) * 0.5});
        }
        return candidates;
    }
    if (shape.geometryType == GeometryType::PolyCurve) {
        for (const Shape::NurbsCurve2D &component : shape.components) {
            QPointF midpoint;
            if (nurbsCurvePointAtFraction(component, 0.5, &midpoint)) {
                candidates.append({SnapType::Midpoint, midpoint});
            }
        }
        return candidates;
    }

    Shape::NurbsCurve2D curve;
    if (subdivisionCurve(shape, &curve)) {
        QPointF midpoint;
        if (nurbsCurvePointAtFraction(curve, 0.5, &midpoint)) {
            candidates.append({SnapType::Midpoint, midpoint});
        }
    }
    return candidates;
}

SnapResult SnapEngine::findEdgeCenterSnapPoint(
    const Document &document,
    const QPointF &screenPosition,
    const ViewportTransform &transform,
    const QSize &viewportSize) const
{
    SnapResult best;
    if (!settings_.enabled || viewportSize.isEmpty()) {
        return best;
    }
    const OcclusionPlane occlusionPlane = occlusionPlaneAt(screenPosition);
    constexpr qreal snapRadiusPixels = 12.0;
    qreal bestDistance = snapRadiusPixels;
    for (int shapeIndex = 0; shapeIndex < document.size(); ++shapeIndex) {
        const ObjectId objectId = document.objectIdAt(shapeIndex);
        if (!document.isObjectVisible(objectId)) {
            continue;
        }
        const Shape &shape = document[shapeIndex];
        const WorkPlaneFrame frame = shapeWorkPlaneFrame(shape);
        for (const SnapCandidate &candidate : edgeCenterCandidatesForShape(shape)) {
            const Point3D worldPoint =
                workPlaneFramePointToWorld(candidate.point, frame);
            QPointF candidateScreen;
            if (!transform.worldPointToScreen(worldPoint, viewportSize,
                                              &candidateScreen)) {
                continue;
            }
            const qreal distance = std::hypot(candidateScreen.x() - screenPosition.x(),
                                              candidateScreen.y() - screenPosition.y());
            if (distance <= bestDistance &&
                pointPassesOcclusionPlane(worldPoint,
                                          occlusionPlane,
                                          transform)) {
                bestDistance = distance;
                best.type = SnapType::Midpoint;
                best.point = transform.screenToWorld(candidateScreen, viewportSize);
                best.worldPoint = worldPoint;
                best.hasWorldPoint = true;
            }
        }
    }
    return best;
}

QVector<SnapCandidate> SnapEngine::snapCandidatesForScene(
    const Document &document,
    const QVector<int> &excludedShapeIndices,
    const ViewportTransform &transform,
    const QSize &viewportSize) const
{
    Q_UNUSED(viewportSize);
    QVector<SnapCandidate> candidates;
    struct IntersectionSegment {
        QPointF start;
        QPointF end;
        qreal startParameter = 0.0;
        qreal endParameter = 0.0;
        int curveIndex = -1;
        int pathIndex = -1;
    };
    QVector<Shape::NurbsCurve2D> intersectionCurves;
    QVector<IntersectionSegment> intersectionSegments;
    const auto appendIntersectionCurve = [&](const Shape::NurbsCurve2D &curve,
                                             const WorkPlaneFrame &frame) {
        if (!settings_.intersection || !validateNurbsCurve(curve)) {
            return;
        }
        Shape::NurbsCurve2D projectedCurve = curve;
        mapCurveBetweenFrames(&projectedCurve,
                              frame,
                              transform.workPlaneFrame());
        const int curveIndex = intersectionCurves.size();
        intersectionCurves.append(std::move(projectedCurve));
        const Shape::NurbsCurve2D &sampleCurve = intersectionCurves.back();
        const QVector<double> fullKnots = expandedNurbsKnotVector(sampleCurve);
        constexpr int samplesPerSpan = 64;
        for (int spanIndex = sampleCurve.degree;
             spanIndex < sampleCurve.controlPoints.size();
             ++spanIndex) {
            const qreal spanStart = fullKnots[spanIndex];
            const qreal spanEnd = fullKnots[spanIndex + 1];
            if (spanEnd <= spanStart) {
                continue;
            }
            QPointF previousPoint;
            if (!evaluateNurbsPoint(sampleCurve, spanStart, &previousPoint)) {
                continue;
            }
            qreal previousParameter = spanStart;
            for (int sample = 1; sample <= samplesPerSpan; ++sample) {
                const qreal fraction = static_cast<qreal>(sample) / samplesPerSpan;
                const qreal parameter = spanStart + (spanEnd - spanStart) * fraction;
                QPointF point;
                if (!evaluateNurbsPoint(sampleCurve, parameter, &point)) {
                    continue;
                }
                if (std::hypot(point.x() - previousPoint.x(),
                               point.y() - previousPoint.y()) > 1.0e-12) {
                    intersectionSegments.append({previousPoint,
                                                 point,
                                                 previousParameter,
                                                 parameter,
                                                 curveIndex,
                                                 curveIndex});
                }
                previousPoint = point;
                previousParameter = parameter;
            }
        }
    };

    for (int shapeIndex = 0; shapeIndex < document.size(); ++shapeIndex) {
        if (excludedShapeIndices.contains(shapeIndex)) {
            continue;
        }
        if (!document.isObjectVisible(document.objectIdAt(shapeIndex))) {
            continue;
        }
        const ObjectId objectId = document.objectIdAt(shapeIndex);
        const SceneObject *sceneObject = document.object(objectId);
        const Shape &shape = document[shapeIndex];
        const Point3D worldOffset = sceneObject != nullptr
                                        ? sceneObject->placementTranslation
                                        : Point3D{};
        const bool spatialSurface = shape.geometryType == GeometryType::NurbsSurface ||
                                    shape.geometryType == GeometryType::NurbsSolid;
        if (!spatialSurface &&
            !workPlaneMatches(shapeWorkPlaneFrame(shape),
                              transform.workPlaneFrame())) {
            continue;
        }
        if (isDimensionGeometryType(shape.geometryType)) {
            continue;
        }
        if (shape.points.isEmpty() && !validateNurbsCurve(shape.nurbs) &&
            shape.geometryType != GeometryType::NurbsSurface &&
            shape.geometryType != GeometryType::NurbsSolid &&
            shape.components.isEmpty()) {
            continue;
        }

        if (settings_.intersection) {
            if (shape.geometryType == GeometryType::PolyCurve) {
                for (int componentIndex = 0;
                     componentIndex < shape.components.size();
                     ++componentIndex) {
                    appendIntersectionCurve(
                        shape.components[componentIndex],
                        shapeComponentWorkPlaneFrame(shape, componentIndex));
                }
            } else if (validateNurbsCurve(shape.nurbs)) {
                appendIntersectionCurve(shape.nurbs, shapeWorkPlaneFrame(shape));
            } else {
                Shape::NurbsCurve2D intersectionCurve;
                if (shape.geometryType == GeometryType::Line &&
                    shape.points.size() >= 2) {
                    intersectionCurve = makeDegreeOneNurbs(shape.points);
                } else if (shape.geometryType == GeometryType::Bezier &&
                           shape.points.size() >= 2) {
                    intersectionCurve = makeBezierNurbs(shape.points);
                } else if (shape.geometryType == GeometryType::Circle &&
                           shape.points.size() >= 2) {
                    intersectionCurve = makeCircleNurbs(shape.points);
                } else if (shape.geometryType == GeometryType::Arc &&
                           shape.points.size() >= 3) {
                    CircularArc2D arc;
                    if (shape.arcMode == ArcMode::OnePoint) {
                        const QPointF radiusVector = shape.points[1] - shape.points[0];
                        const qreal radius = std::hypot(radiusVector.x(),
                                                        radiusVector.y());
                        const qreal startAngle = std::atan2(radiusVector.y(),
                                                           radiusVector.x());
                        qreal sweep = shape.arcSweep;
                        if (std::abs(sweep) <= 1.0e-9) {
                            const QPointF endVector = shape.points[2] - shape.points[0];
                            sweep = std::atan2(endVector.y(), endVector.x()) - startAngle;
                            constexpr qreal pi = 3.14159265358979323846;
                            constexpr qreal twoPi = 2.0 * pi;
                            if (sweep > pi) sweep -= twoPi;
                            if (sweep < -pi) sweep += twoPi;
                        }
                        makeCircularArcFromCenterSweep(shape.points[0],
                                                       radius,
                                                       startAngle,
                                                       sweep,
                                                       &arc);
                    } else {
                        makeCircularArcThroughPoint(shape.points[0],
                                                    shape.points[1],
                                                    shape.points[2],
                                                    &arc);
                    }
                    intersectionCurve = arc.curve;
                } else if (shape.geometryType == GeometryType::Rectangle) {
                    QVector<QPointF> points = rectangleVertices(shape);
                    if (!points.isEmpty()) {
                        points.append(points.first());
                        intersectionCurve = makeDegreeOneNurbs(points);
                    }
                } else if (shape.geometryType == GeometryType::Polygon) {
                    QVector<QPointF> points = polygonVerticesForShape(shape);
                    if (!points.isEmpty()) {
                        points.append(points.first());
                        intersectionCurve = makeDegreeOneNurbs(points);
                    }
                } else if (shape.geometryType == GeometryType::Picture) {
                    QVector<QPointF> points = pictureFrameCorners(shape);
                    if (!points.isEmpty()) {
                        points.append(points.first());
                        intersectionCurve = makeDegreeOneNurbs(points);
                    }
                }
                appendIntersectionCurve(intersectionCurve,
                                        shapeWorkPlaneFrame(shape));
            }
        }

        if (settings_.controlPoint) {
            if (shape.geometryType == GeometryType::PolyCurve) {
                for (int componentIndex = 0;
                     componentIndex < shape.components.size();
                     ++componentIndex) {
                    const Shape::NurbsCurve2D &component =
                        shape.components[componentIndex];
                    const WorkPlaneFrame componentFrame =
                        shapeComponentWorkPlaneFrame(shape, componentIndex);
                    for (const QPointF &controlPoint : component.controlPoints) {
                        SnapCandidate candidate{SnapType::ControlPoint, controlPoint};
                        candidate.componentIndex = componentIndex;
                        candidate.worldPoint = workPlaneFramePointToWorld(
                            controlPoint, componentFrame);
                        candidate.hasWorldPoint = true;
                        candidates.append(candidate);
                    }
                }
            } else if (shape.geometryType != GeometryType::Point) {
                const QVector<QPointF> &controlPoints =
                    shape.nurbs.controlPoints.isEmpty() ? shape.points
                                                        : shape.nurbs.controlPoints;
                for (const QPointF &controlPoint : controlPoints) {
                    candidates.append({SnapType::ControlPoint, controlPoint});
                }
            }
        }

        if (settings_.endpoint) {
            Shape::NurbsCurve2D subdivisionCurveData;
            if (subdivisionCurve(shape, &subdivisionCurveData)) {
                for (const double parameter : shape.subdivisionParameters) {
                    QPointF point;
                    if (evaluateNurbsPoint(subdivisionCurveData, parameter, &point)) {
                        candidates.append({SnapType::Endpoint, point});
                    }
                }
            }
        }

        if (shape.geometryType == GeometryType::NurbsSurface ||
            shape.geometryType == GeometryType::NurbsSolid) {
            if (settings_.endpoint) {
                for (const auto &face : shapeSurfaceFaces(shape)) {
                    for (const Point3D &point : nurbsSurfaceCorners(face,
                                                                   worldOffset)) {
                        SnapCandidate candidate{SnapType::Endpoint,
                                                QPointF(point.x, point.y)};
                        candidate.worldPoint = point;
                        candidate.hasWorldPoint = true;
                        candidates.append(candidate);
                    }
                }
            }
            continue;
        }

        if (shape.geometryType == GeometryType::Point) {
            if (settings_.endpoint && !shape.points.isEmpty()) {
                candidates.append({SnapType::Endpoint, shape.points.first()});
            }
            continue;
        }
        if (shape.geometryType == GeometryType::PolyCurve) {
            for (int componentIndex = 0;
                 componentIndex < shape.components.size();
                 ++componentIndex) {
                const Shape::NurbsCurve2D &component =
                    shape.components[componentIndex];
                const WorkPlaneFrame componentFrame =
                    shapeComponentWorkPlaneFrame(shape, componentIndex);
                const auto appendComponentCandidate = [&](SnapType type,
                                                           const QPointF &point) {
                    SnapCandidate candidate{type, point};
                    candidate.componentIndex = componentIndex;
                    candidate.worldPoint = workPlaneFramePointToWorld(
                        point, componentFrame);
                    candidate.hasWorldPoint = true;
                    candidates.append(candidate);
                };
                QPointF start;
                QPointF end;
                if (settings_.endpoint && nurbsCurveEndpoints(component, &start, &end)) {
                    appendComponentCandidate(SnapType::Endpoint, start);
                    appendComponentCandidate(SnapType::Endpoint, end);
                }
                QPointF midpoint;
                if (settings_.midpoint &&
                    nurbsCurvePointAtFraction(component, 0.5, &midpoint)) {
                    appendComponentCandidate(SnapType::Midpoint, midpoint);
                }
            }
            continue;
        }
        if (shape.geometryType == GeometryType::Bezier ||
            shape.geometryType == GeometryType::Nurbs) {
            Shape::NurbsCurve2D curve;
            if (subdivisionCurve(shape, &curve)) {
                QPointF start;
                QPointF end;
                if (settings_.endpoint && nurbsCurveEndpoints(curve, &start, &end)) {
                    candidates.append({SnapType::Endpoint, start});
                    candidates.append({SnapType::Endpoint, end});
                }
                QPointF midpoint;
                if (settings_.midpoint &&
                    nurbsCurvePointAtFraction(curve, 0.5, &midpoint)) {
                    candidates.append({SnapType::Midpoint, midpoint});
                }
            }
            continue;
        }
        if (shape.geometryType == GeometryType::Ellipse) {
            if (settings_.center &&
                (!shape.points.isEmpty() || validateNurbsCurve(shape.nurbs))) {
                candidates.append({SnapType::Center, ellipseCenter(shape)});
            }
            if (settings_.endpoint && validateNurbsCurve(shape.nurbs) &&
                shape.nurbs.controlPoints.size() >= 9) {
                for (const int controlPointIndex : {0, 2, 4, 6}) {
                    candidates.append({SnapType::Endpoint,
                                       shape.nurbs.controlPoints[controlPointIndex]});
                }
            }
            continue;
        }
        if (shape.geometryType == GeometryType::Circle) {
            if (settings_.center && !shape.points.isEmpty()) {
                candidates.append({SnapType::Center, shape.points.first()});
            }
            continue;
        }
        if (shape.geometryType == GeometryType::Rectangle ||
            shape.geometryType == GeometryType::Polygon ||
            shape.geometryType == GeometryType::Picture) {
            const QVector<QPointF> vertices =
                shape.geometryType == GeometryType::Polygon
                    ? polygonVerticesForShape(shape)
                    : shape.geometryType == GeometryType::Picture
                          ? pictureFrameCorners(shape)
                          : rectangleVertices(shape);
            for (int index = 0; index < vertices.size(); ++index) {
                const QPointF start = vertices[index];
                const QPointF end = vertices[(index + 1) % vertices.size()];
                if (settings_.endpoint) {
                    candidates.append({SnapType::Endpoint, start});
                }
                if (settings_.midpoint) {
                    candidates.append({SnapType::Midpoint, (start + end) / 2.0});
                }
            }
            continue;
        }
        if (shape.geometryType == GeometryType::Arc && shape.points.size() >= 3) {
            QPointF start = shape.arcMode == ArcMode::OnePoint ? shape.points[1]
                                                                : shape.points[0];
            QPointF end = shape.arcMode == ArcMode::OnePoint ? shape.points[2]
                                                              : shape.points[1];
            if (!nurbsCurveEndpoints(shape.nurbs, &start, &end)) {
                QPointF evaluatedEndpoint;
                if (arcSnapPointAtFraction(shape, 0.0, &evaluatedEndpoint)) {
                    start = evaluatedEndpoint;
                }
                if (arcSnapPointAtFraction(shape, 1.0, &evaluatedEndpoint)) {
                    end = evaluatedEndpoint;
                }
            }
            if (settings_.endpoint) {
                candidates.append({SnapType::Endpoint, start});
                candidates.append({SnapType::Endpoint, end});
            }
            if (settings_.midpoint) {
                QPointF midpoint;
                if (arcSnapPointAtFraction(shape, 0.5, &midpoint)) {
                    candidates.append({SnapType::Midpoint, midpoint});
                }
            }
            if (settings_.center) {
                QPointF center;
                if (makeArcSnapGeometry(shape,
                                        &center,
                                        nullptr,
                                        nullptr,
                                        nullptr)) {
                    candidates.append({SnapType::Center, center});
                }
            }
            continue;
        }
        if (shape.geometryType == GeometryType::Arc) {
            QPointF start;
            QPointF end;
            if (settings_.endpoint && nurbsCurveEndpoints(shape.nurbs, &start, &end)) {
                candidates.append({SnapType::Endpoint, start});
                candidates.append({SnapType::Endpoint, end});
            }
            continue;
        }
        if (shape.geometryType != GeometryType::Line) {
            continue;
        }
        if (settings_.endpoint) {
            QPointF start;
            QPointF end;
            if (nurbsCurveEndpoints(shape.nurbs, &start, &end)) {
                candidates.append({SnapType::Endpoint, start});
                candidates.append({SnapType::Endpoint, end});
            } else {
                for (const QPointF &point : shape.points) {
                    candidates.append({SnapType::Endpoint, point});
                }
            }
        }
        for (int index = 0; index + 1 < shape.points.size(); ++index) {
            const QPointF start = shape.points[index];
            const QPointF end = shape.points[index + 1];
            if (settings_.midpoint) {
                candidates.append({SnapType::Midpoint, (start + end) / 2.0});
            }
        }
    }

    if (settings_.intersection) {
        QVector<int> orderedSegments(intersectionSegments.size());
        for (int index = 0; index < orderedSegments.size(); ++index) {
            orderedSegments[index] = index;
        }
        const auto minimumX = [&](int index) {
            const IntersectionSegment &segment = intersectionSegments[index];
            return std::min(segment.start.x(), segment.end.x());
        };
        std::sort(orderedSegments.begin(), orderedSegments.end(),
                  [&](int first, int second) {
                      return minimumX(first) < minimumX(second);
                  });
        const auto refineIntersection = [&](int firstCurveIndex,
                                            int secondCurveIndex,
                                            qreal firstParameter,
                                            qreal secondParameter,
                                            qreal firstParameterMin,
                                            qreal firstParameterMax,
                                            qreal secondParameterMin,
                                            qreal secondParameterMax,
                                            QPointF *point) {
            const Shape::NurbsCurve2D &firstCurve =
                intersectionCurves[firstCurveIndex];
            const Shape::NurbsCurve2D &secondCurve =
                intersectionCurves[secondCurveIndex];
            constexpr qreal epsilon = 1.0e-12;
            for (int iteration = 0; iteration < 24; ++iteration) {
                QPointF firstPoint;
                QPointF secondPoint;
                QPointF firstDerivative;
                QPointF secondDerivative;
                if (!evaluateNurbsPoint(firstCurve, firstParameter, &firstPoint) ||
                    !evaluateNurbsPoint(secondCurve, secondParameter, &secondPoint) ||
                    !evaluateNurbsDerivative(firstCurve, firstParameter,
                                             &firstDerivative) ||
                    !evaluateNurbsDerivative(secondCurve, secondParameter,
                                             &secondDerivative)) {
                    return false;
                }
                const QPointF difference = firstPoint - secondPoint;
                const qreal separation = std::hypot(difference.x(), difference.y());
                if (separation <= 1.0e-8) {
                    *point = (firstPoint + secondPoint) * 0.5;
                    return true;
                }
                const qreal determinant = -crossProduct(firstDerivative,
                                                        secondDerivative);
                const qreal derivativeScale =
                    std::hypot(firstDerivative.x(), firstDerivative.y()) *
                    std::hypot(secondDerivative.x(), secondDerivative.y());
                if (derivativeScale <= epsilon ||
                    std::abs(determinant) <= epsilon * derivativeScale) {
                    return false;
                }
                const qreal firstDelta =
                    (difference.x() * secondDerivative.y() -
                     secondDerivative.x() * difference.y()) / determinant;
                const qreal secondDelta =
                    (-firstDerivative.x() * difference.y() +
                     difference.x() * firstDerivative.y()) / determinant;
                const qreal nextFirst = std::clamp(firstParameter + firstDelta,
                                                   firstParameterMin,
                                                   firstParameterMax);
                const qreal nextSecond = std::clamp(secondParameter + secondDelta,
                                                    secondParameterMin,
                                                    secondParameterMax);
                if (std::abs(nextFirst - firstParameter) <= 1.0e-14 &&
                    std::abs(nextSecond - secondParameter) <= 1.0e-14) {
                    return false;
                }
                firstParameter = nextFirst;
                secondParameter = nextSecond;
            }
            QPointF firstPoint;
            QPointF secondPoint;
            if (!evaluateNurbsPoint(firstCurve, firstParameter, &firstPoint) ||
                !evaluateNurbsPoint(secondCurve, secondParameter, &secondPoint) ||
                std::hypot(firstPoint.x() - secondPoint.x(),
                           firstPoint.y() - secondPoint.y()) > 1.0e-7) {
                return false;
            }
            *point = (firstPoint + secondPoint) * 0.5;
            return true;
        };

        for (int orderedIndex = 0; orderedIndex < orderedSegments.size();
             ++orderedIndex) {
            const IntersectionSegment &first =
                intersectionSegments[orderedSegments[orderedIndex]];
            const qreal firstMaxX = std::max(first.start.x(), first.end.x());
            const qreal firstMinY = std::min(first.start.y(), first.end.y());
            const qreal firstMaxY = std::max(first.start.y(), first.end.y());
            for (int nextIndex = orderedIndex + 1;
                 nextIndex < orderedSegments.size(); ++nextIndex) {
                const IntersectionSegment &second =
                    intersectionSegments[orderedSegments[nextIndex]];
                if (minimumX(orderedSegments[nextIndex]) > firstMaxX) {
                    break;
                }
                if (first.pathIndex == second.pathIndex ||
                    std::max(second.start.y(), second.end.y()) < firstMinY ||
                    std::min(second.start.y(), second.end.y()) > firstMaxY) {
                    continue;
                }
                QPointF intersection;
                if (!segmentIntersection(first.start, first.end,
                                        second.start, second.end,
                                        &intersection)) {
                    continue;
                }
                const QPointF firstDirection = first.end - first.start;
                const QPointF secondDirection = second.end - second.start;
                const qreal firstLengthSquared =
                    QPointF::dotProduct(firstDirection, firstDirection);
                const qreal secondLengthSquared =
                    QPointF::dotProduct(secondDirection, secondDirection);
                if (firstLengthSquared <= 1.0e-24 ||
                    secondLengthSquared <= 1.0e-24) {
                    continue;
                }
                const qreal firstFraction = std::clamp(
                    QPointF::dotProduct(intersection - first.start,
                                        firstDirection) / firstLengthSquared,
                    0.0, 1.0);
                const qreal secondFraction = std::clamp(
                    QPointF::dotProduct(intersection - second.start,
                                        secondDirection) / secondLengthSquared,
                    0.0, 1.0);
                QPointF refinedIntersection;
                if (!refineIntersection(
                        first.curveIndex,
                        second.curveIndex,
                        first.startParameter +
                            (first.endParameter - first.startParameter) * firstFraction,
                        second.startParameter +
                            (second.endParameter - second.startParameter) * secondFraction,
                        std::min(first.startParameter, first.endParameter),
                        std::max(first.startParameter, first.endParameter),
                        std::min(second.startParameter, second.endParameter),
                        std::max(second.startParameter, second.endParameter),
                        &refinedIntersection)) {
                    continue;
                }
                const bool duplicate = std::any_of(
                    candidates.cbegin(), candidates.cend(),
                    [&](const SnapCandidate &candidate) {
                        return candidate.type == SnapType::Intersection &&
                               std::hypot(candidate.point.x() - refinedIntersection.x(),
                                          candidate.point.y() - refinedIntersection.y()) <=
                                   1.0e-7;
                    });
                if (!duplicate) {
                    candidates.append({SnapType::Intersection, refinedIntersection});
                }
            }
        }
    }
    return candidates;
}

bool SnapEngine::perpendicularPointForShape(const Shape &shape,
                                            const QPointF &origin,
                                            const ViewportTransform &transform,
                                            const QSize &viewportSize,
                                            QPointF *point) const
{
    if (point == nullptr || shape.geometryType == GeometryType::Point ||
        shape.geometryType == GeometryType::Rectangle ||
        shape.geometryType == GeometryType::Polygon ||
        shape.geometryType == GeometryType::Picture) {
        return false;
    }

    const QPointF originScreen = transform.worldToScreen(origin, viewportSize);
    qreal bestDistanceSquared = std::numeric_limits<qreal>::infinity();
    QPointF bestPoint;
    const auto considerCurve = [&](const Shape::NurbsCurve2D &curve) {
        for (const SnapCandidate &candidate :
             perpendicularCandidatesForNurbsCurve(curve, origin)) {
            const QPointF difference = candidate.point - origin;
            const qreal distanceSquared = QPointF::dotProduct(difference, difference);
            if (distanceSquared < bestDistanceSquared) {
                bestDistanceSquared = distanceSquared;
                bestPoint = candidate.point;
            }
        }
    };

    if (shape.geometryType == GeometryType::PolyCurve) {
        for (const Shape::NurbsCurve2D &component : shape.components) {
            considerCurve(component);
        }
    } else {
        Shape::NurbsCurve2D curve;
        if (subdivisionCurve(shape, &curve)) {
            considerCurve(curve);
        } else if (shape.geometryType == GeometryType::Circle &&
                   shape.points.size() >= 2) {
            considerCurve(makeCircleNurbs(shape.points));
        } else if (shape.geometryType == GeometryType::Arc &&
                   shape.points.size() >= 3) {
            // Legacy arc fallback: committed arcs use their stored NURBS above.
            constexpr int arcSegments = 128;
            QPointF previousWorld;
            if (arcSnapPointAtFraction(shape, 0.0, &previousWorld)) {
                QPointF previousScreen = transform.worldToScreen(previousWorld,
                                                                  viewportSize);
                for (int segmentIndex = 1; segmentIndex <= arcSegments;
                     ++segmentIndex) {
                    QPointF currentWorld;
                    if (!arcSnapPointAtFraction(
                            shape,
                            static_cast<qreal>(segmentIndex) / arcSegments,
                            &currentWorld)) {
                        break;
                    }
                    const QPointF currentScreen = transform.worldToScreen(currentWorld,
                                                                           viewportSize);
                    const QPointF segment = currentScreen - previousScreen;
                    const qreal lengthSquared = QPointF::dotProduct(segment, segment);
                    const qreal fraction =
                        lengthSquared <= 1.0e-18
                            ? 0.0
                            : std::clamp(
                                  QPointF::dotProduct(originScreen - previousScreen,
                                                      segment) /
                                      lengthSquared,
                                  0.0,
                                  1.0);
                    const QPointF projectedScreen = previousScreen + segment * fraction;
                    const QPointF difference = projectedScreen - originScreen;
                    const qreal distanceSquared = QPointF::dotProduct(difference,
                                                                       difference);
                    if (distanceSquared < bestDistanceSquared) {
                        bestDistanceSquared = distanceSquared;
                        bestPoint = previousWorld + (currentWorld - previousWorld) * fraction;
                    }
                    previousWorld = currentWorld;
                    previousScreen = currentScreen;
                }
            }
        }
    }

    if (!std::isfinite(bestDistanceSquared)) {
        return false;
    }
    *point = bestPoint;
    return true;
}

QVector<SnapCandidate> SnapEngine::perpendicularCandidates(
    const Document &document,
    const QPointF &origin,
    const QPointF &cursor,
    const ViewportTransform &transform,
    const QSize &viewportSize,
    const QVector<int> &excludedShapeIndices) const
{
    Q_UNUSED(viewportSize);
    QVector<SnapCandidate> candidates;
    if (!settings_.perpendicular) {
        return candidates;
    }
    constexpr qreal epsilon = 1.0e-9;
    for (int shapeIndex = 0; shapeIndex < document.size(); ++shapeIndex) {
        if (excludedShapeIndices.contains(shapeIndex) ||
            !document.isObjectVisible(document.objectIdAt(shapeIndex))) {
            continue;
        }
        const Shape &shape = document[shapeIndex];
        if (!workPlaneMatches(shapeWorkPlaneFrame(shape),
                              transform.workPlaneFrame())) {
            continue;
        }
        if (shape.geometryType == GeometryType::Ellipse) {
            if (validateNurbsCurve(shape.nurbs)) {
                candidates += perpendicularCandidatesForNurbsCurve(shape.nurbs, origin);
            }
            continue;
        }
        if (shape.points.isEmpty()) {
            continue;
        }
        if (shape.geometryType == GeometryType::Circle && shape.points.size() >= 2) {
            const QPointF center = shape.points[0];
            const QPointF edge = shape.points[1];
            const qreal radius = std::hypot(edge.x() - center.x(), edge.y() - center.y());
            if (radius <= epsilon) {
                continue;
            }
            const QPointF fromCenter = origin - center;
            const qreal distanceFromCenter = std::hypot(fromCenter.x(), fromCenter.y());
            if (distanceFromCenter <= epsilon) {
                const QPointF towardCursor = cursor - center;
                const qreal cursorDistance = std::hypot(towardCursor.x(), towardCursor.y());
                if (cursorDistance > epsilon) {
                    candidates.append({SnapType::Perpendicular,
                                       center + towardCursor * (radius / cursorDistance)});
                }
            } else {
                const QPointF radialDirection = fromCenter / distanceFromCenter;
                candidates.append({SnapType::Perpendicular,
                                   center + radialDirection * radius});
                candidates.append({SnapType::Perpendicular,
                                   center - radialDirection * radius});
            }
            continue;
        }
        if (shape.geometryType == GeometryType::Arc && shape.points.size() >= 3) {
            if (validateNurbsCurve(shape.nurbs)) {
                candidates += perpendicularCandidatesForNurbsCurve(shape.nurbs,
                                                                   origin);
                continue;
            }
            QPointF center;
            qreal radius = 0.0;
            qreal startAngle = 0.0;
            qreal sweepAngle = 0.0;
            if (!makeArcSnapGeometry(shape,
                                     &center,
                                     &radius,
                                     &startAngle,
                                     &sweepAngle)) {
                continue;
            }
            const QPointF fromCenter = origin - center;
            const qreal distanceFromCenter = std::hypot(fromCenter.x(), fromCenter.y());
            const auto appendIfOnArc = [&](const QPointF &candidate) {
                const qreal candidateAngle = std::atan2(candidate.y() - center.y(),
                                                         candidate.x() - center.x());
                if (arcAngleIsOnSweep(startAngle, sweepAngle, candidateAngle)) {
                    candidates.append({SnapType::Perpendicular, candidate});
                }
            };
            if (distanceFromCenter <= epsilon) {
                const QPointF towardCursor = cursor - center;
                const qreal cursorDistance = std::hypot(towardCursor.x(), towardCursor.y());
                if (cursorDistance > epsilon) {
                    appendIfOnArc(center + towardCursor * (radius / cursorDistance));
                }
            } else {
                const QPointF radialDirection = fromCenter / distanceFromCenter;
                appendIfOnArc(center + radialDirection * radius);
                appendIfOnArc(center - radialDirection * radius);
            }
            continue;
        }
        if (shape.geometryType != GeometryType::Line) {
            continue;
        }
        for (int index = 0; index + 1 < shape.points.size(); ++index) {
            const QPointF start = shape.points[index];
            const QPointF end = shape.points[index + 1];
            const QPointF direction = end - start;
            const qreal lengthSquared = QPointF::dotProduct(direction, direction);
            if (lengthSquared <= epsilon) {
                continue;
            }
            const qreal projection = QPointF::dotProduct(origin - start, direction) /
                                     lengthSquared;
            if (projection < -epsilon || projection > 1.0 + epsilon) {
                continue;
            }
            const QPointF foot = start + direction * std::clamp(projection, 0.0, 1.0);
            if (std::hypot(origin.x() - foot.x(), origin.y() - foot.y()) <= epsilon) {
                continue;
            }
            candidates.append({SnapType::Perpendicular, foot});
        }
    }
    return candidates;
}

QVector<SnapCandidate> SnapEngine::tangentCandidates(
    const Document &document,
    const QPointF &origin,
    const ViewportTransform &transform,
    const QSize &viewportSize,
    const QVector<int> &excludedShapeIndices) const
{
    QVector<SnapCandidate> candidates;
    if (!settings_.tangent) {
        return candidates;
    }
    for (int shapeIndex = 0; shapeIndex < document.size(); ++shapeIndex) {
        if (excludedShapeIndices.contains(shapeIndex) ||
            !document.isObjectVisible(document.objectIdAt(shapeIndex))) {
            continue;
        }
        const Shape &shape = document[shapeIndex];
        if (!workPlaneMatches(shapeWorkPlaneFrame(shape),
                              transform.workPlaneFrame())) {
            continue;
        }
        candidates += tangentCandidatesForShape(shape,
                                                origin,
                                                transform,
                                                viewportSize);
    }
    return candidates;
}

QVector<SnapCandidate> SnapEngine::tangentCandidatesForShape(
    const Shape &shape,
    const QPointF &origin,
    const ViewportTransform &transform,
    const QSize &viewportSize) const
{
    QVector<SnapCandidate> candidates;
    constexpr qreal epsilon = 1.0e-9;
    if (isDimensionGeometryType(shape.geometryType)) {
        return candidates;
    }
    if (!workPlaneMatches(shapeWorkPlaneFrame(shape),
                          transform.workPlaneFrame())) {
        return candidates;
    }
    if (shape.geometryType == GeometryType::Circle &&
        validateNurbsCurve(shape.nurbs)) {
        return tangentCandidatesForNurbsCurve(shape.nurbs,
                                              origin,
                                              transform,
                                              viewportSize);
    }

    if (shape.geometryType == GeometryType::Circle && shape.points.size() >= 2) {
        const QPointF center = shape.points[0];
        const QPointF edge = shape.points[1];
        const qreal radius = std::hypot(edge.x() - center.x(), edge.y() - center.y());
        if (radius <= epsilon) {
            return candidates;
        }
        const QPointF fromCenter = origin - center;
        const qreal distanceFromCenter = std::hypot(fromCenter.x(), fromCenter.y());
        if (distanceFromCenter < radius - epsilon || distanceFromCenter <= epsilon) {
            return candidates;
        }
        const QPointF radialDirection = fromCenter / distanceFromCenter;
        const QPointF tangentDirection(-radialDirection.y(), radialDirection.x());
        const qreal radiusRatio = radius / distanceFromCenter;
        const qreal radialDistance = radius * radiusRatio;
        const qreal tangentDistance =
            radius * std::sqrt(std::max(0.0, 1.0 - radiusRatio * radiusRatio));
        candidates.append({SnapType::Tangent,
                           center + radialDirection * radialDistance +
                               tangentDirection * tangentDistance});
        if (tangentDistance > epsilon) {
            candidates.append({SnapType::Tangent,
                               center + radialDirection * radialDistance -
                                   tangentDirection * tangentDistance});
        }
        return candidates;
    }

    if (shape.geometryType == GeometryType::Arc &&
        validateNurbsCurve(shape.nurbs)) {
        return tangentCandidatesForNurbsCurve(shape.nurbs,
                                              origin,
                                              transform,
                                              viewportSize);
    }

    if (shape.geometryType == GeometryType::Arc && shape.points.size() >= 3) {
        QPointF center;
        qreal radius = 0.0;
        qreal startAngle = 0.0;
        qreal sweepAngle = 0.0;
        if (makeArcSnapGeometry(shape,
                                &center,
                                &radius,
                                &startAngle,
                                &sweepAngle)) {
            const QPointF fromCenter = origin - center;
            const qreal distanceFromCenter = std::hypot(fromCenter.x(), fromCenter.y());
            if (distanceFromCenter >= radius - epsilon && distanceFromCenter > epsilon) {
                const QPointF radialDirection = fromCenter / distanceFromCenter;
                const QPointF tangentDirection(-radialDirection.y(), radialDirection.x());
                const qreal radiusRatio = radius / distanceFromCenter;
                const qreal radialDistance = radius * radiusRatio;
                const qreal tangentDistance =
                    radius * std::sqrt(std::max(0.0, 1.0 - radiusRatio * radiusRatio));
                const auto appendIfOnArc = [&](const QPointF &candidate) {
                    const qreal candidateAngle = std::atan2(candidate.y() - center.y(),
                                                            candidate.x() - center.x());
                    if (arcAngleIsOnSweep(startAngle, sweepAngle, candidateAngle)) {
                        candidates.append({SnapType::Tangent, candidate});
                    }
                };
                appendIfOnArc(center + radialDirection * radialDistance +
                              tangentDirection * tangentDistance);
                if (tangentDistance > epsilon) {
                    appendIfOnArc(center + radialDirection * radialDistance -
                                  tangentDirection * tangentDistance);
                }
            }
            return candidates;
        }
    }

    if (shape.geometryType == GeometryType::PolyCurve) {
        for (const Shape::NurbsCurve2D &component : shape.components) {
            candidates += tangentCandidatesForNurbsCurve(component,
                                                         origin,
                                                         transform,
                                                         viewportSize);
        }
        return candidates;
    }

    Shape::NurbsCurve2D curve;
    if (subdivisionCurve(shape, &curve)) {
        candidates += tangentCandidatesForNurbsCurve(curve,
                                                     origin,
                                                     transform,
                                                     viewportSize);
    }
    return candidates;
}

QVector<SnapCandidate> SnapEngine::nearCandidatesForScene(
    const Document &document,
    const QPointF &cursor,
    const ViewportTransform &transform,
    const QSize &viewportSize,
    const QVector<int> &excludedShapeIndices,
    qreal snapRadiusPixels,
    int targetShapeIndex,
    int targetComponentIndex) const
{
    QVector<SnapCandidate> candidates;
    if (!settings_.near) {
        return candidates;
    }

    const QPointF cursorScreen = transform.worldToScreen(cursor, viewportSize);
    const auto curveMayBeNearCursor = [&](const Shape::NurbsCurve2D &curve,
                                          const WorkPlaneFrame &frame) {
        QRectF controlHullBounds;
        if (!projectedNurbsControlHullBounds(curve,
                                             frame,
                                             transform,
                                             viewportSize,
                                             &controlHullBounds)) {
            return true;
        }
        const QRectF cursorBounds(
            cursorScreen - QPointF(snapRadiusPixels, snapRadiusPixels),
            QSizeF(snapRadiusPixels * 2.0, snapRadiusPixels * 2.0));
        return screenBoundsOverlap(controlHullBounds, cursorBounds);
    };
    for (int shapeIndex = 0; shapeIndex < document.size(); ++shapeIndex) {
        if ((targetShapeIndex >= 0 && shapeIndex != targetShapeIndex) ||
            excludedShapeIndices.contains(shapeIndex) ||
            !document.isObjectVisible(document.objectIdAt(shapeIndex))) {
            continue;
        }

        const Shape &shape = document[shapeIndex];
        if (!workPlaneMatches(shapeWorkPlaneFrame(shape),
                              transform.workPlaneFrame())) {
            continue;
        }
        if (isDimensionGeometryType(shape.geometryType)) {
            continue;
        }
        qreal nearestDistanceSquared = std::numeric_limits<qreal>::infinity();
        QPointF nearestPoint;
        Point3D nearestWorldPoint;
        bool hasNearestWorldPoint = false;
        int nearestComponentIndex = -1;
        const auto considerPoint = [&](const QPointF &worldPoint) {
            const QPointF screenPoint = transform.worldToScreen(worldPoint, viewportSize);
            const QPointF difference = screenPoint - cursorScreen;
            const qreal distanceSquared = QPointF::dotProduct(difference, difference);
            if (distanceSquared < nearestDistanceSquared) {
                nearestDistanceSquared = distanceSquared;
                nearestPoint = worldPoint;
            }
        };
        const auto considerSegment = [&](const QPointF &start, const QPointF &end) {
            const QPointF startScreen = transform.worldToScreen(start, viewportSize);
            const QPointF endScreen = transform.worldToScreen(end, viewportSize);
            const QPointF direction = endScreen - startScreen;
            const qreal lengthSquared = QPointF::dotProduct(direction, direction);
            const qreal fraction = lengthSquared <= 1.0e-18
                                       ? 0.0
                                       : std::clamp(
                                             QPointF::dotProduct(cursorScreen - startScreen,
                                                                 direction) /
                                                 lengthSquared,
                                             0.0,
                                             1.0);
            considerPoint(start + (end - start) * fraction);
        };

        if (shape.geometryType == GeometryType::Point) {
            if (!shape.points.isEmpty()) {
                considerPoint(shape.points.first());
            }
        } else if (shape.geometryType == GeometryType::Rectangle ||
                   shape.geometryType == GeometryType::Polygon ||
                   shape.geometryType == GeometryType::Picture) {
            const QVector<QPointF> vertices =
                   shape.geometryType == GeometryType::Polygon
                    ? polygonVerticesForShape(shape)
                    : shape.geometryType == GeometryType::Picture
                          ? pictureFrameCorners(shape)
                          : rectangleVertices(shape);
            for (int vertexIndex = 0; vertexIndex < vertices.size(); ++vertexIndex) {
                considerSegment(vertices[vertexIndex],
                                vertices[(vertexIndex + 1) % vertices.size()]);
            }
        } else if (shape.geometryType == GeometryType::PolyCurve) {
            for (int componentIndex = 0;
                 componentIndex < shape.components.size();
                 ++componentIndex) {
                if (targetComponentIndex >= 0 &&
                    componentIndex != targetComponentIndex) {
                    continue;
                }
                const Shape::NurbsCurve2D &component = shape.components[componentIndex];
                ViewportTransform componentTransform = transform;
                const WorkPlaneFrame componentFrame =
                    shapeComponentWorkPlaneFrame(shape, componentIndex);
                if (!curveMayBeNearCursor(component, componentFrame)) {
                    continue;
                }
                componentTransform.setWorkPlaneFrame(componentFrame);
                QPointF componentNearestPoint;
                qreal componentDistanceSquared = 0.0;
                if (nearestPointOnNurbsCurve(component,
                                             cursorScreen,
                                             componentTransform,
                                             viewportSize,
                                             &componentNearestPoint,
                                             &componentDistanceSquared) &&
                    componentDistanceSquared < nearestDistanceSquared) {
                    nearestDistanceSquared = componentDistanceSquared;
                    nearestPoint = componentNearestPoint;
                    nearestComponentIndex = componentIndex;
                    nearestWorldPoint = workPlaneFramePointToWorld(
                        componentNearestPoint, componentFrame);
                    hasNearestWorldPoint = true;
                }
            }
        } else {
            Shape::NurbsCurve2D curve;
            if (subdivisionCurve(shape, &curve)) {
                if (curveMayBeNearCursor(curve, shapeWorkPlaneFrame(shape))) {
                    nearestPointOnNurbsCurve(curve,
                                             cursorScreen,
                                             transform,
                                             viewportSize,
                                             &nearestPoint,
                                             &nearestDistanceSquared);
                }
            } else if (shape.geometryType == GeometryType::Arc) {
                constexpr int arcSegments = 96;
                QPointF previousPoint;
                if (arcSnapPointAtFraction(shape, 0.0, &previousPoint)) {
                    for (int segmentIndex = 1; segmentIndex <= arcSegments;
                         ++segmentIndex) {
                        QPointF currentPoint;
                        if (!arcSnapPointAtFraction(
                                shape,
                                static_cast<qreal>(segmentIndex) / arcSegments,
                                &currentPoint)) {
                            break;
                        }
                        considerSegment(previousPoint, currentPoint);
                        previousPoint = currentPoint;
                    }
                }
            } else if (shape.geometryType == GeometryType::Circle &&
                       shape.points.size() >= 2) {
                constexpr int circleSegments = 96;
                const QPointF center = shape.points[0];
                const QPointF radiusPoint = shape.points[1];
                const qreal radius = std::hypot(radiusPoint.x() - center.x(),
                                                radiusPoint.y() - center.y());
                QPointF previousPoint = center + QPointF(radius, 0.0);
                for (int segmentIndex = 1; segmentIndex <= circleSegments;
                     ++segmentIndex) {
                    const qreal angle = 6.28318530717958647692 * segmentIndex /
                                        circleSegments;
                    const QPointF currentPoint(center.x() + radius * std::cos(angle),
                                               center.y() + radius * std::sin(angle));
                    considerSegment(previousPoint, currentPoint);
                    previousPoint = currentPoint;
                }
            }
        }

        if (nearestDistanceSquared <= snapRadiusPixels * snapRadiusPixels) {
            SnapCandidate candidate{SnapType::Near,
                                    nearestPoint,
                                    shapeIndex,
                                    nearestComponentIndex};
            if (!hasNearestWorldPoint) {
                nearestWorldPoint = workPlaneFramePointToWorld(
                    nearestPoint, shapeWorkPlaneFrame(shape));
            }
            candidate.worldPoint = nearestWorldPoint;
            candidate.hasWorldPoint = true;
            candidate.point = worldPointToWorkPlaneFrame(
                nearestWorldPoint, transform.workPlaneFrame());
            candidates.append(candidate);
        }
    }
    return candidates;
}

SnapResult SnapEngine::findSpatialSnapPoint(const Document &document,
                                           const QPointF &screenPosition,
                                           const Point3D *anchor,
                                           const ViewportTransform &transform,
                                           const QSize &viewportSize,
                                           const QVector<Point3D> &previewPoints,
                                           const QVector<int> &excludedShapeIndices,
                                           bool forceEnabled,
                                           bool includeTangentCandidates) const
{
    SnapResult best;
    if ((!settings_.enabled && !forceEnabled) || viewportSize.isEmpty()) return best;
    const OcclusionPlane occlusionPlane =
        occlusionPlaneAt(screenPosition, excludedShapeIndices);
    qreal bestDistance = 12.0;
    int bestPriority = -1;
    const auto considerWorld = [&](SnapType type, const Point3D &world) {
        QPointF screen;
        if (!transform.worldPointToScreen(world, viewportSize, &screen)) return;
        const qreal distance = std::hypot(screen.x() - screenPosition.x(),
                                          screen.y() - screenPosition.y());
        if (distance > 12.0 ||
            !pointPassesOcclusionPlane(world, occlusionPlane, transform)) {
            return;
        }
        const int priority = type == SnapType::Near ? 0 : 1;
        if (distance <= 12.0 && (priority > bestPriority ||
            (priority == bestPriority && distance <= bestDistance))) {
            bestDistance = distance;
            bestPriority = priority;
            best.type = type;
            best.worldPoint = world;
            best.hasWorldPoint = true;
            // Existing markers retain their screen position; geometry uses
            // the actual world point even when its depth differs from the plane.
            best.point = transform.screenToWorld(screen, viewportSize);
        }
    };
    struct PlaneScene { WorkPlaneFrame frame; Document document; };
    QVector<PlaneScene> scenes;
    QHash<QByteArray, int> sceneByFrame;
    for (int index = 0; index < document.size(); ++index) {
        if (excludedShapeIndices.contains(index) ||
            !document.isObjectVisible(document.objectIdAt(index))) {
            continue;
        }
        const WorkPlaneFrame frame = shapeWorkPlaneFrame(document[index]);
        const SceneObject *sourceObject =
            document.object(document.objectIdAt(index));
        QByteArray key;
        QDataStream stream(&key, QIODevice::WriteOnly);
        for (const Point3D &value : {frame.origin, frame.xAxis, frame.yAxis, frame.normal}) {
            stream << double(value.x) << double(value.y) << double(value.z);
        }
        int sceneIndex = sceneByFrame.value(key, -1);
        if (sceneIndex < 0) {
            sceneIndex = scenes.size();
            sceneByFrame.insert(key, sceneIndex);
            scenes.append({frame, Document{}});
        }
        SceneObject snapObject;
        snapObject.geometry = document[index];
        if (sourceObject != nullptr) {
            snapObject.placementTranslation = sourceObject->placementTranslation;
        }
        scenes[sceneIndex].document.insertObject(
            scenes[sceneIndex].document.size(), snapObject);
    }
    for (const PlaneScene &scene : scenes) {
        const WorkPlaneFrame &frame = scene.frame;
        ViewportTransform shapeTransform = transform;
        shapeTransform.setWorkPlaneFrame(frame);
        const auto consider = [&](const SnapCandidate &candidate) {
            const Point3D world = candidateWorldPoint(candidate, frame);
            considerWorld(candidate.type, world);
        };
        for (const SnapCandidate &candidate :
             snapCandidatesForScene(scene.document, {}, shapeTransform, viewportSize)) {
            consider(candidate);
        }
        QPointF localCursor;
        if (transform.screenToWorkPlane(screenPosition, viewportSize, frame, &localCursor)) {
            for (const SnapCandidate &candidate : nearCandidatesForScene(
                    scene.document, localCursor, shapeTransform, viewportSize)) {
                consider(candidate);
            }
            if (anchor != nullptr) {
                const QPointF localAnchor = worldPointToWorkPlaneFrame(*anchor, frame);
                for (const SnapCandidate &candidate : perpendicularCandidates(
                        scene.document, localAnchor, localCursor, shapeTransform, viewportSize)) {
                    consider(candidate);
                }
                if (includeTangentCandidates &&
                    std::abs(signedDistanceFromWorkPlaneFrame(*anchor, frame)) <= 1.0e-8) {
                    for (const SnapCandidate &candidate : tangentCandidates(
                            scene.document, localAnchor, shapeTransform, viewportSize)) {
                        consider(candidate);
                    }
                }
            }
        }
    }
    const bool anySnapModeEnabled = settings_.endpoint || settings_.midpoint ||
                                    settings_.intersection || settings_.center ||
                                    settings_.perpendicular || settings_.tangent ||
                                    settings_.near || settings_.controlPoint;
    if (anySnapModeEnabled) {
        // The add-on self-snaps Point by Line/Arcs preview points whenever
        // any OSnap mode is active, even if endpoint snapping itself is off.
        // Like its modal handler, let the closest self point replace the
        // scene snap only when it is strictly closer to the cursor.
        for (const Point3D &point : previewPoints) {
            QPointF screen;
            if (!transform.worldPointToScreen(point, viewportSize, &screen)) {
                continue;
            }
            const qreal distance = std::hypot(screen.x() - screenPosition.x(),
                                              screen.y() - screenPosition.y());
            if (distance < 12.0 && distance < bestDistance &&
                pointPassesOcclusionPlane(point, occlusionPlane, transform)) {
                bestDistance = distance;
                bestPriority = 1;
                best.type = SnapType::Endpoint;
                best.worldPoint = point;
                best.hasWorldPoint = true;
                best.point = transform.screenToWorld(screen, viewportSize);
            }
        }
    }
    return best;
}

bool SnapEngine::findAxisIntersectionWithHoveredEdge(
    const Document &document,
    const QPointF &screenPosition,
    const Point3D &axisOrigin,
    const Point3D &axisDirection,
    const Point3D &snapWorldPoint,
    const ViewportTransform &transform,
    const QSize &viewportSize,
    Point3D *intersection,
    qreal snapRadiusPixels) const
{
    if (intersection == nullptr || viewportSize.isEmpty() ||
        !std::isfinite(snapRadiusPixels) || snapRadiusPixels < 0.0) {
        return false;
    }
    const qreal axisLengthSquared = axisDirection.x * axisDirection.x +
                                    axisDirection.y * axisDirection.y +
                                    axisDirection.z * axisDirection.z;
    if (axisLengthSquared <= 1.0e-18) return false;

    bool found = false;
    qreal bestSnapGapSquared = std::numeric_limits<qreal>::infinity();
    qreal bestScreenGapSquared = snapRadiusPixels * snapRadiusPixels;
    const auto considerWorldSegment = [&](const Point3D &first,
                                          const Point3D &second) {
        QPointF firstScreen;
        QPointF secondScreen;
        if (!transform.worldPointToScreen(first, viewportSize, &firstScreen) ||
            !transform.worldPointToScreen(second, viewportSize, &secondScreen)) {
            return;
        }
        const QPointF screenEdge = secondScreen - firstScreen;
        const qreal screenLengthSquared = QPointF::dotProduct(screenEdge,
                                                               screenEdge);
        const qreal screenFraction = screenLengthSquared <= 1.0e-18
            ? 0.0
            : std::clamp(QPointF::dotProduct(screenPosition - firstScreen,
                                             screenEdge) /
                             screenLengthSquared,
                         0.0, 1.0);
        const QPointF closestScreen = firstScreen + screenEdge * screenFraction;
        const QPointF screenDelta = closestScreen - screenPosition;
        const qreal screenGapSquared = QPointF::dotProduct(screenDelta,
                                                            screenDelta);
        if (screenGapSquared > snapRadiusPixels * snapRadiusPixels) return;

        const Point3D edge = {second.x - first.x, second.y - first.y,
                              second.z - first.z};
        const qreal edgeLengthSquared = edge.x * edge.x + edge.y * edge.y +
                                        edge.z * edge.z;
        if (edgeLengthSquared <= 1.0e-18) return;
        const qreal snapFraction = std::clamp(
            ((snapWorldPoint.x - first.x) * edge.x +
             (snapWorldPoint.y - first.y) * edge.y +
             (snapWorldPoint.z - first.z) * edge.z) / edgeLengthSquared,
            0.0, 1.0);
        const Point3D nearestToSnap = {
            first.x + edge.x * snapFraction,
            first.y + edge.y * snapFraction,
            first.z + edge.z * snapFraction};
        const qreal snapDx = nearestToSnap.x - snapWorldPoint.x;
        const qreal snapDy = nearestToSnap.y - snapWorldPoint.y;
        const qreal snapDz = nearestToSnap.z - snapWorldPoint.z;
        const qreal snapGapSquared = snapDx * snapDx + snapDy * snapDy +
                                     snapDz * snapDz;
        if (snapGapSquared > bestSnapGapSquared + 1.0e-12 ||
            (std::abs(snapGapSquared - bestSnapGapSquared) <= 1.0e-12 &&
             screenGapSquared >= bestScreenGapSquared)) {
            return;
        }

        const qreal axisEdgeDot = axisDirection.x * edge.x +
                                  axisDirection.y * edge.y +
                                  axisDirection.z * edge.z;
        const Point3D offset = {axisOrigin.x - first.x,
                                axisOrigin.y - first.y,
                                axisOrigin.z - first.z};
        const qreal axisOffset = axisDirection.x * offset.x +
                                 axisDirection.y * offset.y +
                                 axisDirection.z * offset.z;
        const qreal edgeOffset = edge.x * offset.x + edge.y * offset.y +
                                 edge.z * offset.z;
        const qreal denominator = axisLengthSquared * edgeLengthSquared -
                                  axisEdgeDot * axisEdgeDot;
        if (denominator <= 1.0e-14 * axisLengthSquared * edgeLengthSquared) {
            return;
        }
        const qreal axisParameter =
            (axisEdgeDot * edgeOffset - edgeLengthSquared * axisOffset) /
            denominator;
        const Point3D candidate = {
            axisOrigin.x + axisDirection.x * axisParameter,
            axisOrigin.y + axisDirection.y * axisParameter,
            axisOrigin.z + axisDirection.z * axisParameter};
        if (!std::isfinite(candidate.x) || !std::isfinite(candidate.y) ||
            !std::isfinite(candidate.z)) {
            return;
        }
        found = true;
        bestSnapGapSquared = snapGapSquared;
        bestScreenGapSquared = screenGapSquared;
        *intersection = candidate;
    };
    const auto appendCurveSegments = [&](const Shape::NurbsCurve2D &curve,
                                         const WorkPlaneFrame &frame) {
        if (!validateNurbsCurve(curve)) return;
        const QVector<double> fullKnots = expandedNurbsKnotVector(curve);
        constexpr int samplesPerSpan = 64;
        for (int span = curve.degree; span < curve.controlPoints.size(); ++span) {
            const qreal start = fullKnots[span];
            const qreal end = fullKnots[span + 1];
            if (end <= start) continue;
            QPointF previous;
            if (!evaluateNurbsPoint(curve, start, &previous)) continue;
            for (int sample = 1; sample <= samplesPerSpan; ++sample) {
                const qreal parameter = start + (end - start) * sample /
                                                   samplesPerSpan;
                QPointF current;
                if (!evaluateNurbsPoint(curve, parameter, &current)) continue;
                considerWorldSegment(
                    workPlaneFramePointToWorld(previous, frame),
                    workPlaneFramePointToWorld(current, frame));
                previous = current;
            }
        }
    };
    const auto appendClosedVertices = [&](const QVector<QPointF> &vertices,
                                          const WorkPlaneFrame &frame) {
        for (int index = 0; index < vertices.size(); ++index) {
            considerWorldSegment(
                workPlaneFramePointToWorld(vertices[index], frame),
                workPlaneFramePointToWorld(vertices[(index + 1) % vertices.size()],
                                           frame));
        }
    };

    for (int shapeIndex = 0; shapeIndex < document.size(); ++shapeIndex) {
        if (!document.isObjectVisible(document.objectIdAt(shapeIndex))) continue;
        const Shape &shape = document[shapeIndex];
        const WorkPlaneFrame frame = shapeWorkPlaneFrame(shape);
        if (shape.geometryType == GeometryType::PolyCurve) {
            for (const Shape::NurbsCurve2D &component : shape.components) {
                appendCurveSegments(component, frame);
            }
            continue;
        }
        Shape::NurbsCurve2D curve;
        if (subdivisionCurve(shape, &curve)) {
            appendCurveSegments(curve, frame);
            continue;
        }
        if (shape.geometryType == GeometryType::Rectangle) {
            appendClosedVertices(rectangleVertices(shape), frame);
        } else if (shape.geometryType == GeometryType::Polygon) {
            appendClosedVertices(polygonVerticesForShape(shape), frame);
        } else if (shape.geometryType == GeometryType::Picture) {
            appendClosedVertices(pictureFrameCorners(shape), frame);
        }
    }
    return found;
}

SnapResult SnapEngine::findSnapPoint(const Document &document,
                                     const QPointF &rawPoint,
                                     bool drawingSnapActive,
                                     const QVector<QPointF> &pendingPoints,
                                     const ViewportTransform &transform,
                                     const QSize &viewportSize,
                                     const QVector<int> &excludedShapeIndices,
                                     bool forceEnabled,
                                     bool includeTangentCandidates) const
{
    SnapResult best;
    if ((!settings_.enabled && !forceEnabled) || !drawingSnapActive) {
        return best;
    }

    // Each planar curve owns a local frame whose origin is often its first
    // picked point. Compare actual planes here, then express eligible geometry
    // in the active drawing frame before generating snaps. Exact frame equality
    // incorrectly drops every other curve that lies on the same plane.
    const WorkPlaneFrame activeFrame = transform.workPlaneFrame();
    if (!isValidWorkPlaneFrame(activeFrame)) {
        return best;
    }
    QVector<SceneObject> coplanarObjects;
    coplanarObjects.reserve(document.size());
    for (int shapeIndex = 0; shapeIndex < document.size(); ++shapeIndex) {
        if (excludedShapeIndices.contains(shapeIndex) ||
            !document.isObjectVisible(document.objectIdAt(shapeIndex))) {
            continue;
        }
        const Shape &shape = document[shapeIndex];
        const SceneObject *sourceObject =
            document.object(document.objectIdAt(shapeIndex));
        const WorkPlaneFrame shapeFrame = shapeWorkPlaneFrame(shape);
        const bool spatialSurface = shape.geometryType == GeometryType::NurbsSurface ||
                                    shape.geometryType == GeometryType::NurbsSolid;
        if (!spatialSurface &&
            !workPlaneFramesAreCoplanar(shapeFrame, activeFrame)) {
            continue;
        }
        SceneObject snapObject;
        snapObject.geometry = spatialSurface ? shape
                                             : mapShapeBetweenFrames(shape, activeFrame);
        if (sourceObject != nullptr) {
            snapObject.placementTranslation = sourceObject->placementTranslation;
        }
        coplanarObjects.append(std::move(snapObject));
    }
    Document coplanarScene;
    coplanarScene.replaceObjects(coplanarObjects);

    const QPointF cursorScreen = transform.worldToScreen(rawPoint, viewportSize);
    const OcclusionPlane occlusionPlane =
        occlusionPlaneAt(cursorScreen, excludedShapeIndices);
    constexpr qreal snapRadiusPixels = 12.0;
    constexpr qreal nearSnapRadiusPixels = 18.0;
    qreal bestDistance = snapRadiusPixels;
    int bestPriority = -1;
    const auto consider = [&](const SnapCandidate &candidate) {
        QPointF candidateScreen;
        const Point3D candidateWorld = candidateWorldPoint(candidate, activeFrame);
        if (!transform.worldPointToScreen(candidateWorld,
                                          viewportSize,
                                          &candidateScreen)) {
            return;
        }
        const qreal distance = std::hypot(candidateScreen.x() - cursorScreen.x(),
                                           candidateScreen.y() - cursorScreen.y());
        // Near is the fallback snap. If a specific enabled OSnap such as
        // Center is also within range, do not let a closer point on the curve
        // hide that intentional target.
        const int priority = candidate.type == SnapType::Near ? 0 : 1;
        const qreal candidateRadius = candidate.type == SnapType::Near
                                          ? nearSnapRadiusPixels
                                          : snapRadiusPixels;
        if (distance <= candidateRadius &&
            pointPassesOcclusionPlane(candidateWorld,
                                      occlusionPlane,
                                      transform) &&
            (priority > bestPriority ||
             (priority == bestPriority && distance <= bestDistance))) {
            bestDistance = distance;
            bestPriority = priority;
            best.type = candidate.type;
            best.point = worldPointToWorkPlaneFrame(candidateWorld, activeFrame);
            best.worldPoint = candidateWorld;
            best.hasWorldPoint = true;
        }
    };

    for (const SnapCandidate &candidate :
         nearCandidatesForScene(coplanarScene,
                                rawPoint,
                                transform,
                                viewportSize,
                                {},
                                nearSnapRadiusPixels)) {
        consider(candidate);
    }

    for (const SnapCandidate &candidate :
         snapCandidatesForScene(coplanarScene,
                               {},
                               transform,
                               viewportSize)) {
        consider(candidate);
    }
    if (!pendingPoints.isEmpty()) {
        for (const SnapCandidate &candidate :
             perpendicularCandidates(coplanarScene,
                                     pendingPoints.back(),
                                     rawPoint,
                                     transform,
                                     viewportSize)) {
            consider(candidate);
        }
        if (includeTangentCandidates) {
            for (const SnapCandidate &candidate :
                 tangentCandidates(coplanarScene,
                                   pendingPoints.back(),
                                   transform,
                                   viewportSize)) {
                consider(candidate);
            }
        }
    }
    return best;
}

DragSnapResult SnapEngine::findDragSnap(
    const Document &document,
    const QVector<int> &selectedShapeIndices,
    const ViewportTransform &transform,
    const QSize &viewportSize,
    bool forceEnabled,
    bool includeNear) const
{
    DragSnapResult best;
    if ((!settings_.enabled && !forceEnabled) || selectedShapeIndices.isEmpty()) {
        return best;
    }

    const QSet<int> selectedSet(selectedShapeIndices.cbegin(),
                                selectedShapeIndices.cend());
    OcclusionPlaneCache occlusionPlaneCache;
    const auto targetPassesOcclusion = [&](const Point3D &worldPoint,
                                           const QPointF &screenPoint) {
        const OcclusionPlane plane = occlusionPlaneAt(
            screenPoint, selectedShapeIndices, &occlusionPlaneCache);
        return pointPassesOcclusionPlane(worldPoint, plane, transform);
    };
    bool hasTarget = false;
    bool hasNearTarget = false;
    for (int index = 0; index < document.size(); ++index) {
        if (selectedSet.contains(index) ||
            !document.isObjectVisible(document.objectIdAt(index))) {
            continue;
        }
        hasTarget = true;
        const auto type = document[index].geometryType;
        hasNearTarget |= type != GeometryType::NurbsSolid &&
                         type != GeometryType::NurbsSurface &&
                         type != GeometryType::Point;
    }
    if (!hasTarget) {
        return best;
    }

    QVector<SnapCandidate> sourceCandidates;
    struct WorldCandidate { SnapType type; Point3D point; int shapeIndex; };
    QVector<WorldCandidate> worldSources;
    for (const int shapeIndex : selectedShapeIndices) {
        if (shapeIndex >= 0 && shapeIndex < document.size()) {
            const Shape &shape = document[shapeIndex];
            const SceneObject *sceneObject =
                document.object(document.objectIdAt(shapeIndex));
            const Point3D worldOffset = sceneObject != nullptr
                                            ? sceneObject->placementTranslation
                                            : Point3D{};
            QVector<SnapCandidate> candidates = snapCandidatesForShape(
                shape, transform, viewportSize, worldOffset);
            for (SnapCandidate candidate : candidates) {
                const Point3D world = candidateWorldPoint(
                    candidate, shapeWorkPlaneFrame(shape));
                worldSources.append({candidate.type, world, shapeIndex});
                if (std::abs(signedDistanceFromWorkPlaneFrame(
                        world, transform.workPlaneFrame())) <= 1.0e-7) {
                    candidate.point = worldPointToWorkPlaneFrame(world, transform.workPlaneFrame());
                    candidate.worldPoint = world;
                    candidate.hasWorldPoint = true;
                    sourceCandidates.append(candidate);
                }
            }
        }
    }
    if (worldSources.isEmpty()) {
        return best;
    }

    constexpr qreal snapRadiusPixels = 12.0;
    QVector<SnapCandidate> targetCandidates =
        snapCandidatesForScene(document,
                               selectedShapeIndices,
                               transform,
                               viewportSize);
    for (int index = 0; index < document.size(); ++index) {
        if (selectedSet.contains(index) ||
            !document.isObjectVisible(document.objectIdAt(index))) {
            continue;
        }
        const Shape &shape = document[index];
        const SceneObject *sceneObject =
            document.object(document.objectIdAt(index));
        const Point3D worldOffset = sceneObject != nullptr
                                        ? sceneObject->placementTranslation
                                        : Point3D{};
        const WorkPlaneFrame frame = shapeWorkPlaneFrame(shape);
        if (workPlaneMatches(frame, transform.workPlaneFrame())) {
            continue;
        }
        // Project only targets on the drag plane: different local origins and
        // axes do not imply different geometric planes.
        for (SnapCandidate candidate : snapCandidatesForShape(
                 shape, transform, viewportSize, worldOffset)) {
            if ((candidate.type == SnapType::Endpoint && !settings_.endpoint) ||
                (candidate.type == SnapType::Midpoint && !settings_.midpoint) ||
                (candidate.type == SnapType::Center && !settings_.center) ||
                (candidate.type == SnapType::ControlPoint && !settings_.controlPoint)) {
                continue;
            }
            const Point3D world = candidateWorldPoint(candidate, frame);
            if (std::abs(signedDistanceFromWorkPlaneFrame(world, transform.workPlaneFrame())) > 1.0e-7) {
                continue;
            }
            candidate.point = worldPointToWorkPlaneFrame(world, transform.workPlaneFrame());
            candidate.worldPoint = world;
            candidate.hasWorldPoint = true;
            candidate.shapeIndex = index;
            targetCandidates.append(candidate);
        }
    }
    qreal bestDistance = snapRadiusPixels;
    int bestPriority = -1;
    const auto consider = [&](SnapType type,
                              const QPointF &sourcePoint,
                              const QPointF &targetPoint,
                              int targetShapeIndex,
                              int targetComponentIndex) {
        const QPointF sourceScreen = transform.worldToScreen(sourcePoint, viewportSize);
        const QPointF targetScreen = transform.worldToScreen(targetPoint, viewportSize);
        const Point3D targetWorld = workPlaneFramePointToWorld(
            targetPoint, transform.workPlaneFrame());
        const qreal distance = std::hypot(targetScreen.x() - sourceScreen.x(),
                                           targetScreen.y() - sourceScreen.y());
        // As in point snapping, Near is a fallback: an enabled, specific
        // target such as Endpoint wins whenever it is within snap range.
        const int priority = type == SnapType::Near ? 0 : 1;
        if (distance <= snapRadiusPixels &&
            targetPassesOcclusion(targetWorld, targetScreen) &&
            (priority > bestPriority ||
             (priority == bestPriority && distance <= bestDistance))) {
            bestDistance = distance;
            bestPriority = priority;
            best.type = type;
            best.sourcePoint = sourcePoint;
            best.targetPoint = targetPoint;
            best.translation = targetPoint - sourcePoint;
            best.targetShapeIndex = targetShapeIndex;
            best.targetComponentIndex = targetComponentIndex;
        }
    };

    if (includeNear && settings_.near && hasNearTarget) {
        for (const SnapCandidate &source : sourceCandidates) {
            const QVector<SnapCandidate> nearTargets = nearCandidatesForScene(
                document,
                source.point,
                transform,
                viewportSize,
                selectedShapeIndices);
            for (const SnapCandidate &target : nearTargets) {
                consider(SnapType::Near,
                         source.point,
                         target.point,
                         target.shapeIndex,
                         target.componentIndex);
            }
        }
    }

    QVector<SnapCandidate> planarTargetCandidates;
    planarTargetCandidates.reserve(targetCandidates.size());
    ScreenCandidateIndex planarTargets;
    const WorkPlaneFrame &activeFrame = transform.workPlaneFrame();
    for (SnapCandidate candidate : targetCandidates) {
        if (candidate.hasWorldPoint) {
            if (std::abs(signedDistanceFromWorkPlaneFrame(candidate.worldPoint,
                                                          activeFrame)) > 1.0e-7) {
                // Off-plane targets are compared in the spatial screen-space
                // pass below. Treating their world XY as drawing-plane
                // coordinates loses height and can make a drag jump on
                // snap release.
                continue;
            }
            candidate.point = worldPointToWorkPlaneFrame(candidate.worldPoint,
                                                         activeFrame);
        }
        const int targetIndex = planarTargetCandidates.size();
        planarTargetCandidates.append(candidate);
        planarTargets.append(transform.worldToScreen(candidate.point,
                                                     viewportSize), targetIndex);
    }
    for (const SnapCandidate &source : sourceCandidates) {
        const auto sourceScreen = transform.worldToScreen(source.point, viewportSize);
        for (const int targetIndex : planarTargets.nearby(sourceScreen)) {
            const auto &target = planarTargetCandidates[targetIndex];
            consider(target.type,
                     source.point,
                     target.point,
                     target.shapeIndex,
                     target.componentIndex);
        }
    }

    // When moving a line, its endpoints are the source snap points. Generate
    // tangent targets from the opposite endpoint so translating an already
    // tangent line can attach its endpoint to the curve's tangent point.
    if (settings_.endpoint && settings_.tangent) {
        for (const int shapeIndex : selectedShapeIndices) {
            if (shapeIndex < 0 || shapeIndex >= document.size()) {
                continue;
            }
            const Shape &shape = document[shapeIndex];
            if (shape.geometryType != GeometryType::Line) {
                continue;
            }

            if (!workPlaneFramesAreCoplanar(shapeWorkPlaneFrame(shape),
                                           transform.workPlaneFrame())) continue;

            const QVector<QPointF> &linePoints = shape.nurbs.controlPoints.isEmpty()
                                                     ? shape.points
                                                     : shape.nurbs.controlPoints;
            for (int segment = 0; segment + 1 < linePoints.size(); ++segment) {
                const QPointF endpoints[2]{linePoints[segment], linePoints[segment + 1]};
                for (int endpoint = 0; endpoint < 2; ++endpoint) {
                    const QPointF sourcePoint = mapPointBetweenFrames(
                        endpoints[endpoint], shapeWorkPlaneFrame(shape),
                        transform.workPlaneFrame());
                    const QPointF fixedPoint = mapPointBetweenFrames(
                        endpoints[1 - endpoint], shapeWorkPlaneFrame(shape),
                        transform.workPlaneFrame());
                    for (const SnapCandidate &target :
                         tangentCandidates(document,
                                           fixedPoint,
                                           transform,
                                           viewportSize,
                                           selectedShapeIndices)) {
                        consider(SnapType::Tangent,
                                 sourcePoint,
                                 target.point,
                                 -1,
                                 -1);
                    }
                }
            }
        }
    }
    // Object dragging can translate the entire planar curve in world XYZ.
    // Match actual endpoint projections, then retain the full displacement;
    // flattening either endpoint into the drag frame loses its depth.
    ScreenCandidateIndex spatialSources;
    QVector<QPointF> sourceScreens(worldSources.size());
    for (int i = 0; i < worldSources.size(); ++i) {
        if (transform.worldPointToScreen(worldSources[i].point, viewportSize,
                                         &sourceScreens[i])) {
            spatialSources.append(sourceScreens[i], i);
        }
    }
    for (int index = 0; index < document.size(); ++index) {
        if (selectedSet.contains(index) ||
            !document.isObjectVisible(document.objectIdAt(index))) continue;
        const Shape &targetShape = document[index];
        const SceneObject *sceneObject =
            document.object(document.objectIdAt(index));
        const Point3D worldOffset = sceneObject != nullptr
                                        ? sceneObject->placementTranslation
                                        : Point3D{};
        for (const SnapCandidate &target : snapCandidatesForShape(
                 targetShape, transform, viewportSize, worldOffset)) {
            if ((target.type == SnapType::Endpoint && !settings_.endpoint) ||
                (target.type == SnapType::Midpoint && !settings_.midpoint) ||
                (target.type == SnapType::Center && !settings_.center) ||
                (target.type == SnapType::ControlPoint && !settings_.controlPoint) ||
                target.type == SnapType::Near || target.type == SnapType::Intersection) continue;
            const Point3D targetWorld = candidateWorldPoint(
                target, shapeWorkPlaneFrame(targetShape));
            QPointF targetScreen;
            if (!transform.worldPointToScreen(targetWorld, viewportSize, &targetScreen)) continue;
            if (!targetPassesOcclusion(targetWorld, targetScreen)) continue;
            for (const int sourceIndex : spatialSources.nearby(targetScreen)) {
                const auto &source = worldSources[sourceIndex];
                const auto &sourceScreen = sourceScreens[sourceIndex];
                const qreal distance = std::hypot(targetScreen.x()-sourceScreen.x(),
                                                   targetScreen.y()-sourceScreen.y());
                if (distance > snapRadiusPixels ||
                    (bestPriority == 1 && distance > bestDistance)) continue;
                bestDistance = distance;
                bestPriority = 1;
                best.type = target.type;
                best.sourcePoint = std::abs(signedDistanceFromWorkPlaneFrame(
                    source.point, transform.workPlaneFrame())) <= 1.0e-7
                    ? worldPointToWorkPlaneFrame(source.point, transform.workPlaneFrame())
                    : transform.screenToWorld(sourceScreen, viewportSize);
                best.targetPoint = std::abs(signedDistanceFromWorkPlaneFrame(
                    targetWorld, transform.workPlaneFrame())) <= 1.0e-7
                    ? worldPointToWorkPlaneFrame(targetWorld, transform.workPlaneFrame())
                    : transform.screenToWorld(targetScreen, viewportSize);
                best.translation = best.targetPoint-best.sourcePoint;
                best.targetShapeIndex = index;
                best.targetComponentIndex = target.componentIndex;
                best.worldSourcePoint = source.point;
                best.worldTargetPoint = targetWorld;
                best.worldTranslation = {targetWorld.x-source.point.x,
                                         targetWorld.y-source.point.y,
                                         targetWorld.z-source.point.z};
                best.hasWorldTranslation = true;
            }
        }
    }
    return best;
}

DragSnapResult SnapEngine::trackNearDragSnap(
    const Document &document,
    const QVector<int> &selectedShapeIndices,
    const QPointF &sourcePoint,
    int targetShapeIndex,
    int targetComponentIndex,
    const ViewportTransform &transform,
    const QSize &viewportSize,
    qreal snapRadiusPixels) const
{
    DragSnapResult result;
    if (!settings_.enabled || !settings_.near || targetShapeIndex < 0 ||
        targetShapeIndex >= document.size() ||
        selectedShapeIndices.contains(targetShapeIndex)) {
        return result;
    }

    const QVector<SnapCandidate> targets = nearCandidatesForScene(
        document,
        sourcePoint,
        transform,
        viewportSize,
        selectedShapeIndices,
        snapRadiusPixels,
        targetShapeIndex,
        targetComponentIndex);
    if (targets.isEmpty()) {
        return result;
    }

    const SnapCandidate &target = targets.first();
    const Point3D targetWorld = target.hasWorldPoint
                                    ? target.worldPoint
                                    : workPlaneFramePointToWorld(
                                          target.point,
                                          transform.workPlaneFrame());
    QPointF targetScreen;
    if (!transform.worldPointToScreen(targetWorld,
                                      viewportSize,
                                      &targetScreen)) {
        return result;
    }
    const OcclusionPlane occlusionPlane =
        occlusionPlaneAt(targetScreen, selectedShapeIndices);
    if (!pointPassesOcclusionPlane(targetWorld,
                                   occlusionPlane,
                                   transform)) {
        return result;
    }
    result.type = SnapType::Near;
    result.sourcePoint = sourcePoint;
    result.targetPoint = target.point;
    result.translation = target.point - sourcePoint;
    result.targetShapeIndex = target.shapeIndex;
    result.targetComponentIndex = target.componentIndex;
    return result;
}

DragSnapResult SnapEngine::findControlPointSnap(
    const Document &document,
    int selectedShapeIndex,
    int selectedControlPointIndex,
    const QPointF &controlPoint,
    const ViewportTransform &transform,
    const QSize &viewportSize) const
{
    DragSnapResult best;
    if (!settings_.enabled || selectedShapeIndex < 0 ||
        selectedShapeIndex >= document.size()) {
        return best;
    }

    const Shape &sourceShape = document[selectedShapeIndex];
    WorkPlaneFrame sourceFrame = shapeWorkPlaneFrame(sourceShape);
    if (sourceShape.geometryType == GeometryType::PolyCurve) {
        int remainingControlPointIndex = selectedControlPointIndex;
        bool foundComponent = false;
        for (int componentIndex = 0;
             componentIndex < sourceShape.components.size();
             ++componentIndex) {
            const Shape::NurbsCurve2D &component =
                sourceShape.components[componentIndex];
            if (remainingControlPointIndex < component.controlPoints.size()) {
                sourceFrame = shapeComponentWorkPlaneFrame(sourceShape,
                                                           componentIndex);
                foundComponent = true;
                break;
            }
            remainingControlPointIndex -= component.controlPoints.size();
        }
        if (!foundComponent) {
            return best;
        }
    }

    const SceneObject *sourceObject =
        document.object(document.objectIdAt(selectedShapeIndex));
    const Point3D sourceWorldOffset = sourceObject != nullptr
                                          ? sourceObject->placementTranslation
                                          : Point3D{};
    WorkPlaneFrame placedSourceFrame = sourceFrame;
    placedSourceFrame.origin.x += sourceWorldOffset.x;
    placedSourceFrame.origin.y += sourceWorldOffset.y;
    placedSourceFrame.origin.z += sourceWorldOffset.z;
    const Point3D sourceWorld =
        workPlaneFramePointToWorld(controlPoint, placedSourceFrame);
    QPointF sourceScreen;
    if (!transform.worldPointToScreen(sourceWorld,
                                      viewportSize,
                                      &sourceScreen)) {
        return best;
    }
    const WorkPlaneFrame &displayFrame = transform.workPlaneFrame();
    constexpr qreal snapRadiusPixels = 20.0;
    qreal bestDistance = snapRadiusPixels;
    int bestPriority = -1;
    OcclusionPlaneCache occlusionPlaneCache;

    const auto consider = [&](SnapType type,
                              const Point3D &targetWorld,
                              int targetShapeIndex,
                              int targetComponentIndex) {
        QPointF targetScreen;
        if (!transform.worldPointToScreen(targetWorld,
                                          viewportSize,
                                          &targetScreen)) {
            return;
        }
        const qreal distance = std::hypot(targetScreen.x() - sourceScreen.x(),
                                          targetScreen.y() - sourceScreen.y());
        if (distance <= snapRadiusPixels) {
            const OcclusionPlane plane = occlusionPlaneAt(
                targetScreen, {}, &occlusionPlaneCache);
            if (!pointPassesOcclusionPlane(targetWorld, plane, transform)) {
                return;
            }
        }
        QPointF targetInSourcePlane;
        if (!transform.screenToWorkPlane(targetScreen,
                                         viewportSize,
                                         placedSourceFrame,
                                         &targetInSourcePlane)) {
            return;
        }
        const Point3D targetOnSourcePlane =
            workPlaneFramePointToWorld(targetInSourcePlane,
                                       placedSourceFrame);
        constexpr qreal tieTolerancePixels = 1.0e-6;
        // Specific OSnaps take precedence over Near, even if the nearest point
        // on a curve happens to be a little closer to the dragged CV.
        const int priority = type == SnapType::Near ? 0 : 1;
        const bool closer = distance < bestDistance - tieTolerancePixels;
        const bool preferredTie =
            std::abs(distance - bestDistance) <= tieTolerancePixels &&
            (best.type == SnapType::None ||
             (priority == bestPriority && type == SnapType::Endpoint &&
              best.type != SnapType::Endpoint));
        if (distance <= snapRadiusPixels &&
            (priority > bestPriority ||
             (priority == bestPriority && (closer || preferredTie)))) {
            bestDistance = distance;
            bestPriority = priority;
            best.type = type;
            best.sourcePoint = worldPointToWorkPlaneFrame(sourceWorld,
                                                          displayFrame);
            best.targetPoint = worldPointToWorkPlaneFrame(targetOnSourcePlane,
                                                          displayFrame);
            // The viewport applies this delta to the selected CV's local
            // NURBS coordinates, which may use a different frame from the
            // target curve and the current drawing plane.
            best.translation = targetInSourcePlane - controlPoint;
            best.targetShapeIndex = targetShapeIndex;
            best.targetComponentIndex = targetComponentIndex;
            best.worldSourcePoint = sourceWorld;
            best.worldTargetPoint = targetOnSourcePlane;
            best.worldTranslation = {
                targetOnSourcePlane.x - sourceWorld.x,
                targetOnSourcePlane.y - sourceWorld.y,
                targetOnSourcePlane.z - sourceWorld.z};
            best.hasWorldTranslation = true;
        }
    };

    const auto snapTypeEnabled = [this](SnapType type) {
        switch (type) {
        case SnapType::Endpoint: return settings_.endpoint;
        case SnapType::Midpoint: return settings_.midpoint;
        case SnapType::Intersection: return settings_.intersection;
        case SnapType::Center: return settings_.center;
        case SnapType::Perpendicular: return settings_.perpendicular;
        case SnapType::Tangent: return settings_.tangent;
        case SnapType::ControlPoint: return settings_.controlPoint;
        case SnapType::Near: return settings_.near;
        case SnapType::None: return false;
        }
        return false;
    };

    Document nearScene;
    QVector<QPair<int, int>> nearTargetIndices;
    const auto appendNearTarget = [&](const Shape &nearShape,
                                     const WorkPlaneFrame &nearFrame,
                                     const Point3D &worldOffset,
                                     int shapeIndex,
                                     int componentIndex) {
        WorkPlaneFrame placedFrame = nearFrame;
        placedFrame.origin.x += worldOffset.x;
        placedFrame.origin.y += worldOffset.y;
        placedFrame.origin.z += worldOffset.z;
        if (!workPlaneFramesAreCoplanar(placedSourceFrame, placedFrame)) {
            return;
        }

        Shape mapped = mapShapeBetweenFrames(nearShape, placedSourceFrame);
        const QPointF localOffset =
            worldPointToWorkPlaneFrame(placedFrame.origin, placedSourceFrame) -
            worldPointToWorkPlaneFrame(nearFrame.origin, placedSourceFrame);
        for (QPointF &point : mapped.points) {
            point += localOffset;
        }
        for (QPointF &point : mapped.nurbs.controlPoints) {
            point += localOffset;
        }
        for (Shape::NurbsCurve2D &component : mapped.components) {
            for (QPointF &point : component.controlPoints) {
                point += localOffset;
            }
        }
        mapped.workPlaneFrame = placedSourceFrame;
        for (WorkPlaneFrame &frame : mapped.componentWorkPlaneFrames) {
            frame = placedSourceFrame;
        }

        SceneObject nearObject;
        nearObject.geometry = std::move(mapped);
        const int nearIndex = nearScene.size();
        nearScene.insertObject(nearIndex, std::move(nearObject));
        nearTargetIndices.append(qMakePair(shapeIndex, componentIndex));
    };

    for (int shapeIndex = 0; shapeIndex < document.size(); ++shapeIndex) {
        const ObjectId objectId = document.objectIdAt(shapeIndex);
        if (!document.isObjectVisible(objectId)) {
            continue;
        }

        const Shape &shape = document[shapeIndex];
        if (shapeIndex == selectedShapeIndex) {
            if (!settings_.controlPoint) {
                continue;
            }

            if (shape.geometryType == GeometryType::PolyCurve) {
                int flattenedControlPointIndex = 0;
                for (int componentIndex = 0;
                     componentIndex < shape.components.size();
                     ++componentIndex) {
                    const Shape::NurbsCurve2D &component =
                        shape.components[componentIndex];
                    WorkPlaneFrame componentFrame =
                        shapeComponentWorkPlaneFrame(shape, componentIndex);
                    componentFrame.origin.x += sourceWorldOffset.x;
                    componentFrame.origin.y += sourceWorldOffset.y;
                    componentFrame.origin.z += sourceWorldOffset.z;
                    for (const QPointF &point : component.controlPoints) {
                        if (flattenedControlPointIndex !=
                            selectedControlPointIndex) {
                            consider(SnapType::ControlPoint,
                                     workPlaneFramePointToWorld(point,
                                                                componentFrame),
                                     shapeIndex,
                                     componentIndex);
                        }
                        ++flattenedControlPointIndex;
                    }
                }
                continue;
            }

            QVector<QPointF> controlPoints;
            if (shape.geometryType == GeometryType::Rectangle) {
                controlPoints = rectangleVertices(shape);
            } else if (shape.geometryType == GeometryType::Polygon) {
                controlPoints = polygonVerticesForShape(shape);
            } else if (shape.geometryType != GeometryType::Point &&
                       !isDimensionGeometryType(shape.geometryType)) {
                controlPoints = shape.nurbs.controlPoints.isEmpty()
                                    ? shape.points
                                    : shape.nurbs.controlPoints;
            }
            WorkPlaneFrame frame = shapeWorkPlaneFrame(shape);
            frame.origin.x += sourceWorldOffset.x;
            frame.origin.y += sourceWorldOffset.y;
            frame.origin.z += sourceWorldOffset.z;
            for (int pointIndex = 0; pointIndex < controlPoints.size(); ++pointIndex) {
                if (pointIndex != selectedControlPointIndex) {
                    consider(SnapType::ControlPoint,
                             workPlaneFramePointToWorld(controlPoints[pointIndex],
                                                        frame),
                             shapeIndex,
                             -1);
                }
            }
            continue;
        }

        const SceneObject *sceneObject = document.object(objectId);
        const Point3D worldOffset = sceneObject != nullptr
                                        ? sceneObject->placementTranslation
                                        : Point3D{};
        const WorkPlaneFrame targetFrame = shapeWorkPlaneFrame(shape);
        const QVector<SnapCandidate> targetCandidates =
            snapCandidatesForShape(shape, transform, viewportSize, worldOffset);
        for (const SnapCandidate &target : targetCandidates) {
            if (!snapTypeEnabled(target.type)) {
                continue;
            }
            const WorkPlaneFrame candidateFrame =
                target.componentIndex >= 0
                    ? shapeComponentWorkPlaneFrame(shape,
                                                   target.componentIndex)
                    : targetFrame;
            Point3D targetWorld = candidateWorldPoint(target, candidateFrame);
            if (shape.geometryType != GeometryType::NurbsSurface &&
                shape.geometryType != GeometryType::NurbsSolid) {
                targetWorld.x += worldOffset.x;
                targetWorld.y += worldOffset.y;
                targetWorld.z += worldOffset.z;
            }
            consider(target.type,
                     targetWorld,
                     shapeIndex,
                     target.componentIndex);
        }

        if (!settings_.near || isDimensionGeometryType(shape.geometryType)) {
            continue;
        }
        Shape::NurbsCurve2D nearCurve;
        const bool nearGeometrySupported =
            shape.geometryType == GeometryType::Point ||
            shape.geometryType == GeometryType::Rectangle ||
            shape.geometryType == GeometryType::Polygon ||
            shape.geometryType == GeometryType::Picture ||
            shape.geometryType == GeometryType::PolyCurve ||
            shape.geometryType == GeometryType::Arc ||
            shape.geometryType == GeometryType::Circle ||
            subdivisionCurve(shape, &nearCurve);
        if (!nearGeometrySupported) {
            continue;
        }

        if (shape.geometryType == GeometryType::PolyCurve) {
            for (int componentIndex = 0;
                 componentIndex < shape.components.size();
                 ++componentIndex) {
                Shape componentShape = shape;
                componentShape.geometryType = GeometryType::Nurbs;
                componentShape.nurbs = shape.components[componentIndex];
                componentShape.points.clear();
                componentShape.components.clear();
                componentShape.componentWorkPlaneFrames.clear();
                const WorkPlaneFrame componentFrame =
                    shapeComponentWorkPlaneFrame(shape, componentIndex);
                componentShape.workPlaneFrame = componentFrame;
                appendNearTarget(componentShape,
                                 componentFrame,
                                 worldOffset,
                                 shapeIndex,
                                 componentIndex);
            }
        } else {
            appendNearTarget(shape,
                             shapeWorkPlaneFrame(shape),
                             worldOffset,
                             shapeIndex,
                             -1);
        }
    }

    if (settings_.near && !nearScene.isEmpty()) {
        ViewportTransform nearTransform = transform;
        nearTransform.setWorkPlaneFrame(placedSourceFrame);
        const QPointF sourceInFrame =
            worldPointToWorkPlaneFrame(sourceWorld, placedSourceFrame);
        for (const SnapCandidate &nearCandidate : nearCandidatesForScene(
                 nearScene,
                 sourceInFrame,
                 nearTransform,
                 viewportSize,
                 {},
                 snapRadiusPixels)) {
            if (nearCandidate.shapeIndex < 0 ||
                nearCandidate.shapeIndex >= nearTargetIndices.size()) {
                continue;
            }
            const QPair<int, int> target =
                nearTargetIndices[nearCandidate.shapeIndex];
            const Point3D targetWorld = nearCandidate.hasWorldPoint
                                            ? nearCandidate.worldPoint
                                            : workPlaneFramePointToWorld(
                                                  nearCandidate.point,
                                                  placedSourceFrame);
            consider(SnapType::Near,
                     targetWorld,
                     target.first,
                     target.second >= 0 ? target.second
                                        : nearCandidate.componentIndex);
        }
    }

    if (settings_.intersection) {
        const QVector<SnapCandidate> sceneCandidates =
            snapCandidatesForScene(document,
                                   {selectedShapeIndex},
                                   transform,
                                   viewportSize);
        for (const SnapCandidate &candidate : sceneCandidates) {
            if (candidate.type != SnapType::Intersection) {
                continue;
            }
            const Point3D targetWorld = candidateWorldPoint(candidate,
                                                            displayFrame);
            consider(candidate.type, targetWorld,
                     candidate.shapeIndex, candidate.componentIndex);
        }
    }

    if (settings_.tangent && sourceShape.geometryType == GeometryType::Line) {
        const QVector<QPointF> &lineControlPoints =
            sourceShape.nurbs.controlPoints.isEmpty()
                ? sourceShape.points
                : sourceShape.nurbs.controlPoints;
        if (lineControlPoints.size() >= 2 &&
            (selectedControlPointIndex == 0 ||
             selectedControlPointIndex == lineControlPoints.size() - 1)) {
            const int fixedEndpointIndex = selectedControlPointIndex == 0
                                               ? 1
                                               : lineControlPoints.size() - 2;
            const Point3D fixedEndpointWorld = workPlaneFramePointToWorld(
                lineControlPoints[fixedEndpointIndex], sourceFrame);
            const QPointF fixedEndpointInDisplayFrame =
                worldPointToWorkPlaneFrame(fixedEndpointWorld, displayFrame);
            const QVector<SnapCandidate> tangentTargets = tangentCandidates(
                document,
                fixedEndpointInDisplayFrame,
                transform,
                viewportSize,
                {selectedShapeIndex});
            for (const SnapCandidate &target : tangentTargets) {
                consider(target.type,
                         candidateWorldPoint(target, displayFrame),
                         target.shapeIndex,
                         target.componentIndex);
            }
        }
    }
    return best;
}

} // namespace classiCAD
