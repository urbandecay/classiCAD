/* SPDX-FileCopyrightText: 2017-2025 Blender Authors
 * Copyright adapted by classiCAD contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Adapted from Blender's overlay_grid_frag.glsl: level color, finite-grid and
 * grazing-angle fades, far-clip fade, 4px low-alpha stipple, and perspective
 * additive-pass weights are kept in the shader path.
 * Source: https://github.com/blender/blender/blob/main/source/blender/draw/engines/overlay/shaders/overlay_grid_frag.glsl
 */
#version 330 core

uniform vec3 uPlaneNormal;
uniform vec3 uCameraPosition;
uniform vec3 uViewDirection;
uniform float uFarClipDistance;
uniform vec2 uViewportSize;
uniform float uDevicePixelRatio;
uniform int uStippleEnabled;
uniform float uStippleThreshold;
uniform float uStippleDashWidth;
uniform float uGrazingFadeExponent;
uniform float uOrthographicEdgeFadeExponent;
uniform float uFarFadeStart;
uniform float uFarFadeEnd;
uniform int uPerspective;
uniform vec4 uGridColor;
uniform vec4 uGridEmphasisColor;
uniform vec4 uAxisColorX;
uniform vec4 uAxisColorY;
uniform vec4 uAxisColorZ;
uniform int uMode;
uniform int uIteration;

in vec2 vGridCoord;
in vec3 vWorldPosition;
flat in float vAlpha;
flat in float vEmphasis;
flat in float vFixedCoordinate;
flat in int vLineAxis;
flat in int vRenderMode;
flat in vec2 vLineStart;
flat in vec2 vLineEnd;

out vec4 fragmentColor;

float smoothstepBlend(float edge0, float edge1, float value)
{
    return smoothstep(edge0, edge1, value);
}

void main()
{
    vec4 color = mix(uGridColor, uGridEmphasisColor, vEmphasis);
    if (vRenderMode != 0) {
        color = vLineAxis == 0 ? uAxisColorX
                              : (vLineAxis == 1 ? uAxisColorY : uAxisColorZ);
    }
    else if (abs(vFixedCoordinate) <= 2.0e-7) {
        // Blender's fragment shader omits floor-grid fragments underneath axes.
        discard;
    }

    float alpha = color.a * vAlpha;
    if (uPerspective != 0) {
        float lengthFade = 1.0 - min(1.0, length(vGridCoord));
        alpha *= lengthFade;

        vec3 viewVector = uCameraPosition - vWorldPosition;
        float viewDistance = length(viewVector);
        if (viewDistance <= 1.0e-7) {
            discard;
        }
        viewVector /= viewDistance;
        float facing = abs(dot(viewVector, normalize(uPlaneNormal)));
        alpha *= 1.0 - pow(1.0 - facing, uGrazingFadeExponent);
        alpha *= 1.0 - smoothstepBlend(uFarFadeStart * uFarClipDistance,
                                       uFarFadeEnd * uFarClipDistance,
                                       viewDistance);
    } else {
        float lengthFade = 1.0 - min(1.0, dot(vGridCoord, vGridCoord));
        alpha *= pow(lengthFade, uOrthographicEdgeFadeExponent);
        float facing = abs(dot(normalize(uViewDirection),
                               normalize(uPlaneNormal)));
        alpha *= 1.0 - pow(1.0 - facing, uGrazingFadeExponent);
    }

    // Blender uses a four-pixel stipple once the fragment fades below 10%.
    vec2 screenPosition = gl_FragCoord.xy / max(uDevicePixelRatio, 1.0);
    vec2 lineDelta = vLineEnd - vLineStart;
    float lineLength = length(lineDelta);
    float distanceAlongLine = lineLength > 1.0e-5
                                  ? dot(screenPosition - vLineStart,
                                        lineDelta / lineLength)
                                  : 0.0;
    if (uStippleEnabled != 0 && alpha < uStippleThreshold &&
        alpha / uStippleThreshold < fract(distanceAlongLine / uStippleDashWidth)) {
        discard;
    }

    // Blender's perspective grid repeats four additive draws with these weights.
    const float additiveAlpha[4] = float[4](1.0, 0.50, 0.25, 0.125);
    alpha *= additiveAlpha[clamp(uIteration, 0, 3)];
    fragmentColor = vec4(color.rgb, alpha);
}
