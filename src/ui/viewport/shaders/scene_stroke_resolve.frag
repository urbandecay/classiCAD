/* SPDX-FileCopyrightText: 2019-2023 Blender Authors
 * SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#version 330 core

uniform sampler2D uBaseColor;
uniform sampler2D uLineColor;
uniform sampler2D uLineRecord;
uniform sampler2D uCandidateDepth;
uniform sampler2D uSceneDepth;
uniform sampler2D uLineWireFlag;
uniform ivec2 uViewportSize;
uniform int uIgnoreSceneDepth;
uniform int uManualSrgbBlend;
uniform float uXrayHiddenFade;

layout(location = 0) out vec4 fragmentColor;

const float kNoLineDistance = 1.0e10;

vec3 srgbToLinear(vec3 color)
{
    vec3 low = color / 12.92;
    vec3 high = pow((color + 0.055) / 1.055, vec3(2.4));
    return mix(high, low, lessThanEqual(color, vec3(0.04045)));
}

vec3 linearToSrgb(vec3 color)
{
    vec3 low = color * 12.92;
    vec3 high = 1.055 * pow(max(color, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
    return mix(high, low, lessThanEqual(color, vec3(0.0031308)));
}

bool pixelInViewport(ivec2 pixel)
{
    return all(greaterThanEqual(pixel, ivec2(0))) &&
           all(lessThan(pixel, uViewportSize));
}

float xrayFadeForSample(ivec2 pixel, float candidateDepth)
{
    if (uIgnoreSceneDepth == 0) {
        return 1.0;
    }
    float sceneDepth = texelFetch(uSceneDepth, pixel, 0).r;
    return candidateDepth > sceneDepth ? uXrayHiddenFade : 1.0;
}

vec4 fetchLineRecord(ivec2 pixel)
{
    return texelFetch(uLineRecord, pixel, 0);
}

bool isNativeCurveWirePixel(ivec2 pixel)
{
    float kind = texelFetch(uLineWireFlag, pixel, 0).r;
    return kind > 0.2 && kind < 0.3;
}

vec4 decodeBlenderOverlayLineData(vec4 packedRecord)
{
    // Blender overlay_antialiasing.bsl.hh::Line::decode.
    return vec4(packedRecord.xy * 2.0 - 1.0,
                (packedRecord.z - 0.1) * 4.0 - 2.0,
                packedRecord.w);
}

vec4 fetchDecodedLineRecord(ivec2 pixel)
{
    vec4 record = fetchLineRecord(pixel);
    return isNativeCurveWirePixel(pixel)
        ? decodeBlenderOverlayLineData(record)
        : record;
}

float lineCoverage(float distanceToLine, float signedWidth)
{
    // Blender's theme pixel size becomes the signed stroke width here. For
    // the committed curve overlay that is one device pixel, so its kernel is
    // 1/2 - 1/2 == 0.
    float lineKernelSize = max(abs(signedWidth) * 0.5 - 0.5, 0.0);
    float distanceFromCore = abs(distanceToLine) - lineKernelSize;
    if (signedWidth > 0.0) {
        float discRadius = 0.5641895835477563 * 1.05;
        float smoothStart = 0.5 - discRadius;
        float smoothEnd = 0.5 + discRadius;
        // Blender's smoothstep uses reversed endpoints. This equivalent form
        // keeps the interpolation endpoints ordered for GLSL implementations.
        float coverage = 1.0 - smoothstep(smoothStart, smoothEnd,
                                          distanceFromCore);
        return coverage;
    }
    return step(-0.5, lineKernelSize - abs(distanceToLine));
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

void blendNeighbor(inout vec4 targetColor,
                   inout float targetDepth,
                   vec4 candidateColor,
                   float candidateDepth,
                   float coverage)
{
    candidateColor *= coverage;
    if (coverage > 0.0 && candidateDepth < targetDepth) {
        // Blender alpha-overs only a strictly nearer neighboring wire.
        targetColor = candidateColor + targetColor * (1.0 - candidateColor.a);
        targetDepth = candidateDepth;
    } else {
        // Equal-depth samples follow Blender's alpha-under branch.
        targetColor += candidateColor * (1.0 - targetColor.a);
    }
}

float correctedMeshWireDepth(ivec2 pixel, vec4 record, float wireDepth)
{
    float lineWireKind = texelFetch(uLineWireFlag, pixel, 0).r;
    if (uIgnoreSceneDepth != 0 || record.w <= 0.0 ||
        lineWireKind < 0.75) {
        return wireDepth;
    }

    bool directionHorizontal = abs(record.x) > abs(record.y);
    ivec2 offset = directionHorizontal ? ivec2(1, 0) : ivec2(0, 1);
    float sceneDepth = texelFetch(uSceneDepth, pixel, 0).r;
    float neighborMinimum = sceneDepth;
    ivec2 plusPixel = pixel + offset;
    ivec2 minusPixel = pixel - offset;
    if (pixelInViewport(plusPixel)) {
        neighborMinimum = min(
            neighborMinimum, texelFetch(uSceneDepth, plusPixel, 0).r);
    }
    if (pixelInViewport(minusPixel)) {
        neighborMinimum = min(
            neighborMinimum, texelFetch(uSceneDepth, minusPixel, 0).r);
    }
    float depthDelta = abs(sceneDepth - neighborMinimum);
    if (wireDepth < sceneDepth + depthDelta && wireDepth > sceneDepth) {
        return sceneDepth;
    }
    return wireDepth;
}

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec4 baseColor = texelFetch(uBaseColor, pixel, 0);
    if (uManualSrgbBlend != 0) {
        baseColor.rgb = srgbToLinear(baseColor.rgb);
    }
    float sceneDepth = texelFetch(uSceneDepth, pixel, 0).r;
    const ivec2 offsets[4] = ivec2[4](ivec2(1, 0), ivec2(-1, 0),
                                     ivec2(0, 1), ivec2(0, -1));

    vec4 centerRecord = fetchDecodedLineRecord(pixel);
    vec4 resolvedLineColor = vec4(0.0);
    float resolvedLineDepth = uIgnoreSceneDepth != 0 ? 1.0 : sceneDepth;
    vec4 centerRasterColor = vec4(0.0);
    if (centerRecord.w != 0.0) {
        centerRasterColor = texelFetch(uLineColor, pixel, 0);
        float centerCoverage = lineCoverage(centerRecord.z, centerRecord.w);
        float centerDepth = texelFetch(uCandidateDepth, pixel, 0).r;
        resolvedLineColor = centerRasterColor * centerCoverage *
                            xrayFadeForSample(pixel, centerDepth);
        // The opaque stroke prepass already tests center candidates against
        // scene depth with LEQUAL. Retain that candidate as the initial AA
        // overlay sample; neighbors are compared against its depth below.
        resolvedLineDepth = correctedMeshWireDepth(pixel, centerRecord,
                                                   centerDepth);
    }

    bool centerIsNativeCurveWire = isNativeCurveWirePixel(pixel);
    // Blender gathers neighboring pixels in Smooth Wires mode. Native GL
    // curve lines only record pixels owned by the one-pixel line rasterizer,
    // so an empty center must still gather an adjacent curve fragment. Keep
    // the expanded-quad behavior unchanged for the other existing stroke
    // classes.
    bool gatherNeighbors = centerRecord.w > 0.0 || centerRecord.z > 1.0;
    if (!gatherNeighbors && !centerIsNativeCurveWire) {
        for (int index = 0; index < 4; ++index) {
            ivec2 neighborPixel = pixel + offsets[index];
            float neighborWireKind = pixelInViewport(neighborPixel)
                ? texelFetch(uLineWireFlag, neighborPixel, 0).r
                : 0.0;
            if (neighborWireKind > 0.2 && neighborWireKind < 0.3 &&
                fetchLineRecord(neighborPixel).w > 0.0) {
                gatherNeighbors = true;
                break;
            }
        }
    }
    if (gatherNeighbors) {
        for (int index = 0; index < 4; ++index) {
            ivec2 offset = offsets[index];
            ivec2 neighborPixel = pixel + offset;
            if (!pixelInViewport(neighborPixel)) {
                continue;
            }
            vec4 neighborRecord = fetchDecodedLineRecord(neighborPixel);
            if (neighborRecord.w == 0.0) {
                continue;
            }

            float distanceToLine = neighborDistance(neighborRecord, offset);
            float coverage = lineCoverage(distanceToLine,
                                          neighborRecord.w);
            float neighborDepth = correctedMeshWireDepth(
                neighborPixel, neighborRecord,
                texelFetch(uCandidateDepth, neighborPixel, 0).r);
            if (uIgnoreSceneDepth == 0 &&
                neighborDepth > sceneDepth + 1.0e-6) {
                continue;
            }
            float neighborFade = xrayFadeForSample(neighborPixel, neighborDepth);
            blendNeighbor(resolvedLineColor, resolvedLineDepth,
                          texelFetch(uLineColor, neighborPixel, 0) * neighborFade,
                          neighborDepth, coverage);
        }
    }

    // Blender first folds the four neighboring line samples into the center
    // overlay sample, then applies its dense one-pixel-wire correction to
    // that overlay color. The gate uses the original center wire alpha before
    // any line coverage or neighbor blending.
    if (gatherNeighbors && centerRecord.w != 0.0) {
        int validNeighborCount = 0;
        for (int index = 0; index < 4; ++index) {
            ivec2 neighborPixel = pixel + offsets[index];
            if (pixelInViewport(neighborPixel) &&
                fetchLineRecord(neighborPixel).w != 0.0) {
                ++validNeighborCount;
            }
        }
        float lineKernelSize = abs(centerRecord.w) * 0.5 - 0.5;
        // Match Blender's original center-alpha gate and four raw neighbor
        // validity checks after composing the overlay samples.
        if (centerRasterColor.a >= 1.0 && resolvedLineColor.a > 0.0 &&
            lineKernelSize < 0.45 &&
            validNeighborCount > 2) {
            float blend = clamp(float(validNeighborCount) * 0.5 - 1.0,
                                0.0, 1.0);
            resolvedLineColor = mix(
                resolvedLineColor,
                resolvedLineColor / resolvedLineColor.a,
                blend);
        }
    }

    // Resolve the line overlay separately, then composite it over the copied
    // scene target. Folding all wires before a single scene-depth comparison
    // can incorrectly place a farther wire in front of the surface.
    vec4 compositedColor = resolvedLineColor + baseColor *
                           (1.0 - resolvedLineColor.a);
    if (uManualSrgbBlend != 0) {
        compositedColor.rgb = linearToSrgb(compositedColor.rgb);
    }
    fragmentColor = compositedColor;
}
