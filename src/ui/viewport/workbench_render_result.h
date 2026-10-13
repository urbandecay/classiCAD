/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "core/serialization/blender_project_file.h"

#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QSize>
#include <QString>
#include <QVector>
#include <QVector3D>

#include <algorithm>
#include <limits>
#include <cmath>

namespace classiCAD {

inline QString workbenchCombinedInitialSavePath(const QString &renderFilepath,
                                                const QString &extension,
                                                bool useFileExtension = true)
{
    if (!renderFilepath.isEmpty()) return renderFilepath;
    return useFileExtension
        ? QStringLiteral("WorkbenchRender.%1").arg(extension)
        : QStringLiteral("WorkbenchRender");
}

inline QString workbenchCombinedSavePath(const QString &selectedPath,
                                         const QString &extension,
                                         bool useFileExtension)
{
    if (!useFileExtension || selectedPath.isEmpty()) return selectedPath;
    const QFileInfo selected(selectedPath);
    if (selected.suffix().isEmpty()) {
        return selectedPath + QStringLiteral(".") + extension;
    }
    if (selected.suffix().compare(extension, Qt::CaseInsensitive) == 0) {
        return selectedPath;
    }
    return selected.dir().filePath(
        selected.completeBaseName() + QStringLiteral(".") + extension);
}

// Convert the OpenGL depth-buffer value used by the Workbench render target
// into Blender's view-space Z pass convention.
inline float workbenchDepthToViewZ(float depth,
                                   bool perspective,
                                   float nearClip,
                                   float farClip) noexcept
{
    if (!std::isfinite(depth) || depth >= 1.0f || nearClip <= 0.0f ||
        farClip <= nearClip) {
        return 1.0e10f;
    }
    const float normalizedDepth = std::clamp(depth, 0.0f, 1.0f);
    return perspective
        ? (2.0f * nearClip * farClip) /
              (farClip + nearClip -
               (2.0f * normalizedDepth - 1.0f) * (farClip - nearClip))
        : nearClip + normalizedDepth * (farClip - nearClip);
}

// Composite a committed annotation layer over display Combined and, when
// supplied, over the top-down scene-linear RGBA Combined buffer.
bool compositeWorkbenchRenderAnnotations(
    QImage *displayCombined,
    const QImage &annotationLayer,
    QVector<float> *sceneLinearRgba,
    const QSize &sceneLinearSize);

// Blender's render dither is applied while producing an 8-bit byte buffer.
// Higher-depth PNG/TIFF and linear-float outputs keep their source precision.
inline qreal workbenchRenderDitherIntensity(qreal intensity,
                                           int writtenColorDepth) noexcept
{
    return writtenColorDepth == 8 && std::isfinite(intensity) &&
                   intensity > 0.0
               ? intensity
               : 0.0;
}

// Apply Blender's deterministic triangular dither to display-referred pixels
// immediately before an 8-bit render export. The image is kept at 16-bit
// precision until the caller performs its final byte conversion.
inline void applyWorkbenchRenderDither(QImage *image, qreal intensity,
                                       int writtenColorDepth,
                                       bool premultipliedAlpha = false)
{
    if (image == nullptr || image->isNull() ||
        workbenchRenderDitherIntensity(intensity, writtenColorDepth) == 0.0) {
        return;
    }
    if (image->format() != QImage::Format_RGBA64) {
        *image = image->convertToFormat(QImage::Format_RGBA64);
        if (image->isNull()) return;
    }
    for (int row = 0; row < image->height(); ++row) {
        auto *channels = reinterpret_cast<quint16 *>(image->scanLine(row));
        const quint32 y = static_cast<quint32>(image->height() - 1 - row);
        for (int column = 0; column < image->width(); ++column) {
            const quint32 x = static_cast<quint32>(column);
            const quint32 qx = 1103515245u * ((x >> 1u) ^ y);
            const quint32 qy = 1103515245u * ((y >> 1u) ^ x);
            const quint32 hash = 1103515245u * (qx ^ (qy >> 3u));
            float value = static_cast<float>(hash) *
                (1.0f / static_cast<float>(std::numeric_limits<quint32>::max()));
            value = value * 2.0f - 1.0f;
            const float noise = std::copysign(1.0f, value) *
                (1.0f - std::sqrt(1.0f - std::fabs(value)));
            const float offset = noise * 0.0033f *
                                 static_cast<float>(intensity);
            const float alpha = static_cast<float>(channels[column * 4 + 3]) /
                                65535.0f;
            for (int channel = 0; channel < 3; ++channel) {
                const int index = column * 4 + channel;
                float display = static_cast<float>(channels[index]) /
                                65535.0f;
                if (premultipliedAlpha && alpha > 0.0f) {
                    display /= alpha;
                }
                display = std::clamp(display + offset, 0.0f, 1.0f);
                if (premultipliedAlpha) display *= alpha;
                channels[index] = static_cast<quint16>(
                    std::lround(display * 65535.0f));
            }
        }
    }
}

// Offscreen Workbench-style scene output together with the saved camera pose
// and optics used to produce it.
struct WorkbenchRenderResult {
    QImage combined;
    QSize outputSize;
    // Scene-linear RGBA Combined pass, tightly packed as four floats per
    // pixel. This is independent of the display-referred QImage above.
    QVector<float> sceneLinearRgba;
    QSize sceneLinearSize;
    QVector<float> depth;
    bool combinedPassAvailable = true;
    bool depthPassAvailable = true;
    bool transparentFilm = false;
    bool transparentBackground = false;
    QString backgroundType;
    bool sceneWorldColorAvailable = false;
    QVector3D sceneWorldColorSceneLinear;
    bool renderUsesSceneWorldBackground = false;
    ProjectViewportCameraSettings camera;
    QString outputFormat = QStringLiteral("PNG + PFM depth");
    QString blenderImageFormat = QStringLiteral("PNG");
    QString blenderRenderFilepath;
    bool useFileExtension = true;
    QString exrCodec = QStringLiteral("ZIP");
    QString colorMode = QStringLiteral("RGBA");
    int colorDepth = 8;
    qreal pixelAspectX = 1.0;
    qreal pixelAspectY = 1.0;
    int imageQuality = 90;
    int imageCompression = 15;
    QString tiffCodec = QStringLiteral("DEFLATE");
    QString colorDisplayDevice;
    QString viewTransform;
    QString colorLook;
    QString colorLutId;
    qreal colorExposure = 0.0;
    qreal colorGamma = 1.0;
    bool colorManagementApplied = false;

