/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#version 330 core

uniform sampler2D uPicture;
uniform float uOpacity;
in vec2 vTextureCoordinate;
out vec4 fragmentColor;

void main()
{
    vec4 color = texture(uPicture, vTextureCoordinate);
    fragmentColor = vec4(color.rgb, color.a * uOpacity);
}
