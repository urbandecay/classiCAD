#pragma once

#include <QColor>
#include <QString>

#include <cstdint>

namespace classiCAD {

// Shared Blender Workbench Random color identity resolver for software and
// GPU viewport paths.
QColor workbenchRandomObjectColor(std::uint64_t objectId,
                                  const QString &displayName,
                                  const QString &libraryPath);

} // namespace classiCAD
