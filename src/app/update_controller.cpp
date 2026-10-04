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

UpdateController::~UpdateController()
{
    if (process_ != nullptr && process_->state() != QProcess::NotRunning) {
        process_->disconnect(this);
        process_->kill();
        process_->waitForFinished(1000);
    }
}

bool UpdateController::isRunning() const
{
    return process_ != nullptr;
}

bool UpdateController::start(const QString &executablePath,
                             const QString &buildDirectory,
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

    auto *process = new QProcess(this);
    process_ = process;
    process->setWorkingDirectory(buildDirectory);

    const auto failUpdate = [this, process, sessionPath, callbacks](const QString &message) {
        fail(process, sessionPath, callbacks, message);
    };
    connect(process,
            &QProcess::errorOccurred,
            this,
            [process, failUpdate](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart) {
                    failUpdate(QStringLiteral("Could not start cmake: %1")
                                   .arg(process->errorString()));
                }
            });
    connect(process,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this,
            [this,
             process,
             sessionPath,
             executablePath,
             buildDirectory,
             saveSession,
             windowState,
             callbacks,
             failUpdate](int exitCode, QProcess::ExitStatus exitStatus) {
                if (process_ != process) {
                    return;
                }
                const QString output =
                    QString::fromLocal8Bit(process->readAllStandardOutput() +
                                            process->readAllStandardError())
                        .trimmed();
                if (!output.isEmpty()) {
                    DebugLog::instance().write(
                        QStringLiteral("update build output: %1").arg(output));
                }
                if (exitStatus != QProcess::NormalExit || exitCode != 0) {
                    failUpdate(QStringLiteral("Build exited with code %1").arg(exitCode));
                    return;
                }

                QString handoffError;
                if (!saveSession(sessionPath) ||
                    !writeWindowState(sessionPath, windowState, &handoffError)) {
                    failUpdate(QStringLiteral("Could not preserve the current view: %1")
                                   .arg(handoffError));
                    return;
                }

                const QStringList arguments{QStringLiteral("--update-session"), sessionPath};
                if (!QProcess::startDetached(executablePath,
                                             arguments,
                                             buildDirectory)) {
                    failUpdate(QStringLiteral("Could not restart classiCAD."));
                    return;
                }

                process_ = nullptr;
                process->deleteLater();
                if (callbacks.finished) {
                    callbacks.finished(true, QStringLiteral("Update complete — restarting classiCAD"));
                }
                if (callbacks.restarting) {
                    callbacks.restarting();
                }
            });

    if (callbacks.status) {
        callbacks.status(QStringLiteral("Updating classiCAD — rebuilding…"));
    }
    process->start(QStringLiteral("cmake"),
                   QStringList{QStringLiteral("--build"),
                               buildDirectory,
                               QStringLiteral("--target"),
                               QStringLiteral("classiCAD")});
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

void UpdateController::fail(QProcess *process,
                            const QString &sessionPath,
                            const Callbacks &callbacks,
                            const QString &message)
{
    if (process_ != process) {
        return;
    }
    process_ = nullptr;
    QFile::remove(sessionPath);
    process->deleteLater();
    if (callbacks.finished) {
        callbacks.finished(false, message);
    }
}

} // namespace classiCAD
