#include "circle_tangent_tool.h"

#include "core/geometry/circle_construction.h"
#include "core/geometry/curve_evaluator.h"
#include "core/geometry/nurbs_curve.h"
#include "core/model.h"
#include "tool_context.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace classiCAD {
namespace {

constexpr qreal kGeometryEpsilon = 1.0e-9;

struct ClosestPoint {
    QPointF point;
    QPointF direction;
    qreal distance = std::numeric_limits<qreal>::infinity();
};

struct SignedDistance {
    qreal value = 0.0;
    QPointF gradient;
};

using TangencySigns = std::array<int, 3>;

constexpr std::array<TangencySigns, 8> kTangencyPermutations{{
    TangencySigns{1, 1, 1},
    TangencySigns{-1, -1, -1},
    TangencySigns{1, 1, -1},
    TangencySigns{1, -1, 1},
    TangencySigns{-1, 1, 1},
    TangencySigns{-1, -1, 1},
    TangencySigns{-1, 1, -1},
    TangencySigns{1, -1, -1},
}};

qreal squaredLength(const QPointF &point)
{
    return point.x() * point.x() + point.y() * point.y();
}

qreal dotProduct(const QPointF &first, const QPointF &second)
{
    return first.x() * second.x() + first.y() * second.y();
}

bool exactCircleTarget(const Shape &shape,
                       const QVector<QVector<QPointF>> &curves,
                       TangentCircleCandidate *target)
{
    if (target == nullptr || curves.isEmpty() ||
        (shape.geometryType != GeometryType::Circle &&
         shape.geometryType != GeometryType::Arc &&
         shape.geometryType != GeometryType::Nurbs &&
         shape.geometryType != GeometryType::PolyCurve)) {
        return false;
    }

    QVector<QPointF> samples;
    for (const QVector<QPointF> &curve : curves) {
        samples += curve;
    }
    if (samples.size() < 3) {
        return false;
    }

    QVector<QPointF> definition;
    if (!makeCircleDefinitionFromThreePoints(samples[0],
                                             samples[samples.size() / 3],
                                             samples[2 * samples.size() / 3],
                                             &definition)) {
        return false;
    }

    const QPointF center = definition[0];
    const qreal radius = std::hypot(definition[1].x() - center.x(),
                                    definition[1].y() - center.y());
    const qreal tolerance = std::max(radius, 1.0) * 1.0e-6;
    for (const QPointF &point : samples) {
        if (std::abs(std::hypot(point.x() - center.x(),
                                point.y() - center.y()) - radius) > tolerance) {
            return false;
        }
    }

    *target = TangentCircleCandidate{center, radius};
    return true;
}

qreal distanceToCurveSamples(const QVector<QVector<QPointF>> &curves,
                             const QPointF &point)
{
    qreal minimum = std::numeric_limits<qreal>::infinity();
    for (const QVector<QPointF> &curve : curves) {
        for (int index = 1; index < curve.size(); ++index) {
            const QPointF start = curve[index - 1];
            const QPointF segment = curve[index] - start;
            const qreal lengthSquared = squaredLength(segment);
            if (lengthSquared <= kGeometryEpsilon * kGeometryEpsilon) {
                continue;
            }
            const QPointF offset = point - start;
            const qreal fraction = std::clamp(dotProduct(offset, segment) /
                                                  lengthSquared,
                                              0.0,
                                              1.0);
            const QPointF closest = start + segment * fraction;
            minimum = std::min(minimum,
                               std::hypot(point.x() - closest.x(),
                                          point.y() - closest.y()));
        }
    }
    return minimum;
}

QVector<TangentCircleCandidate> tangentCirclesForThreeCircles(
    const QVector<TangentCircleCandidate> &targets,
    const QVector<QVector<QVector<QPointF>>> &curveGroups,
    const QVector<bool> &closedTargets)
{
    QVector<TangentCircleCandidate> solutions;
    if (targets.size() != 3 || curveGroups.size() != 3 ||
        closedTargets.size() != 3 ||
        std::any_of(targets.cbegin(), targets.cend(),
                    [](const TangentCircleCandidate &target) {
                        return !std::isfinite(target.radius) || target.radius <= 0.0;
                    })) {
        return solutions;
    }

    const QPointF firstOffset = targets[1].center - targets[0].center;
    const QPointF secondOffset = targets[2].center - targets[0].center;
    const qreal determinant = 4.0 *
        (firstOffset.x() * secondOffset.y() -
         firstOffset.y() * secondOffset.x());
    const qreal scale = std::max({std::hypot(firstOffset.x(), firstOffset.y()),
                                  std::hypot(secondOffset.x(), secondOffset.y()),
                                  targets[0].radius,
                                  targets[1].radius,
                                  targets[2].radius,
                                  1.0});
    if (std::abs(determinant) <= 1.0e-12 * scale * scale) {
        return solutions;
    }

    const auto solveOffset = [&](qreal firstValue, qreal secondValue) {
        return QPointF((firstValue * 2.0 * secondOffset.y() -
                        secondValue * 2.0 * firstOffset.y()) / determinant,
                       (2.0 * firstOffset.x() * secondValue -
                        2.0 * secondOffset.x() * firstValue) / determinant);
    };

    for (const TangencySigns &signs : kTangencyPermutations) {
        const qreal firstConstant = squaredLength(firstOffset) -
                                    targets[1].radius * targets[1].radius +
                                    targets[0].radius * targets[0].radius;
        const qreal secondConstant = squaredLength(secondOffset) -
                                     targets[2].radius * targets[2].radius +
                                     targets[0].radius * targets[0].radius;
        const QPointF base = solveOffset(firstConstant, secondConstant);
        const QPointF slope = solveOffset(
            -2.0 * (signs[1] * targets[1].radius -
                    signs[0] * targets[0].radius),
            -2.0 * (signs[2] * targets[2].radius -
                    signs[0] * targets[0].radius));

        const qreal a = squaredLength(slope) - 1.0;
        const qreal b = 2.0 * (dotProduct(base, slope) -
                               signs[0] * targets[0].radius);
        const qreal c = squaredLength(base) -
                        targets[0].radius * targets[0].radius;

        QVector<qreal> radii;
        if (std::abs(a) <= 1.0e-12) {
            if (std::abs(b) > 1.0e-12) {
                radii.append(-c / b);
            }
        } else {
            const qreal discriminant = b * b - 4.0 * a * c;
            if (discriminant >= -1.0e-10 * scale * scale) {
                const qreal root = std::sqrt(std::max(0.0, discriminant));
                radii.append((-b - root) / (2.0 * a));
                if (root > 1.0e-12 * scale) {
                    radii.append((-b + root) / (2.0 * a));
                }
            }
        }

        for (qreal radius : radii) {
            if (!std::isfinite(radius) || radius <= 1.0e-9 * scale) {
                continue;
            }
            const QPointF center = targets[0].center + base + slope * radius;
            if (!std::isfinite(center.x()) || !std::isfinite(center.y())) {
                continue;
            }

            bool tangentToAll = true;
            for (int index = 0; index < 3; ++index) {
                const QPointF offset = center - targets[index].center;
                const qreal actualDistance = std::hypot(offset.x(), offset.y());
                const qreal expectedDistance =
                    std::abs(radius + signs[index] * targets[index].radius);
                if (std::abs(actualDistance - expectedDistance) > 1.0e-6 * scale) {
                    tangentToAll = false;
                    break;
                }
                if (!closedTargets[index]) {
                    if (actualDistance <= kGeometryEpsilon) {
                        tangentToAll = false;
                        break;
                    }
                    const QPointF direction = offset / actualDistance;
                    const QPointF nearContact =
                        targets[index].center + direction * targets[index].radius;
                    const QPointF farContact =
                        targets[index].center - direction * targets[index].radius;
                    const qreal contactTolerance = 1.0e-4 * scale;
                    const auto contactIsOnArc = [&](const QPointF &contact) {
                        return std::abs(std::hypot(center.x() - contact.x(),
                                                    center.y() - contact.y()) -
                                        radius) <= contactTolerance &&
                               distanceToCurveSamples(curveGroups[index], contact) <=
                                   contactTolerance;
                    };
                    if (!contactIsOnArc(nearContact) &&
                        !contactIsOnArc(farContact)) {
                        tangentToAll = false;
                        break;
                    }
                }
            }
            if (!tangentToAll) {
                continue;
            }

            const bool duplicate = std::any_of(
                solutions.cbegin(), solutions.cend(),
                [&](const TangentCircleCandidate &existing) {
                    const QPointF offset = existing.center - center;
                    return std::hypot(offset.x(), offset.y()) <= 1.0e-6 * scale &&
                           std::abs(existing.radius - radius) <= 1.0e-6 * scale;
                });
            if (!duplicate) {
                solutions.append(TangentCircleCandidate{center, radius});
            }
        }
    }

    return solutions;
}

ClosestPoint closestPointOnCurves(const QVector<QVector<QPointF>> &curves,
                                  const QPointF &point)
{
    ClosestPoint closest;
    for (const QVector<QPointF> &curve : curves) {
        for (int index = 1; index < curve.size(); ++index) {
            const QPointF start = curve[index - 1];
            const QPointF direction = curve[index] - start;
            const qreal lengthSquared = direction.x() * direction.x() +
                                        direction.y() * direction.y();
            if (lengthSquared <= kGeometryEpsilon * kGeometryEpsilon) {
                continue;
            }

            const QPointF offset = point - start;
            const qreal parameter = std::clamp(
                (offset.x() * direction.x() + offset.y() * direction.y()) /
                    lengthSquared,
                0.0,
                1.0);
            const QPointF candidate = start + direction * parameter;
            const qreal distance = std::hypot(point.x() - candidate.x(),
                                              point.y() - candidate.y());
            if (distance < closest.distance) {
                const qreal directionLength = std::sqrt(lengthSquared);
                closest = ClosestPoint{candidate,
                                       direction / directionLength,
                                       distance};
            }
        }
    }
    return closest;
}

bool signedDistanceToCurves(const QVector<QVector<QPointF>> &curves,
                            const QPointF &point,
                            SignedDistance *result)
{
    if (result == nullptr) {
        return false;
    }
    const ClosestPoint closest = closestPointOnCurves(curves, point);
    if (!std::isfinite(closest.distance) ||
        closest.distance <= kGeometryEpsilon) {
        return false;
    }

    const QPointF offset = point - closest.point;
    const qreal cross = closest.direction.x() * offset.y() -
                        closest.direction.y() * offset.x();
    const qreal side = cross >= 0.0 ? 1.0 : -1.0;
    result->value = side * closest.distance;
    result->gradient = offset * (side / closest.distance);
    return true;
}

bool branchDistanceToCurves(const QVector<QVector<QPointF>> &curves,
                            bool closed,
                            int sign,
                            const QPointF &center,
                            SignedDistance *result,
                            qreal *radiusCoefficient)
{
    if (result == nullptr || radiusCoefficient == nullptr) {
        return false;
    }
    if (!closed) {
        *radiusCoefficient = sign;
        return signedDistanceToCurves(curves, center, result);
    }

    QPointF contact;
    qreal distance = 0.0;
    if (sign > 0) {
        const ClosestPoint closest = closestPointOnCurves(curves, center);
        contact = closest.point;
        distance = closest.distance;
    } else {
        for (const QVector<QPointF> &curve : curves) {
            for (const QPointF &point : curve) {
                const qreal candidateDistance =
                    std::hypot(center.x() - point.x(), center.y() - point.y());
                if (candidateDistance > distance) {
                    distance = candidateDistance;
                    contact = point;
                }
            }
        }
    }
    if (!std::isfinite(distance) || distance <= kGeometryEpsilon) {
        return false;
    }
    *radiusCoefficient = 1.0;
    result->value = distance;
    result->gradient = (center - contact) / distance;
    return true;
}

int tangencyPermutationAt(const QVector<QVector<QVector<QPointF>>> &curveGroups,
                          const QPointF &point)
{
    if (curveGroups.size() != 3) {
        return 0;
    }

    TangencySigns signs{};
    for (int index = 0; index < 3; ++index) {
        SignedDistance distance;
        if (!signedDistanceToCurves(curveGroups[index], point, &distance)) {
            return 0;
        }
        signs[index] = distance.value >= 0.0 ? 1 : -1;
    }

    for (int index = 0; index < static_cast<int>(kTangencyPermutations.size()); ++index) {
        if (kTangencyPermutations[index] == signs) {
            return index;
        }
    }
    return 0;
}

qreal curveGroupsScale(const QVector<QVector<QVector<QPointF>>> &curveGroups)
{
    bool foundPoint = false;
    QPointF minimum;
    QPointF maximum;
    for (const auto &group : curveGroups) {
        for (const QVector<QPointF> &curve : group) {
            for (const QPointF &point : curve) {
                if (!foundPoint) {
                    minimum = point;
                    maximum = point;
                    foundPoint = true;
                } else {
                    minimum.setX(std::min(minimum.x(), point.x()));
                    minimum.setY(std::min(minimum.y(), point.y()));
                    maximum.setX(std::max(maximum.x(), point.x()));
                    maximum.setY(std::max(maximum.y(), point.y()));
                }
            }
        }
    }
    return foundPoint ? std::max({maximum.x() - minimum.x(),
                                  maximum.y() - minimum.y(),
                                  1.0})
                      : 1.0;
}

bool solveLinearSystem3x3(std::array<std::array<qreal, 4>, 3> matrix,
                          std::array<qreal, 3> *solution)
{
    if (solution == nullptr) {
        return false;
    }

    for (int column = 0; column < 3; ++column) {
        int pivot = column;
        for (int row = column + 1; row < 3; ++row) {
            if (std::abs(matrix[row][column]) >
                std::abs(matrix[pivot][column])) {
                pivot = row;
            }
        }
        if (std::abs(matrix[pivot][column]) <= 1.0e-10) {
            return false;
        }
        std::swap(matrix[pivot], matrix[column]);

        const qreal divisor = matrix[column][column];
        for (int entry = column; entry < 4; ++entry) {
            matrix[column][entry] /= divisor;
        }
        for (int row = 0; row < 3; ++row) {
            if (row == column) {
                continue;
            }
            const qreal factor = matrix[row][column];
            for (int entry = column; entry < 4; ++entry) {
                matrix[row][entry] -= factor * matrix[column][entry];
            }
        }
    }

    for (int row = 0; row < 3; ++row) {
        (*solution)[row] = matrix[row][3];
    }
    return true;
}

qreal threeCurveResidual(const QVector<QVector<QVector<QPointF>>> &curveGroups,
                         const QPointF &center,
                         qreal radius)
{
    qreal sumSquared = 0.0;
    for (const auto &group : curveGroups) {
        const ClosestPoint closest = closestPointOnCurves(group, center);
        if (!std::isfinite(closest.distance)) {
            return std::numeric_limits<qreal>::infinity();
        }
        const qreal residual = closest.distance - radius;
        sumSquared += residual * residual;
    }
    return sumSquared;
}

bool solveCircleTangentToTwoCurves(
    const QVector<QVector<QVector<QPointF>>> &curveGroups,
    const QPointF &seed,
    QPointF *center,
    qreal *radius)
{
    if (curveGroups.size() != 2 || center == nullptr || radius == nullptr) {
        return false;
    }

    QPointF candidate = seed;
    const qreal tolerance = curveGroupsScale(curveGroups) * 1.0e-6;
    bool converged = false;
    for (int iteration = 0; iteration < 32; ++iteration) {
        const ClosestPoint first = closestPointOnCurves(curveGroups[0], candidate);
        const ClosestPoint second = closestPointOnCurves(curveGroups[1], candidate);
        if (!std::isfinite(first.distance) || !std::isfinite(second.distance) ||
            first.distance <= kGeometryEpsilon ||
            second.distance <= kGeometryEpsilon) {
            return false;
        }

        const QPointF firstNormal = (candidate - first.point) / first.distance;
        const QPointF secondNormal = (candidate - second.point) / second.distance;
        const QPointF gradient = firstNormal - secondNormal;
        const qreal gradientLengthSquared = gradient.x() * gradient.x() +
                                            gradient.y() * gradient.y();
        const qreal error = first.distance - second.distance;
        if (std::abs(error) <= tolerance) {
            converged = true;
            break;
        }
        if (gradientLengthSquared <= 1.0e-12) {
            return false;
        }

        const qreal step = error / gradientLengthSquared;
        candidate -= gradient * step;
        if (!std::isfinite(candidate.x()) || !std::isfinite(candidate.y())) {
            return false;
        }
    }

    const ClosestPoint first = closestPointOnCurves(curveGroups[0], candidate);
    const ClosestPoint second = closestPointOnCurves(curveGroups[1], candidate);
    const qreal solvedRadius = (first.distance + second.distance) * 0.5;
    if (!converged && std::abs(first.distance - second.distance) > tolerance * 4.0) {
        return false;
    }
    if (solvedRadius <= tolerance) {
        return false;
    }

    *center = candidate;
    *radius = solvedRadius;
    return true;
}

bool solveCircleTangentToThreeCurves(
    const QVector<QVector<QVector<QPointF>>> &curveGroups,
    const QPointF &seed,
    QPointF *center,
    qreal *radius)
{
    if (curveGroups.size() != 3 || center == nullptr || radius == nullptr) {
        return false;
    }

    QPointF candidate = seed;
    qreal candidateRadius = 0.0;
    const qreal tolerance = curveGroupsScale(curveGroups) * 1.0e-4;
    for (const auto &group : curveGroups) {
        const ClosestPoint closest = closestPointOnCurves(group, candidate);
        if (!std::isfinite(closest.distance)) {
            return false;
        }
        candidateRadius += closest.distance / 3.0;
    }
    if (candidateRadius <= tolerance * 1.0e-3) {
        return false;
    }

    for (int iteration = 0; iteration < 48; ++iteration) {
        std::array<std::array<qreal, 4>, 3> matrix{};
        qreal maxError = 0.0;
        for (int curveIndex = 0; curveIndex < 3; ++curveIndex) {
            const ClosestPoint closest =
                closestPointOnCurves(curveGroups[curveIndex], candidate);
            if (!std::isfinite(closest.distance) ||
                closest.distance <= kGeometryEpsilon) {
                return false;
            }
            const QPointF normal = (candidate - closest.point) / closest.distance;
            const qreal error = closest.distance - candidateRadius;
            maxError = std::max(maxError, std::abs(error));
            matrix[curveIndex] = {normal.x(), normal.y(), -1.0, -error};
        }

        if (maxError <= tolerance) {
            *center = candidate;
            *radius = candidateRadius;
            return candidateRadius > tolerance * 1.0e-3;
        }

        std::array<qreal, 3> delta{};
        if (!solveLinearSystem3x3(matrix, &delta)) {
            return false;
        }

        const qreal previousResidual =
            threeCurveResidual(curveGroups, candidate, candidateRadius);
        bool improved = false;
        for (qreal fraction = 1.0; fraction >= 1.0 / 128.0; fraction *= 0.5) {
            const QPointF nextCenter(candidate.x() + delta[0] * fraction,
                                     candidate.y() + delta[1] * fraction);
            const qreal nextRadius = candidateRadius + delta[2] * fraction;
            if (nextRadius <= tolerance * 1.0e-3) {
                continue;
            }
            const qreal nextResidual =
                threeCurveResidual(curveGroups, nextCenter, nextRadius);
            if (nextResidual < previousResidual) {
                candidate = nextCenter;
                candidateRadius = nextRadius;
                improved = true;
                break;
            }
        }
        if (!improved) {
            return false;
        }
    }

    return false;
}

qreal threeCurveSignedResidual(
    const QVector<QVector<QVector<QPointF>>> &curveGroups,
    const QVector<bool> &closedTargets,
    const QPointF &center,
    qreal radius,
    const TangencySigns &signs)
{
    qreal sumSquared = 0.0;
    for (int index = 0; index < 3; ++index) {
        SignedDistance distance;
        qreal radiusCoefficient = 0.0;
        if (!branchDistanceToCurves(curveGroups[index],
                                    closedTargets[index],
                                    signs[index],
                                    center,
                                    &distance,
                                    &radiusCoefficient)) {
            return std::numeric_limits<qreal>::infinity();
        }
        const qreal residual = distance.value - radiusCoefficient * radius;
        sumSquared += residual * residual;
    }
    return sumSquared;
}

bool solveCircleTangentToThreeCurves(
    const QVector<QVector<QVector<QPointF>>> &curveGroups,
    const QVector<bool> &closedTargets,
    const QPointF &seed,
    const TangencySigns &signs,
    QPointF *center,
    qreal *radius)
{
    if (curveGroups.size() != 3 || closedTargets.size() != 3 ||
        center == nullptr || radius == nullptr) {
        return false;
    }

    QPointF candidate = seed;
    qreal candidateRadius = 0.0;
    const qreal tolerance = curveGroupsScale(curveGroups) * 1.0e-5;
    for (int index = 0; index < 3; ++index) {
        SignedDistance distance;
        qreal radiusCoefficient = 0.0;
        if (!branchDistanceToCurves(curveGroups[index],
                                    closedTargets[index],
                                    signs[index],
                                    candidate,
                                    &distance,
                                    &radiusCoefficient)) {
            return false;
        }
        candidateRadius += std::abs(distance.value) / 3.0;
    }
    if (candidateRadius <= tolerance * 1.0e-3) {
        return false;
    }

    for (int iteration = 0; iteration < 48; ++iteration) {
        std::array<std::array<qreal, 4>, 3> matrix{};
        qreal maxError = 0.0;
        for (int index = 0; index < 3; ++index) {
            SignedDistance distance;
            qreal radiusCoefficient = 0.0;
            if (!branchDistanceToCurves(curveGroups[index],
                                        closedTargets[index],
                                        signs[index],
                                        candidate,
                                        &distance,
                                        &radiusCoefficient)) {
                return false;
            }
            const qreal error = distance.value - radiusCoefficient * candidateRadius;
            maxError = std::max(maxError, std::abs(error));
            matrix[index] = {distance.gradient.x(),
                             distance.gradient.y(),
                             -radiusCoefficient,
                             -error};
        }

        if (maxError <= tolerance) {
            *center = candidate;
            *radius = candidateRadius;
            return candidateRadius > tolerance * 1.0e-3;
        }

        std::array<qreal, 3> delta{};
        if (!solveLinearSystem3x3(matrix, &delta)) {
            return false;
        }

        const qreal previousResidual =
            threeCurveSignedResidual(curveGroups,
                                     closedTargets,
                                     candidate,
                                     candidateRadius,
                                     signs);
        bool improved = false;
        for (qreal fraction = 1.0; fraction >= 1.0 / 128.0; fraction *= 0.5) {
            const QPointF nextCenter(candidate.x() + delta[0] * fraction,
                                     candidate.y() + delta[1] * fraction);
            const qreal nextRadius = candidateRadius + delta[2] * fraction;
            if (nextRadius <= tolerance * 1.0e-3) {
                continue;
            }
            const qreal nextResidual = threeCurveSignedResidual(
                curveGroups, closedTargets, nextCenter, nextRadius, signs);
            if (nextResidual < previousResidual) {
                candidate = nextCenter;
                candidateRadius = nextRadius;
                improved = true;
                break;
            }
        }
        if (!improved) {
            return false;
        }
    }

    return false;
}

bool solveTangentBranchFromMultipleSeeds(
    const QVector<QVector<QVector<QPointF>>> &curveGroups,
    const QVector<bool> &closedTargets,
    const QPointF &cursor,
    const QPointF &previousCenter,
    const TangencySigns &signs,
    QPointF *center,
    qreal *radius)
{
    if (solveCircleTangentToThreeCurves(curveGroups,
                                        closedTargets,
                                        cursor,
                                        signs,
                                        center,
                                        radius) ||
        solveCircleTangentToThreeCurves(curveGroups,
                                        closedTargets,
                                        previousCenter,
                                        signs,
                                        center,
                                        radius)) {
        return true;
    }

    QPointF minimum;
    QPointF maximum;
    bool found = false;
    for (const auto &group : curveGroups) {
        for (const auto &curve : group) {
            for (const QPointF &point : curve) {
                if (!found) {
                    minimum = maximum = point;
                    found = true;
                } else {
                    minimum.setX(std::min(minimum.x(), point.x()));
                    minimum.setY(std::min(minimum.y(), point.y()));
                    maximum.setX(std::max(maximum.x(), point.x()));
                    maximum.setY(std::max(maximum.y(), point.y()));
                }
            }
        }
    }
    if (!found) {
        return false;
    }

    const QPointF middle = (minimum + maximum) * 0.5;
    const qreal scale = curveGroupsScale(curveGroups);
    constexpr qreal diagonal = 0.70710678118654752440;
    constexpr std::array<QPointF, 8> directions{
        QPointF(1.0, 0.0), QPointF(-1.0, 0.0),
        QPointF(0.0, 1.0), QPointF(0.0, -1.0),
        QPointF(diagonal, diagonal), QPointF(-diagonal, diagonal),
        QPointF(diagonal, -diagonal), QPointF(-diagonal, -diagonal),
    };
    for (qreal size : {0.35, 1.1, 2.5}) {
        for (const QPointF &direction : directions) {
            if (solveCircleTangentToThreeCurves(curveGroups,
                                                closedTargets,
                                                middle + direction * (size * scale),
                                                signs,
                                                center,
                                                radius)) {
                return true;
            }
        }
    }
    return false;
}

bool isCircleTangentTarget(const Shape &shape)
{
    switch (shape.geometryType) {
    case GeometryType::Line:
    case GeometryType::Arc:
    case GeometryType::Bezier:
    case GeometryType::Nurbs:
    case GeometryType::Rectangle:
    case GeometryType::Circle:
    case GeometryType::Ellipse:
    case GeometryType::PolyCurve:
    case GeometryType::Polygon:
        return true;
    case GeometryType::Invalid:
    case GeometryType::Point:
    case GeometryType::LinearDimension:
    case GeometryType::AngularDimension:
    case GeometryType::Picture:
    case GeometryType::NurbsSurface:
        return false;
    }
    return false;
}

bool isClosedCurveTarget(const Shape &shape,
                         const QVector<QVector<QPointF>> &curves)
{
    if (shape.geometryType == GeometryType::Circle ||
        shape.geometryType == GeometryType::Ellipse ||
        shape.geometryType == GeometryType::Rectangle ||
        shape.geometryType == GeometryType::Polygon) {
        return true;
    }
    if (curves.isEmpty() || curves.first().isEmpty() ||
        curves.last().isEmpty()) {
        return false;
    }
    const QPointF offset = curves.first().first() - curves.last().last();
    return std::hypot(offset.x(), offset.y()) <= 1.0e-7;
}

} // namespace

