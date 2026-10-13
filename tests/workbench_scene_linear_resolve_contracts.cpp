/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "ui/viewport/viewport_raster_workbench_linear.h"
#include "ui/viewport/workbench_lighting.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace classiCAD;

namespace {

bool near(float actual, float expected, float tolerance = 1.0e-6f)
{
    return std::abs(actual - expected) <= tolerance;
}

bool check(bool condition, const char *message)
{
    if (!condition) std::fprintf(stderr, "%s\n", message);
    return condition;
}

} // namespace

int main()
{
    bool passed = true;

    const RasterWorkbenchLinearPixel background =
        RasterWorkbenchLinearPixel::fromStraight(0.03f, 0.08f, 0.16f, 1.0f);
    // Input is deliberately near-first: the resolver must sort by camera depth.
    const std::vector<RasterWorkbenchLinearFragment> fragments{
        {0, 0, 8.0f, {0.8f, 0.15f, 0.3f}, 0.25f},
        {0, 0, 2.0f, {0.2f, 0.4f, 4.0f}, 0.5f},
    };
    std::vector<RasterWorkbenchLinearPixel> resolved;
    passed &= check(resolveRasterWorkbenchLinearFragments(
                        1, 1, background, fragments, &resolved),
                    "linear fragment resolver must accept valid input");
    if (resolved.size() != 1) return 1;

    const float expectedRed = 0.25f * 0.8f + 0.75f *
        (0.5f * 0.2f + 0.5f * 0.03f);
    const float expectedGreen = 0.25f * 0.15f + 0.75f *
        (0.5f * 0.4f + 0.5f * 0.08f);
    const float expectedBlue = 0.25f * 0.3f + 0.75f *
        (0.5f * 4.0f + 0.5f * 0.16f);
    passed &= check(near(resolved[0].red, expectedRed) &&
                        near(resolved[0].green, expectedGreen) &&
                        near(resolved[0].blue, expectedBlue) &&
                        near(resolved[0].alpha, 1.0f),
                    "opaque-base resolve must sort far-to-near and match source-over");
    passed &= check(resolved[0].blue > 1.0f,
                    "scene-linear resolve must retain HDR values above one");
    const RasterWorkbenchLinearPixel opaqueResolved = resolved[0];

    const RasterWorkbenchLinearPixel transparentBase{};
    const std::vector<RasterWorkbenchLinearFragment> transparentFragments{
        {0, 0, 1.0f, {0.2f, 0.4f, 4.0f}, 0.5f},
        {0, 0, 3.0f, {0.8f, 0.15f, 0.3f}, 0.25f},
    };
    passed &= check(resolveRasterWorkbenchLinearFragments(
                        1, 1, transparentBase, transparentFragments,
                        &resolved),
                    "transparent-film resolve must accept valid input");
    const float transparentAlpha = 0.25f + 0.5f * (1.0f - 0.25f);
    passed &= check(near(resolved[0].alpha, transparentAlpha) &&
                        near(resolved[0].red,
                             (0.25f * 0.8f + 0.75f * 0.5f * 0.2f)) &&
                        near(resolved[0].blue,
                             (0.25f * 0.3f + 0.75f * 0.5f * 4.0f)),
                    "transparent-film output must preserve premultiplied linear RGB and alpha");

    const QVector3D straightLinear(opaqueResolved.red / opaqueResolved.alpha,
                                   opaqueResolved.green / opaqueResolved.alpha,
                                   opaqueResolved.blue / opaqueResolved.alpha);
    const QVector3D displayAfterComposite =
        workbenchSceneLinearToDisplaySrgb(straightLinear, 1);
    const QVector3D farDisplay = workbenchSceneLinearToDisplaySrgb(
        QVector3D(0.2f, 0.4f, 4.0f), 1);
    const QVector3D nearDisplay = workbenchSceneLinearToDisplaySrgb(
        QVector3D(0.8f, 0.15f, 0.3f), 1);
    const QVector3D backgroundDisplay = workbenchSceneLinearToDisplaySrgb(
        QVector3D(0.03f, 0.08f, 0.16f), 1);
    const QVector3D displayBeforeComposite =
        nearDisplay * 0.25f +
        (farDisplay * 0.5f + backgroundDisplay * 0.5f) * 0.75f;
    passed &= check(std::isfinite(displayAfterComposite.x()) &&
                        std::abs(displayAfterComposite.x() -
                                 displayBeforeComposite.x()) > 0.01f,
                    "display transform must follow linear compositing, not transform each fragment first");

    std::vector<RasterWorkbenchLinearPixel> unchanged{{9.0f, 8.0f, 7.0f, 1.0f}};
    const std::vector<RasterWorkbenchLinearFragment> invalid{
        {1, 0, 1.0f, {1.0f, 1.0f, 1.0f}, 0.5f},
    };
    passed &= check(!resolveRasterWorkbenchLinearFragments(
                        1, 1, background, invalid, &unchanged) &&
                        unchanged.size() == 1 && unchanged[0].red == 9.0f,
                    "invalid fragments must fail without modifying output");

    return passed ? 0 : 1;
}
