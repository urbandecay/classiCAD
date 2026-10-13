/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "workbench_render_result.h"

#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QFile>
#include <QImageWriter>
#include <QPainter>
#include <QSysInfo>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <vector>

#ifdef CLASSICAD_HAS_OPENEXR
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfCompression.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfOutputFile.h>
#include <OpenEXR/ImfRgba.h>
#include <OpenEXR/OpenEXRConfig.h>
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
        writer->setQuality(quality <= 0 ? 75 : std::min(quality, 100));
    } else if (writerFormat == QLatin1String("png")) {
        const int compressionLevel = std::clamp(
            int(float(std::clamp(compression, 0, 100)) / 11.1111f), 0, 9);
        writer->setCompression(compressionLevel);
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

bool saveRadianceHdr(const QString &path, const QVector<float> &rgba,
                     const QSize &size, QString *errorMessage)
{
    const qsizetype pixelCount = qsizetype(size.width()) * size.height();
    if (size.isEmpty() || pixelCount <= 0 ||
        pixelCount > std::numeric_limits<qsizetype>::max() / 4 ||
        rgba.size() != pixelCount * 4) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral(
                "The scene-linear Combined pass is empty or has invalid dimensions");
        }
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (errorMessage != nullptr) *errorMessage = file.errorString();
        return false;
    }
    QByteArray header("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y ");
    header += QByteArray::number(size.height());
    header += " +X ";
    header += QByteArray::number(size.width());
    header += '\n';
    if (file.write(header) != header.size()) {
        if (errorMessage != nullptr) *errorMessage = file.errorString();
        return false;
    }

    struct RgbePixel {
        std::uint8_t channel[4] = {0, 0, 0, 0};
    };
    const auto encodePixel = [](const float *source) {
        RgbePixel result;
        const auto nonnegativeFinite = [](float value) {
            return std::isfinite(value) ? std::max(value, 0.0f) : 0.0f;
        };
        float red = nonnegativeFinite(source[0]);
        float green = nonnegativeFinite(source[1]);
        float blue = nonnegativeFinite(source[2]);
        const float maximumRepresentable = std::nextafter(
            std::ldexp(1.0f, 127), 0.0f);
        red = std::min(red, maximumRepresentable);
        green = std::min(green, maximumRepresentable);
        blue = std::min(blue, maximumRepresentable);
        const float maximum = std::max({red, green, blue});
        if (maximum < 1.0e-32f) return result;

        // RGBE's exponent byte tops out at 255, representing values below
        // 2^127. Saturate larger float inputs to that representable range.
        int exponent = 0;
        const float mantissa = std::frexp(maximum, &exponent);
        const float scale = mantissa * 256.0f / maximum;
        const auto toByte = [scale, &nonnegativeFinite](float value) {
            return static_cast<std::uint8_t>(std::clamp(
                int(nonnegativeFinite(value) * scale), 0, 255));
        };
        result.channel[0] = toByte(red);
        result.channel[1] = toByte(green);
        result.channel[2] = toByte(blue);
        result.channel[3] = static_cast<std::uint8_t>(exponent + 128);
        return result;
    };

    const int width = size.width();
    const bool useRle = width >= 8 && width <= 32767;
    std::vector<RgbePixel> scanline(static_cast<size_t>(width));
    std::vector<std::uint8_t> channel(static_cast<size_t>(width));
    for (int y = 0; y < size.height(); ++y) {
        const float *source = rgba.constData() + qsizetype(y) * width * 4;
        for (int x = 0; x < width; ++x) {
            scanline[size_t(x)] = encodePixel(source + size_t(x) * 4);
        }
        if (!useRle) {
            const char *bytes = reinterpret_cast<const char *>(scanline.data());
            if (file.write(bytes, qint64(scanline.size() * sizeof(RgbePixel))) !=
                qint64(scanline.size() * sizeof(RgbePixel))) {
                if (errorMessage != nullptr) *errorMessage = file.errorString();
                return false;
            }
            continue;
        }

        const char scanlineHeader[4] = {
            2, 2, char((width >> 8) & 0xff), char(width & 0xff)};
        if (file.write(scanlineHeader, sizeof(scanlineHeader)) !=
            qint64(sizeof(scanlineHeader))) {
            if (errorMessage != nullptr) *errorMessage = file.errorString();
            return false;
        }
        for (int component = 0; component < 4; ++component) {
            for (int x = 0; x < width; ++x) {
                channel[size_t(x)] = scanline[size_t(x)].channel[component];
            }
            int position = 0;
            while (position < width) {
                int runLength = 1;
                while (position + runLength < width && runLength < 127 &&
                       channel[size_t(position + runLength)] ==
                           channel[size_t(position)]) {
                    ++runLength;
                }
                if (runLength >= 4) {
                    const char packet[2] = {
                        char(128 + runLength), char(channel[size_t(position)])};
                    if (file.write(packet, sizeof(packet)) !=
                        qint64(sizeof(packet))) {
                        if (errorMessage != nullptr)
                            *errorMessage = file.errorString();
                        return false;
                    }
                    position += runLength;
                    continue;
                }

                const int literalStart = position;
                position += runLength;
                while (position < width && position - literalStart < 128) {
                    runLength = 1;
                    while (position + runLength < width && runLength < 127 &&
                           channel[size_t(position + runLength)] ==
                               channel[size_t(position)]) {
                        ++runLength;
                    }
                    if (runLength >= 4) break;
                    position += std::min(runLength,
                                         128 - (position - literalStart));
                }
                const char count = char(position - literalStart);
                if (file.write(&count, 1) != 1 ||
                    file.write(reinterpret_cast<const char *>(
                                   channel.data() + literalStart),
                               position - literalStart) != position - literalStart) {
                    if (errorMessage != nullptr)
                        *errorMessage = file.errorString();
                    return false;
                }
            }
        }
    }
    if (errorMessage != nullptr) errorMessage->clear();
    return true;
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

