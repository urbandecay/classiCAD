/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#version 330 core

layout(location = 0) in vec3 aWorldPosition;
layout(location = 1) in vec4 aPickColor;
uniform mat4 uViewProjection;
uniform float uPointSize;
uniform float uDepthBias;
flat out vec4 vPickColor;

void main()
{
    gl_Position = uViewProjection * vec4(aWorldPosition, 1.0);
    // Match visible edit wires so an edge on its own face is pickable.
    gl_Position.z -= uDepthBias * gl_Position.w;
    gl_PointSize = uPointSize;
    vPickColor = aPickColor;
}
