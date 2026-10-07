/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#version 330 core

layout(location = 0) in vec3 aWorldPosition;
layout(location = 1) in float aPatternOffsetPixels;
layout(location = 2) in vec4 aEndpointColor;
uniform mat4 uViewProjection;
uniform vec3 uWorldOffset;
out float vPatternOffsetPixels;
out vec4 vEndpointColor;

void main()
{
    gl_Position = uViewProjection * vec4(aWorldPosition + uWorldOffset, 1.0);
    vPatternOffsetPixels = aPatternOffsetPixels;
    vEndpointColor = aEndpointColor;
}
