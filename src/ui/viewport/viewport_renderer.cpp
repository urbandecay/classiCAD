#include "viewport_renderer.h"
#include "viewport_picture_visibility.h"

#include "core/geometry/curve_construction.h"
#include "core/geometry/shape_mapping.h"

#include "core/geometry/arc_curve_factory.h"
#include "core/geometry/curve_evaluator.h"
#include "core/geometry/nurbs_surface.h"
#include "core/geometry/nurbs_surface_tessellator.h"
#include "core/geometry/polygon_mesh.h"
#include "blender_grid_scale.h"
#include "blender_grid_frame.h"
#include "line_type_style.h"
#include "viewport_depth_geometry.h"
#include "viewport_line_weight.h"
#include "viewport_marker_style.h"
#include "workbench_lighting.h"
#include "viewport_wire_color.h"
#include "workbench_random_color.h"
#include "services/dimensions/dimension_font.h"
#include "services/dimensions/dimension_layout.h"

#include <QFont>
#include <QColorSpace>
#include <QByteArray>
#include <QLineF>
#include <QPainterPath>
#include <QPolygonF>
#include <QTransform>
#include <QImage>
#include <QImageReader>
#include <QBuffer>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QCryptographicHash>
#include <QtEndian>

#include <algorithm>
#include <bit>
#include <array>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>

