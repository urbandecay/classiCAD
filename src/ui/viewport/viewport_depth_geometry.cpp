/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "viewport_depth_geometry.h"

#include "core/geometry/shape_mapping.h"

#include "core/geometry/curve_evaluator.h"
#include "core/geometry/nurbs_surface.h"
#include "core/geometry/nurbs_surface_evaluator.h"
#include "core/geometry/nurbs_surface_tessellator.h"
#include "services/sampling/curve_sampler.h"
#include "viewport_picture_visibility.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QElapsedTimer>
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
    if (shape.pictureImageDepth != PictureImageDepth::Default ||
        shape.pictureUseAlphaBlend) {
        return;
    }
    const QImage image = shape.pictureImage.convertToFormat(
        QImage::Format_RGBA8888);
    if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
        return;
    }

    constexpr int maximumMaskDimension = 256;
    const int maskWidth = std::min(image.width(), maximumMaskDimension);
    const int maskHeight = std::min(image.height(), maximumMaskDimension);
    QVector<uchar> alphaPassingCells(maskWidth * maskHeight, 1);
    for (int rowIndex = 0; rowIndex < image.height(); ++rowIndex) {
        const uchar *pixels = image.constScanLine(rowIndex);
        const int maskRow = rowIndex * maskHeight / image.height();
        for (int column = 0; column < image.width(); ++column) {
            if (pixels[column * 4 + 3] <
                qRound(kWorkbenchImageEmptyAlphaCutoff * 255.0f)) {
                const int maskColumn = column * maskWidth / image.width();
                alphaPassingCells[maskRow * maskWidth + maskColumn] = 0;
            }
        }
    }

    // Build a compact mask mesh: contiguous alpha-passing pixels in each row
    // are merged vertically when their horizontal span matches. The bounded
    // mask marks a cell as depth-writing only if every source pixel in it
    // passes Blender's opaque-image alpha cutoff.
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
                   alphaPassingCells[rowIndex * maskWidth + column] == 0) {
                ++column;
            }
            const int firstOpaqueColumn = column;
            while (column < maskWidth &&
                   alphaPassingCells[rowIndex * maskWidth + column] != 0) {
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
            Point3D localPoint;
            if (!evaluateNurbsPoint3D(curve, parameter, &localPoint)) {
                return;
            }
            const Point3D world = workPlaneFramePointToWorld(
                {localPoint.x, localPoint.y}, localPoint.z, frame);
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
                                 int faceIndex = 0,
                                 const PreparedNurbsSurfaceTessellation::Options &options = {},
                                 bool reverseOrientation = false,
                                 bool collectBuildTimings = false)
{
    if (geometry == nullptr) {
        return;
    }
    const int faceVertexStart = geometry->surfaceVertices.size();
    ViewportDepthGeometry::SurfaceBuildTiming timing;
    timing.faceIndex = faceIndex;
    timing.degreeU = surface.degreeU;
    timing.degreeV = surface.degreeV;
    timing.controlVertexCountU = surface.controlVertexCountU;
    timing.controlVertexCountV = surface.controlVertexCountV;
    timing.trimLoopCount = int(surface.trimLoops.size());

    PreparedNurbsSurfaceTessellation localTessellation;
    QSharedPointer<const PreparedNurbsSurfaceTessellation> cachedTessellation;
    const PreparedNurbsSurfaceTessellation *tessellation = nullptr;
    SurfaceTessellationCache::AcquisitionStats acquisitionStats;
    QElapsedTimer acquisitionTimer;
    if (collectBuildTimings) {
        acquisitionTimer.start();
    }
    if (cache != nullptr) {
        cachedTessellation = cache->acquire(
            objectId, geometryRevision, surface, faceIndex, options,
            collectBuildTimings ? &acquisitionStats : nullptr);
        tessellation = cachedTessellation.data();
    } else if (localTessellation.prepare(surface, options)) {
        tessellation = &localTessellation;
        if (collectBuildTimings) {
            acquisitionStats.prepareMicroseconds =
                acquisitionTimer.nsecsElapsed() / 1000;
        }
    }
    if (collectBuildTimings) {
        timing.acquisitionMicroseconds = acquisitionTimer.nsecsElapsed() / 1000;
        timing.tessellationPrepareMicroseconds =
            acquisitionStats.prepareMicroseconds;
        timing.topologyUpdateMicroseconds =
            acquisitionStats.topologyUpdateMicroseconds;
        timing.preparation = acquisitionStats.preparation;
        timing.cachePath = acquisitionStats.path;
    }
    if (tessellation == nullptr) {
        if (collectBuildTimings) {
            timing.tessellationStrategy =
                PreparedNurbsSurfaceTessellation::Strategy::Failed;
            geometry->surfaceBuildTimings.append(timing);
        }
        return;
    }
    const QVector<QPointF> &textureParameters =
        tessellation->vertexTextureParameters();
    const QVector<double> fullKnotsU = expandedNurbsSurfaceKnotVector(surface.knotsU);
    const QVector<double> fullKnotsV = expandedNurbsSurfaceKnotVector(surface.knotsV);
    const bool textureParametersValid =
        textureParameters.size() == tessellation->vertices().size() &&
        fullKnotsU.size() > surface.degreeU &&
        fullKnotsU.size() > surface.controlVertexCountU &&
        fullKnotsV.size() > surface.degreeV &&
        fullKnotsV.size() > surface.controlVertexCountV &&
        fullKnotsU[surface.controlVertexCountU] > fullKnotsU[surface.degreeU] &&
        fullKnotsV[surface.controlVertexCountV] > fullKnotsV[surface.degreeV];
    timing.tessellationValid = tessellation->isValid();
    timing.tessellationStrategy = tessellation->strategy();
    timing.sampledVertexCount = tessellation->vertices().size();
    timing.triangleCount = tessellation->triangles().size();

    QElapsedTimer wireframeTimer;
    if (collectBuildTimings) {
        wireframeTimer.start();
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
    timing.wireSegmentCount = wireSegmentCount;
    if (collectBuildTimings) {
        timing.wireframeMicroseconds = wireframeTimer.nsecsElapsed() / 1000;
    }
    // A degree-one swept profile is a set of planar panels. Keep each profile
    // knot as a hard edge; curved surfaces still smooth tessellation normals.
    const bool piecewiseLinearExtrusion =
        surface.degreeU == 1 && surface.degreeV == 1 &&
        surface.controlVertexCountV == 2 && surface.trimLoops.isEmpty();
    const float smoothCreaseCosine = piecewiseLinearExtrusion
                                         ? 0.99999f
                                         : 0.70710678f;
    const auto normalizedNormal = [](const QVector3D &value,
                                     const QVector3D &fallback) {
        const float lengthSquared = value.lengthSquared();
        return std::isfinite(lengthSquared) && lengthSquared > 1.0e-20f
                   ? value / std::sqrt(lengthSquared)
                   : fallback;
    };
    QElapsedTimer normalPreparationTimer;
    if (collectBuildTimings) {
        normalPreparationTimer.start();
    }
    QVector<QVector3D> triangleNormals;
    triangleNormals.reserve(tessellation->triangles().size());
    QVector<QVector<int>> trianglesAtVertex(tessellation->vertices().size());
    for (int triangleIndex = 0;
         triangleIndex < tessellation->triangles().size();
         ++triangleIndex) {
        const PreparedNurbsSurfaceTessellation::Triangle &triangle =
            tessellation->triangles()[triangleIndex];
        const int firstVertex = triangle[0];
        const int secondVertex = triangle[reverseOrientation ? 2 : 1];
        const int thirdVertex = triangle[reverseOrientation ? 1 : 2];
        const Point3D &a = tessellation->vertices()[firstVertex];
        const Point3D &b = tessellation->vertices()[secondVertex];
        const Point3D &c = tessellation->vertices()[thirdVertex];
        const QVector3D ab(static_cast<float>(b.x - a.x),
                           static_cast<float>(b.y - a.y),
                           static_cast<float>(b.z - a.z));
        const QVector3D ac(static_cast<float>(c.x - a.x),
                           static_cast<float>(c.y - a.y),
                           static_cast<float>(c.z - a.z));
        const QVector3D normal = QVector3D::crossProduct(ab, ac);
        triangleNormals.append(normal);
        trianglesAtVertex[firstVertex].append(triangleIndex);
        trianglesAtVertex[secondVertex].append(triangleIndex);
        trianglesAtVertex[thirdVertex].append(triangleIndex);
    }
    QVector<QVector3D> evaluatedNormals(tessellation->vertices().size());
    QVector<quint8> evaluatedNormalValid(tessellation->vertices().size(), 0);
    if (textureParametersValid && !piecewiseLinearExtrusion) {
        PreparedNurbsSurfaceEvaluator evaluator;
        if (evaluator.prepare(surface)) {
            for (int vertexIndex = 0;
                 vertexIndex < textureParameters.size();
                 ++vertexIndex) {
                Point3D derivativeU{};
                Point3D derivativeV{};
                const QPointF parameter = textureParameters[vertexIndex];
                if (!evaluator.evaluateDerivatives(parameter.x(), parameter.y(),
                                                   &derivativeU, &derivativeV)) {
                    continue;
                }
                QVector3D normal = QVector3D::crossProduct(
                    asVector(derivativeU), asVector(derivativeV));
                if (reverseOrientation) {
                    normal = -normal;
                }
                if (!std::isfinite(normal.lengthSquared()) ||
                    normal.lengthSquared() <= 1.0e-20f) {
                    continue;
                }
                const int adjacentTriangle =
                    trianglesAtVertex[vertexIndex].isEmpty()
                        ? -1
                        : trianglesAtVertex[vertexIndex].front();
                if (adjacentTriangle >= 0 &&
                    QVector3D::dotProduct(
                        normal, triangleNormals[adjacentTriangle]) < 0.0f) {
                    normal = -normal;
                }
                evaluatedNormals[vertexIndex] = normalizedNormal(
                    normal, QVector3D(0.0f, 0.0f, 1.0f));
                evaluatedNormalValid[vertexIndex] = 1;
            }
        }
    }
    if (collectBuildTimings) {
        timing.normalPreparationMicroseconds =
            normalPreparationTimer.nsecsElapsed() / 1000;
    }

    QElapsedTimer triangleExpansionTimer;
    if (collectBuildTimings) {
        triangleExpansionTimer.start();
    }
    geometry->surfaceVertices.reserve(
        geometry->surfaceVertices.size() + tessellation->triangles().size() * 3);
    geometry->surfaceTextureCoordinates.reserve(
        geometry->surfaceTextureCoordinates.size() +
        tessellation->triangles().size() * 3);
    geometry->surfaceNormals.reserve(
        geometry->surfaceNormals.size() + tessellation->triangles().size() * 3);
    for (int triangleIndex = 0;
         triangleIndex < tessellation->triangles().size();
         ++triangleIndex) {
        const PreparedNurbsSurfaceTessellation::Triangle &triangle =
            tessellation->triangles()[triangleIndex];
        const QVector3D faceNormal = normalizedNormal(
            triangleNormals[triangleIndex], QVector3D(0.0f, 0.0f, 1.0f));
        const int orderedVertices[] = {
            triangle[0], triangle[reverseOrientation ? 2 : 1],
            triangle[reverseOrientation ? 1 : 2]};
        for (const int vertexIndex : orderedVertices) {
            geometry->surfaceVertices.append(
                asVector(tessellation->vertices()[vertexIndex]));
            if (textureParametersValid) {
                const QPointF parameter = textureParameters[vertexIndex];
                geometry->surfaceTextureCoordinates.append(QPointF(
                    (parameter.x() - fullKnotsU[surface.degreeU]) /
                        (fullKnotsU[surface.controlVertexCountU] -
                         fullKnotsU[surface.degreeU]),
                    (parameter.y() - fullKnotsV[surface.degreeV]) /
                        (fullKnotsV[surface.controlVertexCountV] -
                         fullKnotsV[surface.degreeV])));
            }
            if (evaluatedNormalValid[vertexIndex]) {
                geometry->surfaceNormals.append(
                    evaluatedNormals[vertexIndex]);
                continue;
            }
            QVector3D normal;
            for (const int adjacentTriangle : trianglesAtVertex[vertexIndex]) {
                const QVector3D &adjacentNormal =
                    triangleNormals[adjacentTriangle];
                if (adjacentNormal.lengthSquared() <= 1.0e-20f) {
                    continue;
                }
                const QVector3D unitAdjacentNormal = normalizedNormal(
                    adjacentNormal, faceNormal);
                if (QVector3D::dotProduct(faceNormal, unitAdjacentNormal) >=
                    smoothCreaseCosine) {
                    normal += adjacentNormal;
                }
            }
            geometry->surfaceNormals.append(normalizedNormal(normal,
                                                             faceNormal));
        }
    }
    if (collectBuildTimings) {
        timing.triangleExpansionMicroseconds =
            triangleExpansionTimer.nsecsElapsed() / 1000;
        geometry->surfaceBuildTimings.append(timing);
    }
    geometry->surfaceFaceRanges.append(
        {faceIndex, faceVertexStart,
         int(geometry->surfaceVertices.size()) - faceVertexStart});
}

void writeCurve(QDataStream &stream, const Shape::NurbsCurve2D &curve)
{
    stream << qint32(curve.dimension) << qint32(curve.degree)
           << qint32(curve.order) << quint8(curve.rational ? 1 : 0)
           << qint32(curve.controlPoints.size());
    for (const QPointF &point : curve.controlPoints) {
        stream << double(point.x()) << double(point.y());
    }
    stream << qint32(curve.normalCoordinates.size());
    for (double coordinate : curve.normalCoordinates) {
        stream << coordinate;
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

quint64 shadowMeshEdgeKey(int first, int second)
{
    const quint32 low = static_cast<quint32>(std::min(first, second));
    const quint32 high = static_cast<quint32>(std::max(first, second));
    return (quint64(low) << 32u) | high;
}

struct ShadowMeshEdgeUse {
    int count = 0;
    int orientation = 0;
    int firstFrom = -1;
    int firstTo = -1;
};

bool orientAndValidateClosedShadowMesh(
    ViewportDepthGeometry::ShadowCasterMesh *mesh)
{
    if (mesh == nullptr || mesh->vertices.isEmpty() ||
        mesh->triangles.isEmpty()) {
        return false;
    }
    QHash<quint64, ShadowMeshEdgeUse> edges;
    const QVector3D origin = mesh->vertices.first();
    QVector3D minimum(std::numeric_limits<float>::max(),
                      std::numeric_limits<float>::max(),
                      std::numeric_limits<float>::max());
    QVector3D maximum(std::numeric_limits<float>::lowest(),
                      std::numeric_limits<float>::lowest(),
                      std::numeric_limits<float>::lowest());
    for (const QVector3D &point : mesh->vertices) {
        if (!std::isfinite(point.x()) || !std::isfinite(point.y()) ||
            !std::isfinite(point.z())) {
            return false;
        }
        minimum.setX(std::min(minimum.x(), point.x()));
        minimum.setY(std::min(minimum.y(), point.y()));
        minimum.setZ(std::min(minimum.z(), point.z()));
        maximum.setX(std::max(maximum.x(), point.x()));
        maximum.setY(std::max(maximum.y(), point.y()));
        maximum.setZ(std::max(maximum.z(), point.z()));
    }
    const qreal meshScale = std::max<qreal>((maximum - minimum).length(),
                                            1.0e-20);
    const qreal minimumNormalLength = meshScale * meshScale * 1.0e-12;
    qreal signedVolumeTimesSix = 0.0;
    for (const auto &triangle : mesh->triangles) {
        for (const int index : triangle) {
            if (index < 0 || index >= mesh->vertices.size()) return false;
        }
        const QVector3D &a = mesh->vertices[triangle[0]];
        const QVector3D &b = mesh->vertices[triangle[1]];
        const QVector3D &c = mesh->vertices[triangle[2]];
        const QVector3D normal = QVector3D::crossProduct(b - a, c - a);
        if (!std::isfinite(normal.lengthSquared()) ||
            normal.length() <= minimumNormalLength) {
            return false;
        }
        const QVector3D relativeA = a - origin;
        const QVector3D relativeB = b - origin;
        const QVector3D relativeC = c - origin;
        signedVolumeTimesSix += QVector3D::dotProduct(
            relativeA, QVector3D::crossProduct(relativeB, relativeC));
        const int directedEdges[][2] = {
            {triangle[0], triangle[1]},
            {triangle[1], triangle[2]},
            {triangle[2], triangle[0]}};
        for (const auto &edge : directedEdges) {
            ShadowMeshEdgeUse &use = edges[shadowMeshEdgeKey(edge[0], edge[1])];
            ++use.count;
            use.orientation += edge[0] < edge[1] ? 1 : -1;
        }
    }
    const qreal volumeTolerance = meshScale * meshScale * meshScale * 1.0e-12;
    if (!std::isfinite(signedVolumeTimesSix) ||
        std::abs(signedVolumeTimesSix) <= volumeTolerance) {
        return false;
    }
    for (const ShadowMeshEdgeUse &edge : edges) {
        if (edge.count != 2 || edge.orientation != 0) return false;
    }
    if (signedVolumeTimesSix < 0.0) {
        for (auto &triangle : mesh->triangles) {
            std::swap(triangle[1], triangle[2]);
        }
    }
    mesh->valid = true;
    return true;
}

bool prepareOpenSurfaceShadowMesh(
    const NurbsSurface3D &surface,
    const SurfaceTessellationCache *surfaceCache,
    ObjectId objectId,
    quint64 geometryRevision,
    const PreparedNurbsSurfaceTessellation::Options &options,
    ViewportDepthGeometry::ShadowCasterMesh *mesh)
{
    if (mesh == nullptr || !validateNurbsSurface(surface)) return false;
    *mesh = {};
    PreparedNurbsSurfaceTessellation localTessellation;
    QSharedPointer<const PreparedNurbsSurfaceTessellation> cachedTessellation;
    const PreparedNurbsSurfaceTessellation *tessellation = nullptr;
    if (surfaceCache != nullptr) {
        cachedTessellation = surfaceCache->acquire(
            objectId, geometryRevision, surface, 0, options);
        tessellation = cachedTessellation.data();
    } else if (localTessellation.prepare(surface, options)) {
        tessellation = &localTessellation;
    }
    if (tessellation == nullptr || !tessellation->isValid() ||
        tessellation->triangles().isEmpty()) {
        return false;
    }

    struct EdgeUse {
        int count = 0;
        int orientation = 0;
    };
    QHash<quint64, EdgeUse> edges;
    QVector<int> boundaryDegree(tessellation->vertices().size(), 0);
    for (const auto &triangle : tessellation->triangles()) {
        for (const int index : triangle) {
            if (index < 0 || index >= tessellation->vertices().size()) {
                return false;
            }
        }
        const Point3D &a = tessellation->vertices()[triangle[0]];
        const Point3D &b = tessellation->vertices()[triangle[1]];
        const Point3D &c = tessellation->vertices()[triangle[2]];
        const Point3D ab{b.x - a.x, b.y - a.y, b.z - a.z};
        const Point3D ac{c.x - a.x, c.y - a.y, c.z - a.z};
        const Point3D normal{ab.y * ac.z - ab.z * ac.y,
                             ab.z * ac.x - ab.x * ac.z,
                             ab.x * ac.y - ab.y * ac.x};
        if (!std::isfinite(normal.x) || !std::isfinite(normal.y) ||
            !std::isfinite(normal.z) ||
            normal.x * normal.x + normal.y * normal.y +
                    normal.z * normal.z <= 1.0e-24) {
            return false;
        }
        const int directedEdges[][2] = {
            {triangle[0], triangle[1]},
            {triangle[1], triangle[2]},
            {triangle[2], triangle[0]}};
        for (const auto &edge : directedEdges) {
            EdgeUse &use = edges[shadowMeshEdgeKey(edge[0], edge[1])];
            ++use.count;
            use.orientation += edge[0] < edge[1] ? 1 : -1;
            if (use.count > 2) return false;
        }
    }
    for (auto it = edges.cbegin(); it != edges.cend(); ++it) {
        if (it->count == 1) {
            const int first = static_cast<int>(it.key() >> 32u);
            const int second = static_cast<int>(it.key() & 0xffffffffu);
            ++boundaryDegree[first];
            ++boundaryDegree[second];
        } else if (it->count != 2 || it->orientation != 0) {
            return false;
        }
    }
    if (std::any_of(boundaryDegree.cbegin(), boundaryDegree.cend(),
                    [](int degree) { return degree != 0 && degree != 2; })) {
        return false;
    }

    mesh->vertices.reserve(tessellation->vertices().size());
    for (const Point3D &point : tessellation->vertices()) {
        mesh->vertices.append(asVector(point));
    }
    mesh->triangles = tessellation->triangles();
    mesh->valid = true;
    return true;
}

bool prepareNurbsSolidShadowMesh(
    const NurbsExtrusionSolid3D &solid,
    const SurfaceTessellationCache *surfaceCache,
    ObjectId objectId,
    quint64 geometryRevision,
    ViewportDepthGeometry::ShadowCasterMesh *mesh)
{
    if (mesh == nullptr || !validateNurbsSolid(solid)) return false;
    *mesh = {};

    if (!solid.boundaryFaces.isEmpty()) {
        // Materialized vertex edits persist a connected set of bilinear face
        // patches. Shadow volumes support the planar quad subset; a deformed
        // non-planar patch falls back to the disclosed depth-map pass.
        QVector<Point3D> points;
        const auto addPoint = [&points](const Point3D &point) -> int {
            int found = -1;
            for (int index = 0; index < points.size(); ++index) {
                const Point3D &existing = points[index];
                const qreal distance = std::hypot(
                    std::hypot(existing.x - point.x, existing.y - point.y),
                    existing.z - point.z);
                if (distance < 1.0e-7) {
                    if (found >= 0) return -1;
                    found = index;
                }
            }
            if (found >= 0) return found;
            points.append(point);
            return static_cast<int>(points.size() - 1);
        };
        for (int faceIndex = 0; faceIndex < solid.boundaryFaces.size();
             ++faceIndex) {
            const NurbsSurface3D &face = solid.boundaryFaces[faceIndex];
            if (face.controlPoints.size() != 4 || face.rational ||
                !face.trimLoops.isEmpty()) {
                return false;
            }
            const Point3D &p0 = face.controlPoints[0];
            const Point3D &p1 = face.controlPoints[1];
            const Point3D &p2 = face.controlPoints[2];
            const Point3D &p3 = face.controlPoints[3];
            const auto subtract = [](const Point3D &a, const Point3D &b) {
                return Point3D{a.x - b.x, a.y - b.y, a.z - b.z};
            };
            const auto cross = [](const Point3D &a, const Point3D &b) {
                return Point3D{a.y * b.z - a.z * b.y,
                               a.z * b.x - a.x * b.z,
                               a.x * b.y - a.y * b.x};
            };
            const auto dot = [](const Point3D &a, const Point3D &b) {
                return a.x * b.x + a.y * b.y + a.z * b.z;
            };
            const auto length = [](const Point3D &a) {
                return std::hypot(a.x, std::hypot(a.y, a.z));
            };
            const Point3D naturalNormal = cross(subtract(p2, p0),
                                                subtract(p1, p0));
            const qreal normalLength = length(naturalNormal);
            const qreal faceScale = std::max({length(subtract(p1, p0)),
                                              length(subtract(p2, p0)),
                                              length(subtract(p3, p0))});
            if (faceScale <= 0.0 ||
                normalLength <= faceScale * faceScale * 1.0e-12 ||
                std::abs(dot(subtract(p3, p0), naturalNormal)) /
                        normalLength > faceScale * 1.0e-8) {
                return false;
            }
            const Point3D firstNormal = cross(subtract(p2, p0),
                                              subtract(p3, p0));
            const Point3D secondNormal = cross(subtract(p3, p0),
                                               subtract(p1, p0));
            if (dot(firstNormal, secondNormal) <= 0.0) return false;

            int corner[4] = {};
            for (int index = 0; index < 4; ++index) {
                corner[index] = addPoint(face.controlPoints[index]);
                if (corner[index] < 0) return false;
            }
            const bool reversed = solid.boundaryFaceReversed[faceIndex];
            const auto appendTriangle = [&](int a, int b, int c) {
                mesh->triangles.append(reversed
                    ? std::array<int, 3>{a, c, b}
                    : std::array<int, 3>{a, b, c});
            };
            appendTriangle(corner[0], corner[2], corner[3]);
            appendTriangle(corner[0], corner[3], corner[1]);
        }
        mesh->vertices.reserve(points.size());
        for (const Point3D &point : points) mesh->vertices.append(asVector(point));
        return orientAndValidateClosedShadowMesh(mesh);
    }

    PreparedNurbsSurfaceTessellation localTessellation;
    QSharedPointer<const PreparedNurbsSurfaceTessellation> cachedTessellation;
    const PreparedNurbsSurfaceTessellation *tessellation = nullptr;
    if (surfaceCache != nullptr) {
        cachedTessellation = surfaceCache->acquire(
            objectId, geometryRevision, solid.baseSurface, 0);
        tessellation = cachedTessellation.data();
    } else if (localTessellation.prepare(solid.baseSurface)) {
        tessellation = &localTessellation;
    }
    if (tessellation == nullptr || !tessellation->isValid() ||
        tessellation->triangles().isEmpty()) {
        return false;
    }

    QHash<quint64, ShadowMeshEdgeUse> capEdges;
    for (const auto &triangle : tessellation->triangles()) {
        const int directedEdges[][2] = {
            {triangle[0], triangle[1]},
            {triangle[1], triangle[2]},
            {triangle[2], triangle[0]}};
        for (const auto &edge : directedEdges) {
            ShadowMeshEdgeUse &use = capEdges[
                shadowMeshEdgeKey(edge[0], edge[1])];
            if (use.count == 0) {
                use.firstFrom = edge[0];
                use.firstTo = edge[1];
            }
            ++use.count;
            use.orientation += edge[0] < edge[1] ? 1 : -1;
        }
    }
    QVector<ShadowMeshEdgeUse> boundaryEdges;
    QVector<int> boundaryDegree(tessellation->vertices().size(), 0);
    for (const ShadowMeshEdgeUse &edge : capEdges) {
        if (edge.count == 1) {
            boundaryEdges.append(edge);
            ++boundaryDegree[edge.firstFrom];
            ++boundaryDegree[edge.firstTo];
        } else if (edge.count != 2 || edge.orientation != 0) {
            return false;
        }
    }
    if (boundaryEdges.size() < 3 ||
        std::any_of(boundaryDegree.cbegin(), boundaryDegree.cend(),
                    [](int degree) { return degree != 0 && degree != 2; })) {
        return false;
    }

    const int capVertexCount = tessellation->vertices().size();
    mesh->vertices.reserve(capVertexCount * 2);
    for (const Point3D &point : tessellation->vertices()) {
        mesh->vertices.append(asVector(point));
    }
    for (const Point3D &point : tessellation->vertices()) {
        mesh->vertices.append(asVector({point.x + solid.displacement.x,
                                        point.y + solid.displacement.y,
                                        point.z + solid.displacement.z}));
    }
    const bool reverseBottom = nurbsSolidFaceReversed(solid, 0);
    const bool reverseTop = nurbsSolidFaceReversed(solid, 1);
    if (reverseBottom == reverseTop) return false;
    mesh->triangles.reserve(tessellation->triangles().size() * 2 +
                            boundaryEdges.size() * 2);
    for (const auto &triangle : tessellation->triangles()) {
        mesh->triangles.append(reverseBottom
            ? std::array<int, 3>{triangle[0], triangle[2], triangle[1]}
            : triangle);
        const std::array<int, 3> top{
            triangle[0] + capVertexCount,
            triangle[1] + capVertexCount,
            triangle[2] + capVertexCount};
        mesh->triangles.append(reverseTop
            ? std::array<int, 3>{top[0], top[2], top[1]}
            : top);
    }
    for (const ShadowMeshEdgeUse &edge : boundaryEdges) {
        int first = edge.firstFrom;
        int second = edge.firstTo;
        if (reverseBottom) std::swap(first, second);
        const int firstTop = first + capVertexCount;
        const int secondTop = second + capVertexCount;
        mesh->triangles.append({second, first, firstTop});
        mesh->triangles.append({second, firstTop, secondTop});
    }
    return orientAndValidateClosedShadowMesh(mesh);
}

} // namespace

namespace {

void appendShapeDepthGeometry(const Shape &shape,
                              CurveSampler &sampler,
                              ViewportDepthGeometry &geometry,
                              const SurfaceTessellationCache *surfaceCache = nullptr,
                              ObjectId objectId = ObjectId::invalid(),
                              quint64 geometryRevision = 0,
                              bool collectSurfaceBuildTimings = false,
                              bool reverseSurfaceOrientation = false)
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
                geometry.pointWorldRadii.append(0.0f);
            }
            return;
        }
        if (shape.geometryType == GeometryType::PointCloud) {
            if (validatePointCloud(shape.pointCloud)) {
                geometry.pointVertices.reserve(shape.pointCloud.positions.size());
                geometry.precisePointVertices.reserve(
                    shape.pointCloud.positions.size());
                for (qsizetype index = 0;
                     index < shape.pointCloud.positions.size(); ++index) {
                    const Point3D &point = shape.pointCloud.positions[index];
                    geometry.pointVertices.append(asVector(point));
                    geometry.precisePointVertices.append(point);
                    geometry.pointWorldRadii.append(
                        static_cast<float>(shape.pointCloud.radii[index]));
                }
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
            PreparedNurbsSurfaceTessellation::Options displayOptions;
            if (shape.geometryType == GeometryType::NurbsSurface) {
                displayOptions.gridCountU = shape.surfaceDisplayGridU;
                displayOptions.gridCountV = shape.surfaceDisplayGridV;
            }
            for (int index = 0; index < faces.size(); ++index) {
                appendNurbsSurfaceDepthMesh(faces[index],
                                            &geometry,
                                            surfaceCache,
                                            objectId,
                                            geometryRevision, index,
                                            displayOptions,
                                            shape.geometryType ==
                                                    GeometryType::NurbsSolid
                                                ? nurbsSolidFaceReversed(
                                                      shape.nurbsSolid, index)
                                                : reverseSurfaceOrientation,
                                            collectSurfaceBuildTimings);
            }
            if (shape.geometryType == GeometryType::NurbsSolid) {
                prepareNurbsSolidShadowMesh(
                    shape.nurbsSolid, surfaceCache, objectId,
                    geometryRevision, &geometry.shadowCasterMesh);
            } else if (shape.geometryType == GeometryType::NurbsSurface) {
                prepareOpenSurfaceShadowMesh(
                    shape.nurbsSurface, surfaceCache, objectId,
                    geometryRevision, displayOptions,
                    &geometry.shadowCasterMesh);
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
    const SurfaceTessellationCache *surfaceTessellationCache,
    bool collectSurfaceBuildTimings)
{
    ViewportDepthGeometry geometry;
    if (sceneObject.shape.geometryType == GeometryType::Picture &&
        sceneObject.showInFront) {
        return geometry;
    }
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
                             useCache ? sceneObject.geometryRevision : 0,
                             collectSurfaceBuildTimings,
                             sceneObject.reverseSurfaceOrientation);
    return geometry;
}

ViewportDepthGeometry buildViewportDepthGeometry(
    const QVector<ViewportRenderObject> &visibleSceneShapes,
    const SurfaceTessellationCache *surfaceTessellationCache,
    bool applyOffsets)
{
    ViewportDepthGeometry geometry;
    for (const ViewportRenderObject &entry : visibleSceneShapes) {
        if (entry.shape.geometryType == GeometryType::Picture &&
            entry.showInFront) {
            continue;
        }
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
            geometry.pointWorldRadii += prepared.pointWorldRadii;
            geometry.surfaceVertices += prepared.surfaceVertices;
            geometry.surfaceNormals += prepared.surfaceNormals;
            geometry.surfaceTextureCoordinates +=
                prepared.surfaceTextureCoordinates;
            continue;
        }

        const ViewportDepthGeometry prepared =
            buildViewportDepthGeometry(entry, surfaceTessellationCache);
        geometry.lineVertices += prepared.lineVertices;
        geometry.preciseLineVertices += prepared.preciseLineVertices;
        geometry.pointVertices += prepared.pointVertices;
        geometry.pointWorldRadii += prepared.pointWorldRadii;
        geometry.surfaceVertices += prepared.surfaceVertices;
        geometry.surfaceNormals += prepared.surfaceNormals;
        geometry.surfaceTextureCoordinates +=
            prepared.surfaceTextureCoordinates;
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
        stream << qint32(shape.pointCloud.positions.size());
        for (int index = 0; index < shape.pointCloud.positions.size(); ++index) {
            const Point3D &point = shape.pointCloud.positions[index];
            stream << double(point.x) << double(point.y) << double(point.z)
                   << double(shape.pointCloud.radii.value(index, 0.0));
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
        // Presentation metadata partitions overlay depth without rebuilding
        // geometry, but it must still invalidate cached object ranges.
        stream << quint8(entry.showInFront ? 1 : 0);
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
    QByteArray fallbackPayload = viewportDepthGeometryCacheKey(shapes);
    QDataStream fallbackStream(&fallbackPayload, QIODevice::Append);
    fallbackStream << qint32(visibleSceneShapes.size());
    for (const ViewportRenderObject &entry : visibleSceneShapes) {
        fallbackStream << quint8(entry.showInFront ? 1 : 0);
    }
    return QCryptographicHash::hash(fallbackPayload, QCryptographicHash::Sha256);
}

} // namespace classiCAD
