#include "dimension_font.h"

#include <QFontDatabase>

namespace classiCAD {
namespace {

QString architecturalFontFamily()
{
    static const QString family = []() {
        const int fontId = QFontDatabase::addApplicationFont(
            QStringLiteral(":/fonts/ArchitectsDaughter-Regular.ttf"));
        const QStringList families = QFontDatabase::applicationFontFamilies(fontId);
        return families.isEmpty() ? QString{} : families.first();
    }();
    return family;
}

} // namespace

QFont dimensionAnnotationFont(DimensionFontStyle style)
{
    const QString family = style == DimensionFontStyle::Architectural
                               ? architecturalFontFamily()
                               : QStringLiteral("Sans");
    QFont font(family.isEmpty() ? QStringLiteral("Sans") : family);
    font.setPixelSize(12);
    return font;
}

} // namespace classiCAD
