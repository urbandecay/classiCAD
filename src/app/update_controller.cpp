#include "update_controller.h"

#include "core/debug_log.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QStringList>

namespace classiCAD {

UpdateController::UpdateController(QObject *parent)
    : QObject(parent)
{
}

UpdateController::~UpdateController() = default;

bool UpdateController::isRunning() const
{
    return restarting_;
}

bool UpdateController::start(const QString &executablePath,
                             const QString &workingDirectory,
                             const SessionSaver &saveSession,
                             const UpdateSessionWindowState &windowState,
                             const Callbacks &callbacks,
                             QString *errorMessage)
{
    if (isRunning()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Update already in progress.");
        }
        return false;
    }

    const QString sessionPath = QDir(QDir::tempPath()).filePath(
        QStringLiteral("classiCAD-update-%1-%2.json")
            .arg(QCoreApplication::applicationPid())
            .arg(QDateTime::currentMSecsSinceEpoch()));
    if (!saveSession || !saveSession(sessionPath)) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Could not save the current scene.");
        }
        return false;
    }
    QString handoffError;
    if (!writeWindowState(sessionPath, windowState, &handoffError)) {
        QFile::remove(sessionPath);
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Could not save the current window view: %1")
                                .arg(handoffError);
        }
        return false;
    }

    if (callbacks.status) {
        callbacks.status(QStringLiteral("Restarting classiCAD…"));
    }
    const QStringList arguments{QStringLiteral("--update-session"), sessionPath};
    if (!QProcess::startDetached(executablePath, arguments, workingDirectory)) {
        QFile::remove(sessionPath);
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Could not restart classiCAD.");
        }
        return false;
    }

    restarting_ = true;
    DebugLog::instance().write(
        QStringLiteral("restart handoff launched executable=%1 session=%2")
            .arg(executablePath, sessionPath));
    if (callbacks.finished) {
        callbacks.finished(true, QStringLiteral("Restarting classiCAD"));
    }
    if (callbacks.restarting) {
        callbacks.restarting();
    }
    return true;
}

bool UpdateController::readWindowState(const QString &sessionPath,
                                       UpdateSessionWindowState *state,
                                       QString *errorMessage) const
{
    if (state == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("No window-state output was provided.");
        }
        return false;
    }
    QFile input(sessionPath);
    if (!input.open(QIODevice::ReadOnly)) {
        if (errorMessage != nullptr) {
            *errorMessage = input.errorString();
        }
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(input.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (errorMessage != nullptr) {
            *errorMessage = parseError.errorString();
        }
        return false;
    }
    const QJsonObject root = document.object();
    UpdateSessionWindowState restored;
    restored.windowGeometry = QByteArray::fromBase64(
        root.value(QStringLiteral("windowGeometry")).toString().toLatin1());
    restored.workspaceSplitterState =
        QByteArray::fromBase64(
            root.value(QStringLiteral("workspaceSplitterState")).toString().toLatin1());
    const QJsonValue projectPath = root.value(QStringLiteral("projectPath"));
    const QJsonValue workspaceName = root.value(QStringLiteral("workspaceName"));
    const QJsonValue documentModified = root.value(QStringLiteral("documentModified"));
    if ((!projectPath.isUndefined() && !projectPath.isString()) ||
        (!workspaceName.isUndefined() && !workspaceName.isString()) ||
        (!documentModified.isUndefined() && !documentModified.isBool())) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Invalid saved window session state.");
        }
        return false;
    }
    restored.projectPath = projectPath.toString();
    restored.workspaceName = workspaceName.toString();
    restored.documentModified = documentModified.toBool();
    *state = restored;
    return true;
}

bool UpdateController::writeWindowState(const QString &sessionPath,
                                        const UpdateSessionWindowState &state,
                                        QString *errorMessage) const
{
    QFile input(sessionPath);
    if (!input.open(QIODevice::ReadOnly)) {
        if (errorMessage != nullptr) {
            *errorMessage = input.errorString();
        }
        return false;
    }
    QJsonParseError parseError;
    QJsonDocument document = QJsonDocument::fromJson(input.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (errorMessage != nullptr) {
            *errorMessage = parseError.errorString();
        }
        return false;
    }
    input.close();

    QJsonObject root = document.object();
    root.insert(QStringLiteral("windowGeometry"),
                QString::fromLatin1(state.windowGeometry.toBase64()));
    root.insert(QStringLiteral("workspaceSplitterState"),
                QString::fromLatin1(state.workspaceSplitterState.toBase64()));
    root.insert(QStringLiteral("projectPath"), state.projectPath);
    root.insert(QStringLiteral("workspaceName"), state.workspaceName);
    root.insert(QStringLiteral("documentModified"), state.documentModified);
    document.setObject(root);

    QSaveFile output(sessionPath);
    if (!output.open(QIODevice::WriteOnly)) {
        if (errorMessage != nullptr) {
            *errorMessage = output.errorString();
        }
        return false;
    }
    const QByteArray data = document.toJson(QJsonDocument::Compact);
    if (output.write(data) != data.size() || !output.commit()) {
        if (errorMessage != nullptr) {
            *errorMessage = output.errorString();
        }
        return false;
    }
    return true;
}

} // namespace classiCAD
