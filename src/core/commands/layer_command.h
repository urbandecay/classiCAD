#pragma once

#include "core/document/document.h"
#include "core/history/history.h"

#include <QColor>
#include <QString>
#include <QVector>

namespace classiCAD {

enum class LayerCommandOperation {
    Create,
    Remove,
    Activate,
    SetVisible,
    SetLocked,
    Rename,
    Move,
    MoveSelectedObjects,
    SetColor,
    SetFrozen,
    SetLineType,
    SetLineWeight,
    SetPlotted,
    SetDescription,
};

struct LayerCommandRequest {
    LayerCommandOperation command = LayerCommandOperation::Create;
    LayerId layerId = LayerId::invalid();
    int index = -1;
    bool enabled = false;
    QString name;
    QColor color;
    qreal lineWeightMm = 0.0;
};

struct LayerCommandResult {
    bool accepted = false;
    LayerId layerId = LayerId::invalid();
    int count = 0;
    bool changed = false;
    bool pruneSelection = false;
    bool redraw = false;
};

class LayerCommand final {
public:
    static LayerCommandResult execute(
        Document &document,
        History &history,
        const LayerCommandRequest &request,
        const QVector<ObjectId> &selectedObjectIds);
};

} // namespace classiCAD
