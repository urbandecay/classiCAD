#pragma once

#include "layer_id.h"
#include "object_id.h"

#include <QColor>
#include <QString>
#include <QVector>

#include <QtGlobal>

namespace classiCAD {

struct Layer {
    LayerId id = LayerId::invalid();
    QString name;
    QColor color = QColor(QStringLiteral("#d28b45"));
    QString lineType = QStringLiteral("Continuous");
    qreal lineWeightMm = 0.0;
    QString description;
    bool visible = true;
    bool frozen = false;
    bool locked = false;
    bool plotted = true;
    QVector<ObjectId> objectIds;
};

} // namespace classiCAD
