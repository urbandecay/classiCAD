/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "workbench_render_result.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QImageWriter>
#include <QSysInfo>

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>

#ifdef CLASSICAD_HAS_OPENEXR
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfOutputFile.h>
#endif

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

void applyImageWriterSettings(QImageWriter *writer,
                              const QString &writerFormat,
                              const QString &blenderFormat,
                              int quality,
                              int compression,
                              const QString &tiffCodec)
{
    if (writer == nullptr) return;
    const QString key = blenderFormat.toUpper();
    if (writerFormat == QLatin1String("jpeg")) {
        writer->setQuality(std::clamp(quality, 0, 100));
    } else if (writerFormat == QLatin1String("png")) {
        writer->setCompression(std::clamp(compression, 0, 100));
    } else if (writerFormat == QLatin1String("tga") &&
               (key == QLatin1String("TARGA") ||
                key == QLatin1String("TARGA_RAW"))) {
        writer->setCompression(key == QLatin1String("TARGA") ? 1 : 0);
    } else if (writerFormat == QLatin1String("tiff")) {
        const QString codec = tiffCodec.toUpper();
        if (codec == QLatin1String("NONE")) writer->setCompression(0);
        else if (codec == QLatin1String("LZW")) writer->setCompression(1);
        else if (codec == QLatin1String("PACKBITS"))
            writer->setCompression(2);
    }
}

void applyPixelAspectMetadata(QImage *image, qreal aspectX, qreal aspectY)
{
    if (image == nullptr || image->isNull() || !std::isfinite(aspectX) ||
        !std::isfinite(aspectY) || aspectX <= 0.0 || aspectY <= 0.0) {
        return;
    }

    // QImage writers that support per-axis resolution metadata (including
    // PNG pHYs and TIFF resolution tags) encode non-square pixel aspect as
    // inverse pixel density. Keep the values moderate and their ratio equal
    // to Blender's pixel_aspect_x / pixel_aspect_y.
    constexpr qreal referenceDotsPerMeter = 10000.0;
    const qreal maximumAspect = std::max(aspectX, aspectY);
    const int dotsPerMeterX = std::max(
        1, qRound(referenceDotsPerMeter * aspectY / maximumAspect));
    const int dotsPerMeterY = std::max(
        1, qRound(referenceDotsPerMeter * aspectX / maximumAspect));
    image->setDotsPerMeterX(dotsPerMeterX);
    image->setDotsPerMeterY(dotsPerMeterY);
}

} // namespace

bool WorkbenchRenderResult::openExrWriterAvailable() noexcept
{
#ifdef CLASSICAD_HAS_OPENEXR
    return true;
#else
    return false;
#endif
}

bool WorkbenchRenderResult::saveCombinedOpenExr(
    const QString &path, QString *errorMessage) const
{
    if (!hasValidSceneLinearCombined()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("The scene-linear Combined pass is empty or has invalid dimensions");
        return false;
    }
#ifdef CLASSICAD_HAS_OPENEXR
    try {
        const int width = sceneLinearSize.width();
        const int height = sceneLinearSize.height();
        OPENEXR_IMF_NAMESPACE::Header header(width, height);
        using namespace OPENEXR_IMF_NAMESPACE;
        header.channels().insert("R", Channel(FLOAT));
        header.channels().insert("G", Channel(FLOAT));
        header.channels().insert("B", Channel(FLOAT));
        header.channels().insert("A", Channel(FLOAT));
        FrameBuffer frameBuffer;
        char *base = reinterpret_cast<char *>(
            const_cast<float *>(sceneLinearRgba.constData()));
        const size_t xStride = 4 * sizeof(float);
        const size_t yStride = size_t(width) * xStride;
        frameBuffer.insert("R", Slice(FLOAT, base, xStride, yStride, 1, 1, 0.0));
        frameBuffer.insert("G", Slice(FLOAT, base + sizeof(float), xStride,
                                      yStride, 1, 1, 0.0));
        frameBuffer.insert("B", Slice(FLOAT, base + 2 * sizeof(float), xStride,
                                      yStride, 1, 1, 0.0));
        frameBuffer.insert("A", Slice(FLOAT, base + 3 * sizeof(float), xStride,
                                      yStride, 1, 1, 0.0));
        const QByteArray encodedPath = QFile::encodeName(path);
        OutputFile output(encodedPath.constData(), header);
        output.setFrameBuffer(frameBuffer);
        output.writePixels(height);
    } catch (const std::exception &exception) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("Could not write OpenEXR Combined pass: %1")
                                .arg(QString::fromLocal8Bit(exception.what()));
        return false;
    }
    if (errorMessage != nullptr) errorMessage->clear();
    return true;
#else
    Q_UNUSED(path);
    if (errorMessage != nullptr)
        *errorMessage = QStringLiteral("OpenEXR support was not found when classiCAD was configured");
    return false;
#endif
}

