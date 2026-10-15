/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "viewport_cavity_blue_noise.h"

#include <QByteArray>

#include <algorithm>
#include <array>
#include <cmath>

namespace classiCAD {
namespace detail {

inline constexpr int kWorkbenchCavityJitterTextureSize = 64;
inline constexpr int kWorkbenchCavityMaxSamples = 512;
inline constexpr int kWorkbenchCavityJitterChannelCount = 3;
using WorkbenchCavityJitterPixels = std::array<
    float, kWorkbenchCavityJitterTextureSize *
              kWorkbenchCavityJitterTextureSize *
              kWorkbenchCavityJitterChannelCount>;

// Builds Blender 5.2.2 Workbench's RGB portion of the cavity jitter texture
// from its two-channel, 1/256-quantized EEVEE blue-noise LUT.
inline bool buildWorkbenchCavityJitterPixels(
    int totalSamples, WorkbenchCavityJitterPixels *pixels)
{
    if (pixels == nullptr) {
        return false;
    }
    const QByteArray blueNoiseRg = QByteArray::fromBase64(QByteArray(
        kWorkbenchCavityBlueNoiseRgBase64,
        static_cast<qsizetype>(sizeof(kWorkbenchCavityBlueNoiseRgBase64) - 1)));
    constexpr int kBlueNoiseChannelCount = 2;
    constexpr int kExpectedBlueNoiseBytes =
        kWorkbenchCavityJitterTextureSize *
        kWorkbenchCavityJitterTextureSize * kBlueNoiseChannelCount;
    if (blueNoiseRg.size() != kExpectedBlueNoiseBytes) {
        return false;
    }

    const int sampleCount = std::clamp(totalSamples, 1,
                                       kWorkbenchCavityMaxSamples);
    constexpr float kInverseBlueNoiseRange = 1.0f / 256.0f;
    constexpr double kPi = 3.14159265358979323846;
    const float inverseSampleCount = 1.0f / static_cast<float>(sampleCount);
    for (int pixelIndex = 0;
         pixelIndex < kWorkbenchCavityJitterTextureSize *
                          kWorkbenchCavityJitterTextureSize;
         ++pixelIndex) {
        const int sourceIndex = pixelIndex * kBlueNoiseChannelCount;
        const float angleSample = static_cast<float>(
            static_cast<unsigned char>(blueNoiseRg.at(sourceIndex))) *
            kInverseBlueNoiseRange;
        const float radiusSample = static_cast<float>(
            static_cast<unsigned char>(blueNoiseRg.at(sourceIndex + 1))) *
            kInverseBlueNoiseRange;
        const double angle = static_cast<double>(angleSample * 2.0f) * kPi;
        const int destinationIndex =
            pixelIndex * kWorkbenchCavityJitterChannelCount;
        (*pixels)[destinationIndex] = static_cast<float>(std::cos(angle));
        (*pixels)[destinationIndex + 1] =
            static_cast<float>(std::sin(angle));
        (*pixels)[destinationIndex + 2] =
            std::clamp(radiusSample - 0.5f, -0.499f, 0.499f) *
            inverseSampleCount;
    }
    return true;
}

} // namespace detail
} // namespace classiCAD
