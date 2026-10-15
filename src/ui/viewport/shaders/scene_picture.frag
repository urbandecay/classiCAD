/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#version 330 core

uniform sampler2D uPicture;
uniform float uOpacity;
uniform int uSceneLinearOutput;
uniform vec4 uColor;
uniform int uUseAlphaBlend;
uniform int uPremultipliedAlpha;
in vec2 vTextureCoordinate;
out vec4 fragmentColor;

void main()
{
    vec4 texColor = texture(uPicture, vTextureCoordinate);
    if (uUseAlphaBlend == 0 && texColor.a < 0.05) discard;
    vec4 color = texColor;
    vec3 linearColor = texColor.rgb;
    if (uPremultipliedAlpha != 0 &&
        texColor.a > 0.0 && texColor.a < 1.0) {
        linearColor /= texColor.a;
    }
    linearColor *= uColor.rgb;
    color.rgb = uSceneLinearOutput != 0
        ? linearColor
        : mix(linearColor * 12.92,
              1.055 * pow(linearColor, vec3(1.0 / 2.4)) - 0.055,
              step(vec3(0.0031308), linearColor));
    color.a *= uColor.a;
    if (uUseAlphaBlend == 0) color.a = 1.0;
    fragmentColor = vec4(color.rgb,
                         color.a * uOpacity);
}
