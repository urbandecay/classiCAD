#include "core/document/document.h"
#include "core/serialization/document_serializer.h"
#include "core/serialization/vignola_document_file.h"

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

    ONX_Model nativeModel;
    ON_wString readLog;
    ON_TextLog textLog(readLog);
    const std::wstring nativePath = path.toStdWString();
    const bool nativeReadSucceeded = nativeModel.Read(nativePath.c_str(), &textLog);
    passed &= check(nativeReadSucceeded,
                    "a Vignola project must remain readable as a native openNURBS 3DM archive");
    if (nativeReadSucceeded) {
        passed &= check(nativeModel.ActiveComponentCount(
                            ON_ModelComponent::Type::ModelGeometry) >= 3,
                        "the 3DM archive must contain native curve and dimension geometry");
        ON_wString documentData;
        passed &= check(nativeModel.GetDocumentUserString(L"Vignola.DocumentData", documentData),
                        "the 3DM archive must carry the Vignola document metadata");
    }

    return passed ? 0 : 1;
}
