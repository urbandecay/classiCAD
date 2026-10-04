/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "core/document/object_id.h"
#include "core/document/shape.h"
#include "core/tool_id.h"
#include "core/geometry/work_plane.h"
#include "services/viewport/viewport_transform.h"
#include "tools/tool.h"

#include <QColor>
#include <QHash>
#include <QSharedPointer>
#include <QSize>
#include <QString>
#include <QVector>

namespace classiCAD {

class Document;
struct ViewportDepthGeometry;

enum class ViewportRenderTransformKind {
    None,
    Scale,
    Rotate,
};

struct ViewportRenderTransform {
    ViewportRenderTransformKind kind = ViewportRenderTransformKind::None;
    QVector<ObjectId> objectIds;

    QPointF scaleBasePoint;
    QPointF scaleAxis{1.0, 0.0};
    qreal scaleFactor = 1.0;
    ScaleMode scaleMode = ScaleMode::TwoD;
    WorkPlaneFrame scaleSurfaceFrame;

    Point3D rotatePivot;
    Point3D rotateAxis{0.0, 0.0, 1.0};
    qreal rotateAngle = 0.0;
};

struct ViewportRenderFrameInput {
    QVector<ObjectId> selectedObjectIds;
    ObjectId primarySelectedObjectId = ObjectId::invalid();
    QVector<ObjectId> highlightedObjectIds;
    ViewportRenderTransform transformPreview;
    ToolPreview activeToolPreview;
};

// One immutable snapshot of the visible scene and its presentation inputs.
// Geometry is copied so transient transform previews cannot mutate Document.
struct ViewportRenderObject {
    Shape shape;
    ObjectId objectId = ObjectId::invalid();
    int objectIndex = -1;
    quint64 geometryRevision = 0;
    bool cacheable = true;
    QSharedPointer<const ViewportDepthGeometry> preparedDepthGeometry;

    QColor layerColor;
    QString layerLineType;
    qreal layerLineWeightMm = 0.0;
    bool selected = false;
    bool scalePreview = false;
    bool rotatePreview = false;

    bool highlighted() const noexcept
    {
        return selected || scalePreview || rotatePreview;
    }
};

struct ViewportRenderFrame {
    ViewportTransform camera;
    QSize viewportSize;
    QVector<ViewportRenderObject> objects;
    ToolPreview activeToolPreview;

    const ViewportRenderObject *find(ObjectId objectId) const noexcept;

private:
    friend ViewportRenderFrame buildViewportRenderFrame(
        const Document &, const ViewportTransform &, const QSize &,
        const ViewportRenderFrameInput &);
    QHash<quint64, int> objectIndices;
};

ViewportRenderFrame buildViewportRenderFrame(
    const Document &document,
    const ViewportTransform &camera,
    const QSize &viewportSize,
    const ViewportRenderFrameInput &input);

} // namespace classiCAD
