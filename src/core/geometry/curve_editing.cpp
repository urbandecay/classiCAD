#include "curve_editing.h"

#include "core/geometry/nurbs_curve.h"

#include <algorithm>
#include <cmath>

namespace classiCAD {
namespace {

bool sameKnot(qreal first, qreal second)
{
    const qreal tolerance = 1.0e-9 *
                            std::max<qreal>(1.0,
                                           std::max(std::abs(first),
                                                    std::abs(second)));
    return std::abs(first - second) <= tolerance;
}

bool insertNurbsKnot(QVector<HomogeneousControlPoint2D> *controlPoints,
                     QVector<double> *knots,
                     int degree,
                     qreal parameter)
{
    if (controlPoints == nullptr || knots == nullptr ||
        controlPoints->isEmpty() || knots->isEmpty() || degree < 1) {
        return false;
    }

    const int n = controlPoints->size() - 1;
    const int m = knots->size() - 1;
    if (m != n + degree + 1) {
        return false;
    }

    int span = degree;
    if (parameter >= knots->at(n + 1)) {
        span = n;
    } else {
        int low = degree;
        int high = n + 1;
        int middle = (low + high) / 2;
        while (parameter < knots->at(middle) || parameter >= knots->at(middle + 1)) {
            if (parameter < knots->at(middle)) {
                high = middle;
            } else {
                low = middle;
            }
            middle = (low + high) / 2;
        }
        span = middle;
    }

    int multiplicity = 0;
    for (const double knot : *knots) {
        if (sameKnot(knot, parameter)) {
            ++multiplicity;
        }
    }
    if (multiplicity >= degree) {
        return false;
    }

    QVector<HomogeneousControlPoint2D> insertedControlPoints(n + 2);
    for (int index = 0; index <= span - degree; ++index) {
        insertedControlPoints[index] = controlPoints->at(index);
    }
    for (int index = span - multiplicity; index <= n; ++index) {
        insertedControlPoints[index + 1] = controlPoints->at(index);
    }
    for (int index = span - degree + 1; index <= span - multiplicity; ++index) {
        const qreal denominator = knots->at(index + degree) - knots->at(index);
        if (std::abs(denominator) <= 1.0e-12) {
            return false;
        }
        const qreal alpha = (parameter - knots->at(index)) / denominator;
        insertedControlPoints[index] = blendHomogeneousControlPoints(
            controlPoints->at(index - 1),
            controlPoints->at(index),
            alpha);
    }

    QVector<double> insertedKnots(m + 2);
    for (int index = 0; index <= span; ++index) {
        insertedKnots[index] = knots->at(index);
    }
    insertedKnots[span + 1] = parameter;
    for (int index = span + 1; index <= m; ++index) {
        insertedKnots[index + 1] = knots->at(index);
    }

    *controlPoints = insertedControlPoints;
    *knots = insertedKnots;
    return true;
}

} // namespace

bool rationalBezierSpansForCurve(const NurbsCurve2D &curve,
                                 QVector<RationalBezierSpan2D> *spans)
{
    if (spans == nullptr || !validateNurbsCurve(curve)) {
        return false;
    }

    spans->clear();
    QVector<HomogeneousControlPoint2D> controlPoints;
    controlPoints.reserve(curve.controlPoints.size());
    for (int index = 0; index < curve.controlPoints.size(); ++index) {
        const qreal weight = curve.rational ? curve.weights[index] : 1.0;
        controlPoints.append(HomogeneousControlPoint2D{
            curve.controlPoints[index] * weight,
            weight});
    }

    QVector<double> knots = expandedNurbsKnotVector(curve);
    const qreal domainStart = knots[curve.degree];
    const qreal domainEnd = knots[curve.controlPoints.size()];
    QVector<double> internalKnots;
    for (const double knot : knots) {
        if (knot <= domainStart || knot >= domainEnd) {
            continue;
        }
        if (internalKnots.isEmpty() || !sameKnot(internalKnots.back(), knot)) {
            internalKnots.append(knot);
        }
    }

    for (const double internalKnot : internalKnots) {
        int multiplicity = 0;
        for (const double knot : knots) {
            if (sameKnot(knot, internalKnot)) {
                ++multiplicity;
            }
        }
        while (multiplicity < curve.degree) {
            if (!insertNurbsKnot(&controlPoints,
                                 &knots,
                                 curve.degree,
                                 internalKnot)) {
                return false;
            }
            ++multiplicity;
        }
    }

    constexpr qreal knotTolerance = 1.0e-10;
    for (int spanIndex = curve.degree;
         spanIndex < controlPoints.size();
         ++spanIndex) {
        if (knots[spanIndex + 1] - knots[spanIndex] <= knotTolerance) {
            continue;
        }

        RationalBezierSpan2D span;
        span.startParameter = knots[spanIndex];
        span.endParameter = knots[spanIndex + 1];
        span.controlPoints.reserve(curve.degree + 1);
        for (int controlIndex = spanIndex - curve.degree;
             controlIndex <= spanIndex;
             ++controlIndex) {
            span.controlPoints.append(controlPoints[controlIndex]);
        }
        spans->append(span);
    }
    return !spans->isEmpty();
}

bool splitRationalBezierSpan(
    const QVector<HomogeneousControlPoint2D> &source,
    qreal fraction,
    QVector<HomogeneousControlPoint2D> *left,
    QVector<HomogeneousControlPoint2D> *right)
{
    if (source.size() < 2 || left == nullptr || right == nullptr ||
        fraction <= 0.0 || fraction >= 1.0) {
        return false;
    }

    QVector<HomogeneousControlPoint2D> working = source;
    left->resize(source.size());
    right->resize(source.size());
    (*left)[0] = working.first();
    (*right)[source.size() - 1] = working.last();

    for (int level = 1; level < source.size(); ++level) {
        for (int index = 0; index + 1 < working.size(); ++index) {
            working[index] = blendHomogeneousControlPoints(
                working[index],
                working[index + 1],
                fraction);
        }
        (*left)[level] = working.first();
        (*right)[source.size() - 1 - level] =
            working[source.size() - 1 - level];
    }
    return true;
}

bool trimNurbsCurve(const NurbsCurve2D &source,
                    qreal startParameter,
                    qreal endParameter,
                    NurbsCurve2D *trimmed)
{
    if (trimmed == nullptr || !validateNurbsCurve(source)) {
        return false;
    }

    const QVector<double> fullKnots = expandedNurbsKnotVector(source);
    const qreal domainStart = fullKnots[source.degree];
    const qreal domainEnd = fullKnots[source.controlPoints.size()];
    const qreal domainTolerance =
        std::max<qreal>(1.0e-9, std::abs(domainEnd - domainStart) * 1.0e-9);
    const qreal start = std::clamp(startParameter, domainStart, domainEnd);
    const qreal end = std::clamp(endParameter, domainStart, domainEnd);
    if (end - start <= domainTolerance) {
        return false;
    }

    QVector<RationalBezierSpan2D> spans;
    if (!rationalBezierSpansForCurve(source, &spans)) {
        return false;
    }

    QVector<RationalBezierSpan2D> trimmedSpans;
    for (const RationalBezierSpan2D &span : spans) {
        const qreal overlapStart = std::max(start, span.startParameter);
        const qreal overlapEnd = std::min(end, span.endParameter);
        if (overlapEnd - overlapStart <= domainTolerance) {
            continue;
        }

        const qreal spanLength = span.endParameter - span.startParameter;
        qreal localStart = (overlapStart - span.startParameter) / spanLength;
        qreal localEnd = (overlapEnd - span.startParameter) / spanLength;
        QVector<HomogeneousControlPoint2D> working = span.controlPoints;

        if (localStart > 1.0e-10) {
            QVector<HomogeneousControlPoint2D> discarded;
            QVector<HomogeneousControlPoint2D> remaining;
            if (!splitRationalBezierSpan(working,
                                         localStart,
                                         &discarded,
                                         &remaining)) {
                return false;
            }
            working = remaining;
            localEnd = (localEnd - localStart) / (1.0 - localStart);
        }

        if (localEnd < 1.0 - 1.0e-10) {
            QVector<HomogeneousControlPoint2D> remaining;
            QVector<HomogeneousControlPoint2D> discarded;
            if (!splitRationalBezierSpan(working,
                                         localEnd,
                                         &remaining,
                                         &discarded)) {
                return false;
            }
            working = remaining;
        }

        RationalBezierSpan2D trimmedSpan;
        trimmedSpan.startParameter = overlapStart;
        trimmedSpan.endParameter = overlapEnd;
        trimmedSpan.controlPoints = working;
        trimmedSpans.append(trimmedSpan);
    }

    if (trimmedSpans.isEmpty()) {
        return false;
    }

    NurbsCurve2D result;
    result.dimension = source.dimension;
    result.degree = source.degree;
    result.order = source.order;
    result.rational = source.rational;
    for (int spanIndex = 0; spanIndex < trimmedSpans.size(); ++spanIndex) {
        const QVector<HomogeneousControlPoint2D> &spanPoints =
            trimmedSpans[spanIndex].controlPoints;
        const int firstPoint = spanIndex == 0 ? 0 : 1;
        for (int pointIndex = firstPoint; pointIndex < spanPoints.size(); ++pointIndex) {
            const HomogeneousControlPoint2D &point = spanPoints[pointIndex];
            if (std::abs(point.weight) <= 1.0e-12) {
                return false;
            }
            result.controlPoints.append(point.weightedPosition / point.weight);
            result.weights.append(source.rational ? point.weight : 1.0);
        }
    }

    QVector<double> resultFullKnots;
    const auto appendRepeated = [&resultFullKnots](qreal value, int count) {
        for (int index = 0; index < count; ++index) {
            resultFullKnots.append(value);
        }
    };
    appendRepeated(trimmedSpans.first().startParameter, source.degree + 1);
    for (int spanIndex = 0; spanIndex + 1 < trimmedSpans.size(); ++spanIndex) {
        appendRepeated(trimmedSpans[spanIndex].endParameter, source.degree);
    }
    appendRepeated(trimmedSpans.last().endParameter, source.degree + 1);
    if (resultFullKnots.size() < 2) {
        return false;
    }

    result.knots = resultFullKnots;
    result.knots.removeFirst();
    result.knots.removeLast();
    if (!validateNurbsCurve(result)) {
        return false;
    }

    *trimmed = result;
    return true;
}

bool reverseNurbsCurve(const NurbsCurve2D &source,
                       NurbsCurve2D *reversed)
{
    if (reversed == nullptr || !validateNurbsCurve(source)) {
        return false;
    }

    NurbsCurve2D result = source;
    std::reverse(result.controlPoints.begin(), result.controlPoints.end());
    if (!result.weights.isEmpty()) {
        std::reverse(result.weights.begin(), result.weights.end());
    }

    const QVector<double> fullKnots = expandedNurbsKnotVector(source);
    const qreal domainStart = fullKnots[source.degree];
    const qreal domainEnd = fullKnots[source.controlPoints.size()];
    result.knots.clear();
    result.knots.reserve(source.knots.size());
    for (int index = 1; index + 1 < fullKnots.size(); ++index) {
        result.knots.append(domainStart + domainEnd -
                            fullKnots[fullKnots.size() - 1 - index]);
    }

    *reversed = result;
    return true;
}

} // namespace classiCAD
