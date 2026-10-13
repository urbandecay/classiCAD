#include "ui/viewport/workbench_render_result.h"

#include <QDebug>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImageWriter>
#include <QTemporaryDir>

#include <algorithm>

using namespace classiCAD;

namespace {

bool check(bool condition, const char *message)
{
    if (!condition) qCritical("%s", message);
    return condition;
}

WorkbenchRenderResult makeRenderResult()
{
    WorkbenchRenderResult result;
    result.combined = QImage(2, 2, QImage::Format_RGBA64);
    result.combined.fill(QColor(100, 120, 140, 255));
    result.sceneLinearSize = result.combined.size();
    result.sceneLinearRgba = QVector<float>(16, 0.0f);
    for (qsizetype pixel = 0; pixel < 4; ++pixel) {
        result.sceneLinearRgba[pixel * 4] = 0.1f;
        result.sceneLinearRgba[pixel * 4 + 1] = 0.2f;
        result.sceneLinearRgba[pixel * 4 + 2] = 0.3f;
        result.sceneLinearRgba[pixel * 4 + 3] = 1.0f;
    }
    return result;
}

} // namespace

int main(int argc, char *argv[])
{
    QGuiApplication application(argc, argv);
    bool passed = true;
    QTemporaryDir outputDirectory;
    passed &= check(outputDirectory.isValid(),
                    "format contract must have a temporary output directory");

    WorkbenchRenderResult result = makeRenderResult();
    const auto rejectsUnsupportedFormat =
        [&result, &outputDirectory](const QString &format,
                                    const QString &fileName) {
            result.blenderImageFormat = format;
            const QString path = outputDirectory.filePath(fileName);
            QString error;
            const bool rejected = !result.saveCombinedImage(path, &error);
            return rejected && !error.isEmpty() &&
                   result.combinedFileExtension().isEmpty() &&
                   result.writtenColorDepth() == 0 &&
                   !QFileInfo::exists(path);
        };
    passed &= check(
        rejectsUnsupportedFormat(QStringLiteral("OPEN_EXR_MULTILAYER"),
                                 QStringLiteral("multilayer.png")) &&
            rejectsUnsupportedFormat(QStringLiteral("FUTURE_FORMAT"),
                                     QStringLiteral("unknown.png")),
        "unsupported Blender formats must fail explicitly instead of writing PNG under a requested Blender format");

    const QVector<QPair<QString, QString>> expectedExtensions{
        {QStringLiteral("PNG"), QStringLiteral("png")},
        {QStringLiteral("JPEG"), QStringLiteral("jpg")},
        {QStringLiteral("BMP"), QStringLiteral("bmp")},
        {QStringLiteral("TARGA"), QStringLiteral("tga")},
        {QStringLiteral("TARGA_RAW"), QStringLiteral("tga")},
        {QStringLiteral("HDR"), QStringLiteral("hdr")}};
    bool supportedExtensionsMatch = true;
    for (const auto &expected : expectedExtensions) {
        result.blenderImageFormat = expected.first;
        supportedExtensionsMatch &=
            result.combinedImageFormatError().isEmpty() &&
            result.combinedFileExtension() == expected.second;
    }
    passed &= check(supportedExtensionsMatch,
                    "supported PNG, JPEG, BMP, Targa, and HDR writers must resolve to matching output extensions");

    const QList<QByteArray> writerFormats = QImageWriter::supportedImageFormats();
    const bool hasTiffWriter = std::any_of(
        writerFormats.cbegin(), writerFormats.cend(),
        [](const QByteArray &format) {
            return format.compare("tiff", Qt::CaseInsensitive) == 0;
        });
    result.blenderImageFormat = QStringLiteral("TIFF");
    if (hasTiffWriter) {
        passed &= check(result.combinedFileExtension() == QStringLiteral("tif"),
                        "available Qt TIFF writer must resolve to a TIFF extension");
    } else {
        const QString path = outputDirectory.filePath(
            QStringLiteral("unavailable-tiff.png"));
        QString error;
        passed &= check(result.combinedFileExtension().isEmpty() &&
                            !result.saveCombinedImage(path, &error) &&
                            error.contains(QStringLiteral("TIFF")) &&
                            !QFileInfo::exists(path),
                        "unavailable Qt TIFF writer must report an error and must not substitute PNG");
    }

    result.blenderImageFormat = QStringLiteral("OPEN_EXR");
    if (WorkbenchRenderResult::openExrWriterAvailable()) {
        const QString path = outputDirectory.filePath(
            QStringLiteral("available-openexr.exr"));
        QString error;
        passed &= check(result.combinedFileExtension() == QStringLiteral("exr") &&
                            result.saveCombinedImage(path, &error) &&
                            QFileInfo(path).size() > 32,
                        "available OpenEXR writer must save Combined output as EXR");
    } else {
        const QString path = outputDirectory.filePath(
            QStringLiteral("unavailable-openexr.png"));
        QString error;
        passed &= check(result.combinedFileExtension().isEmpty() &&
                            !result.saveCombinedImage(path, &error) &&
                            error.contains(QStringLiteral("OpenEXR")) &&
                            !QFileInfo::exists(path),
                        "unavailable OpenEXR support must report an error and must not substitute PNG");
    }

    result.blenderImageFormat = QStringLiteral("PNG");
    const QString wrongSuffixPath = outputDirectory.filePath(
        QStringLiteral("png-request.jpg"));
    QString wrongSuffixError;
    passed &= check(!result.saveCombinedImage(wrongSuffixPath,
                                              &wrongSuffixError) &&
                        wrongSuffixError.contains(QStringLiteral(".png")) &&
                        !QFileInfo::exists(wrongSuffixPath),
                    "Combined writer must reject a filename suffix that disagrees with the selected Blender format");

    result.blenderImageFormat = QStringLiteral("HDR");
    const QString hdrPath = outputDirectory.filePath(QStringLiteral("linear.hdr"));
    QString hdrError;
    passed &= check(result.saveCombinedImage(hdrPath, &hdrError) &&
                        QFileInfo(hdrPath).size() > 32,
                    "supported Radiance HDR writer must save the scene-linear Combined image");
    return passed ? 0 : 1;
}
