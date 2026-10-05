/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "viewport_depth_geometry.h"

#include "core/geometry/shape_mapping.h"

#include "core/geometry/curve_evaluator.h"
#include "core/geometry/nurbs_surface.h"
#include "core/geometry/nurbs_surface_tessellator.h"
#include "services/sampling/curve_sampler.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QHash>
#include <QIODevice>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace classiCAD {
namespace {

QVector3D asVector(const Point3D &point)
{
    return {static_cast<float>(point.x),
            static_cast<float>(point.y),
            static_cast<float>(point.z)};
}

struct OpaqueImageRun {
    int firstColumn = 0;
    int pastLastColumn = 0;
    int firstRow = 0;
    int pastLastRow = 0;
};

quint64 imageRunKey(int firstColumn, int pastLastColumn)
{
    return (quint64(static_cast<quint32>(firstColumn)) << 32) |
           static_cast<quint32>(pastLastColumn);
}

QPointF picturePointAt(const QVector<QPointF> &corners, qreal u, qreal v)
{
    const QPointF top = corners[0] * (1.0 - u) + corners[1] * u;
    const QPointF bottom = corners[3] * (1.0 - u) + corners[2] * u;
    return top * (1.0 - v) + bottom * v;
}

void appendPictureDepthSurface(const Shape &shape,
                               const QVector<QPointF> &corners,
                               ViewportDepthGeometry *geometry)
{
    const QImage image = shape.pictureImage.convertToFormat(
        QImage::Format_RGBA8888);
    if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
        return;
    }

    constexpr int maximumMaskDimension = 256;
    const int maskWidth = std::min(image.width(), maximumMaskDimension);
    const int maskHeight = std::min(image.height(), maximumMaskDimension);
    QVector<uchar> fullyOpaqueCells(maskWidth * maskHeight, 1);
    for (int rowIndex = 0; rowIndex < image.height(); ++rowIndex) {
        const uchar *pixels = image.constScanLine(rowIndex);
        const int maskRow = rowIndex * maskHeight / image.height();
        for (int column = 0; column < image.width(); ++column) {
            if (pixels[column * 4 + 3] != 255) {
                const int maskColumn = column * maskWidth / image.width();
                fullyOpaqueCells[maskRow * maskWidth + maskColumn] = 0;
            }
        }
    }

    // Build a compact mask mesh: contiguous opaque pixels in each row are
    // merged vertically when their horizontal span matches. The bounded mask
    // marks a cell opaque only if every source pixel in it is opaque.
    // Partially transparent pixels therefore never write depth.
    QVector<OpaqueImageRun> activeRuns;
    QVector<OpaqueImageRun> opaqueRectangles;
    for (int rowIndex = 0; rowIndex < maskHeight; ++rowIndex) {
        QHash<quint64, int> activeRunLookup;
        activeRunLookup.reserve(activeRuns.size());
        for (int runIndex = 0; runIndex < activeRuns.size(); ++runIndex) {
            const OpaqueImageRun &run = activeRuns[runIndex];
            activeRunLookup.insert(
                imageRunKey(run.firstColumn, run.pastLastColumn), runIndex);
        }

        QVector<OpaqueImageRun> nextActiveRuns;
        int column = 0;
        while (column < maskWidth) {
            while (column < maskWidth &&
                   fullyOpaqueCells[rowIndex * maskWidth + column] == 0) {
                ++column;
            }
            const int firstOpaqueColumn = column;
            while (column < maskWidth &&
                   fullyOpaqueCells[rowIndex * maskWidth + column] != 0) {
                ++column;
            }
            if (firstOpaqueColumn == column) {
                continue;
            }

            const quint64 key = imageRunKey(firstOpaqueColumn, column);
            const auto activeRun = activeRunLookup.find(key);
            if (activeRun == activeRunLookup.end()) {
                nextActiveRuns.append({firstOpaqueColumn, column,
                                       rowIndex, rowIndex + 1});
                continue;
            }

            OpaqueImageRun continued = activeRuns[activeRun.value()];
            continued.pastLastRow = rowIndex + 1;
            nextActiveRuns.append(continued);
            activeRunLookup.erase(activeRun);
        }

        for (auto run = activeRunLookup.cbegin();
             run != activeRunLookup.cend();
             ++run) {
            opaqueRectangles.append(activeRuns[run.value()]);
        }
        activeRuns = std::move(nextActiveRuns);
    }
    opaqueRectangles += activeRuns;