CircleTangentTool::CircleTangentTool(ToolId tool)
    : tool_(tool)
{
}

ToolId CircleTangentTool::id() const
{
    return tool_;
}

void CircleTangentTool::begin(ToolContext &context)
{
    selectedCurveIds_.clear();
    sampledCurveGroups_.clear();
    closedCurveTargets_.clear();
    circleTargets_.clear();
    exactSolutions_.clear();
    drawingFrame_ = context.viewportTransform().workPlaneFrame();
    previewShape_ = Shape{};
    previewAvailable_ = false;
    hasLastPreviewInput_ = false;
    solutionCycleActive_ = false;
    tangentSolutionIndex_ = -1;
    status_.state = ToolLifecycleState::Active;
    status_.text = QStringLiteral("Click curve 1 of %1")
                       .arg(tool_ == ToolId::CircleTangentThree ? 3 : 2);
    status_.canCommit = false;
    publish(context);
}

bool CircleTangentTool::handleMousePress(const ToolInput &input,
                                         ToolContext &context)
{
    if (input.button != Qt::LeftButton) {
        return false;
    }

    const int requiredCurves = tool_ == ToolId::CircleTangentThree ? 3 : 2;
    if (selectedCurveIds_.size() < requiredCurves) {
        const int shapeIndex = context.curveHitTester().hitTestShape(
            context.document(),
            input.screenPosition,
            context.viewportTransform(),
            input.viewportSize);
        if (shapeIndex < 0) {
            status_.text = QStringLiteral("Click a curve to select it (%1 of %2)")
                               .arg(selectedCurveIds_.size() + 1)
                               .arg(requiredCurves);
            publish(context);
            return true;
        }

        const Shape &shape = context.document()[shapeIndex];
        const ObjectId objectId = context.document().objectIdAt(shapeIndex);
        if (!isCircleTangentTarget(shape) || !objectId.isValid()) {
            status_.text = QStringLiteral("Choose a curve, not a point");
            publish(context);
            return true;
        }
        if (selectedCurveIds_.contains(objectId)) {
            status_.text = QStringLiteral("Choose a different curve");
            publish(context);
            return true;
        }

        if (selectedCurveIds_.isEmpty()) {
            drawingFrame_ = shapeWorkPlaneFrame(shape);
            if (!isValidWorkPlaneFrame(drawingFrame_)) {
                status_.text = QStringLiteral("That curve has no valid drawing plane");
                publish(context);
                return true;
            }
            context.viewportTransform().setWorkPlaneFrame(drawingFrame_);
        }

        QVector<QVector<QPointF>> sampledCurves;
        if (!collectTargetCurves(shape, context, &sampledCurves)) {
            status_.text = QStringLiteral("Could not read that curve's geometry");
            publish(context);
            return true;
        }

        selectedCurveIds_.append(objectId);
        closedCurveTargets_.append(isClosedCurveTarget(shape, sampledCurves));
        sampledCurveGroups_.append(sampledCurves);
        TangentCircleCandidate circleTarget;
        if (!exactCircleTarget(shape, sampledCurves, &circleTarget)) {
            circleTarget.radius = 0.0;
        }
        circleTargets_.append(circleTarget);
        if (tool_ == ToolId::CircleTangentThree && circleTargets_.size() == 3) {
            exactSolutions_ = tangentCirclesForThreeCircles(circleTargets_,
                                                             sampledCurveGroups_,
                                                             closedCurveTargets_);
        }
        context.selection().setObjectIds(selectedCurveIds_, objectId);
        previewAvailable_ = false;
        status_.canCommit = false;
        status_.text = selectedCurveIds_.size() < requiredCurves
                           ? QStringLiteral("Click curve %1 of %2")
                                 .arg(selectedCurveIds_.size() + 1)
                                 .arg(requiredCurves)
                           : QStringLiteral("Move to preview a tangent circle, then click to place");
        if (selectedCurveIds_.size() == requiredCurves &&
            !exactSolutions_.isEmpty()) {
            updateCirclePreview(input, context);
        } else {
            publish(context);
        }
        return true;
    }

    if (!updateCirclePreview(input, context)) {
        return true;
    }
    if (!context.commitShape(ToolId::Circle, previewShape_)) {
        status_.text = QStringLiteral("Could not create the tangent circle");
        publish(context);
        return true;
    }

    status_.state = ToolLifecycleState::Completed;
    status_.text = QStringLiteral("Tangent circle created");
    status_.canCommit = false;
    previewAvailable_ = false;
    publish(context);
    context.finishTool(ToolId::Select);
    return true;
}

