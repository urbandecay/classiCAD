#pragma once

#include "core/document/shape.h"
#include "core/geometry/shape_mapping.h"
#include "services/viewport/viewport_transform.h"

#include <QSize>
#include <QVector3D>

namespace classiCAD {

inline constexpr float kWorkbenchImageEmptyAlphaCutoff = 0.05f;

inline bool viewportPictureFrameVisibleInView(
    const Shape &shape, const ViewportTransform &transform)
{
    if (shape.geometryType != GeometryType::Picture) return true;
    const bool perspective = transform.isPerspectiveEnabled();
    if ((perspective && !shape.pictureShowPerspective) ||
        (!perspective && !shape.pictureShowOrthographic)) {
        return false;
    }
    return true;
}

inline bool viewportPictureImageVisibleInView(
    const Shape &shape, const ViewportTransform &transform,
    const QSize &viewportSize)
{
    if (!viewportPictureFrameVisibleInView(shape, transform)) return false;
    if (shape.geometryType != GeometryType::Picture) return true;
    const bool perspective = transform.isPerspectiveEnabled();

    const QVector<QPointF> corners = pictureFrameCorners(shape);
    if (corners.size() != 4) return false;
    const Point3D first = shapePointToWorld(shape, corners[0]);
    const Point3D second = shapePointToWorld(shape, corners[1]);
    const Point3D fourth = shapePointToWorld(shape, corners[3]);
    const QVector3D edgeX(float(second.x - first.x),
                          float(second.y - first.y),
                          float(second.z - first.z));
    const QVector3D edgeY(float(first.x - fourth.x),
                          float(first.y - fourth.y),
                          float(first.z - fourth.z));
    QVector3D imageNormal = QVector3D::crossProduct(edgeX, edgeY);
    if (imageNormal.lengthSquared() <= 1.0e-16f) return false;
    imageNormal.normalize();

    Point3D center{};
    for (const QPointF &corner : corners) {
        const Point3D world = shapePointToWorld(shape, corner);
        center.x += world.x * 0.25;
        center.y += world.y * 0.25;
        center.z += world.z * 0.25;
    }

    const Point3D view = transform.viewDirection();
    QVector3D viewAxis(float(view.x), float(view.y), float(view.z));
    if (viewAxis.lengthSquared() <= 1.0e-16f) return false;
    viewAxis.normalize();
    if (shape.pictureOnlyAxisAligned) {
        const QVector3D projectedNormal = imageNormal - viewAxis *
            QVector3D::dotProduct(imageNormal, viewAxis);
        if (projectedNormal.lengthSquared() > 1.0e-5f) return false;
    }

    if (shape.pictureImageSide != PictureImageSide::Both) {
        QVector3D towardViewer = viewAxis;
        if (perspective) {
            const Point3D camera = transform.cameraPosition(viewportSize);
            towardViewer = QVector3D(float(camera.x - center.x),
                                    float(camera.y - center.y),
                                    float(camera.z - center.z));
        }
        const float side = QVector3D::dotProduct(imageNormal, towardViewer);
        const float epsilon = perspective ? 0.0f : 1.0e-5f;
        if ((shape.pictureImageSide == PictureImageSide::Front &&
             side < epsilon) ||
            (shape.pictureImageSide == PictureImageSide::Back &&
             side > -epsilon)) {
            return false;
        }
    }
    return true;
}

inline bool viewportPictureVisibleInView(const Shape &shape,
                                         const ViewportTransform &transform,
                                         const QSize &viewportSize)
{
    return viewportPictureImageVisibleInView(shape, transform, viewportSize);
}

} // namespace classiCAD
