#include "shape_json_codec.h"

#include "core/geometry/arc_mode.h"
#include "core/geometry/geometry_type.h"
#include "core/geometry/nurbs_curve.h"
#include "core/geometry/nurbs_surface.h"
#include "core/geometry/shape_mapping.h"
#include "core/geometry/work_plane.h"

#include <QBuffer>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonObject>
#include <QIODevice>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace classiCAD {

namespace {

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
    if (shape.geometryType == GeometryType::NurbsSolid) {
        QJsonObject solid;
        solid.insert(QStringLiteral("baseSurface"),
                     nurbsSurfaceToJson(shape.nurbsSolid.baseSurface));
        solid.insert(QStringLiteral("displacement"), QJsonArray{
            shape.nurbsSolid.displacement.x, shape.nurbsSolid.displacement.y,
            shape.nurbsSolid.displacement.z});
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
    QImage pictureImage;
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
