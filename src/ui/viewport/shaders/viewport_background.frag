/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#version 330 core

uniform vec2 uViewportSize;
uniform vec3 uHighGradient;
uniform vec3 uGradient;

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
    float gradientFactor = clamp(length(centeredUv) * 1.41421356237,
                                 0.0,
                                 1.0);

    // Blend the theme colors in gamma space, then return to linear space.
    vec3 highGamma = pow(uHighGradient, vec3(1.0 / 2.2));
    vec3 lowGamma = pow(uGradient, vec3(1.0 / 2.2));
    vec3 color = pow(mix(highGamma, lowGamma, gradientFactor), vec3(2.2));
    color += backgroundDither();
    fragmentColor = vec4(color, 1.0);
}