namespace classiCAD {
namespace {

constexpr int kFadeBucketCount = 256;
constexpr int kGridLineHalfCount = 75;

qreal workbenchViewDepth(const ViewportTransform &transform,
                         const QSize &viewportSize,
                         const Point3D &worldPoint)
{
    const Point3D viewDirection = transform.viewDirection();
    const Point3D forward{-viewDirection.x, -viewDirection.y,
                          -viewDirection.z};
    if (transform.isPerspectiveEnabled()) {
        const Point3D camera = transform.cameraPosition(viewportSize);
        return (worldPoint.x - camera.x) * forward.x +
               (worldPoint.y - camera.y) * forward.y +
               (worldPoint.z - camera.z) * forward.z;
    }
    const Point3D target = transform.viewTarget();
    const qreal clipEnd = transform.cameraPreferences().clipEnd;
    return clipEnd + (worldPoint.x - target.x) * forward.x +
           (worldPoint.y - target.y) * forward.y +
           (worldPoint.z - target.z) * forward.z;
}

bool workbenchPointWithinViewDepth(const ViewportTransform &transform,
                                   const QSize &viewportSize,
                                   const Point3D &worldPoint)
{
    const ViewportCameraPreferences preferences = transform.cameraPreferences();
    const qreal depth = workbenchViewDepth(transform, viewportSize, worldPoint);
    return std::isfinite(depth) && depth >= preferences.clipStart &&
           depth <= (transform.isPerspectiveEnabled()
                         ? preferences.clipEnd : 2.0 * preferences.clipEnd);
}

bool clipWorkbenchSegmentToViewDepth(const ViewportTransform &transform,
                                     const QSize &viewportSize,
                                     Point3D *worldStart,
                                     Point3D *worldEnd)
{
    if (worldStart == nullptr || worldEnd == nullptr) return false;
    const ViewportCameraPreferences preferences = transform.cameraPreferences();
    const qreal nearDepth = preferences.clipStart;
    const qreal farDepth = transform.isPerspectiveEnabled()
                               ? preferences.clipEnd
                               : 2.0 * preferences.clipEnd;
    const qreal startDepth = workbenchViewDepth(transform, viewportSize,
                                                *worldStart);
    const qreal endDepth = workbenchViewDepth(transform, viewportSize,
                                              *worldEnd);
    if (!std::isfinite(startDepth) || !std::isfinite(endDepth)) return false;
    if ((startDepth < nearDepth && endDepth < nearDepth) ||
        (startDepth > farDepth && endDepth > farDepth)) return false;
    const qreal delta = endDepth - startDepth;
    qreal first = 0.0;
    qreal last = 1.0;
    if (std::abs(delta) <= 1.0e-15) {
        return startDepth >= nearDepth && startDepth <= farDepth;
    }
    const qreal nearIntersection = (nearDepth - startDepth) / delta;
    const qreal farIntersection = (farDepth - startDepth) / delta;
    if (delta > 0.0) {
        first = std::max(first, nearIntersection);
        last = std::min(last, farIntersection);
    } else {
        first = std::max(first, farIntersection);
        last = std::min(last, nearIntersection);
    }
    first = std::clamp(first, 0.0, 1.0);
    last = std::clamp(last, 0.0, 1.0);
    if (first > last) return false;
    const Point3D start = *worldStart;
    const Point3D end = *worldEnd;
    *worldStart = {start.x + (end.x - start.x) * first,
                   start.y + (end.y - start.y) * first,
                   start.z + (end.z - start.z) * first};
    *worldEnd = {start.x + (end.x - start.x) * last,
                 start.y + (end.y - start.y) * last,
                 start.z + (end.z - start.z) * last};
    return true;
}

qreal workbenchTrianglePixelCoverage(
    const std::array<QPointF, 3> &points, qreal area, qreal px, qreal py,
    qreal stepX, qreal stepY, qreal *b0, qreal *b1, qreal *b2,
    quint16 *sampleMask = nullptr, int samplesPerAxis = 4)
{
    samplesPerAxis = samplesPerAxis == 1 ? 1 : 4;
    const auto weightsAt = [&](qreal x, qreal y,
                               qreal *w0, qreal *w1, qreal *w2) {
        *w1 = ((x - points[0].x()) * (points[2].y() - points[0].y()) -
               (y - points[0].y()) * (points[2].x() - points[0].x())) / area;
        *w2 = ((points[1].x() - points[0].x()) * (y - points[0].y()) -
               (points[1].y() - points[0].y()) * (x - points[0].x())) / area;
        *w0 = 1.0 - *w1 - *w2;
    };
    weightsAt(px, py, b0, b1, b2);
    const qreal edge0 = std::hypot(points[2].x() - points[1].x(),
                                   points[2].y() - points[1].y());
    const qreal edge1 = std::hypot(points[0].x() - points[2].x(),
                                   points[0].y() - points[2].y());
    const qreal edge2 = std::hypot(points[1].x() - points[0].x(),
                                   points[1].y() - points[0].y());
    const qreal orientation = area > 0.0 ? 1.0 : -1.0;
    const auto ownsTopLeftEdge = [orientation](QPointF a, QPointF b) {
        if (orientation < 0.0) std::swap(a, b);
        const qreal dx = b.x() - a.x();
        const qreal dy = b.y() - a.y();
        return dy < 0.0 || (dy == 0.0 && dx > 0.0);
    };
    const std::array<bool, 3> ownsEdge{
        ownsTopLeftEdge(points[1], points[2]),
        ownsTopLeftEdge(points[2], points[0]),
        ownsTopLeftEdge(points[0], points[1])};
    const qreal coordinateScale = std::max(
        {1.0, std::abs(points[0].x()), std::abs(points[0].y()),
         std::abs(points[1].x()), std::abs(points[1].y()),
         std::abs(points[2].x()), std::abs(points[2].y())});
    const qreal edgeTolerance = coordinateScale * 1.0e-12;
    const auto sampleInside = [&](qreal w0, qreal w1, qreal w2) {
        const std::array<qreal, 3> weights{w0, w1, w2};
        const std::array<qreal, 3> edgeLengths{edge0, edge1, edge2};
        for (size_t edge = 0; edge < weights.size(); ++edge) {
            const qreal distance = std::abs(weights[edge]) *
                                   std::abs(area) / edgeLengths[edge];
            if (weights[edge] < 0.0 && distance > edgeTolerance) return false;
            if (distance <= edgeTolerance && !ownsEdge[edge]) return false;
        }
        return true;
    };
    const qreal nearestEdge = std::min({std::abs(*b0) * std::abs(area) / edge0,
                                        std::abs(*b1) * std::abs(area) / edge1,
                                        std::abs(*b2) * std::abs(area) / edge2});
    if (*b0 >= 0.0 && *b1 >= 0.0 && *b2 >= 0.0 &&
        nearestEdge >= 0.75 * std::max(stepX, stepY)) {
        if (sampleMask != nullptr) {
            *sampleMask = samplesPerAxis == 1 ? quint16(1) : quint16(0xffff);
        }
        return qreal(1.0);
    }
    qreal sum0 = 0.0, sum1 = 0.0, sum2 = 0.0;
    int covered = 0;
    quint16 mask = 0;
    for (int sy = 0; sy < samplesPerAxis; ++sy) {
        for (int sx = 0; sx < samplesPerAxis; ++sx) {
            qreal w0, w1, w2;
            const qreal sampleX = samplesPerAxis == 1
                ? px : px + (sx - 1.5) * stepX / 4.0;
            const qreal sampleY = samplesPerAxis == 1
                ? py : py + (sy - 1.5) * stepY / 4.0;
            weightsAt(sampleX, sampleY, &w0, &w1, &w2);
            if (!sampleInside(w0, w1, w2)) continue;
            sum0 += w0;
            sum1 += w1;
            sum2 += w2;
            mask |= static_cast<quint16>(1u << (sy * 4 + sx));
            ++covered;
        }
    }
    if (sampleMask != nullptr) *sampleMask = mask;
    if (covered == 0) return qreal(0.0);
    *b0 = sum0 / covered;
    *b1 = sum1 / covered;
    *b2 = sum2 / covered;
    return covered / qreal(samplesPerAxis * samplesPerAxis);
}

QVector3D transformWorkbenchMeshNormal(const std::array<double, 16> &matrix,
                                       const Point3D &normal)
{
    const double a = matrix[0], b = matrix[1], c = matrix[2];
    const double d = matrix[4], e = matrix[5], f = matrix[6];
    const double g = matrix[8], h = matrix[9], i = matrix[10];
    const double cofactors[9] = {
        e * i - f * h, f * g - d * i, d * h - e * g,
        c * h - b * i, a * i - c * g, b * g - a * h,
        b * f - c * e, c * d - a * f, a * e - b * d};
    const double determinant = a * cofactors[0] + b * cofactors[1] +
                               c * cofactors[2];
    if (!std::isfinite(determinant) || std::abs(determinant) <= 1.0e-15 ||
        !std::isfinite(normal.x) || !std::isfinite(normal.y) ||
        !std::isfinite(normal.z)) {
        return {};
    }
    QVector3D result(
        static_cast<float>((cofactors[0] * normal.x +
                            cofactors[1] * normal.y +
                            cofactors[2] * normal.z) / determinant),
        static_cast<float>((cofactors[3] * normal.x +
                            cofactors[4] * normal.y +
                            cofactors[5] * normal.z) / determinant),
        static_cast<float>((cofactors[6] * normal.x +
                            cofactors[7] * normal.y +
                            cofactors[8] * normal.z) / determinant));
    if (!std::isfinite(result.x()) || !std::isfinite(result.y()) ||
        !std::isfinite(result.z()) || result.lengthSquared() <= 1.0e-16f) {
        return {};
    }
    return result.normalized();
}

void sampleWorkbenchImage(const QImage &image,
                          const QPointF &uv,
                          const QString &interpolation,
                          const QString &extension,
                          uchar *rgba,
                          float *linearRgb = nullptr,
                          bool srgb = false)
{
    if (image.isNull()) {
        std::fill(rgba, rgba + 4, uchar(0));
        return;
    }
    const bool repeat = extension.compare(QStringLiteral("REPEAT"),
                                         Qt::CaseInsensitive) == 0;
    const bool mirror = extension.compare(QStringLiteral("MIRROR"),
                                         Qt::CaseInsensitive) == 0;
    const bool clip = extension.compare(QStringLiteral("CLIP"),
                                        Qt::CaseInsensitive) == 0;
    const auto texel = [&](int x, int y, int component) -> qreal {
        if (clip && (x < 0 || x >= image.width() || y < 0 || y >= image.height()))
            return 0.0;
        if (mirror) {
            x = (x % (2 * image.width()) + 2 * image.width()) %
                (2 * image.width());
            y = (y % (2 * image.height()) + 2 * image.height()) %
                (2 * image.height());
            if (x >= image.width()) x = 2 * image.width() - 1 - x;
            if (y >= image.height()) y = 2 * image.height() - 1 - y;
        } else if (repeat) {
            x = (x % image.width() + image.width()) % image.width();
            y = (y % image.height() + image.height()) % image.height();
        } else {
            x = std::clamp(x, 0, image.width() - 1);
            y = std::clamp(y, 0, image.height() - 1);
        }
        return image.constScanLine(y)[x * 4 + component];
    };
    const bool closest = interpolation.compare(QStringLiteral("Closest"),
                                               Qt::CaseInsensitive) == 0;
    if (closest) {
        const int x = qFloor(uv.x() * image.width());
        // The QPainter UV adapter flips V from Blender's bottom-up texture
        // convention to QImage's top-down rows. At exact texel boundaries,
        // choose the row below that boundary to match GL_NEAREST after the
        // GPU upload's vertical image flip.
        const int y = qCeil(uv.y() * image.height()) - 1;
        for (int c = 0; c < 4; ++c) {
            rgba[c] = static_cast<uchar>(qRound(texel(x, y, c)));
            if (linearRgb != nullptr && c < 3) {
                const float encoded = rgba[c] / 255.0f;
                linearRgb[c] = srgb
                    ? workbenchSrgbToSceneLinear(encoded) : encoded;
            }
        }
        return;
    }
    const qreal tx = uv.x() * image.width() - 0.5;
    const qreal ty = uv.y() * image.height() - 0.5;
    const int x0 = qFloor(tx), y0 = qFloor(ty);
    const qreal fx = tx - x0, fy = ty - y0;
    for (int c = 0; c < 4; ++c) {
        const qreal topLeft = texel(x0, y0, c);
        const qreal topRight = texel(x0 + 1, y0, c);
        const qreal bottomLeft = texel(x0, y0 + 1, c);
        const qreal bottomRight = texel(x0 + 1, y0 + 1, c);
        const qreal top = topLeft * (1.0 - fx) + topRight * fx;
        const qreal bottom = bottomLeft * (1.0 - fx) + bottomRight * fx;
        const qreal value = top * (1.0 - fy) + bottom * fy;
        rgba[c] = static_cast<uchar>(std::clamp(qRound(value), 0, 255));
        if (linearRgb != nullptr && c < 3) {
            const auto linearized = [&](qreal encoded) {
                const float normalized = static_cast<float>(encoded / 255.0);
                return srgb ? workbenchSrgbToSceneLinear(normalized)
                            : normalized;
            };
            const qreal linearTop = linearized(topLeft) * (1.0 - fx) +
                                    linearized(topRight) * fx;
            const qreal linearBottom = linearized(bottomLeft) * (1.0 - fx) +
                                       linearized(bottomRight) * fx;
            linearRgb[c] = static_cast<float>(
                linearTop * (1.0 - fy) + linearBottom * fy);
        }
    }
}

struct WorkbenchFloatTextureData {
    int width = 0;
    int height = 0;
    QVector<float> pixels;
    bool isValid() const
    {
        return width > 0 && height > 0 &&
               quint64(width) * quint64(height) * 4ULL ==
                   static_cast<quint64>(pixels.size());
    }
};

void sampleWorkbenchFloatImage(const WorkbenchFloatTextureData &image,
                               const QPointF &uv,
                               const QString &interpolation,
                               const QString &extension,
                               float rgba[4])
{
    if (!image.isValid()) {
        std::fill(rgba, rgba + 4, 0.0f);
        return;
    }
    const bool repeat = extension.compare(QStringLiteral("REPEAT"),
                                          Qt::CaseInsensitive) == 0;
    const bool mirror = extension.compare(QStringLiteral("MIRROR"),
                                          Qt::CaseInsensitive) == 0;
    const bool clip = extension.compare(QStringLiteral("CLIP"),
                                        Qt::CaseInsensitive) == 0;
    const auto texel = [&](int x, int y, int component) {
        if (clip && (x < 0 || x >= image.width || y < 0 || y >= image.height))
            return 0.0f;
        if (mirror) {
            x = (x % (2 * image.width) + 2 * image.width) % (2 * image.width);
            y = (y % (2 * image.height) + 2 * image.height) % (2 * image.height);
            if (x >= image.width) x = 2 * image.width - 1 - x;
            if (y >= image.height) y = 2 * image.height - 1 - y;
        } else if (repeat) {
            x = (x % image.width + image.width) % image.width;
            y = (y % image.height + image.height) % image.height;
        } else {
            x = std::clamp(x, 0, image.width - 1);
            y = std::clamp(y, 0, image.height - 1);
        }
        // Blender Image.pixels and GL texture uploads use bottom-up rows;
        // QPainter's source-image UV adapter is top-down, so invert Y here.
        return image.pixels[(y * image.width + x) * 4 + component];
    };
    const bool closest = interpolation.compare(QStringLiteral("Closest"),
                                               Qt::CaseInsensitive) == 0;
    const qreal tx = uv.x() * image.width - 0.5;
    const qreal ty = (1.0 - uv.y()) * image.height - 0.5;
    if (closest) {
        const int x = qFloor(uv.x() * image.width);
        const int y = qFloor((1.0 - uv.y()) * image.height);
        for (int component = 0; component < 4; ++component)
            rgba[component] = texel(x, y, component);
        return;
    }
    const int x0 = qFloor(tx), y0 = qFloor(ty);
    const qreal fx = tx - x0, fy = ty - y0;
    for (int component = 0; component < 4; ++component) {
        const qreal top = texel(x0, y0, component) * (1.0 - fx) +
                          texel(x0 + 1, y0, component) * fx;
        const qreal bottom = texel(x0, y0 + 1, component) * (1.0 - fx) +
                             texel(x0 + 1, y0 + 1, component) * fx;
        rgba[component] = static_cast<float>(top * (1.0 - fy) + bottom * fy);
    }
}

bool sampleWorkbenchFloatUdimImage(
    const QHash<int, WorkbenchFloatTextureData> &tiles, const QPointF &uv,
    const QString &interpolation, const QString &extension, float rgba[4])
{
    if (!std::isfinite(uv.x()) || !std::isfinite(uv.y())) return false;
    const int tileU = qFloor(uv.x());
    // UDIM rows are selected in Blender's bottom-up UV space. Flip V only
    // after selecting the tile, when mapping that tile into image rows.
    const int tileV = qFloor(uv.y());
    const int number = 1001 + tileU + 10 * tileV;
    const auto found = tiles.constFind(number);
    if (tileU < 0 || tileU >= 10 || tileV < 0 || tileV > 99 ||
        found == tiles.cend() || !found->isValid()) return false;
    const QPointF local(uv.x() - tileU, uv.y() - tileV);
    sampleWorkbenchFloatImage(found.value(), local, interpolation, extension, rgba);
    return true;
}

bool sampleWorkbenchUdimImage(const QHash<int, QImage> &tiles,
                             const QPointF &uv,
                             const QString &interpolation,
                             const QString &extension,
                             uchar *rgba,
                             float *linearRgb = nullptr,
                             bool srgb = false)
{
    if (!std::isfinite(uv.x()) || !std::isfinite(uv.y())) {
        rgba[0] = 255;
        rgba[1] = 0;
        rgba[2] = 255;
        rgba[3] = 255;
        return false;
    }
    const qreal tileU = std::floor(uv.x());
    // Select the UDIM row in Blender's UV space. The image sampler performs
    // the V flip after the tile-local coordinate has been calculated.
    const qreal tileV = std::floor(uv.y());
    const qreal tileNumber = 1001.0 + tileU + 10.0 * tileV;
    const bool validTile = tileU >= 0.0 && tileU < 10.0 &&
        tileV >= 0.0 && tileV <= 99.0 &&
        tileNumber >= 1001.0 && tileNumber <= 1999.0;
    const auto tile = validTile
        ? tiles.constFind(static_cast<int>(tileNumber))
        : tiles.cend();
    if (tile == tiles.cend()) {
        rgba[0] = 255;
        rgba[1] = 0;
        rgba[2] = 255;
        rgba[3] = 255;
        return false;
    }
    sampleWorkbenchImage(tile.value(),
                         QPointF(uv.x() - tileU,
                                 1.0 - (uv.y() - tileV)),
                         interpolation, extension, rgba, linearRgb, srgb);
    return true;
}

QColor workbenchRandomColorInternal(ObjectId objectId,
                                    const QString &displayName,
                                    const QString &libraryPath)
{
    return workbenchRandomObjectColor(objectId.value(), displayName,
                                      libraryPath);
}

using FadedLineBins = std::array<QVector<QLineF>, kFadeBucketCount>;

qreal smoothstep(qreal minimum, qreal maximum, qreal value)
{
    if (maximum <= minimum) {
        return value < minimum ? 0.0 : 1.0;
    }
    const qreal amount = std::clamp((value - minimum) / (maximum - minimum),
                                    0.0,
                                    1.0);
    return amount * amount * (3.0 - 2.0 * amount);
}

qreal perspectiveGridFade(const Point3D &worldPoint,
                          const Point3D &cameraPosition,
                          const Point3D &planeNormal,
                          qreal farClipDistance,
                          const BlenderGridAppearance &appearance)
{
    const qreal cameraX = cameraPosition.x - worldPoint.x;
    const qreal cameraY = cameraPosition.y - worldPoint.y;
    const qreal cameraZ = cameraPosition.z - worldPoint.z;
    const qreal distance = std::sqrt(cameraX * cameraX + cameraY * cameraY +
                                     cameraZ * cameraZ);
    if (!std::isfinite(distance) || distance <= 1.0e-12) {
        return 0.0;
    }

    const qreal facing = std::clamp(
        std::abs((cameraX * planeNormal.x + cameraY * planeNormal.y +
                  cameraZ * planeNormal.z) /
                 distance),
        0.0,
        1.0);
    const qreal grazingFade = 1.0 - std::pow(
        1.0 - facing, appearance.grazingFadeExponent);
    const qreal distanceFade =
        1.0 - smoothstep(farClipDistance * appearance.farFadeStart,
                         farClipDistance * appearance.farFadeEnd,
                         distance);
    return grazingFade * distanceFade;
}

template<typename AdditionalFade>
void appendPerspectiveLine(const ViewportTransform &transform,
                           const QSize &viewportSize,
                           WorkPlane plane,
                           qreal planeOffset,
                           const Point3D &cameraPosition,
                           const Point3D &planeNormal,
                           qreal farClipDistance,
                           const QPointF &start,
                           const QPointF &end,
                           qreal opacity,
                           int segmentCount,
                           FadedLineBins &lineBins,
                           const BlenderGridAppearance &appearance,
                           AdditionalFade additionalFade)
{
    if (opacity <= 0.0 || segmentCount < 1) {
        return;
    }

    const Point3D worldStart = workPlanePointToWorld(start, plane, planeOffset);
    const Point3D worldEnd = workPlanePointToWorld(end, plane, planeOffset);
    QPointF projectedStart;
    QPointF projectedEnd;
    const bool projectedWholeLine =
        transform.worldPointToScreen(worldStart, viewportSize, &projectedStart) &&
        transform.worldPointToScreen(worldEnd, viewportSize, &projectedEnd);
    if (projectedWholeLine) {
        constexpr qreal maximumSegmentLengthPixels = 3.0;
        constexpr int maximumSegmentCount = 128;
        const qreal requestedSegmentCount = std::ceil(
            QLineF(projectedStart, projectedEnd).length() /
            maximumSegmentLengthPixels);
        const int screenSegmentCount =
            requestedSegmentCount >= maximumSegmentCount
                ? maximumSegmentCount
                : std::max(1, static_cast<int>(requestedSegmentCount));
        segmentCount = std::clamp(std::max(segmentCount, screenSegmentCount),
                                  1,
                                  maximumSegmentCount);
    }

    qreal screenDistanceAlongLine = 0.0;
    for (int segment = 0; segment < segmentCount; ++segment) {
        const qreal firstT = static_cast<qreal>(segment) / segmentCount;
        const qreal secondT = static_cast<qreal>(segment + 1) / segmentCount;
        const QPointF firstPoint = start + (end - start) * firstT;
        const QPointF secondPoint = start + (end - start) * secondT;
        QPointF firstScreen;
        QPointF secondScreen;
        if (!transform.worldPointToScreen(workPlanePointToWorld(firstPoint,
                                                                plane,
                                                                planeOffset),
                                          viewportSize,
                                          &firstScreen) ||
            !transform.worldPointToScreen(workPlanePointToWorld(secondPoint,
                                                                plane,
                                                                planeOffset),
                                          viewportSize,
                                          &secondScreen)) {
            continue;
        }
        const qreal screenSegmentLength = QLineF(firstScreen, secondScreen).length();
        const qreal screenMidpointDistance =
            screenDistanceAlongLine + screenSegmentLength * 0.5;
        screenDistanceAlongLine += screenSegmentLength;

        const QPointF midpoint = start + (end - start) * ((firstT + secondT) * 0.5);
        const Point3D worldMidpoint = workPlanePointToWorld(midpoint,
                                                            plane,
                                                            planeOffset);
        const qreal alpha = std::clamp(
            opacity * additionalFade(midpoint) *
                perspectiveGridFade(worldMidpoint,
                                    cameraPosition,
                                    planeNormal,
                                    farClipDistance,
                                    appearance),
            0.0,
            1.0);
        if (alpha <= 0.0) {
            continue;
        }
        // Blender stipples fragments below 10% alpha into 4px dashes. Sample
        // that coverage at short screen-space segments to avoid a bright pileup
        // where perspective grid lines converge at the horizon.
        if (appearance.lowAlphaStipple &&
            alpha < appearance.stippleThreshold) {
            const qreal dashPhase = std::fmod(screenMidpointDistance /
                                                  appearance.stippleDashWidth,
                                              1.0);
            if (alpha / appearance.stippleThreshold < dashPhase) {
                continue;
            }
        }
        const int bucket = std::clamp(
            static_cast<int>(std::lround(alpha * (kFadeBucketCount - 1))),
            1,
            kFadeBucketCount - 1);
        lineBins[bucket].append(QLineF(firstScreen, secondScreen));
    }
}

void drawFadedLineBins(QPainter &painter,
                       const FadedLineBins &lineBins,
                       const QColor &color,
                       qreal baseOpacity,
                       bool additiveIterations = true)
{
    constexpr std::array<qreal, 4> additivePassOpacity = {1.0, 0.5, 0.25, 0.125};
    painter.save();
    painter.setCompositionMode(QPainter::CompositionMode_Plus);
    const int passCount = additiveIterations
                              ? static_cast<int>(additivePassOpacity.size())
                              : 1;
    for (int pass = 0; pass < passCount; ++pass) {
        const qreal passOpacity = additivePassOpacity[static_cast<size_t>(pass)];
        for (int bucket = 0; bucket < kFadeBucketCount; ++bucket) {
            const QVector<QLineF> &lines = lineBins[bucket];
            if (lines.isEmpty()) {
                continue;
            }
            QColor fadedColor = color;
            fadedColor.setAlphaF(baseOpacity * passOpacity *
                                 static_cast<qreal>(bucket) /
                                     (kFadeBucketCount - 1));
            painter.setPen(QPen(fadedColor, 1.0));
            painter.drawLines(lines.constData(), lines.size());
        }
    }
    painter.restore();
}

template<typename RadialFade>
void appendOrthographicGridLine(const ViewportTransform &transform,
                                const QSize &viewportSize,
                                WorkPlane plane,
                                qreal planeOffset,
                                const QPointF &start,
                                const QPointF &end,
                                qreal opacity,
                                int segmentCount,
                                FadedLineBins &lineBins,
                                const BlenderGridAppearance &appearance,
                                RadialFade radialFade)
{
    if (opacity <= 0.0 || segmentCount < 1) {
        return;
    }
    QPointF projectedStart;
    QPointF projectedEnd;
    if (transform.worldPointToScreenUnclipped(
            workPlanePointToWorld(start, plane, planeOffset),
            viewportSize,
            &projectedStart) &&
        transform.worldPointToScreenUnclipped(
            workPlanePointToWorld(end, plane, planeOffset),
            viewportSize,
            &projectedEnd)) {
        const qreal desiredCount = std::ceil(QLineF(projectedStart, projectedEnd).length() /
                                             3.0);
        segmentCount = std::clamp(std::max(segmentCount,
                                           static_cast<int>(desiredCount)),
                                  1,
                                  128);
    }

    const Point3D normal = workPlaneNormal(plane);
    const Point3D direction = transform.viewDirection();
    const qreal directionLength = std::sqrt(direction.x * direction.x +
                                            direction.y * direction.y +
                                            direction.z * direction.z);
    const qreal normalLength = std::sqrt(normal.x * normal.x + normal.y * normal.y +
                                         normal.z * normal.z);
    const qreal facing = directionLength > 1.0e-12 && normalLength > 1.0e-12
                             ? std::clamp(std::abs((direction.x * normal.x +
                                                   direction.y * normal.y +
                                                   direction.z * normal.z) /
                                                  (directionLength * normalLength)),
                                          0.0,
                                          1.0)
                             : 1.0;
    const qreal grazingFade = 1.0 - std::pow(
        1.0 - facing, appearance.grazingFadeExponent);
    qreal screenDistanceAlongLine = 0.0;
    for (int segment = 0; segment < segmentCount; ++segment) {
        const qreal firstT = static_cast<qreal>(segment) / segmentCount;
        const qreal secondT = static_cast<qreal>(segment + 1) / segmentCount;
        const QPointF first = start + (end - start) * firstT;
        const QPointF second = start + (end - start) * secondT;
        QPointF firstScreen;
        QPointF secondScreen;
        if (!transform.worldPointToScreenUnclipped(
                workPlanePointToWorld(first, plane, planeOffset),
                viewportSize,
                &firstScreen) ||
            !transform.worldPointToScreenUnclipped(
                workPlanePointToWorld(second, plane, planeOffset),
                viewportSize,
                &secondScreen)) {
            continue;
        }
        const qreal screenLength = QLineF(firstScreen, secondScreen).length();
        const qreal distanceOnLine = screenDistanceAlongLine + screenLength * 0.5;
        screenDistanceAlongLine += screenLength;
        const QPointF midpoint = (first + second) * 0.5;
        const qreal alpha = std::clamp(opacity * radialFade(midpoint) * grazingFade,
                                       0.0,
                                       1.0);
        if (alpha <= 0.0) {
            continue;
        }
        if (appearance.lowAlphaStipple && alpha < appearance.stippleThreshold) {
            const qreal phase = std::fmod(distanceOnLine / appearance.stippleDashWidth,
                                          1.0);
            if (alpha / appearance.stippleThreshold < phase) {
                continue;
            }
        }
        const int bucket = std::clamp(
            static_cast<int>(std::lround(alpha * (kFadeBucketCount - 1))),
            1,
            kFadeBucketCount - 1);
        lineBins[bucket].append(QLineF(firstScreen, secondScreen));
    }
}

} // namespace

QColor workbenchRandomColor(ObjectId objectId,
                            const QString &displayName,
                            const QString &libraryPath)
{
    return workbenchRandomColorInternal(objectId, displayName, libraryPath);
}

namespace {

void observeWorkbenchFragment(
    RasterWorkbenchFragmentSink *sink, const QPainter &painter,
    const QPointF &logicalPixelCenter, const Point3D &worldPosition,
    const QVector3D &worldNormal,
    const Point3D &cameraPosition, const Point3D &viewDirection,
    const QVector3D &sceneLinearRgb, qreal alpha, ObjectId sourceId,
    const ViewportTransform &transform)
{
    if (sink == nullptr) return;
    // Keep per-object fragment capture bounded too. One extra sample is an
    // overflow sentinel consumed by the compositor, which can then select its
    // existing fallback path instead of treating a truncated sink as whole.
    constexpr std::size_t kMaximumCapturedFragments = 4'000'000;
    if (sink->size() > kMaximumCapturedFragments) return;
    const QPointF devicePixel = painter.deviceTransform().map(logicalPixelCenter);
    const qreal depth = -((worldPosition.x - cameraPosition.x) * viewDirection.x +
                          (worldPosition.y - cameraPosition.y) * viewDirection.y +
                          (worldPosition.z - cameraPosition.z) * viewDirection.z);
    qreal oitLinearDepth = std::abs(depth);
    if (!transform.isPerspectiveEnabled()) {
        const ViewportCameraState camera = transform.cameraState();
        const ViewportCameraPreferences preferences =
            transform.cameraPreferences();
        const qreal eyeDistance = std::min(
            preferences.clipEnd,
            std::max<qreal>(1000.0, camera.gridViewDistance));
        // `depth` is the negated camera-forward dot stored in the fragment;
        // the GPU shader uses the opposite signed view-space value and
        // computes offset - signedViewDepth, so add this value here.
        oitLinearDepth = std::max(
            eyeDistance - preferences.clipStart + depth, 0.0);
    }
    if (!std::isfinite(devicePixel.x()) || !std::isfinite(devicePixel.y()) ||
        !std::isfinite(depth) || !std::isfinite(sceneLinearRgb.x()) ||
        !std::isfinite(sceneLinearRgb.y()) || !std::isfinite(sceneLinearRgb.z()) ||
        !std::isfinite(alpha) || !std::isfinite(oitLinearDepth)) {
        return;
    }
    RasterWorkbenchLinearFragment fragment;
    fragment.x = qFloor(devicePixel.x());
    fragment.y = qFloor(devicePixel.y());
    fragment.cameraDepth = static_cast<float>(depth);
    fragment.sceneLinearRgb = {sceneLinearRgb.x(), sceneLinearRgb.y(),
                               sceneLinearRgb.z()};
    fragment.alpha = static_cast<float>(std::clamp<qreal>(alpha, 0.0, 1.0));
    fragment.sourceId = sourceId.value();
    fragment.oitLinearDepth = static_cast<float>(oitLinearDepth);
    const QVector3D normal = workbenchNormalize(worldNormal);
    if (normal.lengthSquared() > 1.0e-12f &&
        std::isfinite(worldPosition.x) && std::isfinite(worldPosition.y) &&
        std::isfinite(worldPosition.z)) {
        fragment.worldPosition = {static_cast<float>(worldPosition.x),
                                  static_cast<float>(worldPosition.y),
                                  static_cast<float>(worldPosition.z)};
        fragment.worldNormal = {normal.x(), normal.y(), normal.z()};
        fragment.hasShadowReceiver = true;
    }
    sink->push_back(fragment);
}

} // namespace

ViewportRenderer::ViewportRenderer(const ViewportTransform &transform,
                                   const CurveHitTester &curveHitTester,
                                   const SurfaceTessellationCache *surfaceTessellationCache)
    : transform_(transform)
    , curveHitTester_(curveHitTester)
    , surfaceTessellationCache_(surfaceTessellationCache)
{
}

void ViewportRenderer::setSmoothCurveDisplay(bool enabled)
{
    smoothCurveDisplay_ = enabled;
}

void ViewportRenderer::setGridAppearance(const BlenderGridAppearance &appearance)
{
    if (isValidBlenderGridAppearance(appearance)) {
        gridAppearance_ = appearance;
    }
}

void ViewportRenderer::setShadingSettings(
    const ViewportShadingSettings &settings)
{
    shadingSettings_ = settings;
}

void ViewportRenderer::setWorkbenchRasterAntialiasingEnabled(bool enabled)
{
    workbenchRasterAntialiasingEnabled_ = enabled;
}

void ViewportRenderer::setSmoothWirePreferences(bool overlay, bool editMode)
{
    smoothWiresOverlay_ = overlay;
    smoothWiresEditMode_ = editMode;
}

void ViewportRenderer::setGridBaseStep(qreal baseGridStep)
{
    if (std::isfinite(baseGridStep) && baseGridStep > 0.0) {
        gridBaseStep_ = baseGridStep;
    }
}

void ViewportRenderer::setArchitecturalDimensionFont(bool enabled)
{
    architecturalDimensionFont_ = enabled;
}

QPointF ViewportRenderer::worldToScreen(const QPointF &world,
                                        const QSize &viewportSize) const
{
    return transform_.worldToScreen(world, viewportSize);
}

QPointF ViewportRenderer::screenToWorld(const QPointF &screen,
                                        const QSize &viewportSize) const
{
    return transform_.screenToWorld(screen, viewportSize);
}

bool ViewportRenderer::isValidNurbsCurve(const Shape::NurbsCurve2D &curve) const
{
    return validateNurbsCurve(curve);
}

bool ViewportRenderer::evaluateNurbsPoint(const Shape::NurbsCurve2D &curve,
                                          qreal parameter,
                                          QPointF *point) const
{
    return classiCAD::evaluateNurbsPoint(curve, parameter, point);
}

void ViewportRenderer::drawGrid(QPainter &painter,
                                const QSize &viewportSize) const
{
    const BlenderGridFrame gridFrame = resolveBlenderGridFrame(transform_,
                                                               viewportSize);
    const WorkPlane plane = gridFrame.plane;
    const qreal planeOffset = gridFrame.planeOffset;
    const bool floorVisible = transform_.isPerspectiveEnabled()
                                  ? gridAppearance_.showFloor
                                  : gridAppearance_.showOrthographicGrid;
    if (!floorVisible) return;
    if (transform_.isPerspectiveEnabled()) {
        const qreal farClipDistance = transform_.cameraPreferences().clipEnd;
        const Point3D cameraPosition = transform_.cameraPosition(viewportSize);
        const Point3D planeNormal = workPlaneNormal(plane);
        const QPointF gridCenter = gridFrame.cameraRelativeOffset;
        const qreal gridFocusDistance = gridFrame.focusDistance;

        const BlenderGridLevelSelection gridLevel = selectBlenderGridLevel(
            gridFocusDistance, false, gridBaseStep_);
        const qreal levelBlend = gridLevel.levelFraction;
        const std::array<qreal, 3> levelOpacities = {
            1.0 - levelBlend,
            1.0,
            1.0,
        };

        std::array<FadedLineBins, 3> lineBins;

        for (int level = 2; level >= 0; --level) {
            const qreal step = blenderGridStepAtLevel(gridLevel, level);
            if (!std::isfinite(step) || step <= 1.0e-9) {
                continue;
            }
            const qreal originU = std::round(gridCenter.x() / step) * step;
            const qreal originV = std::round(gridCenter.y() / step) * step;
            const qreal radius = kGridLineHalfCount * step;
            const qreal layerOpacity = levelOpacities[static_cast<size_t>(level)];
            const auto radialFade = [originU, originV, radius](const QPointF &point) {
                const qreal radialDistance =
                    std::hypot(point.x() - originU, point.y() - originV);
                return 1.0 - std::clamp(radialDistance / radius, 0.0, 1.0);
            };
            const qreal emphasis = level == 0
                                       ? 0.0
                                       : (level == 1 ? 1.0 - levelBlend : 1.0);
            const QColor levelColor = QColor::fromRgbF(
                gridAppearance_.gridColor.redF() * (1.0 - emphasis) +
                    gridAppearance_.emphasisColor.redF() * emphasis,
                gridAppearance_.gridColor.greenF() * (1.0 - emphasis) +
                    gridAppearance_.emphasisColor.greenF() * emphasis,
                gridAppearance_.gridColor.blueF() * (1.0 - emphasis) +
                    gridAppearance_.emphasisColor.blueF() * emphasis,
                gridAppearance_.gridColor.alphaF() * (1.0 - emphasis) +
                    gridAppearance_.emphasisColor.alphaF() * emphasis);
            const qreal lineOpacity = layerOpacity * levelColor.alphaF() *
                                      gridAppearance_.opacity;

            for (int lineIndex = -kGridLineHalfCount;
                 lineIndex <= kGridLineHalfCount;
                 ++lineIndex) {
                const qreal fixedU = originU + lineIndex * step;
                const qreal fixedV = originV + lineIndex * step;
                const qreal varyingExtent = std::sqrt(std::max<qreal>(
                    0.0,
                    radius * radius - lineIndex * lineIndex * step * step));

                appendPerspectiveLine(
                    transform_, viewportSize, plane, planeOffset,
                    cameraPosition, planeNormal, farClipDistance,
                    QPointF(fixedU, originV - varyingExtent),
                    QPointF(fixedU, originV + varyingExtent),
                    lineOpacity, 24,
                    lineBins[static_cast<size_t>(level)],
                    gridAppearance_, radialFade);
                appendPerspectiveLine(
                    transform_, viewportSize, plane, planeOffset,
                    cameraPosition, planeNormal, farClipDistance,
                    QPointF(originU - varyingExtent, fixedV),
                    QPointF(originU + varyingExtent, fixedV),
                    lineOpacity, 24,
                    lineBins[static_cast<size_t>(level)],
                    gridAppearance_, radialFade);
            }
        }

        painter.save();
        painter.setClipRect(QRect(QPoint(0, 0), viewportSize));
        for (int level = 2; level >= 0; --level) {
            const qreal emphasis = level == 0
                                       ? 0.0
                                       : (level == 1 ? 1.0 - levelBlend : 1.0);
            const QColor levelColor = QColor::fromRgbF(
                gridAppearance_.gridColor.redF() * (1.0 - emphasis) +
                    gridAppearance_.emphasisColor.redF() * emphasis,
                gridAppearance_.gridColor.greenF() * (1.0 - emphasis) +
                    gridAppearance_.emphasisColor.greenF() * emphasis,
                gridAppearance_.gridColor.blueF() * (1.0 - emphasis) +
                    gridAppearance_.emphasisColor.blueF() * emphasis,
                gridAppearance_.gridColor.alphaF() * (1.0 - emphasis) +
                    gridAppearance_.emphasisColor.alphaF() * emphasis);
            const qreal levelAlpha = level == 0
                                         ? 1.0 - levelBlend
                                         : 1.0;
            const qreal colorAlpha = levelColor.alphaF() * gridAppearance_.opacity;
            QColor opaqueLevelColor = levelColor;
            opaqueLevelColor.setAlpha(255);
            drawFadedLineBins(painter,
                              lineBins[static_cast<size_t>(level)],
                              opaqueLevelColor,
                              levelAlpha * colorAlpha);
        }
        painter.restore();
        return;
    }

    const BlenderGridLevelSelection gridLevel = selectBlenderGridLevel(
        gridFrame.focusDistance,
        gridFrame.fixedAxisOrthographic,
        gridBaseStep_);
    const qreal levelBlend = gridLevel.levelFraction;
    painter.save();
    painter.setClipRect(QRect(QPoint(0, 0), viewportSize));
    for (int level = 2; level >= 0; --level) {
        const qreal step = blenderGridStepAtLevel(gridLevel, level);
        if (!std::isfinite(step) || step <= 1.0e-12) {
            continue;
        }
        const qreal radius = kGridLineHalfCount * step;
        const QPointF offset = gridFrame.cameraRelativeOffset;
        const qreal firstU = std::round(offset.x() / step) * step - radius;
        const qreal firstV = std::round(offset.y() / step) * step - radius;
        const qreal levelAlpha = level == 0 ? 1.0 - levelBlend : 1.0;
        const qreal emphasis = level == 0
                                   ? 0.0
                                   : (level == 1 ? 1.0 - levelBlend : 1.0);
        const QColor levelColor = QColor::fromRgbF(
            gridAppearance_.gridColor.redF() * (1.0 - emphasis) +
                gridAppearance_.emphasisColor.redF() * emphasis,
            gridAppearance_.gridColor.greenF() * (1.0 - emphasis) +
                gridAppearance_.emphasisColor.greenF() * emphasis,
            gridAppearance_.gridColor.blueF() * (1.0 - emphasis) +
                gridAppearance_.emphasisColor.blueF() * emphasis,
            gridAppearance_.gridColor.alphaF() * (1.0 - emphasis) +
                gridAppearance_.emphasisColor.alphaF() * emphasis);
        const qreal pixelFade = blenderOrthographicGridPixelFade(
            step * transform_.viewScalePixelsPerWorldUnit(viewportSize));
        const qreal lineOpacity = levelAlpha * levelColor.alphaF() *
                                  gridAppearance_.opacity * pixelFade;
        const auto radialFade = [offset, radius, this](const QPointF &point) {
            const qreal normalizedU = (point.x() - offset.x()) / radius;
            const qreal normalizedV = (point.y() - offset.y()) / radius;
            const qreal fade = std::clamp(1.0 - normalizedU * normalizedU -
                                              normalizedV * normalizedV,
                                          0.0,
                                          1.0);
            return std::pow(fade,
                            gridAppearance_.orthographicEdgeFadeExponent);
        };
        FadedLineBins lineBins;
        for (int lineIndex = -kGridLineHalfCount;
             lineIndex <= kGridLineHalfCount;
             ++lineIndex) {
            const qreal fixedV = firstV + (lineIndex + kGridLineHalfCount) * step;
            if (std::abs(fixedV) > 2.0e-7) {
                appendOrthographicGridLine(
                    transform_, viewportSize, plane, planeOffset,
                    QPointF(offset.x() - radius, fixedV),
                    QPointF(offset.x() + radius, fixedV),
                    lineOpacity, 24, lineBins, gridAppearance_, radialFade);
            }
            const qreal fixedU = firstU + (lineIndex + kGridLineHalfCount) * step;
            if (std::abs(fixedU) > 2.0e-7) {
                appendOrthographicGridLine(
                    transform_, viewportSize, plane, planeOffset,
                    QPointF(fixedU, offset.y() - radius),
                    QPointF(fixedU, offset.y() + radius),
                    lineOpacity, 24, lineBins, gridAppearance_, radialFade);
            }
        }
        QColor opaqueLevelColor = levelColor;
        opaqueLevelColor.setAlpha(255);
        drawFadedLineBins(painter, lineBins, opaqueLevelColor, 1.0, false);
    }
    painter.restore();
}

void ViewportRenderer::drawOrigin(QPainter &painter,
                                  const QSize &viewportSize) const
{
    const BlenderGridFrame gridFrame = resolveBlenderGridFrame(transform_,
                                                               viewportSize);
    const WorkPlane plane = gridFrame.plane;
    const qreal planeOffset = gridFrame.planeOffset;
    QPointF corners[4];
    const QPointF screenCorners[] = {{0.0, 0.0},
                                     {static_cast<qreal>(viewportSize.width()), 0.0},
                                     {static_cast<qreal>(viewportSize.width()),
                                      static_cast<qreal>(viewportSize.height())},
                                     {0.0, static_cast<qreal>(viewportSize.height())}};
    int cornerCount = 0;
    for (int index = 0; index < 4; ++index) {
        const bool resolved = transform_.isPerspectiveEnabled()
                                  ? transform_.screenToWorkPlane(
                                        screenCorners[index], viewportSize,
                                        plane, planeOffset,
                                        &corners[cornerCount])
                                  : transform_.screenToWorkPlaneUnclipped(
                                        screenCorners[index], viewportSize,
                                        plane, planeOffset,
                                        &corners[cornerCount]);
        if (!resolved) {
            continue;
        }
        ++cornerCount;
    }
    if (cornerCount < 2) {
        return;
    }
    qreal minimumU = corners[0].x();
    qreal maximumU = corners[0].x();
    qreal minimumV = corners[0].y();
    qreal maximumV = corners[0].y();
    for (int index = 0; index < cornerCount; ++index) {
        minimumU = std::min(minimumU, corners[index].x());
        maximumU = std::max(maximumU, corners[index].x());
        minimumV = std::min(minimumV, corners[index].y());
        maximumV = std::max(maximumV, corners[index].y());
    }

    if (transform_.isPerspectiveEnabled()) {
        const qreal farClipDistance = transform_.cameraPreferences().clipEnd;
        minimumU = std::max(minimumU, -farClipDistance);
        maximumU = std::min(maximumU, farClipDistance);
        minimumV = std::max(minimumV, -farClipDistance);
        maximumV = std::min(maximumV, farClipDistance);
        if (minimumU > maximumU || minimumV > maximumV) {
            return;
        }

        const Point3D cameraPosition = transform_.cameraPosition(viewportSize);
        const Point3D planeNormal = workPlaneNormal(plane);
        FadedLineBins uAxisBins;
        FadedLineBins vAxisBins;
        const auto noAdditionalFade = [](const QPointF &) { return 1.0; };
        const int globalUAxis = plane == WorkPlane::YZ ? 1 : 0;
        const int globalVAxis = plane == WorkPlane::XY ? 1 : 2;
        const auto axisColorForIndex = [this](int axis) {
            if (axis == 0) {
                return gridAppearance_.axisXColor;
            }
            if (axis == 1) {
                return gridAppearance_.axisYColor;
            }
            return gridAppearance_.axisZColor;
        };
        const auto axisEnabled = [this](int axis) {
            return axis == 0 ? gridAppearance_.showAxisX
                 : axis == 1 ? gridAppearance_.showAxisY
                             : gridAppearance_.showAxisZ;
        };
        if (gridFrame.visibleAxes[static_cast<size_t>(globalUAxis)] &&
            axisEnabled(globalUAxis)) {
            const QColor color = axisColorForIndex(globalUAxis);
            appendPerspectiveLine(
                transform_, viewportSize, plane, planeOffset, cameraPosition,
                planeNormal, farClipDistance,
                QPointF(minimumU, 0.0), QPointF(maximumU, 0.0),
                color.alphaF() * gridAppearance_.opacity, 128, uAxisBins,
                gridAppearance_, noAdditionalFade);
        }
        if (gridFrame.visibleAxes[static_cast<size_t>(globalVAxis)] &&
            axisEnabled(globalVAxis)) {
            const QColor color = axisColorForIndex(globalVAxis);
            appendPerspectiveLine(
                transform_, viewportSize, plane, planeOffset, cameraPosition,
                planeNormal, farClipDistance,
                QPointF(0.0, minimumV), QPointF(0.0, maximumV),
                color.alphaF() * gridAppearance_.opacity, 128, vAxisBins,
                gridAppearance_, noAdditionalFade);
        }

        painter.save();
        painter.setClipRect(QRect(QPoint(0, 0), viewportSize));
        const std::array<QColor, 3> axisColors{
            gridAppearance_.axisXColor,
            gridAppearance_.axisYColor,
            gridAppearance_.axisZColor,
        };
        QColor uColor = axisColors[static_cast<size_t>(globalUAxis)];
        QColor vColor = axisColors[static_cast<size_t>(globalVAxis)];
        const qreal uOpacity = uColor.alphaF() * gridAppearance_.opacity;
        const qreal vOpacity = vColor.alphaF() * gridAppearance_.opacity;
        uColor.setAlpha(255);
        vColor.setAlpha(255);
        drawFadedLineBins(painter,
                          uAxisBins,
                          uColor,
                          uOpacity);
        drawFadedLineBins(painter,
                          vAxisBins,
                          vColor,
                          vOpacity);
        painter.restore();
        return;
    }

    painter.save();
    painter.setClipRect(QRect(QPoint(0, 0), viewportSize));
    const int globalUAxis = plane == WorkPlane::YZ ? 1 : 0;
    const int globalVAxis = plane == WorkPlane::XY ? 1 : 2;
    const std::array<QColor, 3> axisColors{
        gridAppearance_.axisXColor,
        gridAppearance_.axisYColor,
        gridAppearance_.axisZColor,
    };
    const auto projectGridPoint = [this, &viewportSize, plane, planeOffset](
                                      const QPointF &point) {
        QPointF screenPoint;
        if (!transform_.worldPointToScreenUnclipped(
                workPlanePointToWorld(point, plane, planeOffset),
                viewportSize,
                &screenPoint)) {
            return QPointF();
        }
        return screenPoint;
    };
    const auto axisEnabled = [this](int axis) {
        return axis == 0 ? gridAppearance_.showAxisX
             : axis == 1 ? gridAppearance_.showAxisY
                         : gridAppearance_.showAxisZ;
    };
    if (gridFrame.visibleAxes[static_cast<size_t>(globalUAxis)] &&
        axisEnabled(globalUAxis)) {
        QColor color = axisColors[static_cast<size_t>(globalUAxis)];
        color.setAlphaF(color.alphaF() * gridAppearance_.opacity);
        painter.setPen(QPen(color, 1.25));
        painter.drawLine(projectGridPoint(QPointF(minimumU, 0.0)),
                         projectGridPoint(QPointF(maximumU, 0.0)));
    }
    if (gridFrame.visibleAxes[static_cast<size_t>(globalVAxis)] &&
        axisEnabled(globalVAxis)) {
        QColor color = axisColors[static_cast<size_t>(globalVAxis)];
        color.setAlphaF(color.alphaF() * gridAppearance_.opacity);
        painter.setPen(QPen(color, 1.25));
        painter.drawLine(projectGridPoint(QPointF(0.0, minimumV)),
                         projectGridPoint(QPointF(0.0, maximumV)));
    }
    painter.restore();
}

QVector<QPointF> ViewportRenderer::rectangleVertices(const Shape &shape) const
{
    if (shape.geometryType == GeometryType::Rectangle &&
        isValidNurbsCurve(shape.nurbs) && shape.nurbs.controlPoints.size() == 5) {
        return shape.nurbs.controlPoints.mid(0, 4);
    }
    if (shape.geometryType != GeometryType::Rectangle || shape.points.size() < 2) {
        return {};
    }

    if (shape.points.size() >= 4) {
        return {shape.points[0], shape.points[1], shape.points[2], shape.points[3]};
    }

    const QPointF first = shape.points[0];
    const QPointF second = shape.points[1];
    return {first,
            QPointF(second.x(), first.y()),
            second,
            QPointF(first.x(), second.y())};
}

bool ViewportRenderer::drawPreparedCurveSegments(
    QPainter &painter,
    const ViewportDepthGeometry &preparedGeometry,
    const QSize &viewportSize,
    const QVector<float> *opaqueSceneDepth,
    const QSize &opaqueSceneDepthSize) const
{
    const QVector<Point3D> &worldVertices =
        preparedGeometry.preciseLineVertices;
    if (worldVertices.isEmpty() || worldVertices.size() % 2 != 0) {
        return false;
    }

    if (opaqueSceneDepth != nullptr &&
        opaqueSceneDepthSize.width() > 0 &&
        opaqueSceneDepthSize.height() > 0 &&
        opaqueSceneDepth->size() ==
            opaqueSceneDepthSize.width() * opaqueSceneDepthSize.height()) {
        const QTransform deviceTransform = painter.deviceTransform();
        const Point3D cameraPosition = transform_.cameraPosition(viewportSize);
        const Point3D viewDirection = transform_.viewDirection();
        const auto cameraDepth = [&](const Point3D &point) {
            if (transform_.isPerspectiveEnabled()) {
                return (point.x - cameraPosition.x) * viewDirection.x +
                       (point.y - cameraPosition.y) * viewDirection.y +
                       (point.z - cameraPosition.z) * viewDirection.z;
            }
            return transform_.worldDirectionToView(point).towardCamera;
        };
        QPainterPath visiblePath;
        QPainterPath hiddenPath;
        bool visibleRun = false;
        bool hiddenRun = false;
        quint64 sampleBudget = 0;
        constexpr quint64 maximumDepthSamples = 1'000'000;
        const quint64 segmentCount =
            static_cast<quint64>(worldVertices.size() / 2);
        const quint64 segmentStride = std::max<quint64>(
            1, (segmentCount + maximumDepthSamples - 1) /
                   maximumDepthSamples);
        quint64 requestedSamples = 0;
        for (quint64 segment = 0; segment < segmentCount;
             segment += segmentStride) {
            const int index = static_cast<int>(segment * 2);
            Point3D clippedStart = worldVertices[index];
            Point3D clippedEnd = worldVertices[index + 1];
            if (!clipWorkbenchSegmentToViewDepth(
                    transform_, viewportSize, &clippedStart, &clippedEnd)) {
                continue;
            }
            QPointF screenStart;
            QPointF screenEnd;
            if (!transform_.worldPointToScreenUnclipped(
                    clippedStart, viewportSize, &screenStart) ||
                !transform_.worldPointToScreenUnclipped(
                    clippedEnd, viewportSize, &screenEnd)) {
                continue;
            }
            const QPointF deviceStart = deviceTransform.map(screenStart);
            const QPointF deviceEnd = deviceTransform.map(screenEnd);
            const qreal deviceLength = std::hypot(
                deviceEnd.x() - deviceStart.x(),
                deviceEnd.y() - deviceStart.y());
            if (!std::isfinite(deviceLength)) continue;
            const quint64 steps = static_cast<quint64>(std::clamp<qreal>(
                std::ceil(deviceLength * 2.0), 1.0,
                static_cast<qreal>(maximumDepthSamples)));
            requestedSamples = std::min(
                maximumDepthSamples * maximumDepthSamples,
                requestedSamples + steps + 1);
        }
        // Preserve at least one segment sample but reduce density uniformly
        // when a very large curve would otherwise exceed the bounded depth
        // sampling work. This avoids falling back to an unoccluded full path.
        const quint64 sampleStride = std::max<quint64>(
            1, (requestedSamples + maximumDepthSamples - 1) /
                   maximumDepthSamples);
        for (int index = 0; index + 1 < worldVertices.size(); index += 2) {
            const quint64 segmentIndex =
                static_cast<quint64>(index / 2);
            if (segmentIndex % segmentStride != 0) {
                visibleRun = false;
                hiddenRun = false;
                continue;
            }
            QPointF screenStart;
            QPointF screenEnd;
            const Point3D &worldStart = worldVertices[index];
            const Point3D &worldEnd = worldVertices[index + 1];
            if (index > 0) {
                const Point3D &previousEnd = worldVertices[index - 1];
                if (previousEnd.x != worldStart.x ||
                    previousEnd.y != worldStart.y ||
                    previousEnd.z != worldStart.z) {
                    visibleRun = false;
                    hiddenRun = false;
                }
            }
            Point3D clippedStart = worldStart;
            Point3D clippedEnd = worldEnd;
            if (!clipWorkbenchSegmentToViewDepth(
                    transform_, viewportSize, &clippedStart, &clippedEnd)) {
                visibleRun = false;
                hiddenRun = false;
                continue;
            }
            if (!transform_.worldPointToScreenUnclipped(
                    clippedStart, viewportSize, &screenStart) ||
                !transform_.worldPointToScreenUnclipped(
                    clippedEnd, viewportSize, &screenEnd)) {
                visibleRun = false;
                hiddenRun = false;
                continue;
            }
            const QPointF deviceStart = deviceTransform.map(screenStart);
            const QPointF deviceEnd = deviceTransform.map(screenEnd);
            const qreal deviceLength = std::hypot(
                deviceEnd.x() - deviceStart.x(),
                deviceEnd.y() - deviceStart.y());
            if (!std::isfinite(deviceLength) ||
                sampleBudget + 2 > maximumDepthSamples) {
                visibleRun = false;
                hiddenRun = false;
                continue;
            }
            const quint64 requestedSteps = static_cast<quint64>(
                std::clamp<qreal>(
                    std::ceil(deviceLength * 2.0 / sampleStride), 1.0,
                    static_cast<qreal>(maximumDepthSamples)));
            const quint64 steps = std::min(
                requestedSteps, maximumDepthSamples - sampleBudget - 1);
            if (steps == 0) {
                visibleRun = false;
                hiddenRun = false;
                continue;
            }
            sampleBudget += steps + 1;
            const qreal startDepth = cameraDepth(clippedStart);
            const qreal endDepth = cameraDepth(clippedEnd);
            for (quint64 step = 0; step <= steps; ++step) {
                const qreal fraction = static_cast<qreal>(step) /
                                       static_cast<qreal>(steps);
                const QPointF devicePoint =
                    deviceStart + (deviceEnd - deviceStart) * fraction;
                const int x = qFloor(devicePoint.x());
                const int y = qFloor(devicePoint.y());
                bool visible = true;
                if (x >= 0 && x < opaqueSceneDepthSize.width() &&
                    y >= 0 && y < opaqueSceneDepthSize.height()) {
                    qreal curveDepth = startDepth +
                        (endDepth - startDepth) * fraction;
                    if (transform_.isPerspectiveEnabled() &&
                        startDepth < -1.0e-9 && endDepth < -1.0e-9) {
                        const qreal reciprocal =
                            (1.0 - fraction) / startDepth +
                            fraction / endDepth;
                        if (reciprocal < -1.0e-12) {
                            curveDepth = 1.0 / reciprocal;
                        }
                    }
                    const float surfaceDepth = (*opaqueSceneDepth)[
                        y * opaqueSceneDepthSize.width() + x];
                    const qreal depthTolerance = std::max<qreal>(
                        1.0e-4, std::abs(curveDepth) * 1.0e-6);
                    visible = !std::isfinite(surfaceDepth) ||
                              surfaceDepth <= curveDepth + depthTolerance;
                }
                const QPointF screenPoint = screenStart +
                    (screenEnd - screenStart) * fraction;
                if (visible) {
                    if (!visibleRun) {
                        visiblePath.moveTo(screenPoint);
                        visibleRun = true;
                    } else {
                        visiblePath.lineTo(screenPoint);
                    }
                    hiddenRun = false;
                } else {
                    visibleRun = false;
                    if (!shadingSettings_.xrayEnabled()) {
                        hiddenRun = false;
                        continue;
                    }
                    if (!hiddenRun) {
                        hiddenPath.moveTo(screenPoint);
                        hiddenRun = true;
                    } else {
                        hiddenPath.lineTo(screenPoint);
                    }
                }
            }
        }
        if (shadingSettings_.xrayEnabled() && !hiddenPath.isEmpty()) {
            painter.save();
            QPen hiddenPen = painter.pen();
            QColor hiddenColor = hiddenPen.color();
            hiddenColor.setAlphaF(hiddenColor.alphaF() *
                (1.0 - std::clamp(shadingSettings_.xrayAlphaForMode(), 0.0, 1.0)));
            hiddenPen.setColor(hiddenColor);
            painter.setPen(hiddenPen);
            painter.drawPath(hiddenPath);
            painter.restore();
        }
        if (!visiblePath.isEmpty()) {
            painter.drawPath(visiblePath);
        }
        if (visiblePath.isEmpty() && hiddenPath.isEmpty()) {
            return true;
        }
        return true;
    }

    QPainterPath path;
    QVector<QPointF> projectedRun;
    const auto flushRun = [&]() {
        if (projectedRun.size() < 2) {
            projectedRun.clear();
            return;
        }

        constexpr qreal flatnessTolerancePixels = 0.3;
        const qreal toleranceSquared =
            flatnessTolerancePixels * flatnessTolerancePixels;
        QVector<uchar> keep(projectedRun.size(), 0);
        std::vector<std::pair<int, int>> intervals;
        intervals.reserve(32);
        constexpr int maximumSimplificationChunkSegments = 16;
        for (int chunkStart = 0;
             chunkStart + 1 < projectedRun.size();) {
            const int chunkEnd = std::min(
                static_cast<int>(projectedRun.size()) - 1,
                chunkStart + maximumSimplificationChunkSegments);
            keep[chunkStart] = 1;
            keep[chunkEnd] = 1;
            intervals.clear();
            intervals.emplace_back(chunkStart, chunkEnd);
            while (!intervals.empty()) {
                const auto [first, last] = intervals.back();
                intervals.pop_back();
                const QPointF start = projectedRun[first];
                const QPointF end = projectedRun[last];
                const QPointF direction = end - start;
                const qreal lengthSquared = QPointF::dotProduct(direction, direction);
                qreal maximumDistanceSquared = toleranceSquared;
                int furthestIndex = -1;
                for (int index = first + 1; index < last; ++index) {
                    const QPointF offset = projectedRun[index] - start;
                    const qreal fraction = lengthSquared > 1.0e-20
                                               ? std::clamp(
                                                     QPointF::dotProduct(offset,
                                                                        direction) /
                                                         lengthSquared,
                                                     0.0,
                                                     1.0)
                                               : 0.0;
                    const QPointF closest = start + direction * fraction;
                    const QPointF difference = projectedRun[index] - closest;
                    const qreal distanceSquared =
                        QPointF::dotProduct(difference, difference);
                    if (distanceSquared > maximumDistanceSquared) {
                        maximumDistanceSquared = distanceSquared;
                        furthestIndex = index;
                    }
                }
                if (furthestIndex >= 0) {
                    keep[furthestIndex] = 1;
                    intervals.emplace_back(first, furthestIndex);
                    intervals.emplace_back(furthestIndex, last);
                }
            }
            chunkStart = chunkEnd;
        }

        path.moveTo(projectedRun.first());
        for (int index = 1; index < projectedRun.size(); ++index) {
            if (keep[index] != 0) {
                path.lineTo(projectedRun[index]);
            }
        }
        projectedRun.clear();
    };

    bool hasPreviousEnd = false;
    Point3D previousWorldEnd;
    QPointF previousScreenEnd;
    for (int index = 0; index + 1 < worldVertices.size(); index += 2) {
        QPointF start;
        QPointF end;
        const Point3D &worldStart = worldVertices[index];
        const Point3D &worldEnd = worldVertices[index + 1];
        const bool contiguous =
            hasPreviousEnd && previousWorldEnd.x == worldStart.x &&
            previousWorldEnd.y == worldStart.y &&
            previousWorldEnd.z == worldStart.z;
        if (contiguous) {
            start = previousScreenEnd;
        } else if (!transform_.worldPointToScreen(worldStart,
                                                  viewportSize,
                                                  &start)) {
            // Keep the established adaptive path for any camera-clipped or
            // otherwise unprojectable geometry.
            return false;
        }
        if (!transform_.worldPointToScreen(worldEnd, viewportSize, &end)) {
            return false;
        }
        if (!contiguous) {
            flushRun();
            projectedRun.append(start);
        }
        projectedRun.append(end);
        previousWorldEnd = worldEnd;
        previousScreenEnd = end;
        hasPreviousEnd = true;
    }
    flushRun();
    if (path.isEmpty()) {
        return false;
    }
    painter.drawPath(path);
    return true;
}

void ViewportRenderer::drawShape(QPainter &painter,
                                 const Shape &shape,
                                 const QSize &viewportSize,
                                 bool preview,
                                 bool selected,
                                 bool drawPreviewPoints,
                                 const QColor &layerColor,
                                 const QString &layerLineType,
                                 qreal layerLineWeightMm,
                                 ObjectId shapeObjectId,
                                 quint64 geometryRevision,
                                 const ViewportDepthGeometry *preparedGeometry,
                                 const Point3D &preparedOffset,
                                 std::optional<QVector4D> displayColorSceneLinear,
                                 std::optional<WorkbenchMaterialData>
                                     workbenchMaterial,
                                 const QString &displayName,
                                 const QString &libraryPath,
                                 const MeshObjectTransform &meshTransform,
                                 const QVector<WorkbenchMaterialData> &meshMaterials,
                                 RasterWorkbenchFragmentSink *fragmentSink,
                                 const QHash<QString, WorkbenchMaterialData> &nurbsSolidFaceMaterials,
                                 const QVector<float> *opaqueSceneDepth,
                                 const QSize &opaqueSceneDepthSize,
                                 const QVector<Point3D> *blenderSurfaceWireVerticesWorld) const
{
    if (shape.points.isEmpty() && !isValidNurbsCurve(shape.nurbs) &&
        shape.components.isEmpty() &&
        !validateNurbsSurface(shape.nurbsSurface) &&
        !validateNurbsSolid(shape.nurbsSolid) &&
        !(shape.geometryType == GeometryType::PolygonMesh &&
          validatePolygonMesh(shape.polygonMesh))) {
        return;
    }

    QColor curveColor = selected
                                  ? viewportSelectionColor()
                                  : layerColor.isValid()
                                        ? layerColor
                                        : preview
                                              ? QColor(QStringLiteral("#e6b85c"))
                                              : QColor(QStringLiteral("#000000"));
    if (!selected && !preview &&
        shadingSettings_.wireColorMode == ViewportWireColorMode::Random) {
        const quint32 seed = static_cast<quint32>(shapeObjectId.value());
        curveColor = QColor::fromHsv(
            static_cast<int>((seed * 2654435761u) % 360u), 160, 230);
    }
    if (!selected && !preview &&
        shadingSettings_.mode == ViewportShadingMode::Solid &&
        isViewportCurveStrokeType(shape.geometryType)) {
        curveColor = shadingSettings_.themeEditWireColor;
    } else if (!selected && !preview &&
               shadingSettings_.mode == ViewportShadingMode::Wireframe &&
               shadingSettings_.wireColorMode == ViewportWireColorMode::Theme) {
        curveColor = shadingSettings_.themeWireColor;
    }
    const auto *imageTarget = isDimensionGeometryType(shape.geometryType)
        ? dynamic_cast<const QImage *>(painter.device()) : nullptr;
    const QColorSpace linearSrgb(QColorSpace::SRgbLinear);
    const bool linearAnnotationTarget =
        imageTarget != nullptr && imageTarget->colorSpace() == linearSrgb;
    const auto colorForLinearAnnotation = [](QColor color) {
        const auto srgbToLinear = [](qreal value) {
            return value <= 0.04045
                ? value / 12.92
                : std::pow((value + 0.055) / 1.055, 2.4);
        };
        return QColor::fromRgbF(srgbToLinear(color.redF()),
                                srgbToLinear(color.greenF()),
                                srgbToLinear(color.blueF()), color.alphaF());
    };
    if (linearAnnotationTarget) {
        curveColor = colorForLinearAnnotation(curveColor);
    }
    const QColor controlColor = QColor(QStringLiteral("#8aa7c7"));
    const qreal storedWidth = viewportLineWeightPixels(layerLineWeightMm);
    const qreal curveWidth = preview ? 1.5 : storedWidth;
    const QPen layerPen = selected || preview
                              ? QPen(curveColor, curveWidth)
                              : layerLineTypePen(curveColor, curveWidth, layerLineType);

    const auto drawWithWireHint = [&painter, this, preview](
                                     bool editModeWire,
                                     const auto &draw) {
        painter.save();
        if (!preview) {
            painter.setRenderHint(
                QPainter::Antialiasing,
                editModeWire ? smoothWiresEditMode_ : smoothWiresOverlay_);
        }
        draw();
        painter.restore();
    };

    painter.setPen(layerPen);
    painter.setBrush(Qt::NoBrush);
    const WorkPlaneFrame displayFrame = transform_.workPlaneFrame();
    const auto drawClippedWorkPlaneSegment = [&](const QPointF &start,
                                                  const QPointF &end) {
        Point3D clippedStart = workPlaneFramePointToWorld(start, 0.0,
                                                          displayFrame);
        Point3D clippedEnd = workPlaneFramePointToWorld(end, 0.0,
                                                        displayFrame);
        if (!clipWorkbenchSegmentToViewDepth(transform_, viewportSize,
                                             &clippedStart, &clippedEnd)) {
            return;
        }
        QPointF screenStart;
        QPointF screenEnd;
        if (transform_.worldPointToScreenUnclipped(clippedStart, viewportSize,
                                                   &screenStart) &&
            transform_.worldPointToScreenUnclipped(clippedEnd, viewportSize,
                                                   &screenEnd)) {
            painter.drawLine(screenStart, screenEnd);
        }
    };

    if (shape.geometryType == GeometryType::PolygonMesh &&
        validatePolygonMesh(shape.polygonMesh)) {
        struct MeshTriangle {
            QPolygonF points;
            QColor color;
            QVector3D sceneLinearColor{0.8f, 0.8f, 0.8f};
            qreal sceneLinearAlpha = 1.0;
            std::array<QVector3D, 3> attributeColors{};
            bool hasAttributeColors = false;
            QVector3D normalView{0.0f, 0.0f, 1.0f};
            std::array<QVector3D, 3> cornerNormalsView{};
            QVector3D incidentView{0.0f, 0.0f, 1.0f};
            std::array<Point3D, 3> worldPoints{};
            qreal roughness = 0.4;
            qreal metallic = 0.0;
            bool useWorkbenchColor = false;
            qreal depth = 0.0;
            QImage texture;
            QHash<int, QImage> textureTiles;
            WorkbenchFloatTextureData floatTexture;
            QHash<int, WorkbenchFloatTextureData> floatTextureTiles;
            bool tiledTexture = false;
            bool texturePremultiplied = false;
            bool textureAlphaCutoff = true;
            bool textureSrgb = true;
            QString textureInterpolation = QStringLiteral("Linear");
            QString textureExtension = QStringLiteral("REPEAT");
            std::array<QPointF, 3> uvs{};
            std::array<qreal, 3> reciprocalDepths{};
        };
        QVector<MeshTriangle> triangles;
        const PolygonMesh3D &mesh = shape.polygonMesh;
        const qreal meshTransformDeterminant =
            meshTransform[0] * (meshTransform[5] * meshTransform[10] -
                                meshTransform[6] * meshTransform[9]) -
            meshTransform[1] * (meshTransform[4] * meshTransform[10] -
                                meshTransform[6] * meshTransform[8]) +
            meshTransform[2] * (meshTransform[4] * meshTransform[9] -
                                meshTransform[5] * meshTransform[8]);
        const bool reflectedMeshTransform =
            std::isfinite(meshTransformDeterminant) &&
            meshTransformDeterminant < 0.0;
        const MeshUvMap *activeUvMap = nullptr;
        for (const MeshUvMap &uvMap : mesh.uvMaps) {
            if (uvMap.name == workbenchUvMapName(mesh)) {
                activeUvMap = &uvMap;
                break;
            }
        }
        struct MeshTextureSet {
            QImage image;
            QHash<int, QImage> tiles;
            WorkbenchFloatTextureData floatImage;
            QHash<int, WorkbenchFloatTextureData> floatTiles;
        };
        QHash<QString, MeshTextureSet> textureImages;
        QHash<int, MeshTextureSet> faceTextureImages;
        int firstCorner = 0;
        const MeshColorAttribute *activeAttribute = nullptr;
        if (shadingSettings_.colorMode == ViewportColorMode::Attribute) {
            for (const MeshColorAttribute &attribute : mesh.colorAttributes) {
                if (attribute.name == mesh.defaultColorAttributeName) {
                    activeAttribute = &attribute;
                    break;
                }
            }
            if (activeAttribute == nullptr) {
                for (const MeshColorAttribute &attribute : mesh.colorAttributes) {
                    if (attribute.name == mesh.activeColorAttributeName) {
                        activeAttribute = &attribute;
                        break;
                    }
                }
            }
        }
        const auto materialColorForFace = [&](int faceIndex) {
            QVector4D color(0.8f, 0.8f, 0.8f, 1.0f);
            const int slotIndex = mesh.faceMaterialIndices.value(faceIndex, 0);
            if (slotIndex >= 0 && slotIndex < meshMaterials.size()) {
                const WorkbenchMaterialData &material = meshMaterials[slotIndex];
                color = QVector4D(material.baseColorSceneLinear.x(),
                                  material.baseColorSceneLinear.y(),
                                  material.baseColorSceneLinear.z(),
                                  material.alpha);
                return color;
            }
            if (slotIndex >= 0 && slotIndex < mesh.materialSlots.size()) {
                const PolygonMeshMaterialSlot &slot =
                    mesh.materialSlots[slotIndex];
                if (slot.baseColorSceneLinear.size() >= 3) {
                    color = QVector4D(slot.baseColorSceneLinear[0],
                                      slot.baseColorSceneLinear[1],
                                      slot.baseColorSceneLinear[2],
                                      // Import stores diffuse alpha in both
                                      // payloads; material.alpha is canonical.
                                      slot.alpha);
                }
            }
            return color;
        };
        const auto textureForFace = [&](int faceIndex) -> const MeshTextureSet * {
            if (activeUvMap == nullptr) return nullptr;
            const int slot = mesh.faceMaterialIndices.value(faceIndex, 0);
            if (slot < 0 || slot >= meshMaterials.size()) return nullptr;
            const auto faceFound = faceTextureImages.constFind(slot);
            if (faceFound != faceTextureImages.cend()) return &faceFound.value();
            const WorkbenchMaterialData &material = meshMaterials[slot];
            const QString path = material.textureImagePath;
            const QString packed = material.textureImagePackedDataBase64;
            if ((path.isEmpty() || !QFileInfo(path).isFile()) && packed.isEmpty() &&
                material.textureImageSceneLinearFloatDataBase64.isEmpty() &&
                material.textureImageTiles.isEmpty()) {
                return nullptr;
            }
            const QString alphaMode = material.textureImageAlphaMode;
            QString sourceKey = !packed.isEmpty()
                ? QStringLiteral("packed:") + packed
                : QStringLiteral("file:") + path;
            sourceKey += QStringLiteral("|float:%1:%2:%3")
                .arg(QString::fromLatin1(QCryptographicHash::hash(
                    material.textureImageSceneLinearFloatDataBase64.toLatin1(),
                    QCryptographicHash::Sha256).toHex()))
                .arg(material.textureImageSceneLinearFloatWidth)
                .arg(material.textureImageSceneLinearFloatHeight);
            for (const DocumentImageTextureTile &tile : material.textureImageTiles) {
                sourceKey += QStringLiteral("|%1:%2:%3")
                    .arg(tile.number).arg(tile.filepath)
                    .arg(tile.sourceFileExists ? 1 : 0) + QStringLiteral(":") +
                    QString::fromLatin1(QCryptographicHash::hash(
                        tile.packedDataBase64.toLatin1(), QCryptographicHash::Sha256).toHex()) +
                    QStringLiteral(":float:") +
                    QString::fromLatin1(QCryptographicHash::hash(
                        tile.sceneLinearFloatDataBase64.toLatin1(),
                        QCryptographicHash::Sha256).toHex()) +
                    QStringLiteral(":%1x%2").arg(tile.sceneLinearFloatWidth)
                        .arg(tile.sceneLinearFloatHeight);
            }
            const QString cacheKey = sourceKey + QLatin1Char('|') + alphaMode.toUpper();
            auto found = textureImages.find(cacheKey);
            if (found == textureImages.end()) {
                const bool ignoreAlpha =
                    alphaMode.compare(QStringLiteral("NONE"), Qt::CaseInsensitive) == 0 ||
                    alphaMode.compare(QStringLiteral("IGNORE"), Qt::CaseInsensitive) == 0 ||
                    alphaMode.compare(QStringLiteral("CHANNEL_PACKED"), Qt::CaseInsensitive) == 0;
                const auto loadImage = [ignoreAlpha](const QString &imagePath,
                                                     const QString &packedData) {
                    QImage loaded;
                    if (!packedData.isEmpty()) {
                        const QByteArray encoded = packedData.toLatin1();
                        if (encoded.size() <= 44 * 1024 * 1024) {
                            const QByteArray bytes = QByteArray::fromBase64(
                                encoded, QByteArray::AbortOnBase64DecodingErrors);
                            if (!bytes.isEmpty() && bytes.size() <= 32 * 1024 * 1024) {
                                QBuffer buffer;
                                buffer.setData(bytes);
                                if (buffer.open(QIODevice::ReadOnly)) {
                                    QImageReader reader(&buffer);
                                    const QSize size = reader.size();
                                    if (size.isValid() && size.width() <= 16384 &&
                                        size.height() <= 16384 &&
                                        static_cast<quint64>(size.width()) * size.height() <=
                                            67108864ULL) loaded = reader.read();
                                }
                            }
                        }
                    }
                    if (loaded.isNull() && !imagePath.isEmpty()) loaded.load(imagePath);
                    if (loaded.isNull()) return QImage{};
                    loaded = loaded.convertToFormat(QImage::Format_RGBA8888);
                    for (int y = 0; y < loaded.height(); ++y) {
                        auto *row = loaded.scanLine(y);
                        for (int x = 0; x < loaded.width(); ++x) {
                            if (ignoreAlpha) row[x * 4 + 3] = 255;
                        }
                    }
                    return loaded;
                };
            const auto loadFloatImage = [](const QString &encoded, int width,
                                           int height) {
                WorkbenchFloatTextureData result;
                if (encoded.isEmpty() || width <= 0 || height <= 0 ||
                    quint64(width) * quint64(height) * 16ULL > 32ULL * 1024ULL * 1024ULL ||
                    encoded.size() > 44 * 1024 * 1024) return result;
                const QByteArray bytes = QByteArray::fromBase64(
                    encoded.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
                if (bytes.size() != qint64(width) * height * 16) return result;
                result.width = width;
                result.height = height;
                result.pixels.resize(width * height * 4);
                for (int index = 0; index < result.pixels.size(); ++index) {
                    const auto *source = reinterpret_cast<const uchar *>(
                        bytes.constData() + index * 4);
                    result.pixels[index] = std::bit_cast<float>(
                        qFromLittleEndian<quint32>(source));
                }
                for (float v : result.pixels) if (!std::isfinite(v)) return WorkbenchFloatTextureData{};
                return result;
            };
                MeshTextureSet loaded;
                quint64 decodedTilePixels = 0;
                qsizetype packedTileBytes = 0;
                if (!packed.isEmpty() || (!path.isEmpty() && QFileInfo(path).isFile())) {
                    loaded.image = loadImage(path, packed);
                }
                loaded.floatImage = loadFloatImage(
                    material.textureImageSceneLinearFloatDataBase64,
                    material.textureImageSceneLinearFloatWidth,
                    material.textureImageSceneLinearFloatHeight);
                quint64 floatTileBytes = loaded.floatImage.isValid()
                    ? quint64(loaded.floatImage.pixels.size()) * sizeof(float) : 0;
                for (const DocumentImageTextureTile &tile : material.textureImageTiles) {
                    auto floatTile = loadFloatImage(tile.sceneLinearFloatDataBase64,
                        tile.sceneLinearFloatWidth, tile.sceneLinearFloatHeight);
                    if (floatTile.isValid()) {
                        const quint64 bytes = quint64(floatTile.pixels.size()) * sizeof(float);
                        if (floatTileBytes + bytes <= 32ULL * 1024ULL * 1024ULL) {
                            floatTileBytes += bytes;
                            loaded.floatTiles.insert(tile.number, std::move(floatTile));
                        }
                    }
                    if ((!tile.sourceFileExists && tile.packedDataBase64.isEmpty()) ||
                        tile.number < 1001 || tile.number > 1999) continue;
                    QString packedTile = tile.packedDataBase64;
                    if (!packedTile.isEmpty()) {
                        const QByteArray encoded = packedTile.toLatin1();
                        const QByteArray bytes = encoded.size() <= 44 * 1024 * 1024
                            ? QByteArray::fromBase64(encoded,
                                  QByteArray::AbortOnBase64DecodingErrors)
                            : QByteArray{};
                        if (bytes.isEmpty() || bytes.size() > 32 * 1024 * 1024 ||
                            packedTileBytes + bytes.size() > 32 * 1024 * 1024) {
                            packedTile.clear();
                        } else {
                            packedTileBytes += bytes.size();
                        }
                    }
                    QImage tileImage = loadImage(tile.filepath, packedTile);
                    const quint64 tilePixels = static_cast<quint64>(tileImage.width()) *
                                               tileImage.height();
                    if (tileImage.isNull() || tileImage.width() > 16384 ||
                        tileImage.height() > 16384 || tilePixels > 67108864ULL ||
                        decodedTilePixels + tilePixels > 67108864ULL) continue;
                    decodedTilePixels += tilePixels;
                    if (!tileImage.isNull()) loaded.tiles.insert(tile.number,
                                                                 std::move(tileImage));
                }
                if (loaded.image.isNull() && loaded.tiles.isEmpty() &&
                    !loaded.floatImage.isValid() && loaded.floatTiles.isEmpty() &&
                    material.textureImageTiles.isEmpty()) return nullptr;
                found = textureImages.insert(cacheKey, std::move(loaded));
            }
            return &faceTextureImages.insert(slot, found.value()).value();
        };
        const auto attributeColor = [&](int faceIndex,
                                        const QVector<int> &face,
                                        const std::array<int, 3> &triangle)
            -> std::optional<std::array<QVector3D, 3>> {
            if (activeAttribute == nullptr ||
                activeAttribute->componentCount < 1 ||
                activeAttribute->componentCount > 4) {
                return std::nullopt;
            }
            std::array<QVector3D, 3> colors{};
            int cornerIndex = 0;
            for (int loopIndex : triangle) {
                int element = faceIndex;
                if (activeAttribute->domain == MeshAttributeDomain::Point) {
                    element = face[loopIndex];
                } else if (activeAttribute->domain == MeshAttributeDomain::Corner) {
                    element = firstCorner + loopIndex;
                }
                const int base = element * activeAttribute->componentCount;
                if (base < 0 || base + activeAttribute->componentCount >
                                    activeAttribute->values.size()) {
                    return std::nullopt;
                }
                QVector3D sample;
                if (activeAttribute->componentCount == 1) {
                    const float value = activeAttribute->values[base];
                    sample = QVector3D(value, value, value);
                } else {
                    sample = QVector3D(activeAttribute->values[base],
                                       activeAttribute->values[base + 1],
                                       activeAttribute->values[base + 2]);
                }
                if (activeAttribute->colorSpace.contains(
                        QStringLiteral("srgb"), Qt::CaseInsensitive)) {
                    sample = workbenchSrgbToSceneLinear(sample);
                }
                colors[cornerIndex++] = sample;
            }
            return colors;
        };
        struct MeshClipVertex {
            Point3D position;
            QPointF uv;
            QVector3D normal;
            QVector3D attributeColor;
        };
        const ViewportCameraPreferences clipPreferences =
            transform_.cameraPreferences();
        const Point3D clipTarget = transform_.viewTarget();
        const Point3D clipCamera = transform_.cameraPosition(viewportSize);
        const Point3D clipViewDirection = transform_.viewDirection();
        const Point3D clipForward{-clipViewDirection.x,
                                  -clipViewDirection.y,
                                  -clipViewDirection.z};
        const qreal meshNearDepth = clipPreferences.clipStart;
        const qreal meshFarDepth = transform_.isPerspectiveEnabled()
                                       ? clipPreferences.clipEnd
                                       : 2.0 * clipPreferences.clipEnd;
        const auto meshViewDepth = [&](const Point3D &point) {
            if (transform_.isPerspectiveEnabled()) {
                return (point.x - clipCamera.x) * clipForward.x +
                       (point.y - clipCamera.y) * clipForward.y +
                       (point.z - clipCamera.z) * clipForward.z;
            }
            return clipPreferences.clipEnd +
                   (point.x - clipTarget.x) * clipForward.x +
                   (point.y - clipTarget.y) * clipForward.y +
                   (point.z - clipTarget.z) * clipForward.z;
        };
        const auto appendClippedMeshTriangle =
            [&](const MeshTriangle &sourceTriangle) {
                QVector<MeshClipVertex> polygon;
                polygon.reserve(4);
                for (int corner = 0; corner < 3; ++corner) {
                    polygon.append({sourceTriangle.worldPoints[corner],
                                    sourceTriangle.uvs[corner],
                                    sourceTriangle.cornerNormalsView[corner],
                                    sourceTriangle.attributeColors[corner]});
                }
                const qreal clipPlanes[] = {meshNearDepth, meshFarDepth};
                for (int planeIndex = 0;
                     planeIndex < 2 && !polygon.isEmpty(); ++planeIndex) {
                    const bool keepGreater = planeIndex == 0;
                    const qreal planeDepth = clipPlanes[planeIndex];
                    QVector<MeshClipVertex> clipped;
                    clipped.reserve(polygon.size() + 1);
                    MeshClipVertex previous = polygon.last();
                    qreal previousDepth = meshViewDepth(previous.position);
                    bool previousInside = keepGreater
                                              ? previousDepth >= planeDepth
                                              : previousDepth <= planeDepth;
                    for (const MeshClipVertex &current : polygon) {
                        const qreal currentDepth = meshViewDepth(current.position);
                        const bool currentInside = keepGreater
                                                       ? currentDepth >= planeDepth
                                                       : currentDepth <= planeDepth;
                        if (currentInside != previousInside) {
                            const qreal denominator = currentDepth - previousDepth;
                            if (std::abs(denominator) > 1.0e-15) {
                                const qreal t = std::clamp(
                                    (planeDepth - previousDepth) / denominator,
                                    0.0, 1.0);
                                clipped.append({
                                    {previous.position.x +
                                         (current.position.x - previous.position.x) * t,
                                     previous.position.y +
                                         (current.position.y - previous.position.y) * t,
                                     previous.position.z +
                                         (current.position.z - previous.position.z) * t},
                                    previous.uv + (current.uv - previous.uv) * t,
                                    previous.normal + (current.normal - previous.normal) *
                                                          static_cast<float>(t),
                                    previous.attributeColor +
                                        (current.attributeColor -
                                         previous.attributeColor) *
                                            static_cast<float>(t)});
                            }
                        }
                        if (currentInside) clipped.append(current);
                        previous = current;
                        previousDepth = currentDepth;
                        previousInside = currentInside;
                    }
                    polygon = std::move(clipped);
                }
                for (int fanIndex = 1; fanIndex + 1 < polygon.size();
                     ++fanIndex) {
                    MeshTriangle triangle = sourceTriangle;
                    triangle.points.clear();
                    triangle.depth = 0.0;
                    const MeshClipVertex corners[] = {
                        polygon[0], polygon[fanIndex], polygon[fanIndex + 1]};
                    bool projectable = true;
                    for (int corner = 0; corner < 3; ++corner) {
                        triangle.worldPoints[corner] = corners[corner].position;
                        triangle.uvs[corner] = corners[corner].uv;
                        triangle.cornerNormalsView[corner] = corners[corner].normal;
                        triangle.attributeColors[corner] =
                            corners[corner].attributeColor;
                        QPointF screen;
                        if (!transform_.worldPointToScreenUnclipped(
                                corners[corner].position, viewportSize, &screen)) {
                            projectable = false;
                            break;
                        }
                        triangle.points.append(screen);
                        triangle.depth += transform_.worldDirectionToView(
                            corners[corner].position).towardCamera / 3.0;
                        if (transform_.isPerspectiveEnabled()) {
                            const qreal depth = meshViewDepth(corners[corner].position);
                            triangle.reciprocalDepths[corner] =
                                1.0 / std::max<qreal>(depth, 1.0e-9);
                        } else {
                            triangle.reciprocalDepths[corner] = 1.0;
                        }
                    }
                    if (projectable) {
                        const QTransform deviceTransform = painter.deviceTransform();
                        const QPointF screenA = deviceTransform.map(triangle.points[0]);
                        const QPointF screenB = deviceTransform.map(triangle.points[1]);
                        const QPointF screenC = deviceTransform.map(triangle.points[2]);
                        const qreal area =
                            (screenB.x() - screenA.x()) *
                                (screenC.y() - screenA.y()) -
                            (screenB.y() - screenA.y()) *
                                (screenC.x() - screenA.x());
                        bool frontFacing = area < 0.0;
                        if (reflectedMeshTransform) frontFacing = !frontFacing;
                        if (!shadingSettings_.backfaceCulling || frontFacing) {
                            triangles.append(std::move(triangle));
                        }
                    }
                }
            };
        for (int faceIndex = 0; faceIndex < mesh.faces.size(); ++faceIndex) {
            const QVector<int> &face = mesh.faces[faceIndex];
            QColor faceColor = curveColor;
            const bool useWorkbenchColor = !selected && !preview &&
                shadingSettings_.mode == ViewportShadingMode::Solid;
            QVector4D sourceColor = materialColorForFace(faceIndex);
            if (useWorkbenchColor) {
                switch (shadingSettings_.colorMode) {
                case ViewportColorMode::Material:
                case ViewportColorMode::Attribute:
                    break;
                case ViewportColorMode::Texture:
                    // The active image texture overrides this material color
                    // when the mesh has usable UVs and image data.
                    break;
                case ViewportColorMode::Object:
                    if (displayColorSceneLinear) {
                        sourceColor = *displayColorSceneLinear;
                    }
                    break;
                case ViewportColorMode::Random: {
                    const QColor random = workbenchRandomColor(
                        shapeObjectId, displayName, libraryPath);
                    sourceColor = QVector4D(random.redF(), random.greenF(),
                                            random.blueF(),
                                            displayColorSceneLinear
                                                ? displayColorSceneLinear->w()
                                                : 1.0f);
                    break;
                }
                case ViewportColorMode::Custom:
                    sourceColor = QVector4D(
                        shadingSettings_.customColor.redF(),
                        shadingSettings_.customColor.greenF(),
                        shadingSettings_.customColor.blueF(),
                        workbenchCustomColorSourceAlpha);
                    break;
                }
            }
            if (useWorkbenchColor) {
                const float packedSourceAlpha =
                    workbenchPackUnitFloatToUnorm8(sourceColor.w());
                faceColor = QColor::fromRgbF(
                    std::clamp(qreal(sourceColor.x()), 0.0, 1.0),
                    std::clamp(qreal(sourceColor.y()), 0.0, 1.0),
                    std::clamp(qreal(sourceColor.z()), 0.0, 1.0),
                    packedSourceAlpha);
            }
            QVector<Point3D> world;
            world.reserve(face.size());
            for (int index : face) {
                const Point3D &local = mesh.vertices[index];
                const auto &m = meshTransform;
                const qreal w = m[12] * local.x + m[13] * local.y +
                                m[14] * local.z + m[15];
                const qreal divisor = std::abs(w) > 1.0e-12 ? w : 1.0;
                world.append({(m[0] * local.x + m[1] * local.y +
                               m[2] * local.z + m[3]) / divisor + preparedOffset.x,
                              (m[4] * local.x + m[5] * local.y +
                               m[6] * local.z + m[7]) / divisor + preparedOffset.y,
                              (m[8] * local.x + m[9] * local.y +
                               m[10] * local.z + m[11]) / divisor + preparedOffset.z});
            }
            const auto faceTriangles =
                polygonMeshFaceTriangles(mesh, faceIndex, world);
            for (const std::array<int, 3> &loopTriangle : faceTriangles) {
                MeshTriangle triangle;
                const Point3D vertices[] = {world[loopTriangle[0]],
                                            world[loopTriangle[1]],
                                            world[loopTriangle[2]]};
                std::array<QPointF, 3> triangleUvs;
                bool hasUvs = activeUvMap != nullptr;
                if (hasUvs) {
                    for (int corner = 0; corner < 3; ++corner) {
                        const int uvIndex = firstCorner + loopTriangle[corner];
                        if (uvIndex < 0 || uvIndex >= activeUvMap->cornerValues.size()) {
                            hasUvs = false;
                            break;
                        }
                        const QPointF uv = activeUvMap->cornerValues[uvIndex];
                        if (!std::isfinite(uv.x()) || !std::isfinite(uv.y())) {
                            hasUvs = false;
                            break;
                        }
                        triangleUvs[corner] = QPointF(uv.x(), 1.0 - uv.y());
                    }
                }
                for (int corner = 0; corner < 3; ++corner) {
                    triangle.worldPoints[corner] = vertices[corner];
                    triangle.uvs[corner] = triangleUvs[corner];
                }
                {
                    triangle.color = faceColor;
                    if (useWorkbenchColor && shadingSettings_.colorMode ==
                            ViewportColorMode::Texture && hasUvs &&
                        textureForFace(faceIndex) != nullptr) {
                        const MeshTextureSet *textureSet = textureForFace(faceIndex);
                        triangle.texture = textureSet->image;
                        triangle.textureTiles = textureSet->tiles;
                        triangle.floatTexture = textureSet->floatImage;
                        triangle.floatTextureTiles = textureSet->floatTiles;
                        const int materialIndex =
                            mesh.faceMaterialIndices.value(faceIndex, 0);
                        triangle.tiledTexture = materialIndex >= 0 &&
                            materialIndex < meshMaterials.size() &&
                            !meshMaterials[materialIndex].textureImageTiles.isEmpty();
                        triangle.sceneLinearColor = QVector3D(
                            sourceColor.x(), sourceColor.y(), sourceColor.z());
                        if (materialIndex >= 0 &&
                            materialIndex < meshMaterials.size()) {
                            triangle.roughness = meshMaterials[materialIndex].roughness;
                            triangle.metallic = meshMaterials[materialIndex].metallic;
                            triangle.texturePremultiplied =
                                meshMaterials[materialIndex].textureImageAlphaMode
                                    .compare(QStringLiteral("PREMUL"),
                                             Qt::CaseInsensitive) == 0;
                            const QString alphaMode =
                                meshMaterials[materialIndex]
                                    .textureImageAlphaMode;
                            triangle.textureAlphaCutoff =
                                alphaMode.compare(QStringLiteral("NONE"),
                                                  Qt::CaseInsensitive) != 0 &&
                                alphaMode.compare(QStringLiteral("IGNORE"),
                                                  Qt::CaseInsensitive) != 0 &&
                                alphaMode.compare(
                                    QStringLiteral("CHANNEL_PACKED"),
                                    Qt::CaseInsensitive) != 0;
                            triangle.textureSrgb =
                                meshMaterials[materialIndex].textureImageSrgb;
                            triangle.textureInterpolation =
                                meshMaterials[materialIndex].textureImageInterpolation;
                            triangle.textureExtension =
                                meshMaterials[materialIndex].textureImageExtension;
                        }
                        triangle.uvs = triangleUvs;
                    }
                    if (useWorkbenchColor) {
                        triangle.useWorkbenchColor = true;
                        QVector3D sceneLinearColor(sourceColor.x(), sourceColor.y(),
                                                   sourceColor.z());
                        switch (shadingSettings_.colorMode) {
                        case ViewportColorMode::Random: {
                            const QColor random = workbenchRandomColor(
                                shapeObjectId, displayName, libraryPath);
                            sceneLinearColor = workbenchRec709ToSceneLinear(
                                QVector3D(random.redF(), random.greenF(),
                                          random.blueF()));
                            break;
                        }
                        case ViewportColorMode::Custom:
                            sceneLinearColor = workbenchSrgbToSceneLinear(
                                QVector3D(shadingSettings_.customColor.redF(),
                                          shadingSettings_.customColor.greenF(),
                                          shadingSettings_.customColor.blueF()));
                            break;
                        default:
                            break;
                        }
                        triangle.sceneLinearColor = sceneLinearColor;
                        triangle.sceneLinearAlpha =
                            workbenchPackUnitFloatToUnorm8(sourceColor.w());

                        const int materialIndex =
                            mesh.faceMaterialIndices.value(faceIndex, 0);
                        if (materialIndex >= 0 &&
                            materialIndex < meshMaterials.size()) {
                            triangle.roughness = meshMaterials[materialIndex].roughness;
                            triangle.metallic = meshMaterials[materialIndex].metallic;
                        } else if (materialIndex >= 0 &&
                                   materialIndex < mesh.materialSlots.size()) {
                            triangle.roughness =
                                mesh.materialSlots[materialIndex].roughness;
                            triangle.metallic =
                                mesh.materialSlots[materialIndex].metallic;
                        }
                        if (shadingSettings_.lightingMode ==
                            ViewportLightingMode::Studio) {
                            WorkbenchPackedMaterialData packedMaterial =
                                workbenchPackMaterialData(
                                    static_cast<float>(triangle.metallic),
                                    static_cast<float>(triangle.roughness),
                                    static_cast<float>(
                                        triangle.sceneLinearAlpha));
                            if (!shadingSettings_.xrayEnabled() &&
                                sourceColor.w() >= 1.0f - 1.0e-6f) {
                                packedMaterial =
                                    workbenchQuantizeMaterialLighting(
                                        packedMaterial);
                            }
                            triangle.roughness = packedMaterial.roughness;
                            triangle.metallic = packedMaterial.metallic;
                        }

                        const Point3D ab{vertices[1].x - vertices[0].x,
                                         vertices[1].y - vertices[0].y,
                                         vertices[1].z - vertices[0].z};
                        const Point3D ac{vertices[2].x - vertices[0].x,
                                         vertices[2].y - vertices[0].y,
                                         vertices[2].z - vertices[0].z};
                        const QVector3D normalWorld = workbenchNormalize(
                            QVector3D(static_cast<float>(ab.y * ac.z - ab.z * ac.y),
                                      static_cast<float>(ab.z * ac.x - ab.x * ac.z),
                                      static_cast<float>(ab.x * ac.y - ab.y * ac.x)));
                        const Point3D viewDirection = transform_.viewDirection();
                        const Point3D viewUp = transform_.viewUp();
                        const QVector3D worldFacing(
                            static_cast<float>(viewDirection.x),
                            static_cast<float>(viewDirection.y),
                            static_cast<float>(viewDirection.z));
                        const QVector3D worldUp(static_cast<float>(viewUp.x),
                                                static_cast<float>(viewUp.y),
                                                static_cast<float>(viewUp.z));
                        const QVector3D worldRight = workbenchNormalize(
                            QVector3D::crossProduct(worldUp, worldFacing));
                        triangle.normalView = {
                            QVector3D::dotProduct(normalWorld, worldRight),
                            QVector3D::dotProduct(normalWorld, worldUp),
                            QVector3D::dotProduct(normalWorld, worldFacing)};
                        for (int corner = 0; corner < 3; ++corner) {
                            QVector3D cornerNormalWorld = normalWorld;
                            const int loopIndex = firstCorner +
                                loopTriangle[corner];
                            if (loopIndex >= 0 &&
                                loopIndex < mesh.cornerNormals.size()) {
                                const QVector3D transformedNormal =
                                    transformWorkbenchMeshNormal(
                                        meshTransform,
                                        mesh.cornerNormals[loopIndex]);
                                if (transformedNormal.lengthSquared() > 1.0e-16f) {
                                    cornerNormalWorld = transformedNormal;
                                }
                            }
                            triangle.cornerNormalsView[corner] = {
                                QVector3D::dotProduct(cornerNormalWorld,
                                                     worldRight),
                                QVector3D::dotProduct(cornerNormalWorld,
                                                     worldUp),
                                QVector3D::dotProduct(cornerNormalWorld,
                                                     worldFacing)};
                        }
                        const Point3D center{
                            (vertices[0].x + vertices[1].x + vertices[2].x) / 3.0,
                            (vertices[0].y + vertices[1].y + vertices[2].y) / 3.0,
                            (vertices[0].z + vertices[1].z + vertices[2].z) / 3.0};
                        triangle.incidentView = {0.0f, 0.0f, 1.0f};
                        if (transform_.isPerspectiveEnabled()) {
                            const Point3D camera =
                                transform_.cameraPosition(viewportSize);
                            const QVector3D incidentWorld = workbenchNormalize(
                                QVector3D(static_cast<float>(camera.x - center.x),
                                          static_cast<float>(camera.y - center.y),
                                          static_cast<float>(camera.z - center.z)),
                                worldFacing);
                            triangle.incidentView = {
                                QVector3D::dotProduct(incidentWorld, worldRight),
                                QVector3D::dotProduct(incidentWorld, worldUp),
                                QVector3D::dotProduct(incidentWorld, worldFacing)};
                        }
                    }
                    if (useWorkbenchColor &&
                        shadingSettings_.colorMode == ViewportColorMode::Attribute) {
                        const std::optional<std::array<QVector3D, 3>> colors =
                            attributeColor(faceIndex, face, loopTriangle);
                        if (colors) {
                            triangle.attributeColors = *colors;
                            triangle.hasAttributeColors = true;
                        }
                    }
                    appendClippedMeshTriangle(triangle);
                }
            }
            firstCorner += face.size();
        }
        std::stable_sort(triangles.begin(), triangles.end(),
                         [](const MeshTriangle &a, const MeshTriangle &b) {
                             return a.depth < b.depth;
                         });
        // Mark device pixels touched by fractional triangle coverage. The
        // mesh painter path draws each triangle into its own image, so those
        // pixels need a per-object resolve to avoid blending adjacent pieces
        // of one surface as separate translucent layers.
        const QTransform meshDeviceTransform = painter.deviceTransform();
        const int meshDeviceWidth = painter.device()->width();
        const int meshDeviceHeight = painter.device()->height();
        constexpr int kMaximumMeshCoverageResolvePixels = 250'000;
        constexpr qint64 kMaximumMeshCoverageResolveTests = 8 * 1024 * 1024;
        constexpr qint64 kMaximumMeshCoverageResolveEntries = 1'000'000;
        bool meshCoverageResolveEnabled =
            meshDeviceWidth > 0 && meshDeviceHeight > 0 &&
            qint64(meshDeviceWidth) * meshDeviceHeight <= 16 * 1024 * 1024;
        qint64 meshFractionalCoverageEntries = 0;
        qint64 meshCoverageResolveTests = 0;
        QSet<quint64> meshFractionalCoveragePixels;
        for (const MeshTriangle &triangle : triangles) {
            if (!meshCoverageResolveEnabled) break;
            if (triangle.points.size() != 3 ||
                (!triangle.useWorkbenchColor && triangle.texture.isNull() &&
                 triangle.textureTiles.isEmpty() &&
                 !triangle.floatTexture.isValid() &&
                 triangle.floatTextureTiles.isEmpty())) {
                continue;
            }
            const std::array<QPointF, 3> points{
                meshDeviceTransform.map(triangle.points[0]),
                meshDeviceTransform.map(triangle.points[1]),
                meshDeviceTransform.map(triangle.points[2])};
            const qreal area =
                (points[1].x() - points[0].x()) *
                    (points[2].y() - points[0].y()) -
                (points[1].y() - points[0].y()) *
                    (points[2].x() - points[0].x());
            if (std::abs(area) < 1.0e-12) continue;
            const int left = std::clamp(
                qFloor(std::min({points[0].x(), points[1].x(), points[2].x()})),
                0, meshDeviceWidth);
            const int right = std::clamp(
                qCeil(std::max({points[0].x(), points[1].x(), points[2].x()})),
                0, meshDeviceWidth);
            const int top = std::clamp(
                qFloor(std::min({points[0].y(), points[1].y(), points[2].y()})),
                0, meshDeviceHeight);
            const int bottom = std::clamp(
                qCeil(std::max({points[0].y(), points[1].y(), points[2].y()})),
                0, meshDeviceHeight);
            if (right - left > 16384 || bottom - top > 16384 ||
                qint64(right - left) * (bottom - top) > 64 * 1024 * 1024) {
                meshCoverageResolveEnabled = false;
                meshFractionalCoveragePixels.clear();
                break;
            }
            meshCoverageResolveTests +=
                qint64(right - left) * qint64(bottom - top);
            if (meshCoverageResolveTests > kMaximumMeshCoverageResolveTests) {
                meshCoverageResolveEnabled = false;
                meshFractionalCoveragePixels.clear();
                break;
            }
            for (int y = top; y < bottom && meshCoverageResolveEnabled; ++y) {
                for (int x = left; x < right; ++x) {
                    qreal w0, w1, w2;
                    const qreal coverage = workbenchTrianglePixelCoverage(
                        points, area, x + 0.5, y + 0.5, 1.0, 1.0,
                        &w0, &w1, &w2, nullptr,
                        workbenchRasterAntialiasingEnabled_ ? 4 : 1);
                    if (coverage > 0.0 && coverage < 1.0) {
                        ++meshFractionalCoverageEntries;
                        if (meshFractionalCoverageEntries >
                            kMaximumMeshCoverageResolveEntries) {
                            meshCoverageResolveEnabled = false;
                            meshFractionalCoveragePixels.clear();
                            break;
                        }
                        meshFractionalCoveragePixels.insert(
                            static_cast<quint64>(y) *
                                static_cast<quint64>(meshDeviceWidth) +
                            static_cast<quint64>(x));
                        if (meshFractionalCoveragePixels.size() >
                            kMaximumMeshCoverageResolvePixels) {
                            meshCoverageResolveEnabled = false;
                            meshFractionalCoveragePixels.clear();
                            break;
                        }
                    }
                }
            }
        }
        struct MeshCoverageFragment {
            qreal depth = 0.0;
            qreal coverage = 0.0;
            quint16 sampleMask = 0;
            qreal alpha = 0.0;
            QVector3D displayRgb;
            QVector3D sceneLinearRgb;
            Point3D worldPosition;
            QVector3D worldNormal;
        };
        QHash<quint64, QVector<MeshCoverageFragment>> meshCoverageFragments;
        painter.save();
        painter.setPen(selected ? QPen(curveColor, 1.5) : Qt::NoPen);
        WorkbenchStudioLighting meshViewLighting = workbenchStudioLightingPreset(
            shadingSettings_.studioLightPreset);
        meshViewLighting.useSpecular = shadingSettings_.specularLighting &&
                                       meshViewLighting.useSpecular;
        const Point3D meshViewDirection = transform_.viewDirection();
        const Point3D meshViewUp = transform_.viewUp();
        const QVector3D meshViewFacing(
            static_cast<float>(meshViewDirection.x),
            static_cast<float>(meshViewDirection.y),
            static_cast<float>(meshViewDirection.z));
        const QVector3D meshViewUpVector(
            static_cast<float>(meshViewUp.x), static_cast<float>(meshViewUp.y),
            static_cast<float>(meshViewUp.z));
        const QVector3D meshViewRight = workbenchNormalize(
            QVector3D::crossProduct(meshViewUpVector, meshViewFacing));
        const Point3D meshCameraPosition =
            transform_.cameraPosition(viewportSize);
        meshViewLighting = workbenchStudioLightingForView(
            meshViewLighting, shadingSettings_.studioLightRotationDegrees,
            shadingSettings_.worldSpaceLighting, meshViewRight,
            meshViewUpVector, meshViewFacing);
        const auto drawWorkbenchTriangle = [&](const MeshTriangle &triangle) {
            const QImage &texture = triangle.texture;
            const bool hasFloatTexture = triangle.floatTexture.isValid() ||
                                         !triangle.floatTextureTiles.isEmpty();
            const bool hasTexture = hasFloatTexture || !texture.isNull() ||
                                    !triangle.textureTiles.isEmpty();
            const QTransform deviceTransform = painter.deviceTransform();
            bool inverseValid = false;
            const QTransform logicalFromDevice =
                deviceTransform.inverted(&inverseValid);
            if (!inverseValid) return;
            const std::array<QPointF, 3> devicePoints{
                deviceTransform.map(triangle.points[0]),
                deviceTransform.map(triangle.points[1]),
                deviceTransform.map(triangle.points[2])};
            const qreal area =
                (devicePoints[1].x() - devicePoints[0].x()) *
                    (devicePoints[2].y() - devicePoints[0].y()) -
                (devicePoints[1].y() - devicePoints[0].y()) *
                    (devicePoints[2].x() - devicePoints[0].x());
            if (std::abs(area) < 1.0e-12 ||
                (!hasTexture && !triangle.useWorkbenchColor)) return;
            bool frontFacing = area < 0.0;
            if (reflectedMeshTransform) frontFacing = !frontFacing;
            const qreal minX = std::clamp(
                std::min({devicePoints[0].x(), devicePoints[1].x(), devicePoints[2].x()}),
                0.0, qreal(painter.device()->width()));
            const qreal maxX = std::clamp(
                std::max({devicePoints[0].x(), devicePoints[1].x(), devicePoints[2].x()}),
                0.0, qreal(painter.device()->width()));
            const qreal minY = std::clamp(
                std::min({devicePoints[0].y(), devicePoints[1].y(), devicePoints[2].y()}),
                0.0, qreal(painter.device()->height()));
            const qreal maxY = std::clamp(
                std::max({devicePoints[0].y(), devicePoints[1].y(), devicePoints[2].y()}),
                0.0, qreal(painter.device()->height()));
            if (maxX <= minX || maxY <= minY) return;
            const qreal dpr = painter.device() != nullptr
                ? painter.device()->devicePixelRatioF() : 1.0;
            const int left = qFloor(minX);
            const int top = qFloor(minY);
            const int width = std::max(1, qCeil(maxX) - left);
            const int height = std::max(1, qCeil(maxY) - top);
            constexpr qint64 maximumRasterSamples = 4000000;
            const qint64 rasterArea = qint64(width) * height;
            const qreal sampleScale =
                std::max({qreal(1.0),
                          std::sqrt(static_cast<qreal>(rasterArea) /
                                    static_cast<qreal>(maximumRasterSamples)),
                          static_cast<qreal>(width) / 16384.0,
                          static_cast<qreal>(height) / 16384.0});
            const int sampleWidth = std::max(
                1, qFloor(static_cast<qreal>(width) / sampleScale));
            const int sampleHeight = std::max(
                1, qFloor(static_cast<qreal>(height) / sampleScale));
            const qreal sampleStepX =
                static_cast<qreal>(width) / sampleWidth;
            const qreal sampleStepY =
                static_cast<qreal>(height) / sampleHeight;
            QImage raster(sampleWidth, sampleHeight, QImage::Format_RGBA8888);
            raster.setDevicePixelRatio(dpr);
            raster.fill(Qt::transparent);
            const qreal alpha = hasTexture ? triangle.color.alphaF()
                                           : triangle.sceneLinearAlpha;
            for (int y = 0; y < sampleHeight; ++y) {
                uchar *row = raster.scanLine(y);
                const qreal py = top + (y + 0.5) * sampleStepY;
                for (int x = 0; x < sampleWidth; ++x) {
                    const qreal px = left + (x + 0.5) * sampleStepX;
                    qreal b0, b1, b2;
                    quint16 sampleMask = 0;
                    const qreal coverage = workbenchTrianglePixelCoverage(
                        devicePoints, area, px, py, sampleStepX, sampleStepY,
                        &b0, &b1, &b2, &sampleMask,
                        workbenchRasterAntialiasingEnabled_ ? 4 : 1);
                    if (coverage <= 0.0) continue;
                    const qreal p0 = b0 * triangle.reciprocalDepths[0];
                    const qreal p1 = b1 * triangle.reciprocalDepths[1];
                    const qreal p2 = b2 * triangle.reciprocalDepths[2];
                    const qreal denominator = p0 + p1 + p2;
                    if (denominator <= 1.0e-12) continue;
                    uchar *pixel = row + x * 4;
                    QVector3D textureColor = triangle.sceneLinearColor;
                    qreal pixelAlpha = alpha;
                    if (hasTexture) {
                        const QPointF uv =
                            (triangle.uvs[0] * p0 + triangle.uvs[1] * p1 +
                             triangle.uvs[2] * p2) / denominator;
                        // Mesh raster UVs are pre-flipped for top-down QImage
                        // sampling. UDIM row selection must recover Blender's
                        // original bottom-up UV before looking up a tile.
                        const QPointF udimUv(uv.x(), 1.0 - uv.y());
                        uchar rgba[4]{};
                        float linearRgb[3]{};
                        float floatRgba[4]{};
                        if (hasFloatTexture) {
                            const bool sampled = triangle.tiledTexture
                                ? sampleWorkbenchFloatUdimImage(
                                    triangle.floatTextureTiles, udimUv,
                                    triangle.textureInterpolation,
                                    triangle.textureExtension, floatRgba)
                                : (sampleWorkbenchFloatImage(
                                    triangle.floatTexture, uv,
                                    triangle.textureInterpolation,
                                    triangle.textureExtension, floatRgba),
                                   triangle.floatTexture.isValid());
                            if (!sampled) {
                                pixel[0] = 255; pixel[1] = 0;
                                pixel[2] = 255; pixel[3] = 255;
                                continue;
                            }
                        } else if (triangle.tiledTexture) {
                            if (!sampleWorkbenchUdimImage(
                                    triangle.textureTiles, udimUv,
                                    triangle.textureInterpolation,
                                    triangle.textureExtension, rgba,
                                    linearRgb, triangle.textureSrgb)) {
                                // Blender Workbench displays absent UDIM tiles as
                                // magenta so the incomplete image stays visible.
                                pixel[0] = 255;
                                pixel[1] = 0;
                                pixel[2] = 255;
                                pixel[3] = 255;
                                continue;
                            }
                        } else {
                            sampleWorkbenchImage(texture, uv,
                                                 triangle.textureInterpolation,
                                                 triangle.textureExtension, rgba,
                                                 linearRgb,
                                                 triangle.textureSrgb);
                        }
                        const float textureAlpha = hasFloatTexture
                            ? floatRgba[3] : rgba[3] / 255.0f;
                        if (triangle.textureAlphaCutoff &&
                            textureAlpha < 0.1f) {
                            continue; // Blender's filtered alpha cutoff.
                        }
                        float unassociatedRgb[3]{};
                        for (int component = 0; component < 3; ++component) {
                            unassociatedRgb[component] = hasFloatTexture
                                ? (triangle.texturePremultiplied && textureAlpha > 0.0f
                                       ? floatRgba[component] / textureAlpha
                                       : floatRgba[component])
                                : (triangle.texturePremultiplied &&
                                           rgba[3] != 0 && rgba[3] != 255
                                       ? linearRgb[component] * 255.0f / rgba[3]
                                       : linearRgb[component]);
                        }
                        for (int component = 0; component < 3; ++component)
                            textureColor[component] = unassociatedRgb[component];
                    }
                    if (triangle.hasAttributeColors && !hasTexture) {
                        textureColor =
                            (triangle.attributeColors[0] * static_cast<float>(p0) +
                             triangle.attributeColors[1] * static_cast<float>(p1) +
                             triangle.attributeColors[2] * static_cast<float>(p2)) /
                            static_cast<float>(denominator);
                    }
                    QVector3D shadedLinear = textureColor;
                    QVector3D normalView =
                        (triangle.cornerNormalsView[0] * static_cast<float>(p0) +
                         triangle.cornerNormalsView[1] * static_cast<float>(p1) +
                         triangle.cornerNormalsView[2] * static_cast<float>(p2)) /
                        static_cast<float>(denominator);
                    if (normalView.lengthSquared() <= 1.0e-16f) {
                        normalView = triangle.normalView;
                    } else {
                        normalView.normalize();
                    }
                    // Blender's opaque prepass flips backface normals before
                    // encoding them. Its transparent pass keeps interpolated
                    // mesh normals unchanged.
                    if (alpha >= 1.0 && !frontFacing) {
                        normalView = -normalView;
                    }
                    QVector3D incidentView = triangle.incidentView;
                    if (transform_.isPerspectiveEnabled()) {
                        // Workbench derives its incident vector per fragment.
                        // Reconstruct this pixel's perspective-correct world
                        // position using the same weights as texture sampling.
                        const Point3D &point0 = triangle.worldPoints[0];
                        const Point3D &point1 = triangle.worldPoints[1];
                        const Point3D &point2 = triangle.worldPoints[2];
                        const Point3D samplePosition{
                            (point0.x * p0 + point1.x * p1 + point2.x * p2) /
                                denominator,
                            (point0.y * p0 + point1.y * p1 + point2.y * p2) /
                                denominator,
                            (point0.z * p0 + point1.z * p1 + point2.z * p2) /
                                denominator};
                        const QVector3D incidentWorld = workbenchNormalize(
                            QVector3D(
                                static_cast<float>(meshCameraPosition.x - samplePosition.x),
                                static_cast<float>(meshCameraPosition.y - samplePosition.y),
                                static_cast<float>(meshCameraPosition.z - samplePosition.z)),
                            meshViewFacing);
                        incidentView = {
                            QVector3D::dotProduct(incidentWorld, meshViewRight),
                            QVector3D::dotProduct(incidentWorld, meshViewUpVector),
                            QVector3D::dotProduct(incidentWorld, meshViewFacing)};
                    }
                    if (shadingSettings_.lightingMode ==
                        ViewportLightingMode::Studio) {
                        shadedLinear = workbenchStudioShade(
                            textureColor, normalView,
                            incidentView,
                            static_cast<float>(triangle.roughness),
                            static_cast<float>(triangle.metallic),
                            meshViewLighting);
                    } else if (shadingSettings_.lightingMode ==
                               ViewportLightingMode::MatCap) {
                        shadedLinear = workbenchMatcapShade(
                            shadingSettings_.matcapPreset, textureColor,
                            normalView, incidentView,
                            shadingSettings_.specularLighting,
                            shadingSettings_.matcapFlipX);
                    }
                    if (shadingSettings_.xrayEnabled()) {
                        pixelAlpha *= shadingSettings_.xrayAlphaForMode();
                    }
                    const qreal fragmentAlpha = pixelAlpha;
                    pixelAlpha *= coverage;
                    if (fragmentSink != nullptr &&
                        !meshFractionalCoveragePixels.contains(
                            static_cast<quint64>(qFloor(py)) *
                                static_cast<quint64>(meshDeviceWidth) +
                            static_cast<quint64>(qFloor(px)))) {
                        const Point3D &point0 = triangle.worldPoints[0];
                        const Point3D &point1 = triangle.worldPoints[1];
                        const Point3D &point2 = triangle.worldPoints[2];
                        const Point3D fragmentPosition{
                            (point0.x * p0 + point1.x * p1 + point2.x * p2) /
                                denominator,
                            (point0.y * p0 + point1.y * p1 + point2.y * p2) /
                                denominator,
                            (point0.z * p0 + point1.z * p1 + point2.z * p2) /
                                denominator};
                        observeWorkbenchFragment(
                            fragmentSink, painter,
                            logicalFromDevice.map(QPointF(px, py)),
                            fragmentPosition,
                            normalView.x() * meshViewRight +
                                normalView.y() * meshViewUpVector +
                                normalView.z() * meshViewFacing,
                            meshCameraPosition,
                            meshViewDirection, shadedLinear, pixelAlpha,
                            shapeObjectId, transform_);
                    }
                    const QVector3D displayColor =
                        workbenchSceneLinearToDisplaySrgb(
                            shadedLinear, shadingSettings_.colorTransform,
                            shadingSettings_.colorExposure,
                            shadingSettings_.colorGamma,
                            shadingSettings_.colorLutData,
                            shadingSettings_.colorLutMetadata);
                    const quint64 pixelKey =
                        static_cast<quint64>(qFloor(py)) *
                            static_cast<quint64>(meshDeviceWidth) +
                        static_cast<quint64>(qFloor(px));
                    if (meshFractionalCoveragePixels.contains(pixelKey)) {
                        const Point3D &point0 = triangle.worldPoints[0];
                        const Point3D &point1 = triangle.worldPoints[1];
                        const Point3D &point2 = triangle.worldPoints[2];
                        const Point3D worldPosition{
                            (point0.x * p0 + point1.x * p1 + point2.x * p2) /
                                denominator,
                            (point0.y * p0 + point1.y * p1 + point2.y * p2) /
                                denominator,
                            (point0.z * p0 + point1.z * p1 + point2.z * p2) /
                                denominator};
                        const qreal depth = -(
                            (worldPosition.x - meshCameraPosition.x) *
                                meshViewDirection.x +
                            (worldPosition.y - meshCameraPosition.y) *
                                meshViewDirection.y +
                            (worldPosition.z - meshCameraPosition.z) *
                                meshViewDirection.z);
                        meshCoverageFragments[pixelKey].append(
                            {depth, coverage, sampleMask, fragmentAlpha,
                             displayColor, shadedLinear, worldPosition,
                             normalView.x() * meshViewRight +
                                 normalView.y() * meshViewUpVector +
                                 normalView.z() * meshViewFacing});
                    } else {
                        pixel[0] = static_cast<uchar>(std::clamp(
                            qRound(displayColor.x() * 255.0f), 0, 255));
                        pixel[1] = static_cast<uchar>(std::clamp(
                            qRound(displayColor.y() * 255.0f), 0, 255));
                        pixel[2] = static_cast<uchar>(std::clamp(
                            qRound(displayColor.z() * 255.0f), 0, 255));
                        pixel[3] = static_cast<uchar>(qRound(pixelAlpha * 255.0));
                    }
                }
            }
            painter.save();
            painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
            painter.setWorldTransform(QTransform());
            bool baseInverseValid = false;
            const QTransform baseLogicalFromDevice =
                painter.deviceTransform().inverted(&baseInverseValid);
            if (baseInverseValid) {
                const QRectF targetRect(
                    baseLogicalFromDevice.map(QPointF(left, top)),
                    baseLogicalFromDevice.map(
                        QPointF(left + width, top + height)));
                painter.drawImage(
                    targetRect.normalized(), raster);
            }
            painter.restore();
        };
        for (const MeshTriangle &triangle : triangles) {
            if (triangle.points.size() == 3 &&
                ((!triangle.texture.isNull() || triangle.tiledTexture) ||
                 triangle.useWorkbenchColor)) {
                drawWorkbenchTriangle(triangle);
            } else {
                painter.setBrush(triangle.color);
                painter.drawPolygon(triangle.points);
            }
        }
        if (!meshCoverageFragments.isEmpty()) {
            QImage edgeRaster(meshDeviceWidth, meshDeviceHeight,
                              QImage::Format_ARGB32_Premultiplied);
            edgeRaster.setDevicePixelRatio(painter.device()->devicePixelRatioF());
            edgeRaster.fill(Qt::transparent);
            struct ResolvedEdgePixel {
                float red = 0.0f;
                float green = 0.0f;
                float blue = 0.0f;
                float alpha = 0.0f;
            };
            QHash<quint64, ResolvedEdgePixel> resolvedEdgePixels;
            const auto appendLayer = [&](ResolvedEdgePixel &destination,
                                         const QVector3D &displayRgb,
                                         qreal alpha) {
                const qreal sourceAlpha = std::clamp(alpha, 0.0, 1.0);
                const qreal transmittance = 1.0 - sourceAlpha;
                destination.red = static_cast<float>(
                    displayRgb.x() * sourceAlpha +
                    destination.red * transmittance);
                destination.green = static_cast<float>(
                    displayRgb.y() * sourceAlpha +
                    destination.green * transmittance);
                destination.blue = static_cast<float>(
                    displayRgb.z() * sourceAlpha +
                    destination.blue * transmittance);
                destination.alpha = static_cast<float>(
                    sourceAlpha + destination.alpha * transmittance);
            };
            bool inverseValid = false;
            const QTransform logicalFromDevice =
                meshDeviceTransform.inverted(&inverseValid);
            for (auto iterator = meshCoverageFragments.begin();
                 iterator != meshCoverageFragments.end(); ++iterator) {
                const quint64 key = iterator.key();
                QVector<MeshCoverageFragment> &fragments = iterator.value();
                std::stable_sort(
                    fragments.begin(), fragments.end(),
                    [](const MeshCoverageFragment &a,
                       const MeshCoverageFragment &b) {
                        return a.depth < b.depth;
                    });
                const int x = static_cast<int>(key % meshDeviceWidth);
                const int y = static_cast<int>(key / meshDeviceWidth);
                ResolvedEdgePixel &resolved = resolvedEdgePixels[key];
                for (int first = 0; first < fragments.size();) {
                    int end = first + 1;
                    const qreal referenceDepth = fragments[first].depth;
                    const qreal depthTolerance =
                        1.0e-8 * std::max<qreal>(1.0,
                                                 std::abs(referenceDepth));
                    while (end < fragments.size() &&
                           std::abs(fragments[end].depth - referenceDepth) <=
                               depthTolerance) {
                        ++end;
                    }
                    qreal coverageSum = 0.0;
                    qreal alphaSum = 0.0;
                    quint16 coveredSampleMask = 0;
                    bool disjointSamples = true;
                    QVector3D displayPremultiplied;
                    QVector3D sceneLinearPremultiplied;
                    Point3D worldPosition{};
                    QVector3D worldNormal;
                    for (int index = first; index < end; ++index) {
                        const MeshCoverageFragment &fragment = fragments[index];
                        const qreal coveredAlpha = fragment.alpha *
                                                   fragment.coverage;
                        disjointSamples &=
                            (coveredSampleMask & fragment.sampleMask) == 0;
                        coveredSampleMask |= fragment.sampleMask;
                        coverageSum += fragment.coverage;
                        alphaSum += coveredAlpha;
                        displayPremultiplied +=
                            fragment.displayRgb * static_cast<float>(coveredAlpha);
                        sceneLinearPremultiplied +=
                            fragment.sceneLinearRgb *
                            static_cast<float>(coveredAlpha);
                        worldPosition.x += fragment.worldPosition.x *
                                           fragment.coverage;
                        worldPosition.y += fragment.worldPosition.y *
                                           fragment.coverage;
                        worldPosition.z += fragment.worldPosition.z *
                                           fragment.coverage;
                        worldNormal += fragment.worldNormal *
                                       static_cast<float>(fragment.coverage);
                    }
                    const bool disjointCoverage =
                        disjointSamples && coverageSum <= 1.0 + 1.0e-6;
                    if (disjointCoverage) {
                        const qreal alpha = std::clamp(alphaSum, 0.0, 1.0);
                        if (alphaSum > 0.0) {
                            const QVector3D displayRgb =
                                displayPremultiplied /
                                static_cast<float>(alphaSum);
                            const QVector3D sceneLinearRgb =
                                sceneLinearPremultiplied /
                                static_cast<float>(alphaSum);
                            appendLayer(resolved, displayRgb, alpha);
                            if (fragmentSink != nullptr && inverseValid) {
                                const qreal inverseCoverage =
                                    coverageSum > 0.0 ? 1.0 / coverageSum : 0.0;
                                worldPosition.x *= inverseCoverage;
                                worldPosition.y *= inverseCoverage;
                                worldPosition.z *= inverseCoverage;
                                observeWorkbenchFragment(
                                    fragmentSink, painter,
                                    logicalFromDevice.map(
                                        QPointF(x + 0.5, y + 0.5)),
                                    worldPosition,
                                    worldNormal * static_cast<float>(
                                        coverageSum > 0.0 ?
                                            1.0 / coverageSum : 0.0),
                                    meshCameraPosition,
                                    meshViewDirection, sceneLinearRgb, alpha,
                                    shapeObjectId, transform_);
                            }
                        }
                    } else {
                        // Overlapping coplanar triangles may be intentional
                        // layers. Keep their original independent coverage.
                        for (int index = first; index < end; ++index) {
                            const MeshCoverageFragment &fragment =
                                fragments[index];
                            const qreal alpha = fragment.alpha *
                                                fragment.coverage;
                            appendLayer(resolved, fragment.displayRgb, alpha);
                            if (fragmentSink != nullptr && inverseValid) {
                                observeWorkbenchFragment(
                                    fragmentSink, painter,
                                    logicalFromDevice.map(
                                        QPointF(x + 0.5, y + 0.5)),
                                    fragment.worldPosition,
                                    fragment.worldNormal,
                                    meshCameraPosition, meshViewDirection,
                                    fragment.sceneLinearRgb, alpha,
                                    shapeObjectId, transform_);
                            }
                        }
                    }
                    first = end;
                }
                if (resolved.alpha > 0.0f) {
                    const qreal inverseAlpha = 1.0 / resolved.alpha;
                    const QRgb pixel = qRgba(
                        qRound(std::clamp<qreal>(resolved.red * inverseAlpha,
                                                  0.0, 1.0) * 255.0),
                        qRound(std::clamp<qreal>(resolved.green * inverseAlpha,
                                                  0.0, 1.0) * 255.0),
                        qRound(std::clamp<qreal>(resolved.blue * inverseAlpha,
                                                  0.0, 1.0) * 255.0),
                        qRound(std::clamp<qreal>(resolved.alpha, 0.0, 1.0) *
                               255.0));
                    reinterpret_cast<QRgb *>(edgeRaster.scanLine(y))[x] =
                        qPremultiply(pixel);
                }
            }
            painter.save();
            painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
            painter.setWorldTransform(QTransform());
            bool baseInverseValid = false;
            const QTransform baseLogicalFromDevice =
                painter.deviceTransform().inverted(&baseInverseValid);
            if (baseInverseValid) {
                painter.drawImage(baseLogicalFromDevice.map(QPointF(0.0, 0.0)),
                                  edgeRaster);
            }
            painter.restore();
        }
        painter.restore();
        return;
    }

    const bool canUsePreparedCurve =
        preparedGeometry != nullptr && smoothCurveDisplay_ &&
        (selected || preview || layerLineType.isEmpty() ||
         layerLineType.compare(QStringLiteral("Continuous"),
                               Qt::CaseInsensitive) == 0);
    const auto drawCurve = [&](const Shape::NurbsCurve2D &curve,
                               const WorkPlaneFrame &frame) {
        if (!isValidNurbsCurve(curve)) {
            return;
        }
        // The committed curve stroke is an Overlay wire in both the direct
        // evaluator and prepared-geometry paths. Keep its QPainter fallback
        // controlled by the Overlay preference just like primitive and
        // surface wire strokes; otherwise these NURBS paths inherit whatever
        // antialiasing state a previous draw left on the painter.
        painter.save();
        if (!preview) {
            painter.setRenderHint(QPainter::Antialiasing,
                                  smoothWiresOverlay_);
        }
        bool curveDrawn = canUsePreparedCurve &&
            drawPreparedCurveSegments(
                painter, *preparedGeometry, viewportSize,
                preview ? nullptr : opaqueSceneDepth,
                opaqueSceneDepthSize);
        if (!curveDrawn) {
            WorkPlaneFrame placedFrame = frame;
            placedFrame.origin.x += preparedOffset.x;
            placedFrame.origin.y += preparedOffset.y;
            placedFrame.origin.z += preparedOffset.z;
            if (!preview && opaqueSceneDepth != nullptr) {
                Shape directCurve;
                directCurve.geometryType = GeometryType::Line;
                directCurve.points = curve.controlPoints;
                directCurve.nurbs = curve;
                directCurve.workPlaneFrame = placedFrame;
                const ViewportDepthGeometry sampledCurve =
                    buildViewportDepthGeometry(directCurve);
                curveDrawn = drawPreparedCurveSegments(
                    painter, sampledCurve, viewportSize, opaqueSceneDepth,
                    opaqueSceneDepthSize);
            }
            if (!curveDrawn) {
                drawNurbsCurve(painter, curve, placedFrame, viewportSize);
            }
        }
        painter.restore();
    };

    if (isDimensionGeometryType(shape.geometryType)) {
        const DimensionScreenLayout layout =
            buildDimensionScreenLayout(
                shape,
                transform_,
                viewportSize,
                architecturalDimensionFont_ ? DimensionFontStyle::Architectural
                                            : DimensionFontStyle::Standard);
        if (layout.valid) {
            painter.save();
            const qreal dimensionLineWidth =
                selected ? 2.0 : (preview ? 1.5 : 1.25);
            const QPen dimensionPen = selected || preview
                                          ? QPen(curveColor,
                                                 dimensionLineWidth,
                                                 Qt::SolidLine,
                                                 Qt::RoundCap,
                                                 Qt::RoundJoin)
                                          : layerLineTypePen(curveColor,
                                                             dimensionLineWidth,
                                                             layerLineType,
                                                             Qt::RoundCap);
            painter.setPen(dimensionPen);
            for (const QLineF &line : layout.lines) {
                painter.drawLine(line);
            }
            painter.setBrush(curveColor);
            for (const QPolygonF &arrowHead : layout.arrowHeads) {
                painter.drawPolygon(arrowHead);
            }

            painter.setFont(dimensionAnnotationFont(
                architecturalDimensionFont_ ? DimensionFontStyle::Architectural
                                            : DimensionFontStyle::Standard));
            QColor labelBackground(QStringLiteral("#282828"));
            if (linearAnnotationTarget) {
                labelBackground = colorForLinearAnnotation(labelBackground);
            }
            painter.fillRect(layout.labelBounds, labelBackground);
            painter.setPen(curveColor);
            painter.drawText(layout.labelBounds, Qt::AlignCenter, layout.label);
            painter.restore();
        }
        return;
    }

    if (shape.geometryType == GeometryType::Picture) {
        const QVector<QPointF> corners = pictureFrameCorners(shape);
        if (shape.pictureImage.isNull() || corners.size() != 4 ||
            !viewportPictureVisibleInView(shape, transform_, viewportSize)) {
            return;
        }

        QPolygonF sourceCorners;
        sourceCorners << QPointF(0.0, 0.0)
                      << QPointF(shape.pictureImage.width(), 0.0)
                      << QPointF(shape.pictureImage.width(), shape.pictureImage.height())
                      << QPointF(0.0, shape.pictureImage.height());
        QPolygonF screenCorners;
        for (const QPointF &corner : corners) {
            screenCorners.append(worldToScreen(corner, viewportSize));
        }

        QTransform imageTransform;
        if (QTransform::quadToQuad(sourceCorners, screenCorners, imageTransform)) {
            QImage depthMaskedImage;
            bool drawDepthMaskedImage = false;
            constexpr qint64 maximumPictureDepthPixels = 16'000'000;
            if (!preview && shape.pictureImageDepth != PictureImageDepth::Front &&
                opaqueSceneDepth != nullptr &&
                opaqueSceneDepthSize.width() > 0 &&
                opaqueSceneDepthSize.height() > 0 &&
                opaqueSceneDepth->size() ==
                    opaqueSceneDepthSize.width() *
                        opaqueSceneDepthSize.height() &&
                qint64(opaqueSceneDepthSize.width()) *
                    opaqueSceneDepthSize.height() <=
                        maximumPictureDepthPixels) {
                const QTransform imageToDevice =
                    imageTransform * painter.deviceTransform();
                bool inverseValid = false;
                const QTransform deviceToImage =
                    imageToDevice.inverted(&inverseValid);
                const QVector<QPointF> deviceCorners{
                    imageToDevice.map(sourceCorners[0]),
                    imageToDevice.map(sourceCorners[1]),
                    imageToDevice.map(sourceCorners[2]),
                    imageToDevice.map(sourceCorners[3])};
                if (inverseValid) {
                    depthMaskedImage = QImage(
                        opaqueSceneDepthSize, QImage::Format_ARGB32_Premultiplied);
                    depthMaskedImage.fill(Qt::transparent);
                    QPainter imagePainter(&depthMaskedImage);
                    imagePainter.setRenderHint(
                        QPainter::SmoothPixmapTransform, true);
                    imagePainter.setTransform(imageToDevice);
                    imagePainter.drawImage(QPointF(0.0, 0.0),
                                           shape.pictureImage);
                    imagePainter.end();

                    const Point3D cameraPosition =
                        transform_.cameraPosition(viewportSize);
                    const Point3D viewDirection = transform_.viewDirection();
                    const QVector<Point3D> worldCorners{
                        shapePointToWorld(shape, corners[0]),
                        shapePointToWorld(shape, corners[1]),
                        shapePointToWorld(shape, corners[2]),
                        shapePointToWorld(shape, corners[3])};
                    const auto pictureDepthAt = [&](qreal u, qreal v) {
                        const Point3D top{
                            worldCorners[0].x * (1.0 - u) +
                                worldCorners[1].x * u,
                            worldCorners[0].y * (1.0 - u) +
                                worldCorners[1].y * u,
                            worldCorners[0].z * (1.0 - u) +
                                worldCorners[1].z * u};
                        const Point3D bottom{
                            worldCorners[3].x * (1.0 - u) +
                                worldCorners[2].x * u,
                            worldCorners[3].y * (1.0 - u) +
                                worldCorners[2].y * u,
                            worldCorners[3].z * (1.0 - u) +
                                worldCorners[2].z * u};
                        const Point3D world{
                            top.x * (1.0 - v) + bottom.x * v,
                            top.y * (1.0 - v) + bottom.y * v,
                            top.z * (1.0 - v) + bottom.z * v};
                        return -((world.x - cameraPosition.x) *
                                     viewDirection.x +
                                 (world.y - cameraPosition.y) *
                                     viewDirection.y +
                                 (world.z - cameraPosition.z) *
                                     viewDirection.z);
                    };
                    const int left = std::clamp(
                        qFloor(std::min_element(
                            deviceCorners.cbegin(), deviceCorners.cend(),
                            [](const QPointF &a, const QPointF &b) {
                                return a.x() < b.x();
                            })->x()),
                        0, opaqueSceneDepthSize.width());
                    const int right = std::clamp(
                        qCeil(std::max_element(
                            deviceCorners.cbegin(), deviceCorners.cend(),
                            [](const QPointF &a, const QPointF &b) {
                                return a.x() < b.x();
                            })->x()),
                        0, opaqueSceneDepthSize.width());
                    const int top = std::clamp(
                        qFloor(std::min_element(
                            deviceCorners.cbegin(), deviceCorners.cend(),
                            [](const QPointF &a, const QPointF &b) {
                                return a.y() < b.y();
                            })->y()),
                        0, opaqueSceneDepthSize.height());
                    const int bottom = std::clamp(
                        qCeil(std::max_element(
                            deviceCorners.cbegin(), deviceCorners.cend(),
                            [](const QPointF &a, const QPointF &b) {
                                return a.y() < b.y();
                            })->y()),
                        0, opaqueSceneDepthSize.height());
                    for (int y = top; y < bottom; ++y) {
                        auto *imageRow = reinterpret_cast<QRgb *>(
                            depthMaskedImage.scanLine(y));
                        for (int x = left; x < right; ++x) {
                            const QPointF source = deviceToImage.map(
                                QPointF(x + 0.5, y + 0.5));
                            if (source.x() < 0.0 ||
                                source.x() >= shape.pictureImage.width() ||
                                source.y() < 0.0 ||
                                source.y() >= shape.pictureImage.height()) {
                                continue;
                            }
                            const int depthIndex =
                                y * opaqueSceneDepthSize.width() + x;
                            const float sceneDepth =
                                (*opaqueSceneDepth)[depthIndex];
                            bool visible = !std::isfinite(sceneDepth);
                            if (shape.pictureImageDepth ==
                                PictureImageDepth::Back) {
                                visible = !std::isfinite(sceneDepth);
                            } else if (std::isfinite(sceneDepth)) {
                                const qreal u = source.x() /
                                    shape.pictureImage.width();
                                const qreal v = source.y() /
                                    shape.pictureImage.height();
                                const qreal imageDepth =
                                    pictureDepthAt(u, v);
                                const qreal tolerance = std::max<qreal>(
                                    5.0e-4, std::abs(sceneDepth) * 1.0e-5);
                                visible = imageDepth + tolerance >= sceneDepth;
                            }
                            if (!visible) imageRow[x] = 0;
                        }
                    }
                    depthMaskedImage.setDevicePixelRatio(
                        painter.device()->devicePixelRatioF());
                    drawDepthMaskedImage = true;
                }
            }
            painter.save();
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            if (preview) {
                painter.setOpacity(0.78);
            }
            if (drawDepthMaskedImage) {
                bool baseInverseValid = false;
                const QTransform baseLogicalFromDevice =
                    painter.deviceTransform().inverted(&baseInverseValid);
                if (baseInverseValid) {
                    painter.setWorldTransform(QTransform());
                    painter.drawImage(
                        baseLogicalFromDevice.map(QPointF(0.0, 0.0)),
                        depthMaskedImage);
                }
            } else {
                painter.setTransform(imageTransform, true);
                painter.drawImage(QPointF(0.0, 0.0), shape.pictureImage);
            }
            painter.restore();
        }

        if (selected || preview) {
            QPolygonF outline = screenCorners;
            outline.append(screenCorners.first());
            painter.save();
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(curveColor,
                                selected ? 1.5 : 1.25,
                                preview ? Qt::DashLine : Qt::SolidLine));
            painter.drawPolyline(outline);
            painter.restore();
        }
        return;
    }

    if (shape.geometryType == GeometryType::NurbsSolid) {
        const auto faces = shapeSurfaceFaces(shape);
        QVector<QString> faceIds;
        if (shape.nurbsSolid.boundaryFaces.isEmpty()) {
            faceIds = {shape.nurbsSolid.baseCapFaceId,
                       shape.nurbsSolid.displacedCapFaceId};
            for (const QString &id : shape.nurbsSolid.wallFaceIds)
                faceIds.append(id);
        }
        const auto materialForFace = [&](int index) {
            if (index < faceIds.size()) {
                const auto found = nurbsSolidFaceMaterials.constFind(faceIds[index]);
                if (found != nurbsSolidFaceMaterials.cend())
                    return std::optional<WorkbenchMaterialData>(found.value());
            }
            return std::optional<WorkbenchMaterialData>(workbenchMaterial);
        };
        if (preparedGeometry != nullptr && !faces.isEmpty()) {
            for (int index = 0; index < faces.size(); ++index) {
                ViewportDepthGeometry faceGeometry;
                const auto range = index < preparedGeometry->surfaceFaceRanges.size()
                    ? preparedGeometry->surfaceFaceRanges[index]
                    : ViewportDepthGeometry::SurfaceFaceRange{};
                if (range.vertexCount > 0) {
                    for (int vertex = range.firstVertex;
                         vertex < range.firstVertex + range.vertexCount; ++vertex) {
                        faceGeometry.surfaceVertices.append(
                            preparedGeometry->surfaceVertices[vertex]);
                        if (vertex < preparedGeometry->surfaceNormals.size())
                            faceGeometry.surfaceNormals.append(
                                preparedGeometry->surfaceNormals[vertex]);
                        if (vertex < preparedGeometry->surfaceTextureCoordinates.size())
                            faceGeometry.surfaceTextureCoordinates.append(
                                preparedGeometry->surfaceTextureCoordinates[vertex]);
                    }
                }
                drawNurbsSurface(painter, faces[index], viewportSize,
                                 shapeObjectId, geometryRevision,
                                 faceGeometry.surfaceVertices.isEmpty()
                                     ? preparedGeometry : &faceGeometry,
                                 preparedOffset, preview, selected,
                                 nurbsSolidFaceReversed(shape.nurbsSolid, index),
                                 layerColor, displayColorSceneLinear,
                                 materialForFace(index), displayName, libraryPath,
                                 fragmentSink, opaqueSceneDepth,
                                 opaqueSceneDepthSize, nullptr);
            }
        } else {
            for (int index = 0; index < faces.size(); ++index)
                drawNurbsSurface(painter, faces[index], viewportSize,
                                 shapeObjectId, 0, nullptr, {},
                                 preview, selected,
                                 nurbsSolidFaceReversed(shape.nurbsSolid,
                                                        index),
                                 layerColor, displayColorSceneLinear,
                                 materialForFace(index), displayName, libraryPath,
                                 fragmentSink, opaqueSceneDepth,
                                 opaqueSceneDepthSize, nullptr);
        }
        return;
    }
    if (shape.geometryType == GeometryType::NurbsSurface) {
        drawNurbsSurface(painter,
                         shape.nurbsSurface,
                         viewportSize,
                         preview ? ObjectId::invalid() : shapeObjectId,
                         preview ? 0 : geometryRevision,
                         preparedGeometry, preparedOffset, preview, selected,
                         false, layerColor, displayColorSceneLinear,
                         workbenchMaterial, displayName, libraryPath,
                         fragmentSink, opaqueSceneDepth,
                         opaqueSceneDepthSize, blenderSurfaceWireVerticesWorld);
        return;
    }

    if (shape.geometryType == GeometryType::PolyCurve && !shape.components.isEmpty()) {
        bool preparedCurveDrawn = false;
        if (canUsePreparedCurve) {
            painter.save();
            if (!preview) {
                painter.setRenderHint(QPainter::Antialiasing,
                                      smoothWiresOverlay_);
            }
            for (const Shape::NurbsCurve2D &component : shape.components) {
                if (isValidNurbsCurve(component)) {
                    preparedCurveDrawn = drawPreparedCurveSegments(
                        painter, *preparedGeometry, viewportSize,
                        preview ? nullptr : opaqueSceneDepth,
                        opaqueSceneDepthSize);
                    break;
                }
            }
            painter.restore();
        }
        if (!preparedCurveDrawn) {
            for (int index = 0; index < shape.components.size(); ++index) {
                const Shape::NurbsCurve2D &component = shape.components[index];
                drawCurve(component, shapeComponentWorkPlaneFrame(shape, index));
            }
        }
    } else if (shape.geometryType == GeometryType::Point && !shape.points.isEmpty()) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(curveColor);
        const qreal radius = kViewportPointVertexMarkerDiameterPixels * 0.5;
        painter.drawEllipse(worldToScreen(shape.points.first(), viewportSize),
                            radius, radius);
    } else if (shape.geometryType == GeometryType::Line && shape.points.size() >= 2) {
        if (isValidNurbsCurve(shape.nurbs)) {
            drawCurve(shape.nurbs, shapeWorkPlaneFrame(shape));
        } else {
            drawWithWireHint(false, [&] {
                for (int index = 0; index + 1 < shape.points.size(); ++index) {
                    drawClippedWorkPlaneSegment(shape.points[index],
                                                shape.points[index + 1]);
                }
            });
        }
    } else if (shape.geometryType == GeometryType::Rectangle && shape.points.size() >= 2) {
        if (isValidNurbsCurve(shape.nurbs)) {
            drawCurve(shape.nurbs, shapeWorkPlaneFrame(shape));
            return;
        }
        drawWithWireHint(false, [&] {
            const QVector<QPointF> vertices = rectangleVertices(shape);
            if (vertices.size() >= 4) {
                for (int index = 0; index < vertices.size(); ++index) {
                    drawClippedWorkPlaneSegment(
                        vertices[index], vertices[(index + 1) % vertices.size()]);
                }
            }
        });
    } else if (shape.geometryType == GeometryType::Polygon) {
        if (isValidNurbsCurve(shape.nurbs)) {
            drawCurve(shape.nurbs, shapeWorkPlaneFrame(shape));
        }
    } else if (shape.geometryType == GeometryType::Circle && shape.points.size() >= 2) {
        if (isValidNurbsCurve(shape.nurbs)) {
            drawCurve(shape.nurbs, shapeWorkPlaneFrame(shape));
        } else {
            drawWithWireHint(false, [&] {
                const QPointF center = worldToScreen(shape.points[0], viewportSize);
                const QPointF edge = worldToScreen(shape.points[1], viewportSize);
                const qreal radius =
                    std::hypot(edge.x() - center.x(), edge.y() - center.y());
                painter.drawEllipse(center, radius, radius);
            });
        }
    } else if (shape.geometryType == GeometryType::Ellipse) {
        if (isValidNurbsCurve(shape.nurbs)) {
            drawCurve(shape.nurbs, shapeWorkPlaneFrame(shape));
        }
    } else if (shape.geometryType == GeometryType::Arc && shape.points.size() >= 3) {
        if (isValidNurbsCurve(shape.nurbs)) {
            drawCurve(shape.nurbs, shapeWorkPlaneFrame(shape));
        } else if (shape.arcMode == ArcMode::OnePoint) {
            drawWithWireHint(false, [&] {
                if (std::abs(shape.arcSweep) > 1e-9) {
                    drawCenterArcWithSweep(painter,
                                           shape.points[0],
                                           shape.points[1],
                                           shape.arcSweep,
                                           viewportSize);
                } else {
                    drawCenterArc(painter,
                                  shape.points[0],
                                  shape.points[1],
                                  shape.points[2],
                                  viewportSize);
                }
            });
        } else {
            drawWithWireHint(false, [&] {
                drawCircularArc(painter,
                                shape.points[0],
                                shape.points[1],
                                shape.points[2],
                                viewportSize);
            });
        }
    } else if (shape.geometryType == GeometryType::Bezier ||
               shape.geometryType == GeometryType::Nurbs) {
        const QVector<QPointF> controlPoints = shape.nurbs.controlPoints.isEmpty()
                                                   ? shape.points
                                                   : shape.nurbs.controlPoints;
        const bool hasStoredCurve = isValidNurbsCurve(shape.nurbs);
        if (controlPoints.size() >= 2) {
            const WorkPlaneFrame frame = shapeWorkPlaneFrame(shape);
            const auto controlPointWorld = [&](int index) {
                if (!hasStoredCurve ||
                    shape.nurbs.dimension != 3 ||
                    index >= shape.nurbs.normalCoordinates.size()) {
                    return workPlaneFramePointToWorld(
                        controlPoints[index], 0.0,
                        transform_.workPlaneFrame());
                }
                return workPlaneFramePointToWorld(
                    controlPoints[index],
                    shape.nurbs.normalCoordinates[index], frame);
            };
            drawWithWireHint(true, [&] {
                painter.setPen(QPen(controlColor, 1, Qt::DashLine));
                for (int index = 0; index + 1 < controlPoints.size(); ++index) {
                    Point3D start = controlPointWorld(index);
                    Point3D end = controlPointWorld(index + 1);
                    if (!clipWorkbenchSegmentToViewDepth(
                            transform_, viewportSize, &start, &end)) continue;
                    QPointF screenStart;
                    QPointF screenEnd;
                    if (transform_.worldPointToScreenUnclipped(
                            start, viewportSize, &screenStart) &&
                        transform_.worldPointToScreenUnclipped(
                            end, viewportSize, &screenEnd)) {
                        painter.drawLine(screenStart, screenEnd);
                    }
                }
            });
        }

        painter.setPen(layerPen);
        if (hasStoredCurve) {
            drawCurve(shape.nurbs, shapeWorkPlaneFrame(shape));
        } else if (shape.geometryType == GeometryType::Bezier && shape.points.size() >= 4) {
            drawWithWireHint(false, [&] {
                constexpr int sampleCount = 96;
                QPointF previous = shape.points[0];
                for (int sample = 1; sample <= sampleCount; ++sample) {
                    const qreal t = static_cast<qreal>(sample) / sampleCount;
                    const qreal oneMinusT = 1.0 - t;
                    const QPointF current =
                        shape.points[0] * (oneMinusT * oneMinusT * oneMinusT) +
                        shape.points[1] * (3.0 * oneMinusT * oneMinusT * t) +
                        shape.points[2] * (3.0 * oneMinusT * t * t) +
                        shape.points[3] * (t * t * t);
                    drawClippedWorkPlaneSegment(previous, current);
                    previous = current;
                }
            });
        } else if (shape.geometryType == GeometryType::Nurbs) {
            drawWithWireHint(false, [&] {
                for (int index = 0; index + 1 < controlPoints.size(); ++index) {
                    drawClippedWorkPlaneSegment(controlPoints[index],
                                                controlPoints[index + 1]);
                }
            });
        }
    } else {
        drawWithWireHint(true, [&] {
            painter.setPen(QPen(controlColor, 1, Qt::DashLine));
            for (int index = 0; index + 1 < shape.points.size(); ++index) {
                drawClippedWorkPlaneSegment(shape.points[index],
                                            shape.points[index + 1]);
            }
        });
    }

