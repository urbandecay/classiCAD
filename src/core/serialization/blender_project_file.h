#pragma once

#include <QString>
#include <QHash>
#include <QByteArray>
#include <QJsonArray>
#include <QVector>

#include "core/document/scene_object.h"
#include "core/document/workbench_material_adapter.h"
#include "core/document/document_settings.h"
#include "core/document/document_collection.h"

namespace classiCAD {

class Document;

struct ProjectViewportViewState {
    bool cameraView = false;
    double cameraZoom = 0.0;
    double cameraOffsetX = 0.0;
    double cameraOffsetY = 0.0;
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
    // Temporary transport for Blender's per-space free-view projection.
    // Applied to viewport camera preferences during ordinary .blend import.
    bool hasViewOptics = false;
    double viewLensMillimeters = 50.0;
    double viewClipStart = 0.01;
    double viewClipEnd = 1000.0;
    bool storedInProject = false;
};

struct ProjectViewportCameraSettings {
    double focalLengthMillimeters = 50.0;
    double clipStart = 0.01;
    double clipEnd = 1000.0;
    bool sceneCameraEnabled = false;
    double focusDistance = 10000.0;
    double fStop = 2.8;
    int apertureBlades = 0;
    double apertureRotationRadians = 0.0;
    double apertureRatio = 1.0;
    double sensorWidthMillimeters = 36.0;
    double opticalSensorSizeMillimeters = 36.0;
    // Imported free-view lens/clipping values override only these viewport
    // projection fields while preserving the user's other camera preferences.
    bool hasImportedViewOptics = false;
    ProjectViewportViewState view;
    bool storedInProject = false;
};

struct BlenderSceneImportReport {
    int importedObjectCount = 0;
    int skippedObjectCount = 0;
    int unsupportedFeatureCount = 0;
    QString blenderVersion;
    QString importMode;
    QString warningMessage;
    QString activeSceneId;
    QString activeViewLayerKey;
    ProjectViewportViewState initialViewportView;
    QJsonArray scenes;
    QJsonArray outlinerOnlyObjects;
    // Indices refer to the imported-object array produced by
    // importBlenderScene(); ordinary .blend open also resolves these to the
    // newly assigned classiCAD IDs before returning the report.
    QVector<int> selectedObjectIndices;
    int activeObjectIndex = -1;
    QVector<ObjectId> selectedObjectIds;
    ObjectId activeObjectId = ObjectId::invalid();
    QHash<QString, QByteArray> importedBlenderColorLuts;
    QHash<QString, ImportedBlenderColorLutMetadata> importedBlenderColorLutMetadata;
};

// Extracts supported Blender scene geometry through the pinned Python runtime.
// Returned meshes, curves, and surfaces retain their authoritative geometry;
// transforms follow each geometry type's native representation. No project
// document is replaced here.
bool importBlenderScene(const QString &path,
                        const QString &mode,
                        QVector<SceneObject> *objects,
                        QVector<DocumentCollection> *collections,
                        QVector<DocumentMaterial> *materials,
                        BlenderSceneImportReport *report = nullptr,
                        QString *errorMessage = nullptr);

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
                         QString *errorMessage = nullptr,
                         BlenderSceneImportReport *ordinaryImportReport = nullptr);

} // namespace classiCAD
