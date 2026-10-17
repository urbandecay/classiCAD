#pragma once

#include "work_plane.h"

#include <array>
#include <cmath>
#include <functional>

namespace classiCAD {

// Row-major affine transform acting on column vectors. Translation occupies
// the fourth entry in each of the first three rows.
using AffineTransform3D = std::array<double, 16>;

inline constexpr AffineTransform3D identityAffineTransform3D{
    1.0, 0.0, 0.0, 0.0,
    0.0, 1.0, 0.0, 0.0,
    0.0, 0.0, 1.0, 0.0,
    0.0, 0.0, 0.0, 1.0};

inline Point3D transformAffinePoint(const AffineTransform3D &matrix,
                                    const Point3D &point)
{
    const double w = matrix[12] * point.x + matrix[13] * point.y +
                     matrix[14] * point.z + matrix[15];
    const double divisor = std::abs(w) > 1.0e-12 ? w : 1.0;
    return {(matrix[0] * point.x + matrix[1] * point.y +
             matrix[2] * point.z + matrix[3]) / divisor,
            (matrix[4] * point.x + matrix[5] * point.y +
             matrix[6] * point.z + matrix[7]) / divisor,
            (matrix[8] * point.x + matrix[9] * point.y +
             matrix[10] * point.z + matrix[11]) / divisor};
}

inline Point3D transformAffineVector(const AffineTransform3D &matrix,
                                     const Point3D &vector)
{
    return {matrix[0] * vector.x + matrix[1] * vector.y +
                matrix[2] * vector.z,
            matrix[4] * vector.x + matrix[5] * vector.y +
                matrix[6] * vector.z,
            matrix[8] * vector.x + matrix[9] * vector.y +
                matrix[10] * vector.z};
}

inline AffineTransform3D multiplyAffineTransforms(
    const AffineTransform3D &left, const AffineTransform3D &right)
{
    AffineTransform3D result{};
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            for (int inner = 0; inner < 4; ++inner) {
                result[static_cast<std::size_t>(row * 4 + column)] +=
                    left[static_cast<std::size_t>(row * 4 + inner)] *
                    right[static_cast<std::size_t>(inner * 4 + column)];
            }
        }
    }
    return result;
}

inline AffineTransform3D affineTranslation(const Point3D &translation)
{
    AffineTransform3D result = identityAffineTransform3D;
    result[3] = translation.x;
    result[7] = translation.y;
    result[11] = translation.z;
    return result;
}

inline AffineTransform3D affineLinearTransformFromPointMap(
    const std::function<Point3D(const Point3D &)> &map)
{
    if (!map) return identityAffineTransform3D;
    const Point3D origin = map({});
    const Point3D mappedX = map({1.0, 0.0, 0.0});
    const Point3D mappedY = map({0.0, 1.0, 0.0});
    const Point3D mappedZ = map({0.0, 0.0, 1.0});
    const Point3D x{mappedX.x - origin.x, mappedX.y - origin.y,
                    mappedX.z - origin.z};
    const Point3D y{mappedY.x - origin.x, mappedY.y - origin.y,
                    mappedY.z - origin.z};
    const Point3D z{mappedZ.x - origin.x, mappedZ.y - origin.y,
                    mappedZ.z - origin.z};
    AffineTransform3D result = identityAffineTransform3D;
    result[0] = x.x;
    result[1] = y.x;
    result[2] = z.x;
    result[4] = x.y;
    result[5] = y.y;
    result[6] = z.y;
    result[8] = x.z;
    result[9] = y.z;
    result[10] = z.z;
    return result;
}

inline bool isValidAffineTransform(const AffineTransform3D &matrix,
                                   bool requireInvertible = false)
{
    for (double value : matrix) {
        if (!std::isfinite(value)) return false;
    }
    if (std::abs(matrix[12]) > 1.0e-9 ||
        std::abs(matrix[13]) > 1.0e-9 ||
        std::abs(matrix[14]) > 1.0e-9 ||
        std::abs(matrix[15] - 1.0) > 1.0e-9) {
        return false;
    }
    if (!requireInvertible) return true;
    const double determinant =
        matrix[0] * (matrix[5] * matrix[10] - matrix[6] * matrix[9]) -
        matrix[1] * (matrix[4] * matrix[10] - matrix[6] * matrix[8]) +
        matrix[2] * (matrix[4] * matrix[9] - matrix[5] * matrix[8]);
    return std::isfinite(determinant) && std::abs(determinant) > 1.0e-15;
}

inline bool isValidLinearTransform(const AffineTransform3D &matrix)
{
    return isValidAffineTransform(matrix, true) &&
           std::abs(matrix[3]) <= 1.0e-9 &&
           std::abs(matrix[7]) <= 1.0e-9 &&
           std::abs(matrix[11]) <= 1.0e-9;
}

} // namespace classiCAD
