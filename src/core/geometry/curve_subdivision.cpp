#include "curve_subdivision.h"

#include "core/geometry/curve_construction.h"
#include "core/geometry/curve_evaluator.h"
#include "core/geometry/nurbs_curve.h"

#include <algorithm>
#include <cmath>

namespace classiCAD {

bool isSubdividableCurveShape(const Shape &shape)
{
    if (shape.geometryType == GeometryType::Line) {
        return shape.points.size() >= 2 &&
               (validateNurbsCurve(shape.nurbs) || !shape.points.isEmpty());
    }
    return (shape.geometryType == GeometryType::Arc ||
            shape.geometryType == GeometryType::Bezier ||
            shape.geometryType == GeometryType::Nurbs ||
            shape.geometryType == GeometryType::Circle ||
            shape.geometryType == GeometryType::Ellipse) &&
           validateNurbsCurve(shape.nurbs);
}

bool subdivisionCurveForShape(const Shape &shape,
                              Shape::NurbsCurve2D *curve)
{
    if (curve == nullptr || !isSubdividableCurveShape(shape)) {
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
    return false;
}

QVector<double> equalArcLengthSubdivisionParameters(const Shape &shape,
                                                    int sections)
{
    QVector<double> parameters;
    if (sections < 2) {
        return parameters;
    }

    Shape::NurbsCurve2D curve;
    if (!subdivisionCurveForShape(shape, &curve)) {
        return parameters;
    }

    const QVector<double> fullKnots = expandedNurbsKnotVector(curve);
    if (fullKnots.size() <= curve.degree + 1 ||
        curve.controlPoints.isEmpty()) {
        return parameters;
    }

    const qreal firstParameter = fullKnots[curve.degree];
    const qreal lastParameter = fullKnots[curve.controlPoints.size()];
    if (!std::isfinite(firstParameter) || !std::isfinite(lastParameter) ||
        lastParameter <= firstParameter) {
        return parameters;
    }

    int nonZeroSpans = 0;
    for (int index = curve.degree; index < curve.controlPoints.size(); ++index) {
        if (fullKnots[index + 1] > fullKnots[index]) {
            ++nonZeroSpans;
        }
    }

    const int sampleCount = std::clamp(std::max(128, nonZeroSpans * 64),
                                       128,
                                       4096);
    QVector<qreal> sampleParameters;
    QVector<qreal> cumulativeLengths;
    sampleParameters.reserve(sampleCount + 1);
    cumulativeLengths.reserve(sampleCount + 1);

    Point3D previousPoint;
    if (!evaluateNurbsPoint3D(curve, firstParameter, &previousPoint)) {
        return parameters;
    }
    sampleParameters.append(firstParameter);
    cumulativeLengths.append(0.0);
    qreal totalLength = 0.0;
    for (int sample = 1; sample <= sampleCount; ++sample) {
        const qreal fraction = static_cast<qreal>(sample) / sampleCount;
        const qreal parameter = firstParameter +
                                (lastParameter - firstParameter) * fraction;
        Point3D currentPoint;
        if (!evaluateNurbsPoint3D(curve, parameter, &currentPoint)) {
            return {};
        }
        totalLength += std::hypot(
            std::hypot(currentPoint.x - previousPoint.x,
                       currentPoint.y - previousPoint.y),
            currentPoint.z - previousPoint.z);
        sampleParameters.append(parameter);
        cumulativeLengths.append(totalLength);
        previousPoint = currentPoint;
    }

    if (totalLength <= 1.0e-9) {
        return parameters;
    }

    parameters.reserve(sections - 1);
    for (int division = 1; division < sections; ++division) {
        const qreal targetLength = totalLength * division / sections;
        const auto upper = std::lower_bound(cumulativeLengths.cbegin(),
                                            cumulativeLengths.cend(),
                                            targetLength);
        const int upperIndex = static_cast<int>(upper - cumulativeLengths.cbegin());
        if (upperIndex <= 0) {
            parameters.append(sampleParameters.first());
            continue;
        }
        if (upperIndex >= cumulativeLengths.size()) {
            parameters.append(sampleParameters.last());
            continue;
        }

        const qreal lowerLength = cumulativeLengths[upperIndex - 1];
        const qreal upperLength = cumulativeLengths[upperIndex];
        const qreal span = upperLength - lowerLength;
        const qreal localFraction = span > 1.0e-12
                                        ? (targetLength - lowerLength) / span
                                        : 0.0;
        parameters.append(sampleParameters[upperIndex - 1] +
                          (sampleParameters[upperIndex] -
                           sampleParameters[upperIndex - 1]) * localFraction);
    }
    return parameters;
}

} // namespace classiCAD
