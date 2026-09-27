#include "vignola_document_file.h"

#include "core/document/document.h"
#include "core/serialization/document_serializer.h"

#include <opennurbs.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
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

} // namespace classiCAD