    const auto sourceBoundary = [](int maskIndex,
                                   int sourceExtent,
                                   int maskExtent) {
        return static_cast<int>((qint64(maskIndex) * sourceExtent +
                                 maskExtent - 1) /
                                maskExtent);
    };
    const qreal imageWidth = image.width();
    const qreal imageHeight = image.height();
    for (const OpaqueImageRun &rectangle : opaqueRectangles) {
        const qreal left = sourceBoundary(rectangle.firstColumn,
                                          image.width(), maskWidth) /
                           imageWidth;
        const qreal right = sourceBoundary(rectangle.pastLastColumn,
                                           image.width(), maskWidth) /
                            imageWidth;
        const qreal top = sourceBoundary(rectangle.firstRow,
                                         image.height(), maskHeight) /
                          imageHeight;
        const qreal bottom = sourceBoundary(rectangle.pastLastRow,
                                            image.height(), maskHeight) /
                            imageHeight;
        const std::array<QPointF, 4> localCorners{
            picturePointAt(corners, left, top),
            picturePointAt(corners, right, top),
            picturePointAt(corners, right, bottom),
            picturePointAt(corners, left, bottom),
        };
        std::array<QVector3D, 4> worldCorners;
        for (std::size_t index = 0; index < localCorners.size(); ++index) {
            worldCorners[index] = asVector(shapePointToWorld(shape,
                                                             localCorners[index]));
        }
        geometry->surfaceVertices.append(worldCorners[0]);
        geometry->surfaceVertices.append(worldCorners[1]);
        geometry->surfaceVertices.append(worldCorners[2]);
        geometry->surfaceVertices.append(worldCorners[0]);
        geometry->surfaceVertices.append(worldCorners[2]);
        geometry->surfaceVertices.append(worldCorners[3]);
    }
}

void appendCurveDepthVertices(const Shape::NurbsCurve2D &curve,
                              const WorkPlaneFrame &frame,
                              ViewportDepthGeometry *geometry)
{
    if (geometry == nullptr || !validateNurbsCurve(curve)) {
        return;
    }

    const QVector<double> fullKnots = expandedNurbsKnotVector(curve);
    int nonZeroSpans = 0;
    for (int index = curve.degree; index < curve.controlPoints.size(); ++index) {
        if (fullKnots[index + 1] > fullKnots[index]) {
            ++nonZeroSpans;
        }
    }
    // A degree-one NURBS is exactly linear within each knot span, so its
    // endpoints are sufficient. Higher-degree curves need enough vertices to
    // stay smooth at maximum orthographic zoom. Bound their normal sampling
    // budget; the resulting vertices are cached by the GPU scene renderer.
    const int samplesPerSpan = curve.degree <= 1 ? 1 : 128;
    constexpr int maximumSampleBudget = 8192;
    const int samplesForEachSpan =
        std::max(1,
                 std::min(samplesPerSpan,
                          maximumSampleBudget / std::max(1, nonZeroSpans)));

    for (int spanIndex = curve.degree;
         spanIndex < curve.controlPoints.size();
         ++spanIndex) {
        const qreal spanStart = fullKnots[spanIndex];
        const qreal spanEnd = fullKnots[spanIndex + 1];
        if (spanEnd <= spanStart) {
            continue;
        }
        Point3D previousWorldPoint;
        bool hasPreviousPoint = false;
        for (int sample = 0; sample <= samplesForEachSpan; ++sample) {
            const qreal fraction = static_cast<qreal>(sample) /
                                   samplesForEachSpan;
            const qreal parameter = spanStart +
                                    (spanEnd - spanStart) * fraction;
            QPointF localPoint;
            if (!evaluateNurbsPoint(curve, parameter, &localPoint)) {
                return;
            }
            const Point3D world = workPlaneFramePointToWorld(localPoint, frame);
            if (hasPreviousPoint) {
                geometry->lineVertices.append(asVector(previousWorldPoint));
                geometry->lineVertices.append(asVector(world));
                geometry->preciseLineVertices.append(previousWorldPoint);
                geometry->preciseLineVertices.append(world);
            }
            previousWorldPoint = world;
            hasPreviousPoint = true;
        }
    }
}