bool CircleTangentTool::handleMouseMove(const ToolInput &input,
                                       ToolContext &context)
{
    if (sampledCurveGroups_.size() ==
        (tool_ == ToolId::CircleTangentThree ? 3 : 2)) {
        updateCirclePreview(input, context);
    }
    return true;
}

bool CircleTangentTool::handleKey(const ToolInput &input, ToolContext &context)
{
    if (tool_ == ToolId::CircleTangentThree &&
        (input.key == Qt::Key_Tab || input.key == Qt::Key_Backtab) &&
        (input.modifiers == Qt::NoModifier ||
         input.modifiers == Qt::ShiftModifier)) {
        const int direction = input.key == Qt::Key_Backtab ||
                                      input.modifiers == Qt::ShiftModifier
                                  ? -1
                                  : 1;
        cycleTangentSolution(direction, context);
        return true;
    }

    if (input.key != Qt::Key_Escape) {
        return false;
    }

    status_.state = ToolLifecycleState::Cancelled;
    status_.text = QStringLiteral("Tangent circle cancelled");
    status_.canCommit = false;
    selectedCurveIds_.clear();
    sampledCurveGroups_.clear();
    closedCurveTargets_.clear();
    circleTargets_.clear();
    exactSolutions_.clear();
    previewAvailable_ = false;
    hasLastPreviewInput_ = false;
    solutionCycleActive_ = false;
    tangentSolutionIndex_ = -1;
    publish(context);
    context.finishTool(ToolId::Select);
    return true;
}

