#include "document_serializer.h"

#include "core/serialization/shape_json_codec.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace classiCAD {

namespace {

void setError(QString *errorMessage, const QString &message)
{
    if (errorMessage != nullptr) {
        *errorMessage = message;
    }
}

QJsonValue idToJson(quint64 value)
{
    return QString::number(value);
}

bool idFromJson(const QJsonValue &value, quint64 *id)
{
    if (id == nullptr) {
        return false;
    }

    if (value.isString()) {
        bool converted = false;
        const quint64 parsed = value.toString().toULongLong(&converted);
        if (converted && parsed != 0) {
            *id = parsed;
            return true;
        }
        return false;
    }

    if (value.isDouble()) {
        const double parsed = value.toDouble();
        if (parsed > 0.0 && parsed <= std::numeric_limits<quint64>::max() &&
            std::floor(parsed) == parsed) {
            *id = static_cast<quint64>(parsed);
            return true;
        }
    }
    return false;
}

} // namespace

QJsonObject documentToJson(const Document &document)
{
    QJsonObject serialized;
    serialized.insert(QStringLiteral("version"), 5);
    QJsonObject settings;
    settings.insert(QStringLiteral("lengthUnit"),
                    documentLengthUnitKey(document.settings().lengthUnit));
    settings.insert(QStringLiteral("gridSpacing"), document.settings().gridSpacing);
    serialized.insert(QStringLiteral("settings"), settings);
    serialized.insert(QStringLiteral("activeLayerId"),
                      idToJson(document.activeLayerId().value()));

    QJsonArray layers;
    for (const Layer &layer : document.layers()) {
        QJsonObject serializedLayer;
        serializedLayer.insert(QStringLiteral("id"), idToJson(layer.id.value()));
        serializedLayer.insert(QStringLiteral("name"), layer.name);
        serializedLayer.insert(QStringLiteral("color"), layer.color.name(QColor::HexArgb));
        serializedLayer.insert(QStringLiteral("lineType"), layer.lineType);
        serializedLayer.insert(QStringLiteral("lineWeightMm"), layer.lineWeightMm);
        serializedLayer.insert(QStringLiteral("description"), layer.description);
        serializedLayer.insert(QStringLiteral("visible"), layer.visible);
        serializedLayer.insert(QStringLiteral("frozen"), layer.frozen);
        serializedLayer.insert(QStringLiteral("locked"), layer.locked);
        serializedLayer.insert(QStringLiteral("plotted"), layer.plotted);

        QJsonArray objectIds;
        for (const ObjectId objectId : layer.objectIds) {
            objectIds.append(idToJson(objectId.value()));
        }
        serializedLayer.insert(QStringLiteral("objectIds"), objectIds);
        layers.append(serializedLayer);
    }
    serialized.insert(QStringLiteral("layers"), layers);

    QJsonArray objects;
    for (const SceneObject &sceneObject : document.objects()) {
        QJsonObject serializedObject;
        serializedObject.insert(QStringLiteral("id"), idToJson(sceneObject.id.value()));
        serializedObject.insert(QStringLiteral("layerId"), idToJson(sceneObject.layerId.value()));
        serializedObject.insert(QStringLiteral("geometry"), shapeToJson(sceneObject.geometry));
        QJsonObject placement;
        placement.insert(QStringLiteral("x"), sceneObject.placementTranslation.x);
        placement.insert(QStringLiteral("y"), sceneObject.placementTranslation.y);
        placement.insert(QStringLiteral("z"), sceneObject.placementTranslation.z);
        serializedObject.insert(QStringLiteral("placementTranslation"), placement);
        objects.append(serializedObject);
    }
    serialized.insert(QStringLiteral("objects"), objects);
    return serialized;
}

