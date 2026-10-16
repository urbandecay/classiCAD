#include "blender_project_file.h"

#include "core/document/document.h"
#include "core/document/collection_id.h"
#include "core/document/document_collection.h"
#include "core/geometry/geometry_type.h"
#include "core/geometry/nurbs_surface_evaluator.h"
#include "core/geometry/nurbs_surface_tessellator.h"
#include "core/geometry/polygon_mesh.h"
#include "core/geometry/work_plane.h"
#include "core/serialization/document_serializer.h"

#include <QDir>
#include <QBuffer>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QProcess>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace {

constexpr auto kPinnedBlenderVersion = "5.2.2";
constexpr qint64 kBlenderTimeoutMilliseconds = 180000;
constexpr qint64 kCopyChunkSize = 1024 * 1024;
constexpr qint64 kBlenderExtractionJsonByteLimit = 256LL * 1024 * 1024;
constexpr qsizetype kColorLutTotalByteLimit = 32 * 1024 * 1024;

bool isFiniteColorLutPayload(const QByteArray &bytes, int size)
{
    if (size < 2 || size > 140 ||
        bytes.size() != qsizetype(size) * size * size * 3 * 4) return false;
    const auto *source = reinterpret_cast<const uchar *>(bytes.constData());
    for (qsizetype offset = 0; offset < bytes.size();
         offset += static_cast<qsizetype>(sizeof(quint32))) {
        const quint32 bits = qFromLittleEndian<quint32>(source + offset);
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        if (!std::isfinite(value)) return false;
    }
    return true;
}

