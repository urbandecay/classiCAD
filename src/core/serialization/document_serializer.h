#pragma once

#include "core/document/document.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace classiCAD {

QJsonObject documentToJson(const Document &document);
bool documentFromJson(const QJsonValue &value,
                      Document *document,
                      QString *errorMessage = nullptr);

} // namespace classiCAD
