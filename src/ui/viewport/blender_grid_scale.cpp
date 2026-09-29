/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "blender_grid_scale.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace classiCAD {
namespace {

constexpr int kGridStepCount = 8;
constexpr qreal kAxisOrthoSubdivisionFactor = 1000.0;

std::array<qreal, kGridStepCount> gridSteps(bool fixedAxisOrthographic,
                                            qreal baseGridStep)
{
    std::array<qreal, kGridStepCount> steps{};
    qreal step = fixedAxisOrthographic
                     ? baseGridStep / kAxisOrthoSubdivisionFactor
                     : baseGridStep;
    for (qreal &value : steps) {
        value = step;
        step *= 10.0;
    }
    return steps;
}

} // namespace

BlenderGridLevelSelection selectBlenderGridLevel(
    qreal focusDistance,
    bool fixedAxisOrthographic,
    qreal baseGridStep)
{
    if (!std::isfinite(baseGridStep) || baseGridStep <= 0.0) {
        baseGridStep = 1.0;
    }
    const std::array<qreal, kGridStepCount> steps =
        gridSteps(fixedAxisOrthographic, baseGridStep);
    if (!std::isfinite(focusDistance)) {
        focusDistance = steps.front();
    }
    focusDistance = std::max<qreal>(focusDistance, 0.0);

    for (int index = 0; index < kGridStepCount; ++index) {
        const qreal current = steps[static_cast<std::size_t>(index)];
        const qreal next = current * 10.0;
        if (next >= focusDistance || index == kGridStepCount - 1) {
            return {current,
                    (focusDistance - current) / (next - current),
                    index,
                    kGridStepCount};
        }
    }

    return {steps.back(), 0.0, kGridStepCount - 1, kGridStepCount};
}

qreal blenderGridStepAtLevel(const BlenderGridLevelSelection &selection,
                             int lineLevel)
{
    if (!std::isfinite(selection.baseStep) || selection.baseStep <= 0.0 ||
        selection.stepCount <= 0) {
        return 1.0;
    }
    const int selectedIndex = std::clamp(selection.baseStepIndex,
                                         0,
                                         selection.stepCount - 1);
    const int levelIndex = std::clamp(selectedIndex + lineLevel - 1,
                                      0,
                                      selection.stepCount - 1);
    return selection.baseStep *
           std::pow(10.0, static_cast<qreal>(levelIndex - selectedIndex));
}

bool isBlenderAxisAlignedView(ViewportViewPreset preset)
{
    switch (preset) {
    case ViewportViewPreset::Top:
    case ViewportViewPreset::Bottom:
    case ViewportViewPreset::Front:
    case ViewportViewPreset::Back:
    case ViewportViewPreset::Right:
    case ViewportViewPreset::Left:
        return true;
    case ViewportViewPreset::Isometric:
    case ViewportViewPreset::Perspective:
    case ViewportViewPreset::Custom:
        return false;
    }
    return false;
}

} // namespace classiCAD