void CircleTangentTool::cancel(ToolContext &context)
{
    selectedCurveIds_.clear();
    sampledCurveGroups_.clear();
    closedCurveTargets_.clear();
    circleTargets_.clear();
    exactSolutions_.clear();
    previewAvailable_ = false;
    hasLastPreviewInput_ = false;
    solutionCycleActive_ = false;
    tangentSolutionIndex_ = -1;
    status_.state = ToolLifecycleState::Cancelled;
    status_.text = QStringLiteral("Tangent circle cancelled");
    status_.canCommit = false;
    publish(context);
}

ToolPreview CircleTangentTool::preview() const
{
    ToolPreview result;
    result.workPlaneFrame = drawingFrame_;
    result.hasWorkPlaneFrame = isValidWorkPlaneFrame(drawingFrame_);
    result.planeLocked = !selectedCurveIds_.isEmpty();
    result.shape = previewShape_;
    result.hasShape = previewAvailable_;
    result.statusText = status_.text;
    return result;
}

ToolStatus CircleTangentTool::status() const
{
    return status_;
}

bool CircleTangentTool::collectTargetCurves(
    const Shape &shape,
    ToolContext &context,
    QVector<QVector<QPointF>> *sampledCurves) const
{
    if (sampledCurves == nullptr) {
        return false;
    }
    sampledCurves->clear();

    const WorkPlaneFrame sourceFrame = shapeWorkPlaneFrame(shape);
    const qreal normalDot = sourceFrame.normal.x * drawingFrame_.normal.x +
                            sourceFrame.normal.y * drawingFrame_.normal.y +
                            sourceFrame.normal.z * drawingFrame_.normal.z;
    if (!isValidWorkPlaneFrame(sourceFrame) ||
        !isValidWorkPlaneFrame(drawingFrame_) ||
        std::abs(std::abs(normalDot) - 1.0) > 1.0e-6 ||
        std::abs(signedDistanceFromWorkPlaneFrame(sourceFrame.origin,
                                                  drawingFrame_)) > 1.0e-7) {
        return false;
    }

    QVector<Shape::NurbsCurve2D> curves = context.curveSampler().curvesForShape(shape);
    for (const Shape::NurbsCurve2D &curve : curves) {
        if (!validateNurbsCurve(curve)) {
            continue;
        }
        QVector<qreal> spanBreaks;
        const QVector<double> fullKnots = expandedNurbsKnotVector(curve);
        for (int span = curve.degree; span < curve.controlPoints.size(); ++span) {
            if (fullKnots[span + 1] > fullKnots[span]) {
                spanBreaks.append(fullKnots[span]);
                spanBreaks.append(fullKnots[span + 1]);
            }
        }
        if (spanBreaks.isEmpty()) {
            continue;
        }

        const int spanCount = spanBreaks.size() / 2;
        const int samplesPerSpan = std::clamp(512 / spanCount, 24, 64);
        QVector<QPointF> points;
        points.reserve(spanCount * samplesPerSpan + 1);
        for (int span = 0; span < spanCount; ++span) {
            const qreal start = spanBreaks[span * 2];
            const qreal end = spanBreaks[span * 2 + 1];
            for (int sample = 0; sample <= samplesPerSpan; ++sample) {
                if (span > 0 && sample == 0) {
                    continue;
                }
                const qreal fraction = static_cast<qreal>(sample) / samplesPerSpan;
                QPointF point;
                if (evaluateNurbsPoint(curve, start + (end - start) * fraction, &point)) {
                    const Point3D world = workPlaneFramePointToWorld(point,
                                                                     sourceFrame);
                    if (std::abs(signedDistanceFromWorkPlaneFrame(world,
                                                                  drawingFrame_)) > 1.0e-7) {
                        return false;
                    }
                    points.append(worldPointToWorkPlaneFrame(world, drawingFrame_));
                }
            }
        }
        if (points.size() >= 2) {
            sampledCurves->append(points);
        }
    }
    return !sampledCurves->isEmpty();
}

