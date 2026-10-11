/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <QtGlobal>

#include <algorithm>
#include <cmath>

namespace classiCAD {

inline qreal viewportLineWeightPixels(qreal lineWeightMm) noexcept
{
    if (!std::isfinite(lineWeightMm) || lineWeightMm <= 0.0) {
        return 1.0;
    }
    return std::clamp(lineWeightMm * 6.0, 1.0, 16.0);
}

} // namespace classiCAD
