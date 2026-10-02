#pragma once

#include <QString>

namespace classiCAD {

class Document;

struct ProjectViewportViewState {
    double zoom = 1.0;
    double panX = 0.0;
    double panY = 0.0;
    double orbitPivotX = 0.0;
    double orbitPivotY = 0.0;
    double orbitPivotZ = 0.0;
    double yawRadians = 0.0;
    double pitchRadians = 0.0;
    bool perspective = false;
    int preset = 0;
    double gridViewDistance = 60.0;
    double orientationW = 1.0;
    double orientationX = 0.0;
    double orientationY = 0.0;
    double orientationZ = 0.0;
    bool hasOrientation = false;
    double targetX = 0.0;
    double targetY = 0.0;
    double targetZ = 0.0;
    bool storedInProject = false;
};

struct ProjectViewportCameraSettings {
    double focalLengthMillimeters = 50.0;
    double clipStart = 0.01;
    double clipEnd = 1000.0;
    ProjectViewportViewState view;
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
