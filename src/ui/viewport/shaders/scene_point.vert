/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#version 330 core

layout(location = 0) in vec3 aWorldPosition;
layout(location = 4) in float aWorldRadius;
uniform mat4 uViewProjection;
uniform vec3 uWorldOffset;
uniform float uPointSize;
uniform float uDepthBias;
uniform float uViewportHeight;
uniform bool uUseWorldPointRadius;
out float vPointSize;

void main()
{
    vec4 clipPosition = uViewProjection * vec4(aWorldPosition + uWorldOffset, 1.0);
    clipPosition.z -= uDepthBias * clipPosition.w;
    gl_Position = clipPosition;
    float radiusPixels = aWorldRadius * 0.5 * uViewportHeight *
                         abs(uViewProjection[1][1]) /
                         max(abs(clipPosition.w), 1.0e-8);
    vPointSize = uUseWorldPointRadius
                     ? clamp(2.0 * radiusPixels, 1.0, 4096.0)
                     : uPointSize;
    gl_PointSize = vPointSize;
}
