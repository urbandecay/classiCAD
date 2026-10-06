#include "session_serializer.h"

#include "core/geometry/work_plane.h"
#include "core/serialization/document_serializer.h"
#include "core/serialization/shape_json_codec.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>
#include <limits>

namespace classiCAD {

namespace {

QJsonObject point3DToJson(const Point3D &point)
{
    return {{QStringLiteral("x"), point.x},
            {QStringLiteral("y"), point.y},
            {QStringLiteral("z"), point.z}};
}

bool point3DFromJson(const QJsonValue &value, Point3D *point)
{
    if (point == nullptr || !value.isObject()) {
        return false;
    }

    const QJsonObject object = value.toObject();
    const QJsonValue xValue = object.value(QStringLiteral("x"));
    const QJsonValue yValue = object.value(QStringLiteral("y"));
    const QJsonValue zValue = object.value(QStringLiteral("z"));
    if (!xValue.isDouble() || !yValue.isDouble() || !zValue.isDouble()) {
        return false;
    }

    const Point3D restored{xValue.toDouble(), yValue.toDouble(), zValue.toDouble()};
    if (!std::isfinite(restored.x) || !std::isfinite(restored.y) ||
        !std::isfinite(restored.z)) {
        return false;
    }
    *point = restored;
    return true;
}

QJsonObject workPlaneFrameToJson(const WorkPlaneFrame &frame)
{
    return {{QStringLiteral("origin"), point3DToJson(frame.origin)},
            {QStringLiteral("xAxis"), point3DToJson(frame.xAxis)},
            {QStringLiteral("yAxis"), point3DToJson(frame.yAxis)},
            {QStringLiteral("normal"), point3DToJson(frame.normal)},
            {QStringLiteral("valid"), frame.valid}};
}

bool workPlaneFrameFromJson(const QJsonValue &value, WorkPlaneFrame *frame)
{
    if (frame == nullptr || !value.isObject()) {
        return false;
    }

    const QJsonObject object = value.toObject();
    WorkPlaneFrame restored;
    if (!point3DFromJson(object.value(QStringLiteral("origin")), &restored.origin) ||
        !point3DFromJson(object.value(QStringLiteral("xAxis")), &restored.xAxis) ||
        !point3DFromJson(object.value(QStringLiteral("yAxis")), &restored.yAxis) ||
        !point3DFromJson(object.value(QStringLiteral("normal")), &restored.normal) ||
        !object.value(QStringLiteral("valid")).toBool(false)) {
        return false;
    }
    restored.valid = true;
    if (!isValidWorkPlaneFrame(restored)) {
        return false;
    }
    *frame = restored;
    return true;
}

QJsonObject cameraStateToJson(const ViewportCameraState &state)
{
    QJsonObject orientation;
    orientation.insert(QStringLiteral("w"), state.orientation.w);
    orientation.insert(QStringLiteral("x"), state.orientation.x);
    orientation.insert(QStringLiteral("y"), state.orientation.y);
    orientation.insert(QStringLiteral("z"), state.orientation.z);

    QJsonObject object;
    object.insert(QStringLiteral("zoom"), state.zoom);
    object.insert(QStringLiteral("pan"), pointToJson(state.pan));
    object.insert(QStringLiteral("orbitPivot"), point3DToJson(state.orbitPivot));
    object.insert(QStringLiteral("yawRadians"), state.yawRadians);
    object.insert(QStringLiteral("pitchRadians"), state.pitchRadians);
    object.insert(QStringLiteral("perspective"), state.perspective);
    object.insert(QStringLiteral("preset"), static_cast<int>(state.preset));
    object.insert(QStringLiteral("gridViewDistance"), state.gridViewDistance);
    object.insert(QStringLiteral("orientation"), orientation);
    object.insert(QStringLiteral("hasOrientation"), state.hasOrientation);
    return object;
}

bool cameraStateFromJson(const QJsonValue &value, ViewportCameraState *state)
{
    if (state == nullptr || !value.isObject()) {
        return false;
    }

    const QJsonObject object = value.toObject();
    const auto finiteNumber = [&object](const QString &key, qreal *number) {
        const QJsonValue value = object.value(key);
        if (number == nullptr || !value.isDouble()) {
            return false;
        }
        const qreal restored = value.toDouble();
        if (!std::isfinite(restored)) {
            return false;
        }
        *number = restored;
        return true;
    };

    ViewportCameraState restored;
    if (!finiteNumber(QStringLiteral("zoom"), &restored.zoom) ||
        restored.zoom <= 0.0 ||
        !pointFromJson(object.value(QStringLiteral("pan")), &restored.pan) ||
        !point3DFromJson(object.value(QStringLiteral("orbitPivot")),
                         &restored.orbitPivot) ||
        !finiteNumber(QStringLiteral("yawRadians"), &restored.yawRadians) ||
        !finiteNumber(QStringLiteral("pitchRadians"), &restored.pitchRadians) ||
        !finiteNumber(QStringLiteral("gridViewDistance"),
                      &restored.gridViewDistance) ||
        restored.gridViewDistance <= 0.0 ||
        !object.value(QStringLiteral("preset")).isDouble() ||
        !object.value(QStringLiteral("perspective")).isBool() ||
        !object.value(QStringLiteral("hasOrientation")).isBool()) {
        return false;
    }

    const int preset = object.value(QStringLiteral("preset")).toInt(-1);
    if (preset < static_cast<int>(ViewportViewPreset::Top) ||
        preset > static_cast<int>(ViewportViewPreset::Left)) {
        return false;
    }
    restored.preset = static_cast<ViewportViewPreset>(preset);
    restored.perspective = object.value(QStringLiteral("perspective")).toBool();
    restored.hasOrientation = object.value(QStringLiteral("hasOrientation")).toBool();

    const QJsonObject orientation = object.value(QStringLiteral("orientation")).toObject();
    const QJsonValue orientationW = orientation.value(QStringLiteral("w"));
    const QJsonValue orientationX = orientation.value(QStringLiteral("x"));
    const QJsonValue orientationY = orientation.value(QStringLiteral("y"));
    const QJsonValue orientationZ = orientation.value(QStringLiteral("z"));
    if (!orientationW.isDouble() || !orientationX.isDouble() ||
        !orientationY.isDouble() || !orientationZ.isDouble()) {
        return false;
    }
    restored.orientation = {orientationW.toDouble(),
                            orientationX.toDouble(),
                            orientationY.toDouble(),
                            orientationZ.toDouble()};
    const qreal orientationLengthSquared = restored.orientation.dot(restored.orientation);
    if (!std::isfinite(orientationLengthSquared) ||
        (restored.hasOrientation && orientationLengthSquared <= 1.0e-12)) {
        return false;
    }

    *state = restored;
    return true;
}

QJsonObject viewportShadingToJson(const ViewportShadingSettings &settings)
{
    QJsonArray shadowDirection;
    shadowDirection.append(settings.shadowDirection.x());
    shadowDirection.append(settings.shadowDirection.y());
    shadowDirection.append(settings.shadowDirection.z());

    QJsonObject object;
    object.insert(QStringLiteral("mode"), static_cast<int>(settings.mode));
    object.insert(QStringLiteral("lightingMode"),
                  static_cast<int>(settings.lightingMode));
    object.insert(QStringLiteral("studioLightPreset"), settings.studioLightPreset);
    object.insert(QStringLiteral("matcapPreset"), settings.matcapPreset);
    object.insert(QStringLiteral("studioLightRotationDegrees"),
                  settings.studioLightRotationDegrees);
    object.insert(QStringLiteral("worldSpaceLighting"), settings.worldSpaceLighting);
    object.insert(QStringLiteral("xray"), settings.xray);
    object.insert(QStringLiteral("xrayWireframe"), settings.xrayWireframe);
    object.insert(QStringLiteral("wireColorMode"),
                  static_cast<int>(settings.wireColorMode));
    object.insert(QStringLiteral("colorMode"),
                  static_cast<int>(settings.colorMode));
    object.insert(QStringLiteral("backgroundMode"),
                  static_cast<int>(settings.backgroundMode));
    object.insert(QStringLiteral("cavityType"),
                  static_cast<int>(settings.cavityType));
    object.insert(QStringLiteral("customColor"),
                  settings.customColor.name(QColor::HexArgb));
    object.insert(QStringLiteral("outlineColor"),
                  settings.outlineColor.name(QColor::HexArgb));
    object.insert(QStringLiteral("customBackgroundColor"),
                  settings.customBackgroundColor.name(QColor::HexArgb));
    object.insert(QStringLiteral("shadowDirection"), shadowDirection);
    object.insert(QStringLiteral("backfaceCulling"), settings.backfaceCulling);
    object.insert(QStringLiteral("outline"), settings.outline);
    object.insert(QStringLiteral("specularLighting"), settings.specularLighting);
    object.insert(QStringLiteral("shadows"), settings.shadows);
    object.insert(QStringLiteral("depthOfField"), settings.depthOfField);
    object.insert(QStringLiteral("cavity"), settings.cavity);
    object.insert(QStringLiteral("xrayAlpha"), settings.xrayAlpha);
    object.insert(QStringLiteral("shadowIntensity"), settings.shadowIntensity);
    object.insert(QStringLiteral("shadowOffset"), settings.shadowOffset);
    object.insert(QStringLiteral("shadowFocus"), settings.shadowFocus);
    return object;
}

bool viewportShadingFromJson(const QJsonValue &value,
                             ViewportShadingSettings *settings)
{
    if (settings == nullptr || !value.isObject()) {
        return false;
    }

    const QJsonObject object = value.toObject();
    const auto integer = [&object](const QString &key, int minimum,
                                   int maximum, int *result) {
        const QJsonValue value = object.value(key);
        if (result == nullptr || !value.isDouble()) {
            return false;
        }
        const double number = value.toDouble();
        if (!std::isfinite(number) || std::floor(number) != number ||
            number < minimum || number > maximum) {
            return false;
        }
        *result = static_cast<int>(number);
        return true;
    };
    const auto boolean = [&object](const QString &key, bool *result) {
        const QJsonValue value = object.value(key);
        if (result == nullptr || !value.isBool()) {
            return false;
        }
        *result = value.toBool();
        return true;
    };
    const auto number = [&object](const QString &key, qreal *result) {
        const QJsonValue value = object.value(key);
        if (result == nullptr || !value.isDouble()) {
            return false;
        }
        const qreal parsed = value.toDouble();
        if (!std::isfinite(parsed)) {
            return false;
        }
        *result = parsed;
        return true;
    };
    const auto color = [&object](const QString &key, QColor *result) {
        const QJsonValue value = object.value(key);
        if (result == nullptr || !value.isString()) {
            return false;
        }
        const QColor parsed(value.toString());
        if (!parsed.isValid()) {
            return false;
        }
        *result = parsed;
        return true;
    };

    ViewportShadingSettings restored;
    int parsed = 0;
    if (!integer(QStringLiteral("mode"), 0, 1, &parsed)) {
        return false;
    }
    restored.mode = static_cast<ViewportShadingMode>(parsed);
    if (!integer(QStringLiteral("lightingMode"), 0, 2, &parsed)) {
        return false;
    }
    restored.lightingMode = static_cast<ViewportLightingMode>(parsed);
    const QJsonValue studioPresetValue =
        object.value(QStringLiteral("studioLightPreset"));
    const QJsonValue matcapPresetValue =
        object.value(QStringLiteral("matcapPreset"));
    if (!studioPresetValue.isString() || !matcapPresetValue.isString() ||
        studioPresetValue.toString().isEmpty() ||
        matcapPresetValue.toString().isEmpty()) {
        return false;
    }
    restored.studioLightPreset = studioPresetValue.toString();
    restored.matcapPreset = matcapPresetValue.toString();
    if (!integer(QStringLiteral("studioLightRotationDegrees"), -3600, 3600,
                 &restored.studioLightRotationDegrees) ||
        !boolean(QStringLiteral("worldSpaceLighting"),
                 &restored.worldSpaceLighting) ||
        !boolean(QStringLiteral("xray"), &restored.xray) ||
        !boolean(QStringLiteral("xrayWireframe"), &restored.xrayWireframe) ||
        !integer(QStringLiteral("wireColorMode"), 0, 2, &parsed)) {
        return false;
    }
    restored.wireColorMode = static_cast<ViewportWireColorMode>(parsed);
    if (!integer(QStringLiteral("colorMode"), 0, 5, &parsed)) {
        return false;
    }
    restored.colorMode = static_cast<ViewportColorMode>(parsed);
    if (!integer(QStringLiteral("backgroundMode"), 0, 2, &parsed)) {
        return false;
    }
    restored.backgroundMode = static_cast<ViewportBackgroundMode>(parsed);
    if (!integer(QStringLiteral("cavityType"), 0, 2, &parsed)) {
        return false;
    }
    restored.cavityType = static_cast<ViewportCavityType>(parsed);
    if (!color(QStringLiteral("customColor"), &restored.customColor) ||
        !color(QStringLiteral("outlineColor"), &restored.outlineColor) ||
        !color(QStringLiteral("customBackgroundColor"),
               &restored.customBackgroundColor)) {
        return false;
    }

    const QJsonValue shadowDirectionValue =
        object.value(QStringLiteral("shadowDirection"));
    if (!shadowDirectionValue.isArray()) {
        return false;
    }
    const QJsonArray shadowDirection = shadowDirectionValue.toArray();
    if (shadowDirection.size() != 3 || !shadowDirection[0].isDouble() ||
        !shadowDirection[1].isDouble() || !shadowDirection[2].isDouble()) {
        return false;
    }
    const qreal shadowX = shadowDirection[0].toDouble();
    const qreal shadowY = shadowDirection[1].toDouble();
    const qreal shadowZ = shadowDirection[2].toDouble();
    if (!std::isfinite(shadowX) || !std::isfinite(shadowY) ||
        !std::isfinite(shadowZ)) {
        return false;
    }
    restored.shadowDirection = QVector3D(shadowX, shadowY, shadowZ);

    if (!boolean(QStringLiteral("backfaceCulling"),
                 &restored.backfaceCulling) ||
        !boolean(QStringLiteral("outline"), &restored.outline) ||
        !boolean(QStringLiteral("specularLighting"),
                 &restored.specularLighting) ||
        !boolean(QStringLiteral("shadows"), &restored.shadows) ||
        !boolean(QStringLiteral("depthOfField"), &restored.depthOfField) ||
        !boolean(QStringLiteral("cavity"), &restored.cavity) ||
        !number(QStringLiteral("xrayAlpha"), &restored.xrayAlpha) ||
        !number(QStringLiteral("shadowIntensity"),
                &restored.shadowIntensity) ||
        !number(QStringLiteral("shadowOffset"), &restored.shadowOffset) ||
        !number(QStringLiteral("shadowFocus"), &restored.shadowFocus) ||
        restored.xrayAlpha < 0.0 || restored.xrayAlpha > 1.0 ||
        restored.shadowIntensity < 0.0 || restored.shadowIntensity > 1.0 ||
        restored.shadowOffset < 0.0) {
        return false;
    }

    *settings = std::move(restored);
    return true;
}

bool fail(QString *errorMessage, const QString &message)
{
    if (errorMessage != nullptr) {
        *errorMessage = message;
    }
    return false;
}

} // namespace

bool SessionSerializer::write(const QString &path,
                              const Document &document,
                              const UpdateSessionViewState &view,
                              QString *errorMessage)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return fail(errorMessage, file.errorString());
    }

    QJsonObject root;
    root.insert(QStringLiteral("version"), 6);
    root.insert(QStringLiteral("zoom"), view.zoom);
    root.insert(QStringLiteral("pan"), pointToJson(view.pan));
    root.insert(QStringLiteral("document"), documentToJson(document));
    root.insert(QStringLiteral("viewportCamera"), cameraStateToJson(view.camera));
    root.insert(QStringLiteral("workPlane"), static_cast<int>(view.workPlane));
    root.insert(QStringLiteral("workPlaneOffset"), view.workPlaneOffset);
    root.insert(QStringLiteral("workPlaneFrame"), workPlaneFrameToJson(view.workPlaneFrame));

    QJsonObject cameraPreferences;
    cameraPreferences.insert(QStringLiteral("focalLengthMillimeters"),
                             view.cameraPreferences.focalLengthMillimeters);
    cameraPreferences.insert(QStringLiteral("clipStart"),
                             view.cameraPreferences.clipStart);
    cameraPreferences.insert(QStringLiteral("clipEnd"),
                             view.cameraPreferences.clipEnd);
    root.insert(QStringLiteral("cameraPreferences"), cameraPreferences);

    QJsonArray selectedObjectIds;
    for (const ObjectId objectId : view.selectedObjectIds) {
        selectedObjectIds.append(QString::number(objectId.value()));
    }
    root.insert(QStringLiteral("selectedObjectIds"), selectedObjectIds);
    root.insert(QStringLiteral("primaryObjectId"),
                QString::number(view.primaryObjectId.value()));
    if (view.activeControlPoint.isValid()) {
        QJsonObject controlPoint;
        controlPoint.insert(QStringLiteral("objectId"),
                            QString::number(view.activeControlPoint.objectId.value()));
        controlPoint.insert(QStringLiteral("index"), view.activeControlPoint.index);
        root.insert(QStringLiteral("activeControlPoint"), controlPoint);
    }
    root.insert(QStringLiteral("controlPointsVisible"), view.controlPointsVisible);
    root.insert(QStringLiteral("activeTool"), static_cast<int>(view.activeTool));
    root.insert(QStringLiteral("viewportShading"),
                viewportShadingToJson(view.shading));

    const QByteArray data = QJsonDocument(root).toJson(QJsonDocument::Compact);
    if (file.write(data) != data.size()) {
        return fail(errorMessage, file.errorString());
    }
    return true;
}

