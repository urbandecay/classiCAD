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
uniform int uSmoothWire;
uniform int uEditModeWire;
uniform int uLinearDisplayBlend;
smooth in vec4 gEdgeColor;
flat in float vLength;
flat in float gPatternOffsetPixels;
flat in vec4 gStartColor;
flat in vec4 gEndColor;
noperspective in vec2 vStrokePosition;
out vec4 fragmentColor;

vec3 srgbToLinear(vec3 color)
{
    vec3 low = color / 12.92;
    vec3 high = pow((color + 0.055) / 1.055, vec3(2.4));
    return mix(high, low, lessThanEqual(color, vec3(0.04045)));
}

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
        if (uSmoothWire != 0) {
            // Match Blender's edit-wire coverage: estimate pixel coverage with
            // a smoothstep over a circular pixel footprint instead of a linear
            // one-pixel ramp. uWidth is the full stroke width here, so use its
            // half-width for Blender's edge-size calculation.
            const float inverseSqrtPi = 0.5641895835477563;
            const float discRadius = inverseSqrtPi * 1.05;
            const float smoothStart = 0.5 - discRadius;
            const float smoothEnd = 0.5 + discRadius;
            float edgeSize = uWidth * 0.5;
            float edgeDistance = distanceFromStroke - max(edgeSize - 0.5, 0.0);
            coverage = 1.0 - smoothstep(smoothStart, smoothEnd, edgeDistance);
        } else {
            coverage = clamp(uWidth * 0.5 + 0.5 - distanceFromStroke,
                             0.0, 1.0);
        }
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
    if (uSmoothWire == 0) {
        coverage = coverage >= 0.5 ? 1.0 : 0.0;
        if (coverage <= 0.0) {
            discard;
        }
    }
    float edgeParameter = clamp(vStrokePosition.x / max(vLength, 0.001),
                                0.0, 1.0);
    vec4 edgeColor = uEditModeWire != 0 ? gEdgeColor
                                       : mix(gStartColor, gEndColor, edgeParameter);
    vec3 strokeColor = uLinearDisplayBlend != 0
                           ? srgbToLinear(uColor.rgb)
                           : uColor.rgb;
    fragmentColor = vec4(strokeColor * edgeColor.rgb,
                         uColor.a * edgeColor.a * coverage);
}
