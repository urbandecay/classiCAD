#include "rhino3dm_interchange.h"

#include "core/document/document.h"

#include <opennurbs.h>

#include <QHash>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <utility>

namespace classiCAD {
namespace {

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

bool workPlaneFrameFromOpenNurbsPlane(const ON_Plane &plane,
                                      double unitScale,
                                      WorkPlane *workPlane,
                                      qreal *workPlaneOffset,
                                      WorkPlaneFrame *frame)
{
    if (workPlane == nullptr || workPlaneOffset == nullptr || frame == nullptr ||
        !plane.IsValid() || !std::isfinite(unitScale) || unitScale <= 0.0) {
        return false;
    }
    const ON_3dPoint &origin = plane.Origin();
    const ON_3dVector &xAxis = plane.Xaxis();
    const ON_3dVector &yAxis = plane.Yaxis();
    const ON_3dVector &normal = plane.Normal();
    if (!std::isfinite(origin.x) || !std::isfinite(origin.y) ||
        !std::isfinite(origin.z) || !std::isfinite(xAxis.x) ||
        !std::isfinite(xAxis.y) || !std::isfinite(xAxis.z) ||
        !std::isfinite(yAxis.x) || !std::isfinite(yAxis.y) ||
        !std::isfinite(yAxis.z) || !std::isfinite(normal.x) ||
        !std::isfinite(normal.y) || !std::isfinite(normal.z)) {
        return false;
    }

    constexpr qreal principalAlignmentTolerance = 1.0e-8;
    if (std::abs(normal.z) >= 1.0 - principalAlignmentTolerance) {
        *workPlane = WorkPlane::XY;
        *workPlaneOffset = origin.z * unitScale;
        *frame = makeWorkPlaneFrame(*workPlane, *workPlaneOffset);
    } else if (std::abs(normal.y) >= 1.0 - principalAlignmentTolerance) {
        *workPlane = WorkPlane::XZ;
        *workPlaneOffset = origin.y * unitScale;
        *frame = makeWorkPlaneFrame(*workPlane, *workPlaneOffset);
    } else if (std::abs(normal.x) >= 1.0 - principalAlignmentTolerance) {
        *workPlane = WorkPlane::YZ;
        *workPlaneOffset = origin.x * unitScale;
        *frame = makeWorkPlaneFrame(*workPlane, *workPlaneOffset);
    } else {
        *workPlane = WorkPlane::XY;
        *workPlaneOffset = 0.0;
        Point3D canonicalNormal{normal.x, normal.y, normal.z};
        constexpr qreal signTolerance = 1.0e-12;
        const bool reverseNormal = canonicalNormal.x < -signTolerance ||
            (std::abs(canonicalNormal.x) <= signTolerance &&
             (canonicalNormal.y < -signTolerance ||
              (std::abs(canonicalNormal.y) <= signTolerance &&
               canonicalNormal.z < 0.0)));
        if (reverseNormal) {
            canonicalNormal.x = -canonicalNormal.x;
            canonicalNormal.y = -canonicalNormal.y;
            canonicalNormal.z = -canonicalNormal.z;
        }
        const Point3D scaledOrigin{origin.x * unitScale,
                                   origin.y * unitScale,
                                   origin.z * unitScale};
        const qreal planeDistance = scaledOrigin.x * canonicalNormal.x +
                                    scaledOrigin.y * canonicalNormal.y +
                                    scaledOrigin.z * canonicalNormal.z;
        const Point3D canonicalOrigin{canonicalNormal.x * planeDistance,
                                      canonicalNormal.y * planeDistance,
                                      canonicalNormal.z * planeDistance};
        // Canonicalizing the normal sign, in-plane axes, and origin gives
        // separate .3dm curves on the same plane one identical local frame,
        // so their existing OSnap candidates remain interoperable.
        *frame = makeWorkPlaneFrameFromNormal(canonicalOrigin,
                                              canonicalNormal);
    }
    return isValidWorkPlaneFrame(*frame);
}

bool importNurbsCurve(const ON_Curve &source,
                      double unitScale,
                      double planarTolerance,
                      Shape::NurbsCurve2D *destination,
                      WorkPlane *destinationPlane,
                      qreal *destinationOffset,
                      WorkPlaneFrame *destinationFrame,
                      const WorkPlaneFrame *forcedFrame,
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
    QVector<Point3D> worldControlPoints;
    worldControlPoints.reserve(converted.CVCount());
    for (int index = 0; index < converted.CVCount(); ++index) {
        ON_3dPoint point;
        if (!converted.GetCV(index, point) || !std::isfinite(point.x) ||
            !std::isfinite(point.y) || !std::isfinite(point.z)) {
            if (reason != nullptr) {
                *reason = QStringLiteral("curve has a non-finite control vertex");
            }
            return false;
        }
        worldControlPoints.append({point.x, point.y, point.z});

        const double weight = result.rational ? converted.Weight(index) : 1.0;
        if (!std::isfinite(weight) || weight <= 0.0) {
            if (reason != nullptr) {
                *reason = QStringLiteral("curve has a non-positive rational weight");
            }
            return false;
        }
        result.weights.append(weight);
    }

    qreal minX = worldControlPoints.first().x;
    qreal maxX = minX;
    qreal minY = worldControlPoints.first().y;
    qreal maxY = minY;
    qreal minZ = worldControlPoints.first().z;
    qreal maxZ = minZ;
    for (const Point3D &point : worldControlPoints) {
        minX = std::min(minX, point.x);
        maxX = std::max(maxX, point.x);
        minY = std::min(minY, point.y);
        maxY = std::max(maxY, point.y);
        minZ = std::min(minZ, point.z);
        maxZ = std::max(maxZ, point.z);
    }
    WorkPlane plane = WorkPlane::XY;
    qreal offset = 0.0;
    WorkPlaneFrame frame;
    if (forcedFrame != nullptr && isValidWorkPlaneFrame(*forcedFrame)) {
        frame = *forcedFrame;
    } else if (maxZ - minZ <= planarTolerance) {
        plane = WorkPlane::XY;
        offset = worldControlPoints.first().z * unitScale;
        frame = makeWorkPlaneFrame(plane, offset);
    } else if (maxY - minY <= planarTolerance) {
        plane = WorkPlane::XZ;
        offset = worldControlPoints.first().y * unitScale;
        frame = makeWorkPlaneFrame(plane, offset);
    } else if (maxX - minX <= planarTolerance) {
        plane = WorkPlane::YZ;
        offset = worldControlPoints.first().x * unitScale;
        frame = makeWorkPlaneFrame(plane, offset);
    } else {
        ON_Plane curvePlane;
        if (!converted.IsPlanar(&curvePlane, planarTolerance) ||
            !workPlaneFrameFromOpenNurbsPlane(curvePlane,
                                              unitScale,
                                              &plane,
                                              &offset,
                                              &frame)) {
            if (reason != nullptr) {
                *reason = QStringLiteral("curve is not planar or its plane is invalid");
            }
            return false;
        }
    }
    if (!isValidWorkPlaneFrame(frame)) {
        if (reason != nullptr) {
            *reason = QStringLiteral("curve workplane frame is invalid");
        }
        return false;
    }
    result.controlPoints.reserve(worldControlPoints.size());
    for (const Point3D &point : worldControlPoints) {
        const Point3D scaledPoint{point.x * unitScale,
                                  point.y * unitScale,
                                  point.z * unitScale};
        result.controlPoints.append(worldPointToWorkPlaneFrame(scaledPoint, frame));
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
    if (destinationPlane != nullptr) {
        *destinationPlane = plane;
    }
    if (destinationOffset != nullptr) {
        *destinationOffset = offset;
    }
    if (destinationFrame != nullptr) {
        *destinationFrame = frame;
    }
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
        result.componentWorkPlaneFrames.reserve(polyCurve->Count());
        for (int index = 0; index < polyCurve->Count(); ++index) {
            const ON_Curve *segment = polyCurve->SegmentCurve(index);
            Shape::NurbsCurve2D converted;
            WorkPlane segmentPlane = WorkPlane::XY;
            qreal segmentOffset = 0.0;
            WorkPlaneFrame segmentFrame;
            if (segment == nullptr ||
                !importNurbsCurve(*segment,
                                  unitScale,
                                  planarTolerance,
                                  &converted,
                                  &segmentPlane,
                                  &segmentOffset,
                                  &segmentFrame,
                                  nullptr,
                                  reason)) {
                return false;
            }
            if (result.components.isEmpty()) {
                result.workPlane = segmentPlane;
                result.workPlaneOffset = segmentOffset;
                result.workPlaneFrame = segmentFrame;
            }
            if (result.points.isEmpty()) {
                const Point3D startWorld = workPlaneFramePointToWorld(
                    converted.controlPoints.first(), segmentFrame);
                result.points.append(worldPointToWorkPlaneFrame(
                    startWorld, result.workPlaneFrame));
            }
            const Point3D endWorld = workPlaneFramePointToWorld(
                converted.controlPoints.last(), segmentFrame);
            result.points.append(worldPointToWorkPlaneFrame(
                endWorld, result.workPlaneFrame));
            result.components.append(std::move(converted));
            result.componentWorkPlaneFrames.append(segmentFrame);
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
    if (!importNurbsCurve(curve,
                          unitScale,
                          planarTolerance,
                          &result.nurbs,
                          &result.workPlane,
                          &result.workPlaneOffset,
                          &result.workPlaneFrame,
                          nullptr,
                          reason)) {
        return false;
    }

    if (const ON_ArcCurve *arcCurve = ON_ArcCurve::Cast(&curve);
        arcCurve != nullptr && arcCurve->IsCircle()) {
        const ON_3dPoint center = arcCurve->m_arc.Center();
        const ON_3dPoint edge = arcCurve->m_arc.PointAt(0.0);
        result.geometryType = GeometryType::Circle;
        result.points = {
            shapeWorldPointToLocal(result,
                                   {center.x * unitScale,
                                    center.y * unitScale,
                                    center.z * unitScale}),
            shapeWorldPointToLocal(result,
                                   {edge.x * unitScale,
                                    edge.y * unitScale,
                                    edge.z * unitScale})};
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

bool importNurbsSurface(const ON_Surface &source,
                        double unitScale,
                        double tolerance,
                        Shape::NurbsSurface3D *destination,
                        QString *reason)
{
    if (destination == nullptr) {
        return false;
    }
    ON_NurbsSurface converted;
    if (const ON_NurbsSurface *nurbs = ON_NurbsSurface::Cast(&source)) {
        converted = *nurbs;
    } else if (source.GetNurbForm(converted, tolerance) <= 0) {
        if (reason != nullptr) {
            *reason = QStringLiteral("surface has no NURBS representation");
        }
        return false;
    }
    if (!converted.IsValid() || converted.Dimension() != 3 ||
        converted.CVCount(0) < 2 || converted.CVCount(1) < 2 ||
        converted.Order(0) < 2 || converted.Order(1) < 2) {
        if (reason != nullptr) {
            *reason = QStringLiteral("surface has invalid or unsupported NURBS data");
        }
        return false;
    }

    Shape::NurbsSurface3D result;
    result.degreeU = converted.Order(0) - 1;
    result.degreeV = converted.Order(1) - 1;
    result.orderU = converted.Order(0);
    result.orderV = converted.Order(1);
    result.controlVertexCountU = converted.CVCount(0);
    result.controlVertexCountV = converted.CVCount(1);
    result.rational = converted.IsRational();
    const qint64 controlPointCount = qint64(result.controlVertexCountU) *
                                     result.controlVertexCountV;
    result.controlPoints.reserve(static_cast<int>(controlPointCount));
    result.weights.reserve(static_cast<int>(controlPointCount));
    for (int uIndex = 0; uIndex < result.controlVertexCountU; ++uIndex) {
        for (int vIndex = 0; vIndex < result.controlVertexCountV; ++vIndex) {
            ON_3dPoint point;
            if (!converted.GetCV(uIndex, vIndex, point) ||
                !std::isfinite(point.x) || !std::isfinite(point.y) ||
                !std::isfinite(point.z)) {
                if (reason != nullptr) {
                    *reason = QStringLiteral("surface has a non-finite control vertex");
                }
                return false;
            }
            const double weight = result.rational
                                     ? converted.Weight(uIndex, vIndex)
                                     : 1.0;
            if (!std::isfinite(weight) || weight <= 0.0) {
                if (reason != nullptr) {
                    *reason = QStringLiteral("surface has a non-positive rational weight");
                }
                return false;
            }
            result.controlPoints.append({point.x * unitScale,
                                         point.y * unitScale,
                                         point.z * unitScale});
            result.weights.append(weight);
        }
    }
    for (int index = 0; index < converted.KnotCount(0); ++index) {
        result.knotsU.append(converted.Knot(0, index));
    }
    for (int index = 0; index < converted.KnotCount(1); ++index) {
        result.knotsV.append(converted.Knot(1, index));
    }
    QString validationError;
    if (!validateNurbsSurface(result, &validationError)) {
        if (reason != nullptr) {
            *reason = QStringLiteral("surface NURBS data is invalid: %1")
                          .arg(validationError);
        }
        return false;
    }
    *destination = std::move(result);
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
                !std::isfinite(point->point.z)) {
                reason = QStringLiteral("point has non-finite coordinates");
            } else {
                importedShape.geometryType = GeometryType::Point;
                importedShape.workPlane = WorkPlane::XY;
                importedShape.workPlaneOffset = point->point.z * unitScale;
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
        } else if (const ON_Surface *surface = ON_Surface::Cast(geometry)) {
            importedShape.geometryType = GeometryType::NurbsSurface;
            if (!importNurbsSurface(*surface,
                                    unitScale,
                                    planarTolerance,
                                    &importedShape.nurbsSurface,
                                    &reason)) {
                // The helper reports why the source surface was rejected.
            }
        } else if (ON_InstanceRef::Cast(geometry) != nullptr) {
            reason = QStringLiteral("block instances are not supported yet");
        } else {
            reason = QStringLiteral("meshes, breps, and annotations are not supported yet");
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
                     : QStringLiteral("No supported curves, NURBS surfaces, or points were found in the Rhino 3DM file"));
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