bool SessionSerializer::read(const QString &path,
                             const ViewportCameraPreferences &defaultCameraPreferences,
                             bool defaultControlPointsVisible,
                             RestoredUpdateSession *session,
                             QString *errorMessage)
{
    if (session == nullptr) {
        return fail(errorMessage, QStringLiteral("No session output was provided."));
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return fail(errorMessage, file.errorString());
    }

    QJsonParseError parseError;
    const QJsonDocument json = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !json.isObject()) {
        return fail(errorMessage, parseError.errorString());
    }

    const QJsonObject root = json.object();
    const int version = root.value(QStringLiteral("version")).toInt(-1);
    if (version < 1 || version > 6) {
        return fail(errorMessage,
                    QStringLiteral("Unsupported update session version %1.").arg(version));
    }

    RestoredUpdateSession restored;
    restored.version = version;
    restored.view.cameraPreferences = defaultCameraPreferences;
    restored.view.controlPointsVisible = defaultControlPointsVisible;
    if (!pointFromJson(root.value(QStringLiteral("pan")), &restored.view.pan)) {
        return fail(errorMessage, QStringLiteral("Invalid pan value."));
    }
    const QJsonValue zoomValue = root.value(QStringLiteral("zoom"));
    restored.view.zoom = zoomValue.toDouble(1.0);
    if (!zoomValue.isDouble() || !std::isfinite(restored.view.zoom) ||
        restored.view.zoom <= 1.0e-9) {
        return fail(errorMessage, QStringLiteral("Invalid zoom value."));
    }

    if (version >= 4) {
        if (!cameraStateFromJson(root.value(QStringLiteral("viewportCamera")),
                                 &restored.view.camera)) {
            return fail(errorMessage, QStringLiteral("Invalid viewport camera state."));
        }

        const QJsonValue workPlaneValue = root.value(QStringLiteral("workPlane"));
        const QJsonValue workPlaneOffsetValue = root.value(QStringLiteral("workPlaneOffset"));
        WorkPlane parsedWorkPlane;
        restored.view.workPlaneOffset = workPlaneOffsetValue.toDouble(
            std::numeric_limits<qreal>::quiet_NaN());
        if (!workPlaneValue.isDouble() ||
            !workPlaneFromValue(workPlaneValue.toInt(-1), &parsedWorkPlane) ||
            !std::isfinite(restored.view.workPlaneOffset) ||
            !workPlaneFrameFromJson(root.value(QStringLiteral("workPlaneFrame")),
                                    &restored.view.workPlaneFrame)) {
            return fail(errorMessage, QStringLiteral("Invalid workplane state."));
        }
        restored.view.workPlane = parsedWorkPlane;

        const QJsonObject preferences = root.value(QStringLiteral("cameraPreferences")).toObject();
        const QJsonValue focalLengthValue =
            preferences.value(QStringLiteral("focalLengthMillimeters"));
        const QJsonValue clipStartValue = preferences.value(QStringLiteral("clipStart"));
        const QJsonValue clipEndValue = preferences.value(QStringLiteral("clipEnd"));
        restored.view.cameraPreferences = {
            focalLengthValue.toDouble(std::numeric_limits<qreal>::quiet_NaN()),
            clipStartValue.toDouble(std::numeric_limits<qreal>::quiet_NaN()),
            clipEndValue.toDouble(std::numeric_limits<qreal>::quiet_NaN())};
        if (!focalLengthValue.isDouble() || !clipStartValue.isDouble() ||
            !clipEndValue.isDouble() ||
            !std::isfinite(restored.view.cameraPreferences.focalLengthMillimeters) ||
            restored.view.cameraPreferences.focalLengthMillimeters < 1.0 ||
            restored.view.cameraPreferences.focalLengthMillimeters > 2000.0 ||
            !std::isfinite(restored.view.cameraPreferences.clipStart) ||
            restored.view.cameraPreferences.clipStart < 0.000001 ||
            !std::isfinite(restored.view.cameraPreferences.clipEnd) ||
            restored.view.cameraPreferences.clipEnd <= restored.view.cameraPreferences.clipStart ||
            restored.view.cameraPreferences.clipEnd > 1.0e9) {
            return fail(errorMessage, QStringLiteral("Invalid camera preferences."));
        }

        const QJsonValue selectedIdsValue = root.value(QStringLiteral("selectedObjectIds"));
        if (!selectedIdsValue.isArray()) {
            return fail(errorMessage, QStringLiteral("Invalid object selection."));
        }
        for (const QJsonValue &idValue : selectedIdsValue.toArray()) {
            bool idOk = false;
            const quint64 id = idValue.toString().toULongLong(&idOk);
            if (!idValue.isString() || !idOk || id == 0) {
                return fail(errorMessage, QStringLiteral("Invalid selected object ID."));
            }
            restored.view.selectedObjectIds.append(ObjectId::fromValue(id));
        }

        bool primaryIdOk = false;
        const quint64 primaryId = root.value(QStringLiteral("primaryObjectId"))
                                      .toString().toULongLong(&primaryIdOk);
        if (!primaryIdOk) {
            return fail(errorMessage, QStringLiteral("Invalid primary object ID."));
        }
        restored.view.primaryObjectId = ObjectId::fromValue(primaryId);

        const QJsonValue activeControlPointValue = root.value(QStringLiteral("activeControlPoint"));
        if (!activeControlPointValue.isUndefined()) {
            const QJsonObject activeControlPoint = activeControlPointValue.toObject();
            bool objectIdOk = false;
            const quint64 objectId = activeControlPoint.value(QStringLiteral("objectId"))
                                         .toString().toULongLong(&objectIdOk);
            const QJsonValue indexValue = activeControlPoint.value(QStringLiteral("index"));
            const int index = indexValue.toInt(-1);
            if (!activeControlPointValue.isObject() || !objectIdOk || objectId == 0 ||
                !indexValue.isDouble() || index < 0) {
                return fail(errorMessage, QStringLiteral("Invalid active control point."));
            }
            restored.view.activeControlPoint = {ObjectId::fromValue(objectId), index};
        }

        const QJsonValue controlPointsValue = root.value(QStringLiteral("controlPointsVisible"));
        if (!controlPointsValue.isBool()) {
            return fail(errorMessage, QStringLiteral("Invalid control-point visibility."));
        }
        restored.view.controlPointsVisible = controlPointsValue.toBool();

        if (version >= 5) {
            const QJsonValue activeToolValue =
                root.value(QStringLiteral("activeTool"));
            const int activeToolId = activeToolValue.toInt(-1);
            const ToolId activeTool = static_cast<ToolId>(activeToolId);
            if (!activeToolValue.isDouble() ||
                activeToolValue.toDouble() != activeToolId ||
                toolName(activeTool) == QStringLiteral("Unknown")) {
                return fail(errorMessage, QStringLiteral("Invalid active tool."));
            }
            restored.view.activeTool = activeTool;
        }

        if (version >= 6 &&
            !viewportShadingFromJson(root.value(QStringLiteral("viewportShading")),
                                     &restored.view.shading)) {
            return fail(errorMessage, QStringLiteral("Invalid viewport shading state."));
        }
    }

    if (version >= 3) {
        QString documentError;
        if (!documentFromJson(root.value(QStringLiteral("document")),
                              &restored.document,
                              &documentError)) {
            return fail(errorMessage, documentError);
        }
    } else {
        const QJsonValue shapesValue = root.value(QStringLiteral("shapes"));
        if (!shapesValue.isArray()) {
            return fail(errorMessage, QStringLiteral("Invalid legacy shapes array."));
        }
        QVector<Shape> shapes;
        const QJsonArray shapeArray = shapesValue.toArray();
        shapes.reserve(shapeArray.size());
        for (const QJsonValue &shapeValue : shapeArray) {
            Shape shape{GeometryType::Invalid,
                        {},
                        Shape::NurbsCurve2D{},
                        ArcMode::TwoPoint,
                        0.0,
                        {},
                        {}};
            if (!shapeFromJson(shapeValue, &shape)) {
                return fail(errorMessage, QStringLiteral("Invalid legacy shape record."));
            }
            shapes.append(std::move(shape));
        }
        restored.document = shapes;
    }

    *session = std::move(restored);
    return true;
}

} // namespace classiCAD
