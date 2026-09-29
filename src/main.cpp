#include "core/debug_log.h"
#include "ui/main_window.h"

#include <QApplication>
#include <QSurfaceFormat>

int main(int argc, char *argv[])
{
    QSurfaceFormat viewportFormat;
    viewportFormat.setVersion(3, 3);
    viewportFormat.setProfile(QSurfaceFormat::CoreProfile);
    viewportFormat.setDepthBufferSize(24);
    QSurfaceFormat::setDefaultFormat(viewportFormat);

    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("classiCAD"));
    application.setOrganizationName(QStringLiteral("classiCAD"));

    QString updateSessionPath;
    const QStringList arguments = application.arguments();
    for (int index = 1; index + 1 < arguments.size(); ++index) {
        if (arguments[index] == QStringLiteral("--update-session")) {
            updateSessionPath = arguments[index + 1];
            break;
        }
    }

    classiCAD::DebugLog::instance().write(
        QStringLiteral("application start logFile=%1")
            .arg(classiCAD::DebugLog::instance().path()));

    return classiCAD::runApplication(application, updateSessionPath);
}
