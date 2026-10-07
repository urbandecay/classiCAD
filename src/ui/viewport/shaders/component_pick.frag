/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#version 330 core

flat in vec4 vPickColor;
out vec4 fragmentColor;

void main()
{
    fragmentColor = vPickColor;
}
