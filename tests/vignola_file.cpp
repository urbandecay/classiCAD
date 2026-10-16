#include "core/document/document.h"
#include "core/geometry/curve_construction.h"
#include "core/geometry/shape_mapping.h"
#include "core/geometry/nurbs_surface_factory.h"
#include "core/serialization/document_serializer.h"
#include "core/serialization/blender_project_file.h"
#include "core/serialization/rhino3dm_interchange.h"

#include <opennurbs.h>

#include <QApplication>
#include <QJsonArray>
#include <QDir>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>

using namespace classiCAD;

namespace {

bool check(bool condition, const char *message)
{
    if (!condition) {
        qCritical("%s", message);
    }
    return condition;
}

bool equivalentJson(const QJsonValue &first, const QJsonValue &second)
{
    if (first.isDouble() && second.isDouble()) {
        const double firstNumber = first.toDouble();
        const double secondNumber = second.toDouble();
        const double scale = std::max({1.0, std::abs(firstNumber), std::abs(secondNumber)});
        return std::abs(firstNumber - secondNumber) <= 1.0e-12 * scale;
    }
    if (first.isArray() && second.isArray()) {
        const QJsonArray firstArray = first.toArray();
        const QJsonArray secondArray = second.toArray();
        if (firstArray.size() != secondArray.size()) {
            return false;
        }
        for (qsizetype index = 0; index < firstArray.size(); ++index) {
            if (!equivalentJson(firstArray[index], secondArray[index])) {
                return false;
            }
        }
        return true;
    }
    if (first.isObject() && second.isObject()) {
        const QJsonObject firstObject = first.toObject();
        const QJsonObject secondObject = second.toObject();
        if (firstObject.keys() != secondObject.keys()) {
            return false;
        }
        for (auto iterator = firstObject.constBegin(); iterator != firstObject.constEnd(); ++iterator) {
            if (!equivalentJson(iterator.value(), secondObject.value(iterator.key()))) {
                return false;
            }
        }
        return true;
    }
    return first == second;
}

} // namespace

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    bool passed = true;

    Document source;
    const CollectionId drawingCollection = source.createCollection(QStringLiteral("Drawing"));
    passed &= check(drawingCollection.isValid(),
                    "test document must be able to create a Blender collection");

    Shape circle;
    circle.geometryType = GeometryType::Circle;
    circle.points = {QPointF(12.0, -4.0), QPointF(22.0, -4.0)};
    circle.nurbs = makeCircleNurbs(circle.points);
    const ObjectId circleId = source.append(circle);
    source.setObjectCollections(circleId, {drawingCollection});

    Shape line;
    line.geometryType = GeometryType::Line;
    line.points = {QPointF(-3.0, 1.5), QPointF(8.0, 6.0)};
    line.nurbs = makeDegreeOneNurbs(line.points);
    line.workPlane = WorkPlane::XZ;
    line.workPlaneOffset = 2.5;
    const ObjectId lineId = source.append(line);
    source.setObjectCollections(lineId, {drawingCollection});

    Shape dimension;
    dimension.geometryType = GeometryType::LinearDimension;
    dimension.points = {QPointF(12.0, -4.0), QPointF(22.0, -4.0), QPointF(17.0, 4.0)};
    dimension.dimensionAnchors = {
        DimensionAnchorReference{circleId,
                                 DimensionAnchorKind::CurveParameter,
                                 0,
                                 -1,
                                 0.25},
        DimensionAnchorReference{circleId,
                                 DimensionAnchorKind::CurveParameter,
                                 0,
                                 -1,
                                 0.75}};
    const ObjectId dimensionId = source.append(dimension);
    source.setObjectCollections(dimensionId, {drawingCollection});

    Shape picture;
    picture.geometryType = GeometryType::Picture;
    picture.points = {QPointF(-12.0, 5.0),
                      QPointF(-8.0, 5.0),
                      QPointF(-8.0, 3.0),
                      QPointF(-12.0, 3.0)};
    picture.pictureImage = QImage(4, 2, QImage::Format_ARGB32);
    picture.pictureImage.fill(QColor(QStringLiteral("#4c86b8")));
    const ObjectId pictureId = source.append(picture);
    source.setObjectCollections(pictureId, {drawingCollection});

    Shape solid;
    solid.geometryType = GeometryType::NurbsSolid;
    NurbsSurface3D cap;
    passed &= check(makeNurbsPlanarFillSurface(circle.nurbs, shapeWorkPlaneFrame(circle), &cap) &&
                        makeNurbsExtrusionSolid(cap, {0,0,7}, &solid.nurbsSolid),
                    "solid save fixture must have exact caps and walls");
    DocumentMaterial capMaterial;
    capMaterial.id = QStringLiteral("blend-cap-material");
    capMaterial.name = QStringLiteral("Blend Cap");
    DocumentMaterial wallMaterial;
    wallMaterial.id = QStringLiteral("blend-wall-material");
    wallMaterial.name = QStringLiteral("Blend Wall");
    Document::Snapshot materialSnapshot = source.snapshot();
    materialSnapshot.materials.append(capMaterial);
    materialSnapshot.materials.append(wallMaterial);
    source.restoreSnapshot(materialSnapshot);
    SceneObject solidObject;
    solidObject.collectionIds = {drawingCollection};
    solidObject.geometry = solid;
    solidObject.nurbsSolidFaceMaterialIds.insert(
        solid.nurbsSolid.baseCapFaceId, capMaterial.id);
    solidObject.nurbsSolidFaceMaterialIds.insert(
        solid.nurbsSolid.displacedCapFaceId, capMaterial.id);
    for (const QString &wallFaceId : solid.nurbsSolid.wallFaceIds)
        solidObject.nurbsSolidFaceMaterialIds.insert(wallFaceId, wallMaterial.id);
    const ObjectId solidObjectId = source.insertObject(
        static_cast<int>(source.objects().size()), solidObject);

    QTemporaryDir temporaryDirectory;
    passed &= check(temporaryDirectory.isValid(), "temporary directory must be available");
    if (!temporaryDirectory.isValid()) {
        return 1;
    }
    const QString path = QDir(temporaryDirectory.path()).filePath(
        QStringLiteral("round-trip.vignola"));

    ProjectViewportCameraSettings cameraSettings;
    cameraSettings.focalLengthMillimeters = 85.2;
    cameraSettings.clipStart = 0.125;
    cameraSettings.clipEnd = 12000.0;
    cameraSettings.sceneCameraEnabled = true;
    cameraSettings.focusDistance = 124.5;
    cameraSettings.fStop = 5.6;
    cameraSettings.apertureBlades = 7;
    cameraSettings.apertureRotationRadians = -0.375;
    cameraSettings.apertureRatio = 0.8;
    cameraSettings.sensorWidthMillimeters = 42.5;
    cameraSettings.opticalSensorSizeMillimeters = 29.7;
    cameraSettings.storedInProject = true;
    cameraSettings.view.zoom = 2.75;
    cameraSettings.view.panX = 14.25;
    cameraSettings.view.panY = -8.5;
    cameraSettings.view.orbitPivotX = 2.0;
    cameraSettings.view.orbitPivotY = -3.0;
    cameraSettings.view.orbitPivotZ = 5.5;
    cameraSettings.view.yawRadians = 0.625;
    cameraSettings.view.pitchRadians = -0.25;
    cameraSettings.view.perspective = true;
    cameraSettings.view.preset = 8;
    cameraSettings.view.gridViewDistance = 275.0;
    cameraSettings.view.orientationW = 0.9238795325;
    cameraSettings.view.orientationX = 0.0;
    cameraSettings.view.orientationY = 0.3826834324;
    cameraSettings.view.orientationZ = 0.0;
    cameraSettings.view.hasOrientation = true;
    cameraSettings.view.targetX = 1.25;
    cameraSettings.view.targetY = -2.5;
    cameraSettings.view.targetZ = 3.75;
    cameraSettings.view.storedInProject = true;

    QString errorMessage;
    if (!saveVignolaDocument(path, source, cameraSettings, &errorMessage)) {
        qCritical("Could not save Vignola test document: %s", qPrintable(errorMessage));
        return 1;
    }

    Document restored;
    ProjectViewportCameraSettings restoredCamera;
    if (!loadVignolaDocument(path, &restored, &restoredCamera, &errorMessage)) {
        qCritical("Could not load Vignola test document: %s", qPrintable(errorMessage));
        return 1;
    }
    passed &= check(equivalentJson(documentToJson(source), documentToJson(restored)),
                    "Vignola save/load must preserve document geometry, IDs, layers, and dimension anchors");
    const auto cameraClose = [](double first, double second) {
        return std::abs(first - second) < 1.0e-10;
    };
    const bool cameraSettingsRoundTrip =
        cameraClose(restoredCamera.focalLengthMillimeters,
                    cameraSettings.focalLengthMillimeters) &&
        cameraClose(restoredCamera.clipStart, cameraSettings.clipStart) &&
        cameraClose(restoredCamera.clipEnd, cameraSettings.clipEnd) &&
        restoredCamera.sceneCameraEnabled == cameraSettings.sceneCameraEnabled &&
        cameraClose(restoredCamera.focusDistance, cameraSettings.focusDistance) &&
        cameraClose(restoredCamera.fStop, cameraSettings.fStop) &&
        restoredCamera.apertureBlades == cameraSettings.apertureBlades &&
        cameraClose(restoredCamera.apertureRotationRadians,
                    cameraSettings.apertureRotationRadians) &&
        cameraClose(restoredCamera.apertureRatio, cameraSettings.apertureRatio) &&
        cameraClose(restoredCamera.sensorWidthMillimeters,
                    cameraSettings.sensorWidthMillimeters) &&
        cameraClose(restoredCamera.opticalSensorSizeMillimeters,
                    cameraSettings.opticalSensorSizeMillimeters) &&
        restoredCamera.storedInProject && restoredCamera.view.storedInProject &&
        cameraClose(restoredCamera.view.zoom, cameraSettings.view.zoom) &&
        cameraClose(restoredCamera.view.panX, cameraSettings.view.panX) &&
        cameraClose(restoredCamera.view.panY, cameraSettings.view.panY) &&
        cameraClose(restoredCamera.view.orbitPivotX,
                    cameraSettings.view.orbitPivotX) &&
        cameraClose(restoredCamera.view.orbitPivotY,
                    cameraSettings.view.orbitPivotY) &&
        cameraClose(restoredCamera.view.orbitPivotZ,
                    cameraSettings.view.orbitPivotZ) &&
        cameraClose(restoredCamera.view.yawRadians,
                    cameraSettings.view.yawRadians) &&
        cameraClose(restoredCamera.view.pitchRadians,
                    cameraSettings.view.pitchRadians) &&
        restoredCamera.view.perspective == cameraSettings.view.perspective &&
        restoredCamera.view.preset == cameraSettings.view.preset &&
        cameraClose(restoredCamera.view.gridViewDistance,
                    cameraSettings.view.gridViewDistance) &&
        restoredCamera.view.hasOrientation &&
        cameraClose(restoredCamera.view.orientationW,
                    cameraSettings.view.orientationW) &&
        cameraClose(restoredCamera.view.orientationX,
                    cameraSettings.view.orientationX) &&
        cameraClose(restoredCamera.view.orientationY,
                    cameraSettings.view.orientationY) &&
        cameraClose(restoredCamera.view.orientationZ,
                    cameraSettings.view.orientationZ) &&
        cameraClose(restoredCamera.view.targetX, cameraSettings.view.targetX) &&
        cameraClose(restoredCamera.view.targetY, cameraSettings.view.targetY) &&
        cameraClose(restoredCamera.view.targetZ, cameraSettings.view.targetZ);
    passed &= check(cameraSettingsRoundTrip,
                    "native Vignola save/load must preserve camera optics and stored viewport camera state");
    ProjectViewportCameraSettings invalidCameraSettings = cameraSettings;
    invalidCameraSettings.opticalSensorSizeMillimeters = 0.5;
    QString invalidCameraError;
    passed &= check(!saveVignolaDocument(
                        QDir(temporaryDirectory.path()).filePath(
                            QStringLiteral("invalid-camera.vignola")),
                        source, invalidCameraSettings, &invalidCameraError) &&
                        !invalidCameraError.isEmpty(),
                    "native project save must reject out-of-range optical sensor settings");
    const SceneObject *restoredSolidObject = restored.object(solidObjectId);
    passed &= check(restoredSolidObject != nullptr &&
                        restoredSolidObject->nurbsSolidFaceMaterialIds ==
                            solidObject.nurbsSolidFaceMaterialIds,
                    "Blender project proxies must preserve cap and wall assignments by semantic face ID");

    ONX_Model rhinoModel;
    rhinoModel.m_settings.m_ModelUnitsAndTolerances.m_unit_system =
        ON::LengthUnitSystem::Inches;
    ON_Layer rhinoLayer;
    rhinoLayer.SetName(L"Imported Layer");
    rhinoLayer.SetColor(ON_Color(40, 120, 210));
    const ON_ModelComponentReference addedLayer = rhinoModel.AddModelComponent(rhinoLayer, true);
    const ON_ModelComponent *layerComponent = addedLayer.ModelComponent();
    passed &= check(layerComponent != nullptr && layerComponent->Index() >= 0,
                    "plain Rhino test model must contain its custom layer");
    if (layerComponent != nullptr) {
        ON_3dmObjectAttributes curveAttributes;
        curveAttributes.m_layer_index = layerComponent->Index();
        rhinoModel.AddManagedModelGeometryComponent(
            new ON_ArcCurve(ON_Circle(ON_3dPoint(1.0, 2.0, 0.0), 0.5)),
            new ON_3dmObjectAttributes(curveAttributes));

        ON_3dmObjectAttributes pointAttributes;
        pointAttributes.m_layer_index = layerComponent->Index();
        rhinoModel.AddManagedModelGeometryComponent(
            new ON_Point(ON_3dPoint(2.0, 0.0, 0.0)),
            new ON_3dmObjectAttributes(pointAttributes));

        ON_3dmObjectAttributes spatialCurveAttributes;
        spatialCurveAttributes.m_layer_index = layerComponent->Index();
        rhinoModel.AddManagedModelGeometryComponent(
            new ON_LineCurve(ON_3dPoint(0.0, 0.0, 2.0), ON_3dPoint(1.0, 0.0, 2.0)),
            new ON_3dmObjectAttributes(spatialCurveAttributes));
        rhinoModel.AddManagedModelGeometryComponent(
            new ON_LineCurve(ON_3dPoint(0.0, 0.0, 0.0), ON_3dPoint(1.0, 1.0, 2.0)),
            new ON_3dmObjectAttributes(spatialCurveAttributes));
        // Spatial NURBS curves are supported. Use a mesh to exercise the
        // importer's explicit unsupported-geometry report instead.
        auto *unsupportedMesh = new ON_Mesh();
        unsupportedMesh->SetVertex(0, ON_3dPoint(0.0, 0.0, 0.0));
        rhinoModel.AddManagedModelGeometryComponent(
            unsupportedMesh,
            new ON_3dmObjectAttributes(spatialCurveAttributes));
    }

    const QString rhinoPath = QDir(temporaryDirectory.path()).filePath(
        QStringLiteral("ordinary-rhino-model.3dm"));
    const std::wstring wideRhinoPath = rhinoPath.toStdWString();
    ON_wString writeLog;
    ON_TextLog textLog(writeLog);
    passed &= check(rhinoModel.Write(wideRhinoPath.c_str(), 0, &textLog),
                    "test fixture must be a plain Rhino 3DM without Vignola metadata");

    Document importTarget;
    importTarget.append(line);
    Rhino3dmImportReport importReport;
    errorMessage.clear();
    const bool imported = importRhino3dmDocument(rhinoPath,
                                                 &importTarget,
                                                 &importReport,
                                                 &errorMessage);
    if (!imported) {
        qCritical("Could not import ordinary Rhino test model: %s", qPrintable(errorMessage));
    }
    passed &= check(imported,
                    "ordinary Rhino 3DM files must import without Vignola metadata");
    passed &= check(importTarget.size() == 5 && importReport.importedObjectCount == 4,
                    "Rhino import must merge principal and tilted planar curves and points into the document");
    passed &= check(importReport.skippedObjectCount == 1 &&
                        !importReport.warningMessage.isEmpty(),
                    "Rhino import must explicitly report skipped non-planar geometry");

    const DocumentCollection *importedCollection = nullptr;
    for (const DocumentCollection &collection : importTarget.collections()) {
        if (collection.name == QStringLiteral("Imported Layer")) {
            importedCollection = &collection;
            break;
        }
    }
    passed &= check(importedCollection != nullptr,
                    "Rhino import must preserve source layer names as collection names");
    bool importedObjectsUseCollection = false;
    if (importedCollection != nullptr) {
        for (const SceneObject &object : importTarget.objects()) {
            importedObjectsUseCollection = importedObjectsUseCollection ||
                object.collectionIds.contains(importedCollection->id);
        }
    }
    passed &= check(importedObjectsUseCollection,
                    "Rhino import must assign imported objects to their source-layer collection");

    bool foundRationalCurve = false;
    bool foundScaledPoint = false;
    bool foundOffsetPlaneCurve = false;
    bool foundTiltedCurve = false;
    for (const SceneObject &object : importTarget.objects()) {
        if ((object.geometry.geometryType == GeometryType::Nurbs ||
             object.geometry.geometryType == GeometryType::Circle) &&
            validateNurbsCurve(object.geometry.nurbs) && object.geometry.nurbs.rational) {
            foundRationalCurve = true;
            const QPointF controlPoint = object.geometry.nurbs.controlPoints.first();
            passed &= check(std::abs(controlPoint.x() - 38.1) < 1.0e-8 &&
                                std::abs(controlPoint.y() - 50.8) < 1.0e-8,
                            "Rhino rational curve control vertices must be converted from inches to millimeters");
        }
        if (object.geometry.geometryType == GeometryType::Point &&
            !object.geometry.points.isEmpty()) {
            foundScaledPoint = std::abs(object.geometry.points.first().x() - 50.8) < 1.0e-8;
        }
        if (object.geometry.geometryType == GeometryType::Line &&
            object.geometry.workPlane == WorkPlane::XY) {
            foundOffsetPlaneCurve |= std::abs(object.geometry.workPlaneOffset - 50.8) < 1.0e-8;
        }
        if (object.geometry.geometryType == GeometryType::Line &&
            validateNurbsCurve(object.geometry.nurbs)) {
            const Point3D end = workPlaneFramePointToWorld(
                object.geometry.nurbs.controlPoints.last(), shapeWorkPlaneFrame(object.geometry));
            foundTiltedCurve |= std::abs(end.x - 25.4) < 1.0e-8 &&
                std::abs(end.y - 25.4) < 1.0e-8 && std::abs(end.z - 50.8) < 1.0e-8;
        }
    }
    passed &= check(foundRationalCurve,
                    "Rhino arcs must import as valid rational NURBS curves");
    passed &= check(foundScaledPoint,
                    "Rhino points must be converted from source units to millimeters");
    passed &= check(foundOffsetPlaneCurve,
                    "Rhino import must preserve principal-plane offsets and convert them to millimeters");
    passed &= check(foundTiltedCurve,
                    "Rhino import must preserve the world endpoints of a tilted planar line");

    return passed ? 0 : 1;
}