    if (preview && drawPreviewPoints && !isDimensionGeometryType(shape.geometryType)) {
        painter.setBrush(controlColor);
        painter.setPen(Qt::NoPen);
        for (const QPointF &point : shape.points) {
            painter.drawEllipse(worldToScreen(point, viewportSize), 4.0, 4.0);
        }
    }
}

bool ViewportRenderer::subdivisionCurve(const Shape &shape,
                                        Shape::NurbsCurve2D *curve) const
{
    if (curve == nullptr) {
        return false;
    }

    if (isValidNurbsCurve(shape.nurbs)) {
        *curve = shape.nurbs;
        return true;
    }

    if (shape.geometryType == GeometryType::Line && shape.points.size() >= 2) {
        *curve = makeDegreeOneNurbs(shape.points);
        return isValidNurbsCurve(*curve);
    }

    return false;
}

void ViewportRenderer::drawControlPoints(QPainter &painter,
                                         const Shape &shape,
                                         const QSize &viewportSize,
                                         ObjectId shapeObjectId,
                                         ObjectId selectedObjectId,
                                         bool draggingControlPoint,
                                         int activeControlPointIndex,
                                         bool drawMarkers,
                                         const QVector<float> *regularSceneDepth,
                                         const QSize &regularDepthSize,
                                         const QVector<float> *inFrontSceneDepth,
                                         const QSize &inFrontDepthSize,
                                         bool showInFront) const
{
    const QColor handleColor(QStringLiteral("#77b7e6"));
    const QColor handleFill(QStringLiteral("#263b4b"));
    painter.setPen(QPen(handleColor, 1.0, Qt::DashLine));
    painter.setBrush(Qt::NoBrush);

    int globalControlPointIndex = 0;
    const bool depthAvailable = !shadingSettings_.xrayEnabled() &&
        (showInFront
             ? inFrontSceneDepth != nullptr && inFrontDepthSize.width() > 0 &&
                   inFrontDepthSize.height() > 0 &&
                   inFrontSceneDepth->size() ==
                       inFrontDepthSize.width() * inFrontDepthSize.height()
             : regularSceneDepth != nullptr && regularDepthSize.width() > 0 &&
                   regularDepthSize.height() > 0 &&
                   regularSceneDepth->size() ==
                       regularDepthSize.width() * regularDepthSize.height());
    const QSize &depthSize = showInFront ? inFrontDepthSize : regularDepthSize;
    const QVector<float> *domainDepth = showInFront
        ? inFrontSceneDepth : regularSceneDepth;
    const qreal depthScaleX = depthSize.width() > 0 &&
            viewportSize.width() > 0
        ? qreal(depthSize.width()) / viewportSize.width() : 1.0;
    const qreal depthScaleY = depthSize.height() > 0 &&
            viewportSize.height() > 0
        ? qreal(depthSize.height()) / viewportSize.height() : 1.0;
    const Point3D cameraPosition = transform_.cameraPosition(viewportSize);
    const Point3D viewDirection = transform_.viewDirection();
    const auto pointDepth = [&](const Point3D &point) {
        if (transform_.isPerspectiveEnabled()) {
            return (point.x - cameraPosition.x) * viewDirection.x +
                   (point.y - cameraPosition.y) * viewDirection.y +
                   (point.z - cameraPosition.z) * viewDirection.z;
        }
        return transform_.worldDirectionToView(point).towardCamera;
    };
    const auto depthVisible = [&](const QPointF &screen,
                                  const Point3D &world) {
        if (!depthAvailable) return true;
        const int x = qFloor(screen.x() * depthScaleX);
        const int y = qFloor(screen.y() * depthScaleY);
        if (x < 0 || x >= depthSize.width() ||
            y < 0 || y >= depthSize.height()) return true;
        const int pixelIndex = y * depthSize.width() + x;
        const qreal depth = pointDepth(world);
        const qreal tolerance = std::max<qreal>(1.0e-4,
                                                std::abs(depth) * 1.0e-6);
        const float surfaceDepth = (*domainDepth)[pixelIndex];
        if (std::isfinite(surfaceDepth) &&
            surfaceDepth > depth + tolerance) return false;
        if (!showInFront && inFrontSceneDepth != nullptr &&
            inFrontDepthSize == depthSize &&
            pixelIndex < inFrontSceneDepth->size()) {
            const float frontDepth = (*inFrontSceneDepth)[pixelIndex];
            if (std::isfinite(frontDepth) && frontDepth > depth + tolerance)
                return false;
        }
        return true;
    };
    const auto drawControlPointChain = [&](const QVector<QPointF> &controlPoints,
                                           const QVector<double> &normalCoordinates,
                                           const WorkPlaneFrame &frame) {
        if (controlPoints.isEmpty()) {
            return;
        }

        const auto worldPosition = [&](int index) {
            const qreal normalCoordinate =
                normalCoordinates.size() == controlPoints.size()
                    ? normalCoordinates[index] : 0.0;
            return workPlaneFramePointToWorld(
                controlPoints[index], normalCoordinate, frame);
        };
        const auto screenPosition = [&](int index) {
            QPointF screen;
            transform_.worldPointToScreenUnclipped(worldPosition(index),
                                                   viewportSize, &screen);
            return screen;
        };

        painter.save();
        painter.setRenderHint(QPainter::Antialiasing,
                              smoothWiresEditMode_);
        QPainterPath visibleCage;
        bool connectedCageRun = false;
        quint64 depthSampleBudget = 0;
        constexpr quint64 kMaximumControlCageDepthSamples = 1'000'000;
        for (int index = 0; index + 1 < controlPoints.size(); ++index) {
            Point3D worldStart = worldPosition(index);
            Point3D worldEnd = worldPosition(index + 1);
            if (!clipWorkbenchSegmentToViewDepth(
                    transform_, viewportSize, &worldStart, &worldEnd)) {
                connectedCageRun = false;
                continue;
            }
            QPointF start;
            QPointF end;
            if (!transform_.worldPointToScreenUnclipped(
                    worldStart, viewportSize, &start) ||
                !transform_.worldPointToScreenUnclipped(
                    worldEnd, viewportSize, &end)) {
                connectedCageRun = false;
                continue;
            }
            if (!depthAvailable) {
                painter.drawLine(start, end);
                connectedCageRun = false;
                continue;
            }
            const qreal pixelLength = std::hypot(
                (end.x() - start.x()) * depthScaleX,
                (end.y() - start.y()) * depthScaleY);
            const quint64 steps = static_cast<quint64>(std::clamp<qreal>(
                std::ceil(pixelLength * 2.0), 1.0,
                static_cast<qreal>(kMaximumControlCageDepthSamples)));
            if (steps + 1 > kMaximumControlCageDepthSamples -
                                depthSampleBudget) {
                connectedCageRun = false;
                break;
            }
            depthSampleBudget += steps + 1;
            const qreal startDepth = pointDepth(worldStart);
            const qreal endDepth = pointDepth(worldEnd);
            bool visibleRun = connectedCageRun;
            for (quint64 step = 0; step <= steps; ++step) {
                const qreal fraction = static_cast<qreal>(step) /
                                       static_cast<qreal>(steps);
                const QPointF screen = start + (end - start) * fraction;
                qreal worldFraction = fraction;
                if (transform_.isPerspectiveEnabled() &&
                    startDepth < -1.0e-9 && endDepth < -1.0e-9) {
                    const qreal reciprocal = (1.0 - fraction) / startDepth +
                                             fraction / endDepth;
                    if (reciprocal < -1.0e-12)
                        worldFraction = (fraction / endDepth) / reciprocal;
                }
                const Point3D world{
                    worldStart.x + (worldEnd.x - worldStart.x) * worldFraction,
                    worldStart.y + (worldEnd.y - worldStart.y) * worldFraction,
                    worldStart.z + (worldEnd.z - worldStart.z) * worldFraction};
                if (depthVisible(screen, world)) {
                    if (!visibleRun) {
                        visibleCage.moveTo(screen);
                        visibleRun = true;
                    } else {
                        visibleCage.lineTo(screen);
                    }
                } else {
                    visibleRun = false;
                }
            }
            connectedCageRun = visibleRun;
        }
        if (!visibleCage.isEmpty()) painter.drawPath(visibleCage);
        painter.restore();

        for (int index = 0; index < controlPoints.size(); ++index) {
            if (!drawMarkers) {
                continue;
            }
            const bool active = draggingControlPoint &&
                                shapeObjectId == selectedObjectId &&
                                activeControlPointIndex == globalControlPointIndex + index;
            painter.setPen(QPen(active ? QColor(QStringLiteral("#f0a45a")) : handleColor,
                                1.5));
            painter.setBrush(active ? QColor(QStringLiteral("#f0a45a")) : handleFill);
            const QPointF screenPoint = screenPosition(index);
            if (!workbenchPointWithinViewDepth(transform_, viewportSize,
                                               worldPosition(index))) continue;
            if (!depthVisible(screenPoint, worldPosition(index))) continue;
            painter.drawRect(QRectF(screenPoint - QPointF(4.0, 4.0),
                                    screenPoint + QPointF(4.0, 4.0)));
        }
        globalControlPointIndex += controlPoints.size();
    };

    if (shape.geometryType == GeometryType::PolyCurve) {
        for (int index = 0; index < shape.components.size(); ++index) {
            drawControlPointChain(shape.components[index].controlPoints,
                                  shape.components[index].normalCoordinates,
                                  shapeComponentWorkPlaneFrame(shape, index));
        }
        return;
    }

    const QVector<QPointF> controlPoints =
        curveHitTester_.controlPointsForShape(shape);
    const WorkPlaneFrame frame = shapeWorkPlaneFrame(shape);
    const QVector<double> normalCoordinates =
        shape.nurbs.dimension == 3
            ? shape.nurbs.normalCoordinates.mid(0, controlPoints.size())
            : QVector<double>{};
    drawControlPointChain(controlPoints, normalCoordinates, frame);
    if ((shape.geometryType == GeometryType::Rectangle ||
         shape.geometryType == GeometryType::Polygon) &&
        controlPoints.size() > 2) {
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing,
                              smoothWiresEditMode_);
        painter.setPen(QPen(handleColor, 1.0, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        Point3D startWorld = workPlaneFramePointToWorld(
            controlPoints.last(), 0.0, frame);
        Point3D endWorld = workPlaneFramePointToWorld(
            controlPoints.first(), 0.0, frame);
        if (clipWorkbenchSegmentToViewDepth(
                transform_, viewportSize, &startWorld, &endWorld)) {
            QPointF startScreen;
            QPointF endScreen;
            if (transform_.worldPointToScreenUnclipped(
                    startWorld, viewportSize, &startScreen) &&
                transform_.worldPointToScreenUnclipped(
                    endWorld, viewportSize, &endScreen)) {
                if (!depthAvailable) {
                    painter.drawLine(startScreen, endScreen);
                } else {
                    const qreal pixelLength = std::hypot(
                        (endScreen.x() - startScreen.x()) * depthScaleX,
                        (endScreen.y() - startScreen.y()) * depthScaleY);
                    const quint64 steps = static_cast<quint64>(
                        std::clamp<qreal>(std::ceil(pixelLength * 2.0), 1.0,
                                          1'000'000.0));
                    const qreal startDepth = pointDepth(startWorld);
                    const qreal endDepth = pointDepth(endWorld);
                    QPainterPath visibleClosure;
                    bool visibleRun = false;
                    for (quint64 step = 0; step <= steps; ++step) {
                        const qreal fraction = static_cast<qreal>(step) /
                                               static_cast<qreal>(steps);
                        qreal worldFraction = fraction;
                        if (transform_.isPerspectiveEnabled() &&
                            startDepth < -1.0e-9 && endDepth < -1.0e-9) {
                            const qreal reciprocal =
                                (1.0 - fraction) / startDepth +
                                fraction / endDepth;
                            if (reciprocal < -1.0e-12)
                                worldFraction = (fraction / endDepth) / reciprocal;
                        }
                        const Point3D world{
                            startWorld.x + (endWorld.x - startWorld.x) * worldFraction,
                            startWorld.y + (endWorld.y - startWorld.y) * worldFraction,
                            startWorld.z + (endWorld.z - startWorld.z) * worldFraction};
                        const QPointF screen = startScreen +
                            (endScreen - startScreen) * fraction;
                        if (depthVisible(screen, world)) {
                            if (!visibleRun) {
                                visibleClosure.moveTo(screen);
                                visibleRun = true;
                            } else {
                                visibleClosure.lineTo(screen);
                            }
                        } else {
                            visibleRun = false;
                        }
                    }
                    if (!visibleClosure.isEmpty()) painter.drawPath(visibleClosure);
                }
            }
        }
        painter.restore();
    }
}

void ViewportRenderer::drawSubdivisionPoints(QPainter &painter,
                                             const Shape &shape,
                                             const QVector<double> &parameters,
                                             const QSize &viewportSize,
                                             bool preview) const
{
    if (parameters.isEmpty()) {
        return;
    }

    Shape::NurbsCurve2D curve;
    if (!subdivisionCurve(shape, &curve)) {
        return;
    }

    const QColor pointColor = preview ? QColor(QStringLiteral("#f0a45a"))
                                      : QColor(QStringLiteral("#e6b85c"));
    painter.setPen(QPen(pointColor, preview ? 2.0 : 1.5));
    painter.setBrush(pointColor);

    const WorkPlaneFrame frame = shapeWorkPlaneFrame(shape);
    const auto drawAt = [&](qreal parameter) {
        Point3D local;
        if (!evaluateNurbsPoint3D(curve, parameter, &local)) {
            return;
        }
        const Point3D world = workPlaneFramePointToWorld(
            {local.x, local.y}, local.z, frame);
        if (!workbenchPointWithinViewDepth(transform_, viewportSize, world))
            return;
        QPointF screen;
        if (!transform_.worldPointToScreenUnclipped(world,
                                                    viewportSize,
                                                    &screen)) {
            return;
        }
        painter.drawEllipse(screen, preview ? 5.0 : 4.0,
                            preview ? 5.0 : 4.0);
    };
    const QVector<double> fullKnots = expandedNurbsKnotVector(curve);
    if (fullKnots.size() > curve.controlPoints.size()) {
        const double firstParameter = fullKnots[curve.degree];
        const double lastParameter = fullKnots[curve.controlPoints.size()];
        for (const double parameter : {firstParameter, lastParameter}) {
            drawAt(parameter);
        }
    }

    for (const double parameter : parameters) {
        drawAt(parameter);
    }
}

void ViewportRenderer::drawNurbsCurve(QPainter &painter,
                                      const Shape::NurbsCurve2D &curve,
                                      const QSize &viewportSize) const
{
    drawNurbsCurve(painter, curve, transform_.workPlaneFrame(), viewportSize);
}

void ViewportRenderer::drawNurbsCurve(QPainter &painter,
                                      const Shape::NurbsCurve2D &curve,
                                      const WorkPlaneFrame &frame,
                                      const QSize &viewportSize) const
{
    if (!isValidNurbsCurve(curve) || !isValidWorkPlaneFrame(frame)) {
        return;
    }

    const auto project = [&](const Point3D &localPoint, QPointF *screenPoint) {
        Point3D worldPoint = workPlaneFramePointToWorld(
            {localPoint.x, localPoint.y}, localPoint.z, frame);
        return transform_.worldPointToScreenUnclipped(worldPoint,
                                                      viewportSize,
                                                      screenPoint);
    };
    const auto worldPoint = [&frame](const Point3D &localPoint) {
        return workPlaneFramePointToWorld(
            {localPoint.x, localPoint.y}, localPoint.z, frame);
    };
    const auto drawClippedPolyline = [&](const QVector<Point3D> &worldPoints) {
        QPainterPath clippedPath;
        bool hasRun = false;
        Point3D previousClippedEnd;
        for (int index = 0; index + 1 < worldPoints.size(); ++index) {
            Point3D clippedStart = worldPoints[index];
            Point3D clippedEnd = worldPoints[index + 1];
            if (!clipWorkbenchSegmentToViewDepth(
                    transform_, viewportSize, &clippedStart, &clippedEnd)) {
                hasRun = false;
                continue;
            }
            QPointF screenStart;
            QPointF screenEnd;
            if (!transform_.worldPointToScreenUnclipped(
                    clippedStart, viewportSize, &screenStart) ||
                !transform_.worldPointToScreenUnclipped(
                    clippedEnd, viewportSize, &screenEnd)) {
                hasRun = false;
                continue;
            }
            const bool joinsPrevious = hasRun &&
                std::abs(previousClippedEnd.x - clippedStart.x) <= 1.0e-9 &&
                std::abs(previousClippedEnd.y - clippedStart.y) <= 1.0e-9 &&
                std::abs(previousClippedEnd.z - clippedStart.z) <= 1.0e-9;
            if (joinsPrevious) clippedPath.lineTo(screenEnd);
            else {
                clippedPath.moveTo(screenStart);
                clippedPath.lineTo(screenEnd);
            }
            previousClippedEnd = clippedEnd;
            hasRun = true;
        }
        if (!clippedPath.isEmpty()) painter.drawPath(clippedPath);
    };

    const int controlPointCount = curve.controlPoints.size();
    const QVector<double> fullKnots = expandedNurbsKnotVector(curve);
    const qreal firstParameter = fullKnots[curve.degree];
    const qreal lastParameter = fullKnots[controlPointCount];
    int nonZeroSpans = 0;
    for (int index = curve.degree; index < controlPointCount; ++index) {
        if (fullKnots[index + 1] > fullKnots[index]) {
            ++nonZeroSpans;
        }
    }
    if (!smoothCurveDisplay_) {
        const int sampleCount = std::max(32, nonZeroSpans * 24);
        QVector<Point3D> sampledWorldPoints;
        sampledWorldPoints.reserve(sampleCount + 1);
        for (int sample = 0; sample <= sampleCount; ++sample) {
            const qreal fraction = static_cast<qreal>(sample) / sampleCount;
            const qreal parameter = firstParameter +
                                    (lastParameter - firstParameter) * fraction;
            Point3D point;
            if (evaluateNurbsPoint3D(curve, parameter, &point))
                sampledWorldPoints.append(worldPoint(point));
        }
        drawClippedPolyline(sampledWorldPoints);
        return;
    }

    constexpr qreal flatnessTolerancePixels = 0.3;
    constexpr int maximumSubdivisionDepth = 14;
    constexpr int maximumAdaptivePointCount = 16384;
    QVector<QPointF> screenPoints;
    screenPoints.reserve(std::min(maximumAdaptivePointCount, nonZeroSpans * 8));
    QVector<qreal> adaptiveParameters;
    adaptiveParameters.reserve(std::min(maximumAdaptivePointCount,
                                        nonZeroSpans * 8));
    bool exceededAdaptivePointLimit = false;
    bool needsUniformSampling = false;

    const auto screenPointAt = [&](qreal parameter, QPointF *screenPoint) {
        if (screenPoint == nullptr) {
            return false;
        }
        Point3D localPoint;
        if (!evaluateNurbsPoint3D(curve, parameter, &localPoint)) {
            return false;
        }
        return project(localPoint, screenPoint);
    };
    const auto distanceToSegment = [](const QPointF &point,
                                     const QPointF &segmentStart,
                                     const QPointF &segmentEnd) {
        const QPointF direction = segmentEnd - segmentStart;
        const qreal lengthSquared = QPointF::dotProduct(direction, direction);
        if (lengthSquared <= 1.0e-16) {
            return std::hypot(point.x() - segmentStart.x(),
                              point.y() - segmentStart.y());
        }
        const qreal projection = std::clamp(
            QPointF::dotProduct(point - segmentStart, direction) / lengthSquared,
            0.0,
            1.0);
        const QPointF nearest = segmentStart + direction * projection;
        return std::hypot(point.x() - nearest.x(), point.y() - nearest.y());
    };

    const auto appendAdaptiveSegment = [&](const auto &self,
                                           qreal parameterStart,
                                           const QPointF &screenStart,
                                           qreal parameterEnd,
                                           const QPointF &screenEnd,
                                           int depth) -> void {
        if (exceededAdaptivePointLimit) {
            return;
        }

        const qreal parameterRange = parameterEnd - parameterStart;
        const qreal quarterParameter = parameterStart + parameterRange * 0.25;
        const qreal middleParameter = parameterStart + parameterRange * 0.5;
        const qreal threeQuarterParameter = parameterStart + parameterRange * 0.75;
        QPointF quarterPoint;
        QPointF middlePoint;
        QPointF threeQuarterPoint;
        if (!screenPointAt(quarterParameter, &quarterPoint) ||
            !screenPointAt(middleParameter, &middlePoint) ||
            !screenPointAt(threeQuarterParameter, &threeQuarterPoint)) {
            screenPoints.append(screenEnd);
            adaptiveParameters.append(parameterEnd);
            needsUniformSampling = true;
            if (screenPoints.size() > maximumAdaptivePointCount) {
                exceededAdaptivePointLimit = true;
            }
            return;
        }

        const qreal maximumDeviation = std::max({
            distanceToSegment(quarterPoint, screenStart, screenEnd),
            distanceToSegment(middlePoint, screenStart, screenEnd),
            distanceToSegment(threeQuarterPoint, screenStart, screenEnd)});
        if (maximumDeviation <= flatnessTolerancePixels ||
            depth >= maximumSubdivisionDepth) {
            screenPoints.append(screenEnd);
            adaptiveParameters.append(parameterEnd);
            if (screenPoints.size() > maximumAdaptivePointCount) {
                exceededAdaptivePointLimit = true;
            }
            return;
        }

        self(self,
             parameterStart,
             screenStart,
             middleParameter,
             middlePoint,
             depth + 1);
        self(self,
             middleParameter,
             middlePoint,
             parameterEnd,
             screenEnd,
             depth + 1);
    };

    bool hasStart = false;
    for (int spanIndex = curve.degree;
         spanIndex < controlPointCount && !exceededAdaptivePointLimit;
         ++spanIndex) {
        const qreal spanStart = fullKnots[spanIndex];
        const qreal spanEnd = fullKnots[spanIndex + 1];
        if (spanEnd <= spanStart) {
            continue;
        }

        QPointF spanStartPoint;
        QPointF spanEndPoint;
        if (!screenPointAt(spanStart, &spanStartPoint) ||
            !screenPointAt(spanEnd, &spanEndPoint)) {
            needsUniformSampling = true;
            hasStart = false;
            continue;
        }
        if (!hasStart) {
            screenPoints.append(spanStartPoint);
            adaptiveParameters.append(spanStart);
            hasStart = true;
        }
        appendAdaptiveSegment(appendAdaptiveSegment,
                              spanStart,
                              spanStartPoint,
                              spanEnd,
                              spanEndPoint,
                              0);
    }

    if (exceededAdaptivePointLimit || needsUniformSampling) {
        // Preserve the entire curve if a pathological shape exceeds the
        // adaptive detail budget. This fallback is bounded and still becomes
        // denser at higher zoom levels.
        screenPoints.clear();
        adaptiveParameters.clear();
        const int sampleCount = std::max(
            32,
            std::min(
                maximumAdaptivePointCount,
                static_cast<int>(std::ceil(
                    nonZeroSpans * 24.0 *
                    std::sqrt(std::max(
                        1.0,
                        transform_.viewScalePixelsPerWorldUnit(viewportSize)))))));
        for (int sample = 0; sample <= sampleCount; ++sample) {
            const qreal fraction = static_cast<qreal>(sample) / sampleCount;
            const qreal parameter = firstParameter +
                                    (lastParameter - firstParameter) * fraction;
            Point3D localPoint;
            if (evaluateNurbsPoint3D(curve, parameter, &localPoint))
                adaptiveParameters.append(parameter);
        }
    }
    QVector<Point3D> adaptiveWorldPoints;
    adaptiveWorldPoints.reserve(adaptiveParameters.size());
    for (const qreal parameter : adaptiveParameters) {
        Point3D localPoint;
        if (evaluateNurbsPoint3D(curve, parameter, &localPoint))
            adaptiveWorldPoints.append(worldPoint(localPoint));
    }
    drawClippedPolyline(adaptiveWorldPoints);
}

void ViewportRenderer::drawNurbsSurface(QPainter &painter,
                                        const Shape::NurbsSurface3D &surface,
                                        const QSize &viewportSize,
                                        ObjectId objectId,
                                        quint64 geometryRevision,
                                        const ViewportDepthGeometry *preparedGeometry,
                                        const Point3D &preparedOffset,
                                        bool preview,
                                        bool selected,
                                        bool reverseOrientation,
                                        const QColor &objectColor,
                                        std::optional<QVector4D> displayColorSceneLinear,
                                        std::optional<WorkbenchMaterialData>
                                            workbenchMaterial,
                                        const QString &displayName,
                                        const QString &libraryPath,
                                        RasterWorkbenchFragmentSink *fragmentSink,
                                        const QVector<float> *opaqueSceneDepth,
                                        const QSize &opaqueSceneDepthSize,
                                        const QVector<Point3D> *blenderWireVerticesWorld) const
{
    Q_UNUSED(selected);
    if (shadingSettings_.mode == ViewportShadingMode::Solid) {
        const WorkbenchMaterialData fallbackMaterial;
        const WorkbenchMaterialData &material = workbenchMaterial
                                                    ? *workbenchMaterial
                                                    : fallbackMaterial;
        QImage nurbsTexture;
        QHash<int, QImage> nurbsTextureTiles;
        WorkbenchFloatTextureData nurbsFloatTexture;
        QHash<int, WorkbenchFloatTextureData> nurbsFloatTextureTiles;
        const auto decodeFloatTexture = [](const QString &encoded, int width,
                                           int height) {
            WorkbenchFloatTextureData result;
            const QByteArray data = QByteArray::fromBase64(
                encoded.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
            if (encoded.isEmpty() || width <= 0 || height <= 0 ||
                quint64(width) * height * 16ULL > 32ULL * 1024ULL * 1024ULL ||
                data.size() != qint64(width) * height * 16) return result;
            result.width = width; result.height = height;
            result.pixels.resize(width * height * 4);
            for (int index = 0; index < result.pixels.size(); ++index) {
                const auto *source = reinterpret_cast<const uchar *>(
                    data.constData() + index * 4);
                result.pixels[index] = std::bit_cast<float>(
                    qFromLittleEndian<quint32>(source));
            }
            for (float value : result.pixels)
                if (!std::isfinite(value)) return WorkbenchFloatTextureData{};
            return result;
        };
        nurbsFloatTexture = decodeFloatTexture(
            material.textureImageSceneLinearFloatDataBase64,
            material.textureImageSceneLinearFloatWidth,
            material.textureImageSceneLinearFloatHeight);
        const bool nurbsTiledTexture = !material.textureImageTiles.isEmpty();
        const bool textureAlphaIgnored =
            material.textureImageAlphaMode.compare(QStringLiteral("NONE"), Qt::CaseInsensitive) == 0 ||
            material.textureImageAlphaMode.compare(QStringLiteral("IGNORE"), Qt::CaseInsensitive) == 0 ||
            material.textureImageAlphaMode.compare(QStringLiteral("CHANNEL_PACKED"), Qt::CaseInsensitive) == 0;
        const bool canSampleNurbsTexture =
            shadingSettings_.colorMode == ViewportColorMode::Texture &&
            workbenchMaterial.has_value() &&
            (!material.textureImagePath.isEmpty() ||
             !material.textureImagePackedDataBase64.isEmpty() ||
             !material.textureImageSceneLinearFloatDataBase64.isEmpty() ||
             nurbsTiledTexture);
        if (canSampleNurbsTexture) {
            if (nurbsTiledTexture) {
                static const QSet<QByteArray> allowedFormats{
                    "png", "jpg", "jpeg", "bmp", "tif", "tiff", "webp"};
                QSet<int> loadedTileNumbers;
                quint64 decodedTilePixels = 0;
                quint64 decodedFloatBytes = nurbsFloatTexture.isValid()
                    ? quint64(nurbsFloatTexture.pixels.size()) * sizeof(float) : 0;
                qsizetype packedTileBytes = 0;
                for (const DocumentImageTextureTile &tile :
                     material.textureImageTiles) {
                    auto floatTile = decodeFloatTexture(tile.sceneLinearFloatDataBase64,
                        tile.sceneLinearFloatWidth, tile.sceneLinearFloatHeight);
                    if (floatTile.isValid()) {
                        const quint64 bytes = quint64(floatTile.pixels.size()) * sizeof(float);
                        if (decodedFloatBytes + bytes <= 32ULL * 1024ULL * 1024ULL) {
                            decodedFloatBytes += bytes;
                            nurbsFloatTextureTiles.insert(tile.number, std::move(floatTile));
                        }
                    }
                    if ((!tile.sourceFileExists && tile.packedDataBase64.isEmpty()) ||
                        tile.number < 1001 || tile.number > 1999 ||
                        loadedTileNumbers.contains(tile.number)) {
                        continue;
                    }
                    QByteArray packedBytes;
                    if (!tile.packedDataBase64.isEmpty()) {
                        const QByteArray encoded = tile.packedDataBase64.toLatin1();
                        if (encoded.size() <= 44 * 1024 * 1024) {
                            packedBytes = QByteArray::fromBase64(
                                encoded, QByteArray::AbortOnBase64DecodingErrors);
                        }
                        if (packedBytes.size() > 32 * 1024 * 1024 ||
                            packedTileBytes + packedBytes.size() > 32 * 1024 * 1024) {
                            packedBytes.clear();
                        } else if (!packedBytes.isEmpty()) {
                            packedTileBytes += packedBytes.size();
                        }
                    }
                    QImage tileImage;
                    if (!packedBytes.isEmpty()) {
                        QBuffer buffer;
                        buffer.setData(packedBytes);
                        if (buffer.open(QIODevice::ReadOnly)) {
                            QImageReader reader(&buffer);
                            const QSize size = reader.size();
                            if (size.isValid() && size.width() <= 16384 &&
                                size.height() <= 16384 &&
                                static_cast<quint64>(size.width()) * size.height() <=
                                    67108864ULL &&
                                decodedTilePixels + static_cast<quint64>(size.width()) *
                                    size.height() <= 67108864ULL) {
                                tileImage = reader.read();
                            }
                        }
                    }
                    if (tileImage.isNull() && tile.sourceFileExists &&
                        !tile.filepath.isEmpty()) {
                        const QFileInfo info(tile.filepath);
                        const QByteArray suffix = info.suffix().toLower().toLatin1();
                        if (info.isFile() && allowedFormats.contains(suffix) &&
                            info.size() > 0 && info.size() <= 256LL * 1024LL * 1024LL) {
                            QImageReader reader(info.filePath());
                            const QSize size = reader.size();
                            if (size.isValid() && size.width() <= 16384 &&
                                size.height() <= 16384 &&
                                static_cast<quint64>(size.width()) * size.height() <=
                                    67108864ULL) tileImage = reader.read();
                        }
                    }
                    if (tileImage.isNull()) continue;
                    const quint64 tilePixels = static_cast<quint64>(tileImage.width()) *
                                               tileImage.height();
                    if (decodedTilePixels + tilePixels > 67108864ULL) {
                        continue;
                    }
                    decodedTilePixels += tilePixels;
                    loadedTileNumbers.insert(tile.number);
                    tileImage = tileImage.convertToFormat(QImage::Format_RGBA8888);
                    if (textureAlphaIgnored) {
                        for (int y = 0; y < tileImage.height(); ++y) {
                            uchar *row = tileImage.scanLine(y);
                            for (int x = 0; x < tileImage.width(); ++x)
                                row[x * 4 + 3] = 255;
                        }
                    }
                    nurbsTextureTiles.insert(tile.number, std::move(tileImage));
                }
            } else {
            QByteArray packedBytes;
            QBuffer packedBuffer;
            QImageReader *reader = nullptr;
            std::unique_ptr<QImageReader> fileReader;
            const bool packed = !material.textureImagePackedDataBase64.isEmpty();
            if (packed) {
                const QByteArray encoded = material.textureImagePackedDataBase64.toLatin1();
                if (encoded.size() <= 44 * 1024 * 1024) {
                    packedBytes = QByteArray::fromBase64(encoded);
                    if (packedBytes.size() <= 32 * 1024 * 1024) {
                        packedBuffer.setData(packedBytes);
                        if (packedBuffer.open(QIODevice::ReadOnly)) {
                            fileReader = std::make_unique<QImageReader>(&packedBuffer);
                            reader = fileReader.get();
                        }
                    }
                }
            } else {
                const QFileInfo info(material.textureImagePath);
                static const QSet<QByteArray> allowedFormats{
                    "png", "jpg", "jpeg", "bmp", "tif", "tiff", "webp"};
                const QByteArray suffix = info.suffix().toLower().toLatin1();
                if (info.isFile() && allowedFormats.contains(suffix) &&
                    info.size() > 0 && info.size() <= 256LL * 1024LL * 1024LL) {
                    fileReader = std::make_unique<QImageReader>(info.filePath());
                    reader = fileReader.get();
                }
            }
            if (reader != nullptr) {
                const QSize size = reader->size();
                if (size.isValid() && size.width() <= 16384 &&
                    size.height() <= 16384 &&
                    static_cast<quint64>(size.width()) * size.height() <=
                        67108864ULL) {
                    nurbsTexture = reader->read();
                }
            }
            if (!nurbsTexture.isNull())
                nurbsTexture = nurbsTexture.convertToFormat(QImage::Format_RGBA8888);
            }
        }
        const bool nurbsTextureSrgb = material.textureImageSrgb;
        QVector3D sourceColorLinear = workbenchDefaultSolidMaterialDiffuseColor();
        qreal sourceAlpha = 1.0;
        qreal roughness = 0.4;
        qreal metallic = 0.0;

        switch (shadingSettings_.colorMode) {
        case ViewportColorMode::Material:
        case ViewportColorMode::Attribute:
        case ViewportColorMode::Texture:
            // Texture mode replaces material RGB with the supported image
            // sample. If the image or UVs are unavailable, retain this
            // material fallback.
            if (workbenchMaterial) {
                sourceColorLinear = material.baseColorSceneLinear;
                sourceAlpha = material.alpha;
            } else if (displayColorSceneLinear) {
                // Blender Material/Attribute/Texture modes fall back to its
                // opaque 0.8 gray default material when no slot is assigned.
                sourceColorLinear = workbenchDefaultSolidMaterialDiffuseColor();
                sourceAlpha = 1.0;
            } else {
                sourceColorLinear = workbenchDefaultSolidMaterialDiffuseColor();
                sourceAlpha = 1.0;
            }
            roughness = workbenchMaterial ? material.roughness : 0.4;
            metallic = workbenchMaterial ? material.metallic : 0.0;
            break;
        case ViewportColorMode::Object:
            if (displayColorSceneLinear) {
                sourceColorLinear = QVector3D(displayColorSceneLinear->x(),
                                              displayColorSceneLinear->y(),
                                              displayColorSceneLinear->z());
                sourceAlpha = displayColorSceneLinear->w();
            } else if (objectColor.isValid()) {
                // Legacy callers pass Qt/layer colors in sRGB. Keep that path
                // working while scene objects migrate to the linear payload.
                sourceColorLinear = workbenchSrgbToSceneLinear(
                    QVector3D(objectColor.redF(), objectColor.greenF(),
                              objectColor.blueF()));
                sourceAlpha = objectColor.alphaF();
            }
            break;
        case ViewportColorMode::Random: {
            const QColor randomColor = workbenchRandomColor(
                objectId, displayName, libraryPath);
            sourceColorLinear = workbenchRec709ToSceneLinear(
                QVector3D(randomColor.redF(), randomColor.greenF(),
                          randomColor.blueF()));
            sourceAlpha = displayColorSceneLinear
                              ? displayColorSceneLinear->w()
                              : objectColor.isValid()
                                    ? objectColor.alphaF()
                                    : 1.0;
            break;
        }
        case ViewportColorMode::Custom:
            sourceColorLinear = workbenchSrgbToSceneLinear(
                QVector3D(shadingSettings_.customColor.redF(),
                          shadingSettings_.customColor.greenF(),
                          shadingSettings_.customColor.blueF()));
            sourceAlpha = workbenchCustomColorSourceAlpha;
            break;
        }

        if (preview) {
            const QColor previewColor(QStringLiteral("#d89a4e"));
            sourceColorLinear = workbenchSrgbToSceneLinear(
                QVector3D(previewColor.redF(), previewColor.greenF(),
                          previewColor.blueF()));
            sourceAlpha = 0.5;
            roughness = 0.4;
            metallic = 0.0;
        }
        const WorkbenchPackedMaterialData packed =
            workbenchPackMaterialData(static_cast<float>(metallic),
                                      static_cast<float>(roughness),
                                      static_cast<float>(sourceAlpha));
        roughness = packed.roughness;
        metallic = packed.metallic;
        if (!preview) {
            sourceAlpha = packed.alpha;
        }
        const qreal alpha = std::clamp(
            sourceAlpha * (preview || !shadingSettings_.xrayEnabled()
                               ? 1.0
                               : shadingSettings_.xrayAlphaForMode()),
            0.0, 1.0);
        QColor baseColor = QColor::fromRgbF(
            std::clamp<qreal>(sourceColorLinear.x(), 0.0, 1.0),
            std::clamp<qreal>(sourceColorLinear.y(), 0.0, 1.0),
            std::clamp<qreal>(sourceColorLinear.z(), 0.0, 1.0), alpha);
        const Point3D viewDirection = transform_.viewDirection();
        const Point3D viewUp = transform_.viewUp();
        const Point3D viewRight{
            viewUp.y * viewDirection.z - viewUp.z * viewDirection.y,
            viewUp.z * viewDirection.x - viewUp.x * viewDirection.z,
            viewUp.x * viewDirection.y - viewUp.y * viewDirection.x};
        const Point3D viewFacing = viewDirection;
        const Point3D cameraPosition = transform_.cameraPosition(viewportSize);
        const QVector3D worldRight(static_cast<float>(viewRight.x),
                                   static_cast<float>(viewRight.y),
                                   static_cast<float>(viewRight.z));
        const QVector3D worldUp(static_cast<float>(viewUp.x),
                                static_cast<float>(viewUp.y),
                                static_cast<float>(viewUp.z));
        const QVector3D worldFacing(static_cast<float>(viewFacing.x),
                                    static_cast<float>(viewFacing.y),
                                    static_cast<float>(viewFacing.z));
        const QVector3D camera(static_cast<float>(cameraPosition.x),
                               static_cast<float>(cameraPosition.y),
                               static_cast<float>(cameraPosition.z));
        const QVector3D baseColorVector = sourceColorLinear;
        const auto shadowReceiverNormal = [&](const QVector3D &sourceNormal,
                                              bool frontFacing,
                                              qreal fragmentAlpha) {
            QVector3D normalWorld = workbenchNormalize(
                reverseOrientation && preparedGeometry == nullptr
                    ? -sourceNormal : sourceNormal,
                QVector3D(0.0f, 0.0f, 1.0f));
            if (fragmentAlpha >= 1.0 && !frontFacing) normalWorld = -normalWorld;
            return normalWorld;
        };
        const auto shade = [&](const QVector3D &sourceNormal,
                               const Point3D &sourcePosition,
                               const QVector3D &surfaceBaseColor,
                               bool frontFacing,
                               QVector3D *resolvedSceneLinear = nullptr) {
            QVector3D normalWorld = workbenchNormalize(
                reverseOrientation && preparedGeometry == nullptr
                    ? -sourceNormal
                    : sourceNormal,
                QVector3D(0.0f, 0.0f, 1.0f));
            // Blender flips opaque backface normals in its normal prepass.
            // Its transparent pass uses interpolated normals unchanged.
            if (alpha >= 1.0 && !frontFacing) {
                normalWorld = -normalWorld;
            }
            const QVector3D normalView(
                QVector3D::dotProduct(normalWorld, worldRight),
                QVector3D::dotProduct(normalWorld, worldUp),
                QVector3D::dotProduct(normalWorld, worldFacing));
            QVector3D incidentView(0.0f, 0.0f, 1.0f);
            if (transform_.isPerspectiveEnabled()) {
                const QVector3D position(
                    static_cast<float>(sourcePosition.x + preparedOffset.x),
                    static_cast<float>(sourcePosition.y + preparedOffset.y),
                    static_cast<float>(sourcePosition.z + preparedOffset.z));
                const QVector3D incidentWorld = workbenchNormalize(camera - position,
                                                                  worldFacing);
                incidentView = {QVector3D::dotProduct(incidentWorld, worldRight),
                                QVector3D::dotProduct(incidentWorld, worldUp),
                                QVector3D::dotProduct(incidentWorld, worldFacing)};
            }
            QVector3D shadedLinear = surfaceBaseColor;
            if (shadingSettings_.lightingMode ==
                ViewportLightingMode::Studio) {
                WorkbenchStudioLighting lighting =
                    workbenchStudioLightingPreset(
                        shadingSettings_.studioLightPreset);
                lighting.useSpecular = shadingSettings_.specularLighting &&
                                       lighting.useSpecular;
                const WorkbenchStudioLighting viewLighting =
                    workbenchStudioLightingForView(
                        lighting,
                        shadingSettings_.studioLightRotationDegrees,
                        shadingSettings_.worldSpaceLighting,
                        worldRight, worldUp, worldFacing);
                shadedLinear = workbenchStudioShade(
                    surfaceBaseColor, normalView, incidentView,
                    static_cast<float>(roughness),
                    static_cast<float>(metallic), viewLighting);
            } else if (shadingSettings_.lightingMode ==
                       ViewportLightingMode::MatCap) {
                shadedLinear = workbenchMatcapShade(
                    shadingSettings_.matcapPreset, surfaceBaseColor,
                    normalView, incidentView,
                    shadingSettings_.specularLighting,
                    shadingSettings_.matcapFlipX);
            }
            if (resolvedSceneLinear != nullptr) {
                *resolvedSceneLinear = shadedLinear;
            }
            const QVector3D shaded = workbenchSceneLinearToDisplaySrgb(
                shadedLinear, shadingSettings_.colorTransform,
                shadingSettings_.colorExposure, shadingSettings_.colorGamma,
                shadingSettings_.colorLutData,
                shadingSettings_.colorLutMetadata);
            QColor color = baseColor;
            color.setRedF(std::clamp<qreal>(shaded.x(), 0.0, 1.0));
            color.setGreenF(std::clamp<qreal>(shaded.y(), 0.0, 1.0));
            color.setBlueF(std::clamp<qreal>(shaded.z(), 0.0, 1.0));
            color.setAlphaF(alpha);
            return color;
        };
        const auto project = [&](const Point3D &point, QPointF *screen) {
            const Point3D moved{point.x + preparedOffset.x,
                                point.y + preparedOffset.y,
                                point.z + preparedOffset.z};
            return transform_.worldPointToScreenUnclipped(moved,
                                                          viewportSize,
                                                          screen);
        };
        struct SurfaceFillTriangle {
            Point3D a;
            Point3D b;
            Point3D c;
            QVector3D normal;
            std::array<QVector3D, 3> vertexNormals{};
            bool hasSmoothNormals = false;
            qreal depth = 0.0;
            std::array<QPointF, 3> uvs{};
            bool hasUvs = false;
        };
        QVector<SurfaceFillTriangle> fillTriangles;
        struct CoverageResolvePixel {
            std::array<float, 4> premultipliedRgba{};
            float coverage = 0.0f;
        };
        struct CoverageResolveFragment {
            // Weighted scene-linear RGB, world XYZ, effective alpha, and
            // receiver normal XYZ for the bounded shadow-layer resolve.
            std::array<float, 10> values{};
        };
        std::vector<CoverageResolvePixel> coverageResolvePixels;
        std::vector<CoverageResolveFragment> coverageResolveFragments;
        bool coverageResolveActive = false;
        const QTransform coverageDeviceTransform = painter.deviceTransform();
        bool coverageInverseValid = false;
        const QTransform logicalFromCoverageDevice =
            coverageDeviceTransform.inverted(&coverageInverseValid);
        if (!coverageInverseValid) return;
        const QTransform targetTransform = painter.transform();
        const qreal targetDpr = painter.device() != nullptr
            ? painter.device()->devicePixelRatioF() : 1.0;
        const qreal coverageScaleX = std::max<qreal>(
            1.0e-6, std::abs(targetTransform.m11()) * targetDpr);
        const qreal coverageScaleY = std::max<qreal>(
            1.0e-6, std::abs(targetTransform.m22()) * targetDpr);
        const int coverageTargetWidth = painter.device() != nullptr
            ? painter.device()->width()
            : std::max(1, qRound(viewportSize.width() * coverageScaleX));
        const int coverageTargetHeight = painter.device() != nullptr
            ? painter.device()->height()
            : std::max(1, qRound(viewportSize.height() * coverageScaleY));
        const auto appendTriangle = [&](const Point3D &a,
                                        const Point3D &b,
                                        const Point3D &c,
                                        const QVector3D &normal,
                                        const std::array<QPointF, 3> &uvs = std::array<QPointF, 3>{},
                                        bool hasUvs = false,
                                        const std::array<QVector3D, 3> &vertexNormals =
                                            std::array<QVector3D, 3>{},
                                        bool hasSmoothNormals = false) {
            const Point3D center{(a.x + b.x + c.x) / 3.0,
                                 (a.y + b.y + c.y) / 3.0,
                                 (a.z + b.z + c.z) / 3.0};
            const Point3D worldCenter{center.x + preparedOffset.x,
                                      center.y + preparedOffset.y,
                                      center.z + preparedOffset.z};
            fillTriangles.append({a, b, c, normal, vertexNormals,
                hasSmoothNormals,
                transform_.worldDirectionToView(worldCenter).towardCamera,
                uvs, hasUvs});
        };
        const auto drawUnclippedTriangle = [&](const SurfaceFillTriangle &triangle) {
            QPointF screenA;
            QPointF screenB;
            QPointF screenC;
            if (!project(triangle.a, &screenA) ||
                !project(triangle.b, &screenB) ||
                !project(triangle.c, &screenC)) {
                return;
            }
            const std::array<QPointF, 3> devicePoints{
                coverageDeviceTransform.map(screenA),
                coverageDeviceTransform.map(screenB),
                coverageDeviceTransform.map(screenC)};
            const qreal area =
                (devicePoints[1].x() - devicePoints[0].x()) *
                    (devicePoints[2].y() - devicePoints[0].y()) -
                (devicePoints[1].y() - devicePoints[0].y()) *
                    (devicePoints[2].x() - devicePoints[0].x());
            if (std::abs(area) < 1.0e-12) return;
            // QPainter's screen Y axis points down, so a negative projected
            // signed area corresponds to Blender/OpenGL's CCW front face.
            bool frontFacing = area < 0.0;
            if (reverseOrientation && preparedGeometry == nullptr) {
                frontFacing = !frontFacing;
            }
            if (shadingSettings_.backfaceCulling && !frontFacing) return;
            painter.setPen(Qt::NoPen);
            const Point3D center{(triangle.a.x + triangle.b.x + triangle.c.x) / 3.0,
                                 (triangle.a.y + triangle.b.y + triangle.c.y) / 3.0,
                                 (triangle.a.z + triangle.b.z + triangle.c.z) / 3.0};
            const QColor shaded = shade(triangle.normal, center,
                                        baseColorVector, frontFacing);
            if ((!nurbsTiledTexture && nurbsTexture.isNull() &&
                 !nurbsFloatTexture.isValid()) ||
                !triangle.hasUvs) {
                // Shade untextured triangles per sample as Workbench's resolve
                // pass does. Prepared geometry carries smooth vertex normals;
                // generated fallback triangles retain their flat normal.
                const int left = qFloor(std::clamp(
                    std::min({devicePoints[0].x(), devicePoints[1].x(),
                              devicePoints[2].x()}),
                    0.0, qreal(coverageTargetWidth)));
                const int top = qFloor(std::clamp(
                    std::min({devicePoints[0].y(), devicePoints[1].y(),
                              devicePoints[2].y()}),
                    0.0, qreal(coverageTargetHeight)));
                const int right = qCeil(std::clamp(
                    std::max({devicePoints[0].x(), devicePoints[1].x(),
                              devicePoints[2].x()}),
                    0.0, qreal(coverageTargetWidth)));
                const int bottom = qCeil(std::clamp(
                    std::max({devicePoints[0].y(), devicePoints[1].y(),
                              devicePoints[2].y()}),
                    0.0, qreal(coverageTargetHeight)));
                if (right <= left || bottom <= top) return;
                const int rasterWidth = right - left;
                const int rasterHeight = bottom - top;
                if (qint64(rasterWidth) * rasterHeight > 4000000) {
                    painter.setBrush(shaded);
                    painter.drawPolygon(QPolygonF{screenA, screenB, screenC});
                    return;
                }
                const Point3D cameraWorld = transform_.cameraPosition(viewportSize);
                const Point3D viewDirection = transform_.viewDirection();
                const Point3D points[3] = {triangle.a, triangle.b, triangle.c};
                qreal reciprocal[3] = {1.0, 1.0, 1.0};
                if (transform_.isPerspectiveEnabled()) {
                    for (int i = 0; i < 3; ++i) {
                        const qreal dx = points[i].x + preparedOffset.x - cameraWorld.x;
                        const qreal dy = points[i].y + preparedOffset.y - cameraWorld.y;
                        const qreal dz = points[i].z + preparedOffset.z - cameraWorld.z;
                        // viewDirection points from the scene toward the
                        // camera; perspective projection depth increases in
                        // the opposite (camera-forward) direction.
                        reciprocal[i] = 1.0 / std::max<qreal>(
                            -(dx * viewDirection.x + dy * viewDirection.y +
                              dz * viewDirection.z),
                            1.0e-9);
                    }
                }
                QImage raster(rasterWidth, rasterHeight,
                              QImage::Format_ARGB32_Premultiplied);
                raster.setDevicePixelRatio(targetDpr);
                raster.fill(Qt::transparent);
                for (int y = 0; y < raster.height(); ++y) {
                    auto *row = reinterpret_cast<QRgb *>(raster.scanLine(y));
                    for (int x = 0; x < raster.width(); ++x) {
                        const qreal px = left + x + 0.5;
                        const qreal py = top + y + 0.5;
                        qreal w0, w1, w2;
                        const qreal coverage = workbenchTrianglePixelCoverage(
                            devicePoints, area, px, py, 1.0, 1.0,
                            &w0, &w1, &w2, nullptr,
                            workbenchRasterAntialiasingEnabled_ ? 4 : 1);
                        if (coverage <= 0.0) continue;
                        const qreal cw0 = w0 * reciprocal[0];
                        const qreal cw1 = w1 * reciprocal[1];
                        const qreal cw2 = w2 * reciprocal[2];
                        const qreal denominator = cw0 + cw1 + cw2;
                        if (denominator <= 1.0e-12) continue;
                        Point3D shadingPosition = center;
                        if (transform_.isPerspectiveEnabled()) {
                            shadingPosition = {
                                (points[0].x * cw0 + points[1].x * cw1 + points[2].x * cw2) / denominator,
                                (points[0].y * cw0 + points[1].y * cw1 + points[2].y * cw2) / denominator,
                                (points[0].z * cw0 + points[1].z * cw1 + points[2].z * cw2) / denominator};
                        }
                        QVector3D sampleNormal = triangle.normal;
                        if (triangle.hasSmoothNormals) {
                            sampleNormal =
                                (triangle.vertexNormals[0] *
                                     static_cast<float>(cw0) +
                                 triangle.vertexNormals[1] *
                                     static_cast<float>(cw1) +
                                 triangle.vertexNormals[2] *
                                     static_cast<float>(cw2)) /
                                static_cast<float>(denominator);
                            const float normalLengthSquared =
                                sampleNormal.lengthSquared();
                            if (std::isfinite(normalLengthSquared) &&
                                normalLengthSquared > 1.0e-12f) {
                                sampleNormal.normalize();
                            } else {
                                sampleNormal = triangle.normal;
                            }
                        }
                        QVector3D sampleLinear;
                        const QColor sample = shade(sampleNormal,
                                                    shadingPosition,
                                                    baseColorVector,
                                                    frontFacing,
                                                    &sampleLinear);
                        const Point3D fragmentPosition{
                            (points[0].x * cw0 + points[1].x * cw1 +
                             points[2].x * cw2) / denominator +
                                preparedOffset.x,
                            (points[0].y * cw0 + points[1].y * cw1 +
                             points[2].y * cw2) / denominator +
                                preparedOffset.y,
                            (points[0].z * cw0 + points[1].z * cw1 +
                             points[2].z * cw2) / denominator +
                                preparedOffset.z};
                        if (fragmentSink != nullptr &&
                            !coverageResolveActive) {
                            observeWorkbenchFragment(
                                fragmentSink, painter,
                                logicalFromCoverageDevice.map(QPointF(px, py)),
                                fragmentPosition,
                                shadowReceiverNormal(sampleNormal, frontFacing,
                                                     sample.alphaF()),
                                cameraPosition, viewDirection,
                                sampleLinear, sample.alphaF() * coverage, objectId,
                                transform_);
                        }
                        if (coverageResolveActive) {
                            const int pixelX = qFloor(px);
                            const int pixelY = qFloor(py);
                            if (pixelX < 0 || pixelX >= coverageTargetWidth ||
                                pixelY < 0 || pixelY >= coverageTargetHeight) {
                                continue;
                            }
                            CoverageResolvePixel &resolved =
                                coverageResolvePixels[
                                    static_cast<size_t>(pixelY) *
                                        static_cast<size_t>(coverageTargetWidth) +
                                    static_cast<size_t>(pixelX)];
                            const qreal sampleAlpha = sample.alphaF();
                            resolved.premultipliedRgba[0] +=
                                sample.redF() * sampleAlpha * coverage;
                            resolved.premultipliedRgba[1] +=
                                sample.greenF() * sampleAlpha * coverage;
                            resolved.premultipliedRgba[2] +=
                                sample.blueF() * sampleAlpha * coverage;
                            resolved.premultipliedRgba[3] +=
                                static_cast<float>(sampleAlpha * coverage);
                            resolved.coverage += static_cast<float>(coverage);
                            if (fragmentSink != nullptr) {
                                CoverageResolveFragment &fragment =
                                    coverageResolveFragments[
                                        static_cast<size_t>(pixelY) *
                                            static_cast<size_t>(coverageTargetWidth) +
                                        static_cast<size_t>(pixelX)];
                                fragment.values[0] +=
                                    sampleLinear.x() * static_cast<float>(coverage);
                                fragment.values[1] +=
                                    sampleLinear.y() * static_cast<float>(coverage);
                                fragment.values[2] +=
                                    sampleLinear.z() * static_cast<float>(coverage);
                                fragment.values[3] +=
                                    static_cast<float>(fragmentPosition.x * coverage);
                                fragment.values[4] +=
                                    static_cast<float>(fragmentPosition.y * coverage);
                                fragment.values[5] +=
                                    static_cast<float>(fragmentPosition.z * coverage);
                                fragment.values[6] +=
                                    static_cast<float>(sampleAlpha * coverage);
                                const QVector3D receiverNormal =
                                    shadowReceiverNormal(sampleNormal,
                                                         frontFacing,
                                                         sample.alphaF());
                                fragment.values[7] += receiverNormal.x() *
                                                      static_cast<float>(coverage);
                                fragment.values[8] += receiverNormal.y() *
                                                      static_cast<float>(coverage);
                                fragment.values[9] += receiverNormal.z() *
                                                      static_cast<float>(coverage);
                            }
                        } else {
                            row[x] = qPremultiply(qRgba(
                                sample.red(), sample.green(), sample.blue(),
                                qRound(sample.alpha() * coverage)));
                        }
                    }
                }
                if (!coverageResolveActive) {
                    painter.save();
                    painter.setRenderHint(QPainter::SmoothPixmapTransform,
                                          false);
                    painter.setWorldTransform(QTransform());
                    bool baseInverseValid = false;
                    const QTransform baseLogicalFromDevice =
                        painter.deviceTransform().inverted(&baseInverseValid);
                    if (baseInverseValid) {
                        painter.drawImage(
                            baseLogicalFromDevice.map(QPointF(left, top)),
                            raster);
                    }
                    painter.restore();
                }
                return;
            }
            // QPainter has no perspective-correct textured-triangle primitive.
            // Rasterize the projected triangle and correct UV interpolation by
            // reciprocal camera depth, as the polygon-mesh fallback does.
            const int left = qFloor(std::clamp(
                std::min({devicePoints[0].x(), devicePoints[1].x(),
                          devicePoints[2].x()}),
                0.0, qreal(coverageTargetWidth)));
            const int top = qFloor(std::clamp(
                std::min({devicePoints[0].y(), devicePoints[1].y(),
                          devicePoints[2].y()}),
                0.0, qreal(coverageTargetHeight)));
            const int right = qCeil(std::clamp(
                std::max({devicePoints[0].x(), devicePoints[1].x(),
                          devicePoints[2].x()}),
                0.0, qreal(coverageTargetWidth)));
            const int bottom = qCeil(std::clamp(
                std::max({devicePoints[0].y(), devicePoints[1].y(),
                          devicePoints[2].y()}),
                0.0, qreal(coverageTargetHeight)));
            if (right <= left || bottom <= top) return;
            const int rasterWidth = right - left;
            const int rasterHeight = bottom - top;
            constexpr qint64 maximumRasterSamples = 4000000;
            const qint64 rasterArea = qint64(rasterWidth) * rasterHeight;
            const qreal sampleScale = rasterArea > maximumRasterSamples
                ? std::sqrt(static_cast<qreal>(rasterArea) /
                            static_cast<qreal>(maximumRasterSamples))
                : 1.0;
            const int sampleWidth = std::max(
                1, qFloor(static_cast<qreal>(rasterWidth) / sampleScale));
            const int sampleHeight = std::max(
                1, qFloor(static_cast<qreal>(rasterHeight) / sampleScale));
            const qreal sampleStepX =
                static_cast<qreal>(rasterWidth) / sampleWidth;
            const qreal sampleStepY =
                static_cast<qreal>(rasterHeight) / sampleHeight;
            const Point3D cameraWorld = transform_.cameraPosition(viewportSize);
            const Point3D viewDirection = transform_.viewDirection();
            const Point3D points[3] = {triangle.a, triangle.b, triangle.c};
            qreal reciprocal[3] = {1.0, 1.0, 1.0};
            if (transform_.isPerspectiveEnabled()) {
                for (int i=0; i<3; ++i) {
                    const qreal dx=points[i].x+preparedOffset.x-cameraWorld.x;
                    const qreal dy=points[i].y+preparedOffset.y-cameraWorld.y;
                    const qreal dz=points[i].z+preparedOffset.z-cameraWorld.z;
                    // viewDirection points from the scene toward the
                    // camera; perspective projection depth increases in
                    // the opposite (camera-forward) direction.
                    reciprocal[i] = 1.0 / std::max<qreal>(
                        -(dx*viewDirection.x+dy*viewDirection.y+dz*viewDirection.z),
                        1.0e-9);
                }
            }
            QImage raster(sampleWidth, sampleHeight,
                          QImage::Format_ARGB32_Premultiplied);
            raster.setDevicePixelRatio(targetDpr);
            raster.fill(Qt::transparent);
            for (int y = 0; y < raster.height(); ++y) {
                auto *row = reinterpret_cast<QRgb *>(raster.scanLine(y));
                for (int x = 0; x < raster.width(); ++x) {
                    const qreal px = left + (x + 0.5) * sampleStepX;
                    const qreal py = top + (y + 0.5) * sampleStepY;
                    qreal w0, w1, w2;
                    const qreal coverage = workbenchTrianglePixelCoverage(
                        devicePoints, area, px, py, sampleStepX, sampleStepY,
                        &w0, &w1, &w2, nullptr,
                        workbenchRasterAntialiasingEnabled_ ? 4 : 1);
                    if (coverage <= 0.0) continue;
                    const qreal cw0=w0*reciprocal[0], cw1=w1*reciprocal[1], cw2=w2*reciprocal[2];
                    const qreal denominator=cw0+cw1+cw2;
                    if (denominator <= 1.0e-12) continue;
                    const QPointF uv = (triangle.uvs[0]*cw0 + triangle.uvs[1]*cw1 + triangle.uvs[2]*cw2) / denominator;
                    QVector3D sampleNormal = triangle.normal;
                    if (triangle.hasSmoothNormals) {
                        sampleNormal =
                            (triangle.vertexNormals[0] * static_cast<float>(cw0) +
                             triangle.vertexNormals[1] * static_cast<float>(cw1) +
                             triangle.vertexNormals[2] * static_cast<float>(cw2)) /
                            static_cast<float>(denominator);
                        const float normalLengthSquared =
                            sampleNormal.lengthSquared();
                        if (std::isfinite(normalLengthSquared) &&
                            normalLengthSquared > 1.0e-12f) {
                            sampleNormal.normalize();
                        } else {
                            sampleNormal = triangle.normal;
                        }
                    }
                    uchar rgba[4];
                    float linearRgb[3]{};
                    float floatRgba[4]{};
                    const bool useNurbsFloat = nurbsFloatTexture.isValid() ||
                                               !nurbsFloatTextureTiles.isEmpty();
                    if (useNurbsFloat) {
                        const bool sampled = nurbsTiledTexture
                            ? sampleWorkbenchFloatUdimImage(
                                nurbsFloatTextureTiles, uv,
                                material.textureImageInterpolation,
                                material.textureImageExtension, floatRgba)
                            : (sampleWorkbenchFloatImage(
                                nurbsFloatTexture, uv,
                                material.textureImageInterpolation,
                                material.textureImageExtension, floatRgba),
                               nurbsFloatTexture.isValid());
                        if (!sampled) {
                            row[x] = qPremultiply(qRgba(255, 0, 255, 255));
                            continue;
                        }
                    } else if (nurbsTiledTexture) {
                        if (!sampleWorkbenchUdimImage(
                                nurbsTextureTiles, uv,
                                material.textureImageInterpolation,
                                material.textureImageExtension, rgba,
                                linearRgb, nurbsTextureSrgb)) {
                            row[x] = qPremultiply(qRgba(255, 0, 255, 255));
                            continue;
                        }
                    } else {
                        sampleWorkbenchImage(nurbsTexture, uv,
                                             material.textureImageInterpolation,
                                             material.textureImageExtension, rgba,
                                             linearRgb, nurbsTextureSrgb);
                    }
                    const float sampledAlpha = useNurbsFloat ? floatRgba[3]
                                                              : rgba[3] / 255.0f;
                    if (!textureAlphaIgnored && sampledAlpha < 0.1f) continue;
                    const qreal alpha = shaded.alphaF();
                    const bool premultiplied = material.textureImageAlphaMode.compare(
                        QStringLiteral("PREMUL"), Qt::CaseInsensitive) == 0;
                    QVector3D textureColor;
                    float unassociatedRgb[3]{};
                    for (int channel = 0; channel < 3; ++channel) {
                        const float linear = useNurbsFloat
                            ? (premultiplied && sampledAlpha > 0.0f
                                   ? floatRgba[channel] / sampledAlpha
                                   : floatRgba[channel])
                            : (premultiplied && rgba[3] > 0 && rgba[3] < 255
                                   ? linearRgb[channel] * 255.0f / rgba[3]
                                   : linearRgb[channel]);
                        unassociatedRgb[channel] = linear;
                    }
                    for (int channel = 0; channel < 3; ++channel)
                        textureColor[channel] = unassociatedRgb[channel];
                    Point3D shadingPosition = center;
                    if (transform_.isPerspectiveEnabled()) {
                        // Match the GPU fragment shader's view direction by
                        // shading from this pixel's perspective-correct world
                        // position instead of the triangle's center.
                        shadingPosition = {
                            (points[0].x * cw0 + points[1].x * cw1 +
                             points[2].x * cw2) / denominator,
                            (points[0].y * cw0 + points[1].y * cw1 +
                             points[2].y * cw2) / denominator,
                            (points[0].z * cw0 + points[1].z * cw1 +
                             points[2].z * cw2) / denominator};
                    }
                    QVector3D texturedLinear;
                    const QColor textured = shade(sampleNormal,
                                                  shadingPosition,
                                                  textureColor,
                                                  frontFacing,
                                                  &texturedLinear);
                    const Point3D fragmentPosition{
                        (points[0].x * cw0 + points[1].x * cw1 +
                         points[2].x * cw2) / denominator + preparedOffset.x,
                        (points[0].y * cw0 + points[1].y * cw1 +
                         points[2].y * cw2) / denominator + preparedOffset.y,
                        (points[0].z * cw0 + points[1].z * cw1 +
                         points[2].z * cw2) / denominator + preparedOffset.z};
                    if (fragmentSink != nullptr && !coverageResolveActive) {
                        observeWorkbenchFragment(
                            fragmentSink, painter,
                            logicalFromCoverageDevice.map(QPointF(px, py)),
                            fragmentPosition,
                            shadowReceiverNormal(sampleNormal, frontFacing,
                                                 textured.alphaF() * coverage),
                            cameraPosition, viewDirection,
                            texturedLinear, alpha * coverage, objectId, transform_);
                    }
                    const int red = textured.red();
                    const int green = textured.green();
                    const int blue = textured.blue();
                    if (coverageResolveActive) {
                        const int pixelX = qFloor(px);
                        const int pixelY = qFloor(py);
                        if (pixelX < 0 || pixelX >= coverageTargetWidth ||
                            pixelY < 0 || pixelY >= coverageTargetHeight)
                            continue;
                        const size_t pixelIndex =
                            static_cast<size_t>(pixelY) *
                                static_cast<size_t>(coverageTargetWidth) +
                            static_cast<size_t>(pixelX);
                        CoverageResolvePixel &resolved =
                            coverageResolvePixels[pixelIndex];
                        resolved.premultipliedRgba[0] +=
                            textured.redF() * alpha * coverage;
                        resolved.premultipliedRgba[1] +=
                            textured.greenF() * alpha * coverage;
                        resolved.premultipliedRgba[2] +=
                            textured.blueF() * alpha * coverage;
                        resolved.premultipliedRgba[3] += alpha * coverage;
                        resolved.coverage += coverage;
                        if (fragmentSink != nullptr) {
                            CoverageResolveFragment &fragment =
                                coverageResolveFragments[pixelIndex];
                            fragment.values[0] += texturedLinear.x() * coverage;
                            fragment.values[1] += texturedLinear.y() * coverage;
                            fragment.values[2] += texturedLinear.z() * coverage;
                            fragment.values[3] += fragmentPosition.x * coverage;
                            fragment.values[4] += fragmentPosition.y * coverage;
                            fragment.values[5] += fragmentPosition.z * coverage;
                            fragment.values[6] += alpha * coverage;
                            const QVector3D receiverNormal =
                                shadowReceiverNormal(sampleNormal, frontFacing,
                                                     textured.alphaF());
                            fragment.values[7] += receiverNormal.x() * coverage;
                            fragment.values[8] += receiverNormal.y() * coverage;
                            fragment.values[9] += receiverNormal.z() * coverage;
                        }
                    } else {
                        row[x] = qPremultiply(qRgba(
                            red, green, blue,
                            qRound(alpha * coverage * 255)));
                    }
                }
            }
            painter.save();
            painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
            painter.setWorldTransform(QTransform());
            bool baseInverseValid = false;
            const QTransform baseLogicalFromDevice =
                painter.deviceTransform().inverted(&baseInverseValid);
            if (baseInverseValid) {
                const QRectF targetRect(
                    baseLogicalFromDevice.map(QPointF(left, top)),
                    baseLogicalFromDevice.map(QPointF(right, bottom)));
                painter.drawImage(
                    targetRect.normalized(), raster);
            }
            painter.restore();
        };

        const auto drawTriangle = [&](const SurfaceFillTriangle &triangle) {
            struct ClipVertex {
                Point3D position;
                QVector3D normal;
                QPointF uv;
            };
            const ViewportCameraPreferences preferences =
                transform_.cameraPreferences();
            const Point3D target = transform_.viewTarget();
            const Point3D camera = transform_.cameraPosition(viewportSize);
            const qreal nearDepth = preferences.clipStart;
            const qreal farDepth = transform_.isPerspectiveEnabled()
                                       ? preferences.clipEnd
                                       : 2.0 * preferences.clipEnd;
            const auto depthOf = [&](const Point3D &position) {
                const Point3D world{position.x + preparedOffset.x,
                                    position.y + preparedOffset.y,
                                    position.z + preparedOffset.z};
                const Point3D forward{-transform_.viewDirection().x,
                                      -transform_.viewDirection().y,
                                      -transform_.viewDirection().z};
                if (transform_.isPerspectiveEnabled()) {
                    return (world.x - camera.x) * forward.x +
                           (world.y - camera.y) * forward.y +
                           (world.z - camera.z) * forward.z;
                }
                return preferences.clipEnd +
                       (world.x - target.x) * forward.x +
                       (world.y - target.y) * forward.y +
                       (world.z - target.z) * forward.z;
            };
            QVector<ClipVertex> polygon{
                {triangle.a, triangle.vertexNormals[0], triangle.uvs[0]},
                {triangle.b, triangle.vertexNormals[1], triangle.uvs[1]},
                {triangle.c, triangle.vertexNormals[2], triangle.uvs[2]}};
            const qreal clipPlanes[] = {nearDepth, farDepth};
            for (int planeIndex = 0; planeIndex < 2 && !polygon.isEmpty();
                 ++planeIndex) {
                const bool keepGreater = planeIndex == 0;
                const qreal planeDepth = clipPlanes[planeIndex];
                QVector<ClipVertex> clipped;
                clipped.reserve(polygon.size() + 1);
                ClipVertex previous = polygon.last();
                qreal previousDepth = depthOf(previous.position);
                bool previousInside = keepGreater
                                          ? previousDepth >= planeDepth
                                          : previousDepth <= planeDepth;
                for (const ClipVertex &current : polygon) {
                    const qreal currentDepth = depthOf(current.position);
                    const bool currentInside = keepGreater
                                                   ? currentDepth >= planeDepth
                                                   : currentDepth <= planeDepth;
                    if (currentInside != previousInside) {
                        const qreal denominator = currentDepth - previousDepth;
                        if (std::abs(denominator) > 1.0e-15) {
                            const qreal t = std::clamp(
                                (planeDepth - previousDepth) / denominator,
                                0.0, 1.0);
                            ClipVertex intersection;
                            intersection.position = {
                                previous.position.x +
                                    (current.position.x - previous.position.x) * t,
                                previous.position.y +
                                    (current.position.y - previous.position.y) * t,
                                previous.position.z +
                                    (current.position.z - previous.position.z) * t};
                            intersection.normal = previous.normal +
                                (current.normal - previous.normal) *
                                    static_cast<float>(t);
                            intersection.uv = previous.uv +
                                (current.uv - previous.uv) * t;
                            clipped.append(intersection);
                        }
                    }
                    if (currentInside) clipped.append(current);
                    previous = current;
                    previousDepth = currentDepth;
                    previousInside = currentInside;
                }
                polygon = std::move(clipped);
            }
            for (int index = 1; index + 1 < polygon.size(); ++index) {
                SurfaceFillTriangle clipped = triangle;
                clipped.a = polygon[0].position;
                clipped.b = polygon[index].position;
                clipped.c = polygon[index + 1].position;
                if (triangle.hasSmoothNormals) {
                    clipped.vertexNormals = {polygon[0].normal,
                                             polygon[index].normal,
                                             polygon[index + 1].normal};
                }
                if (triangle.hasUvs) {
                    clipped.uvs = {polygon[0].uv, polygon[index].uv,
                                   polygon[index + 1].uv};
                }
                drawUnclippedTriangle(clipped);
            }
        };

        painter.save();
        if (preparedGeometry != nullptr &&
            preparedGeometry->surfaceVertices.size() >= 3) {
            const QVector<QVector3D> &vertices =
                preparedGeometry->surfaceVertices;
            const QVector<QVector3D> &normals =
                preparedGeometry->surfaceNormals;
            for (int index = 0; index + 2 < vertices.size(); index += 3) {
                const QVector3D normal =
                    normals.size() > index + 2
                        ? normals[index] + normals[index + 1] + normals[index + 2]
                        : QVector3D::crossProduct(vertices[index + 1] - vertices[index],
                                                  vertices[index + 2] - vertices[index]);
                std::array<QVector3D, 3> vertexNormals{};
                bool hasSmoothNormals = normals.size() > index + 2;
                if (hasSmoothNormals) {
                    for (int corner = 0; corner < 3; ++corner) {
                        vertexNormals[corner] = normals[index + corner];
                        const float lengthSquared =
                            vertexNormals[corner].lengthSquared();
                        if (!std::isfinite(lengthSquared) ||
                            lengthSquared <= 1.0e-12f) {
                            hasSmoothNormals = false;
                            break;
                        }
                        vertexNormals[corner].normalize();
                    }
                }
                const auto point = [&preparedOffset](const QVector3D &value) {
                    return Point3D{value.x(), value.y(), value.z()};
                };
                std::array<QPointF, 3> uvs{};
                const bool hasUvs = preparedGeometry->surfaceTextureCoordinates.size() == vertices.size();
                if (hasUvs) for (int corner=0; corner<3; ++corner) {
                    const QPointF uv = preparedGeometry->surfaceTextureCoordinates[index+corner];
                    uvs[corner] = QPointF(uv.x(), 1.0 - uv.y());
                }
                appendTriangle(point(vertices[index]), point(vertices[index + 1]),
                               point(vertices[index + 2]), normal, uvs, hasUvs,
                               vertexNormals, hasSmoothNormals);
            }
        } else {
            PreparedNurbsSurfaceTessellation localTessellation;
            QSharedPointer<const PreparedNurbsSurfaceTessellation> cachedTessellation;
            const PreparedNurbsSurfaceTessellation *tessellation = nullptr;
            if (surfaceTessellationCache_ != nullptr) {
                cachedTessellation = surfaceTessellationCache_->acquire(
                    objectId, geometryRevision, surface);
                tessellation = cachedTessellation.data();
            } else if (localTessellation.prepare(surface)) {
                tessellation = &localTessellation;
            }
            if (tessellation != nullptr) {
                const QVector<QPointF> &textureParameters =
                    tessellation->vertexTextureParameters();
                const QVector<double> fullKnotsU =
                    expandedNurbsSurfaceKnotVector(surface.knotsU);
                const QVector<double> fullKnotsV =
                    expandedNurbsSurfaceKnotVector(surface.knotsV);
                const bool textureParametersValid =
                    textureParameters.size() == tessellation->vertices().size() &&
                    fullKnotsU.size() > surface.degreeU &&
                    fullKnotsU.size() > surface.controlVertexCountU &&
                    fullKnotsV.size() > surface.degreeV &&
                    fullKnotsV.size() > surface.controlVertexCountV &&
                    fullKnotsU[surface.controlVertexCountU] > fullKnotsU[surface.degreeU] &&
                    fullKnotsV[surface.controlVertexCountV] > fullKnotsV[surface.degreeV];
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
                    std::array<QPointF, 3> uvs{};
                    if (textureParametersValid) {
                        const int indices[3] = {triangle[0], triangle[1], triangle[2]};
                        for (int corner = 0; corner < 3; ++corner) {
                            const QPointF parameter = textureParameters[indices[corner]];
                            const qreal u = (parameter.x() - fullKnotsU[surface.degreeU]) /
                                (fullKnotsU[surface.controlVertexCountU] -
                                 fullKnotsU[surface.degreeU]);
                            const qreal v = (parameter.y() - fullKnotsV[surface.degreeV]) /
                                (fullKnotsV[surface.controlVertexCountV] -
                                 fullKnotsV[surface.degreeV]);
                            uvs[corner] = QPointF(u, 1.0 - v);
                        }
                    }
                    appendTriangle(a, b, c, QVector3D::crossProduct(ab, ac),
                                   uvs, textureParametersValid);
                }
            }
        }
        std::stable_sort(fillTriangles.begin(), fillTriangles.end(),
                         [](const SurfaceFillTriangle &a,
                            const SurfaceFillTriangle &b) {
                             return a.depth < b.depth;
                         });
        // A planar NURBS surface's tessellation triangles are one surface, so
        // resolve their subpixel coverage together before source-over
        // blending. This prevents a transparent internal diagonal from being
        // composited as two partially covered layers.
        const bool hasSingleImageTexture =
            !nurbsTiledTexture && !nurbsTexture.isNull() &&
            nurbsFloatTextureTiles.isEmpty();
        const bool hasSingleFloatTexture =
            !nurbsTiledTexture && nurbsTexture.isNull() &&
            nurbsFloatTexture.isValid() && nurbsFloatTextureTiles.isEmpty();
        const bool hasTiledTextureForCoverage = nurbsTiledTexture &&
            (!nurbsTextureTiles.isEmpty() ||
             !nurbsFloatTextureTiles.isEmpty());
        if (((!nurbsTiledTexture && nurbsTexture.isNull() &&
              !nurbsFloatTexture.isValid() && nurbsFloatTextureTiles.isEmpty()) ||
             hasSingleImageTexture || hasSingleFloatTexture ||
             hasTiledTextureForCoverage) &&
            !fillTriangles.isEmpty() &&
            qint64(coverageTargetWidth) * coverageTargetHeight <= 1000000 &&
            qint64(fillTriangles.size()) <= 100000) {
            const QVector3D referenceNormal =
                workbenchNormalize(fillTriangles.first().normal);
            const Point3D referencePoint = fillTriangles.first().a;
            qreal minimumDepth = fillTriangles.first().depth;
            qreal maximumDepth = minimumDepth;
            qreal planeScale = 1.0;
            bool trianglesSharePlane = true;
            for (const SurfaceFillTriangle &triangle : fillTriangles) {
                const Point3D *points[] = {&triangle.a, &triangle.b,
                                           &triangle.c};
                for (const Point3D *point : points) {
                    const QVector3D offset(
                        static_cast<float>(point->x - referencePoint.x),
                        static_cast<float>(point->y - referencePoint.y),
                        static_cast<float>(point->z - referencePoint.z));
                    planeScale = std::max<qreal>(
                        planeScale, offset.length());
                    if (std::abs(QVector3D::dotProduct(
                            referenceNormal, offset)) >
                        1.0e-6 * planeScale) {
                        trianglesSharePlane = false;
                        break;
                    }
                }
                if (!trianglesSharePlane) break;
            }
            coverageResolveActive = true;
            for (const SurfaceFillTriangle &triangle : fillTriangles) {
                if ((hasSingleImageTexture || hasSingleFloatTexture ||
                     hasTiledTextureForCoverage) && !triangle.hasUvs) {
                    coverageResolveActive = false;
                    break;
                }
                const QVector3D normal = workbenchNormalize(triangle.normal);
                if (QVector3D::dotProduct(referenceNormal, normal) <
                        1.0f - 1.0e-6f) {
                    coverageResolveActive = false;
                    break;
                }
                minimumDepth = std::min(minimumDepth, triangle.depth);
                maximumDepth = std::max(maximumDepth, triangle.depth);
            }
            coverageResolveActive = coverageResolveActive &&
                (transform_.isPerspectiveEnabled()
                     ? trianglesSharePlane
                     : maximumDepth - minimumDepth < 1.0e-5);
            if (coverageResolveActive) {
                const size_t pixelCount =
                    static_cast<size_t>(coverageTargetWidth) *
                    static_cast<size_t>(coverageTargetHeight);
                coverageResolvePixels.resize(pixelCount);
                if (fragmentSink != nullptr) {
                    coverageResolveFragments.resize(pixelCount);
                }
            }
        }
        for (const SurfaceFillTriangle &triangle : fillTriangles) {
            drawTriangle(triangle);
        }
        if (coverageResolveActive) {
            QImage resolvedRaster(coverageTargetWidth, coverageTargetHeight,
                                  QImage::Format_ARGB32_Premultiplied);
            resolvedRaster.setDevicePixelRatio(targetDpr);
            resolvedRaster.fill(Qt::transparent);
            for (int y = 0; y < coverageTargetHeight; ++y) {
                auto *row = reinterpret_cast<QRgb *>(resolvedRaster.scanLine(y));
                for (int x = 0; x < coverageTargetWidth; ++x) {
                    const CoverageResolvePixel &resolved =
                        coverageResolvePixels[
                            static_cast<size_t>(y) *
                                static_cast<size_t>(coverageTargetWidth) +
                            static_cast<size_t>(x)];
                    if (resolved.coverage <= 0.0) continue;
                    const qreal scale = resolved.coverage > 1.0
                        ? 1.0 / resolved.coverage
                        : 1.0;
                    row[x] = qRgba(
                        qRound(std::clamp(resolved.premultipliedRgba[0] *
                                             scale,
                                         0.0, 1.0) * 255.0),
                        qRound(std::clamp(resolved.premultipliedRgba[1] *
                                             scale,
                                         0.0, 1.0) * 255.0),
                        qRound(std::clamp(resolved.premultipliedRgba[2] *
                                             scale,
                                         0.0, 1.0) * 255.0),
                        qRound(std::clamp(resolved.premultipliedRgba[3] *
                                             scale,
                                         0.0, 1.0) * 255.0));
                    if (fragmentSink != nullptr &&
                            coverageResolveFragments[
                                static_cast<size_t>(y) *
                                    static_cast<size_t>(coverageTargetWidth) +
                                static_cast<size_t>(x)].values[6] > 0.0f) {
                        const qreal inverseCoverage =
                            1.0 / resolved.coverage;
                        const CoverageResolveFragment &fragment =
                            coverageResolveFragments[
                                static_cast<size_t>(y) *
                                    static_cast<size_t>(coverageTargetWidth) +
                                static_cast<size_t>(x)];
                        const Point3D worldPosition{
                            fragment.values[3] * inverseCoverage,
                            fragment.values[4] * inverseCoverage,
                            fragment.values[5] * inverseCoverage};
                        const QVector3D worldNormal = workbenchNormalize(
                            QVector3D(fragment.values[7], fragment.values[8],
                                      fragment.values[9]));
                        observeWorkbenchFragment(
                            fragmentSink, painter,
                            logicalFromCoverageDevice.map(
                                QPointF(x + 0.5, y + 0.5)),
                            worldPosition, worldNormal,
                            cameraPosition, viewDirection,
                            QVector3D(fragment.values[0], fragment.values[1],
                                      fragment.values[2]) /
                                resolved.coverage,
                            std::min<qreal>(fragment.values[6] * scale, 1.0),
                            objectId, transform_);
                    }
                }
            }
            painter.save();
            painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
            painter.setWorldTransform(QTransform());
            bool baseInverseValid = false;
            const QTransform baseLogicalFromDevice =
                painter.deviceTransform().inverted(&baseInverseValid);
            if (baseInverseValid) {
                painter.drawImage(
                    baseLogicalFromDevice.map(QPointF(0.0, 0.0)),
                    resolvedRaster);
            }
            painter.restore();
        }
        painter.restore();
        return;
    }

    QVector<QLineF> screenLines;
    QVector<Point3D> depthWireVertices;
    const auto appendScreenLine = [&](const Point3D &start,
                                      const Point3D &end) {
        Point3D clippedStart = start;
        Point3D clippedEnd = end;
        if (!clipWorkbenchSegmentToViewDepth(
                transform_, viewportSize, &clippedStart, &clippedEnd)) {
            return;
        }
        QPointF screenStart;
        QPointF screenEnd;
        if (transform_.worldPointToScreenUnclipped(clippedStart,
                                                   viewportSize,
                                                   &screenStart) &&
            transform_.worldPointToScreenUnclipped(clippedEnd,
                                                   viewportSize,
                                                   &screenEnd)) {
            screenLines.append(QLineF(screenStart, screenEnd));
            depthWireVertices.append(clippedStart);
            depthWireVertices.append(clippedEnd);
        }
    };

    if (blenderWireVerticesWorld != nullptr &&
        !blenderWireVerticesWorld->isEmpty()) {
        screenLines.reserve(blenderWireVerticesWorld->size() / 2);
        for (int index = 0; index + 1 < blenderWireVerticesWorld->size(); index += 2) {
            const auto moved = [&preparedOffset](const Point3D &point) {
                return Point3D{point.x + preparedOffset.x,
                               point.y + preparedOffset.y,
                               point.z + preparedOffset.z};
            };
            appendScreenLine(moved(blenderWireVerticesWorld->at(index)),
                             moved(blenderWireVerticesWorld->at(index + 1)));
        }
    } else if (preparedGeometry != nullptr &&
        !preparedGeometry->preciseLineVertices.isEmpty()) {
        const QVector<Point3D> &worldLines =
            preparedGeometry->preciseLineVertices;
        screenLines.reserve(worldLines.size() / 2);
        for (int index = 0; index + 1 < worldLines.size(); index += 2) {
            const auto moved = [&preparedOffset](const Point3D &point) {
                return Point3D{point.x + preparedOffset.x,
                               point.y + preparedOffset.y,
                               point.z + preparedOffset.z};
            };
            appendScreenLine(moved(worldLines[index]), moved(worldLines[index + 1]));
        }
    } else {
        PreparedNurbsSurfaceTessellation localTessellation;
        QSharedPointer<const PreparedNurbsSurfaceTessellation> cachedTessellation;
        const PreparedNurbsSurfaceTessellation *tessellation = nullptr;
        if (surfaceTessellationCache_ != nullptr) {
            cachedTessellation = surfaceTessellationCache_->acquire(
                objectId, geometryRevision, surface);
            tessellation = cachedTessellation.data();
        } else if (localTessellation.prepare(surface)) {
            tessellation = &localTessellation;
        }
        if (tessellation == nullptr) {
            return;
        }
        int segmentCount = 0;
        for (const PreparedNurbsSurfaceTessellation::Polyline &polyline :
             tessellation->wireframe()) {
            segmentCount += std::max(
                0, static_cast<int>(polyline.points.size()) - 1);
        }
        screenLines.reserve(segmentCount);
        for (const PreparedNurbsSurfaceTessellation::Polyline &polyline :
             tessellation->wireframe()) {
            for (int index = 1; index < polyline.points.size(); ++index) {
                const auto moved = [&preparedOffset](const Point3D &point) {
                    return Point3D{point.x + preparedOffset.x,
                                   point.y + preparedOffset.y,
                                   point.z + preparedOffset.z};
                };
                appendScreenLine(moved(polyline.points[index - 1]),
                                 moved(polyline.points[index]));
            }
        }
    }
    if (!screenLines.isEmpty()) {
        painter.save();
        if (!preview) {
            painter.setRenderHint(QPainter::Antialiasing, smoothWiresOverlay_);
        }
        if (!preview && opaqueSceneDepth != nullptr &&
            opaqueSceneDepthSize.width() > 0 &&
            opaqueSceneDepthSize.height() > 0 &&
            opaqueSceneDepth->size() ==
                opaqueSceneDepthSize.width() * opaqueSceneDepthSize.height() &&
            !depthWireVertices.isEmpty()) {
            ViewportDepthGeometry surfaceWireGeometry;
            surfaceWireGeometry.preciseLineVertices =
                std::move(depthWireVertices);
            const bool depthTested = drawPreparedCurveSegments(
                painter, surfaceWireGeometry, viewportSize, opaqueSceneDepth,
                opaqueSceneDepthSize);
            painter.restore();
            if (depthTested) return;
            painter.save();
            if (!preview) {
                painter.setRenderHint(QPainter::Antialiasing,
                                      smoothWiresOverlay_);
            }
        }
        painter.drawLines(screenLines.constData(), screenLines.size());
        painter.restore();
    }
}

void ViewportRenderer::drawCircularArc(QPainter &painter,
                                       const QPointF &start,
                                       const QPointF &end,
                                       const QPointF &through,
                                       const QSize &viewportSize) const
{
    CircularArc2D arc;
    if (!makeCircularArcThroughPoint(start, end, through, &arc)) {
        painter.drawLine(worldToScreen(start, viewportSize),
                         worldToScreen(end, viewportSize));
        return;
    }
    drawNurbsCurve(painter, arc.curve, viewportSize);
}

void ViewportRenderer::drawCenterArcWithSweep(QPainter &painter,
                                              const QPointF &centerWorld,
                                              const QPointF &startWorld,
                                              qreal sweepAngle,
                                              const QSize &viewportSize) const
{
    const QPointF startVector = startWorld - centerWorld;
    const qreal radius = std::hypot(startVector.x(), startVector.y());
    if (radius <= 1e-9 || std::abs(sweepAngle) <= 1e-9) {
        return;
    }

    CircularArc2D arc;
    if (makeCircularArcFromCenterSweep(
            centerWorld,
            radius,
            std::atan2(startVector.y(), startVector.x()),
            sweepAngle,
            &arc)) {
        drawNurbsCurve(painter, arc.curve, viewportSize);
    }
}

void ViewportRenderer::drawCenterArc(QPainter &painter,
                                     const QPointF &centerWorld,
                                     const QPointF &startWorld,
                                     const QPointF &endWorld,
                                     const QSize &viewportSize) const
{
    const QPointF startVector = startWorld - centerWorld;
    const QPointF endVector = endWorld - centerWorld;
    const qreal radius = std::hypot(startVector.x(), startVector.y());
    if (radius <= 1e-9) {
        painter.drawLine(worldToScreen(startWorld, viewportSize),
                         worldToScreen(endWorld, viewportSize));
        return;
    }

    constexpr qreal pi = 3.14159265358979323846;
    constexpr qreal twoPi = 2.0 * pi;
    const qreal startAngle = std::atan2(startVector.y(), startVector.x());
    const qreal endAngle = std::atan2(endVector.y(), endVector.x());
    qreal sweepAngle = endAngle - startAngle;
    if (sweepAngle > pi) {
        sweepAngle -= twoPi;
    } else if (sweepAngle < -pi) {
        sweepAngle += twoPi;
    }

    if (std::abs(sweepAngle) <= 1e-9) {
        painter.drawLine(worldToScreen(startWorld, viewportSize),
                         worldToScreen(endWorld, viewportSize));
        return;
    }
    drawCenterArcWithSweep(painter,
                           centerWorld,
                           startWorld,
                           sweepAngle,
                           viewportSize);
}

} // namespace classiCAD