bool decodeFiniteSceneLinearFloatPayload(const QJsonValue &encodedValue,
                                         const QJsonValue &widthValue,
                                         const QJsonValue &heightValue,
                                         QByteArray *decoded,
                                         int *widthOut = nullptr,
                                         int *heightOut = nullptr)
{
    if (decoded == nullptr || !encodedValue.isString() ||
        !widthValue.isDouble() || !heightValue.isDouble()) return false;
    const QString encoded = encodedValue.toString();
    const double widthNumber = widthValue.toDouble();
    const double heightNumber = heightValue.toDouble();
    if (!std::isfinite(widthNumber) || !std::isfinite(heightNumber) ||
        std::floor(widthNumber) != widthNumber ||
        std::floor(heightNumber) != heightNumber ||
        widthNumber < 1.0 || heightNumber < 1.0 ||
        widthNumber > 16384.0 || heightNumber > 16384.0 ||
        encoded.size() > 44 * 1024 * 1024) return false;
    const int width = static_cast<int>(widthNumber);
    const int height = static_cast<int>(heightNumber);
    const quint64 expected = quint64(width) * quint64(height) * 16ULL;
    if (expected > 32ULL * 1024 * 1024) return false;
    const QByteArray bytes = QByteArray::fromBase64(
        encoded.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (quint64(bytes.size()) != expected) return false;
    const auto *source = reinterpret_cast<const uchar *>(bytes.constData());
    for (qsizetype offset = 0; offset < bytes.size(); offset += 4) {
        const quint32 bits = qFromLittleEndian<quint32>(source + offset);
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        if (!std::isfinite(value)) return false;
    }
    *decoded = bytes;
    if (widthOut != nullptr) *widthOut = width;
    if (heightOut != nullptr) *heightOut = height;
    return true;
}

void setError(QString *errorMessage, const QString &message)
{
    if (errorMessage != nullptr) {
        *errorMessage = message;
    }
}

bool addSurfaceDisplayMeshes(const classiCAD::Document &document,
                             QJsonObject *serializedDocument,
                             QString *errorMessage)
{
    if (serializedDocument == nullptr) {
        setError(errorMessage,
                 QStringLiteral("Could not attach derived surface display meshes"));
        return false;
    }
    const auto displaySurfaceCount = std::count_if(
        document.objects().cbegin(), document.objects().cend(),
        [](const classiCAD::SceneObject &sceneObject) {
            return sceneObject.geometry.geometryType == classiCAD::GeometryType::NurbsSurface ||
                   sceneObject.geometry.geometryType == classiCAD::GeometryType::NurbsSolid;
        });
    classiCAD::PreparedNurbsSurfaceTessellation::Options displayOptions;
    if (displaySurfaceCount >= 256) {
        // These meshes are Blender display proxies only; the exact NURBS
        // remains in the document snapshot. Keep large-project save payloads
        // bounded instead of serializing a 48-by-48 grid for every face.
        displayOptions.gridCount = 24;
        displayOptions.trimBoundaryDepth = 3;
        displayOptions.isocurveCount = 1;
        displayOptions.isocurveSamples = 1;
        displayOptions.trimSamples = 96;
    }
    QJsonObject displayMeshes;
    for (const classiCAD::SceneObject &sceneObject : document.objects()) {
        if (sceneObject.geometry.geometryType != classiCAD::GeometryType::NurbsSurface &&
            sceneObject.geometry.geometryType != classiCAD::GeometryType::NurbsSolid) {
            continue;
        }
        QJsonArray vertices;
        QJsonArray faces;
        QJsonArray cornerUvs;
        QJsonArray solidFaceIds;
        int surfaceIndex = 0;
        for (const auto &surface : classiCAD::shapeSurfaceFaces(sceneObject.geometry)) {
            classiCAD::PreparedNurbsSurfaceTessellation tessellation;
            if (!tessellation.prepare(surface, displayOptions)) {
                setError(errorMessage,
                         QStringLiteral("Could not tessellate NURBS surface object %1 for display")
                             .arg(sceneObject.id.value()));
                return false;
            }
            classiCAD::PreparedNurbsSurfaceEvaluator evaluator;
            qreal uStart = 0.0;
            qreal uEnd = 0.0;
            qreal vStart = 0.0;
            qreal vEnd = 0.0;
            const QVector<QPointF> &textureParameters =
                tessellation.vertexTextureParameters();
            if (!evaluator.prepare(surface) ||
                !evaluator.parameterDomains(&uStart, &uEnd, &vStart, &vEnd) ||
                textureParameters.size() != tessellation.vertices().size() ||
                uEnd <= uStart || vEnd <= vStart) {
                setError(errorMessage,
                         QStringLiteral("Could not prepare NURBS proxy texture coordinates for object %1")
                             .arg(sceneObject.id.value()));
                return false;
            }
            const int vertexOffset = vertices.size();
            const bool reversed = sceneObject.geometry.geometryType == classiCAD::GeometryType::NurbsSolid &&
                classiCAD::nurbsSolidFaceReversed(sceneObject.geometry.nurbsSolid, surfaceIndex);
            for (const classiCAD::Point3D &point : tessellation.vertices()) {
                QJsonArray vertex;
                vertex.append(point.x + sceneObject.placementTranslation.x);
                vertex.append(point.y + sceneObject.placementTranslation.y);
                vertex.append(point.z + sceneObject.placementTranslation.z);
                vertices.append(vertex);
            }
            for (const classiCAD::PreparedNurbsSurfaceTessellation::Triangle &triangle :
                 tessellation.triangles()) {
                QJsonArray face;
                QJsonArray uvFace;
                const int orderedVertices[] = {
                    triangle[0], triangle[reversed ? 2 : 1],
                    triangle[reversed ? 1 : 2]};
                for (const int vertexIndex : orderedVertices) {
                    face.append(vertexOffset + vertexIndex);
                    const QPointF parameter = textureParameters[vertexIndex];
                    QJsonArray uv;
                    uv.append((parameter.x() - uStart) / (uEnd - uStart));
                    uv.append((parameter.y() - vStart) / (vEnd - vStart));
                    uvFace.append(uv);
                }
                faces.append(face);
                cornerUvs.append(uvFace);
                if (sceneObject.geometry.geometryType == classiCAD::GeometryType::NurbsSolid) {
                    const auto &solid = sceneObject.geometry.nurbsSolid;
                    QString semanticFaceId;
                    if (!solid.boundaryFaces.isEmpty()) {
                        semanticFaceId.clear();
                    } else if (surfaceIndex == 0) {
                        semanticFaceId = solid.baseCapFaceId;
                    } else if (surfaceIndex == 1) {
                        semanticFaceId = solid.displacedCapFaceId;
                    } else {
                        semanticFaceId = solid.wallFaceIds.value(surfaceIndex - 2);
                    }
                    solidFaceIds.append(semanticFaceId);
                } else {
                    solidFaceIds.append(QString());
                }
            }
            ++surfaceIndex;
        }
        QJsonObject mesh;
        mesh.insert(QStringLiteral("vertices"), vertices);
        mesh.insert(QStringLiteral("faces"), faces);
        mesh.insert(QStringLiteral("cornerUvs"), cornerUvs);
        mesh.insert(QStringLiteral("solidFaceIds"), solidFaceIds);
        displayMeshes.insert(QString::number(sceneObject.id.value()), mesh);
    }
    serializedDocument->insert(QStringLiteral("_surfaceDisplayMeshes"),
                               displayMeshes);
    return true;
}

bool isValidProjectViewportCameraSettings(
    const classiCAD::ProjectViewportCameraSettings &settings)
{
    if (!std::isfinite(settings.focalLengthMillimeters) ||
        settings.focalLengthMillimeters < 1.0 ||
        settings.focalLengthMillimeters > 2000.0 ||
        !std::isfinite(settings.clipStart) || settings.clipStart < 0.000001 ||
        !std::isfinite(settings.clipEnd) || settings.clipEnd <= settings.clipStart ||
        settings.clipEnd > 1.0e9 || !std::isfinite(settings.focusDistance) ||
        settings.focusDistance <= 0.0 || settings.focusDistance > 1.0e9 ||
        !std::isfinite(settings.fStop) || settings.fStop < 0.1 ||
        settings.fStop > 128.0 ||
        settings.apertureBlades < 0 || settings.apertureBlades > 16 ||
        !std::isfinite(settings.apertureRotationRadians) ||
        settings.apertureRotationRadians < -3.141592653589793 ||
        settings.apertureRotationRadians > 3.141592653589793 ||
        !std::isfinite(settings.apertureRatio) || settings.apertureRatio < 0.01 ||
        settings.apertureRatio > 1.0e6 ||
        !std::isfinite(settings.sensorWidthMillimeters) ||
        settings.sensorWidthMillimeters < 1.0 ||
        settings.sensorWidthMillimeters > 100.0 ||
        !std::isfinite(settings.opticalSensorSizeMillimeters) ||
        settings.opticalSensorSizeMillimeters < 1.0 ||
        settings.opticalSensorSizeMillimeters > 100.0) {
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
    serialized.insert(QStringLiteral("sceneCameraEnabled"),
                      settings.sceneCameraEnabled);
    serialized.insert(QStringLiteral("focusDistance"), settings.focusDistance);
    serialized.insert(QStringLiteral("fStop"), settings.fStop);
    serialized.insert(QStringLiteral("apertureBlades"), settings.apertureBlades);
    serialized.insert(QStringLiteral("apertureRotationRadians"),
                      settings.apertureRotationRadians);
    serialized.insert(QStringLiteral("apertureRatio"), settings.apertureRatio);
    serialized.insert(QStringLiteral("sensorWidthMillimeters"),
                      settings.sensorWidthMillimeters);
    serialized.insert(QStringLiteral("opticalSensorSizeMillimeters"),
                      settings.opticalSensorSizeMillimeters);
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
    const QJsonValue sceneCameraEnabled = camera.value(QStringLiteral("sceneCameraEnabled"));
    if (!sceneCameraEnabled.isUndefined() && !sceneCameraEnabled.isBool()) {
        setError(errorMessage, QStringLiteral("Project scene camera state is invalid"));
        return false;
    }
    settings->sceneCameraEnabled = sceneCameraEnabled.toBool(false);
    settings->focusDistance = camera.value(QStringLiteral("focusDistance")).toDouble(10000.0);
    settings->fStop = camera.value(QStringLiteral("fStop")).toDouble(2.8);
    settings->apertureBlades = camera.value(QStringLiteral("apertureBlades")).toInt(0);
    settings->apertureRotationRadians = camera.value(
        QStringLiteral("apertureRotationRadians")).toDouble(0.0);
    settings->apertureRatio = camera.value(QStringLiteral("apertureRatio")).toDouble(1.0);
    settings->sensorWidthMillimeters = camera.value(QStringLiteral("sensorWidthMillimeters")).toDouble(36.0);
    settings->opticalSensorSizeMillimeters = camera.value(
        QStringLiteral("opticalSensorSizeMillimeters"))
        .toDouble(settings->sensorWidthMillimeters);
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
        process.waitForFinished(5000);
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

bool runBlenderSceneExtractor(const QString &inputPath,
                              const QString &outputPath,
                              const QString &mode,
                              QString *errorMessage)
{
    const QString executable = blenderExecutable();
    if (executable.isEmpty()) {
        setError(errorMessage,
                 QStringLiteral("Blender %1 is required to import Blender scenes. "
                                "Install that version or set CLASSICAD_BLENDER_EXECUTABLE.")
                     .arg(QLatin1String(kPinnedBlenderVersion)));
        return false;
    }

    QTemporaryDir scriptDirectory;
    if (!scriptDirectory.isValid()) {
        setError(errorMessage,
                 QStringLiteral("Could not create a temporary Blender extraction directory"));
        return false;
    }
    QFile scriptResource(
        QStringLiteral(":/classiCAD/serialization/blender_scene_extract.py"));
    if (!scriptResource.open(QIODevice::ReadOnly)) {
        setError(errorMessage,
                 QStringLiteral("The Blender scene extraction script is missing from this build"));
        return false;
    }
    const QString scriptPath =
        scriptDirectory.filePath(QStringLiteral("blender_scene_extract.py"));
    if (!writeFile(scriptPath, scriptResource.readAll(), errorMessage)) {
        return false;
    }

    QProcess process;
    process.setProgram(executable);
    process.setArguments({QStringLiteral("--background"),
                          QStringLiteral("--factory-startup"),
                          QStringLiteral("--python"),
                          scriptPath,
                          QStringLiteral("--"),
                          QStringLiteral("extract"),
                          QFileInfo(inputPath).absoluteFilePath(),
                          outputPath,
                          mode});
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
        process.waitForFinished(5000);
        setError(errorMessage,
                 QStringLiteral("Blender took too long to extract this scene"));
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        QByteArray diagnostic = process.readAllStandardError().trimmed();
        if (diagnostic.isEmpty()) {
            diagnostic = process.readAllStandardOutput().trimmed();
        }
        if (diagnostic.size() > 6000) {
            diagnostic = diagnostic.right(6000);
        }
        const QString details = QString::fromUtf8(diagnostic).trimmed();
        setError(errorMessage,
                 details.isEmpty()
                     ? QStringLiteral("Blender %1 could not extract the scene")
                           .arg(QLatin1String(kPinnedBlenderVersion))
                     : QStringLiteral("Blender %1 could not extract the scene:\n%2")
                           .arg(QLatin1String(kPinnedBlenderVersion), details));
        return false;
    }
    return true;
}

bool jsonFiniteNumber(const QJsonValue &value, double *number)
{
    if (number == nullptr || !value.isDouble()) {
        return false;
    }
    const double parsed = value.toDouble();
    if (!std::isfinite(parsed)) {
        return false;
    }
    *number = parsed;
    return true;
}

bool jsonInteger(const QJsonValue &value, int *number)
{
    double parsed = 0.0;
    if (!jsonFiniteNumber(value, &parsed) ||
        std::floor(parsed) != parsed ||
        parsed < static_cast<double>(std::numeric_limits<int>::min()) ||
        parsed > static_cast<double>(std::numeric_limits<int>::max())) {
        return false;
    }
    *number = static_cast<int>(parsed);
    return true;
}

bool jsonNumberArray(const QJsonValue &value,
                     int requiredSize,
                     QVector<double> *numbers)
{
    if (numbers == nullptr || !value.isArray()) {
        return false;
    }
    const QJsonArray array = value.toArray();
    if (requiredSize >= 0 && array.size() != requiredSize) {
        return false;
    }
    QVector<double> parsed;
    parsed.reserve(array.size());
    for (const QJsonValue &entry : array) {
        double number = 0.0;
        if (!jsonFiniteNumber(entry, &number)) {
            return false;
        }
        parsed.append(number);
    }
    *numbers = std::move(parsed);
    return true;
}

bool parseDisplayBoundsData(const QJsonObject &item,
                            classiCAD::SceneObject *sceneObject,
                            QString *errorMessage)
{
    if (sceneObject == nullptr) return false;
    const QString minimumKey = QStringLiteral("bounds_min_local");
    const QString maximumKey = QStringLiteral("bounds_max_local");
    const QString transformKey = QStringLiteral("bounds_transform_world");
    const bool hasMinimum = item.contains(minimumKey);
    const bool hasMaximum = item.contains(maximumKey);
    const bool hasTransform = item.contains(transformKey);
    if (!hasMinimum && !hasMaximum && !hasTransform) return true;
    QVector<double> minimum;
    QVector<double> maximum;
    const QJsonArray transform = item.value(transformKey).toArray();
    if (!hasMinimum || !hasMaximum || !hasTransform || transform.size() != 4 ||
        !jsonNumberArray(item.value(minimumKey), 3, &minimum) ||
        !jsonNumberArray(item.value(maximumKey), 3, &maximum)) {
        setError(errorMessage,
                 QStringLiteral("object bounds display metadata is incomplete or invalid"));
        return false;
    }
    for (int axis = 0; axis < 3; ++axis) {
        if (minimum[axis] > maximum[axis]) {
            setError(errorMessage,
                     QStringLiteral("object bounds display extents are inverted"));
            return false;
        }
    }
    for (int row = 0; row < 4; ++row) {
        QVector<double> values;
        if (!jsonNumberArray(transform[row], 4, &values)) {
            setError(errorMessage,
                     QStringLiteral("object bounds display transform is invalid"));
            return false;
        }
        for (int column = 0; column < 4; ++column) {
            sceneObject->displayBoundsTransformWorld[
                static_cast<size_t>(row * 4 + column)] = values[column];
        }
    }
    const auto &matrix = sceneObject->displayBoundsTransformWorld;
    if (std::abs(matrix[12]) > 1.0e-9 || std::abs(matrix[13]) > 1.0e-9 ||
        std::abs(matrix[14]) > 1.0e-9 || std::abs(matrix[15] - 1.0) > 1.0e-9) {
        setError(errorMessage,
                 QStringLiteral("object bounds display transform is not affine"));
        return false;
    }
    sceneObject->displayBoundsMinimumLocal =
        {minimum[0], minimum[1], minimum[2]};
    sceneObject->displayBoundsMaximumLocal =
        {maximum[0], maximum[1], maximum[2]};
    sceneObject->hasDisplayBoundsData = true;
    return true;
}

QString blenderItemLabel(const QJsonObject &item)
{
    const QString name = item.value(QStringLiteral("name")).toString();
    return name.isEmpty() ? item.value(QStringLiteral("object_id")).toString()
                          : name;
}

bool parseBlenderMaterials(
    const QJsonObject &sourceMaterials,
    QVector<classiCAD::DocumentMaterial> *destination,
    QHash<QString, classiCAD::DocumentMaterial> *byId,
    QStringList *warnings)
{
    if (destination == nullptr || byId == nullptr) {
        return false;
    }
    QVector<classiCAD::DocumentMaterial> parsedMaterials;
    QHash<QString, classiCAD::DocumentMaterial> parsedById;
    QSet<QByteArray> sceneFloatPayloadIds;
    quint64 sceneFloatPayloadBytes = 0;
    const auto addSceneFloatPayload = [&](const QByteArray &decoded) {
        const QByteArray id = QCryptographicHash::hash(decoded, QCryptographicHash::Sha256);
        if (sceneFloatPayloadIds.contains(id)) return true;
        if (sceneFloatPayloadBytes + quint64(decoded.size()) > 64ULL * 1024 * 1024)
            return false;
        sceneFloatPayloadIds.insert(id);
        sceneFloatPayloadBytes += quint64(decoded.size());
        return true;
    };
    for (const QString &key : sourceMaterials.keys()) {
        const QJsonObject source = sourceMaterials.value(key).toObject();
        classiCAD::DocumentMaterial material;
        material.id = source.value(QStringLiteral("material_id")).toString();
        material.name = source.value(QStringLiteral("name")).toString();
        material.libraryPath = source.value(QStringLiteral("library_path")).toString();
        QVector<double> color;
        double roughness = 0.0;
        double metallic = 0.0;
        double alpha = 0.0;
        QString materialError;
        if (material.id != key || material.id.isEmpty() ||
            !jsonNumberArray(source.value(QStringLiteral("diffuse_color_linear")), 4,
                             &color) ||
            !jsonFiniteNumber(source.value(QStringLiteral("roughness")), &roughness) ||
            !jsonFiniteNumber(source.value(QStringLiteral("metallic")), &metallic) ||
            !jsonFiniteNumber(source.value(QStringLiteral("alpha")), &alpha)) {
            materialError = QStringLiteral("invalid required datablock identity or material values");
        } else {
            material.baseColorSceneLinear = {
                static_cast<float>(color[0]), static_cast<float>(color[1]),
                static_cast<float>(color[2]), static_cast<float>(color[3])};
            material.roughness = static_cast<float>(roughness);
            material.metallic = static_cast<float>(metallic);
            material.alpha = static_cast<float>(alpha);
            const QJsonArray imageTextures = source.value(QStringLiteral("image_textures")).toArray();
            for (const QJsonValue &imageValue : imageTextures) {
                if (!imageValue.isObject()) continue;
                const QJsonObject image = imageValue.toObject();
                classiCAD::DocumentImageTextureReference reference;
                reference.nodeName = image.value(QStringLiteral("node_name")).toString();
                reference.nodeType = image.value(QStringLiteral("node_type"))
                                         .toString(QStringLiteral("TEX_IMAGE"));
                reference.imageName = image.value(QStringLiteral("image_name")).toString();
                reference.imageLibraryPath = image.value(QStringLiteral("image_library_path")).toString();
                reference.filepath = image.value(QStringLiteral("filepath")).toString();
                reference.colorSpace = image.value(QStringLiteral("color_space")).toString();
                reference.sourceTransformId = image.value(
                    QStringLiteral("source_transform_id")).toString(QStringLiteral("identity"));
                reference.alphaMode = image.value(QStringLiteral("alpha_mode"))
                                          .toString(QStringLiteral("STRAIGHT"));
                reference.interpolation = image.value(QStringLiteral("interpolation"))
                                              .toString(QStringLiteral("Linear"));
                reference.extension = image.value(QStringLiteral("extension"))
                                          .toString(QStringLiteral("REPEAT"));
                reference.packed = image.value(QStringLiteral("packed")).toBool();
                reference.packedDataBase64 = image.value(QStringLiteral("packed_data_base64"))
                                                 .toString();
                if (reference.packedDataBase64.size() > 44 * 1024 * 1024 ||
                    (!reference.packedDataBase64.isEmpty() && !reference.packed)) {
                    if (warnings != nullptr) {
                        warnings->append(QStringLiteral("%1: packed image data is invalid or exceeds the size limit")
                                              .arg(reference.imageName));
                    }
                    return false;
                }
                if (QByteArray::fromBase64(reference.packedDataBase64.toLatin1()).size() >
                    32 * 1024 * 1024) {
                    if (warnings != nullptr) {
                        warnings->append(QStringLiteral("%1: packed image data exceeds the size limit")
                                              .arg(reference.imageName));
                    }
                    return false;
                }
                const QJsonValue floatPayloadValue = image.value(
                    QStringLiteral("scene_linear_float_data_base64"));
                if (!floatPayloadValue.isUndefined() && !floatPayloadValue.isString()) {
                    if (warnings != nullptr) warnings->append(
                        QStringLiteral("%1: malformed scene-linear float image payload was omitted")
                            .arg(reference.imageName));
                }
                reference.sceneLinearFloatDataBase64 = floatPayloadValue.toString();
                const QJsonValue floatWidthValue = image.value(
                    QStringLiteral("scene_linear_float_width"));
                const QJsonValue floatHeightValue = image.value(
                    QStringLiteral("scene_linear_float_height"));
                if (!reference.sceneLinearFloatDataBase64.isEmpty()) {
                    QByteArray decoded;
                    if (!decodeFiniteSceneLinearFloatPayload(
                            image.value(QStringLiteral("scene_linear_float_data_base64")),
                            floatWidthValue, floatHeightValue, &decoded,
                            &reference.sceneLinearFloatWidth,
                            &reference.sceneLinearFloatHeight) ||
                        (reference.sourceTransformId.size() != 64 &&
                         reference.sourceTransformId != QStringLiteral("blender-float-buffer")) ||
                        !addSceneFloatPayload(decoded)) {
                        if (warnings != nullptr) warnings->append(
                            QStringLiteral("%1: invalid scene-linear float image payload was omitted")
                                .arg(reference.imageName));
                        reference.sceneLinearFloatDataBase64.clear();
                        reference.sceneLinearFloatWidth = 0;
                        reference.sceneLinearFloatHeight = 0;
                    }
                }
                reference.sourceFileExists = image.value(QStringLiteral("source_file_exists")).toBool();
                const QJsonArray tiles = image.value(QStringLiteral("tiles")).toArray();
                qsizetype packedTileBytes = 0;
                quint64 floatTileBytes = 0;
                for (const QJsonValue &tileValue : tiles) {
                    if (!tileValue.isObject()) continue;
                    const QJsonObject tileObject = tileValue.toObject();
                    const QJsonValue numberValue = tileObject.value(QStringLiteral("number"));
                    const QJsonValue filepathValue = tileObject.value(QStringLiteral("filepath"));
                    const QJsonValue existsValue = tileObject.value(QStringLiteral("source_file_exists"));
                    const QJsonValue packedDataValue = tileObject.value(
                        QStringLiteral("packed_data_base64"));
                    const QString packedData = packedDataValue.toString();
                    if (!numberValue.isDouble() || !filepathValue.isString() ||
                        !existsValue.isBool()) {
                        if (warnings != nullptr) {
                            warnings->append(QStringLiteral("%1: invalid tiled image entry")
                                                 .arg(reference.imageName));
                        }
                        return false;
                    }
                    const double numberNumber = numberValue.toDouble(-1.0);
                    const int number = numberValue.isDouble() &&
                            std::isfinite(numberNumber) &&
                            std::floor(numberNumber) == numberNumber &&
                            numberNumber >= 1001.0 && numberNumber <= 1999.0
                        ? static_cast<int>(numberNumber) : -1;
                    if (number < 1001 || number > 1999) {
                        if (warnings != nullptr) {
                            warnings->append(QStringLiteral("%1: invalid UDIM tile number")
                                                 .arg(reference.imageName));
                        }
                        return false;
                    }
                    QString validPackedData;
                    if (!packedDataValue.isUndefined() && !packedDataValue.isString()) {
                        if (warnings != nullptr) warnings->append(
                            QStringLiteral("%1: malformed packed UDIM tile %2 was omitted")
                                .arg(reference.imageName).arg(number));
                    } else if (packedData.size() > 44 * 1024 * 1024) {
                        if (warnings != nullptr) warnings->append(
                            QStringLiteral("%1: packed UDIM tile %2 exceeds the encoded payload limit and was omitted")
                                .arg(reference.imageName).arg(number));
                    } else if (!packedData.isEmpty()) {
                        const QByteArray decoded = QByteArray::fromBase64(
                            packedData.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
                        if (decoded.isEmpty() || decoded.size() > 32 * 1024 * 1024 ||
                            packedTileBytes + decoded.size() > 32 * 1024 * 1024) {
                            if (warnings != nullptr) warnings->append(
                                QStringLiteral("%1: packed UDIM tile %2 is malformed or exceeds the 32 MiB per-image limit and was omitted")
                                    .arg(reference.imageName).arg(number));
                        } else {
                            packedTileBytes += decoded.size();
                            validPackedData = packedData;
                        }
                    }
                    classiCAD::DocumentImageTextureTile parsedTile{
                        number, filepathValue.toString(), existsValue.toBool(),
                        validPackedData, {}, 0, 0};
                    const QJsonValue floatPayloadValue = tileObject.value(
                        QStringLiteral("scene_linear_float_data_base64"));
                    const QString floatPayload = floatPayloadValue.toString();
                    const QJsonValue floatWidthValue = tileObject.value(
                        QStringLiteral("scene_linear_float_width"));
                    const QJsonValue floatHeightValue = tileObject.value(
                        QStringLiteral("scene_linear_float_height"));
                    if (!floatPayload.isEmpty()) {
                        QByteArray decoded;
                        int floatWidth = 0;
                        int floatHeight = 0;
                        if (!decodeFiniteSceneLinearFloatPayload(
                                tileObject.value(QStringLiteral("scene_linear_float_data_base64")),
                                floatWidthValue, floatHeightValue, &decoded,
                                &floatWidth, &floatHeight) ||
                            floatTileBytes + quint64(decoded.size()) > 32ULL * 1024 * 1024 ||
                            !addSceneFloatPayload(decoded)) {
                            if (warnings != nullptr) warnings->append(
                                QStringLiteral("%1: invalid scene-linear float payload for tile %2 was omitted")
                                    .arg(reference.imageName).arg(number));
                        } else {
                            floatTileBytes += quint64(decoded.size());
                            parsedTile.sceneLinearFloatDataBase64 = floatPayload;
                            parsedTile.sceneLinearFloatWidth = floatWidth;
                            parsedTile.sceneLinearFloatHeight = floatHeight;
                        }
                    } else if (!floatPayloadValue.isUndefined() &&
                               !floatPayloadValue.isString() && warnings != nullptr) {
                        warnings->append(
                            QStringLiteral("%1: malformed scene-linear float payload for tile %2 was omitted")
                                .arg(reference.imageName).arg(number));
                    }
                    reference.tiles.append(std::move(parsedTile));
                }
                if (!reference.imageName.isEmpty()) material.imageTextures.append(std::move(reference));
            }
            if (!classiCAD::isValidDocumentMaterial(material)) {
                materialError = QStringLiteral("material values are outside the supported range");
            }
        }
        if (!materialError.isEmpty()) {
            if (warnings != nullptr) {
                warnings->append(QStringLiteral("Material %1 was omitted: %2")
                                     .arg(key, materialError));
            }
            continue;
        }
        parsedById.insert(material.id, material);
        parsedMaterials.append(std::move(material));
    }
    *destination = std::move(parsedMaterials);
    *byId = std::move(parsedById);
    return true;
}

bool parseBlenderMeshItem(const QJsonObject &item,
                          const QHash<QString, classiCAD::CollectionId> &collectionIds,
                          const QHash<QString, classiCAD::DocumentMaterial> &materials,
                          const QString &blenderVersion,
                          const QString &importMode,
                          classiCAD::SceneObject *sceneObject,
                          QStringList *warnings,
                          QString *errorMessage)
{
    if (sceneObject == nullptr) {
        return false;
    }
    classiCAD::SceneObject result;
    classiCAD::Shape shape;
    shape.geometryType = classiCAD::GeometryType::PolygonMesh;
    result.geometry = shape;
    result.displayName = item.value(QStringLiteral("name")).toString();
    result.libraryPath = item.value(QStringLiteral("library_path")).toString();
    result.workbenchRandomLibraryPath =
        item.value(QStringLiteral("workbench_random_library_path")).toString();
    result.blenderSourceVersion = blenderVersion;
    result.blenderMeshImportMode = importMode;
    result.showInFront = item.value(QStringLiteral("showInFront")).toBool(false);
    result.showWire = item.value(QStringLiteral("show_wire")).toBool(false);
    result.showAllEdges = item.value(QStringLiteral("show_all_edges")).toBool(false);
    result.wireDisplay = item.value(QStringLiteral("wire_display")).toBool(false);
    result.displayBoundsType = item.value(QStringLiteral("bounds_display_type")).toString();
    if (!parseDisplayBoundsData(item, &result, errorMessage)) return false;
    const QJsonArray sourceCollectionIds = item.value(QStringLiteral("collection_ids")).toArray();
    for (const QJsonValue &collectionValue : sourceCollectionIds) {
        const QString sourceId = collectionValue.toString();
        if (sourceId.isEmpty() || !collectionIds.contains(sourceId)) {
            setError(errorMessage, QStringLiteral("object references an unknown Blender collection"));
            return false;
        }
        result.collectionIds.append(collectionIds.value(sourceId));
    }

    QVector<double> objectColor;
    if (!jsonNumberArray(item.value(QStringLiteral("display_color_linear")), 4,
                         &objectColor)) {
        setError(errorMessage, QStringLiteral("object display color is invalid"));
        return false;
    }
    result.displayColorSceneLinear = QVector4D(
        static_cast<float>(objectColor[0]), static_cast<float>(objectColor[1]),
        static_cast<float>(objectColor[2]), static_cast<float>(objectColor[3]));

    const QJsonArray transform = item.value(QStringLiteral("transform_world")).toArray();
    if (transform.size() != 4) {
        setError(errorMessage, QStringLiteral("world transform is not a 4 by 4 matrix"));
        return false;
    }
    for (int row = 0; row < 4; ++row) {
        QVector<double> values;
        if (!jsonNumberArray(transform[row], 4, &values)) {
            setError(errorMessage, QStringLiteral("world transform contains invalid values"));
            return false;
        }
        for (int column = 0; column < 4; ++column) {
            result.meshObjectTransform[static_cast<size_t>(row * 4 + column)] =
                values[column];
        }
    }
    const auto &matrix = result.meshObjectTransform;
    if (std::abs(matrix[12]) > 1.0e-9 || std::abs(matrix[13]) > 1.0e-9 ||
        std::abs(matrix[14]) > 1.0e-9 || std::abs(matrix[15] - 1.0) > 1.0e-9) {
        setError(errorMessage, QStringLiteral("world transform is not affine"));
        return false;
    }

    classiCAD::PolygonMesh3D mesh;
    const QJsonArray vertices = item.value(QStringLiteral("vertices_local")).toArray();
    mesh.vertices.reserve(vertices.size());
    for (const QJsonValue &vertexValue : vertices) {
        QVector<double> coordinates;
        if (!jsonNumberArray(vertexValue, 3, &coordinates)) {
            setError(errorMessage, QStringLiteral("vertex coordinate is invalid"));
            return false;
        }
        mesh.vertices.append({coordinates[0], coordinates[1], coordinates[2]});
    }
    const QJsonArray faces = item.value(QStringLiteral("polygons")).toArray();
    mesh.faces.reserve(faces.size());
    for (const QJsonValue &faceValue : faces) {
        if (!faceValue.isArray()) {
            setError(errorMessage, QStringLiteral("polygon loop is not an array"));
            return false;
        }
        const QJsonArray sourceFace = faceValue.toArray();
        QVector<int> face;
        face.reserve(sourceFace.size());
        for (const QJsonValue &indexValue : sourceFace) {
            int index = -1;
            if (!jsonInteger(indexValue, &index)) {
                setError(errorMessage, QStringLiteral("polygon index is not an integer"));
                return false;
            }
            face.append(index);
        }
        mesh.faces.append(std::move(face));
    }
    const QJsonArray sourceLoopVertexIndices =
        item.value(QStringLiteral("loop_vertex_indices")).toArray();
    int expectedLoopCount = 0;
    for (const QVector<int> &face : mesh.faces) {
        expectedLoopCount += face.size();
    }
    const QJsonValue sourceCornerNormals = item.value(QStringLiteral("corner_normals"));
    const bool customNormalsPresent =
        item.value(QStringLiteral("custom_split_normals_present")).toBool(false);
    if (customNormalsPresent && !sourceCornerNormals.isArray()) {
        setError(errorMessage, QStringLiteral("custom mesh corner normals are missing"));
        return false;
    }
    if (!sourceCornerNormals.isUndefined()) {
        if (!sourceCornerNormals.isArray() ||
            sourceCornerNormals.toArray().size() != expectedLoopCount) {
            setError(errorMessage, QStringLiteral("mesh corner normal count does not match loops"));
            return false;
        }
        mesh.cornerNormals.reserve(expectedLoopCount);
        for (const QJsonValue &normalValue : sourceCornerNormals.toArray()) {
            QVector<double> normal;
            if (!jsonNumberArray(normalValue, 3, &normal)) {
                setError(errorMessage, QStringLiteral("mesh corner normal is invalid"));
                return false;
            }
            mesh.cornerNormals.append({normal[0], normal[1], normal[2]});
        }
    }
    if (sourceLoopVertexIndices.size() != expectedLoopCount) {
        setError(errorMessage, QStringLiteral("mesh loop vertex list does not match polygon loops"));
        return false;
    }
    int loopIndex = 0;
    for (const QVector<int> &face : mesh.faces) {
        for (const int vertexIndex : face) {
            int recordedVertex = -1;
            if (!jsonInteger(sourceLoopVertexIndices[loopIndex++],
                             &recordedVertex) || recordedVertex != vertexIndex) {
                setError(errorMessage,
                         QStringLiteral("mesh loop ordering disagrees with polygon loops"));
                return false;
            }
        }
    }
    const QJsonValue sourceFaceTriangles =
        item.value(QStringLiteral("polygon_loop_triangles"));
    if (!sourceFaceTriangles.isUndefined()) {
        if (!sourceFaceTriangles.isArray() ||
            sourceFaceTriangles.toArray().size() != mesh.faces.size()) {
            setError(errorMessage,
                     QStringLiteral("mesh loop triangles do not match polygon faces"));
            return false;
        }
        mesh.faceTriangles.reserve(mesh.faces.size());
        const QJsonArray faceTriangleArray = sourceFaceTriangles.toArray();
        for (int faceIndex = 0; faceIndex < mesh.faces.size(); ++faceIndex) {
            const QJsonValue faceTrianglesValue = faceTriangleArray[faceIndex];
            if (!faceTrianglesValue.isArray()) {
                setError(errorMessage,
                         QStringLiteral("mesh face triangles are invalid"));
                return false;
            }
            QVector<std::array<int, 3>> triangles;
            for (const QJsonValue &triangleValue : faceTrianglesValue.toArray()) {
                if (!triangleValue.isArray() || triangleValue.toArray().size() != 3) {
                    setError(errorMessage,
                             QStringLiteral("mesh loop triangle is invalid"));
                    return false;
                }
                const QJsonArray corners = triangleValue.toArray();
                std::array<int, 3> triangle{};
                for (int corner = 0; corner < 3; ++corner) {
                    const int cornerIndex = static_cast<int>(corner);
                    if (!jsonInteger(corners[corner], &triangle[cornerIndex]) ||
                        triangle[cornerIndex] < 0 ||
                        triangle[cornerIndex] >= mesh.faces[faceIndex].size()) {
                        setError(errorMessage,
                                 QStringLiteral("mesh loop triangle corner is invalid"));
                        return false;
                    }
                }
                triangles.append(triangle);
            }
            if (!classiCAD::validatePolygonFaceTriangulation(
                    mesh.faces[faceIndex].size(), triangles)) {
                setError(errorMessage,
                         QStringLiteral("mesh loop triangulation is incomplete or invalid"));
                return false;
            }
            mesh.faceTriangles.append(std::move(triangles));
        }
    }
    const QJsonArray materialIndices =
        item.value(QStringLiteral("polygon_material_indices")).toArray();
    mesh.faceMaterialIndices.reserve(materialIndices.size());
    for (const QJsonValue &indexValue : materialIndices) {
        int index = -1;
        if (!jsonInteger(indexValue, &index)) {
            setError(errorMessage, QStringLiteral("polygon material index is invalid"));
            return false;
        }
        mesh.faceMaterialIndices.append(index);
    }

    const QJsonArray sourceSlots = item.value(QStringLiteral("material_slots")).toArray();
    mesh.materialSlotIds.reserve(sourceSlots.size());
    bool fallbackMaterialSet = false;
    for (int slotIndex = 0; slotIndex < sourceSlots.size(); ++slotIndex) {
        const QJsonObject sourceSlot = sourceSlots[slotIndex].toObject();
        int recordedIndex = -1;
        if (!jsonInteger(sourceSlot.value(QStringLiteral("slot_index")),
                         &recordedIndex) || recordedIndex != slotIndex) {
            setError(errorMessage, QStringLiteral("material slot order is invalid"));
            return false;
        }
        const QString materialId = sourceSlot.value(QStringLiteral("material_id")).toString();
        const classiCAD::DocumentMaterial material = materials.value(materialId);
        if (!materialId.isEmpty() && material.id.isEmpty()) {
            if (warnings != nullptr) {
                warnings->append(QStringLiteral("%1: missing material data for slot %2")
                                     .arg(blenderItemLabel(item)).arg(slotIndex));
            }
        }
        mesh.materialSlotIds.append(material.id.isEmpty() ? QString{} : materialId);
        if (!material.id.isEmpty()) {
            result.hasWorkbenchMaterial = true;
            if (!fallbackMaterialSet) {
                result.workbenchMaterial.baseColorSceneLinear = QVector3D(
                    material.baseColorSceneLinear[0],
                    material.baseColorSceneLinear[1],
                    material.baseColorSceneLinear[2]);
                result.workbenchMaterial.roughness = material.roughness;
                result.workbenchMaterial.metallic = material.metallic;
                result.workbenchMaterial.alpha = material.alpha;
                for (const classiCAD::DocumentImageTextureReference &image :
                     material.imageTextures) {
                    if (image.tiles.isEmpty()) continue;
                    result.workbenchMaterial.textureImageTiles = image.tiles;
                    result.workbenchMaterial.textureImageSrgb =
                        image.colorSpace.compare(QStringLiteral("sRGB"),
                                                 Qt::CaseInsensitive) == 0;
                    result.workbenchMaterial.textureImageColorTransformId =
                        image.sourceTransformId;
                    result.workbenchMaterial.textureImageSceneLinearFloatDataBase64 =
                        image.sceneLinearFloatDataBase64;
                    result.workbenchMaterial.textureImageSceneLinearFloatWidth =
                        image.sceneLinearFloatWidth;
                    result.workbenchMaterial.textureImageSceneLinearFloatHeight =
                        image.sceneLinearFloatHeight;
                    result.workbenchMaterial.textureImageAlphaMode = image.alphaMode;
                    result.workbenchMaterial.textureImageInterpolation = image.interpolation;
                    result.workbenchMaterial.textureImageExtension = image.extension;
                    break;
                }
                fallbackMaterialSet = true;
            }
        }
    }

    const QJsonArray sourceUvMaps = item.value(QStringLiteral("uv_layers")).toArray();
    mesh.uvMaps.reserve(sourceUvMaps.size());
    for (const QJsonValue &uvValue : sourceUvMaps) {
        const QJsonObject sourceUv = uvValue.toObject();
        classiCAD::MeshUvMap uvMap;
        uvMap.name = sourceUv.value(QStringLiteral("name")).toString();
        const QJsonArray coordinates = sourceUv.value(QStringLiteral("values")).toArray();
        uvMap.cornerValues.reserve(coordinates.size());
        for (const QJsonValue &coordinateValue : coordinates) {
            QVector<double> coordinate;
            if (!jsonNumberArray(coordinateValue, 2, &coordinate)) {
                setError(errorMessage, QStringLiteral("UV coordinate is invalid"));
                return false;
            }
            uvMap.cornerValues.append(QPointF(coordinate[0], coordinate[1]));
        }
        mesh.uvMaps.append(std::move(uvMap));
    }

    const QJsonArray sourceAttributes =
        item.value(QStringLiteral("color_attributes")).toArray();
    for (const QJsonValue &attributeValue : sourceAttributes) {
        const QJsonObject sourceAttribute = attributeValue.toObject();
        const QString dataType = sourceAttribute.value(QStringLiteral("data_type")).toString();
        if (dataType != QLatin1String("FLOAT_COLOR") &&
            dataType != QLatin1String("BYTE_COLOR")) {
            continue;
        }
        classiCAD::MeshColorAttribute attribute;
        attribute.name = sourceAttribute.value(QStringLiteral("name")).toString();
        attribute.dataType = dataType;
        attribute.colorSpace = QStringLiteral("scene_linear");
        attribute.componentCount = 4;
        const QString domain = sourceAttribute.value(QStringLiteral("domain")).toString();
        if (domain == QLatin1String("POINT")) {
            attribute.domain = classiCAD::MeshAttributeDomain::Point;
        } else if (domain == QLatin1String("FACE")) {
            attribute.domain = classiCAD::MeshAttributeDomain::Face;
        } else if (domain == QLatin1String("CORNER")) {
            attribute.domain = classiCAD::MeshAttributeDomain::Corner;
        } else {
            if (warnings != nullptr) {
                warnings->append(QStringLiteral("%1: color attribute %2 has unsupported domain %3")
                                     .arg(blenderItemLabel(item), attribute.name, domain));
            }
            continue;
        }
        const QJsonArray values = sourceAttribute.value(QStringLiteral("values")).toArray();
        bool validAttribute = sourceAttribute.value(QStringLiteral("values")).isArray();
        for (const QJsonValue &entryValue : values) {
            const QJsonObject entry = entryValue.toObject();
            QVector<double> color;
            if (!jsonNumberArray(entry.value(QStringLiteral("color_linear")), 4, &color)) {
                validAttribute = false;
                break;
            }
            for (double component : color) {
                attribute.values.append(static_cast<float>(component));
            }
        }
        if (!validAttribute) {
            if (warnings != nullptr) {
                warnings->append(QStringLiteral("%1: color attribute %2 could not be represented")
                                     .arg(blenderItemLabel(item), attribute.name));
            }
            continue;
        }
        int expectedAttributeElements = -1;
        switch (attribute.domain) {
        case classiCAD::MeshAttributeDomain::Point:
            expectedAttributeElements = mesh.vertices.size();
            break;
        case classiCAD::MeshAttributeDomain::Face:
            expectedAttributeElements = mesh.faces.size();
            break;
        case classiCAD::MeshAttributeDomain::Corner:
            expectedAttributeElements = expectedLoopCount;
            break;
        }
        const bool duplicateAttribute = std::any_of(
            mesh.colorAttributes.cbegin(), mesh.colorAttributes.cend(),
            [&attribute](const classiCAD::MeshColorAttribute &existing) {
                return existing.name == attribute.name;
            });
        if (attribute.name.isEmpty() || duplicateAttribute ||
            expectedAttributeElements < 0 ||
            attribute.values.size() != expectedAttributeElements *
                                          attribute.componentCount) {
            if (warnings != nullptr) {
                warnings->append(QStringLiteral("%1: malformed color attribute %2 was omitted")
                                     .arg(blenderItemLabel(item), attribute.name));
            }
            continue;
        }
        mesh.colorAttributes.append(std::move(attribute));
    }

    const QString activeUvName = item.value(QStringLiteral("active_uv_layer")).toString();
    const QString defaultUvName = item.value(QStringLiteral("default_uv_layer")).toString();
    if (!activeUvName.isEmpty()) {
        const bool found = std::any_of(
            mesh.uvMaps.cbegin(), mesh.uvMaps.cend(),
            [&activeUvName](const classiCAD::MeshUvMap &uvMap) {
                return uvMap.name == activeUvName;
            });
        if (found) {
            mesh.activeUvMapName = activeUvName;
        } else if (warnings != nullptr) {
            warnings->append(QStringLiteral("%1: active UV map %2 was not retained")
                                 .arg(blenderItemLabel(item), activeUvName));
        }
    }
    if (!defaultUvName.isEmpty()) {
        const bool found = std::any_of(
            mesh.uvMaps.cbegin(), mesh.uvMaps.cend(),
            [&defaultUvName](const classiCAD::MeshUvMap &uvMap) {
                return uvMap.name == defaultUvName;
            });
        if (found) {
            mesh.defaultUvMapName = defaultUvName;
        } else if (warnings != nullptr) {
            warnings->append(QStringLiteral("%1: default UV map %2 was not retained")
                                 .arg(blenderItemLabel(item), defaultUvName));
        }
    }
    const QString activeColorName =
        item.value(QStringLiteral("active_color_attribute")).toString();
    if (!activeColorName.isEmpty()) {
        const bool found = std::any_of(
            mesh.colorAttributes.cbegin(), mesh.colorAttributes.cend(),
            [&activeColorName](const classiCAD::MeshColorAttribute &attribute) {
                return attribute.name == activeColorName;
            });
        if (found) {
            mesh.activeColorAttributeName = activeColorName;
        } else if (warnings != nullptr) {
            warnings->append(QStringLiteral("%1: active color attribute %2 was not retained")
                                 .arg(blenderItemLabel(item), activeColorName));
        }
    }
    const QString defaultColorName =
        item.value(QStringLiteral("default_color_attribute")).toString();
    if (!defaultColorName.isEmpty()) {
        const bool found = std::any_of(
            mesh.colorAttributes.cbegin(), mesh.colorAttributes.cend(),
            [&defaultColorName](const classiCAD::MeshColorAttribute &attribute) {
                return attribute.name == defaultColorName;
            });
        if (found) {
            mesh.defaultColorAttributeName = defaultColorName;
        } else if (warnings != nullptr) {
            warnings->append(QStringLiteral("%1: default color attribute %2 was not retained")
                                 .arg(blenderItemLabel(item), defaultColorName));
        }
    }

    QString validationError;
    if (!classiCAD::validatePolygonMesh(mesh, &validationError)) {
        setError(errorMessage, validationError);
        return false;
    }
    result.geometry.polygonMesh = std::move(mesh);
    const QJsonObject visibility = item.value(QStringLiteral("visibility")).toObject();
    result.hideViewport = visibility.value(QStringLiteral("hide_viewport")).toBool();
    result.hideRender = visibility.value(QStringLiteral("hide_render")).toBool();
    result.hiddenInViewport = visibility.value(QStringLiteral("hidden_in_viewport")).toBool();
    result.hideSelect = visibility.value(QStringLiteral("hide_select")).toBool();
    result.viewLayerHidden = item.value(QStringLiteral("view_layer_hidden")).toObject();
    *sceneObject = std::move(result);
    return true;
}

bool parseBlenderPolyCurveItem(
    const QJsonObject &item,
    const QHash<QString, classiCAD::CollectionId> &collectionIds,
    classiCAD::SceneObject *sceneObject,
    QString *errorMessage)
{
    if (sceneObject == nullptr ||
        item.value(QStringLiteral("object_type")).toString() != QLatin1String("CURVE")) {
        setError(errorMessage, QStringLiteral("object is not a Blender curve"));
        return false;
    }
    const QJsonArray sourceSplines = item.value(QStringLiteral("splines")).toArray();
    if (sourceSplines.isEmpty()) {
        setError(errorMessage, QStringLiteral("curve has no supported spline components"));
        return false;
    }

    classiCAD::SceneObject result;
    result.displayName = item.value(QStringLiteral("name")).toString();
    result.libraryPath = item.value(QStringLiteral("library_path")).toString();
    result.workbenchRandomLibraryPath =
        item.value(QStringLiteral("workbench_random_library_path")).toString();
    result.showInFront = item.value(QStringLiteral("showInFront")).toBool(false);
    result.showWire = item.value(QStringLiteral("show_wire")).toBool(false);
    result.showAllEdges = item.value(QStringLiteral("show_all_edges")).toBool(false);
    result.wireDisplay = item.value(QStringLiteral("wire_display")).toBool(false);
    result.blenderLooseMeshEdgeCompanion = item.value(
        QStringLiteral("classiCAD_loose_mesh_edge_companion")).toBool(false);
    result.displayBoundsType = item.value(QStringLiteral("bounds_display_type")).toString();
    if (!parseDisplayBoundsData(item, &result, errorMessage)) return false;
    for (const QJsonValue &collectionValue :
         item.value(QStringLiteral("collection_ids")).toArray()) {
        const QString sourceId = collectionValue.toString();
        if (sourceId.isEmpty() || !collectionIds.contains(sourceId)) {
            setError(errorMessage,
                     QStringLiteral("object references an unknown Blender collection"));
            return false;
        }
        result.collectionIds.append(collectionIds.value(sourceId));
    }
    QVector<double> objectColor;
    if (!jsonNumberArray(item.value(QStringLiteral("display_color_linear")), 4,
                         &objectColor)) {
        setError(errorMessage, QStringLiteral("object display color is invalid"));
        return false;
    }
    result.displayColorSceneLinear = QVector4D(
        static_cast<float>(objectColor[0]), static_cast<float>(objectColor[1]),
        static_cast<float>(objectColor[2]), static_cast<float>(objectColor[3]));

    QVector<classiCAD::NurbsCurve3D> curves;
    for (const QJsonValue &splineValue : sourceSplines) {
        if (!splineValue.isObject()) {
            setError(errorMessage, QStringLiteral("curve spline record is invalid"));
            return false;
        }
        const QJsonObject spline = splineValue.toObject();
        const QString splineType = spline.value(QStringLiteral("type")).toString(
            QStringLiteral("POLY"));
        if (splineType == QLatin1String("POLY")) {
            const QJsonArray sourcePoints =
                spline.value(QStringLiteral("control_points_world")).toArray();
            if (sourcePoints.size() < 2) {
                setError(errorMessage,
                         QStringLiteral("POLY spline needs at least two control points"));
                return false;
            }
            classiCAD::NurbsCurve3D curve;
            curve.dimension = 3;
            curve.degree = 1;
            curve.order = 2;
            curve.rational = false;
            curve.controlPoints.reserve(sourcePoints.size());
            curve.normalCoordinates.reserve(sourcePoints.size());
            curve.weights.fill(1.0, sourcePoints.size());
            curve.knots.reserve(sourcePoints.size());
            for (int pointIndex = 0; pointIndex < sourcePoints.size(); ++pointIndex) {
                QVector<double> xyz;
                if (!jsonNumberArray(sourcePoints[pointIndex], 3, &xyz)) {
                    setError(errorMessage,
                             QStringLiteral("POLY spline control point is invalid"));
                    return false;
                }
                curve.controlPoints.append(QPointF(xyz[0], xyz[1]));
                curve.normalCoordinates.append(xyz[2]);
                // A degree-one clamped uniform vector reduces to integer knots
                // after Rhino's two redundant outer entries are removed.
                curve.knots.append(static_cast<double>(pointIndex));
            }
            curves.append(std::move(curve));
        } else if (splineType == QLatin1String("NURBS")) {
            const QJsonArray sourcePoints =
                spline.value(QStringLiteral("control_points_world")).toArray();
            QVector<double> weights;
            QVector<double> knots;
            int degree = -1;
            if (!jsonInteger(spline.value(QStringLiteral("degree")), &degree) ||
                degree < 1 || sourcePoints.size() <= degree ||
                !jsonNumberArray(spline.value(QStringLiteral("weights")),
                                 sourcePoints.size(), &weights) ||
                !jsonNumberArray(spline.value(QStringLiteral("knots")), -1, &knots)) {
                setError(errorMessage, QStringLiteral("NURBS spline data is invalid"));
                return false;
            }
            classiCAD::NurbsCurve3D curve;
            curve.dimension = 3;
            curve.degree = degree;
            curve.order = degree + 1;
            curve.rational = spline.value(QStringLiteral("rational")).toBool();
            curve.weights = std::move(weights);
            curve.knots = std::move(knots);
            curve.controlPoints.reserve(sourcePoints.size());
            curve.normalCoordinates.reserve(sourcePoints.size());
            for (const QJsonValue &pointValue : sourcePoints) {
                QVector<double> xyz;
                if (!jsonNumberArray(pointValue, 3, &xyz)) {
                    setError(errorMessage,
                             QStringLiteral("NURBS spline control point is invalid"));
                    return false;
                }
                curve.controlPoints.append(QPointF(xyz[0], xyz[1]));
                curve.normalCoordinates.append(xyz[2]);
            }
            curves.append(std::move(curve));
        } else if (splineType == QLatin1String("BEZIER")) {
            const QJsonArray spans = spline.value(QStringLiteral("spans_world")).toArray();
            if (spans.isEmpty()) {
                setError(errorMessage, QStringLiteral("BEZIER spline has no spans"));
                return false;
            }
            QVector<double> previousEnd;
            for (const QJsonValue &spanValue : spans) {
                const QJsonArray points = spanValue.toArray();
                if (points.size() != 4) {
                    setError(errorMessage,
                             QStringLiteral("BEZIER span must contain four control points"));
                    return false;
                }
                classiCAD::NurbsCurve3D curve;
                curve.dimension = 3;
                curve.degree = 3;
                curve.order = 4;
                curve.rational = false;
                curve.weights.fill(1.0, 4);
                curve.knots = {0.0, 0.0, 0.0, 1.0, 1.0, 1.0};
                QVector<QVector<double>> spanCoordinates;
                spanCoordinates.reserve(4);
                for (const QJsonValue &pointValue : points) {
                    QVector<double> xyz;
                    if (!jsonNumberArray(pointValue, 3, &xyz)) {
                        setError(errorMessage,
                                 QStringLiteral("BEZIER span control point is invalid"));
                        return false;
                    }
                    spanCoordinates.append(xyz);
                }
                if (!previousEnd.isEmpty() &&
                    (previousEnd[0] != spanCoordinates[0][0] ||
                     previousEnd[1] != spanCoordinates[0][1] ||
                     previousEnd[2] != spanCoordinates[0][2])) {
                    setError(errorMessage,
                             QStringLiteral("BEZIER spline span endpoints are disconnected"));
                    return false;
                }
                previousEnd = spanCoordinates[3];
                for (const QVector<double> &xyz : spanCoordinates) {
                    curve.controlPoints.append(QPointF(xyz[0], xyz[1]));
                    curve.normalCoordinates.append(xyz[2]);
                }
                curves.append(std::move(curve));
            }
        } else {
            setError(errorMessage, QStringLiteral("unsupported Blender curve spline type"));
            return false;
        }
    }
    for (const classiCAD::NurbsCurve3D &curve : curves) {
        QString validationError;
        if (!classiCAD::validateNurbsCurve(curve, &validationError)) {
            setError(errorMessage, validationError);
            return false;
        }
    }
    if (result.blenderLooseMeshEdgeCompanion) {
        const QJsonArray sourceEdgeIndices = item.value(
            QStringLiteral("classiCAD_loose_mesh_edge_source_indices"))
                                                .toArray();
        if (sourceEdgeIndices.size() != curves.size()) {
            setError(errorMessage,
                     QStringLiteral("loose-edge source index count does not match its curves"));
            return false;
        }
        result.blenderLooseMeshEdgeSourceIndices.reserve(
            sourceEdgeIndices.size());
        for (const QJsonValue &sourceEdgeIndex : sourceEdgeIndices) {
            int edgeIndex = -1;
            if (!jsonInteger(sourceEdgeIndex, &edgeIndex) || edgeIndex < 0) {
                setError(errorMessage,
                         QStringLiteral("loose-edge source index is invalid"));
                return false;
            }
            result.blenderLooseMeshEdgeSourceIndices.append(edgeIndex);
        }
    }

    result.geometry.workPlane = classiCAD::WorkPlane::XY;
    result.geometry.workPlaneOffset = 0.0;
    result.geometry.workPlaneFrame =
        classiCAD::makeWorkPlaneFrame(classiCAD::WorkPlane::XY, 0.0);
    if (curves.size() == 1) {
        result.geometry.geometryType = classiCAD::GeometryType::Nurbs;
        result.geometry.nurbs = std::move(curves.first());
    } else {
        result.geometry.geometryType = classiCAD::GeometryType::PolyCurve;
        result.geometry.components = std::move(curves);
        result.geometry.componentWorkPlaneFrames.fill(
            result.geometry.workPlaneFrame, result.geometry.components.size());
    }
    const QJsonObject visibility = item.value(QStringLiteral("visibility")).toObject();
    result.hideViewport = visibility.value(QStringLiteral("hide_viewport")).toBool();
    result.hideRender = visibility.value(QStringLiteral("hide_render")).toBool();
    result.hiddenInViewport = visibility.value(QStringLiteral("hidden_in_viewport")).toBool();
    result.hideSelect = visibility.value(QStringLiteral("hide_select")).toBool();
    result.viewLayerHidden = item.value(QStringLiteral("view_layer_hidden")).toObject();
    *sceneObject = std::move(result);
    return true;
}

bool parseBlenderPointItem(
    const QJsonObject &item,
    const QHash<QString, classiCAD::CollectionId> &collectionIds,
    const QString &blenderVersion,
    const QString &importMode,
    classiCAD::SceneObject *sceneObject,
    QString *errorMessage)
{
    if (sceneObject == nullptr ||
        item.value(QStringLiteral("object_type")).toString() !=
            QLatin1String("POINT")) {
        setError(errorMessage, QStringLiteral("object is not a Blender loose vertex"));
        return false;
    }
    QVector<double> worldPoint;
    if (!jsonNumberArray(item.value(QStringLiteral("point_world")), 3,
                         &worldPoint)) {
        setError(errorMessage, QStringLiteral("loose vertex position is invalid"));
        return false;
    }

    classiCAD::SceneObject result;
    result.displayName = item.value(QStringLiteral("name")).toString();
    result.libraryPath = item.value(QStringLiteral("library_path")).toString();
    result.workbenchRandomLibraryPath =
        item.value(QStringLiteral("workbench_random_library_path")).toString();
    result.blenderSourceVersion = blenderVersion;
    result.blenderMeshImportMode = importMode;
    result.showInFront = item.value(QStringLiteral("showInFront")).toBool(false);
    for (const QJsonValue &collectionValue :
         item.value(QStringLiteral("collection_ids")).toArray()) {
        const QString sourceId = collectionValue.toString();
        if (sourceId.isEmpty() || !collectionIds.contains(sourceId)) {
            setError(errorMessage,
                     QStringLiteral("loose vertex references an unknown Blender collection"));
            return false;
        }
        result.collectionIds.append(collectionIds.value(sourceId));
    }
    QVector<double> objectColor;
    if (!jsonNumberArray(item.value(QStringLiteral("display_color_linear")), 4,
                         &objectColor)) {
        setError(errorMessage, QStringLiteral("object display color is invalid"));
        return false;
    }
    result.displayColorSceneLinear = QVector4D(
        static_cast<float>(objectColor[0]), static_cast<float>(objectColor[1]),
        static_cast<float>(objectColor[2]), static_cast<float>(objectColor[3]));

    result.geometry.geometryType = classiCAD::GeometryType::Point;
    result.geometry.points.append(QPointF(0.0, 0.0));
    const classiCAD::Point3D origin{
        worldPoint[0], worldPoint[1], worldPoint[2]};
    result.geometry.workPlaneFrame = classiCAD::makeWorkPlaneFrameFromNormal(
        origin, {0.0, 0.0, 1.0});
    if (!classiCAD::isValidWorkPlaneFrame(result.geometry.workPlaneFrame)) {
        setError(errorMessage, QStringLiteral("loose vertex work frame is invalid"));
        return false;
    }
    const QJsonObject visibility =
        item.value(QStringLiteral("visibility")).toObject();
    result.hideViewport = visibility.value(QStringLiteral("hide_viewport")).toBool();
    result.hideRender = visibility.value(QStringLiteral("hide_render")).toBool();
    result.hiddenInViewport =
        visibility.value(QStringLiteral("hidden_in_viewport")).toBool();
    result.hideSelect = visibility.value(QStringLiteral("hide_select")).toBool();
    result.viewLayerHidden =
        item.value(QStringLiteral("view_layer_hidden")).toObject();
    *sceneObject = std::move(result);
    return true;
}

bool parseBlenderPointCloudItem(
    const QJsonObject &item,
    const QHash<QString, classiCAD::CollectionId> &collectionIds,
    classiCAD::SceneObject *sceneObject,
    QString *errorMessage)
{
    if (sceneObject == nullptr ||
        item.value(QStringLiteral("object_type")).toString() !=
            QLatin1String("POINTCLOUD") ||
        item.value(QStringLiteral("pointcloud_type")).toString() !=
            QLatin1String("POINTS")) {
        setError(errorMessage, QStringLiteral("object is not a point-based Blender Point Cloud"));
        return false;
    }
    const QJsonArray points = item.value(QStringLiteral("points")).toArray();
    if (points.isEmpty() || points.size() > 100'000) {
        setError(errorMessage, QStringLiteral("point cloud point count is invalid or exceeds the per-scene limit"));
        return false;
    }
    classiCAD::PointCloud3D pointCloud;
    pointCloud.positions.reserve(points.size());
    pointCloud.radii.reserve(points.size());
    pointCloud.sourceIndices.reserve(points.size());
    bool hasColors = false;
    bool missingColor = false;
    for (const QJsonValue &pointValue : points) {
        const bool hasColor = pointValue.isObject() &&
            pointValue.toObject().contains(QStringLiteral("color_linear"));
        hasColors = hasColors || hasColor;
        missingColor = missingColor || !hasColor;
    }
    if (hasColors && missingColor) {
        setError(errorMessage, QStringLiteral("point cloud color attribute is incomplete"));
        return false;
    }
    for (const QJsonValue &pointValue : points) {
        if (!pointValue.isObject()) {
            setError(errorMessage, QStringLiteral("point cloud record is malformed"));
            return false;
        }
        const QJsonObject pointRecord = pointValue.toObject();
        QVector<double> position;
        QVector<double> color;
        double radius = 0.0;
        int sourceIndex = -1;
        if (!jsonNumberArray(pointRecord.value(QStringLiteral("position_world")),
                             3, &position) ||
            !jsonFiniteNumber(pointRecord.value(QStringLiteral("radius_world")),
                              &radius) || radius < 0.0 ||
            !jsonInteger(pointRecord.value(QStringLiteral("source_index")),
                         &sourceIndex) || sourceIndex < 0) {
            setError(errorMessage, QStringLiteral("point cloud position, radius, or source index is invalid"));
            return false;
        }
        const QJsonValue colorValue = pointRecord.value(QStringLiteral("color_linear"));
        if (hasColors) {
            if (!jsonNumberArray(colorValue, 4, &color) ||
                std::any_of(color.cbegin(), color.cend(), [](double value) {
                    return std::abs(value) > std::numeric_limits<float>::max();
                })) {
                setError(errorMessage, QStringLiteral("point cloud color is invalid"));
                return false;
            }
        }
        pointCloud.positions.append({position[0], position[1], position[2]});
        pointCloud.radii.append(radius);
        pointCloud.sourceIndices.append(sourceIndex);
        if (hasColors) {
            pointCloud.colors.append(QVector4D(
                float(color[0]), float(color[1]), float(color[2]), float(color[3])));
        }
    }
    if (!classiCAD::validatePointCloud(pointCloud)) {
        setError(errorMessage, QStringLiteral("point cloud payload is invalid"));
        return false;
    }

    classiCAD::SceneObject result;
    result.displayName = item.value(QStringLiteral("name")).toString();
    result.libraryPath = item.value(QStringLiteral("library_path")).toString();
    result.workbenchRandomLibraryPath = item.value(
        QStringLiteral("workbench_random_library_path")).toString();
    result.showInFront = item.value(QStringLiteral("showInFront")).toBool(false);
    QVector<double> objectColor;
    if (!jsonNumberArray(item.value(QStringLiteral("display_color_linear")),
                         4, &objectColor)) {
        setError(errorMessage, QStringLiteral("point cloud object display color is invalid"));
        return false;
    }
    result.displayColorSceneLinear = QVector4D(
        float(objectColor[0]), float(objectColor[1]),
        float(objectColor[2]), float(objectColor[3]));
    for (const QJsonValue &collectionValue :
         item.value(QStringLiteral("collection_ids")).toArray()) {
        const QString sourceId = collectionValue.toString();
        if (sourceId.isEmpty() || !collectionIds.contains(sourceId)) {
            setError(errorMessage, QStringLiteral("point cloud references an unknown Blender collection"));
            return false;
        }
        result.collectionIds.append(collectionIds.value(sourceId));
    }
    result.geometry.geometryType = classiCAD::GeometryType::PointCloud;
    result.geometry.pointCloud = std::move(pointCloud);
    const QJsonObject visibility = item.value(QStringLiteral("visibility")).toObject();
    result.hideViewport = visibility.value(QStringLiteral("hide_viewport")).toBool();
    result.hideRender = visibility.value(QStringLiteral("hide_render")).toBool();
    result.hiddenInViewport = visibility.value(QStringLiteral("hidden_in_viewport")).toBool();
    result.hideSelect = visibility.value(QStringLiteral("hide_select")).toBool();
    result.viewLayerHidden = item.value(QStringLiteral("view_layer_hidden")).toObject();
    *sceneObject = std::move(result);
    return true;
}

bool parseBlenderSurfaceItem(
    const QJsonObject &item,
    const QHash<QString, classiCAD::CollectionId> &collectionIds,
    const QHash<QString, classiCAD::DocumentMaterial> &materials,
    classiCAD::SceneObject *sceneObject,
    QString *errorMessage)
{
    if (sceneObject == nullptr ||
        item.value(QStringLiteral("object_type")).toString() != QLatin1String("SURFACE")) {
        setError(errorMessage, QStringLiteral("object is not a Blender surface"));
        return false;
    }
    const QJsonObject sourceSurface = item.value(QStringLiteral("surface")).toObject();
    classiCAD::NurbsSurface3D surface;
    if (!jsonInteger(sourceSurface.value(QStringLiteral("degree_u")), &surface.degreeU) ||
        !jsonInteger(sourceSurface.value(QStringLiteral("degree_v")), &surface.degreeV) ||
        !jsonInteger(sourceSurface.value(QStringLiteral("order_u")), &surface.orderU) ||
        !jsonInteger(sourceSurface.value(QStringLiteral("order_v")), &surface.orderV) ||
        !jsonInteger(sourceSurface.value(QStringLiteral("control_vertex_count_u")),
                     &surface.controlVertexCountU) ||
        !jsonInteger(sourceSurface.value(QStringLiteral("control_vertex_count_v")),
                     &surface.controlVertexCountV) ||
        surface.controlVertexCountU <= 0 || surface.controlVertexCountV <= 0) {
        setError(errorMessage, QStringLiteral("surface degree, order, or net dimensions are invalid"));
        return false;
    }
    const qint64 pointCount = qint64(surface.controlVertexCountU) *
                              surface.controlVertexCountV;
    const QJsonArray sourcePoints =
        sourceSurface.value(QStringLiteral("control_points_world")).toArray();
    if (pointCount > std::numeric_limits<int>::max() ||
        sourcePoints.size() != pointCount ||
        !jsonNumberArray(sourceSurface.value(QStringLiteral("weights")),
                         static_cast<int>(pointCount), &surface.weights) ||
        !jsonNumberArray(sourceSurface.value(QStringLiteral("knots_u")), -1,
                         &surface.knotsU) ||
        !jsonNumberArray(sourceSurface.value(QStringLiteral("knots_v")), -1,
                         &surface.knotsV)) {
        setError(errorMessage, QStringLiteral("surface control net, weights, or knots are invalid"));
        return false;
    }
    surface.rational = sourceSurface.value(QStringLiteral("rational")).toBool();
    surface.controlPoints.reserve(static_cast<int>(pointCount));
    for (const QJsonValue &pointValue : sourcePoints) {
        QVector<double> xyz;
        if (!jsonNumberArray(pointValue, 3, &xyz)) {
            setError(errorMessage, QStringLiteral("surface control point is invalid"));
            return false;
        }
        surface.controlPoints.append({xyz[0], xyz[1], xyz[2]});
    }
    QString validationError;
    if (!classiCAD::validateNurbsSurface(surface, &validationError)) {
        setError(errorMessage, validationError);
        return false;
    }

    classiCAD::SceneObject result;
    result.displayName = item.value(QStringLiteral("name")).toString();
    result.libraryPath = item.value(QStringLiteral("library_path")).toString();
    result.workbenchRandomLibraryPath =
        item.value(QStringLiteral("workbench_random_library_path")).toString();
    result.showInFront = item.value(QStringLiteral("showInFront")).toBool(false);
    result.showWire = item.value(QStringLiteral("show_wire")).toBool(false);
    result.showAllEdges = item.value(QStringLiteral("show_all_edges")).toBool(false);
    result.wireDisplay = item.value(QStringLiteral("wire_display")).toBool(false);
    result.displayBoundsType = item.value(QStringLiteral("bounds_display_type")).toString();
    if (!parseDisplayBoundsData(item, &result, errorMessage)) return false;
    for (const QJsonValue &collectionValue :
         item.value(QStringLiteral("collection_ids")).toArray()) {
        const QString sourceId = collectionValue.toString();
        if (sourceId.isEmpty() || !collectionIds.contains(sourceId)) {
            setError(errorMessage,
                     QStringLiteral("object references an unknown Blender collection"));
            return false;
        }
        result.collectionIds.append(collectionIds.value(sourceId));
    }
    QVector<double> objectColor;
    if (!jsonNumberArray(item.value(QStringLiteral("display_color_linear")), 4,
                         &objectColor)) {
        setError(errorMessage, QStringLiteral("object display color is invalid"));
        return false;
    }
    result.displayColorSceneLinear = QVector4D(
        static_cast<float>(objectColor[0]), static_cast<float>(objectColor[1]),
        static_cast<float>(objectColor[2]), static_cast<float>(objectColor[3]));
    const QString materialId = sourceSurface.value(QStringLiteral("material_id")).toString();
    const classiCAD::DocumentMaterial material = materials.value(materialId);
    if (!material.id.isEmpty()) {
        result.hasWorkbenchMaterial = true;
        result.workbenchMaterial =
            classiCAD::workbenchMaterialDataFromDocumentMaterial(material);
    }
    result.geometry.geometryType = classiCAD::GeometryType::NurbsSurface;
    result.geometry.nurbsSurface = std::move(surface);
    const QJsonValue wireVerticesValue =
        item.value(QStringLiteral("surface_wire_vertices_world"));
    if (!wireVerticesValue.isUndefined()) {
        if (!wireVerticesValue.isArray()) {
            setError(errorMessage, QStringLiteral("surface wire vertices are invalid"));
            return false;
        }
        const QJsonArray wireVertices = wireVerticesValue.toArray();
        if (wireVertices.size() > 400000 || (wireVertices.size() % 2) != 0) {
            setError(errorMessage, QStringLiteral("surface wire vertex count is invalid"));
            return false;
        }
        result.blenderSurfaceWireVerticesWorld.reserve(wireVertices.size());
        for (const QJsonValue &wireVertexValue : wireVertices) {
            QVector<double> xyz;
            if (!jsonNumberArray(wireVertexValue, 3, &xyz)) {
                setError(errorMessage, QStringLiteral("surface wire vertex is invalid"));
                return false;
            }
            result.blenderSurfaceWireVerticesWorld.append(
                {xyz[0], xyz[1], xyz[2]});
        }
        result.blenderSurfaceWireGeometryFingerprint =
            classiCAD::nurbsSurfaceGeometryFingerprint(
                result.geometry.nurbsSurface);
    }
    const QJsonObject visibility = item.value(QStringLiteral("visibility")).toObject();
    result.hideViewport = visibility.value(QStringLiteral("hide_viewport")).toBool();
    result.hideRender = visibility.value(QStringLiteral("hide_render")).toBool();
    result.hiddenInViewport = visibility.value(QStringLiteral("hidden_in_viewport")).toBool();
    result.hideSelect = visibility.value(QStringLiteral("hide_select")).toBool();
    result.viewLayerHidden = item.value(QStringLiteral("view_layer_hidden")).toObject();
    *sceneObject = std::move(result);
    return true;
}

bool parseBlenderPictureItem(
    const QJsonObject &item,
    const QHash<QString, classiCAD::CollectionId> &collectionIds,
    classiCAD::SceneObject *sceneObject,
    QStringList *warnings,
    QString *errorMessage)
{
    if (sceneObject == nullptr ||
        item.value(QStringLiteral("object_type")).toString() !=
            QLatin1String("EMPTY_IMAGE")) {
        setError(errorMessage, QStringLiteral("object is not a Blender Image Empty"));
        return false;
    }

    const QJsonArray sourceCorners =
        item.value(QStringLiteral("picture_corners_world")).toArray();
    if (sourceCorners.size() != 4) {
        setError(errorMessage, QStringLiteral("Image Empty corners are invalid"));
        return false;
    }
    QVector<classiCAD::Point3D> corners;
    corners.reserve(4);
    for (const QJsonValue &cornerValue : sourceCorners) {
        QVector<double> xyz;
        if (!jsonNumberArray(cornerValue, 3, &xyz)) {
            setError(errorMessage, QStringLiteral("Image Empty corner is invalid"));
            return false;
        }
        corners.append({xyz[0], xyz[1], xyz[2]});
    }
    const QVector3D first(corners[0].x, corners[0].y, corners[0].z);
    const QVector3D edgeX(corners[1].x - corners[0].x,
                          corners[1].y - corners[0].y,
                          corners[1].z - corners[0].z);
    const QVector3D edgeY(corners[0].x - corners[3].x,
                          corners[0].y - corners[3].y,
                          corners[0].z - corners[3].z);
    if (edgeX.lengthSquared() <= 1.0e-16f ||
        edgeY.lengthSquared() <= 1.0e-16f) {
        setError(errorMessage, QStringLiteral("Image Empty plane is degenerate"));
        return false;
    }
    const QVector3D normal = QVector3D::crossProduct(edgeX, edgeY);
    const classiCAD::WorkPlaneFrame frame =
        classiCAD::makeWorkPlaneFrameFromNormal(
            corners[0], {normal.x(), normal.y(), normal.z()},
            {edgeX.x(), edgeX.y(), edgeX.z()});
    if (!classiCAD::isValidWorkPlaneFrame(frame)) {
        setError(errorMessage, QStringLiteral("Image Empty plane frame is invalid"));
        return false;
    }

    constexpr qint64 maximumPackedImageBytes = 32 * 1024 * 1024;
    constexpr qint64 maximumImagePixels = 16 * 1024 * 1024;
    QByteArray imageBytes;
    const QString encoded = item.value(
        QStringLiteral("picture_image_packed_data_base64")).toString();
    if (!encoded.isEmpty()) {
        if (encoded.size() > 44 * 1024 * 1024) {
            setError(errorMessage, QStringLiteral("packed Image Empty image exceeds the transport limit"));
            return false;
        }
        imageBytes = QByteArray::fromBase64(encoded.toLatin1());
        if (imageBytes.size() > maximumPackedImageBytes) {
            setError(errorMessage, QStringLiteral("packed Image Empty image exceeds the byte limit"));
            return false;
        }
    }
    const QString imagePath = item.value(
        QStringLiteral("picture_image_path")).toString();
    QImage image;
    const auto readImage = [&](QImageReader *reader) {
        if (reader == nullptr) return QImage{};
        reader->setAutoTransform(true);
        const QSize sourceSize = reader->size();
        if (sourceSize.isValid() &&
            qint64(sourceSize.width()) * sourceSize.height() > maximumImagePixels) {
            return QImage{};
        }
        QImage decoded = reader->read();
        if (decoded.isNull() ||
            qint64(decoded.width()) * decoded.height() > maximumImagePixels) {
            return QImage{};
        }
        return decoded;
    };
    if (!imageBytes.isEmpty()) {
        QBuffer buffer(&imageBytes);
        if (buffer.open(QIODevice::ReadOnly)) {
            QImageReader reader(&buffer);
            image = readImage(&reader);
        }
    }
    if (image.isNull() && !imagePath.isEmpty()) {
        const QFileInfo imageInfo(imagePath);
        if (imageInfo.isFile() && imageInfo.size() <= maximumPackedImageBytes) {
            QImageReader reader(imagePath);
            image = readImage(&reader);
            if (!image.isNull()) imageBytes.clear();
        }
    }
    if (image.isNull()) {
        setError(errorMessage, QStringLiteral("Image Empty image could not be read or exceeds its size limit"));
        return false;
    }
    const QString pictureColorSpace = item.value(
        QStringLiteral("picture_color_space")).toString(QStringLiteral("sRGB"));
    const QString pictureSourceTransformId = item.value(
        QStringLiteral("picture_source_transform_id")).toString(
            QStringLiteral("identity"));
    const QString encodedSceneLinear = item.value(
        QStringLiteral("picture_scene_linear_float_data_base64")).toString();
    QByteArray sceneLinearFloatData;
    int sceneLinearFloatWidth = 0;
    int sceneLinearFloatHeight = 0;
    if (!encodedSceneLinear.isEmpty()) {
        if (!decodeFiniteSceneLinearFloatPayload(
                encodedSceneLinear,
                item.value(QStringLiteral("picture_scene_linear_float_width")),
                item.value(QStringLiteral("picture_scene_linear_float_height")),
                &sceneLinearFloatData, &sceneLinearFloatWidth,
                &sceneLinearFloatHeight) ||
            sceneLinearFloatWidth != image.width() ||
            sceneLinearFloatHeight != image.height() ||
            (pictureSourceTransformId.size() != 64 &&
             pictureSourceTransformId != QStringLiteral("blender-float-buffer"))) {
            sceneLinearFloatData.clear();
            sceneLinearFloatWidth = 0;
            sceneLinearFloatHeight = 0;
            if (warnings != nullptr) {
                warnings->append(QStringLiteral("%1: invalid Image Empty scene-linear image data was ignored")
                                     .arg(blenderItemLabel(item)));
            }
        }
    }
    const QString sourceTransformError = item.value(
        QStringLiteral("picture_source_transform_error")).toString();
    if (!sourceTransformError.isEmpty() && warnings != nullptr) {
        warnings->append(QStringLiteral("%1: %2")
                             .arg(blenderItemLabel(item),
                                  sourceTransformError));
    }
    QVector<double> pictureColor;
    if (!jsonNumberArray(item.value(QStringLiteral("picture_color_linear")), 4,
                         &pictureColor)) {
        setError(errorMessage, QStringLiteral("Image Empty color is invalid"));
        return false;
    }

    classiCAD::SceneObject result;
    result.displayName = item.value(QStringLiteral("name")).toString();
    result.libraryPath = item.value(QStringLiteral("library_path")).toString();
    result.workbenchRandomLibraryPath = item.value(
        QStringLiteral("workbench_random_library_path")).toString();
    result.showInFront = item.value(QStringLiteral("showInFront")).toBool(false);
    result.geometry.geometryType = classiCAD::GeometryType::Picture;
    result.geometry.workPlane = classiCAD::WorkPlane::XY;
    result.geometry.workPlaneOffset = 0.0;
    result.geometry.workPlaneFrame = frame;
    for (const classiCAD::Point3D &corner : corners) {
        result.geometry.points.append(
            classiCAD::worldPointToWorkPlaneFrame(corner, frame));
    }
    result.geometry.pictureImage = image;
    result.geometry.pictureImageData = imageBytes;
    result.geometry.pictureColorSpace = pictureColorSpace;
    result.geometry.pictureSourceTransformId = pictureSourceTransformId;
    result.geometry.pictureSceneLinearFloatData = sceneLinearFloatData;
    result.geometry.pictureSceneLinearFloatWidth = sceneLinearFloatWidth;
    result.geometry.pictureSceneLinearFloatHeight = sceneLinearFloatHeight;
    result.geometry.pictureColorSceneLinear = QVector4D(
        float(pictureColor[0]), float(pictureColor[1]),
        float(pictureColor[2]), float(pictureColor[3]));
    result.geometry.pictureUseAlphaBlend = item.value(
        QStringLiteral("picture_use_alpha_blend")).toBool(true);
    // This records Blender's image setting, independent of the storage format
    // selected by the active Qt image plugin.
    result.geometry.picturePremultipliedAlpha = item.value(
        QStringLiteral("picture_premultiplied_alpha")).toBool(false);
    result.geometry.pictureShowPerspective = item.value(
        QStringLiteral("show_empty_image_perspective")).toBool(true);
    result.geometry.pictureShowOrthographic = item.value(
        QStringLiteral("show_empty_image_orthographic")).toBool(true);
    result.geometry.pictureOnlyAxisAligned = item.value(
        QStringLiteral("show_empty_image_only_axis_aligned")).toBool(false);
    const QString imageSide = item.value(
        QStringLiteral("empty_image_side")).toString(
            QStringLiteral("DOUBLE_SIDED"));
    if (imageSide == QLatin1String("FRONT")) {
        result.geometry.pictureImageSide = classiCAD::PictureImageSide::Front;
    } else if (imageSide == QLatin1String("BACK")) {
        result.geometry.pictureImageSide = classiCAD::PictureImageSide::Back;
    } else if (imageSide != QLatin1String("DOUBLE_SIDED")) {
        setError(errorMessage, QStringLiteral("Image Empty side setting is invalid"));
        return false;
    }
    const QString imageDepth = item.value(
        QStringLiteral("empty_image_depth")).toString(
            QStringLiteral("DEFAULT"));
    if (imageDepth != QLatin1String("DEFAULT") &&
        imageDepth != QLatin1String("FRONT") &&
        imageDepth != QLatin1String("BACK")) {
        setError(errorMessage, QStringLiteral("Image Empty depth setting is invalid"));
        return false;
    }
    if (imageDepth == QLatin1String("FRONT")) {
        result.geometry.pictureImageDepth =
            classiCAD::PictureImageDepth::Front;
    } else if (imageDepth == QLatin1String("BACK")) {
        result.geometry.pictureImageDepth =
            classiCAD::PictureImageDepth::Back;
    }
    QVector<double> objectColor;
    if (!jsonNumberArray(item.value(QStringLiteral("display_color_linear")), 4,
                         &objectColor)) {
        setError(errorMessage, QStringLiteral("Image Empty display color is invalid"));
        return false;
    }
    result.displayColorSceneLinear = QVector4D(
        float(objectColor[0]), float(objectColor[1]),
        float(objectColor[2]), float(objectColor[3]));
    for (const QJsonValue &collectionValue :
         item.value(QStringLiteral("collection_ids")).toArray()) {
        const QString sourceId = collectionValue.toString();
        if (sourceId.isEmpty() || !collectionIds.contains(sourceId)) {
            setError(errorMessage,
                     QStringLiteral("Image Empty references an unknown Blender collection"));
            return false;
        }
        result.collectionIds.append(collectionIds.value(sourceId));
    }
    const QJsonObject visibility = item.value(QStringLiteral("visibility")).toObject();
    result.hideViewport = visibility.value(QStringLiteral("hide_viewport")).toBool();
    result.hideRender = visibility.value(QStringLiteral("hide_render")).toBool();
    result.hiddenInViewport = visibility.value(QStringLiteral("hidden_in_viewport")).toBool();
    result.hideSelect = visibility.value(QStringLiteral("hide_select")).toBool();
    result.viewLayerHidden = item.value(QStringLiteral("view_layer_hidden")).toObject();
    result.defaultViewLayerKey = item.value(
        QStringLiteral("default_view_layer_key")).toString();
    if (!result.defaultViewLayerKey.isEmpty() &&
        result.viewLayerHidden.contains(result.defaultViewLayerKey)) {
        result.hiddenInViewport = false;
    }
    *sceneObject = std::move(result);
    return true;
}

bool parseBlenderScalarVolumeItem(
    const QJsonObject &item,
    const QHash<QString, classiCAD::CollectionId> &collectionIds,
    classiCAD::SceneObject *sceneObject,
    QString *errorMessage)
{
    if (sceneObject == nullptr) return false;
    const QJsonObject source = item.value(QStringLiteral("volume_grid")).toObject();
    const QJsonObject display = item.value(QStringLiteral("volume_display")).toObject();
    classiCAD::ScalarVolume3D volume;
    volume.name = source.value(QStringLiteral("name")).toString();
    volume.dataType = source.value(QStringLiteral("data_type")).toString();
    const auto readMatrix = [&](const QJsonValue &value,
                                classiCAD::ScalarVolumeMatrix *matrix) {
        if (!value.isArray() || value.toArray().size() != 4) return false;
        const QJsonArray rows = value.toArray();
        for (int row = 0; row < 4; ++row) {
            if (!rows[row].isArray() || rows[row].toArray().size() != 4) return false;
            const QJsonArray columns = rows[row].toArray();
            for (int column = 0; column < 4; ++column) {
                if (!columns[column].isDouble() || !std::isfinite(columns[column].toDouble())) return false;
                (*matrix)[static_cast<size_t>(row * 4 + column)] = columns[column].toDouble();
            }
        }
        return classiCAD::isValidScalarVolumeMatrix(*matrix);
    };
    const QJsonValue channels = source.value(QStringLiteral("channels"));
    const QJsonValue background = source.value(QStringLiteral("background"));
    const QJsonValue expanded = source.value(QStringLiteral("expanded_voxel_count"));
    const QJsonValue dense = source.value(QStringLiteral("dense_voxel_count"));
    const QJsonArray bounds = source.value(QStringLiteral("active_bounds")).toArray();
    if (volume.name.isEmpty() || !channels.isDouble() ||
        std::floor(channels.toDouble()) != channels.toDouble() ||
        !background.isDouble() || !std::isfinite(background.toDouble()) ||
        std::abs(background.toDouble()) > std::numeric_limits<float>::max() ||
        !expanded.isDouble() || std::floor(expanded.toDouble()) != expanded.toDouble() ||
        expanded.toDouble() < 0.0 || expanded.toDouble() > 64.0 * 1024.0 * 1024.0 ||
        !dense.isDouble() ||
        std::floor(dense.toDouble()) != dense.toDouble() || dense.toDouble() < 0.0 ||
        dense.toDouble() > 64.0 * 1024.0 * 1024.0 ||
        bounds.size() != 6 ||
        !readMatrix(source.value(QStringLiteral("grid_to_object")), &volume.gridToObject) ||
        !readMatrix(source.value(QStringLiteral("object_to_world")), &volume.objectToWorld)) {
        setError(errorMessage, QStringLiteral("scalar volume grid metadata is invalid"));
        return false;
    }
    volume.channels = channels.toInt();
    volume.background = static_cast<float>(background.toDouble());
    volume.expandedVoxelCount = static_cast<std::uint64_t>(expanded.toDouble());
    volume.denseVoxelCount = static_cast<std::uint64_t>(dense.toDouble());
    for (int i = 0; i < 6; ++i) {
        int coordinate = 0;
        if (!jsonInteger(bounds[i], &coordinate)) {
            setError(errorMessage, QStringLiteral("scalar volume active bounds are invalid"));
            return false;
        }
        volume.activeBounds[static_cast<size_t>(i)] = coordinate;
    }
    const QJsonArray records = source.value(QStringLiteral("active_records")).toArray();
    if (records.isEmpty() || records.size() > 1'000'000) {
        setError(errorMessage, QStringLiteral("scalar volume active record count is invalid"));
        return false;
    }
    volume.activeRecords.reserve(records.size());
    for (const QJsonValue &recordValue : records) {
        if (!recordValue.isArray() || recordValue.toArray().size() != 6) {
            setError(errorMessage, QStringLiteral("scalar volume active record is malformed"));
            return false;
        }
        const QJsonArray record = recordValue.toArray();
        int values[4]{};
        for (int i = 0; i < 4; ++i) {
            if (!jsonInteger(record[i], &values[i])) {
                setError(errorMessage, QStringLiteral("scalar volume voxel coordinate is invalid"));
                return false;
            }
        }
        if (!record[4].isBool() || !record[5].isDouble() ||
            !std::isfinite(record[5].toDouble()) ||
            std::abs(record[5].toDouble()) > std::numeric_limits<float>::max()) {
            setError(errorMessage, QStringLiteral("scalar volume voxel value is invalid"));
            return false;
        }
        volume.activeRecords.append({values[0], values[1], values[2], values[3],
                                     record[4].toBool(), static_cast<float>(record[5].toDouble())});
    }
    const QJsonValue density = display.value(QStringLiteral("density"));
    const QJsonValue sliceDepth = display.value(QStringLiteral("slice_depth"));
    const QJsonValue renderSpace = display.value(QStringLiteral("render_space"));
    if (!density.isDouble() || !std::isfinite(density.toDouble()) ||
        density.toDouble() < 0.0 || density.toDouble() > std::numeric_limits<float>::max() ||
        !renderSpace.isString() ||
        !display.value(QStringLiteral("interpolation_method")).isString() ||
        !display.value(QStringLiteral("use_slice")).isBool() ||
        !display.value(QStringLiteral("slice_axis")).isString() ||
        !sliceDepth.isDouble() || !std::isfinite(sliceDepth.toDouble()) ||
        std::abs(sliceDepth.toDouble()) > std::numeric_limits<float>::max()) {
        setError(errorMessage, QStringLiteral("scalar volume display settings are invalid"));
        return false;
    }
    volume.displayDensity = static_cast<float>(density.toDouble());
    volume.renderSpace = renderSpace.toString();
    if (!classiCAD::scalarVolumeDensityScale(volume.renderSpace,
                                              volume.objectToWorld,
                                              volume.displayDensity,
                                              &volume.densityScale)) {
        setError(errorMessage, QStringLiteral("scalar volume density scale is invalid"));
        return false;
    }
    volume.interpolationMethod = display.value(QStringLiteral("interpolation_method")).toString();
    volume.useSlice = display.value(QStringLiteral("use_slice")).toBool();
    volume.sliceAxis = display.value(QStringLiteral("slice_axis")).toString();
    volume.sliceDepth = static_cast<float>(sliceDepth.toDouble());
    if (!classiCAD::validateScalarVolume(volume)) {
        setError(errorMessage, QStringLiteral("scalar volume payload exceeds limits or is inconsistent"));
        return false;
    }

    classiCAD::SceneObject result;
    result.displayName = item.value(QStringLiteral("name")).toString();
    result.libraryPath = item.value(QStringLiteral("library_path")).toString();
    result.workbenchRandomLibraryPath =
        item.value(QStringLiteral("workbench_random_library_path")).toString();
    result.showInFront = item.value(QStringLiteral("showInFront")).toBool(false);
    result.showWire = item.value(QStringLiteral("show_wire")).toBool(false);
    result.showAllEdges = item.value(QStringLiteral("show_all_edges")).toBool(false);
    result.wireDisplay = item.value(QStringLiteral("wire_display")).toBool(false);
    result.displayBoundsType = item.value(QStringLiteral("bounds_display_type")).toString();
    if (!parseDisplayBoundsData(item, &result, errorMessage)) return false;
    result.hideViewport = item.value(QStringLiteral("hide_viewport")).toBool(false);
    result.hideRender = item.value(QStringLiteral("hide_render")).toBool(false);
    result.hideSelect = item.value(QStringLiteral("hide_select")).toBool(false);
    result.hiddenInViewport = result.hideViewport;
    result.viewLayerHidden = item.value(QStringLiteral("view_layer_hidden")).toObject();
    QVector<double> objectColor;
    if (!jsonNumberArray(item.value(QStringLiteral("display_color_linear")), 4, &objectColor)) {
        setError(errorMessage, QStringLiteral("volume display color is invalid"));
        return false;
    }
    if (std::any_of(objectColor.cbegin(), objectColor.cend(), [](double value) {
            return std::abs(value) > std::numeric_limits<float>::max();
        })) {
        setError(errorMessage, QStringLiteral("volume display color exceeds float range"));
        return false;
    }
    result.displayColorSceneLinear = QVector4D(static_cast<float>(objectColor[0]),
        static_cast<float>(objectColor[1]), static_cast<float>(objectColor[2]),
        static_cast<float>(objectColor[3]));
    for (const QJsonValue &collectionValue : item.value(QStringLiteral("collection_ids")).toArray()) {
        const QString collectionId = collectionValue.toString();
        if (collectionId.isEmpty() || !collectionIds.contains(collectionId)) {
            setError(errorMessage, QStringLiteral("volume references an unknown Blender collection"));
            return false;
        }
        result.collectionIds.append(collectionIds.value(collectionId));
    }
    result.geometry.geometryType = classiCAD::GeometryType::ScalarVolume;
    result.geometry.scalarVolume = std::move(volume);
    *sceneObject = std::move(result);
    return true;
}

QString conciseImportWarnings(const QStringList &warnings,
                              int skippedObjects,
                              int unsupportedFeatures)
{
    QStringList lines;
    if (skippedObjects > 0) {
        lines.append(QStringLiteral("Skipped %1 unsupported or invalid object(s).")
                         .arg(skippedObjects));
    }
    if (unsupportedFeatures > 0) {
        lines.append(QStringLiteral("%1 unsupported feature(s) could not be represented.")
                         .arg(unsupportedFeatures));
    }
    constexpr int maximumExamples = 8;
    for (int index = 0; index < warnings.size() && index < maximumExamples; ++index) {
        lines.append(QStringLiteral("• %1").arg(warnings[index]));
    }
    if (warnings.size() > maximumExamples) {
        lines.append(QStringLiteral("• and %1 more item details")
                         .arg(warnings.size() - maximumExamples));
    }
    return lines.join(QLatin1Char('\n'));
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
    if (!addSurfaceDisplayMeshes(document,
                                &serializedDocument,
                                errorMessage)) {
        return false;
    }
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
                         QString *errorMessage,
                         BlenderSceneImportReport *ordinaryImportReport)
{
    if (document == nullptr || path.trimmed().isEmpty()) {
        setError(errorMessage, QStringLiteral("A destination document and project file path are required"));
        return false;
    }
    if (ordinaryImportReport != nullptr) {
        *ordinaryImportReport = BlenderSceneImportReport{};
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

    if (serialized.object().value(QStringLiteral("format")).toString() ==
        QLatin1String("classiCAD.ordinary-blend")) {
        QVector<SceneObject> objects;
        QVector<DocumentCollection> collections;
        QVector<DocumentMaterial> materials;
        BlenderSceneImportReport importReport;
        if (!importBlenderScene(path, QStringLiteral("original"), &objects,
                                &collections, &materials, &importReport,
                                errorMessage)) {
            return false;
        }
        Document restored;
        Document::Snapshot snapshot = restored.snapshot();
        const LayerId targetLayer = snapshot.activeLayerId;
        quint64 nextObjectValue = 1;
        for (SceneObject &object : objects) {
            object.id = ObjectId::fromValue(nextObjectValue++);
            object.layerId = targetLayer;
        }
        for (const int index : importReport.selectedObjectIndices) {
            if (index >= 0 && index < objects.size()) {
                importReport.selectedObjectIds.append(objects[index].id);
            }
        }
        if (importReport.activeObjectIndex >= 0 &&
            importReport.activeObjectIndex < objects.size()) {
            const ObjectId activeId =
                objects[importReport.activeObjectIndex].id;
            if (importReport.selectedObjectIds.contains(activeId)) {
                importReport.activeObjectId = activeId;
            }
        }
        snapshot.collections = std::move(collections);
        snapshot.objects = std::move(objects);
        snapshot.materials = std::move(materials);
        snapshot.nextObjectValue = nextObjectValue;
        snapshot.nextCollectionValue = quint64(snapshot.collections.size()) + 1;
        snapshot.settings.importedBlenderScenes = importReport.scenes;
        snapshot.settings.importedBlenderColorLuts =
            importReport.importedBlenderColorLuts;
        snapshot.settings.importedBlenderColorLutMetadata =
            importReport.importedBlenderColorLutMetadata;
        snapshot.settings.activeViewLayerKey = importReport.activeViewLayerKey;
        restored.restoreSnapshot(snapshot);
        *document = std::move(restored);
        if (cameraSettings != nullptr) *cameraSettings = ProjectViewportCameraSettings{};
        if (ordinaryImportReport != nullptr)
            *ordinaryImportReport = std::move(importReport);
        if (errorMessage != nullptr) errorMessage->clear();
        return true;
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

bool importBlenderScene(const QString &path,
                        const QString &mode,
                        QVector<SceneObject> *objects,
                        QVector<DocumentCollection> *collections,
                        QVector<DocumentMaterial> *materials,
                        BlenderSceneImportReport *report,
                        QString *errorMessage)
{
    if (report != nullptr) {
        *report = BlenderSceneImportReport{};
    }
    if (objects == nullptr || collections == nullptr || materials == nullptr || path.trimmed().isEmpty() ||
        (mode != QLatin1String("original") &&
         mode != QLatin1String("evaluated"))) {
        setError(errorMessage,
                 QStringLiteral("A Blender .blend path, import mode, and object destination are required"));
        return false;
    }
    const QFileInfo sourceInfo(path);
    if (!sourceInfo.isFile() ||
        sourceInfo.suffix().compare(QStringLiteral("blend"), Qt::CaseInsensitive) != 0) {
        setError(errorMessage,
                 QStringLiteral("Choose a Blender .blend file to import as a scene"));
        return false;
    }

    QTemporaryDir temporaryDirectory;
    if (!temporaryDirectory.isValid()) {
        setError(errorMessage,
                 QStringLiteral("Could not create temporary Blender scene data"));
        return false;
    }
    const QString extractedPath =
        temporaryDirectory.filePath(QStringLiteral("scene.json"));
    if (!runBlenderSceneExtractor(sourceInfo.absoluteFilePath(), extractedPath,
                                  mode, errorMessage)) {
        return false;
    }
    QFile extractedFile(extractedPath);
    if (!extractedFile.open(QIODevice::ReadOnly)) {
        setError(errorMessage,
                 QStringLiteral("Blender did not return extracted scene data"));
        return false;
    }
    if (extractedFile.size() < 0 ||
        extractedFile.size() > kBlenderExtractionJsonByteLimit) {
        setError(errorMessage,
                 QStringLiteral("Blender scene extraction exceeded the 256 MiB JSON transport limit"));
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument extraction =
        QJsonDocument::fromJson(extractedFile.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !extraction.isObject()) {
        setError(errorMessage,
                 QStringLiteral("The Blender scene extraction JSON is invalid: %1")
                     .arg(parseError.errorString()));
        return false;
    }
    const QJsonObject root = extraction.object();
    int bridgeVersion = -1;
    if (root.value(QStringLiteral("format")).toString() !=
            QLatin1String("classiCAD.blender-scene-extract") ||
        !jsonInteger(root.value(QStringLiteral("format_version")), &bridgeVersion) ||
        (bridgeVersion != 10 && bridgeVersion != 11 && bridgeVersion != 12 &&
         bridgeVersion != 13 && bridgeVersion != 14 && bridgeVersion != 15 &&
         bridgeVersion != 16 && bridgeVersion != 17 && bridgeVersion != 18 &&
         bridgeVersion != 19)) {
        setError(errorMessage,
                 QStringLiteral("The Blender scene extraction format is unsupported"));
        return false;
    }
    const QJsonObject source = root.value(QStringLiteral("source")).toObject();
    QString activeSceneId = root.value(QStringLiteral("active_scene_id")).toString();
    QString activeViewLayerKey = root.value(
        QStringLiteral("active_view_layer_key")).toString();
    const QString sourceVersion =
        source.value(QStringLiteral("blender_version")).toString();
    const QJsonArray version =
        source.value(QStringLiteral("blender_version_tuple")).toArray();
    int major = -1;
    int minor = -1;
    int patch = -1;
    if (version.size() != 3 || !jsonInteger(version[0], &major) ||
        !jsonInteger(version[1], &minor) || !jsonInteger(version[2], &patch) ||
        major != 5 || minor != 2 || patch != 2 ||
        (sourceVersion != QLatin1String(kPinnedBlenderVersion) &&
         sourceVersion != QStringLiteral("5.2.2 LTS")) ||
        source.value(QStringLiteral("import_mode")).toString() != mode) {
        setError(errorMessage,
                 QStringLiteral("The extraction was not produced by Blender 5.2.2 in the requested mode"));
        return false;
    }
    const QJsonObject sourceMaterials = root.value(QStringLiteral("materials")).toObject();
    if (!root.value(QStringLiteral("items")).isArray() ||
        !root.value(QStringLiteral("collections")).isArray() ||
        !root.value(QStringLiteral("unsupported_items")).isArray() ||
        !root.value(QStringLiteral("unsupported_features")).isArray() ||
        !root.value(QStringLiteral("materials")).isObject()) {
        setError(errorMessage,
                 QStringLiteral("The Blender scene extraction is missing required collections"));
        return false;
    }
    const QJsonArray sourceItems = root.value(QStringLiteral("items")).toArray();
    const QJsonArray unsupportedItems =
        root.value(QStringLiteral("unsupported_items")).toArray();
    const QJsonArray unsupportedFeatures =
        root.value(QStringLiteral("unsupported_features")).toArray();
    QStringList warnings;
    QHash<QString, QByteArray> importedBlenderColorLuts;
    QHash<QString, ImportedBlenderColorLutMetadata> importedBlenderColorLutMetadata;
    const QJsonValue colorLutsValue = root.value(QStringLiteral("color_luts"));
    if (bridgeVersion >= 11 && !colorLutsValue.isArray()) {
        setError(errorMessage, QStringLiteral("The Blender scene extraction has no color LUT table"));
        return false;
    }
    qsizetype importedLutBytes = 0;
    for (const QJsonValue &lutValue : colorLutsValue.toArray()) {
        if (!lutValue.isObject()) {
            warnings.append(QStringLiteral("A Blender scene color LUT record is invalid and was skipped"));
            continue;
        }
        const QJsonObject lut = lutValue.toObject();
        const QString id = lut.value(QStringLiteral("id")).toString();
        const QString encoded = lut.value(QStringLiteral("data_base64")).toString();
        const bool idValid = id.size() == 64 && std::all_of(
            id.cbegin(), id.cend(), [](QChar c) {
                return (c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
                       (c >= QLatin1Char('a') && c <= QLatin1Char('f'));
            });
        const QByteArray base64 = encoded.toLatin1();
        const auto decoding = QByteArray::fromBase64Encoding(
            base64, QByteArray::AbortOnBase64DecodingErrors);
        const QByteArray bytes = decoding.decoded;
        const QJsonValue sizeValue = lut.value(QStringLiteral("size"));
        const int size = sizeValue.toInt(-1);
        const QJsonValue domainMinValue = lut.value(QStringLiteral("domain_min"));
        const QJsonValue domainMaxValue = lut.value(QStringLiteral("domain_max"));
        const double domainMin = domainMinValue.toDouble(
            std::numeric_limits<double>::quiet_NaN());
        const double domainMax = domainMaxValue.toDouble(
            std::numeric_limits<double>::quiet_NaN());
        const QString shaper = lut.value(QStringLiteral("shaper")).toString(
            QStringLiteral("linear"));
        const QJsonValue shaperScaleValue = lut.value(QStringLiteral("shaper_scale"));
        const double shaperScale = bridgeVersion >= 12
            ? shaperScaleValue.toDouble(std::numeric_limits<double>::quiet_NaN())
            : 1.0;
        const QString digest = QString::fromLatin1(
            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        const bool metadataValid = idValid && sizeValue.isDouble() &&
            std::floor(sizeValue.toDouble()) == size && size >= 2 && size <= 140 &&
            domainMinValue.isDouble() && std::isfinite(domainMin) &&
            domainMaxValue.isDouble() && std::isfinite(domainMax) &&
            domainMax > domainMin &&
            (bridgeVersion < 12 ||
             (lut.value(QStringLiteral("shaper")).isString() &&
              shaperScaleValue.isDouble())) &&
            (shaper == QStringLiteral("linear") ||
             shaper == QStringLiteral("log2_1p")) &&
            std::isfinite(shaperScale) && shaperScale > 0.0 &&
            lut.value(QStringLiteral("encoding")).toString() ==
                QLatin1String("rgb-f32-le-base64");
        if (!metadataValid || decoding.decodingStatus != QByteArray::Base64DecodingStatus::Ok ||
            !isFiniteColorLutPayload(bytes, size) || digest != id) {
            warnings.append(QStringLiteral("Blender color LUT %1 is invalid (expected a verified RGB float LUT with valid size and domain metadata) and was skipped")
                                .arg(id.isEmpty() ? QStringLiteral("<missing id>") : id));
            continue;
        }
        if (importedBlenderColorLuts.contains(id)) {
            if (importedBlenderColorLutMetadata.value(id) !=
                ImportedBlenderColorLutMetadata{
                    size, domainMin, domainMax, shaper, shaperScale}) {
                warnings.append(QStringLiteral("Blender color LUT %1 repeats an existing payload with conflicting metadata; the later record was skipped")
                                    .arg(id));
            }
            continue;
        }
        if (importedBlenderColorLuts.size() >= 8 ||
            importedLutBytes + bytes.size() > kColorLutTotalByteLimit) {
            warnings.append(QStringLiteral("Blender color LUT %1 exceeds the import limit of 8 LUTs or 32 MiB and was skipped").arg(id));
            continue;
        }
        importedLutBytes += bytes.size();
        importedBlenderColorLuts.insert(id, bytes);
        importedBlenderColorLutMetadata.insert(
            id, {size, domainMin, domainMax, shaper, shaperScale});
    }
    QVector<SceneObject> imported;
    QVector<DocumentCollection> importedCollections;
    QVector<DocumentMaterial> importedMaterials;
    QHash<QString, DocumentMaterial> materialsById;
    QSet<QString> importedIds;
    QHash<QString, int> importedIndexBySourceId;
    QVector<QString> sourceObjectIdsByImportedIndex;
    QHash<QString, CollectionId> collectionIdsBySource;
    QHash<QString, QString> sceneLibraryPaths;
    const QJsonArray sourceScenes = root.value(QStringLiteral("scenes")).toArray();
    QJsonArray importedSceneRecords;
    if (activeSceneId.isEmpty() || activeViewLayerKey.isEmpty()) {
        for (const QJsonValue &sceneValue : sourceScenes) {
            const QJsonObject scene = sceneValue.toObject();
            const QJsonArray viewLayers = scene.value(
                QStringLiteral("view_layers")).toArray();
            if (viewLayers.isEmpty()) continue;
            activeSceneId = scene.value(QStringLiteral("scene_id")).toString();
            activeViewLayerKey = viewLayers.first().toObject().value(
                QStringLiteral("key")).toString();
            break;
        }
    }
    for (const QJsonValue &sceneValue : sourceScenes) {
        if (!sceneValue.isObject()) {
            setError(errorMessage,
                     QStringLiteral("A Blender scene record is invalid"));
            return false;
        }
        const QJsonObject scene = sceneValue.toObject();
        if (scene.value(QStringLiteral("scene_id")).toString().isEmpty() ||
            !scene.value(QStringLiteral("name")).isString() ||
            !scene.value(QStringLiteral("view_layers")).isArray()) {
            setError(errorMessage,
                     QStringLiteral("A Blender scene record is incomplete"));
            return false;
        }
        const QJsonObject colorManagement =
            scene.value(QStringLiteral("color_management")).toObject();
        const QJsonValue lutIdValue = colorManagement.value(QStringLiteral("lut_id"));
        const QString lutId = lutIdValue.toString();
        if (!lutIdValue.isUndefined() &&
            (!lutIdValue.isString() ||
             (!lutId.isEmpty() && !importedBlenderColorLuts.contains(lutId)))) {
            warnings.append(QStringLiteral("Scene %1 references color LUT %2, which is missing or invalid; scene metadata was retained")
                                .arg(scene.value(QStringLiteral("name")).toString(),
                                     lutId.isEmpty() ? QStringLiteral("<invalid id>") : lutId));
        }
        const QJsonObject renderOutput =
            scene.value(QStringLiteral("render_output")).toObject();
        const QJsonValue renderAaValue =
            renderOutput.value(QStringLiteral("render_aa"));
        if (!renderAaValue.isUndefined() &&
            (!renderAaValue.isString() ||
             (renderAaValue.toString() != QLatin1String("OFF") &&
              renderAaValue.toString() != QLatin1String("FXAA") &&
              renderAaValue.toString() != QLatin1String("5") &&
              renderAaValue.toString() != QLatin1String("8") &&
              renderAaValue.toString() != QLatin1String("11") &&
              renderAaValue.toString() != QLatin1String("16") &&
              renderAaValue.toString() != QLatin1String("32")))) {
            warnings.append(QStringLiteral("Scene %1 has an unsupported render anti-aliasing value; the viewport preference will be used")
                                .arg(scene.value(QStringLiteral("name")).toString()));
        }
        int resolutionX = 0;
        int resolutionY = 0;
        int resolutionPercentage = 0;
        const QJsonValue pixelAspectXValue =
            renderOutput.value(QStringLiteral("pixel_aspect_x"));
        const QJsonValue pixelAspectYValue =
            renderOutput.value(QStringLiteral("pixel_aspect_y"));
        const QStringList borderBooleanKeys{
            QStringLiteral("use_border"),
            QStringLiteral("use_crop_to_border")};
        bool borderSettingsValid = true;
        for (const QString &key : borderBooleanKeys) {
            const QJsonValue value = renderOutput.value(key);
            borderSettingsValid &= value.isUndefined() || value.isBool();
        }
        const QStringList borderCoordinateKeys{
            QStringLiteral("border_min_x"), QStringLiteral("border_min_y"),
            QStringLiteral("border_max_x"), QStringLiteral("border_max_y")};
        qreal borderCoordinates[4] = {0.0, 0.0, 1.0, 1.0};
        for (qsizetype i = 0; i < borderCoordinateKeys.size(); ++i) {
            const QJsonValue value = renderOutput.value(borderCoordinateKeys[i]);
            if (value.isUndefined()) continue;
            if (!value.isDouble() || !std::isfinite(value.toDouble())) {
                borderSettingsValid = false;
                continue;
            }
            borderCoordinates[i] = value.toDouble();
        }
        borderSettingsValid &= borderCoordinates[0] >= 0.0 &&
            borderCoordinates[1] >= 0.0 && borderCoordinates[2] <= 1.0 &&
            borderCoordinates[3] <= 1.0 &&
            borderCoordinates[0] < borderCoordinates[2] &&
            borderCoordinates[1] < borderCoordinates[3];
        const QJsonValue ditherIntensityValue =
            renderOutput.value(QStringLiteral("dither_intensity"));
        const QJsonValue colorDepthValue =
            renderOutput.value(QStringLiteral("color_depth"));
        bool colorDepthValid = false;
        const int colorDepth = colorDepthValue.toString().toInt(&colorDepthValid);
        if (!jsonInteger(renderOutput.value(QStringLiteral("resolution_x")),
                         &resolutionX) || resolutionX <= 0 ||
            !jsonInteger(renderOutput.value(QStringLiteral("resolution_y")),
                         &resolutionY) || resolutionY <= 0 ||
            !jsonInteger(renderOutput.value(
                             QStringLiteral("resolution_percentage")),
                         &resolutionPercentage) ||
            resolutionPercentage <= 0 || resolutionPercentage > 32767 ||
            !pixelAspectXValue.isDouble() || !pixelAspectYValue.isDouble() ||
            !std::isfinite(pixelAspectXValue.toDouble()) ||
            !std::isfinite(pixelAspectYValue.toDouble()) ||
            pixelAspectXValue.toDouble() <= 0.0 ||
            pixelAspectYValue.toDouble() <= 0.0 || !borderSettingsValid) {
            setError(errorMessage,
                     QStringLiteral("A Blender scene render-output record is invalid"));
            return false;
        }
        if (!ditherIntensityValue.isUndefined() &&
            (!ditherIntensityValue.isDouble() ||
             !std::isfinite(ditherIntensityValue.toDouble()) ||
             ditherIntensityValue.toDouble() < 0.0)) {
            setError(errorMessage,
                     QStringLiteral("A Blender scene render-output dither value is invalid"));
            return false;
        }
        const QString imageColorMode = renderOutput.value(
            QStringLiteral("color_mode")).toString();
        const QJsonValue exrCodecValue = renderOutput.value(
            QStringLiteral("exr_codec"));
        const QString exrCodec = exrCodecValue.isUndefined()
                                     ? QStringLiteral("ZIP")
                                     : exrCodecValue.toString();
        const bool exrCodecValid = exrCodecValue.isUndefined() ||
            (exrCodecValue.isString() &&
             (exrCodec == QLatin1String("NONE") ||
              exrCodec == QLatin1String("PXR24") ||
              exrCodec == QLatin1String("ZIP") ||
              exrCodec == QLatin1String("PIZ") ||
              exrCodec == QLatin1String("RLE") ||
              exrCodec == QLatin1String("ZIPS") ||
              exrCodec == QLatin1String("B44") ||
              exrCodec == QLatin1String("B44A") ||
              exrCodec == QLatin1String("DWAA") ||
              exrCodec == QLatin1String("DWAB") ||
              exrCodec == QLatin1String("HTJ2K")));
        const QJsonValue filmTransparentValue = renderOutput.value(
            QStringLiteral("film_transparent"));
        const QJsonValue worldColorValue = renderOutput.value(
            QStringLiteral("world_color_scene_linear"));
        const QJsonValue renderBackgroundTypeValue = renderOutput.value(
            QStringLiteral("background_type"));
        const QJsonValue renderFilepathValue = renderOutput.value(
            QStringLiteral("filepath"));
        const QJsonValue useFileExtensionValue = renderOutput.value(
            QStringLiteral("use_file_extension"));
        QVector<double> worldColorSceneLinear;
        bool worldColorValid = worldColorValue.isUndefined() ||
                               worldColorValue.isNull();
        if (worldColorValue.isArray()) {
            worldColorValid = jsonNumberArray(worldColorValue, 3,
                                              &worldColorSceneLinear);
        }
        if (!renderOutput.value(QStringLiteral("file_format")).isString() ||
            (!renderFilepathValue.isUndefined() &&
             !renderFilepathValue.isString()) ||
            (!useFileExtensionValue.isUndefined() &&
             !useFileExtensionValue.isBool()) ||
            !exrCodecValid ||
            (!filmTransparentValue.isUndefined() &&
             !filmTransparentValue.isBool()) ||
            !worldColorValid ||
            (!renderBackgroundTypeValue.isUndefined() &&
             (!renderBackgroundTypeValue.isString() ||
              (renderBackgroundTypeValue.toString() != QLatin1String("THEME") &&
               renderBackgroundTypeValue.toString() != QLatin1String("WORLD") &&
               renderBackgroundTypeValue.toString() != QLatin1String("VIEWPORT")))) ||
            (imageColorMode != QLatin1String("BW") &&
             imageColorMode != QLatin1String("RGB") &&
             imageColorMode != QLatin1String("RGBA")) ||
            !colorDepthValid ||
            (colorDepth != 8 && colorDepth != 16 && colorDepth != 32)) {
            setError(errorMessage,
                     QStringLiteral("A Blender scene image-output record is invalid"));
            return false;
        }
        importedSceneRecords.append(scene);
        sceneLibraryPaths.insert(scene.value(QStringLiteral("name")).toString(),
                                 scene.value(QStringLiteral("library_path")).toString());
    }
    const QJsonArray sourceCollections = root.value(QStringLiteral("collections")).toArray();
    importedCollections.reserve(sourceCollections.size());
    quint64 nextCollectionId = 1;
    for (const QJsonValue &collectionValue : sourceCollections) {
        if (!collectionValue.isObject()) {
            setError(errorMessage, QStringLiteral("A Blender collection record is invalid"));
            return false;
        }
        const QJsonObject record = collectionValue.toObject();
        const QString sourceId = record.value(QStringLiteral("collection_id")).toString();
        if (sourceId.isEmpty() || collectionIdsBySource.contains(sourceId)) {
            setError(errorMessage, QStringLiteral("A Blender collection identity is missing or duplicated"));
            return false;
        }
        DocumentCollection collection;
        collection.id = CollectionId::fromValue(nextCollectionId++);
        collection.sourceId = sourceId;
        collection.name = record.value(QStringLiteral("name")).toString();
        collection.libraryPath = record.value(QStringLiteral("library_path")).toString();
        collection.hideViewport = record.value(QStringLiteral("hide_viewport")).toBool();
        collection.hideRender = record.value(QStringLiteral("hide_render")).toBool();
        collection.viewLayerStates = record.value(QStringLiteral("view_layer_states")).toArray();
        const QJsonArray sourceSceneNames = record.value(QStringLiteral("source_scene_names")).toArray();
        if (!sourceSceneNames.isEmpty()) {
            for (const QJsonValue &sceneNameValue : sourceSceneNames) {
                collection.sourceSceneNames.append(sceneNameValue.toString());
            }
            collection.sourceSceneName = collection.sourceSceneNames.first();
            collection.sourceSceneLibraryPath = sceneLibraryPaths.value(collection.sourceSceneName);
        }
        collectionIdsBySource.insert(sourceId, collection.id);
        importedCollections.append(std::move(collection));
    }
    for (int index = 0; index < sourceCollections.size(); ++index) {
        const QJsonObject record = sourceCollections[index].toObject();
        DocumentCollection &collection = importedCollections[index];
        const QJsonArray parentSourceIds = record.value(QStringLiteral("parent_ids")).toArray();
        for (const QJsonValue &parentValue : parentSourceIds) {
            const QString parentSourceId = parentValue.toString();
            if (!collectionIdsBySource.contains(parentSourceId)) {
                setError(errorMessage, QStringLiteral("A Blender collection references an unknown parent"));
                return false;
            }
            collection.parentIds.append(collectionIdsBySource.value(parentSourceId));
        }
    }
    int skippedCount = unsupportedItems.size();
    for (const QJsonValue &unsupportedValue : unsupportedItems) {
        const QJsonObject unsupported = unsupportedValue.toObject();
        warnings.append(QStringLiteral("%1: %2")
                            .arg(blenderItemLabel(unsupported),
                                 unsupported.value(QStringLiteral("reason")).toString()));
    }
    int unsupportedFeatureCount = unsupportedFeatures.size();
    for (const QJsonValue &featureValue : unsupportedFeatures) {
        const QJsonObject feature = featureValue.toObject();
        warnings.append(QStringLiteral("%1: %2")
                            .arg(feature.value(QStringLiteral("item_id")).toString(),
                                 feature.value(QStringLiteral("reason")).toString()));
    }
    const int warningsBeforeMaterials = warnings.size();
    if (!parseBlenderMaterials(sourceMaterials, &importedMaterials,
                               &materialsById, &warnings)) {
        setError(errorMessage,
                 QStringLiteral("The extracted Blender material table is invalid"));
        return false;
    }
    unsupportedFeatureCount += warnings.size() - warningsBeforeMaterials;
    for (const QJsonValue &itemValue : sourceItems) {
        const QJsonObject item = itemValue.toObject();
        const QString objectId = item.value(QStringLiteral("object_id")).toString();
        const QString objectType = item.value(QStringLiteral("object_type")).toString();
        if (objectId.isEmpty() ||
            (objectType != QLatin1String("MESH") &&
             objectType != QLatin1String("CURVE") &&
             objectType != QLatin1String("SURFACE") &&
             objectType != QLatin1String("POINT") &&
             objectType != QLatin1String("EMPTY_IMAGE") &&
             objectType != QLatin1String("VOLUME"))) {
            ++skippedCount;
            warnings.append(QStringLiteral("A Blender scene record has missing identity or an unexpected object type and was skipped"));
            continue;
        }
        if (importedIds.contains(objectId)) {
            const auto existingIndex = importedIndexBySourceId.constFind(objectId);
            if (existingIndex != importedIndexBySourceId.cend()) {
                SceneObject &existing = imported[*existingIndex];
                for (const QJsonValue &collectionValue :
                     item.value(QStringLiteral("collection_ids")).toArray()) {
                    const QString sourceId = collectionValue.toString();
                    if (collectionIdsBySource.contains(sourceId)) {
                        const CollectionId id = collectionIdsBySource.value(sourceId);
                        if (!existing.collectionIds.contains(id))
                            existing.collectionIds.append(id);
                    }
                }
                const QJsonObject hidden =
                    item.value(QStringLiteral("view_layer_hidden")).toObject();
                for (auto it = hidden.begin(); it != hidden.end(); ++it)
                    existing.viewLayerHidden.insert(it.key(), it.value());
            }
            warnings.append(QStringLiteral("%1: linked into more than one Blender scene; shared memberships and view-layer states were combined")
                                .arg(blenderItemLabel(item)));
            continue;
        }
        const QString proxyType =
            item.value(QStringLiteral("classiCAD_proxy_geometry")).toString();
        if (!proxyType.isEmpty()) {
            ++skippedCount;
            warnings.append(QStringLiteral("%1: classiCAD %2 display proxy was skipped to preserve its authoritative geometry")
                                .arg(blenderItemLabel(item), proxyType));
            continue;
        }
        const int warningCountBeforeItem = warnings.size();
        SceneObject sceneObject;
        QString itemError;
        const bool parsed = objectType == QLatin1String("EMPTY_IMAGE")
            ? parseBlenderPictureItem(item, collectionIdsBySource,
                                      &sceneObject, &warnings, &itemError)
            : objectType == QLatin1String("VOLUME")
            ? parseBlenderScalarVolumeItem(item, collectionIdsBySource,
                                           &sceneObject, &itemError)
            : objectType == QLatin1String("MESH")
            ? parseBlenderMeshItem(
                  item, collectionIdsBySource, materialsById,
                  source.value(QStringLiteral("blender_version")).toString(),
                  mode, &sceneObject, &warnings, &itemError)
            : objectType == QLatin1String("POINT")
                ? parseBlenderPointItem(
                      item, collectionIdsBySource,
                      source.value(QStringLiteral("blender_version")).toString(),
                      mode, &sceneObject, &itemError)
            : objectType == QLatin1String("POINTCLOUD")
                ? parseBlenderPointCloudItem(
                      item, collectionIdsBySource, &sceneObject, &itemError)
            : objectType == QLatin1String("SURFACE")
                ? parseBlenderSurfaceItem(item, collectionIdsBySource,
                                          materialsById,
                                          &sceneObject, &itemError)
                : parseBlenderPolyCurveItem(item, collectionIdsBySource,
                                            &sceneObject, &itemError);
        if (!parsed) {
            ++skippedCount;
            warnings.append(QStringLiteral("%1: object skipped: %2")
                                .arg(blenderItemLabel(item), itemError));
            continue;
        }
        sceneObject.blenderSourceVersion =
            source.value(QStringLiteral("blender_version")).toString();
        sceneObject.blenderMeshImportMode = mode;
        sceneObject.defaultViewLayerKey =
            item.value(QStringLiteral("default_view_layer_key")).toString();
        if (!sceneObject.defaultViewLayerKey.isEmpty() &&
            sceneObject.viewLayerHidden.contains(sceneObject.defaultViewLayerKey)) {
            sceneObject.hiddenInViewport = false;
        }
        unsupportedFeatureCount += warnings.size() - warningCountBeforeItem;
        importedIds.insert(objectId);
        importedIndexBySourceId.insert(objectId, imported.size());
        const QString sourceObjectId = item.value(
            QStringLiteral("source_object_id")).toString();
        sceneObject.blenderSourceObjectId =
            sourceObjectId.isEmpty() ? objectId : sourceObjectId;
        sourceObjectIdsByImportedIndex.append(
            sceneObject.blenderSourceObjectId);
        imported.append(std::move(sceneObject));
    }

    QVector<int> selectedObjectIndices;
    int activeObjectIndex = -1;
    QString activeSourceObjectId;
    QSet<QString> selectedSourceObjectIds;
    bool selectionLayerFound = false;
    for (const QJsonValue &sceneValue : importedSceneRecords) {
        const QJsonObject scene = sceneValue.toObject();
        if (scene.value(QStringLiteral("scene_id")).toString() != activeSceneId) {
            continue;
        }
        const QJsonArray viewLayers = scene.value(
            QStringLiteral("view_layers")).toArray();
        QJsonObject activeViewLayer;
        for (const QJsonValue &viewLayerValue : viewLayers) {
            const QJsonObject candidate = viewLayerValue.toObject();
            if (candidate.value(QStringLiteral("key")).toString() ==
                activeViewLayerKey) {
                activeViewLayer = candidate;
                break;
            }
        }
        if (activeViewLayer.isEmpty() && !viewLayers.isEmpty()) {
            activeViewLayer = viewLayers.first().toObject();
        }
        if (activeViewLayer.isEmpty()) continue;
        for (const QJsonValue &objectIdValue : activeViewLayer.value(
                 QStringLiteral("selected_object_ids")).toArray()) {
            if (objectIdValue.isString() && !objectIdValue.toString().isEmpty()) {
                selectedSourceObjectIds.insert(objectIdValue.toString());
            }
        }
        activeSourceObjectId = activeViewLayer.value(
            QStringLiteral("active_object_id")).toString();
        selectionLayerFound = true;
        break;
    }
    if (!selectionLayerFound) {
        for (const QJsonValue &sceneValue : importedSceneRecords) {
            const QJsonArray viewLayers = sceneValue.toObject().value(
                QStringLiteral("view_layers")).toArray();
            if (viewLayers.isEmpty()) continue;
            const QJsonObject activeViewLayer = viewLayers.first().toObject();
            for (const QJsonValue &objectIdValue : activeViewLayer.value(
                     QStringLiteral("selected_object_ids")).toArray()) {
                if (objectIdValue.isString() && !objectIdValue.toString().isEmpty()) {
                    selectedSourceObjectIds.insert(objectIdValue.toString());
                }
            }
            activeSourceObjectId = activeViewLayer.value(
                QStringLiteral("active_object_id")).toString();
            break;
        }
    }
    for (int index = 0; index < sourceObjectIdsByImportedIndex.size(); ++index) {
        const QString &sourceObjectId = sourceObjectIdsByImportedIndex[index];
        if (selectedSourceObjectIds.contains(sourceObjectId)) {
            selectedObjectIndices.append(index);
        }
        if (activeObjectIndex < 0 && sourceObjectId == activeSourceObjectId) {
            activeObjectIndex = index;
        }
    }
    if (activeObjectIndex >= 0 &&
        !selectedObjectIndices.contains(activeObjectIndex)) {
        activeObjectIndex = -1;
    }

    if (report != nullptr) {
        report->importedObjectCount = imported.size();
        report->skippedObjectCount = skippedCount;
        report->unsupportedFeatureCount = unsupportedFeatureCount;
        report->blenderVersion = source.value(QStringLiteral("blender_version")).toString();
        report->importMode = mode;
        report->warningMessage = conciseImportWarnings(
            warnings, skippedCount, unsupportedFeatureCount);
        report->scenes = importedSceneRecords;
        report->activeSceneId = activeSceneId;
        report->activeViewLayerKey = activeViewLayerKey;
        report->selectedObjectIndices = selectedObjectIndices;
        report->activeObjectIndex = activeObjectIndex;
        report->importedBlenderColorLuts = importedBlenderColorLuts;
        report->importedBlenderColorLutMetadata = importedBlenderColorLutMetadata;
    }
    QSet<QString> usedMaterialIds;
    for (const SceneObject &object : imported) {
        for (const QString &materialId : object.geometry.polygonMesh.materialSlotIds) {
            if (!materialId.isEmpty()) {
                usedMaterialIds.insert(materialId);
            }
        }
    }
    QVector<DocumentMaterial> usedMaterials;
    usedMaterials.reserve(importedMaterials.size());
    for (const DocumentMaterial &material : importedMaterials) {
        if (usedMaterialIds.contains(material.id)) {
            usedMaterials.append(material);
        }
    }
    *objects = std::move(imported);
    *collections = std::move(importedCollections);
    *materials = std::move(usedMaterials);
    return true;
}

} // namespace classiCAD
