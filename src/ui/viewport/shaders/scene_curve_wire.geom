/* SPDX-FileCopyrightText: 2019-2023 Blender Authors
 * SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#version 330 core

layout(lines) in;
layout(line_strip, max_vertices = 2) out;

uniform vec2 uViewportSize;
uniform float uDepthBias;

smooth out vec4 gEdgeColor;
flat out float vLength;
flat out float gPatternOffsetPixels;
flat out vec4 gStartColor;
flat out vec4 gEndColor;
flat out vec2 gStrokeDirection;
flat out vec2 gLineStartPixels;
noperspective out vec2 vStrokePosition;
in float vPatternOffsetPixels[];
in vec4 vEndpointColor[];

void main()
{
    vec4 start = gl_in[0].gl_Position;
    vec4 end = gl_in[1].gl_Position;
    vec4 startColor = vEndpointColor[0];
    vec4 endColor = vEndpointColor[1];
    float nearStart = start.z + start.w;
    float nearEnd = end.z + end.w;
    if (nearStart < 0.0 && nearEnd < 0.0) {
        return;
    }

    float patternOffset = vPatternOffsetPixels[0];
    if (nearStart < 0.0) {
        vec4 originalStart = start;
        float clipT = nearStart / (nearStart - nearEnd);
        start = mix(start, end, clipT);
        startColor = mix(startColor, endColor, clipT);
        if (originalStart.w > 0.0 && start.w > 0.0) {
            patternOffset += length(
                (start.xy / start.w - originalStart.xy / originalStart.w) *
                (0.5 * uViewportSize));
        }
    } else if (nearEnd < 0.0) {
        float clipT = nearEnd / (nearEnd - nearStart);
        end = mix(end, start, clipT);
        endColor = mix(endColor, startColor, clipT);
    }
    if (start.w <= 0.0 || end.w <= 0.0) {
        return;
    }

    vec2 screenStart = (start.xy / start.w * 0.5 + 0.5) * uViewportSize;
    vec2 screenEnd = (end.xy / end.w * 0.5 + 0.5) * uViewportSize;
    vec2 screenDelta = screenEnd - screenStart;
    float lengthPixels = length(screenDelta);
    if (lengthPixels < 0.001) {
        return;
    }

    // Blender's pack_line_data uses edge_start - edge_pos, then rotates that
    // vector clockwise to get the signed perpendicular direction.
    vec2 tangent = screenDelta / lengthPixels;
    gStrokeDirection = vec2(tangent.y, -tangent.x);
    gLineStartPixels = screenStart;
    vLength = lengthPixels;
    gPatternOffsetPixels = patternOffset;
    gStartColor = startColor;
    gEndColor = endColor;

    gEdgeColor = startColor;
    vStrokePosition = vec2(0.0, 0.0);
    gl_Position = start;
    gl_Position.z -= uDepthBias * gl_Position.w;
    EmitVertex();

    gEdgeColor = endColor;
    vStrokePosition = vec2(lengthPixels, 0.0);
    gl_Position = end;
    gl_Position.z -= uDepthBias * gl_Position.w;
    EmitVertex();
    EndPrimitive();
}