bool straightWire(const QVector<Point3D> &points)
{
    if (points.size() < 3) return false;
    const auto &start = points.first();
    const auto &end = points.last();
    const Point3D axis{end.x - start.x, end.y - start.y, end.z - start.z};
    const double squaredLength = axis.x * axis.x + axis.y * axis.y + axis.z * axis.z;
    if (squaredLength == 0) return false;
    const double tolerance = 256 * std::numeric_limits<double>::epsilon() *
        std::max({1.0, std::abs(start.x), std::abs(start.y), std::abs(start.z),
                  std::abs(end.x), std::abs(end.y), std::abs(end.z)});
    double previous = 0;
    for (int i = 1; i + 1 < points.size(); ++i) {
        const auto &point = points[i];
        const Point3D delta{point.x - start.x, point.y - start.y, point.z - start.z};
        const double parameter = (delta.x * axis.x + delta.y * axis.y +
                                  delta.z * axis.z) / squaredLength;
        const Point3D cross{delta.y * axis.z - delta.z * axis.y,
                            delta.z * axis.x - delta.x * axis.z,
                            delta.x * axis.y - delta.y * axis.x};
        const double crossSquared = cross.x * cross.x + cross.y * cross.y + cross.z * cross.z;
        if (!std::isfinite(parameter) || parameter < previous || parameter > 1 ||
            crossSquared > tolerance * tolerance * squaredLength) return false;
        previous = parameter;
    }
    return true;
}

void appendNurbsSurfaceDepthMesh(const Shape::NurbsSurface3D &surface,
                                 ViewportDepthGeometry *geometry,
                                 const SurfaceTessellationCache *cache = nullptr,
                                 ObjectId objectId = ObjectId::invalid(),
                                 quint64 geometryRevision = 0,
                                 int faceIndex = 0)
{
    if (geometry == nullptr) {
        return;
    }
    PreparedNurbsSurfaceTessellation localTessellation;
    QSharedPointer<const PreparedNurbsSurfaceTessellation> cachedTessellation;
    const PreparedNurbsSurfaceTessellation *tessellation = nullptr;
    if (cache != nullptr) {
        cachedTessellation = cache->acquire(objectId, geometryRevision, surface, faceIndex);
        tessellation = cachedTessellation.data();
    } else if (localTessellation.prepare(surface)) {
        tessellation = &localTessellation;
    }
    if (tessellation == nullptr) {
        return;
    }
    int wireSegmentCount = 0;
    QVector<int> wireSteps;
    for (const PreparedNurbsSurfaceTessellation::Polyline &polyline :
         tessellation->wireframe()) {
        const int segments = std::max(0, int(polyline.points.size()) - 1);
        const bool straight = straightWire(polyline.points);
        wireSteps.append(straight ? segments : 1);
        wireSegmentCount += straight ? 1 : segments;
    }
    geometry->lineVertices.reserve(geometry->lineVertices.size() +
                                   wireSegmentCount * 2);
    geometry->preciseLineVertices.reserve(
        geometry->preciseLineVertices.size() + wireSegmentCount * 2);
    int wireIndex = 0;
    for (const PreparedNurbsSurfaceTessellation::Polyline &polyline :
         tessellation->wireframe()) {
        const int step = wireSteps[wireIndex++];
        for (int index = step; index < polyline.points.size(); index += step) {
            const Point3D &start = polyline.points[index - step];
            const Point3D &end = polyline.points[index];
            geometry->lineVertices.append(asVector(start));
            geometry->lineVertices.append(asVector(end));
            geometry->preciseLineVertices.append(start);
            geometry->preciseLineVertices.append(end);
        }
    }
    QVector<QVector3D> vertexNormals(tessellation->vertices().size());
    for (const PreparedNurbsSurfaceTessellation::Triangle &triangle :
         tessellation->triangles()) {
        const Point3D &a = tessellation->vertices()[triangle[0]];
        const Point3D &b = tessellation->vertices()[triangle[1]];
        const Point3D &c = tessellation->vertices()[triangle[2]];
        const QVector3D ab(static_cast<float>(b.x - a.x),
                           static_cast<float>(b.y - a.y),
                           static_cast<float>(b.z - a.z));
        const QVector3D ac(static_cast<float>(c.x - a.x),
                           static_cast<float>(c.y - a.y),
                           static_cast<float>(c.z - a.z));
        const QVector3D normal = QVector3D::crossProduct(ab, ac);
        if (normal.lengthSquared() > 1.0e-20f) {
            vertexNormals[triangle[0]] += normal;
            vertexNormals[triangle[1]] += normal;
            vertexNormals[triangle[2]] += normal;
        }
    }
    for (QVector3D &normal : vertexNormals) {
        if (normal.lengthSquared() > 1.0e-20f) {
            normal.normalize();
        } else {
            normal = QVector3D(0.0f, 0.0f, 1.0f);
        }
    }

    geometry->surfaceVertices.reserve(
        geometry->surfaceVertices.size() + tessellation->triangles().size() * 3);
    geometry->surfaceNormals.reserve(
        geometry->surfaceNormals.size() + tessellation->triangles().size() * 3);
    for (const PreparedNurbsSurfaceTessellation::Triangle &triangle :
         tessellation->triangles()) {
        for (const int vertexIndex : triangle) {
            geometry->surfaceVertices.append(
                asVector(tessellation->vertices()[vertexIndex]));
            geometry->surfaceNormals.append(vertexNormals[vertexIndex]);
        }
    }
}

