/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

namespace classiCAD {

// Stored values intentionally match Blender 5.2.2's preference values. Its
// visible label is "Single Pass Anti-Aliasing" and its RNA description says
// FXAA, but Workbench handles value 1 as one unjittered TAA sample followed by
// SMAA. Multisample modes store their requested sample count.
enum class ViewportAntiAliasingMode : int {
    Off = 0,
    SinglePass = 1,
    Samples5 = 5,
    Samples8 = 8,
    Samples11 = 11,
    Samples16 = 16,
    Samples32 = 32,
};

constexpr bool isViewportAntiAliasingMode(int value) noexcept
{
    return value == static_cast<int>(ViewportAntiAliasingMode::Off) ||
           value == static_cast<int>(ViewportAntiAliasingMode::SinglePass) ||
           value == static_cast<int>(ViewportAntiAliasingMode::Samples5) ||
           value == static_cast<int>(ViewportAntiAliasingMode::Samples8) ||
           value == static_cast<int>(ViewportAntiAliasingMode::Samples11) ||
           value == static_cast<int>(ViewportAntiAliasingMode::Samples16) ||
           value == static_cast<int>(ViewportAntiAliasingMode::Samples32);
}

constexpr int normalizedViewportAntiAliasingMode(int value) noexcept
{
    return isViewportAntiAliasingMode(value)
               ? value
               : static_cast<int>(ViewportAntiAliasingMode::Samples8);
}

} // namespace classiCAD
