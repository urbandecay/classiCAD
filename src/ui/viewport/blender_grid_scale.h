#pragma once

#include "services/viewport/viewport_transform.h"

namespace classiCAD {

struct BlenderGridLevelSelection {
    qreal baseStep = 1.0;
    qreal levelFraction = 0.0;
    int baseStepIndex = 0;
    int stepCount = 8;
};

// Matches Blender's 8-step decimal grid ladder. Fixed-axis orthographic views
// add three subdivisions below the document's base grid spacing.
BlenderGridLevelSelection selectBlenderGridLevel(
    qreal focusDistance,
    bool fixedAxisOrthographic,
    qreal baseGridStep = 1.0);

qreal blenderGridStepAtLevel(const BlenderGridLevelSelection &selection,
                             int lineLevel);

// Fades orthographic grid lines below four pixels so close subdivisions remain visible.
qreal blenderOrthographicGridPixelFade(qreal projectedStepPixels);

bool isBlenderAxisAlignedView(ViewportViewPreset preset);

} // namespace classiCAD
