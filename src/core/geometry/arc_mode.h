#pragma once

#include <QString>

namespace classiCAD {

// Arc construction state persisted with an arc shape.
enum class ArcMode {
    OnePoint,
    TwoPoint,
    ThreePoint,
};

QString arcModeName(ArcMode mode);

} // namespace classiCAD
