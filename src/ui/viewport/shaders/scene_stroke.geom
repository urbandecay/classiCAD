/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#version 330 core

layout(lines) in;
layout(triangle_strip, max_vertices = 4) out;

uniform vec2 uViewportSize;
uniform float uWidth;
flat out float vLength;
flat out float gPatternOffsetPixels;
noperspective out vec2 vStrokePosition;
in float vPatternOffsetPixels[];

void main()
{
    vec4 a = gl_in[0].gl_Position;
    vec4 b = gl_in[1].gl_Position;
    float nearA = a.z + a.w;
    float nearB = b.z + b.w;
    if (nearA < 0.0 && nearB < 0.0) {
        return;
    }
    float patternOffset = vPatternOffsetPixels[0];
    if (nearA < 0.0) {
        vec4 originalA = a;
        a = mix(a, b, nearA / (nearA - nearB));
        if (originalA.w > 0.0 && a.w > 0.0) {
            patternOffset += length((a.xy / a.w - originalA.xy / originalA.w) *
                                    (0.5 * uViewportSize));
        }
    } else if (nearB < 0.0) {
        b = mix(b, a, nearB / (nearB - nearA));
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
    float radius = uWidth * 0.5 + 1.0;
    vLength = lengthPixels;
    gPatternOffsetPixels = patternOffset;
    vStrokePosition = vec2(-radius, radius);
    gl_Position = a + vec4((normal - tangent) * radius * 2.0 /
                                uViewportSize * a.w, 0.0, 0.0);
    EmitVertex();
    vStrokePosition = vec2(-radius, -radius);
    gl_Position = a + vec4((-normal - tangent) * radius * 2.0 /
                                uViewportSize * a.w, 0.0, 0.0);
    EmitVertex();
    vStrokePosition = vec2(lengthPixels + radius, radius);
    gl_Position = b + vec4((normal + tangent) * radius * 2.0 /
                                uViewportSize * b.w, 0.0, 0.0);
    EmitVertex();
    vStrokePosition = vec2(lengthPixels + radius, -radius);
    gl_Position = b + vec4((-normal + tangent) * radius * 2.0 /
                                uViewportSize * b.w, 0.0, 0.0);
    EmitVertex();
    EndPrimitive();
}
