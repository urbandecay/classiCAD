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
    const LayerId drawingLayer = source.createLayer(QStringLiteral("Drawing"));
    passed &= check(drawingLayer.isValid() && source.setActiveLayer(drawingLayer),
                    "test document must be able to create and activate a layer");
    source.setLayerColor(drawingLayer, QColor(QStringLiteral("#629fd1")));
    source.setLayerLineType(drawingLayer, QStringLiteral("DASHED"));
    source.setLayerLineWeight(drawingLayer, 0.35);
    source.setLayerDescription(drawingLayer, QStringLiteral("round-trip test layer"));

    Shape circle;
    circle.geometryType = GeometryType::Circle;
    circle.points = {QPointF(12.0, -4.0), QPointF(22.0, -4.0)};
    circle.nurbs = makeCircleNurbs(circle.points);
    const ObjectId circleId = source.append(circle);

    Shape line;
    line.geometryType = GeometryType::Line;
    line.points = {QPointF(-3.0, 1.5), QPointF(8.0, 6.0)};
    line.nurbs = makeDegreeOneNurbs(line.points);
    line.workPlane = WorkPlane::XZ;
    line.workPlaneOffset = 2.5;
    source.append(line);

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
    source.append(dimension);

    Shape picture;
    picture.geometryType = GeometryType::Picture;
    picture.points = {QPointF(-12.0, 5.0),
                      QPointF(-8.0, 5.0),
                      QPointF(-8.0, 3.0),
                      QPointF(-12.0, 3.0)};
    picture.pictureImage = QImage(4, 2, QImage::Format_ARGB32);
    picture.pictureImage.fill(QColor(QStringLiteral("#4c86b8")));
    source.append(picture);

    Shape solid;
    solid.geometryType = GeometryType::NurbsSolid;
    NurbsSurface3D cap;
    passed &= check(makeNurbsPlanarFillSurface(circle.nurbs, shapeWorkPlaneFrame(circle), &cap) &&
                        makeNurbsExtrusionSolid(cap, {0,0,7}, &solid.nurbsSolid),
                    "solid save fixture must have exact caps and walls");
    source.append(solid);

    QTemporaryDir temporaryDirectory;
    passed &= check(temporaryDirectory.isValid(), "temporary directory must be available");
    if (!temporaryDirectory.isValid()) {
        return 1;
    }
    const QString path = QDir(temporaryDirectory.path()).filePath(
        QStringLiteral("round-trip.vignola"));

    QString errorMessage;
    if (!saveVignolaDocument(path, source, &errorMessage)) {
        qCritical("Could not save Vignola test document: %s", qPrintable(errorMessage));
        return 1;
    }

    Document restored;
    if (!loadVignolaDocument(path, &restored, &errorMessage)) {
        qCritical("Could not load Vignola test document: %s", qPrintable(errorMessage));
        return 1;
    }
    passed &= check(equivalentJson(documentToJson(source), documentToJson(restored)),
                    "Vignola save/load must preserve document geometry, IDs, layers, and dimension anchors");

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
        // A tilted straight line is still planar. Four noncoplanar CVs are
        // needed to exercise rejection of a genuinely spatial NURBS curve.
        auto *nonplanar = new ON_NurbsCurve(3, false, 4, 4);
        nonplanar->SetCV(0, ON_3dPoint(0.0, 0.0, 0.0));
        nonplanar->SetCV(1, ON_3dPoint(1.0, 0.0, 0.0));
        nonplanar->SetCV(2, ON_3dPoint(0.0, 1.0, 0.0));
        nonplanar->SetCV(3, ON_3dPoint(0.0, 0.0, 1.0));
        nonplanar->MakeClampedUniformKnotVector();
        rhinoModel.AddManagedModelGeometryComponent(
            nonplanar, new ON_3dmObjectAttributes(spatialCurveAttributes));
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

    const Layer *importedLayer = nullptr;
    for (const Layer &layer : importTarget.layers()) {
        if (layer.name == QStringLiteral("Imported Layer")) {
            importedLayer = &layer;
            break;
        }
    }
    passed &= check(importedLayer != nullptr && importedLayer->color == QColor(40, 120, 210),
                    "Rhino import must preserve source layer names and colors");

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
