/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#version 330 core

layout(location = 0) in vec3 aWorldPosition;
layout(location = 1) in vec2 aTextureCoordinate;
uniform mat4 uViewProjection;
out vec2 vTextureCoordinate;

void main()
{
    gl_Position = uViewProjection * vec4(aWorldPosition, 1.0);
    vTextureCoordinate = aTextureCoordinate;
}
