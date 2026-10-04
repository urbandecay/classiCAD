#pragma once

#include "core/document/document.h"
#include "core/document/selection_model.h"
#include "services/viewport/viewport_transform.h"

#include <QString>

namespace classiCAD {

struct UpdateSessionViewState {
    qreal zoom = 1.0;
    QPointF pan;
    ViewportCameraState camera;
    WorkPlane workPlane = WorkPlane::XY;
    qreal workPlaneOffset = 0.0;
    WorkPlaneFrame workPlaneFrame;
    ViewportCameraPreferences cameraPreferences;
    QVector<ObjectId> selectedObjectIds;
    ObjectId primaryObjectId = ObjectId::invalid();
    ControlPointReference activeControlPoint;
    bool controlPointsVisible = false;
};

struct RestoredUpdateSession {
    Document document;
    UpdateSessionViewState view;
    int version = 0;
};

// Reads and writes the update handoff format. Versioned JSON validation lives
// here so the viewport only applies already validated session state.
class SessionSerializer final {
public:
    static bool write(const QString &path,
                      const Document &document,
                      const UpdateSessionViewState &view,
                      QString *errorMessage = nullptr);

    static bool read(const QString &path,
                     const ViewportCameraPreferences &defaultCameraPreferences,
                     bool defaultControlPointsVisible,
                     RestoredUpdateSession *session,
                     QString *errorMessage = nullptr);
};

} // namespace classiCAD
