#pragma once

#include <QString>

namespace classiCAD {

class Document;

struct ProjectViewportCameraSettings {
    double focalLengthMillimeters = 50.0;
    double clipStart = 0.01;
    double clipEnd = 1000.0;
    bool storedInProject = false;
};

// .vignola and .blend projects are native Blender files produced by the pinned
// Blender runtime. The exact classiCAD document snapshot is stored in a
// Blender Text datablock so curve knots and other CAD data survive without
// translation.
bool saveVignolaDocument(const QString &path,
                         const Document &document,
                         QString *errorMessage = nullptr);

bool saveVignolaDocument(const QString &path,
                         const Document &document,
                         const ProjectViewportCameraSettings &cameraSettings,
                         QString *errorMessage = nullptr);

bool loadVignolaDocument(const QString &path,
                         Document *document,
                         QString *errorMessage = nullptr);

bool loadVignolaDocument(const QString &path,
                         Document *document,
                         ProjectViewportCameraSettings *cameraSettings,
                         QString *errorMessage = nullptr);

} // namespace classiCAD
