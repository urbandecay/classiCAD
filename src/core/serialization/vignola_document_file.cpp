#include "vignola_document_file.h"

#include "core/document/document.h"
#include "core/serialization/document_serializer.h"

#include <opennurbs.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStringList>
#include <QTemporaryFile>

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>

namespace classiCAD {
namespace {

constexpr wchar_t kDocumentDataKey[] = L"Vignola.DocumentData";
constexpr wchar_t kDocumentFormatKey[] = L"Vignola.DocumentFormat";
constexpr wchar_t kDocumentFormatVersion[] = L"1";
constexpr wchar_t kObjectIdKey[] = L"Vignola.ObjectId";
constexpr wchar_t kComponentIndexKey[] = L"Vignola.ComponentIndex";
constexpr wchar_t kLayerIdKey[] = L"Vignola.LayerId";

class OpenNurbsRuntime final {
public:
    OpenNurbsRuntime()
    {
        ON::Begin();
    }

    ~OpenNurbsRuntime()
    {
        ON::End();
    }

    OpenNurbsRuntime(const OpenNurbsRuntime &) = delete;
    OpenNurbsRuntime &operator=(const OpenNurbsRuntime &) = delete;
};

OpenNurbsRuntime &openNurbsRuntime()
{
    static OpenNurbsRuntime runtime;
    return runtime;
}

void setError(QString *errorMessage, const QString &message)
{
    if (errorMessage != nullptr) {
        *errorMessage = message;
    }
}

std::wstring toWide(const QString &value)
{
    return value.toStdWString();
}

QString openNurbsError(const ON_wString &log, const QString &fallback)
{
    const wchar_t *text = log.Array();
    if (text == nullptr || text[0] == L'\0') {
        return fallback;
    }
    return QString::fromStdWString(text).trimmed();
}

bool makeNurbsCurve(const Shape::NurbsCurve2D &source,
                    std::unique_ptr<ON_NurbsCurve> *curve,
                    QString *errorMessage)
{
    if (curve == nullptr || !validateNurbsCurve(source, errorMessage)) {
        return false;
    }

    auto result = std::make_unique<ON_NurbsCurve>(3,
                                                  source.rational,
                                                  source.order,
                                                  source.controlPoints.size());
    for (int index = 0; index < source.controlPoints.size(); ++index) {
        const QPointF point = source.controlPoints[index];
        if (source.rational) {
            const double weight = source.weights[index];
            const ON_4dPoint homogeneous(point.x() * weight,
                                         point.y() * weight,
                                         0.0,
                                         weight);
            if (!result->SetCV(index, homogeneous)) {
                setError(errorMessage, QStringLiteral("Could not write a NURBS control vertex"));
                return false;
            }
        } else if (!result->SetCV(index, ON_3dPoint(point.x(), point.y(), 0.0))) {
            setError(errorMessage, QStringLiteral("Could not write a NURBS control vertex"));
            return false;
        }
    }

    for (int index = 0; index < source.knots.size(); ++index) {
        if (!result->SetKnot(index, source.knots[index])) {
            setError(errorMessage, QStringLiteral("Could not write a NURBS knot"));
            return false;
        }
    }

    if (!result->IsValid()) {
        setError(errorMessage, QStringLiteral("The curve is not a valid openNURBS curve"));
        return false;
    }

    *curve = std::move(result);
    return true;
}

bool samePoint(const QPointF &first, const QPointF &second)
{
    return first.x() == second.x() && first.y() == second.y();
}

bool makeObjectGeometry(const Shape &shape,
                        const ON_UUID &dimensionStyleId,
                        std::vector<std::unique_ptr<ON_Object>> *geometry,
                        QString *errorMessage)
{
    if (geometry == nullptr) {
        setError(errorMessage, QStringLiteral("No geometry output was provided"));
        return false;
    }

    if (shape.geometryType == GeometryType::Point) {
        if (shape.points.isEmpty()) {
            return true;
        }
        const QPointF point = shape.points.first();
        geometry->push_back(std::make_unique<ON_Point>(ON_3dPoint(point.x(), point.y(), 0.0)));
        return true;
    }

    if (shape.geometryType == GeometryType::LinearDimension) {
        if (shape.points.size() < 3) {
            setError(errorMessage, QStringLiteral("A linear dimension needs three points"));
            return false;
        }
        auto dimension = std::make_unique<ON_DimLinear>();
        const bool created = dimension->Create(
            ON::AnnotationType::Aligned,
            dimensionStyleId,
            ON_Plane::World_xy,
            ON_3dVector::XAxis,
            ON_3dPoint(shape.points[0].x(), shape.points[0].y(), 0.0),
            ON_3dPoint(shape.points[1].x(), shape.points[1].y(), 0.0),
            ON_3dPoint(shape.points[2].x(), shape.points[2].y(), 0.0));
        if (!created) {
            setError(errorMessage, QStringLiteral("Could not create the linear dimension"));
            return false;
        }
        geometry->push_back(std::move(dimension));
        return true;
    }

    if (shape.geometryType == GeometryType::AngularDimension) {
        if (shape.points.size() < 3) {
            setError(errorMessage, QStringLiteral("An angular dimension needs three points"));
            return false;
        }
        const QPointF vertex = shape.points[0];
        const QPointF firstRay = shape.points[1] - vertex;
        const QPointF secondRay = shape.points[2] - vertex;
        const double firstLength = std::hypot(firstRay.x(), firstRay.y());
        const double secondLength = std::hypot(secondRay.x(), secondRay.y());
        if (firstLength <= 1.0e-12 || secondLength <= 1.0e-12) {
            setError(errorMessage, QStringLiteral("The angular dimension has a zero-length ray"));
            return false;
        }
        const double sweep = std::atan2(firstRay.x() * secondRay.y() -
                                            firstRay.y() * secondRay.x(),
                                        QPointF::dotProduct(firstRay, secondRay));
        const double firstAngle = std::atan2(firstRay.y(), firstRay.x());
        const double middleAngle = firstAngle + sweep * 0.5;
        const double radius = std::min(firstLength, secondLength);
        const ON_3dPoint arcPoint(vertex.x() + std::cos(middleAngle) * radius,
                                 vertex.y() + std::sin(middleAngle) * radius,
                                 0.0);

        auto dimension = std::make_unique<ON_DimAngular>();
        if (!dimension->Create(dimensionStyleId,
                               ON_Plane::World_xy,
                               ON_3dVector::XAxis,
                               ON_3dPoint(vertex.x(), vertex.y(), 0.0),
                               ON_3dPoint(shape.points[1].x(), shape.points[1].y(), 0.0),
                               ON_3dPoint(shape.points[2].x(), shape.points[2].y(), 0.0),
                               arcPoint)) {
            setError(errorMessage, QStringLiteral("Could not create the angular dimension"));
            return false;
        }
        geometry->push_back(std::move(dimension));
        return true;
    }

    if (!shape.components.isEmpty()) {
        for (const Shape::NurbsCurve2D &component : shape.components) {
            std::unique_ptr<ON_NurbsCurve> curve;
            if (!makeNurbsCurve(component, &curve, errorMessage)) {
                return false;
            }
            geometry->push_back(std::move(curve));
        }
        return true;
    }

    if (!shape.nurbs.controlPoints.isEmpty()) {
        std::unique_ptr<ON_NurbsCurve> curve;
        if (!makeNurbsCurve(shape.nurbs, &curve, errorMessage)) {
            return false;
        }
        geometry->push_back(std::move(curve));
        return true;
    }

    QVector<QPointF> points = shape.points;
    if ((shape.geometryType == GeometryType::Rectangle ||
         shape.geometryType == GeometryType::Polygon) &&
        points.size() >= 3 && !samePoint(points.first(), points.last())) {
        points.append(points.first());
    }
    if (points.size() >= 2) {
        const Shape::NurbsCurve2D polyline = makeDegreeOneNurbs(points);
        std::unique_ptr<ON_NurbsCurve> curve;
        if (!makeNurbsCurve(polyline, &curve, errorMessage)) {
            return false;
        }
        geometry->push_back(std::move(curve));
    }
    return true;
}

bool addGeometry(ONX_Model &model,
                 const SceneObject &sceneObject,
                 int layerIndex,
                 const ON_UUID &dimensionStyleId,
                 QString *errorMessage)
{
    std::vector<std::unique_ptr<ON_Object>> geometry;
    if (!makeObjectGeometry(sceneObject.geometry,
                            dimensionStyleId,
                            &geometry,
                            errorMessage)) {
        return false;
    }

    for (int componentIndex = 0;
         componentIndex < static_cast<int>(geometry.size());
         ++componentIndex) {
        auto attributes = std::make_unique<ON_3dmObjectAttributes>();
        attributes->m_layer_index = layerIndex;
        const std::wstring objectId =
            std::to_wstring(sceneObject.id.value());
        const std::wstring componentNumber = std::to_wstring(componentIndex);
        if (!attributes->SetUserString(kObjectIdKey, objectId.c_str()) ||
            !attributes->SetUserString(kComponentIndexKey, componentNumber.c_str())) {
            setError(errorMessage, QStringLiteral("Could not attach Vignola object metadata"));
            return false;
        }

        const auto component = model.AddManagedModelGeometryComponent(
            geometry[componentIndex].release(), attributes.release());
        if (component.IsEmpty()) {
            setError(errorMessage, QStringLiteral("openNURBS rejected a scene object"));
            return false;
        }
    }
    return true;
}

bool writeModelAtomically(const QString &path,
                          const ONX_Model &model,
                          QString *errorMessage)
{
    const QFileInfo destinationInfo(path);
    QTemporaryFile stagedFile(destinationInfo.dir().filePath(
        QStringLiteral(".vignola-save-XXXXXX")));
    stagedFile.setAutoRemove(false);
    if (!stagedFile.open()) {
        setError(errorMessage,
                 QStringLiteral("Could not create a temporary save file: %1")
                     .arg(stagedFile.errorString()));
        return false;
    }
    const QString stagedPath = stagedFile.fileName();
    stagedFile.close();

    ON_wString writeLog;
    ON_TextLog textLog(writeLog);
    const std::wstring wideStagedPath = toWide(stagedPath);
    if (!model.Write(wideStagedPath.c_str(), 0, &textLog)) {
        QFile::remove(stagedPath);
        setError(errorMessage,
                 openNurbsError(writeLog, QStringLiteral("openNURBS could not write the file")));
        return false;
    }

    QFile stagedInput(stagedPath);
    if (!stagedInput.open(QIODevice::ReadOnly)) {
        QFile::remove(stagedPath);
        setError(errorMessage,
                 QStringLiteral("Could not read the completed temporary file: %1")
                     .arg(stagedInput.errorString()));
        return false;
    }

    QSaveFile destination(path);
    destination.setDirectWriteFallback(false);
    if (!destination.open(QIODevice::WriteOnly)) {
        QFile::remove(stagedPath);
        setError(errorMessage,
                 QStringLiteral("Could not open the project file for saving: %1")
                     .arg(destination.errorString()));
        return false;
    }

    constexpr qint64 chunkSize = 1024 * 1024;
    while (!stagedInput.atEnd()) {
        const QByteArray chunk = stagedInput.read(chunkSize);
        if (chunk.isEmpty() && stagedInput.error() != QFileDevice::NoError) {
            destination.cancelWriting();
            QFile::remove(stagedPath);
            setError(errorMessage,
                     QStringLiteral("Could not read the staged project file: %1")
                         .arg(stagedInput.errorString()));
            return false;
        }
        if (!chunk.isEmpty() && destination.write(chunk) != chunk.size()) {
            destination.cancelWriting();
            QFile::remove(stagedPath);
            setError(errorMessage,
                     QStringLiteral("Could not write the project file: %1")
                         .arg(destination.errorString()));
            return false;
        }
    }

    if (!destination.commit()) {
        QFile::remove(stagedPath);
        setError(errorMessage,
                 QStringLiteral("Could not finish saving the project file: %1")
                     .arg(destination.errorString()));
        return false;
    }

    QFile::remove(stagedPath);
    return true;
}

QString layerNameForImport(const ON_Layer &layer)
{
    const ON_wString name = layer.Name();
    const wchar_t *text = name.Array();
    const QString value = text == nullptr ? QString{} : QString::fromWCharArray(text);
    return value.trimmed().isEmpty() ? QStringLiteral("Imported") : value.trimmed();
}

LayerId matchingLayer(const Document &document, const QString &name)
{
    for (const Layer &layer : document.layers()) {
        if (layer.name.compare(name, Qt::CaseInsensitive) == 0) {
            return layer.id;
        }
    }
    return LayerId::invalid();
}

bool importNurbsCurve(const ON_Curve &source,
                      double unitScale,
                      double planarTolerance,
                      Shape::NurbsCurve2D *destination,
                      QString *reason)
{
    ON_NurbsCurve converted;
    const ON_NurbsCurve *nurbs = ON_NurbsCurve::Cast(&source);
    if (nurbs != nullptr) {
        converted = *nurbs;
    } else if (source.GetNurbForm(converted, planarTolerance) <= 0) {
        if (reason != nullptr) {
            *reason = QStringLiteral("curve has no NURBS representation");
        }
        return false;
    }

    if (!converted.IsValid() || converted.CVCount() < 2 || converted.Dimension() < 2 ||
        converted.Dimension() > 3 || converted.Order() < 2) {
        if (reason != nullptr) {
            *reason = QStringLiteral("curve has invalid or unsupported NURBS data");
        }
        return false;
    }

    Shape::NurbsCurve2D result;
    result.dimension = 2;
    result.order = converted.Order();
    result.degree = result.order - 1;
    result.rational = converted.IsRational();
    result.controlPoints.reserve(converted.CVCount());
    result.weights.reserve(converted.CVCount());
    for (int index = 0; index < converted.CVCount(); ++index) {
        ON_3dPoint point;
        if (!converted.GetCV(index, point) || !std::isfinite(point.x) ||
            !std::isfinite(point.y) || !std::isfinite(point.z) ||
            std::abs(point.z) > planarTolerance) {
            if (reason != nullptr) {
                *reason = QStringLiteral("curve is not in the XY plane");
            }
            return false;
        }

        const double weight = result.rational ? converted.Weight(index) : 1.0;
        if (!std::isfinite(weight) || weight <= 0.0) {
            if (reason != nullptr) {
                *reason = QStringLiteral("curve has a non-positive rational weight");
            }
            return false;
        }
        result.controlPoints.append(QPointF(point.x * unitScale, point.y * unitScale));
        result.weights.append(weight);
    }

    result.knots.reserve(converted.KnotCount());
    for (int index = 0; index < converted.KnotCount(); ++index) {
        result.knots.append(converted.Knot(index));
    }

    QString validationError;
    if (!validateNurbsCurve(result, &validationError)) {
        if (reason != nullptr) {
            *reason = QStringLiteral("curve NURBS data is invalid: %1").arg(validationError);
        }
        return false;
    }

    *destination = std::move(result);
    return true;
}

bool importCurve(const ON_Curve &curve,
                 double unitScale,
                 double planarTolerance,
                 Shape *shape,
                 QString *reason)
{
    if (shape == nullptr) {
        return false;
    }

    if (const ON_PolyCurve *polyCurve = ON_PolyCurve::Cast(&curve)) {
        Shape result;
        result.geometryType = GeometryType::PolyCurve;
        result.components.reserve(polyCurve->Count());
        for (int index = 0; index < polyCurve->Count(); ++index) {
            const ON_Curve *segment = polyCurve->SegmentCurve(index);
            Shape::NurbsCurve2D converted;
            if (segment == nullptr ||
                !importNurbsCurve(*segment,
                                  unitScale,
                                  planarTolerance,
                                  &converted,
                                  reason)) {
                return false;
            }
            if (result.points.isEmpty()) {
                result.points.append(converted.controlPoints.first());
            }
            result.points.append(converted.controlPoints.last());
            result.components.append(std::move(converted));
        }
        if (result.components.isEmpty()) {
            if (reason != nullptr) {
                *reason = QStringLiteral("polycurve has no segments");
            }
            return false;
        }
        *shape = std::move(result);
        return true;
    }

    Shape result;
    if (!importNurbsCurve(curve, unitScale, planarTolerance, &result.nurbs, reason)) {
        return false;
    }

    if (const ON_ArcCurve *arcCurve = ON_ArcCurve::Cast(&curve);
        arcCurve != nullptr && arcCurve->IsCircle()) {
        const ON_3dPoint center = arcCurve->m_arc.Center();
        const ON_3dPoint edge = arcCurve->m_arc.PointAt(0.0);
        result.geometryType = GeometryType::Circle;
        result.points = {QPointF(center.x * unitScale, center.y * unitScale),
                         QPointF(edge.x * unitScale, edge.y * unitScale)};
        *shape = std::move(result);
        return true;
    }

    result.geometryType = curve.IsLinear(planarTolerance)
                              ? GeometryType::Line
                              : GeometryType::Nurbs;
    result.points = result.nurbs.controlPoints;
    *shape = std::move(result);
    return true;
}

void addImportWarning(QStringList *warnings, const QString &reason)
{
    if (warnings != nullptr && !reason.isEmpty() && !warnings->contains(reason) &&
        warnings->size() < 5) {
        warnings->append(reason);
    }
}

} // namespace

bool saveVignolaDocument(const QString &path,
                         const Document &document,
                         QString *errorMessage)
{
    Q_UNUSED(openNurbsRuntime())
    if (path.trimmed().isEmpty()) {
        setError(errorMessage, QStringLiteral("A project file path is required"));
        return false;
    }

    ONX_Model model;
    model.m_sStartSectionComments = "Vignola project file";
    model.m_properties.m_Application.m_application_name = L"Vignola";
    model.m_properties.m_Application.m_application_details =
        L"Created by Vignola using the openNURBS toolkit";
    model.m_settings.m_ModelUnitsAndTolerances.m_unit_system =
        ON::LengthUnitSystem::Millimeters;

    const QJsonDocument documentJson(documentToJson(document));
    const QString documentText = QString::fromUtf8(
        documentJson.toJson(QJsonDocument::Compact));
    const std::wstring wideDocumentText = toWide(documentText);
    if (!model.SetDocumentUserString(kDocumentFormatKey, kDocumentFormatVersion) ||
        !model.SetDocumentUserString(kDocumentDataKey, wideDocumentText.c_str())) {
        setError(errorMessage, QStringLiteral("Could not store Vignola document metadata"));
        return false;
    }

    QHash<quint64, int> layerIndices;
    QHash<quint64, ON_UUID> layerIds;
    for (const Layer &layer : document.layers()) {
        ON_Layer openNurbsLayer;
        const std::wstring layerName = toWide(layer.name);
        if (!openNurbsLayer.SetName(layerName.c_str())) {
            setError(errorMessage, QStringLiteral("A layer name is not valid for 3DM"));
            return false;
        }
        openNurbsLayer.SetColor(ON_Color(layer.color.red(),
                                         layer.color.green(),
                                         layer.color.blue(),
                                         255 - layer.color.alpha()));
        openNurbsLayer.SetVisible(layer.visible);
        openNurbsLayer.SetLocked(layer.locked || layer.frozen);
        openNurbsLayer.SetPlotWeight(layer.lineWeightMm);
        const std::wstring layerId = std::to_wstring(layer.id.value());
        openNurbsLayer.SetUserString(kLayerIdKey, layerId.c_str());

        const ON_ModelComponentReference addedLayer = model.AddModelComponent(openNurbsLayer,
                                                                               true);
        const ON_ModelComponent *layerComponent = addedLayer.ModelComponent();
        if (layerComponent == nullptr || layerComponent->Index() < 0) {
            setError(errorMessage, QStringLiteral("openNURBS rejected a document layer"));
            return false;
        }
        layerIndices.insert(layer.id.value(), layerComponent->Index());
        layerIds.insert(layer.id.value(), layerComponent->Id());
    }

    const int dimensionStyleIndex = model.AddDefaultDimensionStyle(
        L"Vignola Dimensions",
        ON::LengthUnitSystem::Millimeters,
        0.01);
    if (dimensionStyleIndex < 0) {
        setError(errorMessage, QStringLiteral("Could not create the 3DM dimension style"));
        return false;
    }
    const ON_UUID dimensionStyleId = model.m_settings.CurrentDimensionStyleId();

    const Layer *activeLayer = document.layer(document.activeLayerId());
    if (activeLayer != nullptr) {
        const auto activeLayerId = layerIds.constFind(activeLayer->id.value());
        if (activeLayerId != layerIds.cend()) {
            model.m_settings.SetCurrentLayerId(*activeLayerId);
        }
    }

    for (const SceneObject &sceneObject : document.objects()) {
        const auto layerIndex = layerIndices.constFind(sceneObject.layerId.value());
        if (layerIndex == layerIndices.cend()) {
            setError(errorMessage, QStringLiteral("A scene object refers to a missing layer"));
            return false;
        }
        if (!addGeometry(model,
                         sceneObject,
                         *layerIndex,
                         dimensionStyleId,
                         errorMessage)) {
            return false;
        }
    }

    return writeModelAtomically(path, model, errorMessage);
}

bool loadVignolaDocument(const QString &path,
                         Document *document,
                         QString *errorMessage)
{
    Q_UNUSED(openNurbsRuntime())
    if (document == nullptr || path.trimmed().isEmpty()) {
        setError(errorMessage, QStringLiteral("A destination document and file path are required"));
        return false;
    }

    const std::wstring widePath = toWide(path);
    ON_wString readLog;
    ON_TextLog textLog(readLog);
    ONX_Model model;
    if (!model.Read(widePath.c_str(), &textLog)) {
        setError(errorMessage,
                 openNurbsError(readLog,
                                QStringLiteral("The file is not a readable 3DM project")));
        return false;
    }

    ON_wString formatVersion;
    if (!model.GetDocumentUserString(kDocumentFormatKey, formatVersion) ||
        formatVersion != kDocumentFormatVersion) {
        setError(errorMessage,
                 QStringLiteral("This is not a supported Vignola project file"));
        return false;
    }

    ON_wString documentText;
    if (!model.GetDocumentUserString(kDocumentDataKey, documentText)) {
        setError(errorMessage, QStringLiteral("The Vignola document data is missing"));
        return false;
    }

    const QByteArray jsonBytes = QString::fromStdWString(documentText.Array()).toUtf8();
    QJsonParseError parseError;
    const QJsonDocument jsonDocument = QJsonDocument::fromJson(jsonBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !jsonDocument.isObject()) {
        setError(errorMessage,
                 QStringLiteral("The Vignola document data is invalid: %1")
                     .arg(parseError.errorString()));
        return false;
    }

    Document restored;
    QString documentError;
    if (!documentFromJson(jsonDocument.object(), &restored, &documentError)) {
        setError(errorMessage,
                 QStringLiteral("The Vignola document could not be restored: %1")
                     .arg(documentError));
        return false;
    }

    *document = restored;
    return true;
}

bool importRhino3dmDocument(const QString &path,
                            Document *document,
                            Rhino3dmImportReport *report,
                            QString *errorMessage)
{
    Q_UNUSED(openNurbsRuntime())
    if (report != nullptr) {
        *report = Rhino3dmImportReport{};
    }
    if (document == nullptr || path.trimmed().isEmpty()) {
        setError(errorMessage, QStringLiteral("A destination document and 3DM file path are required"));
        return false;
    }

    const std::wstring widePath = toWide(path);
    ON_wString readLog;
    ON_TextLog textLog(readLog);
    ONX_Model model;
    if (!model.Read(widePath.c_str(), &textLog)) {
        setError(errorMessage,
                 openNurbsError(readLog,
                                QStringLiteral("openNURBS could not read this Rhino 3DM file")));
        return false;
    }

    const double unitScale = ON::UnitScale(
        model.m_settings.m_ModelUnitsAndTolerances.m_unit_system,
        ON::LengthUnitSystem::Millimeters);
    if (!std::isfinite(unitScale) || unitScale <= 0.0) {
        setError(errorMessage,
                 QStringLiteral("The Rhino file uses an unknown model unit system"));
        return false;
    }
    const double planarTolerance = std::max(
        1.0e-9,
        std::abs(model.m_settings.m_ModelUnitsAndTolerances.m_absolute_tolerance));

    Document candidate = *document;
    QHash<int, LayerId> importedLayerIds;
    ONX_ModelComponentIterator layerIterator(model, ON_ModelComponent::Type::Layer);
    for (ON_ModelComponentReference reference = layerIterator.FirstComponentReference();
         !reference.IsEmpty();
         reference = layerIterator.NextComponentReference()) {
        const ON_Layer *sourceLayer = ON_Layer::FromModelComponentRef(reference, nullptr);
        if (sourceLayer == nullptr) {
            continue;
        }

        const QString layerName = layerNameForImport(*sourceLayer);
        LayerId destinationLayerId = matchingLayer(candidate, layerName);
        if (!destinationLayerId.isValid()) {
            destinationLayerId = candidate.createLayer(layerName);
            const ON_Color sourceColor = sourceLayer->Color();
            candidate.setLayerColor(destinationLayerId,
                                    QColor(sourceColor.Red(),
                                           sourceColor.Green(),
                                           sourceColor.Blue(),
                                           255 - sourceColor.Alpha()));
            candidate.setLayerVisible(destinationLayerId, sourceLayer->IsVisible());
            candidate.setLayerLocked(destinationLayerId, sourceLayer->IsLocked());

            const double plotWeight = sourceLayer->PlotWeight();
            candidate.setLayerPlotted(destinationLayerId, plotWeight >= 0.0);
            candidate.setLayerLineWeight(destinationLayerId,
                                         std::clamp(plotWeight, 0.0, 2.11));

            const int linePatternIndex = sourceLayer->LinetypeIndex();
            if (linePatternIndex >= 0) {
                const ON_ModelComponentReference pattern = model.ComponentFromIndex(
                    ON_ModelComponent::Type::LinePattern,
                    linePatternIndex);
                if (const ON_ModelComponent *patternComponent = pattern.ModelComponent()) {
                    const ON_wString patternName = patternComponent->Name();
                    if (patternName.Array() != nullptr && patternName.Array()[0] != L'\0') {
                        candidate.setLayerLineType(destinationLayerId,
                                                   QString::fromWCharArray(patternName.Array()));
                    }
                }
            }
        }
        importedLayerIds.insert(sourceLayer->Index(), destinationLayerId);
    }

    QStringList warnings;
    Rhino3dmImportReport completedReport;
    ONX_ModelComponentIterator geometryIterator(model,
                                                 ON_ModelComponent::Type::ModelGeometry);
    for (ON_ModelComponentReference reference = geometryIterator.FirstComponentReference();
         !reference.IsEmpty();
         reference = geometryIterator.NextComponentReference()) {
        const ON_ModelGeometryComponent *modelGeometry =
            ON_ModelGeometryComponent::FromModelComponentRef(reference, nullptr);
        if (modelGeometry == nullptr) {
            continue;
        }
        if (modelGeometry->IsInstanceDefinitionGeometry()) {
            continue;
        }

        const ON_Geometry *geometry = modelGeometry->Geometry(nullptr);
        const ON_3dmObjectAttributes *attributes = modelGeometry->Attributes(nullptr);
        if (geometry == nullptr) {
            ++completedReport.skippedObjectCount;
            addImportWarning(&warnings, QStringLiteral("empty Rhino object"));
            continue;
        }

        Shape importedShape;
        QString reason;
        if (const ON_Point *point = ON_Point::Cast(geometry)) {
            if (!std::isfinite(point->point.x) || !std::isfinite(point->point.y) ||
                !std::isfinite(point->point.z) ||
                std::abs(point->point.z) > planarTolerance) {
                reason = QStringLiteral("point is not in the XY plane");
            } else {
                importedShape.geometryType = GeometryType::Point;
                importedShape.points.append(QPointF(point->point.x * unitScale,
                                                    point->point.y * unitScale));
            }
        } else if (const ON_Curve *curve = ON_Curve::Cast(geometry)) {
            if (!importCurve(*curve,
                             unitScale,
                             planarTolerance,
                             &importedShape,
                             &reason)) {
                // The helper provides a specific reason for unsupported curves.
            }
        } else if (ON_InstanceRef::Cast(geometry) != nullptr) {
            reason = QStringLiteral("block instances are not supported yet");
        } else {
            reason = QStringLiteral("surfaces, meshes, and annotations are not supported yet");
        }

        if (!reason.isEmpty()) {
            ++completedReport.skippedObjectCount;
            addImportWarning(&warnings, reason);
            continue;
        }

        SceneObject importedObject;
        importedObject.layerId = attributes == nullptr
                                     ? candidate.activeLayerId()
                                     : importedLayerIds.value(attributes->m_layer_index,
                                                              candidate.activeLayerId());
        importedObject.geometry = std::move(importedShape);
        candidate.insertObject(candidate.size(), std::move(importedObject));
        ++completedReport.importedObjectCount;
    }

    if (completedReport.importedObjectCount == 0) {
        setError(errorMessage,
                 completedReport.skippedObjectCount == 0
                     ? QStringLiteral("The Rhino 3DM file contains no importable geometry")
                     : QStringLiteral("No supported 2D curves or points were found in the Rhino 3DM file"));
        if (report != nullptr) {
            *report = completedReport;
        }
        return false;
    }

    if (completedReport.skippedObjectCount > 0) {
        completedReport.warningMessage =
            QStringLiteral("Skipped %1 unsupported object(s): %2")
                .arg(completedReport.skippedObjectCount)
                .arg(warnings.join(QStringLiteral("; ")));
    }

    document->restoreSnapshot(candidate.snapshot());
    if (report != nullptr) {
        *report = completedReport;
    }
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    return true;
}

} // namespace classiCAD
