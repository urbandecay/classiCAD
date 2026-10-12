#pragma once

#include <QString>
#include <QVector>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace classiCAD {

using ScalarVolumeMatrix = std::array<double, 16>;

inline constexpr ScalarVolumeMatrix identityScalarVolumeMatrix{
    1.0, 0.0, 0.0, 0.0,
    0.0, 1.0, 0.0, 0.0,
    0.0, 0.0, 1.0, 0.0,
    0.0, 0.0, 0.0, 1.0};

struct ScalarVolumeActiveRecord {
    int x = 0;
    int y = 0;
    int z = 0;
    int extent = 1;
    bool isTile = false;
    float value = 0.0f;
};

// Sparse payload for one Blender scalar FLOAT volume grid. `extent` is the
// cube edge in voxels; a tile record carries its uniform value over that cube.
struct ScalarVolume3D {
    QString name;
    QString dataType = QStringLiteral("FLOAT");
    int channels = 1;
    float background = 0.0f;
    float displayDensity = 1.0f;
    float densityScale = 1.0f;
    QString renderSpace = QStringLiteral("OBJECT");
    QString interpolationMethod = QStringLiteral("LINEAR");
    bool useSlice = false;
    QString sliceAxis = QStringLiteral("AUTO");
    float sliceDepth = 0.0f;
    ScalarVolumeMatrix gridToObject = identityScalarVolumeMatrix;
    ScalarVolumeMatrix objectToWorld = identityScalarVolumeMatrix;
    // Inclusive min xyz / max xyz of the expanded active voxel region.
    std::array<int, 6> activeBounds{0, 0, 0, -1, -1, -1};
    std::uint64_t expandedVoxelCount = 0;
    std::uint64_t denseVoxelCount = 0;
    QVector<ScalarVolumeActiveRecord> activeRecords;
};

inline bool isValidScalarVolumeMatrix(const ScalarVolumeMatrix &matrix)
{
    for (double value : matrix) {
        if (!std::isfinite(value) ||
            std::abs(value) > std::numeric_limits<float>::max()) return false;
    }
    const double determinant =
        matrix[0] * (matrix[5] * matrix[10] - matrix[6] * matrix[9]) -
        matrix[1] * (matrix[4] * matrix[10] - matrix[6] * matrix[8]) +
        matrix[2] * (matrix[4] * matrix[9] - matrix[5] * matrix[8]);
    return std::isfinite(determinant) && std::abs(determinant) > 1.0e-15 &&
           std::abs(matrix[12]) <= 1.0e-9 &&
           std::abs(matrix[13]) <= 1.0e-9 &&
           std::abs(matrix[14]) <= 1.0e-9 &&
           std::abs(matrix[15] - 1.0) <= 1.0e-9;
}

inline bool scalarVolumeDensityScale(const QString &renderSpace,
                                     const ScalarVolumeMatrix &objectToWorld,
                                     float displayDensity,
                                     float *densityScale)
{
    if (densityScale == nullptr || !std::isfinite(displayDensity) ||
        displayDensity < 0.0f ||
        (renderSpace != QStringLiteral("OBJECT") &&
         renderSpace != QStringLiteral("WORLD"))) return false;
    double scale = 1.0;
    if (renderSpace == QStringLiteral("OBJECT")) {
        constexpr double inverseSqrtThree = 0.57735026918962576451;
        // Blender's mul_mat3_m4_v3 computes each output component from a
        // matrix column (the same convention used by BKE_volume_density_scale).
        const double x = (objectToWorld[0] + objectToWorld[4] + objectToWorld[8]) * inverseSqrtThree;
        const double y = (objectToWorld[1] + objectToWorld[5] + objectToWorld[9]) * inverseSqrtThree;
        const double z = (objectToWorld[2] + objectToWorld[6] + objectToWorld[10]) * inverseSqrtThree;
        const double axisScale = std::sqrt(x * x + y * y + z * z);
        if (!std::isfinite(axisScale) || axisScale <= 1.0e-15) return false;
        scale = 1.0 / axisScale;
    }
    const double effectiveDensity = double(displayDensity) * scale;
    if (!std::isfinite(effectiveDensity) ||
        effectiveDensity > std::numeric_limits<float>::max()) return false;
    *densityScale = static_cast<float>(effectiveDensity);
    return std::isfinite(*densityScale);
}

