/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "workbench_render_result.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>

namespace classiCAD {

bool WorkbenchRenderResult::savePng(const QString &path,
                                    QString *errorMessage) const
{
    if (!isValid()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("The viewport render result is empty");
        }
        return false;
    }

    QImage output = combined;
    QJsonObject cameraJson;
    cameraJson.insert(QStringLiteral("focalLengthMillimeters"),
                      camera.focalLengthMillimeters);
    cameraJson.insert(QStringLiteral("clipStart"), camera.clipStart);
    cameraJson.insert(QStringLiteral("clipEnd"), camera.clipEnd);
    cameraJson.insert(QStringLiteral("sceneCameraEnabled"),
                      camera.sceneCameraEnabled);
    cameraJson.insert(QStringLiteral("focusDistance"), camera.focusDistance);
    cameraJson.insert(QStringLiteral("fStop"), camera.fStop);
    cameraJson.insert(QStringLiteral("sensorWidthMillimeters"),
                      camera.sensorWidthMillimeters);
    cameraJson.insert(QStringLiteral("outputWidthPixels"), combined.width());
    cameraJson.insert(QStringLiteral("outputHeightPixels"), combined.height());
    cameraJson.insert(QStringLiteral("outputFormat"), outputFormat);
    cameraJson.insert(QStringLiteral("depthPass"),
                      QStringLiteral("linear camera-space distance; zero is background"));
    QJsonObject poseJson;
    poseJson.insert(QStringLiteral("perspective"), camera.view.perspective);
    poseJson.insert(QStringLiteral("preset"), int(camera.view.preset));
    poseJson.insert(QStringLiteral("zoom"), camera.view.zoom);
    poseJson.insert(QStringLiteral("panX"), camera.view.panX);
    poseJson.insert(QStringLiteral("panY"), camera.view.panY);
    poseJson.insert(QStringLiteral("orbitPivotX"), camera.view.orbitPivotX);
    poseJson.insert(QStringLiteral("orbitPivotY"), camera.view.orbitPivotY);
    poseJson.insert(QStringLiteral("orbitPivotZ"), camera.view.orbitPivotZ);
    poseJson.insert(QStringLiteral("yawRadians"), camera.view.yawRadians);
    poseJson.insert(QStringLiteral("pitchRadians"), camera.view.pitchRadians);
    poseJson.insert(QStringLiteral("gridViewDistance"),
                    camera.view.gridViewDistance);
    poseJson.insert(QStringLiteral("targetX"), camera.view.targetX);
    poseJson.insert(QStringLiteral("targetY"), camera.view.targetY);
    poseJson.insert(QStringLiteral("targetZ"), camera.view.targetZ);
    poseJson.insert(QStringLiteral("orientationW"), camera.view.orientationW);
    poseJson.insert(QStringLiteral("orientationX"), camera.view.orientationX);
    poseJson.insert(QStringLiteral("orientationY"), camera.view.orientationY);
    poseJson.insert(QStringLiteral("orientationZ"), camera.view.orientationZ);
    poseJson.insert(QStringLiteral("hasOrientation"), camera.view.hasOrientation);
    cameraJson.insert(QStringLiteral("viewPose"), poseJson);
    output.setText(QStringLiteral("classiCAD.RenderType"),
                   QStringLiteral("WorkbenchOffscreenScene"));
    output.setText(QStringLiteral("classiCAD.Camera"),
                   QString::fromUtf8(QJsonDocument(cameraJson).toJson(
                       QJsonDocument::Compact)));

    if (!output.save(path, "PNG")) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Could not write PNG render result: %1")
                                .arg(path);
        }
        return false;
    }
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    return true;
}

bool WorkbenchRenderResult::saveDepthPfm(const QString &path,
                                         QString *errorMessage) const
{
    if (!isValid() || depth.size() != combined.width() * combined.height()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("The depth render pass is empty");
        }
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Could not write depth pass: %1")
                                .arg(file.errorString());
        }
        return false;
    }
    const QByteArray header = QStringLiteral("Pf\n%1 %2\n-1.0\n")
                                  .arg(combined.width())
                                  .arg(combined.height()).toLatin1();
    if (file.write(header) != header.size()) {
        if (errorMessage != nullptr) {
            *errorMessage = file.errorString();
        }
        return false;
    }
    for (int y = combined.height() - 1; y >= 0; --y) {
        const float *row = depth.constData() +
                           qsizetype(y) * combined.width();
        const qint64 bytes = qint64(combined.width()) * qint64(sizeof(float));
        if (file.write(reinterpret_cast<const char *>(row), bytes) != bytes) {
            if (errorMessage != nullptr) {
                *errorMessage = file.errorString();
            }
            return false;
        }
    }
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    return true;
}

} // namespace classiCAD
