#include "debug_log.h"

#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QTextStream>

namespace classiCAD {

DebugLog &DebugLog::instance()
{
    static DebugLog logger;
    return logger;
}

DebugLog::DebugLog()
{
    file_.setFileName(QStringLiteral("/tmp/classiCAD.log"));
    file_.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
}

void DebugLog::write(const QString &message)
{
    const QString line = QStringLiteral("[%1] %2")
                             .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs))
                             .arg(message);

    if (file_.isOpen()) {
        QTextStream stream(&file_);
        stream << line << Qt::endl;
        file_.flush();
    }

    qInfo().noquote() << line;
}

QString DebugLog::path() const
{
    return file_.fileName();
}

} // namespace classiCAD
