/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#version 330 core

uniform vec4 uColor;
uniform float uWidth;
uniform int uLineStyle;
uniform float uPatternPeriod;
uniform float uPatternOnLength;
flat in float vLength;
noperspective in vec2 vStrokePosition;
out vec4 fragmentColor;

void main()
{
    float coverage;
    if (uLineStyle == 2) {
        float dotCenter = floor(max(vStrokePosition.x, 0.0) /
                                    uPatternPeriod + 0.5) * uPatternPeriod;
        if (dotCenter > vLength) {
            dotCenter = floor(vLength / uPatternPeriod) * uPatternPeriod;
        }
        float distanceFromDot = length(vec2(vStrokePosition.x - dotCenter,
                                            vStrokePosition.y));
        coverage = clamp(uPatternOnLength * 0.5 + 0.5 - distanceFromDot,
                         0.0, 1.0);
    } else {
        float beyond = max(max(-vStrokePosition.x,
                               vStrokePosition.x - vLength), 0.0);
        float distanceFromStroke = length(vec2(beyond, vStrokePosition.y));
        coverage = clamp(uWidth * 0.5 + 0.5 - distanceFromStroke,
                         0.0, 1.0);
        if (uLineStyle == 1 && uPatternPeriod > 0.0 &&
            mod(max(vStrokePosition.x, 0.0), uPatternPeriod) >=
                uPatternOnLength) {
            discard;
        }
    }
    if (coverage <= 0.0) {
        discard;
    }
    fragmentColor = vec4(uColor.rgb, uColor.a * coverage);
}