void writeCurve(QDataStream &stream, const Shape::NurbsCurve2D &curve)
{
    stream << qint32(curve.dimension) << qint32(curve.degree)
           << qint32(curve.order) << quint8(curve.rational ? 1 : 0)
           << qint32(curve.controlPoints.size());
    for (const QPointF &point : curve.controlPoints) {
        stream << double(point.x()) << double(point.y());
    }
    stream << qint32(curve.weights.size());
    for (double weight : curve.weights) {
        stream << weight;
    }
    stream << qint32(curve.knots.size());
    for (double knot : curve.knots) {
        stream << knot;
    }
}

} // namespace

namespace {

void appendShapeDepthGeometry(const Shape &shape,
                              CurveSampler &sampler,
                              ViewportDepthGeometry &geometry,
                              const SurfaceTessellationCache *surfaceCache = nullptr,
                              ObjectId objectId = ObjectId::invalid(),
                              quint64 geometryRevision = 0)
{
        if (isDimensionGeometryType(shape.geometryType)) {
            // Dimension text and leaders are viewport annotations, not scene
            // surfaces; they are drawn in the foreground overlay pass.
            return;
        }
        if (shape.geometryType == GeometryType::Point) {
            if (!shape.points.isEmpty()) {
                geometry.pointVertices.append(asVector(shapePointToWorld(
                    shape, shape.points.first())));
            }
            return;
        }
        if (shape.geometryType == GeometryType::Picture) {
            const QVector<QPointF> corners = pictureFrameCorners(shape);
            if (!shape.pictureImage.isNull() && corners.size() == 4) {
                appendPictureDepthSurface(shape, corners, &geometry);
            }
            return;
        }
        if (shape.geometryType == GeometryType::NurbsSurface ||
            shape.geometryType == GeometryType::NurbsSolid) {
            const auto faces = shapeSurfaceFaces(shape);
            for (int index = 0; index < faces.size(); ++index) {
                appendNurbsSurfaceDepthMesh(faces[index],
                                            &geometry,
                                            surfaceCache,
                                            objectId,
                                            geometryRevision, index);
            }
            return;
        }

        const QVector<Shape::NurbsCurve2D> curves = sampler.curvesForShape(shape);
        for (int index = 0; index < curves.size(); ++index) {
            const WorkPlaneFrame frame =
                shape.geometryType == GeometryType::PolyCurve
                    ? shapeComponentWorkPlaneFrame(shape, index)
                    : shapeWorkPlaneFrame(shape);
            appendCurveDepthVertices(curves[index], frame, &geometry);
        }
}

} // namespace

