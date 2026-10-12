/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#version 330 core

uniform sampler2D uPicture;
uniform float uOpacity;
uniform int uSceneLinearOutput;
in vec2 vTextureCoordinate;
out vec4 fragmentColor;

void main()
{
    vec4 color = texture(uPicture, vTextureCoordinate);
    if (uSceneLinearOutput != 0) {
        vec3 encoded = color.rgb;
        color.rgb = mix(encoded / 12.92,
                        pow((encoded + 0.055) / 1.055, vec3(2.4)),
                        step(vec3(0.04045), encoded));
    }
    fragmentColor = vec4(color.rgb, color.a * uOpacity);
}