bool saveTarga(const QString &path, const QImage &source, const QString &colorMode,
               bool useRle, QString *errorMessage)
{
    if (source.isNull() || source.width() > 65535 || source.height() > 65535) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral(
                "Targa output requires a nonempty image no larger than 65535 pixels per side");
        }
        return false;
    }

    const QString mode = colorMode.toUpper();
    const bool grayscale = mode == QLatin1String("BW");
    const bool alpha = mode != QLatin1String("RGB") && !grayscale;
    const int bytesPerPixel = grayscale ? 1 : (alpha ? 4 : 3);
    const QImage image = source.convertToFormat(
        grayscale ? QImage::Format_Grayscale8
                  : (alpha ? QImage::Format_RGBA8888 : QImage::Format_RGB888));

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorMessage != nullptr) *errorMessage = file.errorString();
        return false;
    }

    // TGA stores a little-endian 18-byte header. Bit 5 marks top-left origin;
    // the low descriptor bits report the eight alpha bits in RGBA mode.
    const std::uint8_t imageType = grayscale
        ? (useRle ? 11 : 3) : (useRle ? 10 : 2);
    const std::uint8_t descriptor = std::uint8_t(0x20 | (alpha ? 8 : 0));
    const std::uint8_t header[18] = {
        0, 0, imageType, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        std::uint8_t(image.width() & 0xff),
        std::uint8_t((image.width() >> 8) & 0xff),
        std::uint8_t(image.height() & 0xff),
        std::uint8_t((image.height() >> 8) & 0xff),
        std::uint8_t(bytesPerPixel * 8), descriptor};
    if (file.write(reinterpret_cast<const char *>(header), sizeof(header)) !=
        qint64(sizeof(header))) {
        if (errorMessage != nullptr) *errorMessage = file.errorString();
        return false;
    }

    const auto writePixels = [&file, bytesPerPixel](const std::uint8_t *pixels,
                                                    int pixelCount,
                                                    bool rle) {
        const auto pixel = [bytesPerPixel](const std::uint8_t *data, int index) {
            return data + qsizetype(index) * bytesPerPixel;
        };
        if (!rle) {
            return file.write(reinterpret_cast<const char *>(pixels),
                              qint64(pixelCount) * bytesPerPixel) ==
                   qint64(pixelCount) * bytesPerPixel;
        }
        int position = 0;
        while (position < pixelCount) {
            int runLength = 1;
            while (position + runLength < pixelCount && runLength < 128 &&
                   std::memcmp(pixel(pixels, position),
                               pixel(pixels, position + runLength),
                               size_t(bytesPerPixel)) == 0) {
                ++runLength;
            }
            if (runLength >= 2) {
                const std::uint8_t packetHeader =
                    std::uint8_t(0x80 | (runLength - 1));
                if (file.write(reinterpret_cast<const char *>(&packetHeader), 1) != 1 ||
                    file.write(reinterpret_cast<const char *>(pixel(pixels, position)),
                               bytesPerPixel) != bytesPerPixel) {
                    return false;
                }
                position += runLength;
                continue;
            }

            const int rawStart = position++;
            while (position < pixelCount && position - rawStart < 128) {
                runLength = 1;
                while (position + runLength < pixelCount && runLength < 128 &&
                       std::memcmp(pixel(pixels, position),
                                   pixel(pixels, position + runLength),
                                   size_t(bytesPerPixel)) == 0) {
                    ++runLength;
                }
                if (runLength >= 2) break;
                ++position;
            }
            const std::uint8_t packetHeader =
                std::uint8_t(position - rawStart - 1);
            const qint64 byteCount =
                qint64(position - rawStart) * bytesPerPixel;
            if (file.write(reinterpret_cast<const char *>(&packetHeader), 1) != 1 ||
                file.write(reinterpret_cast<const char *>(pixel(pixels, rawStart)),
                           byteCount) != byteCount) {
                return false;
            }
        }
        return true;
    };

    std::vector<std::uint8_t> row(size_t(image.width()) * bytesPerPixel);
    for (int y = 0; y < image.height(); ++y) {
        const auto *input = image.constScanLine(y);
        if (grayscale) {
            std::copy_n(input, row.size(), row.data());
        } else if (alpha) {
            for (int x = 0; x < image.width(); ++x) {
                const auto *rgba = input + x * 4;
                auto *bgra = row.data() + size_t(x) * 4;
                bgra[0] = rgba[2];
                bgra[1] = rgba[1];
                bgra[2] = rgba[0];
                bgra[3] = rgba[3];
            }
        } else {
            for (int x = 0; x < image.width(); ++x) {
                const auto *rgb = input + x * 3;
                auto *bgr = row.data() + size_t(x) * 3;
                bgr[0] = rgb[2];
                bgr[1] = rgb[1];
                bgr[2] = rgb[0];
            }
        }
        if (!writePixels(row.data(), image.width(), useRle)) {
            if (errorMessage != nullptr) *errorMessage = file.errorString();
            return false;
        }
    }
    if (errorMessage != nullptr) errorMessage->clear();
    return true;
}

} // namespace

