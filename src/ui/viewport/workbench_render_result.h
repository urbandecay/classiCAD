/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "core/serialization/blender_project_file.h"

#include <QImage>
#include <QSize>
#include <QString>
#include <QVector>

#include <limits>

namespace classiCAD {

// Offscreen Workbench-style scene output together with the saved camera pose
// and optics used to produce it.
struct WorkbenchRenderResult {
    QImage combined;
    // Scene-linear RGBA Combined pass, tightly packed as four floats per
    // pixel. This is independent of the display-referred QImage above.
    QVector<float> sceneLinearRgba;
    QSize sceneLinearSize;
    QVector<float> depth;
    ProjectViewportCameraSettings camera;
    QString outputFormat = QStringLiteral("PNG + PFM depth");
    QString blenderImageFormat = QStringLiteral("PNG");
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
        return !combined.isNull();
    }

    bool hasValidSceneLinearCombined() const noexcept
    {
        if (sceneLinearSize.isEmpty()) return false;
        const qsizetype pixelCount = qsizetype(sceneLinearSize.width()) *
                                     sceneLinearSize.height();
        return pixelCount > 0 && pixelCount <= std::numeric_limits<qsizetype>::max() / 4 &&
               sceneLinearRgba.size() == pixelCount * 4;
    }

    bool savePng(const QString &path, QString *errorMessage = nullptr) const;
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

} // namespace classiCAD
