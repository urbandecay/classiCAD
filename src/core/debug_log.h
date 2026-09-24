#pragma once

#include <QFile>
#include <QString>

namespace classiCAD {

class DebugLog final {
public:
    static DebugLog &instance();

    void write(const QString &message);
    QString path() const;

private:
    DebugLog();
    QFile file_;
};

} // namespace classiCAD