bool compositeWorkbenchRenderAnnotations(
    QImage *displayCombined,
    const QImage &annotationLayer,
    QVector<float> *sceneLinearRgba,
    const QSize &sceneLinearSize)
{
    if (displayCombined == nullptr || displayCombined->isNull() ||
        annotationLayer.isNull() ||
        displayCombined->size() != annotationLayer.size()) {
        return false;
    }
    const bool hasSceneLinear = sceneLinearRgba != nullptr;
    if (hasSceneLinear &&
        (sceneLinearSize != annotationLayer.size() ||
         sceneLinearRgba->size() !=
             qsizetype(sceneLinearSize.width()) * sceneLinearSize.height() * 4)) {
        return false;
    }
    QPainter combinedPainter(displayCombined);
    if (!combinedPainter.isActive()) return false;
    combinedPainter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    combinedPainter.drawImage(0, 0, annotationLayer);
    combinedPainter.end();

    if (!hasSceneLinear) return true;
    QImage premultipliedAnnotations = annotationLayer;
    if (premultipliedAnnotations.format() !=
        QImage::Format_RGBA64_Premultiplied) {
        premultipliedAnnotations = premultipliedAnnotations.convertToFormat(
            QImage::Format_RGBA64_Premultiplied);
        if (premultipliedAnnotations.isNull()) return false;
    }
    const auto srgbToLinear = [](float value) {
        return value <= 0.04045f
                   ? value / 12.92f
                   : std::pow((value + 0.055f) / 1.055f, 2.4f);
    };
    for (int y = 0; y < sceneLinearSize.height(); ++y) {
        const auto *annotationRow = reinterpret_cast<const QRgba64 *>(
            premultipliedAnnotations.constScanLine(y));
        for (int x = 0; x < sceneLinearSize.width(); ++x) {
            const QRgba64 pixel = annotationRow[x];
            const float alpha = pixel.alpha() / 65535.0f;
            if (alpha <= 0.0f) continue;
            const qsizetype base =
                (qsizetype(y) * sceneLinearSize.width() + x) * 4;
            const float inverseAlpha = 1.0f - alpha;
            const float redSrgb =
                std::min(1.0f, pixel.red() / 65535.0f / alpha);
            const float greenSrgb =
                std::min(1.0f, pixel.green() / 65535.0f / alpha);
            const float blueSrgb =
                std::min(1.0f, pixel.blue() / 65535.0f / alpha);
            (*sceneLinearRgba)[base] = srgbToLinear(redSrgb) * alpha +
                (*sceneLinearRgba)[base] * inverseAlpha;
            (*sceneLinearRgba)[base + 1] = srgbToLinear(greenSrgb) * alpha +
                (*sceneLinearRgba)[base + 1] * inverseAlpha;
            (*sceneLinearRgba)[base + 2] = srgbToLinear(blueSrgb) * alpha +
                (*sceneLinearRgba)[base + 2] * inverseAlpha;
            (*sceneLinearRgba)[base + 3] = alpha +
                (*sceneLinearRgba)[base + 3] * inverseAlpha;
        }
    }
    return true;
}