bool CircleTangentTool::cycleTangentSolution(int direction,
                                             ToolContext &context)
{
    if (sampledCurveGroups_.size() != 3 || !hasLastPreviewInput_) {
        status_.text = QStringLiteral("Select 3 curves and move the cursor before cycling solutions");
        status_.canCommit = previewAvailable_;
        publish(context);
        return false;
    }

    if (!exactSolutions_.isEmpty()) {
        const int solutionCount = exactSolutions_.size();
        tangentSolutionIndex_ = (tangentSolutionIndex_ + direction + solutionCount) %
                                solutionCount;
        solutionCycleActive_ = true;
        return updateCirclePreview(lastPreviewInput_, context);
    }

    const int currentIndex = solutionCycleActive_
                                 ? tangentSolutionIndex_
                                 : tangencyPermutationAt(sampledCurveGroups_,
                                                         lastPreviewInput_.worldPosition);
    const QPointF previousCenter = previewShape_.points.value(0);
    tangentSolutionIndex_ = (currentIndex + direction +
                             static_cast<int>(kTangencyPermutations.size())) %
                            static_cast<int>(kTangencyPermutations.size());
    solutionCycleActive_ = true;
    QPointF center;
    qreal radius = 0.0;
    const bool solved = solveTangentBranchFromMultipleSeeds(
        sampledCurveGroups_,
        closedCurveTargets_,
        lastPreviewInput_.worldPosition,
        previousCenter,
        kTangencyPermutations[tangentSolutionIndex_],
        &center,
        &radius);
    previewAvailable_ = false;
    if (solved) {
        const QVector<QPointF> circleDefinition{center,
                                                 center + QPointF(radius, 0.0)};
        previewAvailable_ = context.createShape(ToolId::Circle,
                                                circleDefinition,
                                                ArcMode::TwoPoint,
                                                0.0,
                                                &previewShape_);
        if (previewAvailable_) {
            previewShape_.workPlaneFrame = drawingFrame_;
        }
    }
    status_.state = ToolLifecycleState::Active;
    status_.canCommit = previewAvailable_;
    status_.text = previewAvailable_
                       ? QStringLiteral("Tangent configuration %1 of 8 — Tab: next, Shift+Tab: previous, click: place")
                             .arg(tangentSolutionIndex_ + 1)
                       : QStringLiteral("Tangent configuration %1 of 8 has no solution here — Tab tries the next")
                             .arg(tangentSolutionIndex_ + 1);
    publish(context);
    return previewAvailable_;
}

