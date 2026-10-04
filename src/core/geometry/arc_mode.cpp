#include "arc_mode.h"

namespace classiCAD {

QString arcModeName(ArcMode mode)
{
    switch (mode) {
    case ArcMode::OnePoint:
        return QStringLiteral("1 Point Arc");
    case ArcMode::TwoPoint:
        return QStringLiteral("2 Point Arc");
    case ArcMode::ThreePoint:
        return QStringLiteral("3 Point Arc");
    }

    return QStringLiteral("Arc");
}

} // namespace classiCAD