ViewportDepthGeometry buildViewportDepthGeometry(const Shape &shape)
{
    ViewportDepthGeometry geometry;
    CurveSampler sampler;
    appendShapeDepthGeometry(shape, sampler, geometry);
    return geometry;
}

ViewportDepthGeometry buildViewportDepthGeometry(
    const QVector<Shape> &visibleSceneShapes)
{
    ViewportDepthGeometry geometry;
    CurveSampler sampler;
    for (const Shape &shape : visibleSceneShapes) {
        appendShapeDepthGeometry(shape, sampler, geometry);
    }

    return geometry;
}

ViewportDepthGeometry buildViewportDepthGeometry(
    const ViewportRenderObject &sceneObject,
    const SurfaceTessellationCache *surfaceTessellationCache)
{
    ViewportDepthGeometry geometry;
    CurveSampler sampler;
    const bool useCache = sceneObject.cacheable &&
                          sceneObject.objectId.isValid() &&
                          sceneObject.geometryRevision != 0 &&
                          surfaceTessellationCache != nullptr;
    appendShapeDepthGeometry(sceneObject.shape,
                             sampler,
                             geometry,
                             surfaceTessellationCache,
                             useCache ? sceneObject.objectId : ObjectId::invalid(),
                             useCache ? sceneObject.geometryRevision : 0);
    return geometry;
}

ViewportDepthGeometry buildViewportDepthGeometry(
    const QVector<ViewportRenderObject> &visibleSceneShapes,
    const SurfaceTessellationCache *surfaceTessellationCache,
    bool applyOffsets)
{
    ViewportDepthGeometry geometry;
    for (const ViewportRenderObject &entry : visibleSceneShapes) {
        if (entry.preparedDepthGeometry) {
            ViewportDepthGeometry prepared = *entry.preparedDepthGeometry;
            const Point3D &offset = entry.preparedGeometryOffset;
            if (applyOffsets && (offset.x != 0 || offset.y != 0 || offset.z != 0)) {
                const QVector3D gpuOffset(offset.x, offset.y, offset.z);
                for (auto *vertices : {&prepared.lineVertices, &prepared.pointVertices,
                                       &prepared.surfaceVertices}) {
                    for (auto &point : *vertices) {
                        point += gpuOffset;
                    }
                }
                for (auto &point : prepared.preciseLineVertices) {
                    point.x += offset.x;
                    point.y += offset.y;
                    point.z += offset.z;
                }
            }
            geometry.lineVertices += prepared.lineVertices;
            geometry.preciseLineVertices += prepared.preciseLineVertices;
            geometry.pointVertices += prepared.pointVertices;
            geometry.surfaceVertices += prepared.surfaceVertices;
            geometry.surfaceNormals += prepared.surfaceNormals;
            continue;
        }

        const ViewportDepthGeometry prepared =
            buildViewportDepthGeometry(entry, surfaceTessellationCache);
        geometry.lineVertices += prepared.lineVertices;
        geometry.preciseLineVertices += prepared.preciseLineVertices;
        geometry.pointVertices += prepared.pointVertices;
        geometry.surfaceVertices += prepared.surfaceVertices;
        geometry.surfaceNormals += prepared.surfaceNormals;
    }
    return geometry;
}

