/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#version 330 core

uniform vec4 uColor;
uniform float uPointSize;
uniform bool uPointOutline;
out vec4 fragmentColor;

void main()
{
    float distanceFromCenter = length((gl_PointCoord - vec2(0.5)) * uPointSize);
    float coverage = clamp(uPointSize * 0.5 + 0.5 - distanceFromCenter,
                           0.0, 1.0);
    if (coverage <= 0.0) {
        discard;
    }
    if (uPointOutline && distanceFromCenter < uPointSize * 0.34) {
        discard;
    }
    fragmentColor = vec4(uColor.rgb, uColor.a * coverage);
}