inline bool validateScalarVolume(const ScalarVolume3D &volume)
{
    if (volume.dataType != QStringLiteral("FLOAT") || volume.channels != 1 ||
        !std::isfinite(volume.background) ||
        !std::isfinite(volume.displayDensity) || volume.displayDensity < 0.0f ||
        !std::isfinite(volume.densityScale) ||
        volume.densityScale < 0.0f || !std::isfinite(volume.sliceDepth) ||
        (volume.interpolationMethod != QStringLiteral("CLOSEST") &&
         volume.interpolationMethod != QStringLiteral("LINEAR") &&
         volume.interpolationMethod != QStringLiteral("CUBIC")) ||
        (volume.sliceAxis != QStringLiteral("AUTO") &&
         volume.sliceAxis != QStringLiteral("X") &&
         volume.sliceAxis != QStringLiteral("Y") &&
         volume.sliceAxis != QStringLiteral("Z")) ||
        (volume.renderSpace != QStringLiteral("OBJECT") &&
         volume.renderSpace != QStringLiteral("WORLD")) ||
        !isValidScalarVolumeMatrix(volume.gridToObject) ||
        !isValidScalarVolumeMatrix(volume.objectToWorld) ||
        volume.activeRecords.isEmpty() || volume.expandedVoxelCount == 0 ||
        volume.expandedVoxelCount > 64ull * 1024ull * 1024ull ||
        volume.denseVoxelCount == 0 ||
        volume.denseVoxelCount > 64ull * 1024ull * 1024ull ||
        volume.activeRecords.size() > 1'000'000) {
        return false;
    }
    float expectedDensityScale = 0.0f;
    if (!scalarVolumeDensityScale(volume.renderSpace, volume.objectToWorld,
                                  volume.displayDensity,
                                  &expectedDensityScale) ||
        std::abs(expectedDensityScale - volume.densityScale) >
            std::max(1.0e-6f, expectedDensityScale * 1.0e-6f)) return false;
    std::uint64_t boundsVoxelCount = 1;
    for (int axis = 0; axis < 3; ++axis) {
        const std::int64_t extent =
            std::int64_t(volume.activeBounds[axis + 3]) - volume.activeBounds[axis] + 1;
        if (extent <= 0 || extent > 64ll * 1024ll * 1024ll ||
            boundsVoxelCount > (64ull * 1024ull * 1024ull) /
                                   static_cast<std::uint64_t>(extent)) return false;
        boundsVoxelCount *= static_cast<std::uint64_t>(extent);
    }
    if (boundsVoxelCount != volume.denseVoxelCount) return false;
    std::uint64_t count = 0;
    std::array<std::int64_t, 6> actualBounds{
        INT64_MAX, INT64_MAX, INT64_MAX, INT64_MIN, INT64_MIN, INT64_MIN};
    for (const ScalarVolumeActiveRecord &record : volume.activeRecords) {
        if (record.extent <= 0 || (!record.isTile && record.extent != 1) ||
            !std::isfinite(record.value) ||
            record.x < volume.activeBounds[0] ||
            record.y < volume.activeBounds[1] ||
            record.z < volume.activeBounds[2] ||
            std::int64_t(record.x) + record.extent - 1 > volume.activeBounds[3] ||
            std::int64_t(record.y) + record.extent - 1 > volume.activeBounds[4] ||
            std::int64_t(record.z) + record.extent - 1 > volume.activeBounds[5]) {
            return false;
        }
        if (record.extent > 4096) return false;
        const std::uint64_t edge = static_cast<std::uint64_t>(record.extent);
        const std::uint64_t cube = edge * edge * edge;
        if (cube > volume.expandedVoxelCount ||
            count > volume.expandedVoxelCount - cube) {
            return false;
        }
        count += edge * edge * edge;
        actualBounds[0] = std::min<std::int64_t>(actualBounds[0], record.x);
        actualBounds[1] = std::min<std::int64_t>(actualBounds[1], record.y);
        actualBounds[2] = std::min<std::int64_t>(actualBounds[2], record.z);
        actualBounds[3] = std::max<std::int64_t>(actualBounds[3], std::int64_t(record.x) + record.extent - 1);
        actualBounds[4] = std::max<std::int64_t>(actualBounds[4], std::int64_t(record.y) + record.extent - 1);
        actualBounds[5] = std::max<std::int64_t>(actualBounds[5], std::int64_t(record.z) + record.extent - 1);
    }
    if (count != volume.expandedVoxelCount) return false;
    for (int i = 0; i < 6; ++i) {
        if (actualBounds[i] != volume.activeBounds[i]) return false;
    }
    return true;
}

} // namespace classiCAD
