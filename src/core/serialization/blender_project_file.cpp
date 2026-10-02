#include "blender_project_file.h"

#include "core/document/document.h"
#include "core/serialization/document_serializer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTemporaryFile>

#include <cmath>
#include <utility>

namespace {

constexpr auto kPinnedBlenderVersion = "5.2.2";
constexpr qint64 kBlenderTimeoutMilliseconds = 180000;
constexpr qint64 kCopyChunkSize = 1024 * 1024;

void setError(QString *errorMessage, const QString &message)
{
    if (errorMessage != nullptr) {
        *errorMessage = message;
    }
}

bool isValidProjectViewportCameraSettings(
    const classiCAD::ProjectViewportCameraSettings &settings)
{
    if (!std::isfinite(settings.focalLengthMillimeters) ||
        settings.focalLengthMillimeters < 1.0 ||
        settings.focalLengthMillimeters > 2000.0 ||
        !std::isfinite(settings.clipStart) || settings.clipStart < 0.000001 ||
        !std::isfinite(settings.clipEnd) || settings.clipEnd <= settings.clipStart ||
        settings.clipEnd > 1.0e9) {
        return false;
    }
    if (!settings.view.storedInProject) {
        return true;
    }

    const classiCAD::ProjectViewportViewState &view = settings.view;
    const double orientationLengthSquared =
        view.orientationW * view.orientationW +
        view.orientationX * view.orientationX +
        view.orientationY * view.orientationY +
        view.orientationZ * view.orientationZ;
    return std::isfinite(view.zoom) && view.zoom > 0.0 && view.zoom <= 1.0e12 &&
           std::isfinite(view.panX) && std::isfinite(view.panY) &&
           std::isfinite(view.orbitPivotX) && std::isfinite(view.orbitPivotY) &&
           std::isfinite(view.orbitPivotZ) && std::isfinite(view.yawRadians) &&
           std::isfinite(view.pitchRadians) && view.preset >= 0 && view.preset <= 8 &&
           std::isfinite(view.gridViewDistance) && view.gridViewDistance > 0.0 &&
           std::isfinite(view.orientationW) && std::isfinite(view.orientationX) &&
           std::isfinite(view.orientationY) && std::isfinite(view.orientationZ) &&
           (!view.hasOrientation ||
            (std::isfinite(orientationLengthSquared) &&
             orientationLengthSquared > 1.0e-12)) &&
           std::isfinite(view.targetX) && std::isfinite(view.targetY) &&
           std::isfinite(view.targetZ);
}

QJsonObject projectViewportCameraSettingsToJson(
    const classiCAD::ProjectViewportCameraSettings &settings)
{
    QJsonObject serialized;
    serialized.insert(QStringLiteral("focalLengthMillimeters"),
                      settings.focalLengthMillimeters);
    serialized.insert(QStringLiteral("clipStart"), settings.clipStart);
    serialized.insert(QStringLiteral("clipEnd"), settings.clipEnd);
    if (settings.view.storedInProject) {
        const classiCAD::ProjectViewportViewState &view = settings.view;
        QJsonObject viewState;
        viewState.insert(QStringLiteral("zoom"), view.zoom);
        viewState.insert(QStringLiteral("panX"), view.panX);
        viewState.insert(QStringLiteral("panY"), view.panY);
        viewState.insert(QStringLiteral("orbitPivotX"), view.orbitPivotX);
        viewState.insert(QStringLiteral("orbitPivotY"), view.orbitPivotY);
        viewState.insert(QStringLiteral("orbitPivotZ"), view.orbitPivotZ);
        viewState.insert(QStringLiteral("yawRadians"), view.yawRadians);
        viewState.insert(QStringLiteral("pitchRadians"), view.pitchRadians);
        viewState.insert(QStringLiteral("perspective"), view.perspective);
        viewState.insert(QStringLiteral("preset"), view.preset);
        viewState.insert(QStringLiteral("gridViewDistance"), view.gridViewDistance);
        viewState.insert(QStringLiteral("orientationW"), view.orientationW);
        viewState.insert(QStringLiteral("orientationX"), view.orientationX);
        viewState.insert(QStringLiteral("orientationY"), view.orientationY);
        viewState.insert(QStringLiteral("orientationZ"), view.orientationZ);
        viewState.insert(QStringLiteral("hasOrientation"), view.hasOrientation);
        viewState.insert(QStringLiteral("targetX"), view.targetX);
        viewState.insert(QStringLiteral("targetY"), view.targetY);
        viewState.insert(QStringLiteral("targetZ"), view.targetZ);
        serialized.insert(QStringLiteral("viewState"), viewState);
    }
    return serialized;
}

bool projectViewportCameraSettingsFromJson(
    const QJsonObject &serializedDocument,
    classiCAD::ProjectViewportCameraSettings *settings,
    QString *errorMessage)
{
    if (settings == nullptr) {
        setError(errorMessage, QStringLiteral("A destination for viewport settings is required"));
        return false;
    }

    *settings = classiCAD::ProjectViewportCameraSettings{};
    const QJsonValue cameraValue = serializedDocument.value(QStringLiteral("viewportCamera"));
    if (cameraValue.isUndefined()) {
        return true;
    }
    if (!cameraValue.isObject()) {
        setError(errorMessage, QStringLiteral("Project viewport camera settings are invalid"));
        return false;
    }

    const QJsonObject camera = cameraValue.toObject();
    const QJsonValue focalLength = camera.value(QStringLiteral("focalLengthMillimeters"));
    const QJsonValue clipStart = camera.value(QStringLiteral("clipStart"));
    const QJsonValue clipEnd = camera.value(QStringLiteral("clipEnd"));
    if (!focalLength.isDouble() || !clipStart.isDouble() || !clipEnd.isDouble()) {
        setError(errorMessage, QStringLiteral("Project viewport camera settings are incomplete"));
        return false;
    }

    settings->focalLengthMillimeters = focalLength.toDouble();
    settings->clipStart = clipStart.toDouble();
    settings->clipEnd = clipEnd.toDouble();
    settings->storedInProject = true;

    const QJsonValue viewValue = camera.value(QStringLiteral("viewState"));
    if (!viewValue.isUndefined()) {
        if (!viewValue.isObject()) {
            setError(errorMessage, QStringLiteral("Project viewport view state is invalid"));
            return false;
        }
        const QJsonObject view = viewValue.toObject();
        const auto readNumber = [&view](const QString &key, double *destination) {
            const QJsonValue value = view.value(key);
            if (!value.isDouble()) {
                return false;
            }
            *destination = value.toDouble();
            return true;
        };
        classiCAD::ProjectViewportViewState &restoredView = settings->view;
        const QJsonValue perspective = view.value(QStringLiteral("perspective"));
        const QJsonValue preset = view.value(QStringLiteral("preset"));
        const QJsonValue hasOrientation = view.value(QStringLiteral("hasOrientation"));
        if (!readNumber(QStringLiteral("zoom"), &restoredView.zoom) ||
            !readNumber(QStringLiteral("panX"), &restoredView.panX) ||
            !readNumber(QStringLiteral("panY"), &restoredView.panY) ||
            !readNumber(QStringLiteral("orbitPivotX"), &restoredView.orbitPivotX) ||
            !readNumber(QStringLiteral("orbitPivotY"), &restoredView.orbitPivotY) ||
            !readNumber(QStringLiteral("orbitPivotZ"), &restoredView.orbitPivotZ) ||
            !readNumber(QStringLiteral("yawRadians"), &restoredView.yawRadians) ||
            !readNumber(QStringLiteral("pitchRadians"), &restoredView.pitchRadians) ||
            !readNumber(QStringLiteral("gridViewDistance"),
                        &restoredView.gridViewDistance) ||
            !readNumber(QStringLiteral("orientationW"), &restoredView.orientationW) ||
            !readNumber(QStringLiteral("orientationX"), &restoredView.orientationX) ||
            !readNumber(QStringLiteral("orientationY"), &restoredView.orientationY) ||
            !readNumber(QStringLiteral("orientationZ"), &restoredView.orientationZ) ||
            !readNumber(QStringLiteral("targetX"), &restoredView.targetX) ||
            !readNumber(QStringLiteral("targetY"), &restoredView.targetY) ||
            !readNumber(QStringLiteral("targetZ"), &restoredView.targetZ) ||
            !perspective.isBool() || !preset.isDouble() ||
            !hasOrientation.isBool()) {
            setError(errorMessage, QStringLiteral("Project viewport view state is incomplete"));
            return false;
        }
        restoredView.perspective = perspective.toBool();
        restoredView.preset = preset.toInt(-1);
        restoredView.hasOrientation = hasOrientation.toBool();
        restoredView.storedInProject = true;
    }
    if (!isValidProjectViewportCameraSettings(*settings)) {
        setError(errorMessage, QStringLiteral("Project viewport settings are out of range"));
        return false;
    }
    return true;
}

QString blenderExecutable()
{
    const QString configuredPath =
        qEnvironmentVariable("CLASSICAD_BLENDER_EXECUTABLE").trimmed();
    if (!configuredPath.isEmpty()) {
        if (configuredPath.contains(QLatin1Char('/')) ||
            configuredPath.contains(QLatin1Char('\\'))) {
            return configuredPath;
        }
        return QStandardPaths::findExecutable(configuredPath);
    }
    return QStandardPaths::findExecutable(QStringLiteral("blender"));
}

bool writeFile(const QString &path,
               const QByteArray &contents,
               QString *errorMessage)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size()) {
        setError(errorMessage,
                 QStringLiteral("Could not write temporary Blender adapter data: %1")
                     .arg(file.errorString()));
        return false;
    }
    return true;
}

