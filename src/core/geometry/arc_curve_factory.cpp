#include "arc_curve_factory.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace classiCAD {
namespace {

constexpr qreal pi = 3.14159265358979323846;
constexpr qreal twoPi = 2.0 * pi;
constexpr qreal halfPi = pi / 2.0;

qreal cross(const QPointF &first, const QPointF &second)
{
    return first.x() * second.y() - first.y() * second.x();
}

qreal normalizePositiveAngle(qreal angle)
{
    angle = std::fmod(angle, twoPi);
    if (angle < 0.0) {
        angle += twoPi;
    }
    return angle;
}

QPointF pointOnCircle(const QPointF &center, qreal radius, qreal angle)
{
    return center + QPointF(radius * std::cos(angle), radius * std::sin(angle));
}

NurbsCurve2D makeRationalArc(const QPointF &center,
                             qreal radius,
                             qreal startAngle,
                             qreal sweepAngle)
{
    NurbsCurve2D curve;
    curve.dimension = 2;
    curve.degree = 2;
    curve.order = 3;
    curve.rational = true;

    const int spanCount = std::max(
        1,
        static_cast<int>(std::ceil(std::abs(sweepAngle) / halfPi)));
    const qreal spanSweep = sweepAngle / spanCount;
    curve.controlPoints.reserve(spanCount * 2 + 1);
    curve.weights.reserve(spanCount * 2 + 1);

    for (int span = 0; span < spanCount; ++span) {
        const qreal spanStart = startAngle + spanSweep * span;
        const qreal spanEnd = spanStart + spanSweep;
        const qreal spanMiddle = (spanStart + spanEnd) * 0.5;
        const qreal middleWeight = std::cos(std::abs(spanSweep) * 0.5);

        if (span == 0) {
            curve.controlPoints.append(pointOnCircle(center, radius, spanStart));
            curve.weights.append(1.0);
        }

        curve.controlPoints.append(pointOnCircle(
            center,
            radius / middleWeight,
            spanMiddle));
        curve.weights.append(middleWeight);
        curve.controlPoints.append(pointOnCircle(center, radius, spanEnd));
        curve.weights.append(1.0);
    }

    curve.knots.reserve(curve.controlPoints.size() + curve.order - 2);
    curve.knots.append(0.0);
    curve.knots.append(0.0);
    const qreal knotDelta = std::abs(spanSweep);
    for (int knot = 1; knot < spanCount; ++knot) {
        const qreal parameter = knotDelta * knot;
        curve.knots.append(parameter);
        curve.knots.append(parameter);
    }
    const qreal endParameter = std::abs(sweepAngle);
    curve.knots.append(endParameter);
    curve.knots.append(endParameter);
    return curve;
}

} // namespace

bool makeCircularArcThroughPoint(const QPointF &start,
                                 const QPointF &end,
                                 const QPointF &through,
                                 CircularArc2D *arc)
{
    if (arc == nullptr) {
        return false;
    }

    const QPointF endOffset = end - start;
    const QPointF throughOffset = through - start;
    const qreal endLength = std::hypot(endOffset.x(), endOffset.y());
    const qreal throughLength = std::hypot(throughOffset.x(), throughOffset.y());
    const qreal determinant = cross(endOffset, throughOffset);
    const qreal determinantScale = endLength * throughLength;
    if (!std::isfinite(determinant) ||
        endLength <= std::numeric_limits<qreal>::min() ||
        throughLength <= std::numeric_limits<qreal>::min() ||
        std::abs(determinant) <= 1.0e-12 * determinantScale) {
        return false;
    }

    // Translate the start point to the origin before solving for the
    // circumcenter. This avoids subtracting large squared world coordinates.
    const qreal endSquared = endOffset.x() * endOffset.x() +
                             endOffset.y() * endOffset.y();
    const qreal throughSquared = throughOffset.x() * throughOffset.x() +
                                 throughOffset.y() * throughOffset.y();
    const QPointF centerOffset(
        (endSquared * throughOffset.y() -
         throughSquared * endOffset.y()) / (2.0 * determinant),
        (endOffset.x() * throughSquared -
         throughOffset.x() * endSquared) / (2.0 * determinant));
    const QPointF center = start + centerOffset;
    const qreal radius = std::hypot(centerOffset.x(), centerOffset.y());
    if (!std::isfinite(center.x()) || !std::isfinite(center.y()) ||
        !std::isfinite(radius) || radius <= std::numeric_limits<qreal>::min()) {
        return false;
    }

    const qreal startAngle = std::atan2(start.y() - center.y(),
                                        start.x() - center.x());
    const qreal endAngle = std::atan2(end.y() - center.y(),
                                      end.x() - center.x());
    const qreal throughAngle = std::atan2(through.y() - center.y(),
                                          through.x() - center.x());
    const qreal counterClockwiseEnd =
        normalizePositiveAngle(endAngle - startAngle);
    const qreal counterClockwiseThrough =
        normalizePositiveAngle(throughAngle - startAngle);
    if (counterClockwiseEnd <= 1.0e-12) {
        return false;
    }

    // Select the sweep containing the third point. The signed sweep records
    // direction explicitly, including major arcs, just like the reference
    // tool's P1 -> P3 -> P2 winding test.
    const qreal sweepAngle = counterClockwiseThrough < counterClockwiseEnd
                                 ? counterClockwiseEnd
                                 : counterClockwiseEnd - twoPi;
    const NurbsCurve2D curve = makeRationalArc(center,
                                                radius,
                                                startAngle,
                                                sweepAngle);
    if (!validateNurbsCurve(curve)) {
        return false;
    }

    arc->center = center;
    arc->radius = radius;
    arc->startAngle = startAngle;
    arc->sweepAngle = sweepAngle;
    arc->curve = curve;
    return true;
}

} // namespace classiCAD
