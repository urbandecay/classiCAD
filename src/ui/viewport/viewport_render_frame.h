/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "core/document/object_id.h"
#include "core/document/scene_object.h"
#include "core/document/shape.h"
#include "core/tool_id.h"
#include "core/geometry/work_plane.h"
#include "services/viewport/viewport_transform.h"
#include "tools/tool.h"

#include <QColor>
#include <QHash>
#include <QSet>
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
    QHash<quint64, QSet<int>> controlPointIndices;

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
    enum class Visibility {
        Viewport,
        Render,
    };

    Visibility visibility = Visibility::Viewport;
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
    QString displayName;
    QString libraryPath;
    QString workbenchRandomLibraryPath;
    QVector<Point3D> blenderSurfaceWireVerticesWorld;
    int objectIndex = -1;
    quint64 geometryRevision = 0;
    bool cacheable = true;
    Point3D placementTranslation;
    MeshObjectTransform meshObjectTransform = identityMeshObjectTransform;
    QVector<WorkbenchMaterialData> meshMaterials;
    QHash<QString, WorkbenchMaterialData> nurbsSolidFaceMaterials;
    QSharedPointer<const ViewportDepthGeometry> preparedDepthGeometry;
    // The immutable mesh may stay at its original position after translation.
    quint64 preparedGeometryRevision = 0;
    Point3D preparedGeometryOffset;

    QColor layerColor;
    QVector4D displayColorSceneLinear{1.0f, 1.0f, 1.0f, 1.0f};
    WorkbenchMaterialData workbenchMaterial;
    bool hasWorkbenchMaterial = false;
    QString layerLineType;
    qreal layerLineWeightMm = 0.0;
    // Solid surface perimeter is rendered through ViewportSceneRenderer so
    // it shares the same pixel width and antialiasing as spline strokes.
    bool outlineHandledBySceneStroke = false;
    bool selected = false;
    bool showInFront = false;
    bool showWire = false;
    bool showAllEdges = false;
    bool wireDisplay = false;
    bool blenderLooseMeshEdgeCompanion = false;
    bool blenderLooseMeshEdgeSourceSelected = false;
    bool blenderLooseMeshEdgeSourceActive = false;
    bool blenderLooseMeshEdgeSourceTransforming = false;
    QString displayBoundsType;
    bool hasDisplayBoundsData = false;
    Point3D displayBoundsMinimumLocal;
    Point3D displayBoundsMaximumLocal;
    MeshObjectTransform displayBoundsTransformWorld =
        identityMeshObjectTransform;
    // Blender's SURFACE tessellation has the opposite winding convention
    // from classiCAD's native U/V triangle expansion.
    bool reverseSurfaceOrientation = false;
    bool scalePreview = false;
    bool rotatePreview = false;

    bool highlighted() const noexcept
    {
        return selected || scalePreview || rotatePreview;
    }
};

inline QString workbenchRandomIdentityLibraryPath(
    const ViewportRenderObject &object)
{
    return object.workbenchRandomLibraryPath.isEmpty()
               ? object.libraryPath
               : object.workbenchRandomLibraryPath;
}

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