    bool isValid() const noexcept
    {
        const bool hasCombined = combinedPassAvailable && !combined.isNull();
        const bool hasDepth = hasDepthPass();
        return hasCombined || hasDepth;
    }

    // Drop output payloads that were not registered by the selected Blender
    // view layer. The renderer may still use depth internally while drawing.
    void discardUnavailablePassPayloads();

    bool hasValidSceneLinearCombined() const noexcept
    {
        if (sceneLinearSize.isEmpty()) return false;
        const qsizetype pixelCount = qsizetype(sceneLinearSize.width()) *
                                     sceneLinearSize.height();
        return pixelCount > 0 && pixelCount <= std::numeric_limits<qsizetype>::max() / 4 &&
               sceneLinearRgba.size() == pixelCount * 4;
    }

    bool hasDepthPass() const noexcept
    {
        const QSize size = outputSize.isEmpty() ? combined.size() : outputSize;
        return depthPassAvailable && !size.isEmpty() &&
               depth.size() == qsizetype(size.width()) * size.height();
    }

    bool hasSceneLinearDepthPass() const noexcept
    {
        return hasDepthPass() && hasValidSceneLinearCombined() &&
               sceneLinearSize == (outputSize.isEmpty() ? combined.size()
                                                         : outputSize);
    }

    bool savePng(const QString &path, QString *errorMessage = nullptr) const;
    QString combinedImageFormatError() const;
    QString combinedFileExtension() const;
    int writtenColorDepth() const;
    bool saveCombinedImage(const QString &path,
                           QString *errorMessage = nullptr) const;
    static bool openExrWriterAvailable() noexcept;
    bool saveCombinedOpenExr(const QString &path,
                             QString *errorMessage = nullptr) const;
    bool saveDepthPfm(const QString &path,
                      QString *errorMessage = nullptr) const;
};

// Apply Blender's normalized render border to already rendered full-frame
// buffers. Border coordinates use Blender's bottom-left origin. Crop mode
// changes output dimensions while preserving the original camera framing;
// non-crop mode keeps the full frame and clears pixels outside the border.
bool applyWorkbenchRenderBorder(WorkbenchRenderResult *result,
                                bool useBorder, bool cropToBorder,
                                qreal minX, qreal minY,
                                qreal maxX, qreal maxY);

} // namespace classiCAD
