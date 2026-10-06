#pragma once

#include <QObject>
#include <QByteArray>
#include <QString>

#include <functional>

namespace classiCAD {

struct UpdateSessionWindowState {
    QByteArray windowGeometry;
    QByteArray workspaceSplitterState;
    QString projectPath;
    QString workspaceName;
    bool documentModified = false;
};

// Coordinates the restart handoff while leaving dialogs and status
// presentation to MainWindow.
class UpdateController final : public QObject {
public:
    using SessionSaver = std::function<bool(const QString &)>;

    struct Callbacks {
        std::function<void(const QString &)> status;
        std::function<void(bool, const QString &)> finished;
        std::function<void()> restarting;
    };

    explicit UpdateController(QObject *parent = nullptr);
    ~UpdateController() override;

    bool isRunning() const;
    bool start(const QString &executablePath,
               const QString &workingDirectory,
               const SessionSaver &saveSession,
               const UpdateSessionWindowState &windowState,
               const Callbacks &callbacks,
               QString *errorMessage = nullptr);

    bool readWindowState(const QString &sessionPath,
                         UpdateSessionWindowState *state,
                         QString *errorMessage = nullptr) const;

private:
    bool writeWindowState(const QString &sessionPath,
                          const UpdateSessionWindowState &state,
                          QString *errorMessage) const;

    bool restarting_ = false;
};

} // namespace classiCAD