QString WorkbenchRenderResult::combinedFileExtension() const
{
    if (blenderImageFormat.compare(QLatin1String("OPEN_EXR"),
                                   Qt::CaseInsensitive) == 0) {
        return openExrWriterAvailable() ? QStringLiteral("exr")
                                        : QStringLiteral("png");
    }
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
    if (extension == QLatin1String("exr") && openExrWriterAvailable())
        return 32;
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
    if (blenderImageFormat.compare(QLatin1String("OPEN_EXR"),
                                   Qt::CaseInsensitive) == 0 &&
        openExrWriterAvailable()) {
        return saveCombinedOpenExr(path, errorMessage);
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
    applyPixelAspectMetadata(&output, pixelAspectX, pixelAspectY);

    QJsonObject cameraJson;
    cameraJson.insert(QStringLiteral("focalLengthMillimeters"),
                      camera.focalLengthMillimeters);
    cameraJson.insert(QStringLiteral("clipStart"), camera.clipStart);
    cameraJson.insert(QStringLiteral("clipEnd"), camera.clipEnd);
    cameraJson.insert(QStringLiteral("sceneCameraEnabled"),
                      camera.sceneCameraEnabled);
    cameraJson.insert(QStringLiteral("focusDistance"), camera.focusDistance);
    cameraJson.insert(QStringLiteral("fStop"), camera.fStop);
    cameraJson.insert(QStringLiteral("apertureBlades"), camera.apertureBlades);
    cameraJson.insert(QStringLiteral("apertureRotationRadians"),
                      camera.apertureRotationRadians);
    cameraJson.insert(QStringLiteral("apertureRatio"), camera.apertureRatio);
    cameraJson.insert(QStringLiteral("sensorWidthMillimeters"),
                      camera.sensorWidthMillimeters);
    cameraJson.insert(QStringLiteral("outputWidthPixels"), combined.width());
    cameraJson.insert(QStringLiteral("outputHeightPixels"), combined.height());
    cameraJson.insert(QStringLiteral("pixelAspectX"), pixelAspectX);
    cameraJson.insert(QStringLiteral("pixelAspectY"), pixelAspectY);
    cameraJson.insert(QStringLiteral("imageQuality"), imageQuality);
    cameraJson.insert(QStringLiteral("imageCompression"), imageCompression);
    cameraJson.insert(QStringLiteral("tiffCodec"), tiffCodec);
    cameraJson.insert(QStringLiteral("displayDevice"), colorDisplayDevice);
    cameraJson.insert(QStringLiteral("viewTransform"), viewTransform);
    cameraJson.insert(QStringLiteral("look"), colorLook);
    if (!colorLutId.isEmpty())
        cameraJson.insert(QStringLiteral("lutId"), colorLutId);
    cameraJson.insert(QStringLiteral("exposure"), colorExposure);
    cameraJson.insert(QStringLiteral("gamma"), colorGamma);
    cameraJson.insert(QStringLiteral("colorManagementApplied"),
                      colorManagementApplied);
    cameraJson.insert(QStringLiteral("outputFormat"), outputFormat);
    cameraJson.insert(QStringLiteral("blenderImageFormat"), blenderImageFormat);
    cameraJson.insert(QStringLiteral("writtenImageFormat"), writerFormat);
    cameraJson.insert(QStringLiteral("colorMode"), mode);
    cameraJson.insert(QStringLiteral("sourceColorDepth"), colorDepth);
    cameraJson.insert(QStringLiteral("writtenColorDepth"), outputDepth);
    cameraJson.insert(QStringLiteral("renderPrecisionBits"), 16);
    cameraJson.insert(QStringLiteral("renderPrecision"),
                      QStringLiteral("display-referred normalized RGBA; no HDR range"));
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
    applyImageWriterSettings(&writer, writerFormat, blenderImageFormat, imageQuality,
                             imageCompression, tiffCodec);
    bool written = writer.write(output);
    QString writeError = writer.errorString();
    if (!written && outputDepth == 16) {
        output = combined.convertToFormat(
            mode == QLatin1String("RGB") ? QImage::Format_RGB888
            : mode == QLatin1String("BW") ? QImage::Format_Grayscale8
                                             : QImage::Format_RGBA8888);
        applyPixelAspectMetadata(&output, pixelAspectX, pixelAspectY);
        cameraJson.insert(QStringLiteral("writtenColorDepth"), 8);
        cameraJson.insert(QStringLiteral("renderPrecisionBits"), 16);
        output.setText(QStringLiteral("classiCAD.RenderType"),
                       QStringLiteral("WorkbenchOffscreenScene"));
        output.setText(QStringLiteral("classiCAD.Camera"),
                       QString::fromUtf8(QJsonDocument(cameraJson).toJson(
                           QJsonDocument::Compact)));
        QImageWriter fallbackWriter(path, writerFormat.toLatin1());
        applyImageWriterSettings(&fallbackWriter, writerFormat,
                                 blenderImageFormat,
                                 imageQuality, imageCompression, tiffCodec);
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
    // PFM uses the sign of its scale value to declare the byte order of the
    // following float samples. Write native floats and declare that order so
    // the depth pass remains valid on both little- and big-endian hosts.
    const char *scale = QSysInfo::ByteOrder == QSysInfo::LittleEndian
                            ? "-1.0" : "1.0";
    const QByteArray header = QStringLiteral("Pf\n%1 %2\n%3\n")
                                  .arg(combined.width())
                                  .arg(combined.height())
                                  .arg(QString::fromLatin1(scale)).toLatin1();
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
