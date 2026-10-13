/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "viewport_raster_workbench_linear.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

namespace classiCAD {
namespace {

bool finite(const RasterWorkbenchLinearPixel &pixel)
{
    return std::isfinite(pixel.red) && std::isfinite(pixel.green) &&
           std::isfinite(pixel.blue) && std::isfinite(pixel.alpha) &&
           pixel.alpha >= 0.0f && pixel.alpha <= 1.0f;
}

bool finite(const RasterWorkbenchLinearFragment &fragment)
{
    return std::isfinite(fragment.cameraDepth) &&
           std::isfinite(fragment.sceneLinearRgb[0]) &&
           std::isfinite(fragment.sceneLinearRgb[1]) &&
           std::isfinite(fragment.sceneLinearRgb[2]) &&
           std::isfinite(fragment.alpha) && fragment.alpha >= 0.0f &&
           fragment.alpha <= 1.0f && std::isfinite(fragment.oitLinearDepth) &&
           fragment.oitLinearDepth >= -1.0f;
}

struct OrderedFragment {
    std::size_t pixelIndex = 0;
    std::size_t inputIndex = 0;
};

} // namespace

RasterWorkbenchLinearPixel RasterWorkbenchLinearPixel::fromStraight(
    float red, float green, float blue, float alpha) noexcept
{
    return {red * alpha, green * alpha, blue * alpha, alpha};
}

bool resolveRasterWorkbenchLinearFragments(
    int width, int height, const RasterWorkbenchLinearPixel &base,
    const std::vector<RasterWorkbenchLinearFragment> &fragments,
    std::vector<RasterWorkbenchLinearPixel> *output)
{
    if (output == nullptr || width <= 0 || height <= 0 || !finite(base)) {
        return false;
    }
    const std::size_t w = static_cast<std::size_t>(width);
    const std::size_t h = static_cast<std::size_t>(height);
    if (w > std::numeric_limits<std::size_t>::max() / h) return false;
    const std::size_t pixelCount = w * h;

    std::vector<OrderedFragment> order;
    order.reserve(fragments.size());
    for (std::size_t index = 0; index < fragments.size(); ++index) {
        const RasterWorkbenchLinearFragment &fragment = fragments[index];
        if (!finite(fragment) || fragment.x < 0 || fragment.x >= width ||
            fragment.y < 0 || fragment.y >= height) {
            return false;
        }
        order.push_back({static_cast<std::size_t>(fragment.y) * w +
                             static_cast<std::size_t>(fragment.x),
                         index});
    }
    std::stable_sort(order.begin(), order.end(),
                     [&fragments](const OrderedFragment &left,
                                  const OrderedFragment &right) {
        if (left.pixelIndex != right.pixelIndex)
            return left.pixelIndex < right.pixelIndex;
        return fragments[left.inputIndex].cameraDepth <
               fragments[right.inputIndex].cameraDepth;
    });

    std::vector<RasterWorkbenchLinearPixel> resolved(pixelCount, base);
    for (const OrderedFragment &entry : order) {
        const RasterWorkbenchLinearFragment &fragment =
            fragments[entry.inputIndex];
        RasterWorkbenchLinearPixel &destination = resolved[entry.pixelIndex];
        const float remain = 1.0f - fragment.alpha;
        destination.red = fragment.sceneLinearRgb[0] * fragment.alpha +
                          destination.red * remain;
        destination.green = fragment.sceneLinearRgb[1] * fragment.alpha +
                            destination.green * remain;
        destination.blue = fragment.sceneLinearRgb[2] * fragment.alpha +
                           destination.blue * remain;
        destination.alpha = fragment.alpha + destination.alpha * remain;
    }

    *output = std::move(resolved);
    return true;
}

float rasterWorkbenchTransparentWeight(float cameraDepth) noexcept
{
    if (!std::isfinite(cameraDepth)) return 0.0f;
    const float z = std::abs(cameraDepth);
    const float a = z / 5.0f;
    float b = z / 200.0f;
    b *= b;
    const float weight = 10.0f /
        ((1.0e-5f + a * a) + b * (b * b));
    return std::clamp(weight, 1.0e-2f, 3.0e2f);
}

bool resolveRasterWorkbenchWeightedOitFragments(
    int width, int height, const RasterWorkbenchLinearPixel &base,
    const std::vector<RasterWorkbenchLinearFragment> &fragments,
    std::vector<RasterWorkbenchLinearPixel> *output)
{
    if (width <= 0 || height <= 0 || !finite(base)) return false;
    const std::size_t w = static_cast<std::size_t>(width);
    const std::size_t h = static_cast<std::size_t>(height);
    if (w > std::numeric_limits<std::size_t>::max() / h) return false;
    return resolveRasterWorkbenchWeightedOitFragments(
        width, height, std::vector<RasterWorkbenchLinearPixel>(w * h, base),
        fragments, output);
}

bool resolveRasterWorkbenchWeightedOitFragments(
    int width, int height,
    const std::vector<RasterWorkbenchLinearPixel> &basePixels,
    const std::vector<RasterWorkbenchLinearFragment> &fragments,
    std::vector<RasterWorkbenchLinearPixel> *output)
{
    if (output == nullptr || width <= 0 || height <= 0) return false;
    const std::size_t w = static_cast<std::size_t>(width);
    const std::size_t h = static_cast<std::size_t>(height);
    if (w > std::numeric_limits<std::size_t>::max() / h) return false;
    const std::size_t pixelCount = w * h;
    if (basePixels.size() != pixelCount ||
        !std::all_of(basePixels.begin(), basePixels.end(),
                     [](const RasterWorkbenchLinearPixel &pixel) {
                         return finite(pixel);
                     })) {
        return false;
    }

    struct Accumulation {
        float red = 0.0f;
        float green = 0.0f;
        float blue = 0.0f;
        float weight = 0.0f;
        float revealage = 1.0f;
    };
    std::vector<Accumulation> accumulations(pixelCount);
    for (const RasterWorkbenchLinearFragment &fragment : fragments) {
        if (!finite(fragment) || fragment.x < 0 || fragment.x >= width ||
            fragment.y < 0 || fragment.y >= height) {
            return false;
        }
        const std::size_t index = static_cast<std::size_t>(fragment.y) * w +
                                  static_cast<std::size_t>(fragment.x);
        const float weightedAlpha = fragment.alpha *
            rasterWorkbenchTransparentWeight(
                fragment.oitLinearDepth >= 0.0f
                    ? fragment.oitLinearDepth
                    : fragment.cameraDepth);
        Accumulation &accumulation = accumulations[index];
        accumulation.red += fragment.sceneLinearRgb[0] * weightedAlpha;
        accumulation.green += fragment.sceneLinearRgb[1] * weightedAlpha;
        accumulation.blue += fragment.sceneLinearRgb[2] * weightedAlpha;
        accumulation.weight += weightedAlpha;
        accumulation.revealage *= 1.0f - fragment.alpha;
    }

    std::vector<RasterWorkbenchLinearPixel> resolved = basePixels;
    for (std::size_t index = 0; index < pixelCount; ++index) {
        const Accumulation &accumulation = accumulations[index];
        if (accumulation.weight <= 0.0f) continue;
        const float alpha = 1.0f - accumulation.revealage;
        const float transmittance = 1.0f - alpha;
        const float weight = std::clamp(accumulation.weight, 1.0e-4f, 5.0e4f);
        const float sourceRed = accumulation.red / weight;
        const float sourceGreen = accumulation.green / weight;
        const float sourceBlue = accumulation.blue / weight;
        RasterWorkbenchLinearPixel &pixel = resolved[index];
        pixel.red = sourceRed * alpha + pixel.red * transmittance;
        pixel.green = sourceGreen * alpha + pixel.green * transmittance;
        pixel.blue = sourceBlue * alpha + pixel.blue * transmittance;
        pixel.alpha = alpha + pixel.alpha * transmittance;
    }

    *output = std::move(resolved);
    return true;
}

} // namespace classiCAD
