/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace classiCAD {

// Premultiplied scene-linear RGBA. RGB is intentionally allowed above 1.0;
// this type is the CPU Workbench surface path's pre-display-transform value.
struct RasterWorkbenchLinearPixel {
    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;
    float alpha = 0.0f;

    static RasterWorkbenchLinearPixel fromStraight(float red, float green,
                                                    float blue, float alpha) noexcept;
};

// A straight-color fragment at one pixel. Smaller cameraDepth values are
// farther from the camera; fragments at a pixel resolve far-to-near. Equal
// depths retain their input order.
struct RasterWorkbenchLinearFragment {
    // Physical target-pixel coordinates (not logical Qt coordinates).
    int x = 0;
    int y = 0;
    // Signed camera-forward depth: -dot(worldPosition - cameraPosition,
    // viewDirection), matching rasterWorkbenchCameraDepth; larger is nearer.
    float cameraDepth = 0.0f;
    std::array<float, 3> sceneLinearRgb{};
    float alpha = 1.0f;
    std::uint64_t sourceId = 0;
};

// Optional observation target used by CPU surface rasterization. Collected
// fragments are diagnostic inputs only; the QPainter display path is unchanged.
using RasterWorkbenchFragmentSink =
    std::vector<RasterWorkbenchLinearFragment>;

// Resolve depth-sorted fragments over a premultiplied scene-linear base.
// Invalid dimensions, pixels, alpha values, or non-finite inputs fail without
// modifying `output`. On success output has width*height row-major pixels.
bool resolveRasterWorkbenchLinearFragments(
    int width, int height, const RasterWorkbenchLinearPixel &base,
    const std::vector<RasterWorkbenchLinearFragment> &fragments,
    std::vector<RasterWorkbenchLinearPixel> *output);

// Resolve transparent fragments with Blender Workbench 5.2.2's weighted
// blended OIT accumulation and revealage equations. cameraDepth is the
// fragment's signed camera-forward linear depth; the weight uses its absolute
// value, as Blender's shader does with linear view-space Z.
float rasterWorkbenchTransparentWeight(float cameraDepth) noexcept;
bool resolveRasterWorkbenchWeightedOitFragments(
    int width, int height, const RasterWorkbenchLinearPixel &base,
    const std::vector<RasterWorkbenchLinearFragment> &fragments,
    std::vector<RasterWorkbenchLinearPixel> *output);

} // namespace classiCAD
