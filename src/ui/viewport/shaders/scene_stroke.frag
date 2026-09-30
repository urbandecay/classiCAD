/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#version 330 core

uniform vec4 uColor;
uniform float uWidth;
uniform int uLineStyle;
uniform float uPatternPeriod;
uniform float uPatternOnLength;
uniform int uPatternSegmentCount;
uniform float uPatternSegments[8];
flat in float vLength;
flat in float gPatternOffsetPixels;
noperspective in vec2 vStrokePosition;
out vec4 fragmentColor;

void main()
{
    float coverage;
    if (uLineStyle == 2) {
        float patternPosition = vStrokePosition.x + gPatternOffsetPixels;
        float dotCenter = floor(max(patternPosition, 0.0) /
                                    uPatternPeriod + 0.5) * uPatternPeriod;
        if (dotCenter > gPatternOffsetPixels + vLength) {
            dotCenter = floor((gPatternOffsetPixels + vLength) /
                              uPatternPeriod) * uPatternPeriod;
        }
        float distanceFromDot = length(vec2(patternPosition - dotCenter,
                                            vStrokePosition.y));
        coverage = clamp(uPatternOnLength * 0.5 + 0.5 - distanceFromDot,
                         0.0, 1.0);
    } else if (uLineStyle == 3) {
        float patternPosition = vStrokePosition.x + gPatternOffsetPixels;
        float baseCycle = floor(patternPosition / uPatternPeriod);
        float radius = uWidth * 0.5;
        float nearestPatternDistance = 1.0e20;
        for (int cycleOffset = -1; cycleOffset <= 1; ++cycleOffset) {
            float segmentStart =
                (baseCycle + float(cycleOffset)) * uPatternPeriod;
            for (int segmentIndex = 0; segmentIndex < 8; ++segmentIndex) {
                if (segmentIndex >= uPatternSegmentCount) {
                    break;
                }
                float segmentEnd = segmentStart + uPatternSegments[segmentIndex];
                if ((segmentIndex % 2) == 0) {
                    float segmentLength = segmentEnd - segmentStart;
                    float nearestX = segmentLength >= uWidth
                                         ? clamp(patternPosition,
                                                 segmentStart + radius,
                                                 segmentEnd - radius)
                                         : (segmentStart + segmentEnd) * 0.5;
                    float distanceToOnSegment = length(vec2(
                        patternPosition - nearestX, vStrokePosition.y));
                    nearestPatternDistance = min(nearestPatternDistance,
                                                 distanceToOnSegment);
                }
                segmentStart = segmentEnd;
            }
        }
        coverage = clamp(radius + 0.5 - nearestPatternDistance, 0.0, 1.0);
    } else {
        float beyond = max(max(-vStrokePosition.x,
                               vStrokePosition.x - vLength), 0.0);
        float distanceFromStroke = length(vec2(beyond, vStrokePosition.y));
        coverage = clamp(uWidth * 0.5 + 0.5 - distanceFromStroke,
                         0.0, 1.0);
        if (uLineStyle == 1 && uPatternPeriod > 0.0 &&
            mod(max(vStrokePosition.x + gPatternOffsetPixels, 0.0),
                uPatternPeriod) >=
                uPatternOnLength) {
            discard;
        }
    }
    if (coverage <= 0.0) {
        discard;
    }
    fragmentColor = vec4(uColor.rgb, uColor.a * coverage);
}
