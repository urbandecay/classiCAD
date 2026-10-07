/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#version 330 core

layout(lines) in;
layout(triangle_strip, max_vertices = 4) out;

uniform vec2 uViewportSize;
uniform float uWidth;
uniform float uDepthBias;
uniform int uEditModeWire;
uniform int uSmoothWire;
smooth out vec4 gEdgeColor;
flat out float vLength;
flat out float gPatternOffsetPixels;
flat out vec4 gStartColor;
flat out vec4 gEndColor;
noperspective out vec2 vStrokePosition;
in float vPatternOffsetPixels[];
in vec4 vEndpointColor[];

void main()
{
    vec4 a = gl_in[0].gl_Position;
    vec4 b = gl_in[1].gl_Position;
    vec4 colorA = vEndpointColor[0];
    vec4 colorB = vEndpointColor[1];
    float nearA = a.z + a.w;
    float nearB = b.z + b.w;
    if (nearA < 0.0 && nearB < 0.0) {
        return;
    }
    float patternOffset = vPatternOffsetPixels[0];
    if (nearA < 0.0) {
        vec4 originalA = a;
        float clipT = nearA / (nearA - nearB);
        a = mix(a, b, clipT);
        colorA = mix(colorA, colorB, clipT);
        if (originalA.w > 0.0 && a.w > 0.0) {
            patternOffset += length((a.xy / a.w - originalA.xy / originalA.w) *
                                    (0.5 * uViewportSize));
        }
    } else if (nearB < 0.0) {
        float clipT = nearB / (nearB - nearA);
        b = mix(b, a, clipT);
        colorB = mix(colorB, colorA, clipT);
    }
    if (a.w <= 0.0 || b.w <= 0.0) {
        return;
    }
    vec2 screenDelta = (b.xy / b.w - a.xy / a.w) * (0.5 * uViewportSize);
    float lengthPixels = length(screenDelta);
    if (lengthPixels < 0.001) {
        return;
    }
    vec2 tangent = screenDelta / lengthPixels;
    vec2 normal = vec2(-tangent.y, tangent.x);
    if (uEditModeWire != 0) {
        // Blender expands along the minor screen axis, with no end caps.
        float halfSize = uWidth * 0.5 + (uSmoothWire != 0 ? 0.5 : 0.0);
        vec2 offset = abs(screenDelta.x) > abs(screenDelta.y)
                          ? vec2(0.0, halfSize) : vec2(halfSize, 0.0);
        for (int endpoint = 0; endpoint < 2; ++endpoint) {
            for (int side = 0; side < 2; ++side) {
                float sign = side == 0 ? 1.0 : -1.0;
                vec4 position = endpoint == 0 ? a : b;
                vLength = lengthPixels;
                gPatternOffsetPixels = patternOffset;
                gStartColor = colorA;
                gEndColor = colorB;
                gEdgeColor = endpoint == 0 ? colorA : colorB;
                vStrokePosition = vec2(float(endpoint) * lengthPixels,
                                       sign * halfSize);
                gl_Position = position + vec4(sign * offset * 2.0 /
                                                uViewportSize * position.w, 0.0, 0.0);
                gl_Position.z -= uDepthBias * gl_Position.w;
                EmitVertex();
            }
        }
        EndPrimitive();
        return;
    }
    float radius = uWidth * 0.5 + 1.0;
    vLength = lengthPixels;
    gPatternOffsetPixels = patternOffset;
    gStartColor = colorA;
    gEndColor = colorB;
    vStrokePosition = vec2(-radius, radius);
    gl_Position = a + vec4((normal - tangent) * radius * 2.0 /
                                uViewportSize * a.w, 0.0, 0.0);
    gl_Position.z -= uDepthBias * gl_Position.w;
    EmitVertex();
    vLength = lengthPixels;
    gPatternOffsetPixels = patternOffset;
    gStartColor = colorA;
    gEndColor = colorB;
    vStrokePosition = vec2(-radius, -radius);
    gl_Position = a + vec4((-normal - tangent) * radius * 2.0 /
                                uViewportSize * a.w, 0.0, 0.0);
    gl_Position.z -= uDepthBias * gl_Position.w;
    EmitVertex();
    vLength = lengthPixels;
    gPatternOffsetPixels = patternOffset;
    gStartColor = colorA;
    gEndColor = colorB;
    vStrokePosition = vec2(lengthPixels + radius, radius);
    gl_Position = b + vec4((normal + tangent) * radius * 2.0 /
                                uViewportSize * b.w, 0.0, 0.0);
    gl_Position.z -= uDepthBias * gl_Position.w;
    EmitVertex();
    vLength = lengthPixels;
    gPatternOffsetPixels = patternOffset;
    gStartColor = colorA;
    gEndColor = colorB;
    vStrokePosition = vec2(lengthPixels + radius, -radius);
    gl_Position = b + vec4((-normal + tangent) * radius * 2.0 /
                                uViewportSize * b.w, 0.0, 0.0);
    gl_Position.z -= uDepthBias * gl_Position.w;
    EmitVertex();
    EndPrimitive();
}