bool documentFromJson(const QJsonValue &value,
                      Document *document,
                      QString *errorMessage)
{
    if (document == nullptr || !value.isObject()) {
        setError(errorMessage, QStringLiteral("document must be an object"));
        return false;
    }

    const QJsonObject serialized = value.toObject();
    const int version = serialized.value(QStringLiteral("version")).toInt(-1);
    if (version < 1 || version > 5) {
        setError(errorMessage, QStringLiteral("unsupported document version"));
        return false;
    }

    const QJsonValue layersValue = serialized.value(QStringLiteral("layers"));
    const QJsonValue objectsValue = serialized.value(QStringLiteral("objects"));
    if (!layersValue.isArray() || !objectsValue.isArray()) {
        setError(errorMessage, QStringLiteral("document layers and objects must be arrays"));
        return false;
    }

    Document::Snapshot snapshot;
    if (version >= 4) {
        const QJsonValue settingsValue = serialized.value(QStringLiteral("settings"));
        if (!settingsValue.isObject()) {
            setError(errorMessage, QStringLiteral("document settings must be an object"));
            return false;
        }
        const QJsonObject settingsObject = settingsValue.toObject();
        DocumentSettings settings;
        const QJsonValue lengthUnitValue =
            settingsObject.value(QStringLiteral("lengthUnit"));
        const QJsonValue gridSpacingValue =
            settingsObject.value(QStringLiteral("gridSpacing"));
        if (!lengthUnitValue.isString() ||
            !documentLengthUnitFromKey(lengthUnitValue.toString(),
                                       &settings.lengthUnit) ||
            !gridSpacingValue.isDouble()) {
            setError(errorMessage, QStringLiteral("invalid document grid settings"));
            return false;
        }
        settings.gridSpacing = gridSpacingValue.toDouble();
        if (!isValidDocumentSettings(settings)) {
            setError(errorMessage, QStringLiteral("invalid document grid settings"));
            return false;
        }
        snapshot.settings = settings;
    }
    QSet<quint64> layerIds;
    quint64 nextLayerValue = 1;
    for (const QJsonValue &layerValue : layersValue.toArray()) {
        if (!layerValue.isObject()) {
            setError(errorMessage, QStringLiteral("layer record must be an object"));
            return false;
        }

        const QJsonObject serializedLayer = layerValue.toObject();
        quint64 layerValueId = 0;
        const QString name = serializedLayer.value(QStringLiteral("name")).toString().trimmed();
        if (!idFromJson(serializedLayer.value(QStringLiteral("id")), &layerValueId) ||
            layerIds.contains(layerValueId) || name.isEmpty() ||
            !serializedLayer.value(QStringLiteral("visible")).isBool() ||
            !serializedLayer.value(QStringLiteral("locked")).isBool()) {
            setError(errorMessage, QStringLiteral("invalid layer record"));
            return false;
        }

        Layer layer;
        layer.id = LayerId::fromValue(layerValueId);
        layer.name = name;
        const QJsonValue colorValue = serializedLayer.value(QStringLiteral("color"));
        if (colorValue.isUndefined()) {
            if (version >= 2) {
                setError(errorMessage, QStringLiteral("layer color is missing"));
                return false;
            }
            // Keep the version-1 file default stable when the current default
            // for newly created layers changes.
            layer.color = QColor(QStringLiteral("#d28b45"));
        } else {
            if (!colorValue.isString()) {
                setError(errorMessage, QStringLiteral("invalid layer color"));
                return false;
            }
            const QColor color(colorValue.toString());
            if (!color.isValid()) {
                setError(errorMessage, QStringLiteral("invalid layer color"));
                return false;
            }
            layer.color = color;
        }
        if (version >= 3) {
            const QJsonValue lineTypeValue =
                serializedLayer.value(QStringLiteral("lineType"));
            const QJsonValue lineWeightValue =
                serializedLayer.value(QStringLiteral("lineWeightMm"));
            const QJsonValue descriptionValue =
                serializedLayer.value(QStringLiteral("description"));
            const QJsonValue frozenValue = serializedLayer.value(QStringLiteral("frozen"));
            const QJsonValue plottedValue = serializedLayer.value(QStringLiteral("plotted"));
            const QString lineType = lineTypeValue.toString().trimmed();
            const qreal lineWeightMm = lineWeightValue.toDouble(-1.0);
            if (!lineTypeValue.isString() || lineType.isEmpty() ||
                !lineWeightValue.isDouble() || !std::isfinite(lineWeightMm) ||
                lineWeightMm < 0.0 || lineWeightMm > 2.11 ||
                !descriptionValue.isString() || !frozenValue.isBool() ||
                !plottedValue.isBool()) {
                setError(errorMessage, QStringLiteral("invalid layer properties"));
                return false;
            }
            layer.lineType = lineType;
            layer.lineWeightMm = lineWeightMm;
            layer.description = descriptionValue.toString();
            layer.frozen = frozenValue.toBool();
            layer.plotted = plottedValue.toBool();
        }
        layer.visible = serializedLayer.value(QStringLiteral("visible")).toBool();
        layer.locked = serializedLayer.value(QStringLiteral("locked")).toBool();
        snapshot.layers.append(layer);
        layerIds.insert(layerValueId);
        if (layerValueId < std::numeric_limits<quint64>::max()) {
            nextLayerValue = std::max(nextLayerValue, layerValueId + 1);
        }
    }

    if (snapshot.layers.isEmpty()) {
        setError(errorMessage, QStringLiteral("document must contain a layer"));
        return false;
    }

    quint64 activeLayerValue = 0;
    if (!idFromJson(serialized.value(QStringLiteral("activeLayerId")), &activeLayerValue) ||
        !layerIds.contains(activeLayerValue)) {
        setError(errorMessage, QStringLiteral("invalid active layer"));
        return false;
    }
    snapshot.activeLayerId = LayerId::fromValue(activeLayerValue);
    const auto activeLayer = std::find_if(
        snapshot.layers.cbegin(),
        snapshot.layers.cend(),
        [activeLayerValue](const Layer &layer) {
            return layer.id.value() == activeLayerValue;
        });
    if (activeLayer == snapshot.layers.cend() || !activeLayer->visible ||
        activeLayer->frozen || activeLayer->locked) {
        setError(errorMessage, QStringLiteral("active layer must be visible and unlocked"));
        return false;
    }

    QSet<quint64> objectIds;
    quint64 nextObjectValue = 1;
    for (const QJsonValue &objectValue : objectsValue.toArray()) {
        if (!objectValue.isObject()) {
            setError(errorMessage, QStringLiteral("scene object record must be an object"));
            return false;
        }

        const QJsonObject serializedObject = objectValue.toObject();
        quint64 objectValueId = 0;
        quint64 objectLayerValue = 0;
        Shape shape;
        if (!idFromJson(serializedObject.value(QStringLiteral("id")), &objectValueId) ||
            objectIds.contains(objectValueId) ||
            !idFromJson(serializedObject.value(QStringLiteral("layerId")), &objectLayerValue) ||
            !layerIds.contains(objectLayerValue) ||
            !shapeFromJson(serializedObject.value(QStringLiteral("geometry")), &shape)) {
            setError(errorMessage, QStringLiteral("invalid scene object record"));
            return false;
        }

        Point3D placementTranslation;
        if (version >= 5) {
            const QJsonValue placementValue =
                serializedObject.value(QStringLiteral("placementTranslation"));
            if (!placementValue.isObject()) {
                setError(errorMessage, QStringLiteral("invalid object placement"));
                return false;
            }
            const QJsonObject placementObject = placementValue.toObject();
            const QJsonValue xValue = placementObject.value(QStringLiteral("x"));
            const QJsonValue yValue = placementObject.value(QStringLiteral("y"));
            const QJsonValue zValue = placementObject.value(QStringLiteral("z"));
            if (!xValue.isDouble() || !yValue.isDouble() || !zValue.isDouble() ||
                !std::isfinite(xValue.toDouble()) ||
                !std::isfinite(yValue.toDouble()) ||
                !std::isfinite(zValue.toDouble())) {
                setError(errorMessage, QStringLiteral("invalid object placement"));
                return false;
            }
            placementTranslation = {xValue.toDouble(), yValue.toDouble(),
                                    zValue.toDouble()};
            const bool hasSpatialPlacement =
                placementTranslation.x != 0.0 || placementTranslation.y != 0.0 ||
                placementTranslation.z != 0.0;
            if (hasSpatialPlacement &&
                shape.geometryType != GeometryType::NurbsSurface &&
                shape.geometryType != GeometryType::NurbsSolid) {
                setError(errorMessage,
                         QStringLiteral("placement is only supported for spatial NURBS objects"));
                return false;
            }
        }

        SceneObject sceneObject;
        sceneObject.id = ObjectId::fromValue(objectValueId);
        sceneObject.layerId = LayerId::fromValue(objectLayerValue);
        sceneObject.geometry = std::move(shape);
        sceneObject.placementTranslation = placementTranslation;
        snapshot.objects.append(std::move(sceneObject));
        objectIds.insert(objectValueId);
        if (objectValueId < std::numeric_limits<quint64>::max()) {
            nextObjectValue = std::max(nextObjectValue, objectValueId + 1);
        }
    }

    snapshot.nextLayerValue = nextLayerValue;
    snapshot.nextObjectValue = nextObjectValue;
    document->restoreSnapshot(snapshot);
    if (document->activeLayerId() != snapshot.activeLayerId) {
        setError(errorMessage, QStringLiteral("restored active layer is invalid"));
        return false;
    }
    return true;
}

} // namespace classiCAD
