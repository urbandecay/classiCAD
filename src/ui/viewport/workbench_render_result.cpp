/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "workbench_render_result.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QImageWriter>

#include <algorithm>

namespace classiCAD {

namespace {

QString qtWriterFormat(const QString &blenderFormat)
{
    const QString key = blenderFormat.toUpper();
    if (key == QLatin1String("PNG")) return QStringLiteral("png");
    if (key == QLatin1String("JPEG")) return QStringLiteral("jpeg");
    if (key == QLatin1String("BMP")) return QStringLiteral("bmp");
    if (key == QLatin1String("TIFF")) return QStringLiteral("tiff");
    if (key == QLatin1String("TARGA") || key == QLatin1String("TARGA_RAW"))
        return QStringLiteral("tga");
    if (key == QLatin1String("OPEN_EXR")) return QStringLiteral("exr");
    if (key == QLatin1String("HDR")) return QStringLiteral("hdr");
    return QStringLiteral("png");
}

QString fileExtension(const QString &writerFormat)
{
    if (writerFormat == QLatin1String("jpeg")) return QStringLiteral("jpg");
    if (writerFormat == QLatin1String("tiff")) return QStringLiteral("tif");
    return writerFormat;
}

} // namespace

QString WorkbenchRenderResult::combinedFileExtension() const
{
    QString format = qtWriterFormat(blenderImageFormat);
    const QList<QByteArray> supported = QImageWriter::supportedImageFormats();
    const QByteArray requested = format.toLatin1();
    if (std::none_of(supported.cbegin(), supported.cend(),
                     [&requested](const QByteArray &candidate) {
                         return candidate.compare(requested,
                                                  Qt::CaseInsensitive) == 0;
                     })) {
        format = QStringLiteral("png");
    }
    return fileExtension(format);
}

int WorkbenchRenderResult::writtenColorDepth() const
{
    const QString extension = combinedFileExtension();
    return colorDepth == 16 &&
                   (extension == QLatin1String("png") ||
                    extension == QLatin1String("tif"))
               ? 16 : 8;
}

bool WorkbenchRenderResult::saveCombinedImage(
    const QString &path, QString *errorMessage) const
{
    if (!isValid()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("The viewport render result is empty");
        return false;
    }
    QString writerFormat = qtWriterFormat(blenderImageFormat);
    const QString extension = combinedFileExtension();
    if (extension == QLatin1String("png") &&
        writerFormat != QLatin1String("png")) {
        writerFormat = QStringLiteral("png");
    }
    const QString mode = colorMode.toUpper();
    const int outputDepth = writtenColorDepth();
    const auto makeOutputImage = [this, &mode, outputDepth]() {
        QImage::Format format = QImage::Format_RGBA8888;
        if (outputDepth == 16) {
            if (mode == QLatin1String("BW"))
                format = QImage::Format_Grayscale16;
            else if (mode == QLatin1String("RGB"))
                format = QImage::Format_RGBX64;
            else
                format = QImage::Format_RGBA64;
        } else if (mode == QLatin1String("RGB")) {
            format = QImage::Format_RGB888;
        } else if (mode == QLatin1String("BW")) {
            format = QImage::Format_Grayscale8;
        }
        return combined.convertToFormat(format);
    };
    QImage output = makeOutputImage();

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
    cameraJson.insert(QStringLiteral("blenderImageFormat"), blenderImageFormat);
    cameraJson.insert(QStringLiteral("writtenImageFormat"), writerFormat);
    cameraJson.insert(QStringLiteral("colorMode"), mode);
    cameraJson.insert(QStringLiteral("sourceColorDepth"), colorDepth);
    cameraJson.insert(QStringLiteral("writtenColorDepth"), outputDepth);
    cameraJson.insert(QStringLiteral("renderPrecisionBits"), 8);
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

    QImageWriter writer(path, writerFormat.toLatin1());
    bool written = writer.write(output);
    QString writeError = writer.errorString();
    if (!written && outputDepth == 16) {
        output = combined.convertToFormat(
            mode == QLatin1String("RGB") ? QImage::Format_RGB888
            : mode == QLatin1String("BW") ? QImage::Format_Grayscale8
                                             : QImage::Format_RGBA8888);
        cameraJson.insert(QStringLiteral("writtenColorDepth"), 8);
        output.setText(QStringLiteral("classiCAD.RenderType"),
                       QStringLiteral("WorkbenchOffscreenScene"));
        output.setText(QStringLiteral("classiCAD.Camera"),
                       QString::fromUtf8(QJsonDocument(cameraJson).toJson(
                           QJsonDocument::Compact)));
        QImageWriter fallbackWriter(path, writerFormat.toLatin1());
        written = fallbackWriter.write(output);
        if (!written) writeError = fallbackWriter.errorString();
    }
    if (!written) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("Could not write combined render: %1")
                                .arg(writeError);
        return false;
    }
    if (errorMessage != nullptr) errorMessage->clear();
    return true;
}

bool WorkbenchRenderResult::savePng(const QString &path,
                                    QString *errorMessage) const
{
    return saveCombinedImage(path, errorMessage);
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