bool runBlenderAdapter(const QString &operation,
                       const QString &inputPath,
                       const QString &outputPath,
                       QString *errorMessage)
{
    const QString executable = blenderExecutable();
    if (executable.isEmpty()) {
        setError(errorMessage,
                 QStringLiteral("Blender %1 is required to read or write .vignola and .blend files. "
                                "Install that Blender version or set "
                                "CLASSICAD_BLENDER_EXECUTABLE.")
                     .arg(QLatin1String(kPinnedBlenderVersion)));
        return false;
    }

    QTemporaryDir adapterDirectory;
    if (!adapterDirectory.isValid()) {
        setError(errorMessage, QStringLiteral("Could not create a temporary Blender adapter directory"));
        return false;
    }

    QFile adapterResource(QStringLiteral(":/classiCAD/serialization/blender_project_adapter.py"));
    if (!adapterResource.open(QIODevice::ReadOnly)) {
        setError(errorMessage, QStringLiteral("The Blender project adapter is missing from this build"));
        return false;
    }
    const QString adapterPath = adapterDirectory.filePath(QStringLiteral("blender_project_adapter.py"));
    if (!writeFile(adapterPath, adapterResource.readAll(), errorMessage)) {
        return false;
    }

    QProcess process;
    process.setProgram(executable);
    process.setArguments({QStringLiteral("--background"),
                          QStringLiteral("--factory-startup"),
                          QStringLiteral("--python"),
                          adapterPath,
                          QStringLiteral("--"),
                          operation,
                          inputPath,
                          outputPath});
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start();
    if (!process.waitForStarted(10000)) {
        setError(errorMessage,
                 QStringLiteral("Could not start Blender %1: %2")
                     .arg(QLatin1String(kPinnedBlenderVersion), process.errorString()));
        return false;
    }
    if (!process.waitForFinished(kBlenderTimeoutMilliseconds)) {
        process.kill();
        process.waitForFinished();
        setError(errorMessage,
                 QStringLiteral("Blender took too long to %1 the project")
                     .arg(operation == QLatin1String("save")
                              ? QStringLiteral("save")
                              : QStringLiteral("open")));
        return false;
    }

    const QByteArray standardOutput = process.readAllStandardOutput();
    const QByteArray standardError = process.readAllStandardError();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        QByteArray diagnostic = standardError.trimmed();
        if (diagnostic.isEmpty()) {
            diagnostic = standardOutput.trimmed();
        }
        if (diagnostic.size() > 6000) {
            diagnostic = diagnostic.right(6000);
        }
        const QString details = QString::fromUtf8(diagnostic).trimmed();
        setError(errorMessage,
                 details.isEmpty()
                     ? QStringLiteral("Blender %1 could not %2 the project")
                           .arg(QLatin1String(kPinnedBlenderVersion), operation)
                     : QStringLiteral("Blender %1 could not %2 the project:\n%3")
                           .arg(QLatin1String(kPinnedBlenderVersion), operation, details));
        return false;
    }
    return true;
}

