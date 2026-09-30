/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "viewport_depth_geometry.h"

#include "core/geometry/curve_evaluator.h"
#include "services/sampling/curve_sampler.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QHash>
#include <QIODevice>

#include <algorithm>
#include <array>

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

void appendCurveDepthVertices(const Shape &shape,
                              const Shape::NurbsCurve2D &curve,
                              QVector<QVector3D> *vertices)
{
    if (!validateNurbsCurve(curve)) {
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
        QVector3D previousPoint;
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
            const QVector3D worldPoint = asVector(shapePointToWorld(shape,
                                                                   localPoint));
            if (hasPreviousPoint) {
                vertices->append(previousPoint);
                vertices->append(worldPoint);
            }
            previousPoint = worldPoint;
            hasPreviousPoint = true;
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
                              ViewportDepthGeometry &geometry)
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

        const QVector<Shape::NurbsCurve2D> curves = sampler.curvesForShape(shape);
        for (const Shape::NurbsCurve2D &curve : curves) {
            appendCurveDepthVertices(shape,
                                     curve,
                                     &geometry.lineVertices);
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
        stream << qint32(shape.components.size());
        for (const Shape::NurbsCurve2D &curve : shape.components) {
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

} // namespace classiCAD
