#pragma once

#include "core/document/object_id.h"
#include "core/document/scene_object.h"
#include "core/document/shape.h"
#include "services/hit_testing/curve_hit_tester.h"
#include "services/sampling/surface_tessellation_cache.h"
#include "services/viewport/viewport_transform.h"
#include "blender_grid_appearance.h"
#include "viewport_shading.h"
#include "viewport_raster_workbench_linear.h"

#include <QPainter>
#include <QColor>
#include <QSize>

#include <optional>

namespace classiCAD {

struct ViewportDepthGeometry;

QColor workbenchRandomColor(ObjectId objectId,
                            const QString &displayName,
                            const QString &libraryPath);

class ViewportRenderer {
public:
    ViewportRenderer(const ViewportTransform &transform,
                     const CurveHitTester &curveHitTester,
                     const SurfaceTessellationCache *surfaceTessellationCache = nullptr);

    void drawGrid(QPainter &painter, const QSize &viewportSize) const;
    void drawOrigin(QPainter &painter, const QSize &viewportSize) const;
    void setGridAppearance(const BlenderGridAppearance &appearance);
    void setShadingSettings(const ViewportShadingSettings &settings);
    void setWorkbenchRasterAntialiasingEnabled(bool enabled);
    void setSmoothWirePreferences(bool overlay, bool editMode);
    void setGridBaseStep(qreal baseGridStep);
    void drawShape(QPainter &painter,
                   const Shape &shape,
                   const QSize &viewportSize,
                   bool preview,
                   bool selected = false,
                   bool drawPreviewPoints = true,
                   const QColor &layerColor = QColor(),
                   const QString &layerLineType = QString(),
                   qreal layerLineWeightMm = 0.0,
                   ObjectId shapeObjectId = ObjectId::invalid(),
                   quint64 geometryRevision = 0,
                   const ViewportDepthGeometry *preparedGeometry = nullptr,
                   const Point3D &preparedOffset = {},
                   std::optional<QVector4D> displayColorSceneLinear = std::nullopt,
                   std::optional<WorkbenchMaterialData> workbenchMaterial =
                       std::nullopt,
                   const QString &displayName = QString(),
                   const QString &libraryPath = QString(),
                   const MeshObjectTransform &meshTransform =
                       identityMeshObjectTransform,
                   const QVector<WorkbenchMaterialData> &meshMaterials = {},
                   RasterWorkbenchFragmentSink *fragmentSink = nullptr,
                   const QHash<QString, WorkbenchMaterialData> &nurbsSolidFaceMaterials = {},
                   const QVector<float> *opaqueSceneDepth = nullptr,
                   const QSize &opaqueSceneDepthSize = {},
                   const QVector<Point3D> *blenderSurfaceWireVerticesWorld =
                       nullptr) const;
    void setSmoothCurveDisplay(bool enabled);
    void drawNurbsCurve(QPainter &painter,
                        const Shape::NurbsCurve2D &curve,
                        const QSize &viewportSize) const;
    void drawNurbsCurve(QPainter &painter,
                        const Shape::NurbsCurve2D &curve,
                        const WorkPlaneFrame &frame,
                        const QSize &viewportSize) const;
    void drawNurbsSurface(QPainter &painter,
                          const Shape::NurbsSurface3D &surface,
                          const QSize &viewportSize,
                          ObjectId objectId = ObjectId::invalid(),
                          quint64 geometryRevision = 0,
                          const ViewportDepthGeometry *preparedGeometry = nullptr,
                          const Point3D &preparedOffset = {},
                          bool preview = false,
                          bool selected = false,
                          bool reverseOrientation = false,
                          const QColor &objectColor = QColor(),
                          std::optional<QVector4D> displayColorSceneLinear =
                              std::nullopt,
                          std::optional<WorkbenchMaterialData> workbenchMaterial =
                              std::nullopt,
                          const QString &displayName = QString(),
                          const QString &libraryPath = QString(),
                          RasterWorkbenchFragmentSink *fragmentSink = nullptr,
                          const QVector<float> *opaqueSceneDepth = nullptr,
                          const QSize &opaqueSceneDepthSize = {},
                          const QVector<Point3D> *blenderWireVerticesWorld =
                              nullptr) const;
    void drawControlPoints(QPainter &painter,
                           const Shape &shape,
                           const QSize &viewportSize,
                           ObjectId shapeObjectId,
                           ObjectId selectedObjectId,
                           bool draggingControlPoint,
                           int activeControlPointIndex,
                           bool drawMarkers = true,
                           const QVector<float> *regularSceneDepth = nullptr,
                           const QSize &regularDepthSize = {},
                           const QVector<float> *inFrontSceneDepth = nullptr,
                           const QSize &inFrontDepthSize = {},
                           bool showInFront = false) const;
    void drawSubdivisionPoints(QPainter &painter,
                               const Shape &shape,
                               const QVector<double> &parameters,
                               const QSize &viewportSize,
                               bool preview) const;
    void setArchitecturalDimensionFont(bool enabled);

    void drawCircularArc(QPainter &painter,
                         const QPointF &start,
                         const QPointF &end,
                         const QPointF &through,
                         const QSize &viewportSize) const;
    void drawCenterArcWithSweep(QPainter &painter,
                                const QPointF &centerWorld,
                                const QPointF &startWorld,
                                qreal sweepAngle,
                                const QSize &viewportSize) const;

private:
    QPointF worldToScreen(const QPointF &world,
                          const QSize &viewportSize) const;
    QPointF screenToWorld(const QPointF &screen,
                          const QSize &viewportSize) const;
    bool isValidNurbsCurve(const Shape::NurbsCurve2D &curve) const;
    bool evaluateNurbsPoint(const Shape::NurbsCurve2D &curve,
                            qreal parameter,
                            QPointF *point) const;
    bool drawPreparedCurveSegments(
        QPainter &painter,
        const ViewportDepthGeometry &preparedGeometry,
        const QSize &viewportSize,
        const QVector<float> *opaqueSceneDepth = nullptr,
        const QSize &opaqueSceneDepthSize = {}) const;
    bool subdivisionCurve(const Shape &shape,
                          Shape::NurbsCurve2D *curve) const;
    QVector<QPointF> rectangleVertices(const Shape &shape) const;
    void drawCenterArc(QPainter &painter,
                       const QPointF &centerWorld,
                       const QPointF &startWorld,
                       const QPointF &endWorld,
                       const QSize &viewportSize) const;

    const ViewportTransform &transform_;
    const CurveHitTester &curveHitTester_;
    const SurfaceTessellationCache *surfaceTessellationCache_ = nullptr;
    bool smoothCurveDisplay_ = true;
    bool workbenchRasterAntialiasingEnabled_ = true;
    bool smoothWiresOverlay_ = true;
    bool smoothWiresEditMode_ = true;
    bool architecturalDimensionFont_ = false;
    BlenderGridAppearance gridAppearance_;
    ViewportShadingSettings shadingSettings_;
    qreal gridBaseStep_ = 1.0;
};

} // namespace classiCAD
