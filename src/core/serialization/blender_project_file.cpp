#include "blender_project_file.h"

#include "core/document/document.h"
#include "core/document/collection_id.h"
#include "core/document/document_collection.h"
#include "core/geometry/geometry_type.h"
#include "core/geometry/nurbs_surface_tessellator.h"
#include "core/geometry/polygon_mesh.h"
#include "core/serialization/document_serializer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
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

#include <algorithm>
#include <cmath>
#include <limits>
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
        int surfaceIndex = 0;
        for (const auto &surface : classiCAD::shapeSurfaceFaces(sceneObject.geometry)) {
            classiCAD::PreparedNurbsSurfaceTessellation tessellation;
            if (!tessellation.prepare(surface, displayOptions)) {
                setError(errorMessage,
                         QStringLiteral("Could not tessellate NURBS surface object %1 for display")
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
                face.append(vertexOffset + triangle[0]);
                face.append(vertexOffset + triangle[reversed ? 2 : 1]);
                face.append(vertexOffset + triangle[reversed ? 1 : 2]);
                faces.append(face);
            }
            ++surfaceIndex;
        }
        QJsonObject mesh;
        mesh.insert(QStringLiteral("vertices"), vertices);
        mesh.insert(QStringLiteral("faces"), faces);
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
        !std::isfinite(settings.sensorWidthMillimeters) ||
        settings.sensorWidthMillimeters < 1.0 ||
        settings.sensorWidthMillimeters > 100.0) {
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
    serialized.insert(QStringLiteral("sensorWidthMillimeters"),
                      settings.sensorWidthMillimeters);
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
    settings->sensorWidthMillimeters = camera.value(QStringLiteral("sensorWidthMillimeters")).toDouble(36.0);
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
        process.waitForFinished();
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
                reference.imageName = image.value(QStringLiteral("image_name")).toString();
                reference.imageLibraryPath = image.value(QStringLiteral("image_library_path")).toString();
                reference.filepath = image.value(QStringLiteral("filepath")).toString();
                reference.colorSpace = image.value(QStringLiteral("color_space")).toString();
                reference.alphaMode = image.value(QStringLiteral("alpha_mode"))
                                          .toString(QStringLiteral("STRAIGHT"));
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
                reference.sourceFileExists = image.value(QStringLiteral("source_file_exists")).toBool();
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
    result.blenderSourceVersion = blenderVersion;
    result.blenderMeshImportMode = importMode;
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
            if (!fallbackMaterialSet) {
                result.workbenchMaterial.baseColorSceneLinear = QVector3D(
                    material.baseColorSceneLinear[0],
                    material.baseColorSceneLinear[1],
                    material.baseColorSceneLinear[2]);
                result.workbenchMaterial.roughness = material.roughness;
                result.workbenchMaterial.metallic = material.metallic;
                result.workbenchMaterial.alpha = material.alpha;
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
        bridgeVersion != 2) {
        setError(errorMessage,
                 QStringLiteral("The Blender scene extraction format is unsupported"));
        return false;
    }
    const QJsonObject source = root.value(QStringLiteral("source")).toObject();
    const QJsonArray version =
        source.value(QStringLiteral("blender_version_tuple")).toArray();
    int major = -1;
    int minor = -1;
    int patch = -1;
    if (version.size() != 3 || !jsonInteger(version[0], &major) ||
        !jsonInteger(version[1], &minor) || !jsonInteger(version[2], &patch) ||
        major != 5 || minor != 2 || patch != 2 ||
        source.value(QStringLiteral("blender_version")).toString() !=
            QLatin1String(kPinnedBlenderVersion) ||
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
    QVector<SceneObject> imported;
    QVector<DocumentCollection> importedCollections;
    QVector<DocumentMaterial> importedMaterials;
    QHash<QString, DocumentMaterial> materialsById;
    QSet<QString> importedIds;
    QHash<QString, CollectionId> collectionIdsBySource;
    QHash<QString, QString> sceneLibraryPaths;
    const QJsonArray sourceScenes = root.value(QStringLiteral("scenes")).toArray();
    for (const QJsonValue &sceneValue : sourceScenes) {
        const QJsonObject scene = sceneValue.toObject();
        sceneLibraryPaths.insert(scene.value(QStringLiteral("name")).toString(),
                                 scene.value(QStringLiteral("library_path")).toString());
        const QJsonArray viewLayers = scene.value(QStringLiteral("view_layer_names")).toArray();
        if (!viewLayers.isEmpty()) {
            warnings.append(QStringLiteral("Scene %1: Blender view-layer membership and per-view-layer collection exclusions/visibility are not retained")
                                .arg(scene.value(QStringLiteral("name")).toString()));
        }
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
        if (objectId.isEmpty() ||
            item.value(QStringLiteral("object_type")).toString() !=
                QLatin1String("MESH")) {
            ++skippedCount;
            warnings.append(QStringLiteral("A Blender mesh record has missing identity or an unexpected object type and was skipped"));
            continue;
        }
        if (importedIds.contains(objectId)) {
            warnings.append(QStringLiteral("%1: linked into more than one Blender scene; imported once")
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
        if (!parseBlenderMeshItem(item, collectionIdsBySource, materialsById,
                                  source.value(QStringLiteral("blender_version")).toString(),
                                  mode,
                                  &sceneObject,
                                  &warnings, &itemError)) {
            ++skippedCount;
            warnings.append(QStringLiteral("%1: mesh skipped: %2")
                                .arg(blenderItemLabel(item), itemError));
            continue;
        }
        unsupportedFeatureCount += warnings.size() - warningCountBeforeItem;
        importedIds.insert(objectId);
        imported.append(std::move(sceneObject));
    }

    if (report != nullptr) {
        report->importedObjectCount = imported.size();
        report->skippedObjectCount = skippedCount;
        report->unsupportedFeatureCount = unsupportedFeatureCount;
        report->blenderVersion = source.value(QStringLiteral("blender_version")).toString();
        report->importMode = mode;
        report->warningMessage = conciseImportWarnings(
            warnings, skippedCount, unsupportedFeatureCount);
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
