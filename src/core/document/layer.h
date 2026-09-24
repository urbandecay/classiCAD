#pragma once

#include "layer_id.h"
#include "object_id.h"

#include <QString>
#include <QVector>

namespace classiCAD {

struct Layer {
    LayerId id = LayerId::invalid();
    QString name;
    bool visible = true;
    bool locked = false;
    QVector<ObjectId> objectIds;
};

} // namespace classiCAD
