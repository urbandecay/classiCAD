/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "core/serialization/blender_project_file.h"

#include <QImage>
#include <QString>
#include <QVector>

namespace classiCAD {

// Offscreen Workbench-style scene output together with the saved camera pose
// and optics used to produce it.
struct WorkbenchRenderResult {
    QImage combined;
    QVector<float> depth;
    ProjectViewportCameraSettings camera;
    QString outputFormat = QStringLiteral("PNG + PFM depth");

    bool isValid() const noexcept
    {
        return !combined.isNull();
    }

    bool savePng(const QString &path, QString *errorMessage = nullptr) const;
    bool saveDepthPfm(const QString &path,
                      QString *errorMessage = nullptr) const;
};

} // namespace classiCAD