bool CircleTangentTool::updateCirclePreview(const ToolInput &input,
                                            ToolContext &context)
{
    lastPreviewInput_ = input;
    hasLastPreviewInput_ = true;
    QPointF center;
    qreal radius = 0.0;
    bool solved = false;
    if (!exactSolutions_.isEmpty()) {
        if (!solutionCycleActive_) {
            qreal bestDistanceSquared = std::numeric_limits<qreal>::infinity();
            for (int index = 0; index < exactSolutions_.size(); ++index) {
                const QPointF offset = exactSolutions_[index].center -
                                       input.worldPosition;
                const qreal distanceSquared = squaredLength(offset);
                if (distanceSquared < bestDistanceSquared) {
                    bestDistanceSquared = distanceSquared;
                    tangentSolutionIndex_ = index;
                }
            }
        }
        const TangentCircleCandidate &solution =
            exactSolutions_[tangentSolutionIndex_];
        center = solution.center;
        radius = solution.radius;
        solved = true;
    } else if (tool_ == ToolId::CircleTangentThree) {
        if (solutionCycleActive_ && previewAvailable_) {
            return true;
        }
        solved = solutionCycleActive_
                     ? solveCircleTangentToThreeCurves(
                           sampledCurveGroups_,
                           closedCurveTargets_,
                           input.worldPosition,
                           kTangencyPermutations[tangentSolutionIndex_],
                           &center,
                           &radius)
                     : solveCircleTangentToThreeCurves(sampledCurveGroups_,
                                                       input.worldPosition,
                                                       &center,
                                                       &radius);
    } else {
        solved = solveCircleTangentToTwoCurves(sampledCurveGroups_,
                                               input.worldPosition,
                                               &center,
                                               &radius);
    }
    previewAvailable_ = false;
    if (solved) {
        const QVector<QPointF> circleDefinition{
            center,
            center + QPointF(radius, 0.0),
        };
        previewAvailable_ = context.createShape(ToolId::Circle,
                                                circleDefinition,
                                                ArcMode::TwoPoint,
                                                0.0,
                                                &previewShape_);
        if (previewAvailable_) {
            previewShape_.workPlaneFrame = drawingFrame_;
        }
    }

    status_.state = ToolLifecycleState::Active;
    status_.canCommit = previewAvailable_;
    if (previewAvailable_ && !exactSolutions_.isEmpty()) {
        status_.text = QStringLiteral("Tangent solution %1 of %2 — Tab: next, Shift+Tab: previous, click: place")
                           .arg(tangentSolutionIndex_ + 1)
                           .arg(exactSolutions_.size());
    } else if (previewAvailable_ && solutionCycleActive_) {
        status_.text = QStringLiteral("Tangent configuration %1 of 8 — Tab: next, Shift+Tab: previous, click: place")
                           .arg(tangentSolutionIndex_ + 1);
    } else if (previewAvailable_ && tool_ == ToolId::CircleTangentThree) {
        status_.text = QStringLiteral("Tangent circle ready — Tab cycles solutions; click to place; Esc cancels");
    } else {
        status_.text = previewAvailable_
                           ? QStringLiteral("Tangent circle ready — click to place; Esc cancels")
                           : solutionCycleActive_
                                 ? QStringLiteral("Tangent configuration %1 of 8 has no solution here — Tab tries the next")
                                       .arg(tangentSolutionIndex_ + 1)
                                 : QStringLiteral("Move to a position where a tangent circle fits");
    }
    publish(context);
    return previewAvailable_;
}

void CircleTangentTool::publish(ToolContext &context)
{
    context.publishPreview(preview());
    context.publishStatus(status_);
}

} // namespace classiCAD
