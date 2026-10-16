/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#version 330 core

uniform vec2 uViewportSize;
uniform vec3 uHighGradient;
uniform vec3 uGradient;
uniform int uSceneLinearOutput;
uniform int uColorsSceneLinear;
uniform int uGradientStyle;

out vec4 fragmentColor;

float backgroundDither()
{
    const int bayer4x4[16] = int[16](
        0, 8, 2, 10,
        12, 4, 14, 6,
        3, 11, 1, 9,
        15, 7, 13, 5);
    ivec2 pixel = ivec2(gl_FragCoord.xy) & ivec2(3);
    int index = pixel.x * 4 + pixel.y;
    return (((float(bayer4x4[index]) + 0.5) / 16.0) - 0.5) / 255.0;
}

void main()
{
    vec2 screenUv = gl_FragCoord.xy / max(uViewportSize, vec2(1.0));
    vec2 centeredUv = screenUv - 0.5;
    float gradientFactor = uGradientStyle == 0
        ? 0.0
        : uGradientStyle == 1
            ? clamp(1.0 - screenUv.y, 0.0, 1.0)
            : clamp(length(centeredUv) * 1.41421356237, 0.0, 1.0);

    if (uSceneLinearOutput != 0) {
        vec3 highLinear = uHighGradient;
        vec3 lowLinear = uGradient;
        if (uColorsSceneLinear == 0) {
            // QColor components are sRGB encoded. Decode before
            // interpolation so this pass can feed a scene-linear target.
            highLinear = mix(uHighGradient / 12.92,
                             pow((uHighGradient + 0.055) / 1.055,
                                 vec3(2.4)),
                             step(vec3(0.04045), uHighGradient));
            lowLinear = mix(uGradient / 12.92,
                            pow((uGradient + 0.055) / 1.055,
                                vec3(2.4)),
                            step(vec3(0.04045), uGradient));
        }
        vec3 color = mix(highLinear, lowLinear, gradientFactor);
        fragmentColor = vec4(color, 1.0);
        return;
    }

    // Preserve the legacy output for callers rendering directly to a display target.
    vec3 highGamma = pow(uHighGradient, vec3(1.0 / 2.2));
    vec3 lowGamma = pow(uGradient, vec3(1.0 / 2.2));
    vec3 color = pow(mix(highGamma, lowGamma, gradientFactor), vec3(2.2));
    color += backgroundDither();
    fragmentColor = vec4(color, 1.0);
}
