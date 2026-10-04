#pragma once

#include "core/serialization/blender_project_file.h"
#include "core/serialization/rhino3dm_interchange.h"

namespace classiCAD {

class ApplicationSession;

// Owns document-level open, save, import, and replacement workflows. Qt
// dialogs and viewport camera presentation remain at the UI boundary.
class ProjectController final {
public:
    explicit ProjectController(ApplicationSession &session);

    bool save(const QString &path,
              const ProjectViewportCameraSettings &cameraSettings,
              QString *errorMessage = nullptr) const;
    bool open(const QString &path,
              ProjectViewportCameraSettings *cameraSettings = nullptr,
              QString *errorMessage = nullptr);
    bool importRhino3dm(const QString &path,
                        Rhino3dmImportReport *report = nullptr,
                        QString *errorMessage = nullptr);
    void createNewDocument();

private:
    ApplicationSession &session_;
};

} // namespace classiCAD
