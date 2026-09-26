#include "curve_evaluator.h"

#include <algorithm>
#include <cmath>

namespace classiCAD {

bool nurbsParameterDomain(const NurbsCurve2D &curve,
                          qreal *startParameter,
                          qreal *endParameter)
{
    if (!validateNurbsCurve(curve) ||
        (startParameter == nullptr && endParameter == nullptr)) {
        return false;
    }

    const QVector<double> fullKnots = expandedNurbsKnotVector(curve);
    const qreal start = fullKnots[curve.degree];
    const qreal end = fullKnots[curve.controlPoints.size()];
    if (end <= start) {
        return false;
    }

    if (startParameter != nullptr) {
        *startParameter = start;
    }
    if (endParameter != nullptr) {
        *endParameter = end;
    }
    return true;
}

bool evaluateNurbsPoint(const NurbsCurve2D &curve,
                        qreal parameter,
                        QPointF *point)
{
    if (!validateNurbsCurve(curve) || point == nullptr) {
        return false;
    }

    const int controlPointCount = curve.controlPoints.size();
    const QVector<double> fullKnots = expandedNurbsKnotVector(curve);
    const int endKnotIndex = controlPointCount;
    const qreal firstParameter = fullKnots[curve.degree];
    const qreal lastParameter = fullKnots[endKnotIndex];
    constexpr qreal epsilon = 1.0e-12;
    if (parameter <= firstParameter + epsilon) {
        *point = curve.controlPoints.first();
        return true;
    }
    if (parameter >= lastParameter - epsilon) {
        *point = curve.controlPoints.last();
        return true;
    }

    const auto basis = [&fullKnots](const auto &self,
                                    int index,
                                    int degree,
                                    qreal parameterValue) -> qreal {
        if (degree == 0) {
            return fullKnots[index] <= parameterValue &&
                           parameterValue < fullKnots[index + 1]
                       ? 1.0
                       : 0.0;
        }

        qreal value = 0.0;
        const qreal leftDenominator = fullKnots[index + degree] - fullKnots[index];
        if (std::abs(leftDenominator) > 1.0e-12) {
            value += (parameterValue - fullKnots[index]) / leftDenominator *
                     self(self, index, degree - 1, parameterValue);
        }

        const qreal rightDenominator = fullKnots[index + degree + 1] -
                                       fullKnots[index + 1];
        if (std::abs(rightDenominator) > 1.0e-12) {
            value += (fullKnots[index + degree + 1] - parameterValue) /
                     rightDenominator *
                     self(self, index + 1, degree - 1, parameterValue);
        }
        return value;
    };

    QPointF numerator(0.0, 0.0);
    qreal denominator = 0.0;
    for (int index = 0; index < controlPointCount; ++index) {
        const qreal weightedBasis = basis(basis, index, curve.degree, parameter) *
                                     (curve.rational ? curve.weights[index] : 1.0);
        numerator += curve.controlPoints[index] * weightedBasis;
        denominator += weightedBasis;
    }

    if (std::abs(denominator) <= 1.0e-12) {
        return false;
    }
    *point = numerator / denominator;
    return true;
}

bool evaluateNurbsDerivative(const NurbsCurve2D &curve,
                             qreal parameter,
                             QPointF *derivative)
{
    if (!validateNurbsCurve(curve) || derivative == nullptr || curve.degree < 1) {
        return false;
    }

    const QVector<double> fullKnots = expandedNurbsKnotVector(curve);
    const qreal domainStart = fullKnots[curve.degree];
    const qreal domainEnd = fullKnots[curve.controlPoints.size()];
    if (domainEnd <= domainStart) {
        return false;
    }

    qreal evaluationParameter = std::clamp(parameter, domainStart, domainEnd);
    if (evaluationParameter >= domainEnd) {
        evaluationParameter = std::nextafter(domainEnd, domainStart);
    }

    const auto basis = [&fullKnots](const auto &self,
                                    int index,
                                    int degree,
                                    qreal value) -> qreal {
        if (degree == 0) {
            return fullKnots[index] <= value && value < fullKnots[index + 1]
                       ? 1.0
                       : 0.0;
        }

        qreal result = 0.0;
        const qreal leftDenominator = fullKnots[index + degree] - fullKnots[index];
        if (std::abs(leftDenominator) > 1.0e-12) {
            result += (value - fullKnots[index]) / leftDenominator *
                      self(self, index, degree - 1, value);
        }

        const qreal rightDenominator = fullKnots[index + degree + 1] -
                                       fullKnots[index + 1];
        if (std::abs(rightDenominator) > 1.0e-12) {
            result += (fullKnots[index + degree + 1] - value) / rightDenominator *
                      self(self, index + 1, degree - 1, value);
        }
        return result;
    };

    QPointF numerator(0.0, 0.0);
    QPointF numeratorDerivative(0.0, 0.0);
    qreal denominator = 0.0;
    qreal denominatorDerivative = 0.0;
    for (int index = 0; index < curve.controlPoints.size(); ++index) {
        const qreal weight = curve.rational ? curve.weights[index] : 1.0;
        const qreal pointBasis = basis(basis,
                                       index,
                                       curve.degree,
                                       evaluationParameter);
        const qreal lowerBasis = basis(basis,
                                       index,
                                       curve.degree - 1,
                                       evaluationParameter);
        const qreal nextLowerBasis = basis(basis,
                                           index + 1,
                                           curve.degree - 1,
                                           evaluationParameter);

        const qreal leftDenominator = fullKnots[index + curve.degree] -
                                      fullKnots[index];
        const qreal rightDenominator = fullKnots[index + curve.degree + 1] -
                                       fullKnots[index + 1];
        qreal pointBasisDerivative = 0.0;
        if (std::abs(leftDenominator) > 1.0e-12) {
            pointBasisDerivative += curve.degree / leftDenominator * lowerBasis;
        }
        if (std::abs(rightDenominator) > 1.0e-12) {
            pointBasisDerivative -= curve.degree / rightDenominator * nextLowerBasis;
        }

        const qreal weightedBasis = weight * pointBasis;
        const qreal weightedBasisDerivative = weight * pointBasisDerivative;
        numerator += curve.controlPoints[index] * weightedBasis;
        numeratorDerivative += curve.controlPoints[index] * weightedBasisDerivative;
        denominator += weightedBasis;
        denominatorDerivative += weightedBasisDerivative;
    }

    if (std::abs(denominator) <= 1.0e-12) {
        return false;
    }

    const QPointF point = numerator / denominator;
    *derivative = (numeratorDerivative - point * denominatorDerivative) / denominator;
    return std::isfinite(derivative->x()) && std::isfinite(derivative->y());
}

} // namespace classiCAD
