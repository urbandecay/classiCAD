/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#version 330 core

uniform vec4 uColor;
uniform float uWidth;
uniform bool uDashed;
flat in float vLength;
noperspective in vec2 vStrokePosition;
out vec4 fragmentColor;

void main()
{
    if (uDashed && mod(max(vStrokePosition.x, 0.0), 6.0) >= 4.0) {
        discard;
    }
    float beyond = max(max(-vStrokePosition.x,
                           vStrokePosition.x - vLength), 0.0);
    float distanceFromStroke = length(vec2(beyond, vStrokePosition.y));
    float coverage = clamp(uWidth * 0.5 + 0.5 - distanceFromStroke,
                           0.0, 1.0);
    if (coverage <= 0.0) {
        discard;
    }
    fragmentColor = vec4(uColor.rgb, uColor.a * coverage);
}
