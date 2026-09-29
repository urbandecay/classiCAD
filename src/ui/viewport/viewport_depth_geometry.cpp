/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "viewport_depth_geometry.h"

#include "core/geometry/curve_evaluator.h"
#include "services/sampling/curve_sampler.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>

#include <algorithm>

namespace classiCAD {
namespace {

QVector3D asVector(const Point3D &point)
{
    return {static_cast<float>(point.x),
            static_cast<float>(point.y),
            static_cast<float>(point.z)};
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
    const int samplesPerSpan = curve.degree <= 1 ? 64 : 32;
    constexpr int maximumSampleCount = 2048;
    const int samplesForEachSpan =
        std::max(1,
                 std::min(samplesPerSpan,
                          maximumSampleCount / std::max(1, nonZeroSpans)));

    QVector3D previousPoint;
    bool hasPreviousPoint = false;
    for (int spanIndex = curve.degree;
         spanIndex < curve.controlPoints.size();
         ++spanIndex) {
        const qreal spanStart = fullKnots[spanIndex];
        const qreal spanEnd = fullKnots[spanIndex + 1];
        if (spanEnd <= spanStart) {
            continue;
        }
        for (int sample = 0; sample <= samplesForEachSpan; ++sample) {
            if (spanIndex > curve.degree && sample == 0) {
                continue;
            }
            const qreal fraction = static_cast<qreal>(sample) /
                                   samplesForEachSpan;
            const qreal parameter = spanStart +
                                    (spanEnd - spanStart) * fraction;
            QPointF localPoint;
            if (!evaluateNurbsPoint(curve, parameter, &localPoint)) {
                return;
            }
            const QVector3D worldPoint = asVector(workPlanePointToWorld(
                localPoint, shape.workPlane, shape.workPlaneOffset));
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
                geometry.pointVertices.append(asVector(workPlanePointToWorld(
                    shape.points.first(), shape.workPlane, shape.workPlaneOffset)));
            }
            return;
        }
        if (shape.geometryType == GeometryType::Picture) {
            const QVector<QPointF> corners = pictureFrameCorners(shape);
            if (shape.pictureImage.isNull() || corners.size() != 4) {
                return;
            }
            const QVector<QVector3D> worldCorners{
                asVector(workPlanePointToWorld(corners[0], shape.workPlane,
                                               shape.workPlaneOffset)),
                asVector(workPlanePointToWorld(corners[1], shape.workPlane,
                                               shape.workPlaneOffset)),
                asVector(workPlanePointToWorld(corners[2], shape.workPlane,
                                               shape.workPlaneOffset)),
                asVector(workPlanePointToWorld(corners[3], shape.workPlane,
                                               shape.workPlaneOffset)),
            };
            geometry.surfaceVertices.append(worldCorners[0]);
            geometry.surfaceVertices.append(worldCorners[1]);
            geometry.surfaceVertices.append(worldCorners[2]);
            geometry.surfaceVertices.append(worldCorners[0]);
            geometry.surfaceVertices.append(worldCorners[2]);
            geometry.surfaceVertices.append(worldCorners[3]);
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
        for (const QPointF &point : shape.points) {
            stream << double(point.x()) << double(point.y());
        }
        writeCurve(stream, shape.nurbs);
        stream << qint32(shape.components.size());
        for (const Shape::NurbsCurve2D &curve : shape.components) {
            writeCurve(stream, curve);
        }
        // Picture geometry depends on the frame and aspect ratio, not pixels.
        stream << quint8(shape.pictureImage.isNull() ? 1 : 0)
               << qint32(shape.pictureImage.width())
               << qint32(shape.pictureImage.height());
    }
    return QCryptographicHash::hash(payload, QCryptographicHash::Sha256);
}

} // namespace classiCAD
