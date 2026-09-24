#include "document_serializer.h"

#include "core/model.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>

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
    serialized.insert(QStringLiteral("version"), 1);
    serialized.insert(QStringLiteral("activeLayerId"),
                      idToJson(document.activeLayerId().value()));

    QJsonArray layers;
    for (const Layer &layer : document.layers()) {
        QJsonObject serializedLayer;
        serializedLayer.insert(QStringLiteral("id"), idToJson(layer.id.value()));
        serializedLayer.insert(QStringLiteral("name"), layer.name);
        serializedLayer.insert(QStringLiteral("visible"), layer.visible);
        serializedLayer.insert(QStringLiteral("locked"), layer.locked);

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
    if (serialized.value(QStringLiteral("version")).toInt(-1) != 1) {
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
    if (activeLayer == snapshot.layers.cend() || !activeLayer->visible || activeLayer->locked) {
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
        Shape shape{GeometryType::Invalid,
                    {},
                    Shape::NurbsCurve2D{},
                    ArcMode::TwoPoint,
                    0.0,
                    {},
                    {}};
        if (!idFromJson(serializedObject.value(QStringLiteral("id")), &objectValueId) ||
            objectIds.contains(objectValueId) ||
            !idFromJson(serializedObject.value(QStringLiteral("layerId")), &objectLayerValue) ||
            !layerIds.contains(objectLayerValue) ||
            !shapeFromJson(serializedObject.value(QStringLiteral("geometry")), &shape)) {
            setError(errorMessage, QStringLiteral("invalid scene object record"));
            return false;
        }

        snapshot.objects.append(SceneObject{ObjectId::fromValue(objectValueId),
                                            LayerId::fromValue(objectLayerValue),
                                            shape});
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
