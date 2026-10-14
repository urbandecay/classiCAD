/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <QByteArray>
#include <QOpenGLFunctions_3_3_Core>
#include <QtEndian>

#include <cstring>

namespace classiCAD {

// Upload the exact two-layer half-float MatCap payload used by the Workbench
// GPU path. QMC rows are top-down; OpenGL texture coordinates use a bottom-up
// origin, so reverse each layer's rows while preserving its RGB half values.
inline bool uploadWorkbenchMatcapTextureArray(
    QOpenGLFunctions_3_3_Core &gl, GLuint *texture,
    const QByteArray &layers, int width, int height)
{
    if (texture == nullptr || width <= 0 || height <= 0 || layers.isEmpty()) {
        return false;
    }
    const qsizetype rowBytes = qsizetype(width) * 3 * qsizetype(sizeof(quint16));
    const qsizetype expectedBytes = rowBytes * height * 2;
    if (layers.size() != expectedBytes) return false;

    QByteArray texturePixels(layers.size(), Qt::Uninitialized);
    for (int layer = 0; layer < 2; ++layer) {
        for (int row = 0; row < height; ++row) {
            const qsizetype sourceOffset =
                (qsizetype(layer) * height + (height - row - 1)) * rowBytes;
            const qsizetype destinationOffset =
                (qsizetype(layer) * height + row) * rowBytes;
            std::memcpy(texturePixels.data() + destinationOffset,
                        layers.constData() + sourceOffset,
                        static_cast<size_t>(rowBytes));
        }
    }

    GLint previousBinding = 0;
    GLint previousUnpackAlignment = 4;
    gl.glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &previousBinding);
    gl.glGetIntegerv(GL_UNPACK_ALIGNMENT, &previousUnpackAlignment);
    if (*texture == 0) gl.glGenTextures(1, texture);
    if (*texture == 0) return false;

    gl.glBindTexture(GL_TEXTURE_2D_ARRAY, *texture);
    gl.glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    gl.glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGB16F, width, height, 2, 0,
                    GL_RGB, GL_HALF_FLOAT, texturePixels.constData());
    gl.glPixelStorei(GL_UNPACK_ALIGNMENT, previousUnpackAlignment);
    gl.glBindTexture(GL_TEXTURE_2D_ARRAY, static_cast<GLuint>(previousBinding));
    return true;
}

} // namespace classiCAD
