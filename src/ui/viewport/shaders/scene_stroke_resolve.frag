/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#version 330 core

uniform sampler2D uBaseColor;
uniform sampler2D uLineColor;
uniform sampler2D uLineRecord;
uniform sampler2D uCandidateDepth;
uniform sampler2D uSceneDepth;
uniform ivec2 uViewportSize;

layout(location = 0) out vec4 fragmentColor;

const float kDiscRadius = 0.5641895835477563 * 1.05;
const float kLineSmoothStart = 0.5 - kDiscRadius;
const float kLineSmoothEnd = 0.5 + kDiscRadius;
const float kNoLineDistance = 1.0e10;

bool pixelInViewport(ivec2 pixel)
{
    return all(greaterThanEqual(pixel, ivec2(0))) &&
           all(lessThan(pixel, uViewportSize));
}

vec4 fetchLineRecord(ivec2 pixel)
{
    return texelFetch(uLineRecord, pixel, 0);
}

float lineCoverage(float distanceToLine, float signedWidth)
{
    float halfCoverageWidth = max(abs(signedWidth) * 0.5 - 0.5, 0.0);
    float distanceFromCore = abs(distanceToLine) - halfCoverageWidth;
    if (signedWidth > 0.0) {
        // Blender's smoothstep uses reversed endpoints. This equivalent form
        // keeps the interpolation endpoints ordered for GLSL implementations.
        return 1.0 - smoothstep(kLineSmoothStart, kLineSmoothEnd,
                                distanceFromCore);
    }
    return step(-0.5, halfCoverageWidth - abs(distanceToLine));
}

float neighborDistance(vec4 record, ivec2 offset)
{
    if (record.w == 0.0) {
        return kNoLineDistance;
    }

    // XY stores Blender's signed perpendicular direction, not the segment
    // tangent. Only the cardinal axis perpendicular to the dominant direction
    // contributes to the one-pixel search.
    bool directionHorizontal = abs(record.x) > abs(record.y);
    bool offsetHorizontal = offset.x != 0;
    if (directionHorizontal != offsetHorizontal) {
        return kNoLineDistance;
    }
    return record.z + dot(vec2(offset), -record.xy);
}

void blendCandidate(inout vec4 targetColor,
                    inout float targetDepth,
                    vec4 candidateColor,
                    float candidateDepth,
                    float coverage)
{
    candidateColor *= coverage;
    if (coverage > 0.0 && candidateDepth < targetDepth) {
        // Premultiplied alpha-over for the nearer line candidate.
        targetColor = candidateColor + targetColor * (1.0 - candidateColor.a);
        targetDepth = candidateDepth;
    } else {
        // Alpha-under preserves the scene or nearer candidate in front.
        targetColor += candidateColor * (1.0 - targetColor.a);
    }
}

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec4 targetColor = texelFetch(uBaseColor, pixel, 0);
    float targetDepth = texelFetch(uSceneDepth, pixel, 0).r;
    const ivec2 offsets[4] = ivec2[4](ivec2(1, 0), ivec2(-1, 0),
                                     ivec2(0, 1), ivec2(0, -1));

    vec4 centerRecord = fetchLineRecord(pixel);
    if (centerRecord.w != 0.0) {
        vec4 centerColor = texelFetch(uLineColor, pixel, 0);
        float centerCoverage =
            lineCoverage(centerRecord.z, centerRecord.w);
        centerColor *= centerCoverage;

        // Blender's dense one-pixel-wire fix restores some line opacity when
        // more than two neighboring pixels also carry a line sample.
        int validNeighborCount = 0;
        for (int index = 0; index < 4; ++index) {
            ivec2 neighborPixel = pixel + offsets[index];
            if (pixelInViewport(neighborPixel) &&
                fetchLineRecord(neighborPixel).w != 0.0) {
                ++validNeighborCount;
            }
        }
        float lineKernelSize = abs(centerRecord.w) * 0.5 - 0.5;
        if (centerCoverage > 0.0 &&
            texelFetch(uLineColor, pixel, 0).a >= 1.0 &&
            lineKernelSize < 0.45 && validNeighborCount > 2) {
            float blend = clamp(float(validNeighborCount) * 0.5 - 1.0,
                                0.0, 1.0);
            centerColor = mix(centerColor,
                              centerColor / max(centerColor.a, 1.0e-6),
                              blend);
        }
        blendCandidate(targetColor, targetDepth, centerColor,
                       texelFetch(uCandidateDepth, pixel, 0).r, 1.0);
    }

    for (int index = 0; index < 4; ++index) {
        ivec2 offset = offsets[index];
        ivec2 neighborPixel = pixel + offset;
        if (!pixelInViewport(neighborPixel)) {
            continue;
        }
        vec4 neighborRecord = fetchLineRecord(neighborPixel);
        if (neighborRecord.w == 0.0) {
            continue;
        }

        float distanceToLine = neighborDistance(neighborRecord, offset);
        float coverage = lineCoverage(distanceToLine, neighborRecord.w);
        blendCandidate(targetColor, targetDepth,
                       texelFetch(uLineColor, neighborPixel, 0),
                       texelFetch(uCandidateDepth, neighborPixel, 0).r,
                       coverage);
    }

    fragmentColor = targetColor;
    // Write the nearest resolved line depth only where a line candidate
    // actually contributed. Pixels without a visible candidate retain the
    // copied scene depth. This avoids copying the expanded stroke-quad depth
    // buffer back wholesale and lets the alpha resolve decide the depth.
    gl_FragDepth = targetDepth;
}
