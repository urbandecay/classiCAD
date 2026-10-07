/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#version 330 core

layout(location = 0) in vec3 aWorldPosition;
layout(location = 1) in vec4 aPickColor;
uniform mat4 uViewProjection;
uniform float uPointSize;
flat out vec4 vPickColor;

void main()
{
    gl_Position = uViewProjection * vec4(aWorldPosition, 1.0);
    gl_PointSize = uPointSize;
    vPickColor = aPickColor;
}
