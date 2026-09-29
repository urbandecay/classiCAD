/* SPDX-FileCopyrightText: 2017-2025 Blender Authors
 * Copyright adapted by classiCAD contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Adapted from Blender's overlay_grid_vert.glsl. The original procedural
 * vertex-ID line decoding, three-level grid layout, camera-relative snapping,
 * level blending, and finite line placement are retained; Blender's draw
 * engine interfaces are replaced with standalone uniforms.
 * Source: https://github.com/blender/blender/blob/main/source/blender/draw/engines/overlay/shaders/overlay_grid_vert.glsl
 */
#version 330 core

uniform mat4 uViewProjection;
uniform vec3 uGridOrigin;
uniform vec3 uAxisU;
uniform vec3 uAxisV;
uniform int uAxisVisibleX;
uniform int uAxisVisibleY;
uniform int uAxisVisibleZ;
uniform vec2 uGridOffset;
uniform float uStepBase;
uniform int uBaseStepIndex;
uniform int uStepCount;
uniform float uLevelFraction;
uniform int uGridLineCount;
uniform int uMode;
uniform int uPerspective;
uniform int uIteration;
uniform float uZoom;
uniform vec2 uViewportSize;

out vec2 vGridCoord;
out vec3 vWorldPosition;
flat out float vAlpha;
flat out float vEmphasis;
flat out float vFixedCoordinate;
flat out int vLineAxis;
flat out int vRenderMode;
flat out vec2 vLineStart;
flat out vec2 vLineEnd;

struct LineData {
    vec2 P;
    int axis;
    int level;
};

// Blender overlay_grid_vert.glsl: gl_VertexID encodes line side, axis,
// level, and index so no per-line CPU geometry buffer is needed.
LineData decodeGridData(uint vertexId)
{
    LineData line;
    uint side = vertexId & 1u;
    vertexId >>= 1u;
    line.axis = int(vertexId & 1u);
    vertexId >>= 1u;
    line.level = 2 - int(vertexId / uint(uGridLineCount));
    vertexId %= uint(uGridLineCount);

    float halfLines = max(float(uGridLineCount >> 1), 1.0);
    line.P = vec2(halfLines, float(vertexId) - halfLines);
    line.P.x = side != 0u ? line.P.x : -line.P.x;
    if (line.axis != 0) {
        line.P = line.P.yx;
    }
    return line;
}

LineData decodeAxisData(uint vertexId)
{
    LineData line;
    uint side = vertexId & 1u;
    line.axis = int(vertexId >> 1u);
    line.level = 2;
    line.P = vec2(max(float(uGridLineCount >> 1), 1.0) *
                      (side != 0u ? -1.0 : 1.0),
                  0.0);
    return line;
}

vec2 snapGridOffset(vec2 offset, int lineAxis, float stepSize)
{
    // Blender snaps only the fixed coordinate; the other follows the camera.
    if (lineAxis == 0) {
        offset.y = round(offset.y / stepSize) * stepSize;
    }
    else {
        offset.x = round(offset.x / stepSize) * stepSize;
    }
    return offset;
}

vec3 worldPosition(vec2 gridPosition)
{
    return uGridOrigin + uAxisU * gridPosition.x + uAxisV * gridPosition.y;
}

vec3 globalAxisDirection(int axisIndex)
{
    if (axisIndex == 0) {
        return vec3(1.0, 0.0, 0.0);
    }
    if (axisIndex == 1) {
        return vec3(0.0, 1.0, 0.0);
    }
    return vec3(0.0, 0.0, 1.0);
}

bool globalAxisVisible(int axisIndex)
{
    if (axisIndex == 0) {
        return uAxisVisibleX != 0;
    }
    if (axisIndex == 1) {
        return uAxisVisibleY != 0;
    }
    return uAxisVisibleZ != 0;
}

float stepSizeForLevel(int lineLevel)
{
    int levelIndex = clamp(uBaseStepIndex + lineLevel - 1,
                           0,
                           max(uStepCount - 1, 0));
    return uStepBase * pow(10.0, float(levelIndex - uBaseStepIndex));
}

vec2 screenPosition(vec4 clipPosition)
{
    return (clipPosition.xy / clipPosition.w * 0.5 + 0.5) * uViewportSize;
}

void main()
{
    LineData line;
    if (uMode == 0) {
        line = decodeGridData(uint(gl_VertexID));
    }
    else {
        line = decodeAxisData(uint(gl_VertexID));
    }
    float halfLines = max(float(uGridLineCount >> 1), 1.0);
    float stepSize = stepSizeForLevel(line.level);
    vec2 offset = uMode == 0 ? snapGridOffset(uGridOffset, line.axis, stepSize)
                            : vec2(0.0);
    vec2 rawPosition = line.P;
    vec2 position = offset + stepSize * line.P;

    vGridCoord = rawPosition / halfLines;
    vEmphasis = clamp(float(line.level) - uLevelFraction, 0.0, 1.0);
    vAlpha = clamp(float(line.level) + 1.0 - uLevelFraction, 0.0, 1.0);
    if (uMode == 0 && uPerspective == 0) {
        // Blender fades ortho levels that would otherwise shimmer below pixel size.
        vAlpha *= smoothstep(0.25, 4.0, stepSize * uZoom);
    }
    vFixedCoordinate = uMode == 0 ? position[1 - line.axis] : 0.0;
    vLineAxis = line.axis;
    vRenderMode = uMode;
    vWorldPosition = uMode == 0
                         ? worldPosition(position)
                         : uGridOrigin + globalAxisDirection(line.axis) *
                                               (stepSize * line.P.x);
    if (uMode != 0) {
        vAlpha = globalAxisVisible(line.axis) ? 1.0 : 0.0;
        vEmphasis = 1.0;
    }

    LineData pairedLine;
    if (uMode == 0) {
        pairedLine = decodeGridData(uint(gl_VertexID) ^ 1u);
    }
    else {
        pairedLine = decodeAxisData(uint(gl_VertexID) ^ 1u);
    }
    float pairedStep = stepSizeForLevel(pairedLine.level);
    vec2 pairedOffset = uMode == 0
                            ? snapGridOffset(uGridOffset,
                                             pairedLine.axis,
                                             pairedStep)
                            : vec2(0.0);
    vec2 pairedPosition = pairedOffset + pairedStep * pairedLine.P;
    vec3 pairedWorldPosition = uMode == 0
                                   ? worldPosition(pairedPosition)
                                   : uGridOrigin + globalAxisDirection(pairedLine.axis) *
                                                         (pairedStep * pairedLine.P.x);
    vec4 thisClip = uViewProjection * vec4(vWorldPosition, 1.0);
    if (uPerspective != 0) {
        // Blender progressively biases its four perspective grid iterations
        // through the scene depth so coincident geometry does not z-fight.
        float zFactor = float(uIteration * 3 + line.level) / 12.0;
        thisClip.z += mix(0.00025, -0.00020, zFactor);
    }
    vec4 pairedClip = uViewProjection * vec4(pairedWorldPosition, 1.0);
    vec2 thisScreen = screenPosition(thisClip);
    vec2 pairedScreen = screenPosition(pairedClip);
    bool firstVertex = (uint(gl_VertexID) & 1u) == 0u;
    vLineStart = firstVertex ? thisScreen : pairedScreen;
    vLineEnd = firstVertex ? pairedScreen : thisScreen;
    gl_Position = thisClip;
}
