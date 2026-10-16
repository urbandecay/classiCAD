#include "shape_json_codec.h"

#include "core/geometry/arc_mode.h"
#include "core/geometry/geometry_type.h"
#include "core/geometry/nurbs_curve.h"
#include "core/geometry/nurbs_surface.h"
#include "core/geometry/polygon_mesh.h"
#include "core/geometry/shape_mapping.h"
#include "core/geometry/work_plane.h"

#include <QBuffer>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonObject>
#include <QIODevice>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace classiCAD {

namespace {

bool decodePictureFloatPayload(const QString &encoded,
                               int width,
                               int height,
                               QByteArray *decoded)
{
    if (decoded == nullptr || width <= 0 || height <= 0 ||
        width > 16384 || height > 16384 ||
        quint64(width) * quint64(height) * 16ULL > 32ULL * 1024 * 1024 ||
        encoded.size() > 44 * 1024 * 1024) {
        return false;
    }
    const QByteArray bytes = QByteArray::fromBase64(
        encoded.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (quint64(bytes.size()) != quint64(width) * quint64(height) * 16ULL)
        return false;
    const auto *source = reinterpret_cast<const uchar *>(bytes.constData());
    for (qsizetype offset = 0; offset < bytes.size(); offset += 4) {
        const quint32 bits = qFromLittleEndian<quint32>(source + offset);
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        if (!std::isfinite(value)) return false;
    }
    *decoded = bytes;
    return true;
}

QJsonArray point3DToJson(const Point3D &point)
{
    QJsonArray value;
    value.append(point.x);
    value.append(point.y);
    value.append(point.z);
    return value;
}

bool point3DFromJson(const QJsonValue &value, Point3D *point)
{
    if (point == nullptr || !value.isArray() || value.toArray().size() != 3) {
        return false;
    }
    const QJsonArray coordinates = value.toArray();
    for (const QJsonValue &coordinate : coordinates) {
        if (!coordinate.isDouble() || !std::isfinite(coordinate.toDouble())) {
            return false;
        }
    }
    *point = {coordinates[0].toDouble(),
              coordinates[1].toDouble(),
              coordinates[2].toDouble()};
    return true;
}

QJsonArray scalarVolumeMatrixToJson(const ScalarVolumeMatrix &matrix)
{
    QJsonArray rows;
    for (int row = 0; row < 4; ++row) {
        QJsonArray columns;
        for (int column = 0; column < 4; ++column)
            columns.append(matrix[static_cast<size_t>(row * 4 + column)]);
        rows.append(columns);
    }
    return rows;
}

QJsonObject pointCloudToJson(const PointCloud3D &cloud)
{
    QJsonObject object;
    QJsonArray positions;
    QJsonArray radii;
    QJsonArray colors;
    QJsonArray sourceIndices;
    for (qsizetype index = 0; index < cloud.positions.size(); ++index) {
        positions.append(point3DToJson(cloud.positions[index]));
        radii.append(cloud.radii[index]);
        if (!cloud.colors.isEmpty()) {
            const QVector4D &color = cloud.colors[index];
            colors.append(QJsonArray{color.x(), color.y(), color.z(), color.w()});
        }
        if (!cloud.sourceIndices.isEmpty())
            sourceIndices.append(cloud.sourceIndices[index]);
    }
    object.insert(QStringLiteral("positions"), positions);
    object.insert(QStringLiteral("radii"), radii);
    object.insert(QStringLiteral("colors"), colors);
    object.insert(QStringLiteral("sourceIndices"), sourceIndices);
    return object;
}

bool pointCloudFromJson(const QJsonValue &value, PointCloud3D *cloud)
{
    if (cloud == nullptr || !value.isObject()) return false;
    const QJsonObject object = value.toObject();
    const QJsonValue positionsValue = object.value(QStringLiteral("positions"));
    const QJsonValue radiiValue = object.value(QStringLiteral("radii"));
    const QJsonValue colorsValue = object.value(QStringLiteral("colors"));
    const QJsonValue indicesValue = object.value(QStringLiteral("sourceIndices"));
    if (!positionsValue.isArray() || positionsValue.toArray().isEmpty() ||
        positionsValue.toArray().size() > 1'000'000 || !radiiValue.isArray() ||
        radiiValue.toArray().size() != positionsValue.toArray().size() ||
        !colorsValue.isArray() ||
        (!colorsValue.toArray().isEmpty() &&
         colorsValue.toArray().size() != positionsValue.toArray().size()) ||
        !indicesValue.isArray() ||
        (!indicesValue.toArray().isEmpty() &&
         indicesValue.toArray().size() != positionsValue.toArray().size()))
        return false;
    PointCloud3D parsed;
    const QJsonArray positions = positionsValue.toArray();
    const QJsonArray radii = radiiValue.toArray();
    const QJsonArray colors = colorsValue.toArray();
    const QJsonArray indices = indicesValue.toArray();
    parsed.positions.reserve(positions.size());
    parsed.radii.reserve(positions.size());
    if (!colors.isEmpty()) parsed.colors.reserve(positions.size());
    if (!indices.isEmpty()) parsed.sourceIndices.reserve(positions.size());
    for (qsizetype index = 0; index < positions.size(); ++index) {
        Point3D point;
        if (!point3DFromJson(positions[index], &point) ||
            !radii[index].isDouble() ||
            !std::isfinite(radii[index].toDouble()) ||
            radii[index].toDouble() < 0.0) return false;
        parsed.positions.append(point);
        parsed.radii.append(radii[index].toDouble());
        if (!colors.isEmpty()) {
            const QJsonValue colorValue = colors[index];
            if (!colorValue.isArray() || colorValue.toArray().size() != 4)
                return false;
            const QJsonArray color = colorValue.toArray();
            for (const QJsonValue &channel : color) {
                if (!channel.isDouble() || !std::isfinite(channel.toDouble()))
                    return false;
            }
            parsed.colors.append(QVector4D(
                float(color[0].toDouble()), float(color[1].toDouble()),
                float(color[2].toDouble()), float(color[3].toDouble())));
        }
        if (!indices.isEmpty()) {
            const QJsonValue sourceIndexValue = indices[index];
            if (!sourceIndexValue.isDouble() ||
                !std::isfinite(sourceIndexValue.toDouble()) ||
                std::floor(sourceIndexValue.toDouble()) !=
                    sourceIndexValue.toDouble() ||
                sourceIndexValue.toDouble() < 0.0 ||
                sourceIndexValue.toDouble() >
                    std::numeric_limits<int>::max()) return false;
            parsed.sourceIndices.append(sourceIndexValue.toInt());
        }
    }
    if (!validatePointCloud(parsed)) return false;
    *cloud = std::move(parsed);
    return true;
}

bool scalarVolumeMatrixFromJson(const QJsonValue &value,
                                ScalarVolumeMatrix *matrix)
{
    if (matrix == nullptr || !value.isArray() || value.toArray().size() != 4)
        return false;
    ScalarVolumeMatrix parsed{};
    const QJsonArray rows = value.toArray();
    for (int row = 0; row < 4; ++row) {
        if (!rows[row].isArray() || rows[row].toArray().size() != 4)
            return false;
        const QJsonArray columns = rows[row].toArray();
        for (int column = 0; column < 4; ++column) {
            if (!columns[column].isDouble() ||
                !std::isfinite(columns[column].toDouble())) return false;
            parsed[static_cast<size_t>(row * 4 + column)] =
                columns[column].toDouble();
        }
    }
    if (!isValidScalarVolumeMatrix(parsed)) return false;
    *matrix = parsed;
    return true;
}

QJsonObject scalarVolumeToJson(const ScalarVolume3D &volume)
{
    QJsonObject object;
    object.insert(QStringLiteral("name"), volume.name);
    object.insert(QStringLiteral("dataType"), volume.dataType);
    object.insert(QStringLiteral("channels"), volume.channels);
    object.insert(QStringLiteral("background"), volume.background);
    object.insert(QStringLiteral("displayDensity"), volume.displayDensity);
    object.insert(QStringLiteral("renderSpace"), volume.renderSpace);
    object.insert(QStringLiteral("interpolationMethod"), volume.interpolationMethod);
    object.insert(QStringLiteral("useSlice"), volume.useSlice);
    object.insert(QStringLiteral("sliceAxis"), volume.sliceAxis);
    object.insert(QStringLiteral("sliceDepth"), volume.sliceDepth);
    object.insert(QStringLiteral("gridToObject"), scalarVolumeMatrixToJson(volume.gridToObject));
    object.insert(QStringLiteral("objectToWorld"), scalarVolumeMatrixToJson(volume.objectToWorld));
    QJsonArray bounds;
    for (int value : volume.activeBounds) bounds.append(value);
    object.insert(QStringLiteral("activeBounds"), bounds);
    object.insert(QStringLiteral("expandedVoxelCount"),
                  static_cast<double>(volume.expandedVoxelCount));
    object.insert(QStringLiteral("denseVoxelCount"),
                  static_cast<double>(volume.denseVoxelCount));
    QJsonArray records;
    for (const ScalarVolumeActiveRecord &record : volume.activeRecords) {
        records.append(QJsonArray{record.x, record.y, record.z, record.extent,
                                  record.isTile, record.value});
    }
    object.insert(QStringLiteral("activeRecords"), records);
    return object;
}

bool scalarVolumeFromJson(const QJsonValue &value, ScalarVolume3D *volume)
{
    if (volume == nullptr || !value.isObject()) return false;
    const QJsonObject object = value.toObject();
    ScalarVolume3D parsed;
    const QJsonValue channels = object.value(QStringLiteral("channels"));
    const QJsonValue count = object.value(QStringLiteral("expandedVoxelCount"));
    const QJsonValue denseCount = object.value(QStringLiteral("denseVoxelCount"));
    const QJsonValue boundsValue = object.value(QStringLiteral("activeBounds"));
    const QJsonValue recordsValue = object.value(QStringLiteral("activeRecords"));
    const QJsonValue density = object.value(QStringLiteral("displayDensity"));
    const QJsonValue sliceDepth = object.value(QStringLiteral("sliceDepth"));
    if (!object.value(QStringLiteral("name")).isString() ||
        !object.value(QStringLiteral("dataType")).isString() ||
        !channels.isDouble() || std::floor(channels.toDouble()) != channels.toDouble() ||
        !object.value(QStringLiteral("background")).isDouble() ||
        std::abs(object.value(QStringLiteral("background")).toDouble()) >
            std::numeric_limits<float>::max() ||
        !density.isDouble() || !object.value(QStringLiteral("renderSpace")).isString() ||
        !std::isfinite(density.toDouble()) || density.toDouble() < 0.0 ||
        density.toDouble() > std::numeric_limits<float>::max() ||
        !object.value(QStringLiteral("interpolationMethod")).isString() ||
        !object.value(QStringLiteral("useSlice")).isBool() ||
        !object.value(QStringLiteral("sliceAxis")).isString() || !sliceDepth.isDouble() ||
        !std::isfinite(sliceDepth.toDouble()) ||
        std::abs(sliceDepth.toDouble()) > std::numeric_limits<float>::max() ||
        !scalarVolumeMatrixFromJson(object.value(QStringLiteral("gridToObject")), &parsed.gridToObject) ||
        !scalarVolumeMatrixFromJson(object.value(QStringLiteral("objectToWorld")), &parsed.objectToWorld) ||
        !boundsValue.isArray() || boundsValue.toArray().size() != 6 ||
        !count.isDouble() || std::floor(count.toDouble()) != count.toDouble() ||
        count.toDouble() < 0.0 || count.toDouble() > 64.0 * 1024.0 * 1024.0 ||
        !denseCount.isDouble() || std::floor(denseCount.toDouble()) != denseCount.toDouble() ||
        denseCount.toDouble() < 0.0 || denseCount.toDouble() > 64.0 * 1024.0 * 1024.0 ||
        !recordsValue.isArray() || recordsValue.toArray().size() > 1'000'000) return false;
    parsed.name = object.value(QStringLiteral("name")).toString();
    parsed.dataType = object.value(QStringLiteral("dataType")).toString();
    parsed.channels = channels.toInt();
    parsed.background = static_cast<float>(object.value(QStringLiteral("background")).toDouble());
    parsed.displayDensity = static_cast<float>(density.toDouble());
    parsed.renderSpace = object.value(QStringLiteral("renderSpace")).toString();
    parsed.interpolationMethod = object.value(QStringLiteral("interpolationMethod")).toString();
    parsed.useSlice = object.value(QStringLiteral("useSlice")).toBool();
    parsed.sliceAxis = object.value(QStringLiteral("sliceAxis")).toString();
    parsed.sliceDepth = static_cast<float>(sliceDepth.toDouble());
    if (!scalarVolumeDensityScale(parsed.renderSpace, parsed.objectToWorld,
                                  parsed.displayDensity, &parsed.densityScale)) return false;
    parsed.expandedVoxelCount = static_cast<std::uint64_t>(count.toDouble());
    parsed.denseVoxelCount = static_cast<std::uint64_t>(denseCount.toDouble());
    const QJsonArray bounds = boundsValue.toArray();
    for (int i = 0; i < 6; ++i) {
        if (!bounds[i].isDouble() || std::floor(bounds[i].toDouble()) != bounds[i].toDouble() ||
            bounds[i].toDouble() < std::numeric_limits<int>::min() ||
            bounds[i].toDouble() > std::numeric_limits<int>::max()) return false;
        parsed.activeBounds[static_cast<size_t>(i)] = bounds[i].toInt();
    }
    const QJsonArray records = recordsValue.toArray();
    parsed.activeRecords.reserve(records.size());
    for (const QJsonValue &recordValue : records) {
        if (!recordValue.isArray() || recordValue.toArray().size() != 6) return false;
        const QJsonArray record = recordValue.toArray();
        int values[4]{};
        for (int i = 0; i < 4; ++i) {
            if (!record[i].isDouble() || std::floor(record[i].toDouble()) != record[i].toDouble() ||
                record[i].toDouble() < std::numeric_limits<int>::min() ||
                record[i].toDouble() > std::numeric_limits<int>::max()) return false;
            values[i] = record[i].toInt();
        }
        if (!record[4].isBool() || !record[5].isDouble() ||
            !std::isfinite(record[5].toDouble()) ||
            std::abs(record[5].toDouble()) > std::numeric_limits<float>::max()) return false;
        parsed.activeRecords.append({values[0], values[1], values[2], values[3],
                                     record[4].toBool(),
                                     static_cast<float>(record[5].toDouble())});
    }
    if (!validateScalarVolume(parsed)) return false;
    *volume = std::move(parsed);
    return true;
}

QJsonObject workPlaneFrameToJson(const WorkPlaneFrame &frame)
{
    QJsonObject value;
    value.insert(QStringLiteral("origin"), point3DToJson(frame.origin));
    value.insert(QStringLiteral("xAxis"), point3DToJson(frame.xAxis));
    value.insert(QStringLiteral("yAxis"), point3DToJson(frame.yAxis));
    value.insert(QStringLiteral("normal"), point3DToJson(frame.normal));
    return value;
}

bool workPlaneFrameFromJson(const QJsonValue &value, WorkPlaneFrame *frame)
{
    if (frame == nullptr || !value.isObject()) {
        return false;
    }
    const QJsonObject object = value.toObject();
    WorkPlaneFrame parsed;
    if (!point3DFromJson(object.value(QStringLiteral("origin")), &parsed.origin) ||
        !point3DFromJson(object.value(QStringLiteral("xAxis")), &parsed.xAxis) ||
        !point3DFromJson(object.value(QStringLiteral("yAxis")), &parsed.yAxis) ||
        !point3DFromJson(object.value(QStringLiteral("normal")), &parsed.normal)) {
        return false;
    }
    parsed.valid = true;
    if (!isValidWorkPlaneFrame(parsed)) {
        return false;
    }
    *frame = parsed;
    return true;
}

} // namespace

QJsonObject pointToJson(const QPointF &point)
{
    QJsonObject object;
    object.insert(QStringLiteral("x"), point.x());
    object.insert(QStringLiteral("y"), point.y());
    return object;
}

bool pointFromJson(const QJsonValue &value, QPointF *point)
{
    if (point == nullptr || !value.isObject()) {
        return false;
    }

    const QJsonObject object = value.toObject();
    const QJsonValue xValue = object.value(QStringLiteral("x"));
    const QJsonValue yValue = object.value(QStringLiteral("y"));
    if (!xValue.isDouble() || !yValue.isDouble()) {
        return false;
    }

    const qreal x = xValue.toDouble();
    const qreal y = yValue.toDouble();
    if (!std::isfinite(x) || !std::isfinite(y)) {
        return false;
    }

    *point = QPointF(x, y);
    return true;
}

QJsonArray pointsToJson(const QVector<QPointF> &points)
{
    QJsonArray array;
    for (const QPointF &point : points) {
        array.append(pointToJson(point));
    }
    return array;
}

bool pointsFromJson(const QJsonValue &value, QVector<QPointF> *points)
{
    if (points == nullptr || !value.isArray()) {
        return false;
    }

    QVector<QPointF> restoredPoints;
    const QJsonArray array = value.toArray();
    restoredPoints.reserve(array.size());
    for (const QJsonValue &pointValue : array) {
        QPointF point;
        if (!pointFromJson(pointValue, &point)) {
            return false;
        }
        restoredPoints.append(point);
    }

    *points = restoredPoints;
    return true;
}

QJsonObject nurbsToJson(const Shape::NurbsCurve2D &curve)
{
    QJsonObject object;
    object.insert(QStringLiteral("dimension"), curve.dimension);
    object.insert(QStringLiteral("degree"), curve.degree);
    object.insert(QStringLiteral("order"), curve.order);
    object.insert(QStringLiteral("rational"), curve.rational);
    object.insert(QStringLiteral("controlPoints"), pointsToJson(curve.controlPoints));
    if (curve.dimension == 3) {
        QJsonArray normalCoordinates;
        for (const double coordinate : curve.normalCoordinates) {
            normalCoordinates.append(coordinate);
        }
        object.insert(QStringLiteral("normalCoordinates"), normalCoordinates);
    }

    QJsonArray weights;
    for (const double weight : curve.weights) {
        weights.append(weight);
    }
    object.insert(QStringLiteral("weights"), weights);

    QJsonArray knots;
    for (const double knot : curve.knots) {
        knots.append(knot);
    }
    object.insert(QStringLiteral("knots"), knots);
    return object;
}

bool nurbsFromJson(const QJsonValue &value, Shape::NurbsCurve2D *curve)
{
    if (curve == nullptr || !value.isObject()) {
        return false;
    }

    const QJsonObject object = value.toObject();
    QVector<QPointF> controlPoints;
    if (!pointsFromJson(object.value(QStringLiteral("controlPoints")), &controlPoints)) {
        return false;
    }

    QVector<double> weights;
    const QJsonValue weightsValue = object.value(QStringLiteral("weights"));
    if (!weightsValue.isArray()) {
        return false;
    }
    for (const QJsonValue &weightValue : weightsValue.toArray()) {
        if (!weightValue.isDouble() || !std::isfinite(weightValue.toDouble())) {
            return false;
        }
        weights.append(weightValue.toDouble());
    }

    QVector<double> knots;
    const QJsonValue knotsValue = object.value(QStringLiteral("knots"));
    if (!knotsValue.isArray()) {
        return false;
    }
    for (const QJsonValue &knotValue : knotsValue.toArray()) {
        if (!knotValue.isDouble() || !std::isfinite(knotValue.toDouble())) {
            return false;
        }
        knots.append(knotValue.toDouble());
    }

    QVector<double> normalCoordinates;
    const QJsonValue normalCoordinatesValue =
        object.value(QStringLiteral("normalCoordinates"));
    if (!normalCoordinatesValue.isUndefined()) {
        if (!normalCoordinatesValue.isArray()) {
            return false;
        }
        for (const QJsonValue &coordinateValue : normalCoordinatesValue.toArray()) {
            if (!coordinateValue.isDouble() ||
                !std::isfinite(coordinateValue.toDouble())) {
                return false;
            }
            normalCoordinates.append(coordinateValue.toDouble());
        }
    }

    curve->dimension = object.value(QStringLiteral("dimension")).toInt(2);
    curve->degree = object.value(QStringLiteral("degree")).toInt(1);
    curve->order = object.value(QStringLiteral("order")).toInt(2);
    curve->rational = object.value(QStringLiteral("rational")).toBool(false);
    curve->controlPoints = controlPoints;
    curve->normalCoordinates = normalCoordinates;
    curve->weights = weights;
    curve->knots = knots;
    // Point and rectangle records carry an empty placeholder NURBS object.
    // Preserve that representation, but reject any populated curve that
    // does not satisfy the shared core invariants.
    if (!curve->controlPoints.isEmpty() || !curve->weights.isEmpty() ||
        !curve->knots.isEmpty()) {
        if (!validateNurbsCurve(*curve)) {
            return false;
        }
    }
    return true;
}

QJsonObject nurbsSurfaceToJson(const Shape::NurbsSurface3D &surface)
{
    QJsonObject object;
    object.insert(QStringLiteral("dimension"), surface.dimension);
    object.insert(QStringLiteral("degreeU"), surface.degreeU);
    object.insert(QStringLiteral("degreeV"), surface.degreeV);
    object.insert(QStringLiteral("orderU"), surface.orderU);
    object.insert(QStringLiteral("orderV"), surface.orderV);
    object.insert(QStringLiteral("controlVertexCountU"),
                  surface.controlVertexCountU);
    object.insert(QStringLiteral("controlVertexCountV"),
                  surface.controlVertexCountV);
    object.insert(QStringLiteral("rational"), surface.rational);

    QJsonArray controlPoints;
    for (const Point3D &point : surface.controlPoints) {
        controlPoints.append(point3DToJson(point));
    }
    object.insert(QStringLiteral("controlPoints"), controlPoints);

    const auto numbersToJson = [](const QVector<double> &numbers) {
        QJsonArray array;
        for (const double number : numbers) {
            array.append(number);
        }
        return array;
    };
    object.insert(QStringLiteral("weights"), numbersToJson(surface.weights));
    object.insert(QStringLiteral("knotsU"), numbersToJson(surface.knotsU));
    object.insert(QStringLiteral("knotsV"), numbersToJson(surface.knotsV));
    QJsonArray trimLoops;
    for (const NurbsSurfaceTrimLoop &loop : surface.trimLoops) {
        QJsonObject trimLoop;
        trimLoop.insert(QStringLiteral("curve"), nurbsToJson(loop.curve));
        trimLoop.insert(QStringLiteral("isHole"), loop.isHole);
        trimLoops.append(trimLoop);
    }
    object.insert(QStringLiteral("trimLoops"), trimLoops);
    return object;
}

bool nurbsSurfaceFromJson(const QJsonValue &value,
                          Shape::NurbsSurface3D *surface)
{
    if (surface == nullptr || !value.isObject()) {
        return false;
    }
    const QJsonObject object = value.toObject();
    const QJsonValue controlPointsValue = object.value(QStringLiteral("controlPoints"));
    const QJsonValue weightsValue = object.value(QStringLiteral("weights"));
    const QJsonValue knotsUValue = object.value(QStringLiteral("knotsU"));
    const QJsonValue knotsVValue = object.value(QStringLiteral("knotsV"));
    if (!controlPointsValue.isArray() || !weightsValue.isArray() ||
        !knotsUValue.isArray() || !knotsVValue.isArray()) {
        return false;
    }

    Shape::NurbsSurface3D result;
    result.dimension = object.value(QStringLiteral("dimension")).toInt(3);
    result.degreeU = object.value(QStringLiteral("degreeU")).toInt(1);
    result.degreeV = object.value(QStringLiteral("degreeV")).toInt(1);
    result.orderU = object.value(QStringLiteral("orderU")).toInt(2);
    result.orderV = object.value(QStringLiteral("orderV")).toInt(2);
    result.controlVertexCountU =
        object.value(QStringLiteral("controlVertexCountU")).toInt(0);
    result.controlVertexCountV =
        object.value(QStringLiteral("controlVertexCountV")).toInt(0);
    result.rational = object.value(QStringLiteral("rational")).toBool(false);

    if (result.controlVertexCountU == 0 && result.controlVertexCountV == 0 &&
        controlPointsValue.toArray().isEmpty() && weightsValue.toArray().isEmpty() &&
        knotsUValue.toArray().isEmpty() && knotsVValue.toArray().isEmpty()) {
        *surface = std::move(result);
        return true;
    }

    result.controlPoints.reserve(controlPointsValue.toArray().size());
    for (const QJsonValue &pointValue : controlPointsValue.toArray()) {
        Point3D point;
        if (!point3DFromJson(pointValue, &point)) {
            return false;
        }
        result.controlPoints.append(point);
    }
    const auto numbersFromJson = [](const QJsonArray &array,
                                    QVector<double> *numbers) {
        numbers->reserve(array.size());
        for (const QJsonValue &numberValue : array) {
            if (!numberValue.isDouble() || !std::isfinite(numberValue.toDouble())) {
                return false;
            }
            numbers->append(numberValue.toDouble());
        }
        return true;
    };
    if (!numbersFromJson(weightsValue.toArray(), &result.weights) ||
        !numbersFromJson(knotsUValue.toArray(), &result.knotsU) ||
        !numbersFromJson(knotsVValue.toArray(), &result.knotsV)) {
        return false;
    }
    const QJsonValue trimLoopsValue = object.value(QStringLiteral("trimLoops"));
    if (!trimLoopsValue.isUndefined()) {
        if (!trimLoopsValue.isArray()) {
            return false;
        }
        for (const QJsonValue &trimLoopValue : trimLoopsValue.toArray()) {
            if (!trimLoopValue.isObject()) {
                return false;
            }
            const QJsonObject trimLoopObject = trimLoopValue.toObject();
            NurbsSurfaceTrimLoop loop;
            if (!nurbsFromJson(trimLoopObject.value(QStringLiteral("curve")),
                               &loop.curve)) {
                return false;
            }
            loop.isHole = trimLoopObject.value(QStringLiteral("isHole")).toBool(false);
            result.trimLoops.append(std::move(loop));
        }
    }
    if (!validateNurbsSurface(result)) {
        return false;
    }
    *surface = std::move(result);
    return true;
}

QJsonObject polygonMeshToJson(const PolygonMesh3D &mesh)
{
    QJsonObject object;
    QJsonArray vertices;
    for (const Point3D &point : mesh.vertices) {
        vertices.append(point3DToJson(point));
    }
    object.insert(QStringLiteral("vertices"), vertices);
    QJsonArray faces;
    for (const QVector<int> &face : mesh.faces) {
        QJsonArray indices;
        for (const int index : face) indices.append(index);
        faces.append(indices);
    }
    object.insert(QStringLiteral("faces"), faces);
    if (!mesh.faceTriangles.isEmpty()) {
        QJsonArray faceTriangles;
        for (const auto &triangles : mesh.faceTriangles) {
            QJsonArray triangleArray;
            for (const auto &triangle : triangles) {
                triangleArray.append(QJsonArray{triangle[0], triangle[1], triangle[2]});
            }
            faceTriangles.append(triangleArray);
        }
        object.insert(QStringLiteral("faceTriangles"), faceTriangles);
    }
    if (!mesh.cornerNormals.isEmpty()) {
        QJsonArray normals;
        for (const Point3D &normal : mesh.cornerNormals)
            normals.append(QJsonArray{normal.x, normal.y, normal.z});
        object.insert(QStringLiteral("cornerNormals"), normals);
    }
    QJsonArray materialIndices;
    for (const int index : mesh.faceMaterialIndices) materialIndices.append(index);
    object.insert(QStringLiteral("faceMaterialIndices"), materialIndices);
    QJsonArray materialSlotIds;
    for (const QString &materialId : mesh.materialSlotIds) {
        materialSlotIds.append(materialId);
    }
    object.insert(QStringLiteral("materialSlotIds"), materialSlotIds);
    QJsonArray uvMaps;
    for (const MeshUvMap &uvMap : mesh.uvMaps) {
        QJsonObject serialized;
        serialized.insert(QStringLiteral("name"), uvMap.name);
        QJsonArray values;
        for (const QPointF &uv : uvMap.cornerValues) {
            values.append(QJsonArray{uv.x(), uv.y()});
        }
        serialized.insert(QStringLiteral("cornerValues"), values);
        uvMaps.append(serialized);
    }
    object.insert(QStringLiteral("uvMaps"), uvMaps);
    QJsonArray attributes;
    for (const MeshColorAttribute &attribute : mesh.colorAttributes) {
        QJsonObject serialized;
        serialized.insert(QStringLiteral("name"), attribute.name);
        serialized.insert(QStringLiteral("domain"), static_cast<int>(attribute.domain));
        serialized.insert(QStringLiteral("dataType"), attribute.dataType);
        serialized.insert(QStringLiteral("colorSpace"), attribute.colorSpace);
        serialized.insert(QStringLiteral("componentCount"), attribute.componentCount);
        QJsonArray values;
        for (const float value : attribute.values) values.append(value);
        serialized.insert(QStringLiteral("values"), values);
        attributes.append(serialized);
    }
    object.insert(QStringLiteral("colorAttributes"), attributes);
    object.insert(QStringLiteral("activeUvMapName"), mesh.activeUvMapName);
    object.insert(QStringLiteral("defaultUvMapName"), mesh.defaultUvMapName);
    object.insert(QStringLiteral("activeColorAttributeName"),
                  mesh.activeColorAttributeName);
    object.insert(QStringLiteral("defaultColorAttributeName"),
                  mesh.defaultColorAttributeName);
    return object;
}

bool polygonMeshFromJson(const QJsonValue &value, PolygonMesh3D *mesh)
{
    if (mesh == nullptr || !value.isObject()) return false;
    const QJsonObject object = value.toObject();
    const QJsonValue verticesValue = object.value(QStringLiteral("vertices"));
    const QJsonValue facesValue = object.value(QStringLiteral("faces"));
    const QJsonValue materialIndicesValue = object.value(QStringLiteral("faceMaterialIndices"));
    const QJsonValue materialSlotsValue = object.value(QStringLiteral("materialSlots"));
    const QJsonValue materialSlotIdsValue = object.value(QStringLiteral("materialSlotIds"));
    const QJsonValue uvMapsValue = object.value(QStringLiteral("uvMaps"));
    const QJsonValue attributesValue = object.value(QStringLiteral("colorAttributes"));
    if (!verticesValue.isArray() || !facesValue.isArray() ||
        !materialIndicesValue.isArray() ||
        (!materialSlotsValue.isArray() && !materialSlotIdsValue.isArray()) ||
        !uvMapsValue.isArray() ||
        !attributesValue.isArray()) return false;

    PolygonMesh3D result;
    for (const QJsonValue &vertexValue : verticesValue.toArray()) {
        Point3D point;
        if (!point3DFromJson(vertexValue, &point)) return false;
        result.vertices.append(point);
    }
    for (const QJsonValue &faceValue : facesValue.toArray()) {
        if (!faceValue.isArray()) return false;
        QVector<int> face;
        for (const QJsonValue &indexValue : faceValue.toArray()) {
            if (!indexValue.isDouble() || !std::isfinite(indexValue.toDouble()) ||
                std::floor(indexValue.toDouble()) != indexValue.toDouble() ||
                indexValue.toInt(-1) < 0) return false;
            face.append(indexValue.toInt());
        }
        result.faces.append(face);
    }
    const QJsonValue faceTrianglesValue = object.value(QStringLiteral("faceTriangles"));
    if (!faceTrianglesValue.isUndefined()) {
        if (!faceTrianglesValue.isArray() ||
            faceTrianglesValue.toArray().size() != result.faces.size()) return false;
        const QJsonArray faceTriangleArray = faceTrianglesValue.toArray();
        for (int faceIndex = 0; faceIndex < result.faces.size(); ++faceIndex) {
            if (!faceTriangleArray[faceIndex].isArray()) return false;
            QVector<std::array<int, 3>> triangles;
            for (const QJsonValue &triangleValue : faceTriangleArray[faceIndex].toArray()) {
                if (!triangleValue.isArray() || triangleValue.toArray().size() != 3)
                    return false;
                const QJsonArray corners = triangleValue.toArray();
                std::array<int, 3> triangle{};
                for (int corner = 0; corner < 3; ++corner) {
                    const QJsonValue value = corners[corner];
                    if (!value.isDouble() || !std::isfinite(value.toDouble()) ||
                        std::floor(value.toDouble()) != value.toDouble()) return false;
                    triangle[static_cast<size_t>(corner)] = value.toInt(-1);
                    if (triangle[static_cast<size_t>(corner)] < 0 ||
                        triangle[static_cast<size_t>(corner)] >=
                            result.faces[faceIndex].size()) return false;
                }
                triangles.append(triangle);
            }
            if (!validatePolygonFaceTriangulation(
                    result.faces[faceIndex].size(), triangles)) {
                return false;
            }
            result.faceTriangles.append(std::move(triangles));
        }
    }
    const QJsonValue cornerNormalsValue = object.value(QStringLiteral("cornerNormals"));
    if (!cornerNormalsValue.isUndefined() && !cornerNormalsValue.isArray()) return false;
    for (const QJsonValue &normalValue : cornerNormalsValue.toArray()) {
        Point3D normal;
        if (!point3DFromJson(normalValue, &normal)) return false;
        result.cornerNormals.append(normal);
    }
    for (const QJsonValue &indexValue : materialIndicesValue.toArray()) {
        if (!indexValue.isDouble() || !std::isfinite(indexValue.toDouble()) ||
            std::floor(indexValue.toDouble()) != indexValue.toDouble() ||
            indexValue.toInt(-1) < 0) return false;
        result.faceMaterialIndices.append(indexValue.toInt());
    }
    for (const QJsonValue &slotIdValue : materialSlotIdsValue.toArray()) {
        if (!slotIdValue.isString()) return false;
        result.materialSlotIds.append(slotIdValue.toString());
    }
    for (const QJsonValue &slotValue : materialSlotsValue.toArray()) {
        if (!slotValue.isObject()) return false;
        const QJsonObject slotObject = slotValue.toObject();
        const QJsonValue color = slotObject.value(QStringLiteral("baseColorSceneLinear"));
        const QJsonValue roughness = slotObject.value(QStringLiteral("roughness"));
        const QJsonValue metallic = slotObject.value(QStringLiteral("metallic"));
        const QJsonValue alpha = slotObject.value(QStringLiteral("alpha"));
        if (!slotObject.value(QStringLiteral("name")).isString() ||
            !slotObject.value(QStringLiteral("libraryPath")).isString() ||
            !color.isArray() || color.toArray().size() != 4 ||
            !roughness.isDouble() || !metallic.isDouble() || !alpha.isDouble()) return false;
        PolygonMeshMaterialSlot slot;
        slot.name = slotObject.value(QStringLiteral("name")).toString();
        slot.libraryPath = slotObject.value(QStringLiteral("libraryPath")).toString();
        slot.baseColorSceneLinear.clear();
        for (const QJsonValue &component : color.toArray()) {
            if (!component.isDouble() || !std::isfinite(component.toDouble())) return false;
            slot.baseColorSceneLinear.append(static_cast<float>(component.toDouble()));
        }
        if (!std::isfinite(roughness.toDouble()) || !std::isfinite(metallic.toDouble()) ||
            !std::isfinite(alpha.toDouble())) return false;
        slot.roughness = static_cast<float>(roughness.toDouble());
        slot.metallic = static_cast<float>(metallic.toDouble());
        slot.alpha = static_cast<float>(alpha.toDouble());
        result.materialSlots.append(std::move(slot));
    }
    for (const QJsonValue &uvMapValue : uvMapsValue.toArray()) {
        if (!uvMapValue.isObject()) return false;
        const QJsonObject uvObject = uvMapValue.toObject();
        if (!uvObject.value(QStringLiteral("name")).isString() ||
            !uvObject.value(QStringLiteral("cornerValues")).isArray()) return false;
        MeshUvMap uvMap;
        uvMap.name = uvObject.value(QStringLiteral("name")).toString();
        for (const QJsonValue &uvValue : uvObject.value(QStringLiteral("cornerValues")).toArray()) {
            if (!uvValue.isArray() || uvValue.toArray().size() != 2 ||
                !uvValue.toArray()[0].isDouble() || !uvValue.toArray()[1].isDouble() ||
                !std::isfinite(uvValue.toArray()[0].toDouble()) ||
                !std::isfinite(uvValue.toArray()[1].toDouble())) return false;
            uvMap.cornerValues.append(QPointF(uvValue.toArray()[0].toDouble(),
                                               uvValue.toArray()[1].toDouble()));
        }
        result.uvMaps.append(std::move(uvMap));
    }
    for (const QJsonValue &attributeValue : attributesValue.toArray()) {
        if (!attributeValue.isObject()) return false;
        const QJsonObject attributeObject = attributeValue.toObject();
        const QJsonValue name = attributeObject.value(QStringLiteral("name"));
        const QJsonValue domain = attributeObject.value(QStringLiteral("domain"));
        const QJsonValue dataType = attributeObject.value(QStringLiteral("dataType"));
        const QJsonValue colorSpace = attributeObject.value(QStringLiteral("colorSpace"));
        const QJsonValue components = attributeObject.value(QStringLiteral("componentCount"));
        const QJsonValue values = attributeObject.value(QStringLiteral("values"));
        if (!name.isString() || !domain.isDouble() || !dataType.isString() ||
            !colorSpace.isString() || !components.isDouble() ||
            !values.isArray()) return false;
        MeshColorAttribute attribute;
        const int domainValue = domain.toInt(-1);
        if (domainValue < static_cast<int>(MeshAttributeDomain::Point) ||
            domainValue > static_cast<int>(MeshAttributeDomain::Corner)) return false;
        attribute.name = name.toString();
        attribute.domain = static_cast<MeshAttributeDomain>(domainValue);
        attribute.dataType = dataType.toString();
        attribute.colorSpace = colorSpace.toString();
        attribute.componentCount = components.toInt(0);
        for (const QJsonValue &component : values.toArray()) {
            if (!component.isDouble() || !std::isfinite(component.toDouble())) return false;
            attribute.values.append(static_cast<float>(component.toDouble()));
        }
        result.colorAttributes.append(std::move(attribute));
    }
    const QJsonValue activeUvMapName = object.value(QStringLiteral("activeUvMapName"));
    const QJsonValue defaultUvMapName = object.value(QStringLiteral("defaultUvMapName"));
    const QJsonValue activeColorAttributeName =
        object.value(QStringLiteral("activeColorAttributeName"));
    const QJsonValue defaultColorAttributeName =
        object.value(QStringLiteral("defaultColorAttributeName"));
    if ((!activeUvMapName.isUndefined() && !activeUvMapName.isString()) ||
        (!defaultUvMapName.isUndefined() && !defaultUvMapName.isString()) ||
        (!activeColorAttributeName.isUndefined() &&
         !activeColorAttributeName.isString()) ||
        (!defaultColorAttributeName.isUndefined() &&
         !defaultColorAttributeName.isString())) return false;
    result.activeUvMapName = activeUvMapName.toString();
    result.defaultUvMapName = defaultUvMapName.toString();
    result.activeColorAttributeName = activeColorAttributeName.toString();
    result.defaultColorAttributeName = defaultColorAttributeName.toString();
    if (!validatePolygonMesh(result)) return false;
    *mesh = std::move(result);
    return true;
}

QJsonObject shapeToJson(const Shape &shape)
{
    QJsonObject object;
    const int geometryTypeValue = static_cast<int>(shape.geometryType);
    const int legacyGeometryTypeValue = legacyValueForGeometryType(shape.geometryType);
    // geometryType is the canonical field. Keep writing the old "tool"
    // integer as a compatibility bridge for version-1 session readers.
    object.insert(QStringLiteral("geometryType"), geometryTypeValue);
    object.insert(QStringLiteral("tool"), legacyGeometryTypeValue);
    object.insert(QStringLiteral("points"), pointsToJson(shape.points));
    object.insert(QStringLiteral("nurbs"), nurbsToJson(shape.nurbs));
    object.insert(QStringLiteral("nurbsSurface"),
                  nurbsSurfaceToJson(shape.nurbsSurface));
    if (shape.geometryType == GeometryType::ScalarVolume) {
        object.insert(QStringLiteral("scalarVolume"),
                      scalarVolumeToJson(shape.scalarVolume));
    }
    if (shape.geometryType == GeometryType::PointCloud) {
        object.insert(QStringLiteral("pointCloud"),
                      pointCloudToJson(shape.pointCloud));
    }
    if (shape.geometryType == GeometryType::PolygonMesh) {
        object.insert(QStringLiteral("polygonMesh"), polygonMeshToJson(shape.polygonMesh));
    }
    if (shape.geometryType == GeometryType::NurbsSolid) {
        QJsonObject solid;
        solid.insert(QStringLiteral("baseSurface"),
                     nurbsSurfaceToJson(shape.nurbsSolid.baseSurface));
        solid.insert(QStringLiteral("displacement"), QJsonArray{
            shape.nurbsSolid.displacement.x, shape.nurbsSolid.displacement.y,
            shape.nurbsSolid.displacement.z});
        if (shape.nurbsSolid.boundaryFaces.isEmpty()) {
            solid.insert(QStringLiteral("baseCapFaceId"),
                         shape.nurbsSolid.baseCapFaceId);
            solid.insert(QStringLiteral("displacedCapFaceId"),
                         shape.nurbsSolid.displacedCapFaceId);
            QJsonArray wallFaceIds;
            for (const QString &faceId : shape.nurbsSolid.wallFaceIds)
                wallFaceIds.append(faceId);
            solid.insert(QStringLiteral("wallFaceIds"), wallFaceIds);
        }
        if (!shape.nurbsSolid.boundaryFaces.isEmpty()) {
            QJsonArray faces;
            QJsonArray reversed;
            for (const NurbsSurface3D &face : shape.nurbsSolid.boundaryFaces)
                faces.append(nurbsSurfaceToJson(face));
            for (bool value : shape.nurbsSolid.boundaryFaceReversed)
                reversed.append(value);
            solid.insert(QStringLiteral("boundaryFaces"), faces);
            solid.insert(QStringLiteral("boundaryFaceReversed"), reversed);
        }
        object.insert(QStringLiteral("nurbsSolid"), solid);
    }
    object.insert(QStringLiteral("workPlane"), static_cast<int>(shape.workPlane));
    object.insert(QStringLiteral("workPlaneOffset"), shape.workPlaneOffset);
    if (isValidWorkPlaneFrame(shape.workPlaneFrame)) {
        object.insert(QStringLiteral("workPlaneFrame"),
                      workPlaneFrameToJson(shape.workPlaneFrame));
    }
    object.insert(QStringLiteral("arcMode"), static_cast<int>(shape.arcMode));
    object.insert(QStringLiteral("arcSweep"), shape.arcSweep);

    if (shape.geometryType == GeometryType::Picture) {
        object.insert(QStringLiteral("pictureShowPerspective"),
                      shape.pictureShowPerspective);
        object.insert(QStringLiteral("pictureShowOrthographic"),
                      shape.pictureShowOrthographic);
        object.insert(QStringLiteral("pictureOnlyAxisAligned"),
                      shape.pictureOnlyAxisAligned);
        const QString pictureSide =
            shape.pictureImageSide == PictureImageSide::Front
                ? QStringLiteral("FRONT")
                : shape.pictureImageSide == PictureImageSide::Back
                      ? QStringLiteral("BACK")
                      : QStringLiteral("DOUBLE_SIDED");
        object.insert(QStringLiteral("pictureImageSide"), pictureSide);
        const QString pictureDepth =
            shape.pictureImageDepth == PictureImageDepth::Front
                ? QStringLiteral("FRONT")
                : shape.pictureImageDepth == PictureImageDepth::Back
                      ? QStringLiteral("BACK")
                      : QStringLiteral("DEFAULT");
        object.insert(QStringLiteral("pictureImageDepth"), pictureDepth);
        object.insert(QStringLiteral("pictureCameraViewOnly"),
                      shape.pictureCameraViewOnly);
        if (shape.pictureCameraLocalCorners.size() == 4) {
            QJsonArray localCorners;
            for (const Point3D &corner : shape.pictureCameraLocalCorners) {
                localCorners.append(QJsonArray{corner.x, corner.y, corner.z});
            }
            object.insert(QStringLiteral("pictureCameraLocalCorners"),
                          localCorners);
        }
        object.insert(QStringLiteral("pictureColorSceneLinear"),
                      QJsonArray{shape.pictureColorSceneLinear.x(),
                                 shape.pictureColorSceneLinear.y(),
                                 shape.pictureColorSceneLinear.z(),
                                 shape.pictureColorSceneLinear.w()});
        object.insert(QStringLiteral("pictureUseAlphaBlend"),
                      shape.pictureUseAlphaBlend);
        object.insert(QStringLiteral("pictureUseViewTransform"),
                      shape.pictureUseViewTransform);
        object.insert(QStringLiteral("picturePremultipliedAlpha"),
                      shape.picturePremultipliedAlpha);
        object.insert(QStringLiteral("pictureColorSpace"),
                      shape.pictureColorSpace);
        object.insert(QStringLiteral("pictureSourceTransformId"),
                      shape.pictureSourceTransformId);
        if (!shape.pictureSceneLinearFloatData.isEmpty()) {
            object.insert(QStringLiteral("pictureSceneLinearFloatData"),
                          QString::fromLatin1(
                              shape.pictureSceneLinearFloatData.toBase64()));
            object.insert(QStringLiteral("pictureSceneLinearFloatWidth"),
                          shape.pictureSceneLinearFloatWidth);
            object.insert(QStringLiteral("pictureSceneLinearFloatHeight"),
                          shape.pictureSceneLinearFloatHeight);
        }
        QByteArray encodedImage = shape.pictureImageData;
        if (encodedImage.isEmpty() && !shape.pictureImage.isNull()) {
            QBuffer imageBuffer(&encodedImage);
            if (imageBuffer.open(QIODevice::WriteOnly)) {
                shape.pictureImage.save(&imageBuffer, "PNG");
            }
        }
        if (!encodedImage.isEmpty()) {
            object.insert(QStringLiteral("pictureImage"),
                          QString::fromLatin1(encodedImage.toBase64()));
        }
    }

    QJsonArray subdivisionParameters;
    for (const double parameter : shape.subdivisionParameters) {
        subdivisionParameters.append(parameter);
    }
    object.insert(QStringLiteral("subdivisionParameters"), subdivisionParameters);

    QJsonArray components;
    for (const Shape::NurbsCurve2D &component : shape.components) {
        components.append(nurbsToJson(component));
    }
    object.insert(QStringLiteral("components"), components);
    if (!shape.componentWorkPlaneFrames.isEmpty()) {
        QJsonArray componentFrames;
        for (const WorkPlaneFrame &frame : shape.componentWorkPlaneFrames) {
            componentFrames.append(workPlaneFrameToJson(frame));
        }
        object.insert(QStringLiteral("componentWorkPlaneFrames"), componentFrames);
    }
    if (!shape.controlPointWeldGroups.isEmpty()) {
        QJsonArray weldGroups;
        for (const quint64 group : shape.controlPointWeldGroups) {
            weldGroups.append(QString::number(group));
        }
        object.insert(QStringLiteral("controlPointWeldGroups"), weldGroups);
    }

    if (!shape.dimensionAnchors.isEmpty()) {
        QJsonArray anchors;
        for (const DimensionAnchorReference &anchor : shape.dimensionAnchors) {
            QJsonObject serializedAnchor;
            serializedAnchor.insert(QStringLiteral("kind"),
                                    static_cast<int>(anchor.kind));
            if (anchor.objectId.isValid()) {
                serializedAnchor.insert(QStringLiteral("objectId"),
                                        QString::number(anchor.objectId.value()));
            }
            serializedAnchor.insert(QStringLiteral("componentIndex"),
                                    anchor.componentIndex);
            serializedAnchor.insert(QStringLiteral("pointIndex"), anchor.pointIndex);
            serializedAnchor.insert(QStringLiteral("parameterFraction"),
                                    anchor.parameterFraction);
            anchors.append(serializedAnchor);
        }
        object.insert(QStringLiteral("dimensionAnchors"), anchors);
    }
    if (shape.dimensionOffsetValid) {
        object.insert(QStringLiteral("dimensionOffset"), shape.dimensionOffset);
    }
    return object;
}

bool shapeFromJson(const QJsonValue &value, Shape *shape)
{
    if (shape == nullptr || !value.isObject()) {
        return false;
    }

    const QJsonObject object = value.toObject();
    WorkPlane workPlane = WorkPlane::XY;
    const QJsonValue workPlaneValue = object.value(QStringLiteral("workPlane"));
    if (!workPlaneValue.isUndefined() &&
        (!workPlaneValue.isDouble() ||
         !workPlaneFromValue(workPlaneValue.toInt(-1), &workPlane))) {
        return false;
    }
    const QJsonValue workPlaneOffsetValue =
        object.value(QStringLiteral("workPlaneOffset"));
    const qreal workPlaneOffset = workPlaneOffsetValue.isUndefined()
                                      ? 0.0
                                      : workPlaneOffsetValue.toDouble(
                                            std::numeric_limits<qreal>::quiet_NaN());
    if (!std::isfinite(workPlaneOffset)) {
        return false;
    }
    WorkPlaneFrame workPlaneFrame;
    const QJsonValue workPlaneFrameValue =
        object.value(QStringLiteral("workPlaneFrame"));
    if (!workPlaneFrameValue.isUndefined() &&
        !workPlaneFrameFromJson(workPlaneFrameValue, &workPlaneFrame)) {
        return false;
    }
    GeometryType geometryType = GeometryType::Invalid;
    const QJsonValue geometryTypeValue = object.value(QStringLiteral("geometryType"));
    if (!geometryTypeValue.isUndefined()) {
        if (!geometryTypeValue.isDouble() ||
            !geometryTypeFromValue(geometryTypeValue.toInt(), &geometryType)) {
            return false;
        }
    } else {
        const QJsonValue legacyToolValue = object.value(QStringLiteral("tool"));
        if (!legacyToolValue.isDouble() ||
            !geometryTypeFromLegacyValue(legacyToolValue.toInt(), &geometryType)) {
            return false;
        }
    }

    const int arcModeValue = object.value(QStringLiteral("arcMode")).toInt(-1);
    if (arcModeValue < static_cast<int>(ArcMode::OnePoint) ||
        arcModeValue > static_cast<int>(ArcMode::ThreePoint)) {
        return false;
    }

    QVector<QPointF> points;
    const QJsonValue pointsValue = object.value(QStringLiteral("points"));
    if (!pointsFromJson(pointsValue, &points) &&
        !(geometryType == GeometryType::Polygon && pointsValue.isUndefined())) {
        return false;
    }
    if (isDimensionGeometryType(geometryType) && points.size() != 3) {
        return false;
    }

    Shape::NurbsCurve2D nurbs;
    if (!nurbsFromJson(object.value(QStringLiteral("nurbs")), &nurbs)) {
        return false;
    }
    if (geometryType == GeometryType::Polygon &&
        !isClosedPolygonNurbs(nurbs)) {
        return false;
    }
    Shape::NurbsSurface3D nurbsSurface;
    const QJsonValue nurbsSurfaceValue = object.value(QStringLiteral("nurbsSurface"));
    if (!nurbsSurfaceValue.isUndefined() &&
        !nurbsSurfaceFromJson(nurbsSurfaceValue, &nurbsSurface)) {
        return false;
    }
    if (geometryType == GeometryType::NurbsSurface &&
        !validateNurbsSurface(nurbsSurface)) {
        return false;
    }
    PolygonMesh3D polygonMesh;
    const QJsonValue polygonMeshValue = object.value(QStringLiteral("polygonMesh"));
    if (geometryType == GeometryType::PolygonMesh &&
        !polygonMeshFromJson(polygonMeshValue, &polygonMesh)) {
        return false;
    }
    ScalarVolume3D scalarVolume;
    if (geometryType == GeometryType::ScalarVolume &&
        !scalarVolumeFromJson(object.value(QStringLiteral("scalarVolume")),
                              &scalarVolume)) return false;
    PointCloud3D pointCloud;
    if (geometryType == GeometryType::PointCloud &&
        !pointCloudFromJson(object.value(QStringLiteral("pointCloud")),
                            &pointCloud)) return false;
    QImage pictureImage;
    bool pictureShowPerspective = true;
    bool pictureShowOrthographic = true;
    bool pictureOnlyAxisAligned = false;
    bool pictureCameraViewOnly = false;
    QVector<Point3D> pictureCameraLocalCorners;
    PictureImageSide pictureImageSide = PictureImageSide::Both;
    PictureImageDepth pictureImageDepth = PictureImageDepth::Default;
    QVector4D pictureColorSceneLinear(1.0f, 1.0f, 1.0f, 1.0f);
    bool pictureUseAlphaBlend = true;
    bool pictureUseViewTransform = false;
    bool picturePremultipliedAlpha = false;
    QString pictureColorSpace = QStringLiteral("sRGB");
    QString pictureSourceTransformId = QStringLiteral("identity");
    QByteArray pictureSceneLinearFloatData;
    int pictureSceneLinearFloatWidth = 0;
    int pictureSceneLinearFloatHeight = 0;
    if (geometryType == GeometryType::Picture) {
        const auto readBoolean = [&object](const QString &key, bool fallback,
                                           bool *destination) {
            const QJsonValue value = object.value(key);
            if (value.isUndefined()) {
                *destination = fallback;
                return true;
            }
            if (!value.isBool()) return false;
            *destination = value.toBool();
            return true;
        };
        if (!readBoolean(QStringLiteral("pictureShowPerspective"), true,
                         &pictureShowPerspective) ||
            !readBoolean(QStringLiteral("pictureShowOrthographic"), true,
                         &pictureShowOrthographic) ||
            !readBoolean(QStringLiteral("pictureOnlyAxisAligned"), false,
                         &pictureOnlyAxisAligned) ||
            !readBoolean(QStringLiteral("pictureCameraViewOnly"), false,
                         &pictureCameraViewOnly)) {
            return false;
        }
        const QJsonValue cameraLocalCornersValue = object.value(
            QStringLiteral("pictureCameraLocalCorners"));
        if (!cameraLocalCornersValue.isUndefined()) {
            if (!cameraLocalCornersValue.isArray() ||
                cameraLocalCornersValue.toArray().size() != 4) return false;
            for (const QJsonValue &cornerValue :
                 cameraLocalCornersValue.toArray()) {
                if (!cornerValue.isArray() ||
                    cornerValue.toArray().size() != 3) return false;
                const QJsonArray coordinates = cornerValue.toArray();
                for (const QJsonValue &coordinate : coordinates) {
                    if (!coordinate.isDouble() ||
                        !std::isfinite(coordinate.toDouble())) return false;
                }
                pictureCameraLocalCorners.append(
                    {coordinates[0].toDouble(), coordinates[1].toDouble(),
                     coordinates[2].toDouble()});
            }
        }
        const QJsonValue pictureColorValue =
            object.value(QStringLiteral("pictureColorSceneLinear"));
        if (!pictureColorValue.isUndefined()) {
            if (!pictureColorValue.isArray() ||
                pictureColorValue.toArray().size() != 4)
                return false;
            const QJsonArray pictureColor = pictureColorValue.toArray();
            for (const QJsonValue &channel : pictureColor) {
                if (!channel.isDouble() ||
                    !std::isfinite(channel.toDouble())) return false;
            }
            pictureColorSceneLinear = QVector4D(
                float(pictureColor[0].toDouble()),
                float(pictureColor[1].toDouble()),
                float(pictureColor[2].toDouble()),
                float(pictureColor[3].toDouble()));
        }
        if (!readBoolean(QStringLiteral("pictureUseAlphaBlend"), true,
                         &pictureUseAlphaBlend) ||
            !readBoolean(QStringLiteral("pictureUseViewTransform"), false,
                         &pictureUseViewTransform) ||
            !readBoolean(QStringLiteral("picturePremultipliedAlpha"), false,
                         &picturePremultipliedAlpha)) {
            return false;
        }
        const QJsonValue pictureColorSpaceValue =
            object.value(QStringLiteral("pictureColorSpace"));
        const QJsonValue pictureSourceTransformIdValue =
            object.value(QStringLiteral("pictureSourceTransformId"));
        const QJsonValue pictureFloatDataValue =
            object.value(QStringLiteral("pictureSceneLinearFloatData"));
        if ((!pictureColorSpaceValue.isUndefined() &&
             !pictureColorSpaceValue.isString()) ||
            (!pictureSourceTransformIdValue.isUndefined() &&
             !pictureSourceTransformIdValue.isString()) ||
            (!pictureFloatDataValue.isUndefined() &&
             !pictureFloatDataValue.isString())) {
            return false;
        }
        pictureColorSpace = pictureColorSpaceValue.toString(
            QStringLiteral("sRGB"));
        pictureSourceTransformId = pictureSourceTransformIdValue.toString(
            QStringLiteral("identity"));
        const QString encodedPictureFloatData =
            pictureFloatDataValue.toString();
        if (!encodedPictureFloatData.isEmpty()) {
            const QJsonValue widthValue = object.value(
                QStringLiteral("pictureSceneLinearFloatWidth"));
            const QJsonValue heightValue = object.value(
                QStringLiteral("pictureSceneLinearFloatHeight"));
            const double widthNumber = widthValue.toDouble(-1.0);
            const double heightNumber = heightValue.toDouble(-1.0);
            if (!widthValue.isDouble() || !heightValue.isDouble() ||
                !std::isfinite(widthNumber) || !std::isfinite(heightNumber) ||
                std::floor(widthNumber) != widthNumber ||
                std::floor(heightNumber) != heightNumber ||
                widthNumber < 1.0 || heightNumber < 1.0 ||
                widthNumber > 16384.0 || heightNumber > 16384.0) {
                return false;
            }
            pictureSceneLinearFloatWidth = static_cast<int>(widthNumber);
            pictureSceneLinearFloatHeight = static_cast<int>(heightNumber);
            if (!decodePictureFloatPayload(
                    encodedPictureFloatData,
                    pictureSceneLinearFloatWidth,
                    pictureSceneLinearFloatHeight,
                    &pictureSceneLinearFloatData)) {
                return false;
            }
        }
        const QJsonValue sideValue =
            object.value(QStringLiteral("pictureImageSide"));
        if (!sideValue.isUndefined()) {
            if (!sideValue.isString()) return false;
            const QString side = sideValue.toString();
            if (side == QLatin1String("FRONT")) {
                pictureImageSide = PictureImageSide::Front;
            } else if (side == QLatin1String("BACK")) {
                pictureImageSide = PictureImageSide::Back;
            } else if (side != QLatin1String("DOUBLE_SIDED")) {
                return false;
            }
        }
        const QJsonValue depthValue =
            object.value(QStringLiteral("pictureImageDepth"));
        if (!depthValue.isUndefined()) {
            if (!depthValue.isString()) return false;
            const QString depth = depthValue.toString();
            if (depth == QLatin1String("FRONT")) {
                pictureImageDepth = PictureImageDepth::Front;
            } else if (depth == QLatin1String("BACK")) {
                pictureImageDepth = PictureImageDepth::Back;
            } else if (depth != QLatin1String("DEFAULT")) {
                return false;
            }
        }
    }
    NurbsExtrusionSolid3D nurbsSolid;
    if (geometryType == GeometryType::NurbsSolid) {
        const QJsonObject solid = object.value(QStringLiteral("nurbsSolid")).toObject();
        const QJsonArray offset = solid.value(QStringLiteral("displacement")).toArray();
        if (offset.size() != 3 || !offset[0].isDouble() ||
            !offset[1].isDouble() || !offset[2].isDouble() ||
            !nurbsSurfaceFromJson(solid.value(QStringLiteral("baseSurface")),
                                  &nurbsSolid.baseSurface)) return false;
        nurbsSolid.displacement = {offset[0].toDouble(), offset[1].toDouble(),
                                    offset[2].toDouble()};
        if (solid.contains(QStringLiteral("boundaryFaces"))) {
            if (!solid.value(QStringLiteral("boundaryFaces")).isArray() ||
                !solid.value(QStringLiteral("boundaryFaceReversed")).isArray()) return false;
            for (const QJsonValue &value : solid.value(QStringLiteral("boundaryFaces")).toArray()) {
                NurbsSurface3D face;
                if (!nurbsSurfaceFromJson(value, &face)) return false;
                nurbsSolid.boundaryFaces.append(face);
            }
            for (const QJsonValue &value : solid.value(QStringLiteral("boundaryFaceReversed")).toArray()) {
                if (!value.isBool()) return false;
                nurbsSolid.boundaryFaceReversed.append(value.toBool());
            }
        }
        if (nurbsSolid.boundaryFaces.isEmpty()) {
            const bool hasAnyFaceIds = solid.contains(QStringLiteral("baseCapFaceId")) ||
                solid.contains(QStringLiteral("displacedCapFaceId")) ||
                solid.contains(QStringLiteral("wallFaceIds"));
            if (hasAnyFaceIds) {
                if (!solid.value(QStringLiteral("baseCapFaceId")).isString() ||
                    !solid.value(QStringLiteral("displacedCapFaceId")).isString() ||
                    !solid.value(QStringLiteral("wallFaceIds")).isArray()) return false;
                nurbsSolid.baseCapFaceId =
                    solid.value(QStringLiteral("baseCapFaceId")).toString();
                nurbsSolid.displacedCapFaceId =
                    solid.value(QStringLiteral("displacedCapFaceId")).toString();
                for (const QJsonValue &value :
                     solid.value(QStringLiteral("wallFaceIds")).toArray()) {
                    if (!value.isString()) return false;
                    nurbsSolid.wallFaceIds.append(value.toString());
                }
            } else if (!initializeNurbsSolidSeedFaceIds(&nurbsSolid)) {
                return false;
            }
        }
        if (!validateNurbsSolid(nurbsSolid)) return false;
    }
    if (geometryType == GeometryType::Picture) {
        const QJsonValue imageValue = object.value(QStringLiteral("pictureImage"));
        if (points.size() != 4 || !imageValue.isString() ||
            imageValue.toString().isEmpty()) {
            return false;
        }
        const QByteArray imageBytes = QByteArray::fromBase64(
            imageValue.toString().toLatin1());
        QBuffer imageBuffer;
        imageBuffer.setData(imageBytes);
        if (!imageBuffer.open(QIODevice::ReadOnly)) {
            return false;
        }
        QImageReader reader(&imageBuffer);
        reader.setAutoTransform(true);
        pictureImage = reader.read();
        if (pictureImage.isNull()) {
            return false;
        }
        if (!pictureSceneLinearFloatData.isEmpty() &&
            (pictureSceneLinearFloatWidth != pictureImage.width() ||
             pictureSceneLinearFloatHeight != pictureImage.height())) {
            return false;
        }
    }

    const QJsonValue arcSweepValue = object.value(QStringLiteral("arcSweep"));
    if (!arcSweepValue.isDouble() || !std::isfinite(arcSweepValue.toDouble())) {
        return false;
    }

    QVector<double> subdivisionParameters;
    const QJsonValue subdivisionValue = object.value(QStringLiteral("subdivisionParameters"));
    if (!subdivisionValue.isUndefined()) {
        if (!subdivisionValue.isArray()) {
            return false;
        }
        for (const QJsonValue &parameterValue : subdivisionValue.toArray()) {
            if (!parameterValue.isDouble() || !std::isfinite(parameterValue.toDouble())) {
                return false;
            }
            subdivisionParameters.append(parameterValue.toDouble());
        }
    }

    QVector<Shape::NurbsCurve2D> components;
    const QJsonValue componentsValue = object.value(QStringLiteral("components"));
    if (!componentsValue.isUndefined()) {
        if (!componentsValue.isArray()) {
            return false;
        }
        for (const QJsonValue &componentValue : componentsValue.toArray()) {
            Shape::NurbsCurve2D component;
            if (!nurbsFromJson(componentValue, &component)) {
                return false;
            }
            components.append(component);
        }
    }

    QVector<WorkPlaneFrame> componentWorkPlaneFrames;
    const QJsonValue componentFramesValue =
        object.value(QStringLiteral("componentWorkPlaneFrames"));
    if (!componentFramesValue.isUndefined()) {
        if (!componentFramesValue.isArray() ||
            componentFramesValue.toArray().size() != components.size()) {
            return false;
        }
        for (const QJsonValue &frameValue : componentFramesValue.toArray()) {
            WorkPlaneFrame frame;
            if (!workPlaneFrameFromJson(frameValue, &frame)) {
                return false;
            }
            componentWorkPlaneFrames.append(frame);
        }
    }

    QVector<quint64> controlPointWeldGroups;
    const QJsonValue weldGroupsValue =
        object.value(QStringLiteral("controlPointWeldGroups"));
    if (!weldGroupsValue.isUndefined()) {
        int controlPointCount = 0;
        if (geometryType == GeometryType::Point) {
            controlPointCount = points.size();
        } else if (geometryType == GeometryType::NurbsSurface) {
            controlPointCount = nurbsSurface.controlPoints.size();
        } else if (geometryType == GeometryType::PolyCurve) {
            for (const Shape::NurbsCurve2D &component : components) {
                controlPointCount += component.controlPoints.size();
            }
        } else {
            switch (geometryType) {
            case GeometryType::Line:
            case GeometryType::Arc:
            case GeometryType::Bezier:
            case GeometryType::Nurbs:
            case GeometryType::Rectangle:
            case GeometryType::Circle:
            case GeometryType::Ellipse:
            case GeometryType::Polygon:
                controlPointCount = nurbs.controlPoints.size();
                break;
            default:
                break;
            }
        }
        if (!weldGroupsValue.isArray() ||
            weldGroupsValue.toArray().size() != controlPointCount) {
            return false;
        }
        controlPointWeldGroups.reserve(controlPointCount);
        for (const QJsonValue &groupValue : weldGroupsValue.toArray()) {
            if (!groupValue.isString()) {
                return false;
            }
            bool groupValid = false;
            const quint64 group = groupValue.toString().toULongLong(&groupValid);
            if (!groupValid) {
                return false;
            }
            controlPointWeldGroups.append(group);
        }
    }

    QVector<DimensionAnchorReference> dimensionAnchors;
    const QJsonValue dimensionAnchorsValue =
        object.value(QStringLiteral("dimensionAnchors"));
    if (!dimensionAnchorsValue.isUndefined()) {
        if (!isDimensionGeometryType(geometryType) ||
            !dimensionAnchorsValue.isArray() ||
            dimensionAnchorsValue.toArray().size() > 3) {
            return false;
        }
        for (const QJsonValue &anchorValue : dimensionAnchorsValue.toArray()) {
            if (!anchorValue.isObject()) {
                return false;
            }
            const QJsonObject anchorObject = anchorValue.toObject();
            const int kindValue = anchorObject.value(QStringLiteral("kind")).toInt(-1);
            if (kindValue < static_cast<int>(DimensionAnchorKind::None) ||
                kindValue > static_cast<int>(DimensionAnchorKind::Center)) {
                return false;
            }
            DimensionAnchorReference anchor;
            anchor.kind = static_cast<DimensionAnchorKind>(kindValue);
            const QJsonValue objectIdValue =
                anchorObject.value(QStringLiteral("objectId"));
            if (anchor.kind != DimensionAnchorKind::None) {
                if (!objectIdValue.isString()) {
                    return false;
                }
                bool objectIdValid = false;
                const quint64 objectId = objectIdValue.toString().toULongLong(
                    &objectIdValid);
                if (!objectIdValid || objectId == 0) {
                    return false;
                }
                anchor.objectId = ObjectId::fromValue(objectId);
            }
            anchor.componentIndex =
                anchorObject.value(QStringLiteral("componentIndex")).toInt(-1);
            anchor.pointIndex = anchorObject.value(QStringLiteral("pointIndex")).toInt(-1);
            const QJsonValue parameterFractionValue =
                anchorObject.value(QStringLiteral("parameterFraction"));
            anchor.parameterFraction = parameterFractionValue.isDouble()
                                           ? parameterFractionValue.toDouble()
                                           : 0.0;
            if (!std::isfinite(anchor.parameterFraction) ||
                anchor.parameterFraction < 0.0 ||
                anchor.parameterFraction > 1.0 ||
                (anchor.kind == DimensionAnchorKind::ControlPoint &&
                 anchor.pointIndex < 0) ||
                (anchor.kind == DimensionAnchorKind::ShapePoint &&
                 anchor.pointIndex < 0) ||
                (anchor.kind == DimensionAnchorKind::CurveParameter &&
                 anchor.componentIndex < -1)) {
                return false;
            }
            dimensionAnchors.append(anchor);
        }
    }

    qreal dimensionOffset = 0.0;
    bool dimensionOffsetValid = false;
    const QJsonValue dimensionOffsetValue =
        object.value(QStringLiteral("dimensionOffset"));
    if (!dimensionOffsetValue.isUndefined()) {
        if (!isDimensionGeometryType(geometryType) ||
            !dimensionOffsetValue.isDouble() ||
            !std::isfinite(dimensionOffsetValue.toDouble())) {
            return false;
        }
        dimensionOffset = dimensionOffsetValue.toDouble();
        dimensionOffsetValid = true;
    }

    if ((geometryType == GeometryType::Circle ||
         geometryType == GeometryType::Ellipse) &&
        validateNurbsCurve(nurbs) && nurbs.controlPoints.size() >= 2) {
        const QPointF seam =
            (nurbs.controlPoints.first() + nurbs.controlPoints.last()) * 0.5;
        nurbs.controlPoints.first() = seam;
        nurbs.controlPoints.last() = seam;
        if (nurbs.dimension == 3) {
            const qreal normalSeam =
                (nurbs.normalCoordinates.first() +
                 nurbs.normalCoordinates.last()) * 0.5;
            nurbs.normalCoordinates.first() = normalSeam;
            nurbs.normalCoordinates.last() = normalSeam;
        }
    }

    shape->geometryType = geometryType;
    shape->points = points;
    shape->nurbs = nurbs;
    shape->nurbsSurface = nurbsSurface;
    shape->nurbsSolid = nurbsSolid;
    shape->polygonMesh = std::move(polygonMesh);
    shape->scalarVolume = std::move(scalarVolume);
    shape->pointCloud = std::move(pointCloud);
    shape->arcMode = static_cast<ArcMode>(arcModeValue);
    shape->arcSweep = arcSweepValue.toDouble();
    shape->subdivisionParameters = subdivisionParameters;
    shape->components = components;
    shape->componentWorkPlaneFrames = componentWorkPlaneFrames;
    shape->controlPointWeldGroups = controlPointWeldGroups;
    shape->dimensionAnchors = dimensionAnchors;
    shape->dimensionOffset = dimensionOffset;
    shape->dimensionOffsetValid = dimensionOffsetValid;
    shape->pictureImage = pictureImage;
    shape->pictureShowPerspective = pictureShowPerspective;
    shape->pictureShowOrthographic = pictureShowOrthographic;
    shape->pictureOnlyAxisAligned = pictureOnlyAxisAligned;
    shape->pictureCameraViewOnly = pictureCameraViewOnly;
    shape->pictureCameraLocalCorners = pictureCameraLocalCorners;
    shape->pictureImageSide = pictureImageSide;
    shape->pictureImageDepth = pictureImageDepth;
    shape->pictureColorSceneLinear = pictureColorSceneLinear;
    shape->pictureUseAlphaBlend = pictureUseAlphaBlend;
    shape->pictureUseViewTransform = pictureUseViewTransform;
    shape->picturePremultipliedAlpha = picturePremultipliedAlpha;
    shape->pictureColorSpace = pictureColorSpace;
    shape->pictureSourceTransformId = pictureSourceTransformId;
    shape->pictureSceneLinearFloatData = pictureSceneLinearFloatData;
    shape->pictureSceneLinearFloatWidth = pictureSceneLinearFloatWidth;
    shape->pictureSceneLinearFloatHeight = pictureSceneLinearFloatHeight;
    shape->pictureImageData = geometryType == GeometryType::Picture
                                  ? QByteArray::fromBase64(
                                        object.value(QStringLiteral("pictureImage"))
                                            .toString()
                                            .toLatin1())
                                  : QByteArray{};
    shape->workPlane = workPlane;
    shape->workPlaneOffset = workPlaneOffset;
    shape->workPlaneFrame = workPlaneFrame;
    return true;
}

} // namespace classiCAD