QByteArray viewportDepthGeometryCacheKey(
    const QVector<Shape> &visibleSceneShapes)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setFloatingPointPrecision(QDataStream::DoublePrecision);
    stream << qint32(visibleSceneShapes.size());
    for (const Shape &shape : visibleSceneShapes) {
        stream << qint32(static_cast<int>(shape.geometryType))
               << qint32(static_cast<int>(shape.workPlane))
               << double(shape.workPlaneOffset)
               << qint32(shape.points.size());
        const WorkPlaneFrame frame = shapeWorkPlaneFrame(shape);
        stream << double(frame.origin.x) << double(frame.origin.y)
               << double(frame.origin.z)
               << double(frame.xAxis.x) << double(frame.xAxis.y)
               << double(frame.xAxis.z)
               << double(frame.yAxis.x) << double(frame.yAxis.y)
               << double(frame.yAxis.z)
               << double(frame.normal.x) << double(frame.normal.y)
               << double(frame.normal.z);
        for (const QPointF &point : shape.points) {
            stream << double(point.x()) << double(point.y());
        }
        writeCurve(stream, shape.nurbs);
        const auto faces = shapeSurfaceFaces(shape);
        stream << qint32(faces.size());
        for (const Shape::NurbsSurface3D &surface : faces) {
            stream << qint32(surface.dimension)
                   << qint32(surface.degreeU) << qint32(surface.degreeV)
                   << qint32(surface.orderU) << qint32(surface.orderV)
                   << qint32(surface.controlVertexCountU)
                   << qint32(surface.controlVertexCountV)
                   << quint8(surface.rational ? 1 : 0)
                   << qint32(surface.controlPoints.size());
            for (int index = 0; index < surface.controlPoints.size(); ++index) {
                const Point3D &point = surface.controlPoints[index];
                stream << double(point.x) << double(point.y) << double(point.z)
                       << double(surface.weights.value(index, 1.0));
            }
            stream << qint32(surface.knotsU.size());
            for (const double knot : surface.knotsU) {
                stream << knot;
            }
            stream << qint32(surface.knotsV.size());
            for (const double knot : surface.knotsV) {
                stream << knot;
            }
            stream << qint32(surface.trimLoops.size());
            for (const NurbsSurfaceTrimLoop &loop : surface.trimLoops) {
                stream << quint8(loop.isHole ? 1 : 0);
                writeCurve(stream, loop.curve);
            }
        }
        stream << qint32(shape.components.size());
        for (int index = 0; index < shape.components.size(); ++index) {
            const Shape::NurbsCurve2D &curve = shape.components[index];
            const WorkPlaneFrame componentFrame =
                shapeComponentWorkPlaneFrame(shape, index);
            stream << double(componentFrame.origin.x)
                   << double(componentFrame.origin.y)
                   << double(componentFrame.origin.z)
                   << double(componentFrame.xAxis.x)
                   << double(componentFrame.xAxis.y)
                   << double(componentFrame.xAxis.z)
                   << double(componentFrame.yAxis.x)
                   << double(componentFrame.yAxis.y)
                   << double(componentFrame.yAxis.z)
                   << double(componentFrame.normal.x)
                   << double(componentFrame.normal.y)
                   << double(componentFrame.normal.z);
            writeCurve(stream, curve);
        }
        // Picture depth geometry also depends on pixel alpha, so include the
        // image cache key as well as its dimensions in this cache key.
        stream << quint8(shape.pictureImage.isNull() ? 1 : 0)
               << qint32(shape.pictureImage.width())
               << qint32(shape.pictureImage.height())
               << qint64(shape.pictureImage.cacheKey());
    }
    return QCryptographicHash::hash(payload, QCryptographicHash::Sha256);
}

QByteArray viewportDepthGeometryCacheKey(
    const QVector<ViewportRenderObject> &visibleSceneShapes,
    bool includeOffsets)
{
    bool allCacheable = true;
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream << qint32(visibleSceneShapes.size());
    for (const ViewportRenderObject &entry : visibleSceneShapes) {
        if (!entry.cacheable || !entry.objectId.isValid() ||
            entry.geometryRevision == 0) {
            allCacheable = false;
            break;
        }
        stream << quint64(entry.objectId.value())
               << (entry.preparedGeometryRevision != 0
                       ? entry.preparedGeometryRevision : entry.geometryRevision);
        if (includeOffsets) {
            stream << entry.preparedGeometryOffset.x << entry.preparedGeometryOffset.y
                   << entry.preparedGeometryOffset.z;
        }
    }
    if (allCacheable) {
        return QCryptographicHash::hash(payload, QCryptographicHash::Sha256);
    }

    QVector<Shape> shapes;
    shapes.reserve(visibleSceneShapes.size());
    for (const ViewportRenderObject &entry : visibleSceneShapes) {
        shapes.append(entry.shape);
    }
    return viewportDepthGeometryCacheKey(shapes);
}

} // namespace classiCAD