bool WorkbenchRenderResult::openExrWriterAvailable() noexcept
{
#ifdef CLASSICAD_HAS_OPENEXR
    return true;
#else
    return false;
#endif
}

void WorkbenchRenderResult::discardUnavailablePassPayloads()
{
    if (outputSize.isEmpty()) outputSize = combined.size();
    if (!combinedPassAvailable) {
        combined = QImage();
        sceneLinearRgba.clear();
        sceneLinearSize = QSize();
    }
    if (!depthPassAvailable) depth.clear();
}

bool WorkbenchRenderResult::saveCombinedOpenExr(
    const QString &path, QString *errorMessage) const
{
    if (!combinedPassAvailable) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("The Blender view layer has its Combined pass disabled");
        return false;
    }
    if (!hasValidSceneLinearCombined()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("The scene-linear Combined pass is empty or has invalid dimensions");
        return false;
    }
#ifdef CLASSICAD_HAS_OPENEXR
    try {
        const int width = sceneLinearSize.width();
        const int height = sceneLinearSize.height();
        const bool halfPrecision = colorDepth == 16;
        OPENEXR_IMF_NAMESPACE::Header header(width, height);
        using namespace OPENEXR_IMF_NAMESPACE;
        const QString codec = exrCodec.toUpper();
        Compression compression = ZIP_COMPRESSION;
        if (codec == QLatin1String("NONE")) compression = NO_COMPRESSION;
        else if (codec == QLatin1String("PXR24")) compression = PXR24_COMPRESSION;
        else if (codec == QLatin1String("ZIP")) compression = ZIP_COMPRESSION;
        else if (codec == QLatin1String("PIZ")) compression = PIZ_COMPRESSION;
        else if (codec == QLatin1String("RLE")) compression = RLE_COMPRESSION;
        else if (codec == QLatin1String("ZIPS")) compression = ZIPS_COMPRESSION;
        else if (codec == QLatin1String("B44")) compression = B44_COMPRESSION;
        else if (codec == QLatin1String("B44A")) compression = B44A_COMPRESSION;
#if OPENEXR_VERSION_MAJOR > 2 || \
    (OPENEXR_VERSION_MAJOR == 2 && OPENEXR_VERSION_MINOR >= 2)
        else if (codec == QLatin1String("DWAA")) compression = DWAA_COMPRESSION;
        else if (codec == QLatin1String("DWAB")) compression = DWAB_COMPRESSION;
#endif
#if OPENEXR_VERSION_MAJOR > 3 || \
    (OPENEXR_VERSION_MAJOR == 3 && OPENEXR_VERSION_MINOR >= 4)
        else if (codec == QLatin1String("HTJ2K"))
            compression = HTJ2K256_COMPRESSION;
#endif
        header.compression() = compression;
        bool dwaCompression = false;
#if OPENEXR_VERSION_MAJOR > 2 || \
    (OPENEXR_VERSION_MAJOR == 2 && OPENEXR_VERSION_MINOR >= 2)
        dwaCompression = compression == DWAA_COMPRESSION ||
                         compression == DWAB_COMPRESSION;
#endif
        int dwaQuality = 0;
        if (dwaCompression) {
            const int quality = std::clamp(imageQuality, 0, 100);
            // Blender maps quality 100 to level 0 and quality 90 to 45.
            dwaQuality = ((100 - quality) * 45) / 10;
            header.dwaCompressionLevel() = static_cast<float>(dwaQuality);
        }
        if (std::isfinite(pixelAspectX) && std::isfinite(pixelAspectY) &&
            pixelAspectX > 0.0 && pixelAspectY > 0.0) {
            // Blender's OpenEXR writer stores ppm_y / ppm_x. The PNG/TIFF
            // path maps pixelAspectX/Y to dots-per-meter with the same
            // convention, so the resulting pixel width/height ratio is X/Y.
            header.pixelAspectRatio() =
                static_cast<float>(pixelAspectX / pixelAspectY);
        }
        const PixelType pixelType = halfPrecision ? HALF : FLOAT;
        const bool rgbOnly = colorMode.compare(QLatin1String("RGB"),
                                               Qt::CaseInsensitive) == 0;
        const bool bwOnly = colorMode.compare(QLatin1String("BW"),
                                              Qt::CaseInsensitive) == 0;
        const bool omitAlpha = rgbOnly || bwOnly;
        const auto luminance = [](const float *rgb) {
            // Blender's bundled default OCIO config uses these Rec.709 luma
            // coefficients. The scene-linear Combined RGB is already
            // premultiplied, matching Blender's alpha-under-black step.
            return 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
        };
        header.channels().insert("R", Channel(pixelType));
        header.channels().insert("G", Channel(pixelType));
        header.channels().insert("B", Channel(pixelType));
        if (!omitAlpha) {
            header.channels().insert("A", Channel(pixelType));
        }
        const bool includeDepth = hasSceneLinearDepthPass();
        if (includeDepth) {
            header.channels().insert("Z", Channel(pixelType));
        }
        FrameBuffer frameBuffer;
        std::vector<Rgba> halfColor;
        std::vector<Rgba> halfDepth;
        std::vector<float> rgbChannels;
        if (halfPrecision) {
            // Blender clamps float render channels to the finite HALF range
            // before converting them to OpenEXR half precision.
            float halfMaximum = 65504.0f;
            if (dwaCompression) {
                const int margin = dwaQuality <= 50
                    ? 2048 + 32 * dwaQuality
                    : 3624 + 128 * (dwaQuality - 50);
                halfMaximum -= static_cast<float>(margin);
            }
            const auto toHalfRange = [halfMaximum](float value) {
                return std::clamp(value, -halfMaximum, halfMaximum);
            };
            halfColor.resize(size_t(width) * size_t(height));
            for (size_t index = 0; index < halfColor.size(); ++index) {
                const float *source = sceneLinearRgba.constData() + index * 4;
                if (bwOnly) {
                    const float gray = toHalfRange(luminance(source));
                    halfColor[index] = Rgba(gray, gray, gray, 1.0f);
                } else {
                    halfColor[index] = Rgba(toHalfRange(source[0]),
                                            toHalfRange(source[1]),
                                            toHalfRange(source[2]),
                                            toHalfRange(source[3]));
                }
            }
            constexpr size_t componentStride = sizeof(Rgba) / 4;
            char *halfBase = reinterpret_cast<char *>(&halfColor.front().r);
            const size_t xStride = sizeof(Rgba);
            const size_t yStride = size_t(width) * xStride;
            frameBuffer.insert("R", Slice(HALF, halfBase, xStride, yStride));
            frameBuffer.insert("G", Slice(HALF, halfBase + componentStride,
                                          xStride, yStride));
            frameBuffer.insert("B", Slice(HALF, halfBase + 2 * componentStride,
                                          xStride, yStride));
            if (!omitAlpha) {
                frameBuffer.insert("A", Slice(HALF,
                                              halfBase + 3 * componentStride,
                                              xStride, yStride));
            }
            if (includeDepth) {
                halfDepth.resize(size_t(width) * size_t(height));
                for (size_t index = 0; index < halfDepth.size(); ++index) {
                    halfDepth[index].r = toHalfRange(depth.constData()[index]);
                }
                char *depthBase =
                    reinterpret_cast<char *>(&halfDepth.front().r);
                frameBuffer.insert(
                    "Z", Slice(HALF, depthBase, sizeof(Rgba),
                               size_t(width) * sizeof(Rgba)));
            }
        } else {
            if (omitAlpha) {
                rgbChannels.resize(size_t(width) * size_t(height) * 3);
                for (size_t index = 0; index < size_t(width) * size_t(height);
                     ++index) {
                    const float *source = sceneLinearRgba.constData() + index * 4;
                    float *destination = rgbChannels.data() + index * 3;
                    if (bwOnly) {
                        const float gray = luminance(source);
                        destination[0] = gray;
                        destination[1] = gray;
                        destination[2] = gray;
                    } else {
                        destination[0] = source[0];
                        destination[1] = source[1];
                        destination[2] = source[2];
                    }
                }
                char *base = reinterpret_cast<char *>(rgbChannels.data());
                const size_t xStride = 3 * sizeof(float);
                const size_t yStride = size_t(width) * xStride;
                frameBuffer.insert("R", Slice(FLOAT, base, xStride, yStride));
                frameBuffer.insert("G", Slice(FLOAT, base + sizeof(float),
                                              xStride, yStride));
                frameBuffer.insert("B", Slice(FLOAT, base + 2 * sizeof(float),
                                              xStride, yStride));
            } else {
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
            }
            if (includeDepth) {
                char *depthBase = reinterpret_cast<char *>(
                    const_cast<float *>(depth.constData()));
                const size_t depthXStride = sizeof(float);
                const size_t depthYStride = size_t(width) * depthXStride;
                frameBuffer.insert(
                    "Z", Slice(FLOAT, depthBase, depthXStride, depthYStride,
                               1, 1, 0.0));
            }
        }
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
    if (blenderImageFormat.compare(QLatin1String("TARGA"),
                                   Qt::CaseInsensitive) == 0 ||
        blenderImageFormat.compare(QLatin1String("TARGA_RAW"),
                                   Qt::CaseInsensitive) == 0) {
        return QStringLiteral("tga");
    }
    if (blenderImageFormat.compare(QLatin1String("OPEN_EXR"),
                                   Qt::CaseInsensitive) == 0) {
        return openExrWriterAvailable() ? QStringLiteral("exr")
                                        : QStringLiteral("png");
    }
    if (blenderImageFormat.compare(QLatin1String("HDR"),
                                   Qt::CaseInsensitive) == 0) {
        return QStringLiteral("hdr");
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
        return colorDepth == 16 ? 16 : 32;
    return colorDepth == 16 &&
                   (extension == QLatin1String("png") ||
                    extension == QLatin1String("tif"))
               ? 16 : 8;
}

bool WorkbenchRenderResult::saveCombinedImage(
    const QString &path, QString *errorMessage) const
{
    if (!combinedPassAvailable) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("The Blender view layer has its Combined pass disabled");
        return false;
    }
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
    if (blenderImageFormat.compare(QLatin1String("HDR"),
                                   Qt::CaseInsensitive) == 0) {
        return saveRadianceHdr(path, sceneLinearRgba, sceneLinearSize,
                               errorMessage);
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

    const bool targa = blenderImageFormat.compare(QLatin1String("TARGA"),
                                                   Qt::CaseInsensitive) == 0;
    const bool targaRaw = blenderImageFormat.compare(
        QLatin1String("TARGA_RAW"), Qt::CaseInsensitive) == 0;
    if (targa || targaRaw) {
        return saveTarga(path, output, mode, targa, errorMessage);
    }

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
    cameraJson.insert(QStringLiteral("opticalSensorSizeMillimeters"),
                      camera.opticalSensorSizeMillimeters);
    const QSize imageSize = outputSize.isEmpty() ? combined.size() : outputSize;
    cameraJson.insert(QStringLiteral("outputWidthPixels"), imageSize.width());
    cameraJson.insert(QStringLiteral("outputHeightPixels"), imageSize.height());
    cameraJson.insert(QStringLiteral("pixelAspectX"), pixelAspectX);
    cameraJson.insert(QStringLiteral("pixelAspectY"), pixelAspectY);
    cameraJson.insert(QStringLiteral("imageQuality"), imageQuality);
    cameraJson.insert(QStringLiteral("imageCompression"), imageCompression);
    cameraJson.insert(QStringLiteral("tiffCodec"), tiffCodec);
    if (writerFormat == QLatin1String("tiff")) {
        const QString requestedTiffCodec = tiffCodec.toUpper();
        const QString writtenTiffCodec =
            requestedTiffCodec == QLatin1String("LZW") ||
                    requestedTiffCodec == QLatin1String("PACKBITS")
                ? QStringLiteral("LZW")
                : QStringLiteral("NONE");
        cameraJson.insert(QStringLiteral("writtenTiffCodec"),
                          writtenTiffCodec);
    }
    cameraJson.insert(QStringLiteral("exrCodec"), exrCodec);
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
                      QStringLiteral("Blender Workbench view-Z; background is 1e10"));
    cameraJson.insert(QStringLiteral("depthPassAvailable"),
                      depthPassAvailable);
    cameraJson.insert(QStringLiteral("filmTransparent"), transparentFilm);
    cameraJson.insert(QStringLiteral("backgroundType"), backgroundType);
    cameraJson.insert(QStringLiteral("transparentBackground"),
                      transparentBackground);
    cameraJson.insert(QStringLiteral("sceneWorldBackgroundUsed"),
                      !transparentFilm && renderUsesSceneWorldBackground &&
                          sceneWorldColorAvailable);
    if (sceneWorldColorAvailable) {
        QJsonArray worldColor;
        worldColor.append(sceneWorldColorSceneLinear.x());
        worldColor.append(sceneWorldColorSceneLinear.y());
        worldColor.append(sceneWorldColorSceneLinear.z());
        cameraJson.insert(QStringLiteral("sceneWorldColorSceneLinear"),
                          worldColor);
    }
    cameraJson.insert(QStringLiteral("combinedPassAvailable"),
                      combinedPassAvailable);
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
    if (!hasDepthPass()) {
        if (errorMessage != nullptr) {
            *errorMessage = depthPassAvailable
                ? QStringLiteral("The depth render pass is empty")
                : QStringLiteral("The Blender view layer has its depth pass disabled");
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
    const QSize imageSize = outputSize.isEmpty() ? combined.size() : outputSize;
    const char *scale = QSysInfo::ByteOrder == QSysInfo::LittleEndian
                            ? "-1.0" : "1.0";
    const QByteArray header = QStringLiteral("Pf\n%1 %2\n%3\n")
                                  .arg(imageSize.width())
                                  .arg(imageSize.height())
                                  .arg(QString::fromLatin1(scale)).toLatin1();
    if (file.write(header) != header.size()) {
        if (errorMessage != nullptr) {
            *errorMessage = file.errorString();
        }
        return false;
    }
    for (int y = imageSize.height() - 1; y >= 0; --y) {
        const float *row = depth.constData() +
                           qsizetype(y) * imageSize.width();
        const qint64 bytes = qint64(imageSize.width()) * qint64(sizeof(float));
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