bool hasBlenderFileHeader(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray header = file.read(7);
    // Blender 5.2 uses the normal BLENDER header for uncompressed files and
    // Zstandard's frame magic when it writes a compressed .blend file.
    return header == QByteArrayLiteral("BLENDER") ||
           header.startsWith(QByteArray::fromHex("28b52ffd"));
}

bool commitStagedFile(const QString &stagedPath,
                      const QString &destinationPath,
                      QString *errorMessage)
{
    QFile stagedFile(stagedPath);
    if (!stagedFile.open(QIODevice::ReadOnly)) {
        setError(errorMessage,
                 QStringLiteral("Could not read the Blender project after saving: %1")
                     .arg(stagedFile.errorString()));
        return false;
    }

    QSaveFile destination(destinationPath);
    destination.setDirectWriteFallback(false);
    if (!destination.open(QIODevice::WriteOnly)) {
        setError(errorMessage,
                 QStringLiteral("Could not open the project file for saving: %1")
                     .arg(destination.errorString()));
        return false;
    }

    while (!stagedFile.atEnd()) {
        const QByteArray chunk = stagedFile.read(kCopyChunkSize);
        if (chunk.isEmpty() && stagedFile.error() != QFileDevice::NoError) {
            destination.cancelWriting();
            setError(errorMessage,
                     QStringLiteral("Could not read the staged Blender project: %1")
                         .arg(stagedFile.errorString()));
            return false;
        }
        if (!chunk.isEmpty() && destination.write(chunk) != chunk.size()) {
            destination.cancelWriting();
            setError(errorMessage,
                     QStringLiteral("Could not write the project file: %1")
                         .arg(destination.errorString()));
            return false;
        }
    }

    if (!destination.commit()) {
        setError(errorMessage,
                 QStringLiteral("Could not finish saving the project file: %1")
                     .arg(destination.errorString()));
        return false;
    }
    return true;
}

} // namespace

