/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#version 330 core

layout(location = 0) in vec3 aWorldPosition;
uniform mat4 uViewProjection;
uniform vec3 uWorldOffset;
uniform float uPointSize;
uniform float uDepthBias;

void main()
{
    gl_Position = uViewProjection * vec4(aWorldPosition + uWorldOffset, 1.0);
    gl_Position.z -= uDepthBias * gl_Position.w;
    gl_PointSize = uPointSize;
}
