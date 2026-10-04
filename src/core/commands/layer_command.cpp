#include "layer_command.h"

namespace classiCAD {
namespace {

bool hasEditableAlternative(const Document &document, LayerId excludedLayer)
{
    for (const Layer &layer : document.layers()) {
        if (layer.id != excludedLayer && layer.visible && !layer.frozen &&
            !layer.locked) {
            return true;
        }
    }
    return false;
}

LayerCommandResult acceptedUnchanged(LayerId layerId)
{
    LayerCommandResult result;
    result.accepted = true;
    result.layerId = layerId;
    return result;
}

LayerCommandResult acceptedChanged(LayerId layerId,
                                   bool pruneSelection,
                                   bool redraw,
                                   int count = 0)
{
    LayerCommandResult result;
    result.accepted = true;
    result.layerId = layerId;
    result.count = count;
    result.changed = true;
    result.pruneSelection = pruneSelection;
    result.redraw = redraw;
    return result;
}

} // namespace

LayerCommandResult LayerCommand::execute(
    Document &document,
    History &history,
    const LayerCommandRequest &request,
    const QVector<ObjectId> &selectedObjectIds)
{
    switch (request.command) {
    case LayerCommandOperation::Create: {
        history.record();
        const LayerId layerId = document.createLayer(request.name);
        if (!document.setActiveLayer(layerId)) {
            return {};
        }
        return acceptedChanged(layerId, false, true);
    }
    case LayerCommandOperation::Remove: {
        const Layer *layer = document.layer(request.layerId);
        if (layer == nullptr || document.layers().size() <= 1 ||
            !layer->objectIds.isEmpty() ||
            (document.activeLayerId() == request.layerId &&
             !hasEditableAlternative(document, request.layerId))) {
            return {};
        }
        history.record();
        if (!document.removeLayer(request.layerId)) {
            return {};
        }
        return acceptedChanged(document.activeLayerId(), true, true);
    }
    case LayerCommandOperation::Activate: {
        if (document.activeLayerId() == request.layerId) {
            return acceptedUnchanged(request.layerId);
        }
        if (!document.isLayerEditable(request.layerId)) {
            return {};
        }
        history.record();
        if (!document.setActiveLayer(request.layerId)) {
            return {};
        }
        return acceptedChanged(request.layerId, false, true);
    }
    case LayerCommandOperation::SetVisible: {
        const Layer *layer = document.layer(request.layerId);
        if (layer == nullptr) {
            return {};
        }
        if (layer->visible == request.enabled) {
            return acceptedUnchanged(request.layerId);
        }
        if (!request.enabled && document.activeLayerId() == request.layerId &&
            !hasEditableAlternative(document, request.layerId)) {
            return {};
        }
        history.record();
        if (!document.setLayerVisible(request.layerId, request.enabled)) {
            return {};
        }
        return acceptedChanged(request.layerId, true, true);
    }
    case LayerCommandOperation::SetLocked: {
        const Layer *layer = document.layer(request.layerId);
        if (layer == nullptr) {
            return {};
        }
        if (layer->locked == request.enabled) {
            return acceptedUnchanged(request.layerId);
        }
        if (request.enabled && document.activeLayerId() == request.layerId &&
            !hasEditableAlternative(document, request.layerId)) {
            return {};
        }
        history.record();
        if (!document.setLayerLocked(request.layerId, request.enabled)) {
            return {};
        }
        return acceptedChanged(request.layerId, true, true);
    }
    case LayerCommandOperation::SetFrozen: {
        const Layer *layer = document.layer(request.layerId);
        if (layer == nullptr) {
            return {};
        }
        if (layer->frozen == request.enabled) {
            return acceptedUnchanged(request.layerId);
        }
        if (request.enabled && document.activeLayerId() == request.layerId &&
            !hasEditableAlternative(document, request.layerId)) {
            return {};
        }
        history.record();
        if (!document.setLayerFrozen(request.layerId, request.enabled)) {
            return {};
        }
        return acceptedChanged(request.layerId, true, true);
    }
    case LayerCommandOperation::SetColor: {
        const Layer *layer = document.layer(request.layerId);
        if (layer == nullptr || !request.color.isValid()) {
            return {};
        }
        if (layer->color == request.color) {
            return acceptedUnchanged(request.layerId);
        }
        history.record();
        if (!document.setLayerColor(request.layerId, request.color)) {
            return {};
        }
        return acceptedChanged(request.layerId, false, true);
    }
    case LayerCommandOperation::SetLineType: {
        const Layer *layer = document.layer(request.layerId);
        const QString lineType = request.name.trimmed();
        if (layer == nullptr || lineType.isEmpty()) {
            return {};
        }
        if (layer->lineType == lineType) {
            return acceptedUnchanged(request.layerId);
        }
        history.record();
        if (!document.setLayerLineType(request.layerId, lineType)) {
            return {};
        }
        return acceptedChanged(request.layerId, false, true);
    }
    case LayerCommandOperation::SetLineWeight: {
        const Layer *layer = document.layer(request.layerId);
        if (layer == nullptr) {
            return {};
        }
        if (layer->lineWeightMm == request.lineWeightMm) {
            return acceptedUnchanged(request.layerId);
        }
        history.record();
        if (!document.setLayerLineWeight(request.layerId,
                                         request.lineWeightMm)) {
            return {};
        }
        return acceptedChanged(request.layerId, false, true);
    }
    case LayerCommandOperation::SetPlotted: {
        const Layer *layer = document.layer(request.layerId);
        if (layer == nullptr) {
            return {};
        }
        if (layer->plotted == request.enabled) {
            return acceptedUnchanged(request.layerId);
        }
        history.record();
        if (!document.setLayerPlotted(request.layerId, request.enabled)) {
            return {};
        }
        return acceptedChanged(request.layerId, false, true);
    }
    case LayerCommandOperation::SetDescription: {
        const Layer *layer = document.layer(request.layerId);
        if (layer == nullptr) {
            return {};
        }
        if (layer->description == request.name) {
            return acceptedUnchanged(request.layerId);
        }
        history.record();
        if (!document.setLayerDescription(request.layerId, request.name)) {
            return {};
        }
        return acceptedChanged(request.layerId, false, false);
    }
    case LayerCommandOperation::Rename: {
        const Layer *layer = document.layer(request.layerId);
        const QString trimmedName = request.name.trimmed();
        if (layer == nullptr || trimmedName.isEmpty()) {
            return {};
        }
        if (layer->name == trimmedName) {
            return acceptedUnchanged(request.layerId);
        }
        history.record();
        if (!document.renameLayer(request.layerId, trimmedName)) {
            return {};
        }
        return acceptedChanged(request.layerId, false, true);
    }
    case LayerCommandOperation::Move: {
        const int currentIndex = [&document, &request]() {
            for (int index = 0; index < document.layers().size(); ++index) {
                if (document.layers()[index].id == request.layerId) {
                    return index;
                }
            }
            return -1;
        }();
        if (currentIndex < 0 || request.index < 0 ||
            request.index >= document.layers().size()) {
            return {};
        }
        if (currentIndex == request.index) {
            return acceptedUnchanged(request.layerId);
        }
        history.record();
        if (!document.moveLayer(request.layerId, request.index)) {
            return {};
        }
        return acceptedChanged(request.layerId, false, true);
    }
    case LayerCommandOperation::MoveSelectedObjects: {
        if (!document.isLayerEditable(request.layerId)) {
            return {};
        }
        QVector<ObjectId> movableObjects;
        for (const ObjectId objectId : selectedObjectIds) {
            const SceneObject *object = document.object(objectId);
            if (document.isObjectEditable(objectId) && object != nullptr &&
                object->layerId != request.layerId) {
                movableObjects.append(objectId);
            }
        }
        if (movableObjects.isEmpty()) {
            return {};
        }
        history.record();
        int movedCount = 0;
        for (const ObjectId objectId : movableObjects) {
            if (document.moveObjectToLayer(objectId, request.layerId)) {
                ++movedCount;
            }
        }
        if (movedCount == 0) {
            return {};
        }
        return acceptedChanged(request.layerId, false, true, movedCount);
    }
    }
    return {};
}

} // namespace classiCAD
