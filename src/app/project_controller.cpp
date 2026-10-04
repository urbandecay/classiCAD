#include "project_controller.h"

#include "application_session.h"

#include <cmath>
#include <utility>

namespace classiCAD {

namespace {

bool validCameraPreferences(const ProjectViewportCameraSettings &settings)
{
    return std::isfinite(settings.focalLengthMillimeters) &&
           settings.focalLengthMillimeters >= 1.0 &&
           settings.focalLengthMillimeters <= 2000.0 &&
           std::isfinite(settings.clipStart) && settings.clipStart >= 0.000001 &&
           std::isfinite(settings.clipEnd) &&
           settings.clipEnd > settings.clipStart && settings.clipEnd <= 1.0e9;
}

} // namespace

ProjectController::ProjectController(ApplicationSession &session)
    : session_(session)
{
}

bool ProjectController::save(const QString &path,
                             const ProjectViewportCameraSettings &cameraSettings,
                             QString *errorMessage) const
{
    return saveVignolaDocument(path, session_.document(), cameraSettings, errorMessage);
}

bool ProjectController::open(const QString &path,
                             ProjectViewportCameraSettings *cameraSettings,
                             QString *errorMessage)
{
    Document candidate;
    ProjectViewportCameraSettings restoredCameraSettings;
    if (!loadVignolaDocument(path,
                             &candidate,
                             &restoredCameraSettings,
                             errorMessage)) {
        return false;
    }
    if (restoredCameraSettings.storedInProject &&
        !validCameraPreferences(restoredCameraSettings)) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral(
                "The project contains invalid viewport camera settings.");
        }
        return false;
    }

    session_.replaceDocument(std::move(candidate));
    if (cameraSettings != nullptr) {
        *cameraSettings = restoredCameraSettings;
    }
    return true;
}

bool ProjectController::importRhino3dm(const QString &path,
                                       Rhino3dmImportReport *report,
                                       QString *errorMessage)
{
    DocumentTransaction transaction = session_.beginTransaction();
    Document candidate = session_.document();
    if (!classiCAD::importRhino3dmDocument(path,
                                          &candidate,
                                          report,
                                          errorMessage)) {
        return false;
    }
    transaction.replaceDocument(candidate.snapshot());
    return session_.commitTransaction(transaction);
}

void ProjectController::createNewDocument()
{
    session_.replaceDocument(Document{});
}

} // namespace classiCAD
