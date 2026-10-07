/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#version 330 core

layout(location = 0) in vec3 aWorldPosition;
layout(location = 1) in float aPatternOffsetPixels;
layout(location = 2) in vec4 aEndpointColor;
uniform mat4 uViewProjection;
uniform vec3 uWorldOffset;
uniform int uLinearDisplayBlend;
out float vPatternOffsetPixels;
out vec4 vEndpointColor;

vec3 srgbToLinear(vec3 color)
{
    vec3 low = color / 12.92;
    vec3 high = pow((color + 0.055) / 1.055, vec3(2.4));
    return mix(high, low, lessThanEqual(color, vec3(0.04045)));
}

void main()
{
    gl_Position = uViewProjection * vec4(aWorldPosition + uWorldOffset, 1.0);
    vPatternOffsetPixels = aPatternOffsetPixels;
    vEndpointColor = aEndpointColor;
    if (uLinearDisplayBlend != 0) {
        vEndpointColor.rgb = srgbToLinear(vEndpointColor.rgb);
    }
}