namespace classiCAD {

bool saveVignolaDocument(const QString &path,
                         const Document &document,
                         QString *errorMessage)
{
    return saveVignolaDocument(path,
                               document,
                               ProjectViewportCameraSettings{},
                               errorMessage);
}

bool saveVignolaDocument(const QString &path,
                         const Document &document,
                         const ProjectViewportCameraSettings &cameraSettings,
                         QString *errorMessage)
{
    if (path.trimmed().isEmpty()) {
        setError(errorMessage, QStringLiteral("A project file path is required"));
        return false;
    }
    const QString suffix = QFileInfo(path).suffix();
    if (suffix.compare(QStringLiteral("vignola"), Qt::CaseInsensitive) != 0 &&
        suffix.compare(QStringLiteral("blend"), Qt::CaseInsensitive) != 0) {
        setError(errorMessage,
                 QStringLiteral("Project files must use the .vignola or .blend extension"));
        return false;
    }

    const QFileInfo destinationInfo(path);
    if (!destinationInfo.dir().exists()) {
        setError(errorMessage, QStringLiteral("The project file directory does not exist"));
        return false;
    }

    QTemporaryDir temporaryDirectory;
    if (!temporaryDirectory.isValid()) {
        setError(errorMessage, QStringLiteral("Could not create temporary project data"));
        return false;
    }
    if (!isValidProjectViewportCameraSettings(cameraSettings)) {
        setError(errorMessage, QStringLiteral("Viewport camera settings are invalid"));
        return false;
    }
    const QString documentPath = temporaryDirectory.filePath(QStringLiteral("document.json"));
    QJsonObject serializedDocument = documentToJson(document);
    serializedDocument.insert(QStringLiteral("viewportCamera"),
                              projectViewportCameraSettingsToJson(cameraSettings));
    const QByteArray documentBytes =
        QJsonDocument(serializedDocument).toJson(QJsonDocument::Compact);
    if (!writeFile(documentPath, documentBytes, errorMessage)) {
        return false;
    }

    QTemporaryFile stagedFile(destinationInfo.dir().filePath(
        QStringLiteral(".vignola-save-XXXXXX")));
    stagedFile.setAutoRemove(false);
    if (!stagedFile.open()) {
        setError(errorMessage,
                 QStringLiteral("Could not create a temporary project file: %1")
                     .arg(stagedFile.errorString()));
        return false;
    }
    const QString stagedPath = stagedFile.fileName();
    stagedFile.close();
    if (!QFile::remove(stagedPath)) {
        setError(errorMessage, QStringLiteral("Could not prepare the temporary Blender project file"));
        return false;
    }

    if (!runBlenderAdapter(QStringLiteral("save"),
                           documentPath,
                           stagedPath,
                           errorMessage)) {
        QFile::remove(stagedPath);
        return false;
    }
    if (!hasBlenderFileHeader(stagedPath)) {
        QFile::remove(stagedPath);
        setError(errorMessage,
                 QStringLiteral("Blender did not produce a native project file"));
        return false;
    }

    const bool committed = commitStagedFile(stagedPath, path, errorMessage);
    QFile::remove(stagedPath);
    return committed;
}

bool loadVignolaDocument(const QString &path,
                         Document *document,
                         QString *errorMessage)
{
    return loadVignolaDocument(path,
                               document,
                               static_cast<ProjectViewportCameraSettings *>(nullptr),
                               errorMessage);
}

bool loadVignolaDocument(const QString &path,
                         Document *document,
                         ProjectViewportCameraSettings *cameraSettings,
                         QString *errorMessage)
{
    if (document == nullptr || path.trimmed().isEmpty()) {
        setError(errorMessage, QStringLiteral("A destination document and project file path are required"));
        return false;
    }
    if (!hasBlenderFileHeader(path)) {
        setError(errorMessage,
                 QStringLiteral("This is not a Blender-backed classiCAD project. "
                                "Rhino .3dm import is a separate operation; old 3DM-backed "
                                ".vignola files are not migrated."));
        return false;
    }

    QTemporaryDir temporaryDirectory;
    if (!temporaryDirectory.isValid()) {
        setError(errorMessage, QStringLiteral("Could not create temporary project data"));
        return false;
    }
    const QString documentPath = temporaryDirectory.filePath(QStringLiteral("document.json"));
    if (!runBlenderAdapter(QStringLiteral("load"), path, documentPath, errorMessage)) {
        return false;
    }

    QFile documentFile(documentPath);
    if (!documentFile.open(QIODevice::ReadOnly)) {
        setError(errorMessage, QStringLiteral("Blender did not return classiCAD document data"));
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument serialized = QJsonDocument::fromJson(documentFile.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !serialized.isObject()) {
        setError(errorMessage,
                 QStringLiteral("The classiCAD data inside the Blender file is invalid: %1")
                     .arg(parseError.errorString()));
        return false;
    }

    ProjectViewportCameraSettings restoredCameraSettings;
    if (!projectViewportCameraSettingsFromJson(serialized.object(),
                                               &restoredCameraSettings,
                                               errorMessage)) {
        return false;
    }

    Document restored;
    QString documentError;
    if (!documentFromJson(serialized.object(), &restored, &documentError)) {
        setError(errorMessage,
                 QStringLiteral("The classiCAD document could not be restored: %1")
                     .arg(documentError));
        return false;
    }
    *document = std::move(restored);
    if (cameraSettings != nullptr) {
        *cameraSettings = restoredCameraSettings;
    }
    return true;
}

} // namespace classiCAD
