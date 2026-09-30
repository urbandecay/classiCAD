/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#version 330 core

uniform vec3 uBackgroundColor;

out vec4 fragmentColor;

void main()
{
    fragmentColor = vec4(uBackgroundColor, 1.0);
}
