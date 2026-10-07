#include "viewport_widget_api.h"
#include "../app/application_session.h"
#include "../app/command_router.h"
#include "../app/project_controller.h"
#include "../app/session_serializer.h"
#include "../core/model.h" // Transitional declarations used by this legacy adapter.
#include "../core/document/document.h"
#include "../core/document/document_settings.h"
#include "../core/document/selection_model.h"
#include "../core/commands/delete_command.h"
#include "../core/commands/duplicate_command.h"
#include "../core/commands/explode_command.h"
#include "../core/commands/fill_command.h"
#include "../core/commands/trim_erase_command.h"
#include "../core/debug_log.h"
#include "../core/geometry/circle_construction.h"
#include "../core/geometry/arc_curve_factory.h"
#include "../core/geometry/curve_evaluator.h"
#include "../core/geometry/curve_erase_intervals.h"
#include "../core/geometry/curve_editing.h"
#include "../core/geometry/curve_join.h"
#include "../core/geometry/curve_subdivision.h"
#include "../core/geometry/geometry_transform.h"
#include "../core/geometry/nurbs_surface_factory.h"
#include "../core/geometry/nurbs_surface_evaluator.h"
#include "../core/geometry/nurbs_solid.h"
#include "../core/geometry/shape_mapping.h"
#include "../core/geometry/work_plane.h"
#include "../core/history/history.h"
#include "../core/serialization/document_serializer.h"
#include "../core/serialization/blender_project_file.h"
#include "../core/serialization/rhino3dm_interchange.h"
#include "../services/hit_testing/curve_hit_tester.h"
#include "../services/hit_testing/selection_box_query.h"
#include "../services/input/drawing_plane_resolver.h"
#include "../services/input/input_constraint_service.h"
#include "../services/dimensions/dimension_association.h"
#include "../services/erase/curve_erase_query.h"
#include "../services/erase/trim_erase_query.h"
#include "../services/sampling/curve_sampler.h"
#include "../services/sampling/surface_tessellation_cache.h"
#include "../services/snapping/snap_engine.h"
#include "../services/viewport/viewport_transform.h"
#include "../tools/tool_context.h"
#include "../tools/tool_input.h"
#include "../tools/tool_registry.h"
#include "../tools/select_tool.h"
#include "../tools/arc_tool.h"
#include "../tools/rotate_tool.h"
#include "../tools/mirror_tool.h"
#include "../tools/scale_tool.h"
#include "../tools/join_tool.h"
#include "../tools/subdivision_tool.h"
#include "../tools/grab_tool.h"
#include "../tools/duplicate_tool.h"
#include "../tools/erase_tool.h"
#include "../tools/trim_tool.h"
#include "input_helpers.h"
#include "input/tool_input_translator.h"
#include "viewport/blender_grid_renderer.h"
#include "viewport/viewport_shading.h"
#include "viewport/navigation_controller.h"
#include "viewport/viewport_control_point_renderer.h"
#include "viewport/viewport_gpu_surface.h"
#include "viewport/viewport_hud_renderer.h"
#include "viewport/viewport_scene_renderer.h"
#include "viewport/viewport_overlay.h"
#include "viewport/viewport_erase_overlay_renderer.h"
#include "viewport/viewport_renderer.h"
#include "viewport/viewport_render_frame.h"
#include "viewport/viewport_geometry_cache.h"

#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QBuffer>
#include <QCheckBox>
#include <QColorDialog>
#include <QCursor>
#include <QDateTime>
#include <QDebug>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFormLayout>
#include <QFrame>
#include <QHash>
#include <QSet>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QInputDialog>
#include <QLabel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineF>
#include <QImageReader>
#include <QKeyEvent>
#include <QMap>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QPixmap>
#include <QRadialGradient>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QStringList>
#include <QStyle>
#include <QTextStream>
#include <QToolButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>

namespace classiCAD {

namespace {

constexpr qreal kDragSnapBreakawayPixels = 36.0;
constexpr qreal kViewportShadingButtonSize = 20.0;
constexpr qreal kViewportShadingButtonGap = 1.0;
constexpr qreal kViewportShadingPanelPadding = 2.0;
constexpr qreal kViewportShadingSettingsButtonWidth = 15.0;
constexpr qreal kComponentModeButtonSize = kViewportShadingButtonSize;
constexpr qreal kComponentModeButtonGap = kViewportShadingButtonGap;

enum class ComponentSelectionMode { Vertex, Edge, Face };

struct ComponentPickCycleState {
    bool valid = false;
    QPointF position;
    ObjectId object = ObjectId::invalid();
    int componentIndex = -1;
};

QRectF componentModePanelRect(const QSize &size)
{
    const qreal width = kComponentModeButtonSize * 3.0 +
                        kComponentModeButtonGap * 2.0 +
                        kViewportShadingPanelPadding * 2.0;
    const qreal height = kComponentModeButtonSize +
                         kViewportShadingPanelPadding * 2.0;
    return QRectF(8.0, size.height() - 12.0 - height, width, height);
}

QRectF componentModeButtonRect(const QSize &size, int index)
{
    const QRectF panel = componentModePanelRect(size);
    const qreal x = panel.left() + kViewportShadingPanelPadding + index *
                    (kComponentModeButtonSize + kComponentModeButtonGap);
    return QRectF(x, panel.top() + kViewportShadingPanelPadding,
                  kComponentModeButtonSize, kComponentModeButtonSize);
}

const QPixmap &blenderSelectionModeIcon(int mode, bool active)
{
    // Retain the pixmaps across frames, just like the shading toolbar icons.
    // Destroying temporary pixmaps during an OpenGL paint invalidates textures
    // while Qt still has their bindings cached for the remaining draws.
    static const std::array<QPixmap, 6> icons = [] {
        const std::array<QString, 3> paths{
            QStringLiteral(":/blender-shading/selection_vertex.png"),
            QStringLiteral(":/blender-shading/selection_edge.png"),
            QStringLiteral(":/blender-shading/selection_face.png")};
        std::array<QPixmap, 6> result;
        for (int state = 0; state < 2; ++state) {
            for (int index = 0; index < 3; ++index) {
                QImage image(paths[index]);
                image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
                QPainter tint(&image);
                tint.setCompositionMode(QPainter::CompositionMode_SourceIn);
                tint.fillRect(image.rect(), state ? QColor(235, 239, 246)
                                                 : QColor(205, 205, 205));
                tint.end();
                result[state * 3 + index] = QPixmap::fromImage(std::move(image));
            }
        }
        return result;
    }();
    return icons[(active ? 3 : 0) + mode];
}

QRectF viewportShadingPanelRect(const QSize &size)
{
    const qreal buttonWidth = kViewportShadingButtonSize * 3.0 +
                              kViewportShadingButtonGap * 3.0 +
                              kViewportShadingSettingsButtonWidth;
    const qreal width = buttonWidth + kViewportShadingPanelPadding * 2.0;
    const qreal height = kViewportShadingButtonSize +
                         kViewportShadingPanelPadding * 2.0;
    return QRectF(size.width() - 12.0 - width,
                  size.height() - 12.0 - height,
                  width,
                  height);
}

QRectF viewportShadingButtonRect(const QSize &size, int index)
{
    const QRectF panel = viewportShadingPanelRect(size);
    return QRectF(panel.left() + kViewportShadingPanelPadding +
                      index * (kViewportShadingButtonSize +
                               kViewportShadingButtonGap),
                  panel.top() + kViewportShadingPanelPadding,
                  kViewportShadingButtonSize,
                  kViewportShadingButtonSize);
}

QRectF viewportShadingSettingsButtonRect(const QSize &size)
{
    const QRectF panel = viewportShadingPanelRect(size);
    return QRectF(panel.left() + kViewportShadingPanelPadding +
                      3.0 * (kViewportShadingButtonSize +
                             kViewportShadingButtonGap),
                  panel.top() + kViewportShadingPanelPadding,
                  kViewportShadingSettingsButtonWidth,
                  kViewportShadingButtonSize);
}

QColor viewportShadingBackgroundColor(
    const ViewportShadingSettings &settings)
{
    if (settings.backgroundMode == ViewportBackgroundMode::Custom) {
        return settings.customBackgroundColor;
    }
    if (settings.backgroundMode == ViewportBackgroundMode::World) {
        // classiCAD has no scene World datablock yet; this is the current
        // viewport world's neutral background color.
        return QColor(48, 48, 48);
    }
    return {};
}

void fillViewportBackground(QPainter &painter,
                            const QRect &bounds,
                            const QColor &solidColor = QColor())
{
    if (bounds.isEmpty()) {
        return;
    }
    if (solidColor.isValid()) {
        painter.fillRect(bounds, solidColor);
        return;
    }
    const QPointF center(bounds.x() + bounds.width() * 0.5,
                         bounds.y() + bounds.height() * 0.5);
    const qreal radius = 0.5 * std::hypot(bounds.width(), bounds.height());
    QRadialGradient gradient(center, std::max<qreal>(radius, 1.0), center);
    constexpr qreal gamma = 2.2;
    constexpr qreal high = 61.0 / 255.0;
    constexpr qreal low = 48.0 / 255.0;
    constexpr int stopCount = 32;
    for (int stopIndex = 0; stopIndex <= stopCount; ++stopIndex) {
        const qreal factor = static_cast<qreal>(stopIndex) / stopCount;
        const qreal gammaColor = std::pow(
            std::pow(high, 1.0 / gamma) * (1.0 - factor) +
                std::pow(low, 1.0 / gamma) * factor,
            gamma);
        const QColor color = QColor::fromRgbF(gammaColor,
                                              gammaColor,
                                              gammaColor);
        gradient.setColorAt(factor, color);
    }
    painter.fillRect(bounds, gradient);
}

Shape pictureFrameOutline(const Shape &picture)
{
    Shape frame = picture;
    frame.geometryType = GeometryType::Polygon;
    frame.pictureImage = QImage();
    frame.pictureImageData.clear();
    const QVector<QPointF> corners = pictureFrameCorners(picture);
    if (corners.size() == 4) {
        QVector<QPointF> closedCorners = corners;
        closedCorners.append(closedCorners.first());
        frame.nurbs = makeDegreeOneNurbs(closedCorners);
    }
    return frame;
}

QString arcLengthUnitSuffix(DocumentLengthUnit unit)
{
    switch (unit) {
    case DocumentLengthUnit::Millimeter:
        return QStringLiteral("mm");
    case DocumentLengthUnit::Centimeter:
        return QStringLiteral("cm");
    case DocumentLengthUnit::Meter:
        return QStringLiteral("m");
    case DocumentLengthUnit::Inch:
        return QStringLiteral("in");
    case DocumentLengthUnit::Foot:
        return QStringLiteral("ft");
    }
    return QStringLiteral("mm");
}

QString precisePoint3DText(const Point3D &point)
{
    return QStringLiteral("(%1,%2,%3)")
        .arg(point.x, 0, 'g', 12)
        .arg(point.y, 0, 'g', 12)
        .arg(point.z, 0, 'g', 12);
}

ViewportDepthGeometry selectedSurfaceCage(const Shape &shape)
{
    ViewportDepthGeometry geometry;
    const QVector<NurbsSurface3D> faces = shapeSurfaceFaces(shape);
    QVector<Point3D> uniquePoints;
    const auto appendSegment = [&geometry, &uniquePoints](const Point3D &a,
                                                           const Point3D &b) {
        const auto close = [](const Point3D &lhs, const Point3D &rhs) {
            const double dx = lhs.x - rhs.x;
            const double dy = lhs.y - rhs.y;
            const double dz = lhs.z - rhs.z;
            return dx * dx + dy * dy + dz * dz <= 1.0e-16;
        };
        for (int i = 0; i + 1 < geometry.preciseLineVertices.size(); i += 2) {
            const Point3D &first = geometry.preciseLineVertices[i];
            const Point3D &second = geometry.preciseLineVertices[i + 1];
            if ((close(first, a) && close(second, b)) ||
                (close(first, b) && close(second, a))) {
                return;
            }
        }
        geometry.preciseLineVertices.append(a);
        geometry.preciseLineVertices.append(b);
        geometry.lineVertices.append(QVector3D(float(a.x), float(a.y), float(a.z)));
        geometry.lineVertices.append(QVector3D(float(b.x), float(b.y), float(b.z)));
    };
    const auto appendVertex = [&geometry, &uniquePoints](const Point3D &point) {
        const auto close = [](const Point3D &lhs, const Point3D &rhs) {
            const double dx = lhs.x - rhs.x;
            const double dy = lhs.y - rhs.y;
            const double dz = lhs.z - rhs.z;
            return dx * dx + dy * dy + dz * dz <= 1.0e-16;
        };
        if (std::none_of(uniquePoints.cbegin(), uniquePoints.cend(),
                         [&point, &close](const Point3D &existing) {
                             return close(point, existing);
                         })) {
            uniquePoints.append(point);
            geometry.pointVertices.append(QVector3D(float(point.x),
                                                    float(point.y),
                                                    float(point.z)));
        }
    };
    for (const NurbsSurface3D &face : faces) {
        PreparedNurbsSurfaceEvaluator evaluator;
        if (!evaluator.prepare(face)) {
            continue;
        }
        QVector<QVector<QPointF>> boundaries;
        QVector<QPointF> topologyVertices;
        if (!face.trimLoops.isEmpty()) {
            for (const NurbsSurfaceTrimLoop &loop : face.trimLoops) {
                if (loop.curve.degree == 1) {
                    boundaries.append(loop.curve.controlPoints);
                } else {
                    boundaries.append(sampleNurbsSurfaceTrimLoop(loop, 64));
                }
                topologyVertices += loop.curve.controlPoints;
            }
        } else {
            qreal u0 = 0.0, u1 = 0.0, v0 = 0.0, v1 = 0.0;
            if (!evaluator.parameterDomains(&u0, &u1, &v0, &v1)) {
                continue;
            }
            QVector<double> uBreaks;
            QVector<double> vBreaks;
            const auto appendKnotBreaks = [](const QVector<double> &knots,
                                             qreal start,
                                             qreal end,
                                             QVector<double> *breaks) {
                for (double knot : knots) {
                    if (knot >= start - 1.0e-10 &&
                        knot <= end + 1.0e-10 &&
                        (breaks->isEmpty() ||
                         std::abs(breaks->last() - knot) > 1.0e-10)) {
                        breaks->append(knot);
                    }
                }
                if (breaks->isEmpty() || std::abs(breaks->first() - start) > 1.0e-10) {
                    breaks->prepend(start);
                }
                if (std::abs(breaks->last() - end) > 1.0e-10) {
                    breaks->append(end);
                }
            };
            if (face.degreeU == 1 && face.degreeV == 1) {
                appendKnotBreaks(expandedNurbsSurfaceKnotVector(face.knotsU),
                                 u0, u1, &uBreaks);
                appendKnotBreaks(expandedNurbsSurfaceKnotVector(face.knotsV),
                                 v0, v1, &vBreaks);
                for (double u : uBreaks) {
                    for (double v : vBreaks) {
                        Point3D point;
                        if (evaluator.evaluate(u, v, &point)) {
                            appendVertex(point);
                        }
                    }
                }
                for (double u : uBreaks) {
                    Point3D previous;
                    if (!evaluator.evaluate(u, vBreaks.first(), &previous)) {
                        continue;
                    }
                    for (int i = 1; i < vBreaks.size(); ++i) {
                        Point3D current;
                        if (evaluator.evaluate(u, vBreaks[i], &current)) {
                            appendSegment(previous, current);
                            previous = current;
                        }
                    }
                }
                for (double v : vBreaks) {
                    Point3D previous;
                    if (!evaluator.evaluate(uBreaks.first(), v, &previous)) {
                        continue;
                    }
                    for (int i = 1; i < uBreaks.size(); ++i) {
                        Point3D current;
                        if (evaluator.evaluate(uBreaks[i], v, &current)) {
                            appendSegment(previous, current);
                            previous = current;
                        }
                    }
                }
                continue;
            }
            QVector<QPointF> rectangle;
            rectangle << QPointF(u0, v0) << QPointF(u1, v0)
                      << QPointF(u1, v1) << QPointF(u0, v1);
            boundaries.append(rectangle);
            topologyVertices += rectangle;
        }
        for (const QVector<QPointF> &boundary : boundaries) {
            if (boundary.size() < 2) {
                continue;
            }
            Point3D previous;
            if (!evaluator.evaluate(boundary.first().x(), boundary.first().y(),
                                    &previous)) {
                continue;
            }
            for (int i = 1; i <= boundary.size(); ++i) {
                Point3D current;
                const QPointF uv = boundary[i % boundary.size()];
                if (evaluator.evaluate(uv.x(), uv.y(), &current)) {
                    appendSegment(previous, current);
                    previous = current;
                }
            }
        }
        for (const QPointF &uv : topologyVertices) {
            Point3D point;
            if (evaluator.evaluate(uv.x(), uv.y(), &point)) {
                appendVertex(point);
            }
        }
    }
    return geometry;
}

} // namespace

struct TransientPreviewStroke {
    Shape shape;
    QColor color;
    float width = 1.5f;
    bool controlGuide = false;
    float pointDiameter = 0.0f;
    bool dashed = false;
    bool pointOutline = false;
    Point3D worldOffset;
    bool opaqueSurface = false;
};

struct TransientPreviewPicture {
    Shape image;
    Shape frame;
    int duplicateIndex = -1;
    int sceneShapeIndex = -1;
    ObjectId mirrorObjectId = ObjectId::invalid();
    bool activeToolPreview = false;
    float opacity = 0.78f;
    QColor frameColor = QColor(QStringLiteral("#e6b85c"));
};

enum class DragAxisLock {
    None,
    X,
    Y,
    Z,
};

QString dragAxisLockName(DragAxisLock lock)
{
    switch (lock) {
    case DragAxisLock::X:
        return QStringLiteral("X");
    case DragAxisLock::Y:
        return QStringLiteral("Y");
    case DragAxisLock::Z:
        return QStringLiteral("Z");
    case DragAxisLock::None:
        return QStringLiteral("None");
    }

    return QStringLiteral("None");
}

QString precisePointText(const QPointF &point)
{
    return QStringLiteral("(%1, %2)")
        .arg(point.x(), 0, 'g', 12)
        .arg(point.y(), 0, 'g', 12);
}

struct ArcHudDisplay {
    QString dimensionsLine;
    QString instructionsLine;
};

class ViewportWidget final : public ViewportWidgetApi {
public:
    explicit ViewportWidget(ApplicationSession *session = nullptr,
                            QWidget *parent = nullptr)
        : ViewportWidgetApi(parent)
        , ownedSession_(session == nullptr
                            ? std::make_unique<ApplicationSession>()
                            : nullptr)
        , session_(session == nullptr ? *ownedSession_ : *session)
        , document_(session_.document())
        , selection_(session_.selection())
        , history_(session_.history())
        , curveHitTester_(&surfaceTessellationCache_)
        , viewportRenderer_(viewportTransform_,
                            curveHitTester_,
                            &surfaceTessellationCache_)
        , viewportOverlay_(viewportRenderer_, viewportTransform_)
        , navigationGizmo_(viewportTransform_)
        , navigationController_(
              *this,
              viewportTransform_,
              navigationGizmo_,
              NavigationControllerCallbacks{
                  [this](const QPointF &position) { beginOrbitAt(position); },
                  [this] {
                      return activeTool_ == Tool::Select ? Qt::ArrowCursor
                                                         : Qt::CrossCursor;
                  },
                  [this] { update(); },
                  [this] { emitCoordinateUpdate(); },
                  [this] { notifyViewStateChanged(); }})
        , toolContext_(document_,
                       selection_,
                       history_,
                       viewportTransform_,
                       curveSampler_,
                       curveHitTester_,
                       snapEngine_)
        , toolRegistry_(session_.toolRegistry())
        , arcTool_(static_cast<ArcTool &>(*toolRegistry_.find(Tool::Arc)))
        , rotateTool_(static_cast<RotateTool &>(*toolRegistry_.find(Tool::Rotate)))
        , mirrorTool_(static_cast<MirrorTool &>(*toolRegistry_.find(Tool::Mirror)))
        , scaleTool_(static_cast<ScaleTool &>(*toolRegistry_.find(Tool::Scale)))
        , trimTool_(static_cast<TrimTool &>(*toolRegistry_.find(Tool::Trim)))
        , eraseTool_(static_cast<EraseTool &>(*toolRegistry_.find(Tool::Erase)))
        , shapes_(document_)
        , pan_(viewportTransform_.pan())
        , selectedShapeIndices_(selection_.objectIds())
        , selectedShapeIndex_(selection_.primaryObjectId())
        , controlPointIndex_(selection_.activeControlPoint().index)
        , zoom_(viewportTransform_.zoom())
    {
        configureCommandRouter();
        setupShadingPopover();
        viewportTransform_.setGridSpacing(
            documentGridSpacingInMillimeters(document_.settings()));
        toolContext_.setShapeFactory(
            [this](ToolId tool,
                   const QVector<QPointF> &points,
                   ArcMode arcMode,
                   qreal arcSweep,
                   Shape *shape) {
                return makeToolShape(tool, points, arcMode, arcSweep, shape);
        });
        toolContext_.setShapeCommitter([this](ToolId tool, const Shape &shape) {
            Shape committedShape = shape;
            if (!isValidWorkPlaneFrame(committedShape.workPlaneFrame)) {
                committedShape.workPlane = viewportTransform_.workPlane();
                committedShape.workPlaneOffset = viewportTransform_.workPlaneOffset();
                committedShape.workPlaneFrame = viewportTransform_.workPlaneFrame();
            }
            DocumentTransaction transaction = session_.beginTransaction();
            if (!transaction.addShape(committedShape).isValid() ||
                !session_.commitTransaction(transaction)) {
                return false;
            }
            DebugLog::instance().write(QStringLiteral("tool commit tool=%1 shapes=%2")
                                           .arg(toolName(tool))
                                           .arg(shapes_.size()));
            return true;
        });
        toolContext_.setToolFinisher([this](ToolId tool) {
            setTool(tool);
            if (commandFinished_) {
                commandFinished_(tool);
            }
        });
        toolContext_.setTransactionCommitter(
            [this](DocumentTransaction &transaction) {
                return session_.commitTransaction(transaction);
            });
        toolContext_.setLayersChangedNotifier(
            [this]() { notifyLayersChanged(); });
        toolContext_.setSelectionChangedNotifier(
            [this]() { session_.notifySelectionChanged(); });
        toolContext_.setShapesCommitter([this](ToolId, const QVector<Shape> &shapes) {
            if (shapes.isEmpty()) return false;
            for (const Shape &shape : shapes) {
                if (shape.geometryType == GeometryType::NurbsSurface) {
                    if (!validateNurbsSurface(shape.nurbsSurface)) return false;
                } else if (shape.geometryType == GeometryType::NurbsSolid) {
                    if (!validateNurbsSolid(shape.nurbsSolid)) return false;
                } else if (!isValidWorkPlaneFrame(shape.workPlaneFrame)) {
                    return false;
                } else if (shape.geometryType == GeometryType::Point) {
                    if (shape.points.size() != 1 ||
                        !std::isfinite(shape.points.first().x()) ||
                        !std::isfinite(shape.points.first().y())) return false;
                } else if (!validateNurbsCurve(shape.nurbs)) {
                    return false;
                }
            }
            DocumentTransaction transaction = session_.beginTransaction();
            for (const Shape &shape : shapes) {
                if (!transaction.addShape(shape).isValid()) {
                    transaction.rollback();
                    return false;
                }
            }
            return session_.commitTransaction(transaction);
        });
        toolContext_.setPreviewPublisher([this](const ToolPreview &preview) {
            toolPreview_ = preview;
            // Rendering remains in the viewport for now, but the pending
            // points themselves are owned by the active tool. Arc stores its
            // staged points in ArcTool's interaction record.
            if (activeTool_ == Tool::Arc) {
                pendingPoints_ = arcTool_.inputPoints();
            } else {
                pendingPoints_ = preview.points;
            }
            if (preview.hasWorkPlaneFrame && activeTool_ != Tool::Line) {
                toolDrawingFrame_ = preview.workPlaneFrame;
                toolDrawingPlaneLocked_ = preview.planeLocked;
                if (toolDrawingPlaneLocked_) {
                    viewportTransform_.setWorkPlaneFrame(toolDrawingFrame_);
                }
            }
            if (activeTool_ == Tool::Line) {
                if (preview.overridesSnap) currentSnap_ = preview.snap;
                if (preview.hasCursorPoint) {
                    cursorWorld_ = preview.cursorPoint;
                    cursorValid_ = true;
                }
            }
            if (activeTool_ == Tool::PointExtrude && preview.overridesSnap) {
                currentSnap_ = preview.snap;
                if (preview.hasCursorPoint &&
                    isValidWorkPlaneFrame(viewportTransform_.workPlaneFrame())) {
                    cursorWorld_ = worldPointToWorkPlaneFrame(
                        preview.worldCursorPoint,
                        viewportTransform_.workPlaneFrame());
                    cursorValid_ = true;
                }
            }
            if (preview.hasShape &&
                !isValidWorkPlaneFrame(toolPreview_.shape.workPlaneFrame)) {
                toolPreview_.shape.workPlane = viewportTransform_.workPlane();
                toolPreview_.shape.workPlaneOffset =
                    viewportTransform_.workPlaneOffset();
                toolPreview_.shape.workPlaneFrame =
                    viewportTransform_.workPlaneFrame();
            }
            if (isCircleConstructionTool(activeTool_)) {
                if (preview.hasCursorPoint) {
                    cursorWorld_ = preview.cursorPoint;
                    cursorValid_ = true;
                }
            }
            if (isEllipseTool(activeTool_)) {
                cursorValid_ = preview.hasCursorPoint;
                if (preview.hasCursorPoint) {
                    cursorWorld_ = preview.cursorPoint;
                }
            }
            if (isRectangleTool(activeTool_)) {
                cursorValid_ = preview.hasCursorPoint;
                if (preview.hasCursorPoint) cursorWorld_ = preview.cursorPoint;
            }
            if (isPolygonTool(activeTool_)) {
                cursorValid_ = preview.hasCursorPoint;
                if (preview.hasCursorPoint) {
                    cursorWorld_ = preview.cursorPoint;
                }
            }
            if (isPointCreationTool(activeTool_) && activeTool_ != Tool::Point) {
                if (activeTool_ == Tool::PointByLine && preview.overridesSnap) {
                    currentSnap_ = preview.snap;
                }
                if (preview.hasCursorPoint && activeTool_ == Tool::PointByLine) {
                    cursorWorld_ = preview.cursorPoint;
                    cursorValid_ = true;
                }
            }
            update();
        });
        toolContext_.setStatusPublisher([this](const ToolStatus &status) {
            toolStatus_ = status;
            if (activeTool_ == Tool::CircleTangentThree && toolStatusUpdate_) {
                toolStatusUpdate_(status.text);
            }
        });
        toolContext_.setPointConstraint(
            [this](ToolId, const QPointF &rawPoint, const QVector<QPointF> &) {
                return constrainLinePoint(rawPoint);
            });
        toolContext_.setArcModeProvider([this] {
            return arcState().mode;
        });
        toolContext_.setArcSweepProvider([this] {
            return arcState().previewSweepAngle;
        });
        toolContext_.setPolygonSideCountCallbacks(
            [this] { return polygonSideCount_; },
            [this](int sideCount) {
                polygonSideCount_ = std::clamp(sideCount, 3, 1001);
            });
        toolContext_.setSelectionInteractionCallbacks(
            [this](const QPointF &screenPosition, bool includeControlPoint) {
                if (includeControlPoint &&
                    (controlPointsVisible_ ||
                     componentSelectionMode_ == ComponentSelectionMode::Vertex)) {
                    int shapeIndex = -1;
                    int controlPointIndex = -1;
                    if (hitTestSelectedControlPoint(screenPosition,
                                                    &shapeIndex,
                                                    &controlPointIndex)) {
                        return SelectionHit{shapes_.objectIdAt(shapeIndex),
                                            controlPointIndex};
                    }
                }
                const int shapeIndex = hitTestShape(screenPosition);
                return SelectionHit{shapes_.objectIdAt(shapeIndex), -1};
            },
            [this](SelectionGestureKind gesture,
                   const ToolInput &input,
                   const SelectionHit &hit,
                   bool additive) {
                rawCursorWorld_ = input.rawWorldPosition;
                cursorWorld_ = input.rawWorldPosition;
                lastWorldPosition_ = input.rawWorldPosition;
                cursorValid_ = true;

                if (gesture == SelectionGestureKind::BeginSelectionBox) {
                    beginSelectionBox(input.screenPosition, additive);
                    return;
                }

                dragHistoryRecorded_ = false;
                currentDragSnap_ = DragSnapResult{};
                dragSnapLocked_ = false;
                dragAxisLock_ = DragAxisLock::None;
                if (gesture == SelectionGestureKind::BeginControlPointDrag) {
                    controlPointDragFrame_ = controlPointWorkPlaneFrame(
                        hit.objectId, hit.controlPointIndex);
                    controlPointDragFrameValid_ =
                        isValidWorkPlaneFrame(controlPointDragFrame_);
                    controlPointLastCursorScreen_ = input.screenPosition;
                    controlPointCursorOffsetScreen_ = {};
                    QPointF controlPointCursor;
                    if (controlPointDragFrameValid_ &&
                        viewportTransform_.screenToWorkPlane(
                            input.screenPosition,
                            size(),
                            controlPointDragFrame_,
                            &controlPointCursor)) {
                        setSelectionLastControlPointWorldPosition(
                            controlPointCursor);
                    }
                    const Shape *dragShape = document_.shape(hit.objectId);
                    if (controlPointDragFrameValid_ && dragShape != nullptr &&
                        hit.controlPointIndex >= 0) {
                        const QVector<QPointF> controlPoints =
                            controlPointsForShape(*dragShape);
                        if (hit.controlPointIndex < controlPoints.size()) {
                            const Point3D controlPointWorld =
                                workPlaneFramePointToWorld(
                                    controlPoints[hit.controlPointIndex],
                                    controlPointDragFrame_);
                            QPointF controlPointScreen;
                            if (viewportTransform_.worldPointToScreen(
                                    controlPointWorld,
                                    size(),
                                    &controlPointScreen)) {
                                controlPointCursorOffsetScreen_ =
                                    controlPointScreen - input.screenPosition;
                            }
                        }
                    }
                    setCursor(Qt::SizeAllCursor);
                    DebugLog::instance().write(
                        QStringLiteral("control point drag start shape=%1 index=%2 world=%3")
                            .arg(objectIndex(hit.objectId))
                            .arg(hit.controlPointIndex)
                            .arg(pointText(selectionLastControlPointWorldPosition())));
                    return;
                }

                controlPointDragFrameValid_ = false;
                controlPointCursorOffsetScreen_ = {};
                setFocus(Qt::MouseFocusReason);
                setCursor(Qt::SizeAllCursor);
                const int shapeIndex = objectIndex(selection_.primaryObjectId());
                const QString geometryName = shapeIndex >= 0 && shapeIndex < shapes_.size()
                    ? geometryTypeName(shapes_[shapeIndex].geometryType)
                    : QStringLiteral("unknown");
                DebugLog::instance().write(
                        QStringLiteral("selection hit shape=%1 tool=%2 dragStart=%3")
                        .arg(shapeIndex)
                        .arg(geometryName)
                        .arg(pointText(selectionLastDragWorldPosition())));
            });
        setMinimumSize(480, 320);
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        qApp->installEventFilter(this);
        setCursor(Qt::CrossCursor);
        if (ViewportGpuSurface::isSupported()) {
            gpuSurface_ = new ViewportGpuSurface(this);
            gpuSurface_->setSurfaceTessellationCache(
                &surfaceTessellationCache_);
            gpuSurface_->setGeometry(rect());
            gpuSurface_->setAntiAliasingSamples(
                blenderGridRenderer_.antiAliasingSamples());
            gpuSurface_->setDrawCallback(
                [this](QPainter &painter, BlenderGridRenderer &gridRenderer,
                       ViewportSceneRenderer &sceneRenderer,
                       ViewportSceneRenderer &previewRenderer,
                       ViewportControlPointRenderer &controlPointRenderer,
                       ViewportSurfaceRenderer &surfaceRenderer) {
                    paintViewport(painter, &gridRenderer, &sceneRenderer,
                                  &previewRenderer, &controlPointRenderer,
                                  &surfaceRenderer);
                });
            gpuSurface_->show();
        }
        DebugLog::instance().write(QStringLiteral("viewport constructed"));
    }

    ~ViewportWidget() override
    {
        qApp->removeEventFilter(this);
    }

    // Navigation updates the GPU surface directly, so its frame uses the
    // camera state from this event instead of waiting for a parent repaint.
    void update()
    {
        if (gpuSurface_ != nullptr) {
            gpuSurface_->update();
        } else {
            QWidget::update();
        }
    }

    void setTool(ToolId tool)
    {
        DebugLog::instance().write(QStringLiteral("setTool requested=%1 previous=%2")
                                       .arg(toolName(tool), toolName(activeTool_)));
        if (duplicateTool_.isActive()) {
            cancelDuplicate();
        }
        if (grabTool_.isActive() && tool != Tool::Select) {
            cancelGrab();
        }
        const Tool previousTool = activeTool_;
        if (previousTool == Tool::Picture && tool != Tool::Picture) {
            pendingPictureImage_ = QImage();
            pendingPictureImageData_.clear();
            pendingPicturePath_.clear();
        }
        if (activeToolController_ != nullptr && activeToolController_->id() != tool) {
            activeToolController_->cancel(toolContext_);
        }
        if (subdivisionTool_.isActive() && tool != Tool::Select) {
            cancelSubdivisionPreview();
        }
        if (joinTool_.isActive() && tool != Tool::Select) {
            cancelJoinMode();
        }
        if (tool != Tool::Rotate) {
            resetRotateInteraction();
        }
        if (tool != Tool::Scale) {
            resetScaleInteraction();
        }
        if (tool != Tool::Mirror) {
            resetMirrorInteraction();
        }
        activeTool_ = tool;
        activeToolController_ = toolRegistry_.find(tool);
        pendingPoints_.clear();
        toolPreview_ = ToolPreview{};
        toolDrawingFrame_ = {};
        toolDrawingPlaneLocked_ = false;
        resetArcPreviewTracking();
        resetArcInputState();
        lineCommandActive_ = tool == Tool::Line;
        if (!isEraseLikeTool(tool) || previousTool != tool) {
            eraseTool_.resetInteraction();
            trimTool_.clearCandidates();
            eraseTool_.clearCandidates();
            eraseTargetShapeIndices_.clear();
            eraseSceneCurveCaches_.clear();
            eraseTargetCurveCaches_.clear();
            eraseGeometryCachePrepared_ = false;
            trimTool_.resetInteraction();
        }

        if (tool != Tool::Select) {
            if (!isEraseLikeTool(tool) && tool != Tool::Rotate &&
                tool != Tool::Scale && tool != Tool::Mirror) {
                if (tool != Tool::PointCenter && tool != Tool::PointExtrude) {
                    selection_.clear();
                }
            }
            resetSelectionBoxState();
            clearSelectionDragState();
            selection_.clearActiveControlPoint();
            dragHistoryRecorded_ = false;
            currentDragSnap_ = DragSnapResult{};
            dragSnapLocked_ = false;
            dragAxisLock_ = DragAxisLock::None;
        }

        if (lineCommandActive_) {
            const QPoint localCursor = mapFromGlobal(QCursor::pos());
            if (rect().contains(localCursor)) {
                rawCursorWorld_ = screenToWorld(localCursor);
                cursorValid_ = true;
            }
        }

        if (activeToolController_ != nullptr) {
            activeToolController_->begin(toolContext_);
        }

        refreshCursorConstraint();

        if (tool == Tool::Arc &&
            (arcState().mode == ArcMode::OnePoint ||
             arcState().mode == ArcMode::TwoPoint ||
             arcState().mode == ArcMode::ThreePoint) &&
            rect().contains(lastMousePosition_)) {
            updateDrawingWorkPlaneFromHover(lastMousePosition_);
        }
        if (usesHoveredFaceDrawingPlane(tool) &&
            rect().contains(lastMousePosition_)) {
            updateDrawingWorkPlaneFromHover(lastMousePosition_);
        }

        if (tool == Tool::Select) {
            setCursor(Qt::ArrowCursor);
        } else {
            setCursor(Qt::CrossCursor);
        }
        if (isCircleTangentTool(tool) || tool == Tool::Arc) {
            setFocus(Qt::OtherFocusReason);
        }

        DebugLog::instance().write(QStringLiteral("setTool applied=%1 lineCommandActive=%2 cursorValid=%3 cursorWorld=%4")
                                       .arg(toolName(activeTool_))
                                       .arg(lineCommandActive_)
                                       .arg(cursorValid_)
                                       .arg(pointText(cursorWorld_)));
        update();
    }

    bool beginPicturePlacement(const QString &imagePath,
                               QString *errorMessage) override
    {
        QFile imageFile(imagePath);
        if (!imageFile.open(QIODevice::ReadOnly)) {
            if (errorMessage != nullptr) {
                *errorMessage = imageFile.errorString();
            }
            return false;
        }
        QByteArray imageData = imageFile.readAll();
        if (imageData.isEmpty() || imageFile.error() != QFileDevice::NoError) {
            if (errorMessage != nullptr) {
                *errorMessage = imageFile.errorString().isEmpty()
                                    ? QStringLiteral("The selected image file is empty.")
                                    : imageFile.errorString();
            }
            return false;
        }
        QBuffer imageBuffer;
        imageBuffer.setData(imageData);
        imageBuffer.open(QIODevice::ReadOnly);
        QImageReader reader(&imageBuffer);
        reader.setAutoTransform(true);
        QImage image = reader.read();
        if (image.isNull()) {
            if (errorMessage != nullptr) {
                *errorMessage = reader.errorString().isEmpty()
                                    ? QStringLiteral("The selected file is not a readable image.")
                                    : reader.errorString();
            }
            return false;
        }

        pendingPictureImage_ = std::move(image);
        pendingPictureImageData_ = std::move(imageData);
        pendingPicturePath_ = imagePath;
        setTool(Tool::Picture);
        setFocus(Qt::OtherFocusReason);
        return true;
    }

    ViewportCommandResult executeCommand(ViewportCommand command,
                                         int argument = 0) override
    {
        return commandRouter_.execute(command, argument);
    }

    QVector<ViewportLayerInfo> layerInfos() const override
    {
        QVector<ViewportLayerInfo> infos;
        infos.reserve(document_.layers().size());
        for (const Layer &layer : document_.layers()) {
            ViewportLayerInfo info;
            info.id = layer.id;
            info.name = layer.name;
            info.visible = layer.visible;
            info.locked = layer.locked;
            info.active = layer.id == document_.activeLayerId();
            info.objectCount = static_cast<int>(layer.objectIds.size());
            info.color = layer.color;
            info.lineType = layer.lineType;
            info.lineWeightMm = layer.lineWeightMm;
            info.description = layer.description;
            info.frozen = layer.frozen;
            info.plotted = layer.plotted;
            infos.append(info);
        }
        return infos;
    }

    ViewportLayerCommandResult executeLayerCommand(
        const ViewportLayerCommandRequest &request) override
    {
        const LayerCommandResult result = LayerCommand::execute(
            document_, history_, request, selectedShapeIndices_);
        if (!result.accepted) {
            return {};
        }
        if (result.changed) {
            notifyHistoryChanged();
            notifyLayersChanged();
        }
        if (result.pruneSelection) {
            pruneSelectionToEditableLayers();
        }
        if (result.redraw) {
            update();
        }
        return result;
    }

    bool canUndo() const
    {
        return history_.canUndo();
    }

    bool canRedo() const
    {
        return history_.canRedo();
    }

    void undo()
    {
        if (!history_.canUndo()) {
            DebugLog::instance().write(QStringLiteral("undo ignored empty-history"));
            return;
        }

        history_.undo();
        viewportTransform_.setGridSpacing(
            documentGridSpacingInMillimeters(document_.settings()));
        viewportRenderer_.setGridBaseStep(
            documentGridSpacingInMillimeters(document_.settings()));
        resetInteractionAfterHistory();
        notifyHistoryChanged();
        notifyLayersChanged();
        DebugLog::instance().write(QStringLiteral("undo applied shapes=%1 undoRemaining=%2 redoAvailable=%3")
                                       .arg(shapes_.size())
                                       .arg(history_.undoCount())
                                       .arg(history_.redoCount()));
    }

    void redo()
    {
        if (!history_.canRedo()) {
            DebugLog::instance().write(QStringLiteral("redo ignored empty-history"));
            return;
        }

        history_.redo();
        viewportTransform_.setGridSpacing(
            documentGridSpacingInMillimeters(document_.settings()));
        viewportRenderer_.setGridBaseStep(
            documentGridSpacingInMillimeters(document_.settings()));
        resetInteractionAfterHistory();
        notifyHistoryChanged();
        notifyLayersChanged();
        DebugLog::instance().write(QStringLiteral("redo applied shapes=%1 undoAvailable=%2 redoRemaining=%3")
                                       .arg(shapes_.size())
                                       .arg(history_.undoCount())
                                       .arg(history_.redoCount()));
    }

    ToolId activeTool() const
    {
        return activeTool_;
    }

    void setArcMode(ArcMode mode)
    {
        arcTool_.setMode(mode);
        pendingPoints_.clear();
        resetArcPreviewTracking();
        resetArcInputState();
        currentSnap_ = SnapResult{};
        if (activeTool_ == Tool::Arc &&
            (mode == ArcMode::OnePoint || mode == ArcMode::TwoPoint ||
             mode == ArcMode::ThreePoint) &&
            rect().contains(lastMousePosition_)) {
            updateDrawingWorkPlaneFromHover(lastMousePosition_);
        }
        DebugLog::instance().write(QStringLiteral("setArcMode mode=%1")
                                       .arg(arcModeName(arcState().mode)));
        update();
    }

    ArcMode arcMode() const
    {
        return arcTool_.mode();
    }

    void setWorkPlane(WorkPlane plane, qreal offset = 0.0) override
    {
        navigationController_.stopAnimation();
        if (!std::isfinite(offset)) {
            return;
        }
        const bool planeChanged = viewportTransform_.workPlane() != plane;
        const bool offsetChanged = !workPlaneMatches(viewportTransform_.workPlane(),
                                                     viewportTransform_.workPlaneOffset(),
                                                     plane,
                                                     offset);
        const bool frameChanged = !workPlaneMatches(
            viewportTransform_.workPlaneFrame(), makeWorkPlaneFrame(plane, offset));
        if (!planeChanged && !offsetChanged && !frameChanged) {
            return;
        }

        setTool(Tool::Select);
        selection_.clear();
        selection_.clearActiveControlPoint();
        currentSnap_ = SnapResult{};
        currentDragSnap_ = DragSnapResult{};
        viewportTransform_.setWorkPlane(plane, offset);
        if (planeChanged) {
            switch (plane) {
            case WorkPlane::XY:
                viewportTransform_.setViewPreset(ViewportViewPreset::Top);
                break;
            case WorkPlane::XZ:
                viewportTransform_.setViewPreset(ViewportViewPreset::Front);
                break;
            case WorkPlane::YZ:
                viewportTransform_.setViewPreset(ViewportViewPreset::Right);
                break;
            }
        }
        const QPoint localCursor = mapFromGlobal(QCursor::pos());
        if (rect().contains(localCursor)) {
            rawCursorWorld_ = screenToWorld(localCursor);
            cursorWorld_ = rawCursorWorld_;
        }
        DebugLog::instance().write(QStringLiteral("work plane changed plane=%1")
                                       .arg(workPlaneName(plane)));
        notifyViewStateChanged();
        update();
        emitCoordinateUpdate();
    }

    WorkPlane workPlane() const override
    {
        return viewportTransform_.workPlane();
    }

    qreal workPlaneOffset() const override
    {
        return viewportTransform_.workPlaneOffset();
    }

    void setViewPreset(ViewportViewPreset preset) override
    {
        navigationController_.animateCameraChange([this, preset]() {
            viewportTransform_.setViewPreset(preset);
        });
    }

    ViewportViewPreset viewPreset() const override
    {
        return viewportTransform_.viewPreset();
    }

    void setControlPointsVisible(bool visible)
    {
        controlPointsVisible_ = visible;
        if (!visible) {
            clearSelectionDragState();
            selection_.clearActiveControlPoint();
            dragHistoryRecorded_ = false;
            currentDragSnap_ = DragSnapResult{};
            dragSnapLocked_ = false;
        }
        DebugLog::instance().write(QStringLiteral("setControlPointsVisible=%1 selectedShape=%2")
                                       .arg(controlPointsVisible_)
                                       .arg(objectIndex(selectedShapeIndex_)));
        update();
    }

    bool controlPointsVisible() const override
    {
        return controlPointsVisible_;
    }

    void setSnapLabelsVisible(bool visible) override
    {
        viewportOverlay_.setSnapLabelsVisible(visible);
        DebugLog::instance().write(QStringLiteral("setSnapLabelsVisible=%1").arg(visible));
        update();
    }

    void setSmoothCurveDisplay(bool enabled) override
    {
        viewportRenderer_.setSmoothCurveDisplay(enabled);
        DebugLog::instance().write(QStringLiteral("setSmoothCurveDisplay=%1").arg(enabled));
        update();
    }

    RotateToolPreferences rotateToolPreferences() const override
    {
        return rotateToolPreferences_;
    }

    bool setRotateToolPreferences(
        const RotateToolPreferences &preferences) override
    {
        constexpr qreal supportedDegreeIncrements[]{1.0, 2.0, 3.0, 5.0, 10.0,
                                                      15.0, 22.5, 30.0, 45.0,
                                                      60.0, 90.0};
        constexpr qreal supportedRadianIncrements[]{1.0, 2.0, 3.0, 5.0, 10.0,
                                                      15.0, 22.5, 30.0, 45.0,
                                                      60.0, 90.0};
        const auto isSupportedIncrement = [](qreal value,
                                             const qreal *increments,
                                             std::size_t count) {
            return std::any_of(increments, increments + count,
                               [value](qreal increment) {
                                   return std::abs(value - increment) < 1.0e-9;
                               });
        };
        if (!isSupportedIncrement(preferences.angleSnapIncrementDegrees,
                                  std::begin(supportedDegreeIncrements),
                                  std::size(supportedDegreeIncrements)) ||
            !isSupportedIncrement(preferences.angleSnapIncrementRadiansDegrees,
                                  std::begin(supportedRadianIncrements),
                                  std::size(supportedRadianIncrements)) ||
            !std::isfinite(preferences.angleSnapStrengthDegrees) ||
            preferences.angleSnapStrengthDegrees < 0.1 ||
            preferences.angleSnapStrengthDegrees > 45.0) {
            return false;
        }
        rotateToolPreferences_ = preferences;
        rotateTool_.setAngleSnapEnabled(preferences.angleSnapEnabled);
        rotateTool_.setAngleSnapParameters(
            preferences.useRadians
                ? preferences.angleSnapIncrementRadiansDegrees
                : preferences.angleSnapIncrementDegrees,
            preferences.angleSnapStrengthDegrees);
        update();
        return true;
    }

    DocumentSettings documentSettings() const override
    {
        return document_.settings();
    }

    bool setDocumentSettings(const DocumentSettings &settings) override
    {
        if (!isValidDocumentSettings(settings)) {
            return false;
        }
        if (document_.settings() == settings) {
            return true;
        }
        DocumentTransaction transaction = session_.beginTransaction();
        if (!transaction.setSettings(settings) ||
            !session_.commitTransaction(transaction)) {
            return false;
        }
        const qreal gridSpacing = documentGridSpacingInMillimeters(settings);
        viewportTransform_.setGridSpacing(gridSpacing);
        viewportRenderer_.setGridBaseStep(gridSpacing);
        update();
        return true;
    }

    BlenderGridAppearance gridAppearance() const override
    {
        return gridAppearance_;
    }

    void setGridAppearance(const BlenderGridAppearance &appearance) override
    {
        if (!isValidBlenderGridAppearance(appearance)) {
            return;
        }
        gridAppearance_ = appearance;
        viewportRenderer_.setGridAppearance(appearance);
        update();
    }

    ViewportCameraPreferences cameraPreferences() const override
    {
        return viewportTransform_.cameraPreferences();
    }

    bool setCameraPreferences(
        const ViewportCameraPreferences &preferences) override
    {
        if (!viewportTransform_.setCameraPreferences(preferences)) {
            return false;
        }
        update();
        emitCoordinateUpdate();
        return true;
    }

    ViewportNavigationPreferences navigationPreferences() const override
    {
        return viewportTransform_.navigationPreferences();
    }

    void setNavigationPreferences(
        const ViewportNavigationPreferences &preferences) override
    {
        viewportTransform_.setNavigationPreferences(preferences);
        update();
    }

    int viewportAntiAliasingSamples() const override
    {
        return blenderGridRenderer_.antiAliasingSamples();
    }

    void setViewportAntiAliasingSamples(int samples) override
    {
        blenderGridRenderer_.setAntiAliasingSamples(samples);
        if (gpuSurface_ != nullptr) {
            gpuSurface_->setAntiAliasingSamples(samples);
        }
        update();
    }

    bool smoothWiresOverlay() const override { return smoothWiresOverlay_; }
    bool smoothWiresEditMode() const override { return smoothWiresEditMode_; }

    void setSmoothWirePreferences(bool overlay, bool editMode) override
    {
        if (smoothWiresOverlay_ == overlay && smoothWiresEditMode_ == editMode)
            return;
        smoothWiresOverlay_ = overlay;
        smoothWiresEditMode_ = editMode;
        update();
    }

    void setArchitecturalDimensionFont(bool enabled) override
    {
        viewportRenderer_.setArchitecturalDimensionFont(enabled);
        curveHitTester_.setArchitecturalDimensionFont(enabled);
        DebugLog::instance().write(
            QStringLiteral("setArchitecturalDimensionFont=%1").arg(enabled));
        update();
    }

    void setPanButton(Qt::MouseButton button)
    {
        if (button != Qt::MiddleButton && button != Qt::RightButton) {
            DebugLog::instance().write(QStringLiteral("setPanButton ignored invalid=%1")
                                           .arg(static_cast<int>(button)));
            return;
        }

        navigationController_.setPanButton(button);
        DebugLog::instance().write(QStringLiteral("setPanButton applied=%1")
                                       .arg(inputButtonName(button)));
        update();
    }

    Qt::MouseButton panButton() const
    {
        return navigationController_.panButton();
    }

    void setOrthoEnabled(bool enabled)
    {
        orthoEnabled_ = enabled;
        refreshCursorConstraint();

        if (activeTool_ == Tool::Arc && arcState().mode == ArcMode::OnePoint &&
            pendingPoints_.size() >= 2 && cursorValid_) {
            // Reconcile the live sweep immediately when Ortho changes while
            // the endpoint is being positioned. Otherwise the cursor marker
            // and the preview can briefly represent different angles.
            updateArcPreviewTracking(cursorWorld_);
        }

        DebugLog::instance().write(QStringLiteral("setOrthoEnabled=%1 cursor=%2")
                                       .arg(orthoEnabled_)
                                       .arg(pointText(cursorWorld_)));
        update();
    }

    bool orthoEnabled() const
    {
        return orthoEnabled_;
    }

    void setOsnapEnabled(bool enabled)
    {
        osnapEnabled_ = enabled;
        updateSnapEngineSettings();
        if (!enabled) {
            currentDragSnap_ = DragSnapResult{};
            dragSnapLocked_ = false;
        }
        refreshCursorConstraint();
        DebugLog::instance().write(QStringLiteral("setOsnapEnabled=%1 snap=%2")
                                       .arg(osnapEnabled_)
                                       .arg(snapTypeName(currentSnap_.type)));
        update();
    }

    bool osnapEnabled() const
    {
        return osnapEnabled_;
    }

    void setSnapModes(bool endpoint,
                      bool midpoint,
                      bool intersection,
                      bool center,
                      bool perpendicular,
                      bool tangent,
                      bool near,
                      bool controlPoint)
    {
        endpointSnapEnabled_ = endpoint;
        midpointSnapEnabled_ = midpoint;
        intersectionSnapEnabled_ = intersection;
        centerSnapEnabled_ = center;
        perpendicularSnapEnabled_ = perpendicular;
        tangentSnapEnabled_ = tangent;
        nearSnapEnabled_ = near;
        controlPointSnapEnabled_ = controlPoint;
        updateSnapEngineSettings();
        refreshCursorConstraint();
        DebugLog::instance().write(QStringLiteral("setSnapModes endpoint=%1 midpoint=%2 intersection=%3 center=%4 perpendicular=%5 tangent=%6 near=%7 controlPoint=%8 snap=%9")
                                       .arg(endpointSnapEnabled_)
                                       .arg(midpointSnapEnabled_)
                                       .arg(intersectionSnapEnabled_)
                                       .arg(centerSnapEnabled_)
                                       .arg(perpendicularSnapEnabled_)
                                       .arg(tangentSnapEnabled_)
                                       .arg(nearSnapEnabled_)
                                       .arg(controlPointSnapEnabled_)
                                       .arg(snapTypeName(currentSnap_.type)));
        update();
    }

    QString coordinateText() const
    {
        const QString firstAxis = viewportTransform_.workPlane() == WorkPlane::YZ
                                      ? QStringLiteral("Y")
                                      : QStringLiteral("X");
        const QString secondAxis = viewportTransform_.workPlane() == WorkPlane::XY
                                       ? QStringLiteral("Y")
                                       : QStringLiteral("Z");
        return QStringLiteral("%1 %2   %3 %4   Zoom %5%")
            .arg(firstAxis)
            .arg(lastWorldPosition_.x(), 0, 'f', 2)
            .arg(secondAxis)
            .arg(lastWorldPosition_.y(), 0, 'f', 2)
            .arg(zoom_ * 100.0, 0, 'f', 0);
    }

    bool beginSubdivisionWheelMode()
    {
        const int selectedIndex = objectIndex(selectedShapeIndex_);
        if (selectedIndex < 0 ||
            !isSubdividableCurveShape(shapes_[selectedIndex])) {
            DebugLog::instance().write(QStringLiteral("beginSubdivisionWheelMode ignored selectedShape=%1")
                                           .arg(selectedIndex));
            return false;
        }

        const int initialSections = std::clamp(
            static_cast<int>(shapes_[selectedIndex].subdivisionParameters.size()) + 1,
            2,
            maxSubdivisionSections);
        subdivisionTool_.begin(selectedShapeIndex_,
                               initialSections,
                               maxSubdivisionSections);
        subdivisionTool_.refreshPreview(shapes_[selectedIndex]);
        setFocus(Qt::OtherFocusReason);
        notifySubdivisionStatus();
        update();
        DebugLog::instance().write(QStringLiteral("beginSubdivisionWheelMode shape=%1 sections=%2")
                                       .arg(selectedIndex)
                                       .arg(subdivisionTool_.sections()));
        return true;
    }

    void cancelSubdivisionWheelMode()
    {
        cancelSubdivisionPreview();
    }

    bool applySubdivision(int sections)
    {
        if (sections < 2 || sections > maxSubdivisionSections) {
            DebugLog::instance().write(QStringLiteral("applySubdivision rejected sections=%1")
                                           .arg(sections));
            return false;
        }

        const ObjectId shapeId = subdivisionTool_.isActive()
                                     ? subdivisionTool_.targetObjectId()
                                     : selectedShapeIndex_;
        const int shapeIndex = objectIndex(shapeId);
        if (shapeIndex < 0 ||
            !isSubdividableCurveShape(shapes_[shapeIndex])) {
            DebugLog::instance().write(QStringLiteral("applySubdivision ignored selectedShape=%1")
                                           .arg(shapeIndex));
            return false;
        }

        const QVector<double> parameters = equalArcLengthSubdivisionParameters(
            shapes_[shapeIndex], sections);
        if (parameters.size() != sections - 1) {
            DebugLog::instance().write(QStringLiteral("applySubdivision failed shape=%1 sections=%2 generated=%3")
                                           .arg(shapeIndex)
                                           .arg(sections)
                                           .arg(parameters.size()));
            return false;
        }

        const SubdivisionCommitResult result = subdivisionTool_.commit(
            shapeId, sections, parameters, toolContext_);
        if (!result.committed) {
            return false;
        }

        notifySubdivisionStatus();
        update();
        DebugLog::instance().write(QStringLiteral("applySubdivision shape=%1 sections=%2 points=%3")
                                       .arg(shapeIndex)
                                       .arg(sections)
                                       .arg(parameters.size()));
        return true;
    }

    bool subdivisionActive() const
    {
        return subdivisionTool_.isActive();
    }

    QString subdivisionStatusText() const
    {
        return subdivisionTool_.prompt();
    }

    int beginPointExtrude()
    {
        QVector<ObjectId> selected = selectedShapeIndices_;
        if (selectedShapeIndex_.isValid() &&
            !selected.contains(selectedShapeIndex_)) {
            selected.append(selectedShapeIndex_);
        }
        int pointCount = 0;
        for (const ObjectId sourceObjectId : selected) {
            const Shape *sourceShape = document_.shape(sourceObjectId);
            if (sourceShape != nullptr &&
                sourceShape->geometryType == GeometryType::Point &&
                sourceShape->points.size() == 1 &&
                document_.isObjectVisible(sourceObjectId) &&
                document_.isObjectEditable(sourceObjectId)) {
                ++pointCount;
            } else if (sourceShape != nullptr &&
                       document_.isObjectVisible(sourceObjectId) &&
                       document_.isObjectEditable(sourceObjectId)) {
                WorkPlaneFrame faceFrame;
                if (sourceShape->geometryType == GeometryType::NurbsSurface &&
                    nurbsSolidBaseFrame(sourceShape->nurbsSurface, &faceFrame)) {
                    ++pointCount;
                    continue;
                }
                for (const auto &curve : curveSampler_.curvesForShape(*sourceShape)) {
                    if (validateNurbsCurve(curve)) ++pointCount;
                }
            }
        }
        if (pointCount == 0) {
            DebugLog::instance().write(
                QStringLiteral("beginPointExtrude ignored selectionCount=%1 noEditableSources")
                    .arg(selected.size()));
            return 0;
        }

        setTool(Tool::PointExtrude);
        setFocus(Qt::OtherFocusReason);
        setCursor(Qt::CrossCursor);
        update();
        DebugLog::instance().write(QStringLiteral("beginPointExtrude selectedPoints=%1")
                                       .arg(pointCount));
        return activeTool_ == Tool::PointExtrude ? pointCount : 0;
    }



    bool beginRotate()
    {
        QVector<ObjectId> selected = selectedShapeIndices_;
        if (joinTool_.isActive()) {
            selected = joinTool_.selectedObjectIds();
        }
        if (selectedShapeIndex_.isValid() && objectIndex(selectedShapeIndex_) >= 0 &&
            !selected.contains(selectedShapeIndex_)) {
            selected.append(selectedShapeIndex_);
        }

        QVector<ObjectId> validSelection;
        for (const ObjectId objectId : selected) {
            if (objectIndex(objectId) >= 0 &&
                document_.isObjectEditable(objectId) &&
                !validSelection.contains(objectId)) {
                validSelection.append(objectId);
            }
        }
        if (validSelection.isEmpty()) {
            DebugLog::instance().write(QStringLiteral("beginRotate ignored no selection"));
            return false;
        }

        setTool(Tool::Rotate);
        rotateTool_.setAngleSnapParameters(
            rotateSnapIncrementDegrees(),
            rotateToolPreferences_.angleSnapStrengthDegrees);
        rotateTool_.beginSelection(validSelection,
                                   rotateToolPreferences_.angleSnapEnabled);
        updateDrawingWorkPlaneFromHover(QPointF(lastMousePosition_));
        setFocus(Qt::OtherFocusReason);
        setCursor(Qt::CrossCursor);
        update();
        DebugLog::instance().write(QStringLiteral("beginRotate shapes=%1")
                                       .arg(rotateState().sourceObjectIds.size()));
        return true;
    }

    bool beginScale(ScaleMode mode)
    {
        QVector<ObjectId> selected = selectedShapeIndices_;
        if (selectedShapeIndex_.isValid() && !selected.contains(selectedShapeIndex_)) {
            selected.append(selectedShapeIndex_);
        }

        QVector<ObjectId> validSelection;
        for (const ObjectId objectId : selected) {
            if (document_.isObjectEditable(objectId) && !validSelection.contains(objectId)) {
                validSelection.append(objectId);
            }
        }
        if (validSelection.isEmpty()) {
            DebugLog::instance().write(QStringLiteral("beginScale ignored no editable selection"));
            return false;
        }

        setTool(Tool::Scale);
        scaleTool_.beginSelection(validSelection, mode);
        setFocus(Qt::OtherFocusReason);
        setCursor(Qt::CrossCursor);
        publishScalePrompt();
        update();
        DebugLog::instance().write(QStringLiteral("beginScale mode=%1 shapes=%2")
                                       .arg(scaleModeName(scaleState().mode))
                                       .arg(scaleState().sourceObjectIds.size()));
        return true;
    }

    bool beginMirror()
    {
        QVector<ObjectId> selected = selectedShapeIndices_;
        if (selectedShapeIndex_.isValid() && !selected.contains(selectedShapeIndex_)) {
            selected.append(selectedShapeIndex_);
        }

        QVector<ObjectId> validSelection;
        for (const ObjectId objectId : selected) {
            if (document_.isObjectEditable(objectId) && !validSelection.contains(objectId)) {
                validSelection.append(objectId);
            }
        }
        if (validSelection.isEmpty()) {
            DebugLog::instance().write(QStringLiteral("beginMirror ignored no editable selection"));
            return false;
        }

        setTool(Tool::Mirror);
        mirrorTool_.setSourceObjectIds(validSelection);
        pendingPoints_.clear();
        setFocus(Qt::OtherFocusReason);
        setCursor(Qt::CrossCursor);
        update();
        DebugLog::instance().write(QStringLiteral("beginMirror shapes=%1")
                                       .arg(mirrorTool_.sourceObjectIds().size()));
        return true;
    }

    bool grabObjectWorldBoundsCenter(ObjectId objectId,
                                     Point3D *center) const
    {
        if (center == nullptr) {
            return false;
        }
        const SceneObject *sceneObject = document_.object(objectId);
        if (sceneObject == nullptr) {
            return false;
        }

        const Shape &shape = sceneObject->geometry;
        const Point3D placement = sceneObject->placementTranslation;
        QVector<Point3D> worldPoints;
        const auto isFiniteWorldPoint = [](const Point3D &point) {
            return std::isfinite(point.x) && std::isfinite(point.y) &&
                   std::isfinite(point.z);
        };
        const auto appendWorldPoint = [&](const Point3D &point) {
            if (isFiniteWorldPoint(point)) {
                worldPoints.append(point);
            }
        };
        const auto appendLocalPoint = [&](const QPointF &point,
                                          const WorkPlaneFrame &frame) {
            Point3D world = workPlaneFramePointToWorld(point, frame);
            world.x += placement.x;
            world.y += placement.y;
            world.z += placement.z;
            appendWorldPoint(world);
        };
        const auto appendSurfacePoints = [&](const NurbsSurface3D &surface) {
            for (const Point3D &point : surface.controlPoints) {
                appendWorldPoint({point.x + placement.x,
                                  point.y + placement.y,
                                  point.z + placement.z});
            }
        };

        if (shape.geometryType == GeometryType::NurbsSurface) {
            appendSurfacePoints(shape.nurbsSurface);
        } else if (shape.geometryType == GeometryType::NurbsSolid) {
            appendSurfacePoints(shape.nurbsSolid.baseSurface);
            for (const Point3D &point : shape.nurbsSolid.baseSurface.controlPoints) {
                appendWorldPoint({point.x + shape.nurbsSolid.displacement.x +
                                      placement.x,
                                  point.y + shape.nurbsSolid.displacement.y +
                                      placement.y,
                                  point.z + shape.nurbsSolid.displacement.z +
                                      placement.z});
            }
        } else {
            const WorkPlaneFrame frame = shapeWorkPlaneFrame(shape);
            for (const QPointF &point : shape.points) {
                appendLocalPoint(point, frame);
            }
            for (const QPointF &point : shape.nurbs.controlPoints) {
                appendLocalPoint(point, frame);
            }
            for (int componentIndex = 0;
                 componentIndex < shape.components.size(); ++componentIndex) {
                const WorkPlaneFrame componentFrame =
                    shapeComponentWorkPlaneFrame(shape, componentIndex);
                for (const QPointF &point :
                     shape.components[componentIndex].controlPoints) {
                    appendLocalPoint(point, componentFrame);
                }
            }
        }

        if (worldPoints.isEmpty()) {
            WorkPlaneFrame frame = shapeWorkPlaneFrame(shape);
            frame.origin.x += placement.x;
            frame.origin.y += placement.y;
            frame.origin.z += placement.z;
            appendWorldPoint(frame.origin);
        }
        if (worldPoints.isEmpty()) {
            return false;
        }

        Point3D minimum = worldPoints.first();
        Point3D maximum = minimum;
        for (const Point3D &point : worldPoints) {
            minimum.x = std::min(minimum.x, point.x);
            minimum.y = std::min(minimum.y, point.y);
            minimum.z = std::min(minimum.z, point.z);
            maximum.x = std::max(maximum.x, point.x);
            maximum.y = std::max(maximum.y, point.y);
            maximum.z = std::max(maximum.z, point.z);
        }
        *center = {(minimum.x + maximum.x) * 0.5,
                   (minimum.y + maximum.y) * 0.5,
                   (minimum.z + maximum.z) * 0.5};
        return isFiniteWorldPoint(*center);
    }

    Point3D grabSelectionPivotWorld(const QVector<ObjectId> &objectIds) const
    {
        Point3D center{};
        int centerCount = 0;
        for (const ObjectId objectId : objectIds) {
            Point3D objectCenter;
            if (!grabObjectWorldBoundsCenter(objectId, &objectCenter)) {
                continue;
            }
            center.x += objectCenter.x;
            center.y += objectCenter.y;
            center.z += objectCenter.z;
            ++centerCount;
        }
        if (centerCount > 0) {
            const qreal inverseCount = 1.0 / centerCount;
            center.x *= inverseCount;
            center.y *= inverseCount;
            center.z *= inverseCount;
        }
        return center;
    }

    bool makeSelectionViewPlaneFrame(const QVector<ObjectId> &objectIds,
                                     const QPointF &screenPosition,
                                     WorkPlaneFrame *frame,
                                     Point3D *screenPointWorld) const
    {
        if (frame == nullptr || screenPointWorld == nullptr) {
            return false;
        }
        const Point3D pivot = grabSelectionPivotWorld(objectIds);
        const Point3D viewDirection = viewportTransform_.viewDirection();
        const Point3D viewUp = viewportTransform_.viewUp();
        const Point3D viewRight{
            viewUp.y * viewDirection.z - viewUp.z * viewDirection.y,
            viewUp.z * viewDirection.x - viewUp.x * viewDirection.z,
            viewUp.x * viewDirection.y - viewUp.y * viewDirection.x};
        *frame = makeWorkPlaneFrameFromNormal(pivot, viewDirection, viewRight);
        QPointF coordinates;
        if (!isValidWorkPlaneFrame(*frame) ||
            !viewportTransform_.screenToWorkPlaneUnclipped(
                screenPosition, size(), *frame, &coordinates)) {
            return false;
        }
        *screenPointWorld = workPlaneFramePointToWorld(coordinates, *frame);
        return std::isfinite(screenPointWorld->x) &&
               std::isfinite(screenPointWorld->y) &&
               std::isfinite(screenPointWorld->z);
    }

    void beginSelectionViewPlaneDrag(const QVector<ObjectId> &objectIds,
                                     const QPointF &screenPosition)
    {
        selectionDragViewPlaneAnchorValid_ = makeSelectionViewPlaneFrame(
            objectIds, screenPosition, &selectionDragViewPlaneFrame_,
            &selectionDragLastViewPlaneWorld_);
        selectionDragCurrentViewPlaneWorld_ = selectionDragLastViewPlaneWorld_;
        selectionDragLastScreenPosition_ = screenPosition;
        dragSnapViewPlaneAnchorValid_ = false;
        dragHistoryRecorded_ = false;
        currentDragSnap_ = DragSnapResult{};
        dragSnapLocked_ = false;
        nearDragFreeSourcePointValid_ = false;
        dragAxisLock_ = DragAxisLock::None;
    }

    bool updateSelectionViewPlaneDrag(const QPointF &screenPosition,
                                      Point3D *worldDelta)
    {
        if (worldDelta == nullptr || !selectionDragViewPlaneAnchorValid_) {
            return false;
        }
        QPointF coordinates;
        if (!viewportTransform_.screenToWorkPlaneUnclipped(
                screenPosition, size(), selectionDragViewPlaneFrame_,
                &coordinates)) {
            return false;
        }
        const Point3D current = workPlaneFramePointToWorld(
            coordinates, selectionDragViewPlaneFrame_);
        *worldDelta = {current.x - selectionDragLastViewPlaneWorld_.x,
                       current.y - selectionDragLastViewPlaneWorld_.y,
                       current.z - selectionDragLastViewPlaneWorld_.z};
        selectionDragLastViewPlaneWorld_ = current;
        selectionDragCurrentViewPlaneWorld_ = current;
        selectionDragLastScreenPosition_ = screenPosition;
        return std::isfinite(worldDelta->x) && std::isfinite(worldDelta->y) &&
               std::isfinite(worldDelta->z);
    }

    QPointF projectWorldDeltaToWorkPlane(const Point3D &worldDelta) const
    {
        const WorkPlaneFrame &frame = viewportTransform_.workPlaneFrame();
        return QPointF(worldDelta.x * frame.xAxis.x +
                           worldDelta.y * frame.xAxis.y +
                           worldDelta.z * frame.xAxis.z,
                       worldDelta.x * frame.yAxis.x +
                           worldDelta.y * frame.yAxis.y +
                           worldDelta.z * frame.yAxis.z);
    }

    void translateSelectionDrag(const QVector<ObjectId> &objectIds,
                                const QPointF &workPlaneDelta,
                                const Point3D &worldDelta,
                                bool useViewPlaneDelta)
    {
        if (useViewPlaneDelta) {
            translateShapesWorldDelta(objectIds, worldDelta);
        } else {
            translateShapes(objectIds, workPlaneDelta);
        }
    }

    void updateSelectionObjectDragWithoutWorkPlane(
        const QVector<ObjectId> &objectIds,
        const QPointF &screenPosition)
    {
        if (!objectSelectionDragActive() || objectIds.isEmpty()) {
            return;
        }
        if (!selectionDragStarted()) {
            constexpr qreal dragStartThresholdPixels = 4.0;
            const QPointF screenDelta =
                screenPosition - selectionDragStartScreenPosition();
            if (std::hypot(screenDelta.x(), screenDelta.y()) <
                dragStartThresholdPixels) {
                return;
            }
            setSelectionDragStarted(true);
        }

        Point3D mouseWorldDelta;
        if (!updateSelectionViewPlaneDrag(screenPosition, &mouseWorldDelta)) {
            return;
        }
        if (dragAxisLock_ != DragAxisLock::None) {
            if (dragAxisLock_ == DragAxisLock::Z) {
                updateWorldZAxisDrag(objectIds, screenPosition);
            } else {
                const WorkPlaneFrame &frame =
                    viewportTransform_.workPlaneFrame();
                const Point3D axis = dragAxisLock_ == DragAxisLock::X
                                         ? frame.xAxis
                                         : frame.yAxis;
                const qreal distance = mouseWorldDelta.x * axis.x +
                                       mouseWorldDelta.y * axis.y +
                                       mouseWorldDelta.z * axis.z;
                const Point3D axisDelta{axis.x * distance,
                                        axis.y * distance,
                                        axis.z * distance};
                if (!isZeroWorldDelta(axisDelta)) {
                    beginDragHistory();
                    translateShapesWorldDelta(objectIds, axisDelta);
                }
                currentDragSnap_ = DragSnapResult{};
                dragSnapLocked_ = false;
                dragSnapViewPlaneAnchorValid_ = false;
                setSelectionLastDragWorldPosition(rawCursorWorld_);
            }
            return;
        }

        Point3D moveDelta = mouseWorldDelta;
        if (dragSnapLocked_ && currentDragSnap_.type != SnapType::Near) {
            const qreal snapDistance = std::hypot(
                screenPosition.x() - selectionDragSnapScreen_.x(),
                screenPosition.y() - selectionDragSnapScreen_.y());
            if (snapDistance <= kDragSnapBreakawayPixels) {
                return;
            }
            moveDelta = {selectionDragCurrentViewPlaneWorld_.x -
                             dragSnapViewPlaneWorld_.x,
                         selectionDragCurrentViewPlaneWorld_.y -
                             dragSnapViewPlaneWorld_.y,
                         selectionDragCurrentViewPlaneWorld_.z -
                             dragSnapViewPlaneWorld_.z};
            dragSnapLocked_ = false;
            currentDragSnap_ = DragSnapResult{};
            dragSnapViewPlaneAnchorValid_ = false;
            nearDragFreeSourcePointValid_ = false;
        } else if (dragSnapLocked_ && currentDragSnap_.type == SnapType::Near) {
            // Near snaps are workplane rails. When the drawing workplane is
            // edge-on, release that rail and keep the pointer's view-plane
            // motion continuous.
            dragSnapLocked_ = false;
            currentDragSnap_ = DragSnapResult{};
            dragSnapViewPlaneAnchorValid_ = false;
            nearDragFreeSourcePointValid_ = false;
        }
        if (isZeroWorldDelta(moveDelta)) {
            return;
        }

        beginDragHistory();
        translateShapesWorldDelta(objectIds, moveDelta);
        currentDragSnap_ = objectIds.size() > 1
                               ? findDragSnap(objectIds)
                               : findDragSnap(objectIds.first());
        if (currentDragSnap_.isValid()) {
            applyObjectDragSnap(objectIds, currentDragSnap_);
            dragSnapLocked_ = true;
            dragSnapViewPlaneWorld_ = selectionDragCurrentViewPlaneWorld_;
            selectionDragSnapScreen_ = screenPosition;
            dragSnapViewPlaneAnchorValid_ = true;
            dragSnapCursorWorld_ = rawCursorWorld_;
        }
        setSelectionLastDragWorldPosition(rawCursorWorld_);
        update();
        emitCoordinateUpdate();
    }

    bool beginGrab()
    {
        if (!selectionShortcutsAvailable()) {
            return false;
        }

        QVector<ObjectId> selected = selectedShapeIndices_;
        if (selectedShapeIndex_.isValid() && !selected.contains(selectedShapeIndex_)) {
            selected.append(selectedShapeIndex_);
        }

        QVector<ObjectId> validSelection;
        for (const ObjectId objectId : selected) {
            if (document_.isObjectEditable(objectId) && !validSelection.contains(objectId)) {
                validSelection.append(objectId);
            }
        }
        if (validSelection.isEmpty()) {
            DebugLog::instance().write(QStringLiteral("beginGrab ignored no editable selection"));
            return false;
        }

        const QPoint localCursor = mapFromGlobal(QCursor::pos());
        if (rect().contains(localCursor)) {
            rawCursorWorld_ = screenToWorld(localCursor);
            cursorWorld_ = rawCursorWorld_;
            cursorValid_ = true;
        }
        if (!grabTool_.begin(document_, validSelection, rawCursorWorld_)) {
            return false;
        }
        grabAxisStartScreen_ = localCursor;
        grabViewPlaneAnchorValid_ = makeSelectionViewPlaneFrame(
            validSelection, localCursor, &grabViewPlaneFrame_,
            &grabViewPlaneStartWorld_);
        if (!selectedShapeIndex_.isValid() || !validSelection.contains(selectedShapeIndex_)) {
            selection_.setPrimaryObjectId(validSelection.back());
        }
        dragHistoryRecorded_ = false;
        currentDragSnap_ = DragSnapResult{};
        dragSnapLocked_ = false;
        dragAxisLock_ = DragAxisLock::None;
        currentSnap_ = SnapResult{};
        beginSelectionObjectDrag(validSelection,
                                 localCursor,
                                 rawCursorWorld_,
                                 true);
        setFocus(Qt::OtherFocusReason);
        setCursor(Qt::SizeAllCursor);
        update();
        DebugLog::instance().write(QStringLiteral("beginGrab shapes=%1")
                                       .arg(grabTool_.objectIds().size()));
        return true;
    }

    void beginGrabBasePointMode()
    {
        if (!grabTool_.isActive()) {
            return;
        }

        if (grabTool_.enterBasePointMode(document_)) {
            notifyLayersChanged();
        }
        currentSnap_ = SnapResult{};
        currentDragSnap_ = DragSnapResult{};
        dragSnapLocked_ = false;
        setCursor(Qt::CrossCursor);
        update();
        DebugLog::instance().write(QStringLiteral("grab base-point selection started"));
    }

    void finishGrab()
    {
        if (!grabTool_.isActive()) {
            return;
        }

        if (grabTool_.moved()) {
            recordGeometrySnapshot(grabTool_.startSnapshot());
        }
        const bool moved = grabTool_.moved();
        resetGrabInteraction();
        clearSelectionDragState();
        dragHistoryRecorded_ = false;
        currentDragSnap_ = DragSnapResult{};
        currentSnap_ = SnapResult{};
        dragSnapLocked_ = false;
        dragAxisLock_ = DragAxisLock::None;
        setCursor(Qt::ArrowCursor);
        update();
        DebugLog::instance().write(QStringLiteral("grab finished moved=%1").arg(moved));
    }

    void cancelGrab()
    {
        if (!grabTool_.isActive()) {
            return;
        }

        const bool moved = grabTool_.moved();
        if (moved) {
            document_.restoreSnapshot(grabTool_.startSnapshot());
            notifyLayersChanged();
        }
        resetGrabInteraction();
        clearSelectionDragState();
        dragHistoryRecorded_ = false;
        currentDragSnap_ = DragSnapResult{};
        currentSnap_ = SnapResult{};
        dragSnapLocked_ = false;
        dragAxisLock_ = DragAxisLock::None;
        setCursor(Qt::ArrowCursor);
        update();
        DebugLog::instance().write(QStringLiteral("grab canceled moved=%1").arg(moved));
    }

    bool beginDuplicate()
    {
        if (!selectionShortcutsAvailable()) {
            return false;
        }

        QVector<ObjectId> selected = selectedShapeIndices_;
        if (selectedShapeIndex_.isValid() && !selected.contains(selectedShapeIndex_)) {
            selected.append(selectedShapeIndex_);
        }

        if (!duplicateTool_.begin(document_, selected)) {
            return false;
        }
        currentSnap_ = SnapResult{};
        setFocus(Qt::OtherFocusReason);
        setCursor(Qt::CrossCursor);
        update();
        DebugLog::instance().write(
            QStringLiteral("beginDuplicate objects=%1")
                .arg(duplicateTool_.sourceObjects().size()));
        return true;
    }

    bool duplicateInPlace()
    {
        if (!beginDuplicate()) {
            return false;
        }

        duplicateTool_.beginInPlace();
        finishDuplicate();
        return true;
    }

    void updateDuplicatePreview(const QPointF &rawPoint)
    {
        if (!duplicateTool_.isActive() || !duplicateTool_.hasBasePoint()) {
            return;
        }

        const QPointF destinationCursor = rawPoint - duplicateTool_.cursorOffset();
        currentSnap_ = findDuplicateDestinationSnap(destinationCursor);
        duplicateTool_.updatePlacement(
            destinationCursor,
            currentSnap_,
            [this](Shape &shape, Point3D &placement, const QPointF &delta) {
                if (shape.geometryType == GeometryType::NurbsSurface ||
                    shape.geometryType == GeometryType::NurbsSolid) {
                    const WorkPlaneFrame frame = viewportTransform_.workPlaneFrame();
                    placement.x += frame.xAxis.x * delta.x() +
                                   frame.yAxis.x * delta.y();
                    placement.y += frame.xAxis.y * delta.x() +
                                   frame.yAxis.y * delta.y();
                    placement.z += frame.xAxis.z * delta.x() +
                                   frame.yAxis.z * delta.y();
                } else {
                    translateShapeGeometry(shape, delta);
                }
            });
        cursorWorld_ = duplicateTool_.destination();
        lastWorldPosition_ = duplicateTool_.destination();
        update();
    }

    void finishDuplicate()
    {
        if (!duplicateTool_.isActive() || !duplicateTool_.hasBasePoint() ||
            duplicateTool_.previewShapes().isEmpty()) {
            return;
        }

        const QVector<ObjectId> sourceObjectIds = duplicateTool_.sourceObjectIds();
        DuplicateCommandPlan plan;
        if (!buildDuplicateCommandPlan(document_,
                                      sourceObjectIds,
                                      duplicateTool_.previewShapes(),
                                      &plan,
                                      duplicateTool_.previewPlacementTranslations())) {
            return;
        }

        DocumentTransaction transaction = session_.beginTransaction();
        QVector<ObjectId> duplicateIds;
        if (!applyDuplicateCommand(document_, transaction, plan, &duplicateIds)) {
            transaction.rollback();
            return;
        }

        if (!session_.commitTransaction(transaction)) {
            return;
        }
        selection_.setObjectIds(duplicateIds,
                                duplicateIds.isEmpty() ? ObjectId::invalid()
                                                       : duplicateIds.back());
        const int duplicateCount = duplicateIds.size();
        const QPointF committedDelta = duplicateTool_.destination() -
                                       duplicateTool_.basePoint();
        resetDuplicateInteraction();
        currentSnap_ = SnapResult{};
        setCursor(Qt::ArrowCursor);
        notifyLayersChanged();
        update();
        emitCoordinateUpdate();
        if (commandFinished_) {
            commandFinished_(Tool::Select);
        }
        DebugLog::instance().write(
            QStringLiteral("duplicate committed objects=%1 delta=%2")
                .arg(duplicateCount)
                .arg(pointText(committedDelta)));
    }

    void cancelDuplicate()
    {
        if (!duplicateTool_.isActive()) {
            return;
        }
        resetDuplicateInteraction();
        currentSnap_ = SnapResult{};
        setCursor(activeTool_ == Tool::Select ? Qt::ArrowCursor : Qt::CrossCursor);
        update();
        DebugLog::instance().write(QStringLiteral("duplicate canceled"));
    }

    bool beginJoinMode()
    {
        if (subdivisionTool_.isActive()) {
            cancelSubdivisionPreview();
        }

        const QVector<ObjectId> preselectedShapes = selectedShapeIndices_;
        setTool(Tool::Select);
        joinTool_.begin(preselectedShapes);
        if (joinTool_.selectedObjectIds().isEmpty()) {
            selection_.clear();
        } else {
            selection_.setPrimaryObjectId(joinTool_.selectedObjectIds().back());
        }
        resetSelectionBoxState();
        clearSelectionDragState();
        selection_.clearActiveControlPoint();
        setFocus(Qt::OtherFocusReason);
        setCursor(Qt::CrossCursor);
        update();
        DebugLog::instance().write(
            QStringLiteral("beginJoinMode preselected=%1")
                .arg(joinTool_.selectedCount()));
        if (!joinTool_.selectedObjectIds().isEmpty()) {
            if (!applyJoin()) {
                cancelJoinMode(false);
            }
            return true;
        }

        notifyJoinStatus();
        return true;
    }

    void cancelJoinMode(bool notifyStatus = true)
    {
        if (!joinTool_.isActive()) {
            return;
        }

        const QVector<ObjectId> joinedSelection = joinTool_.selectedObjectIds();
        joinTool_.cancel();
        selection_.setObjectIds(joinedSelection,
                                joinedSelection.isEmpty()
                                    ? ObjectId::invalid()
                                    : joinedSelection.back());
        setCursor(Qt::ArrowCursor);
        if (notifyStatus) {
            notifyJoinStatus();
        }
        update();
        DebugLog::instance().write(QStringLiteral("cancelJoinMode"));
    }

    QString joinStatusText() const
    {
        return joinTool_.prompt();
    }

    bool applyJoin()
    {
        const JoinExecutionResult result = joinTool_.executeJoin(
            selectedJoinEndpointTolerance(), toolContext_);
        if (!result.committed) {
            switch (result.failure) {
            case JoinExecutionFailure::NeedTwoCurves:
                notifyJoinStatus(result.sourceObjectCount == 0
                                     ? QStringLiteral("Join needs at least two curves")
                                     : QStringLiteral("Join failed — select at least two curves"));
                break;
            case JoinExecutionFailure::CurvesUnavailable:
                notifyJoinStatus(QStringLiteral("Join failed — selected curves are unavailable"));
                break;
            case JoinExecutionFailure::InvalidCurveSelection:
                notifyJoinStatus(QStringLiteral("Join failed — select lines or curves only"));
                break;
            case JoinExecutionFailure::DisconnectedCurves:
                notifyJoinStatus(QStringLiteral("Join failed — selected curves are not connected"));
                if (result.nearestEndpointGap >= 0.0) {
                    const qreal viewScale = viewportTransform_.viewScalePixelsPerWorldUnit(size());
                    DebugLog::instance().write(
                        QStringLiteral("applyJoin rejected disconnected components=%1 tolerance=%2 nearestEndpointGap=%3 nearestGapPixels=%4")
                            .arg(result.componentCount)
                            .arg(selectedJoinEndpointTolerance(), 0, 'f', 6)
                            .arg(result.nearestEndpointGap, 0, 'f', 6)
                            .arg(result.nearestEndpointGap * viewScale, 0, 'f', 2));
                }
                break;
            case JoinExecutionFailure::CommandFailed:
            case JoinExecutionFailure::Inactive:
            case JoinExecutionFailure::None:
                break;
            }
            return false;
        }

        setCursor(Qt::ArrowCursor);
        notifyJoinStatus(QStringLiteral("Joined %1 curve segments into one spline")
                             .arg(result.componentCount));
        update();
        DebugLog::instance().write(QStringLiteral("applyJoin committed components=%1 shapes=%2 lineMerges=%3")
                                       .arg(result.componentCount)
                                       .arg(shapes_.size())
                                       .arg(result.lineMergeCount));
        return true;
    }

    int explodeSelectedShapes()
    {
        if (joinTool_.isActive()) {
            cancelJoinMode();
        }

        QVector<ObjectId> selected = selectedShapeIndices_;
        if (selectedShapeIndex_.isValid() &&
            objectIndex(selectedShapeIndex_) >= 0 &&
            !selected.contains(selectedShapeIndex_)) {
            selected.append(selectedShapeIndex_);
        }
        if (selected.isEmpty()) {
            DebugLog::instance().write(QStringLiteral("explode ignored no selection"));
            return 0;
        }

        ExplodeCommandPlan plan;
        if (!buildExplodeCommandPlan(document_, selected, &plan) ||
            plan.sourceObjectCount == 0) {
            DebugLog::instance().write(
                QStringLiteral("explode ignored no rectangle or PolyCurve selection"));
            return 0;
        }

        DocumentTransaction transaction = session_.beginTransaction();
        if (!applyExplodeCommand(transaction, plan)) {
            return 0;
        }
        if (!session_.commitTransaction(transaction)) {
            return 0;
        }
        QVector<ObjectId> explodedSelection;
        explodedSelection.reserve(plan.selectedObjectIndices.size());
        for (const int selectedIndex : plan.selectedObjectIndices) {
            const ObjectId objectId = document_.objectIdAt(selectedIndex);
            if (objectId.isValid()) {
                explodedSelection.append(objectId);
            }
        }
        selection_.setObjectIds(explodedSelection);
        clearSelectionDragState();
        selection_.clearActiveControlPoint();
        dragHistoryRecorded_ = false;
        currentDragSnap_ = DragSnapResult{};
        dragSnapLocked_ = false;
        update();
        DebugLog::instance().write(
            QStringLiteral("explode committed shapes=%1 components=%2")
                .arg(plan.sourceObjectCount)
                .arg(plan.outputComponentCount));
        return plan.outputComponentCount;
    }

    int fillSelectedClosedCurves()
    {
        QVector<ObjectId> selected = selectedShapeIndices_;
        if (selectedShapeIndex_.isValid() &&
            objectIndex(selectedShapeIndex_) >= 0 &&
            !selected.contains(selectedShapeIndex_)) {
            selected.append(selectedShapeIndex_);
        }

        FillCommandPlan plan;
        if (!buildFillCommandPlan(document_, selected, &plan) ||
            plan.candidates.isEmpty()) {
            if (toolStatusUpdate_) {
                toolStatusUpdate_(QStringLiteral(
                    "Fill: select a closed planar curve"));
            }
            return 0;
        }

        DocumentTransaction transaction = session_.beginTransaction();
        int filledCount = 0;
        if (!applyFillCommand(transaction, plan, &filledCount)) {
            return 0;
        }
        if (!session_.commitTransaction(transaction)) {
            return 0;
        }

        update();
        if (toolStatusUpdate_) {
            toolStatusUpdate_(QStringLiteral("Filled %1 closed curve%2")
                                  .arg(filledCount)
                                  .arg(filledCount == 1
                                           ? QString()
                                           : QStringLiteral("s")));
        }
        DebugLog::instance().write(
            QStringLiteral("fill committed surfaces=%1").arg(filledCount));
        return filledCount;
    }

    bool saveUpdateSession(const QString &path) const
    {
        UpdateSessionViewState view;
        view.zoom = zoom_;
        view.pan = pan_;
        view.camera = viewportTransform_.cameraState();
        view.workPlane = viewportTransform_.workPlane();
        view.workPlaneOffset = viewportTransform_.workPlaneOffset();
        view.workPlaneFrame = viewportTransform_.workPlaneFrame();
        view.cameraPreferences = viewportTransform_.cameraPreferences();
        view.selectedObjectIds = selectedShapeIndices_;
        view.primaryObjectId = selectedShapeIndex_;
        view.activeControlPoint = selection_.activeControlPoint();
        view.controlPointsVisible = controlPointsVisible_;
        view.activeTool = activeTool_;
        view.shading = viewportShadingSettings_;

        QString errorMessage;
        if (!SessionSerializer::write(path, document_, view, &errorMessage)) {
            DebugLog::instance().write(
                QStringLiteral("saveUpdateSession failed path=%1 error=%2")
                    .arg(path, errorMessage));
            return false;
        }

        DebugLog::instance().write(
            QStringLiteral("saveUpdateSession path=%1 shapes=%2 layers=%3 version=6 preset=%4 perspective=%5 zoom=%6 pan=%7 shading=%8")
                .arg(path)
                .arg(shapes_.size())
                .arg(document_.layers().size())
                .arg(static_cast<int>(viewportTransform_.viewPreset()))
                .arg(viewportTransform_.isPerspectiveEnabled())
                .arg(zoom_, 0, 'g', 17)
                .arg(precisePointText(pan_))
                .arg(viewportShadingSettings_.mode == ViewportShadingMode::Solid
                         ? QStringLiteral("Solid")
                         : QStringLiteral("Wireframe")));
        return true;
    }

    bool restoreUpdateSession(const QString &path)
    {
        RestoredUpdateSession restored;
        QString errorMessage;
        if (!SessionSerializer::read(path,
                                     viewportTransform_.cameraPreferences(),
                                     controlPointsVisible_,
                                     &restored,
                                     &errorMessage)) {
            DebugLog::instance().write(
                QStringLiteral("restoreUpdateSession failed path=%1 error=%2")
                    .arg(path, errorMessage));
            return false;
        }

        const int version = restored.version;
        prepareForDocumentReplacement();
        session_.replaceDocument(std::move(restored.document));
        normalizeDisconnectedPolyCurveObjects();
        resetForDocumentReplacement();

        pan_ = restored.view.pan;
        zoom_ = restored.view.zoom;
        if (version >= 4) {
            viewportTransform_.setGridSpacing(
                documentGridSpacingInMillimeters(document_.settings()));
            viewportTransform_.setCameraPreferences(restored.view.cameraPreferences);
            viewportTransform_.setWorkPlane(restored.view.workPlane,
                                             restored.view.workPlaneOffset);
            viewportTransform_.setWorkPlaneFrame(restored.view.workPlaneFrame);
            viewportTransform_.setCameraState(restored.view.camera);
            selection_.setObjectIds(restored.view.selectedObjectIds,
                                    restored.view.primaryObjectId);
            selection_.prune(document_);
            if (restored.view.activeControlPoint.isValid() &&
                selection_.contains(restored.view.activeControlPoint.objectId)) {
                selection_.setActiveControlPoint(
                    restored.view.activeControlPoint.objectId,
                    restored.view.activeControlPoint.index);
            }
            controlPointsVisible_ = restored.view.controlPointsVisible;
            if (version >= 5) {
                setTool(restored.view.activeTool);
            }
            if (version >= 6) {
                viewportShadingSettings_ = restored.view.shading;
                viewportRenderer_.setShadingSettings(viewportShadingSettings_);
                refreshShadingPopover();
            }
            notifyViewStateChanged();
        }
        update();
        emitCoordinateUpdate();
        notifyHistoryChanged();
        notifyLayersChanged();

        DebugLog::instance().write(
            QStringLiteral("restoreUpdateSession path=%1 shapes=%2 layers=%3 version=%4 preset=%5 perspective=%6 zoom=%7 pan=%8 shading=%9")
                .arg(path)
                .arg(shapes_.size())
                .arg(document_.layers().size())
                .arg(version)
                .arg(static_cast<int>(viewportTransform_.viewPreset()))
                .arg(viewportTransform_.isPerspectiveEnabled())
                .arg(zoom_, 0, 'g', 17)
                .arg(precisePointText(pan_))
                .arg(viewportShadingSettings_.mode == ViewportShadingMode::Solid
                         ? QStringLiteral("Solid")
                         : QStringLiteral("Wireframe")));
        return true;
    }

    ProjectViewportCameraSettings projectCameraSettings() const override
    {
        const ViewportCameraPreferences preferences = viewportTransform_.cameraPreferences();
        ProjectViewportCameraSettings cameraSettings;
        cameraSettings.focalLengthMillimeters = preferences.focalLengthMillimeters;
        cameraSettings.clipStart = preferences.clipStart;
        cameraSettings.clipEnd = preferences.clipEnd;
        const ViewportCameraState viewState = viewportTransform_.cameraState();
        const Point3D target = viewportTransform_.viewTarget();
        ProjectViewportViewState &storedView = cameraSettings.view;
        storedView.zoom = viewState.zoom;
        storedView.panX = viewState.pan.x();
        storedView.panY = viewState.pan.y();
        storedView.orbitPivotX = viewState.orbitPivot.x;
        storedView.orbitPivotY = viewState.orbitPivot.y;
        storedView.orbitPivotZ = viewState.orbitPivot.z;
        storedView.yawRadians = viewState.yawRadians;
        storedView.pitchRadians = viewState.pitchRadians;
        storedView.perspective = viewState.perspective;
        storedView.preset = static_cast<int>(viewState.preset);
        storedView.gridViewDistance = viewState.gridViewDistance;
        storedView.orientationW = viewState.orientation.w;
        storedView.orientationX = viewState.orientation.x;
        storedView.orientationY = viewState.orientation.y;
        storedView.orientationZ = viewState.orientation.z;
        storedView.hasOrientation = viewState.hasOrientation;
        storedView.targetX = target.x;
        storedView.targetY = target.y;
        storedView.targetZ = target.z;
        storedView.storedInProject = true;
        cameraSettings.storedInProject = true;
        return cameraSettings;
    }

    void prepareForDocumentReplacement() override
    {
        if (grabTool_.isActive()) {
            cancelGrab();
        }
        if (duplicateTool_.isActive()) {
            cancelDuplicate();
        }
        setTool(Tool::Select);
    }

    void applyProjectCameraSettings(
        const ProjectViewportCameraSettings &cameraSettings) override
    {
        normalizeDisconnectedPolyCurveObjects();
        resetForDocumentReplacement();
        viewportTransform_.setGridSpacing(
            documentGridSpacingInMillimeters(document_.settings()));
        viewportTransform_.resetView();
        if (cameraSettings.storedInProject) {
            const ViewportCameraPreferences preferences{
                cameraSettings.focalLengthMillimeters,
                cameraSettings.clipStart,
                cameraSettings.clipEnd};
            viewportTransform_.setCameraPreferences(preferences);
        }
        if (cameraSettings.view.storedInProject) {
            const ProjectViewportViewState &storedView = cameraSettings.view;
            ViewportCameraState viewState;
            viewState.zoom = storedView.zoom;
            viewState.pan = QPointF(storedView.panX, storedView.panY);
            viewState.orbitPivot = {storedView.orbitPivotX,
                                    storedView.orbitPivotY,
                                    storedView.orbitPivotZ};
            viewState.yawRadians = storedView.yawRadians;
            viewState.pitchRadians = storedView.pitchRadians;
            viewState.perspective = storedView.perspective;
            viewState.preset = static_cast<ViewportViewPreset>(storedView.preset);
            viewState.gridViewDistance = storedView.gridViewDistance;
            viewState.orientation = {storedView.orientationW,
                                     storedView.orientationX,
                                     storedView.orientationY,
                                     storedView.orientationZ};
            viewState.hasOrientation = storedView.hasOrientation;
            viewportTransform_.setCameraState(viewState);
        }
        notifyViewStateChanged();
        setCursor(Qt::ArrowCursor);
        update();
        emitCoordinateUpdate();
    }

    void refreshAfterProjectImport() override
    {
        normalizeDisconnectedPolyCurveObjects();
        update();
    }

    void refreshAfterNewDocument() override
    {
        resetForDocumentReplacement();
        viewportTransform_.setGridSpacing(
            documentGridSpacingInMillimeters(document_.settings()));
        viewportTransform_.resetView();
        notifyViewStateChanged();
        setCursor(Qt::ArrowCursor);
        update();
        emitCoordinateUpdate();
    }

    bool saveVignolaDocument(const QString &path,
                             QString *errorMessage) const override
    {
        const ProjectController controller(session_);
        return controller.save(path, projectCameraSettings(), errorMessage);
    }

    bool loadVignolaDocument(const QString &path,
                             QString *errorMessage) override
    {
        ProjectController controller(session_);
        ProjectViewportCameraSettings cameraSettings;
        prepareForDocumentReplacement();
        if (!controller.open(path, &cameraSettings, errorMessage)) {
            return false;
        }
        applyProjectCameraSettings(cameraSettings);
        return true;
    }

    bool importRhino3dmDocument(const QString &path,
                                Rhino3dmImportReport *report,
                                QString *errorMessage) override
    {
        ProjectController controller(session_);
        if (!controller.importRhino3dm(path, report, errorMessage)) {
            return false;
        }
        refreshAfterProjectImport();
        return true;
    }

    void createNewDocument() override
    {
        ProjectController controller(session_);
        prepareForDocumentReplacement();
        controller.createNewDocument();
        refreshAfterNewDocument();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        if (gpuSurface_ != nullptr) {
            return;
        }
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        paintViewport(painter, nullptr, nullptr, nullptr, nullptr, nullptr);
    }

    void resizeEvent(QResizeEvent *event) override
    {
        if (gpuSurface_ != nullptr) {
            gpuSurface_->setGeometry(rect());
        }
        QWidget::resizeEvent(event);
    }

    ViewportRenderFrame viewportRenderFrame()
    {
        ViewportRenderFrameInput input;
        input.selectedObjectIds = selectedShapeIndices_;
        input.primarySelectedObjectId = selectedShapeIndex_;
        input.highlightedObjectIds = joinTool_.selectedObjectIds();
        input.activeToolPreview = toolPreview_;

        if (activeTool_ == Tool::Scale && scaleState().previewValid) {
            input.transformPreview.kind = ViewportRenderTransformKind::Scale;
            input.transformPreview.objectIds = scaleState().sourceObjectIds;
            input.transformPreview.scaleBasePoint = scaleState().basePoint;
            input.transformPreview.scaleAxis = scaleState().previewAxis;
            input.transformPreview.scaleFactor = scaleState().previewFactor;
            input.transformPreview.scaleMode = scaleState().mode;
            input.transformPreview.scaleSurfaceFrame =
                viewportTransform_.workPlaneFrame();
        } else if (activeTool_ == Tool::Rotate && rotateState().stage == 2) {
            input.transformPreview.kind = ViewportRenderTransformKind::Rotate;
            input.transformPreview.objectIds = rotateState().sourceObjectIds;
            input.transformPreview.rotatePivot = rotateState().baseWorldPoint;
            input.transformPreview.rotateAxis = rotateState().frame.normal;
            input.transformPreview.rotateAngle = rotateState().previewAngle;
        }
        ViewportRenderFrame frame =
            buildViewportRenderFrame(document_, viewportTransform_, size(), input);
        viewportGeometryCache_.prepareFrame(&frame, &surfaceTessellationCache_);
        return frame;
    }

    void setupShadingPopover()
    {
        shadingPopover_ = new QFrame(this, Qt::Popup | Qt::FramelessWindowHint);
        shadingPopover_->setObjectName(QStringLiteral("ViewportShadingPopover"));
        shadingPopover_->setFixedWidth(246);
        shadingPopover_->setStyleSheet(QStringLiteral(
            "QFrame#ViewportShadingPopover { background: #252525; color: #dedede; "
            "border: 1px solid #111; }"
            "QLabel { color: #d0d0d0; border: 0; }"
            "QToolButton { color: #dedede; background: #3b3b3b; "
            "border: 1px solid #303030; border-radius: 2px; padding: 2px; }"
            "QToolButton:hover { background: #505050; }"
            "QToolButton:checked { background: #4b79a8; border-color: #3d6e9e; }"
            "QToolButton:disabled { color: #777; background: #303030; }"
            "QSlider::groove:horizontal { background: #555; height: 4px; }"
            "QSlider::handle:horizontal { background: #b8b8b8; width: 10px; "
            "margin: -4px 0; border-radius: 5px; }"));

        auto *layout = new QVBoxLayout(shadingPopover_);
        layout->setContentsMargins(0, 0, 0, 0);
        auto *scroll = new QScrollArea(shadingPopover_);
        scroll->setObjectName(QStringLiteral("ShadingPopoverScroll"));
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto *content = new QWidget(scroll);
        auto *contentLayout = new QVBoxLayout(content);
        contentLayout->setContentsMargins(7, 6, 7, 7);
        contentLayout->setSpacing(5);
        scroll->setWidget(content);
        layout->addWidget(scroll);
        layout = contentLayout;

        auto *heading = new QLabel(QString::fromUtf8("▾  Lighting"),
                                   shadingPopover_);
        heading->setObjectName(QStringLiteral("LightingHeading"));
        layout->addWidget(heading);

        auto *modeRow = new QWidget(shadingPopover_);
        auto *modeLayout = new QHBoxLayout(modeRow);
        modeLayout->setContentsMargins(0, 0, 0, 0);
        modeLayout->setSpacing(1);
        lightingModeGroup_ = new QButtonGroup(shadingPopover_);
        lightingModeGroup_->setExclusive(true);
        const QStringList modeNames{QStringLiteral("Studio"),
                                    QStringLiteral("MatCap"),
                                    QStringLiteral("Flat")};
        for (int index = 0; index < modeNames.size(); ++index) {
            auto *button = new QToolButton(modeRow);
            button->setObjectName(QStringLiteral("LightingMode%1")
                                      .arg(modeNames[index]));
            button->setText(modeNames[index]);
            button->setCheckable(true);
            button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            button->setFixedHeight(23);
            lightingModeButtons_[index] = button;
            lightingModeGroup_->addButton(button, index);
            modeLayout->addWidget(button);
        }
        connect(lightingModeButtons_[0], &QToolButton::clicked,
                this, [this] { setViewportLightingMode(ViewportLightingMode::Studio); });
        connect(lightingModeButtons_[1], &QToolButton::clicked,
                this, [this] { setViewportLightingMode(ViewportLightingMode::MatCap); });
        connect(lightingModeButtons_[2], &QToolButton::clicked,
                this, [this] { setViewportLightingMode(ViewportLightingMode::Flat); });
        layout->addWidget(modeRow);

        auto *previewFrame = new QFrame(shadingPopover_);
        previewFrame->setObjectName(QStringLiteral("StudioLightPreviewFrame"));
        previewFrame->setFixedHeight(72);
        previewFrame->setStyleSheet(QStringLiteral(
            "QFrame#StudioLightPreviewFrame { background: #202020; "
            "border: 1px solid #373737; border-radius: 3px; }"));
        auto *previewLayout = new QGridLayout(previewFrame);
        previewLayout->setContentsMargins(3, 3, 3, 3);
        previewLayout->setSpacing(0);
        studioLightPreviewButton_ = new QToolButton(previewFrame);
        studioLightPreviewButton_->setObjectName(QStringLiteral("StudioLightPreview"));
        studioLightPreviewButton_->setAutoRaise(true);
        studioLightPreviewButton_->setToolButtonStyle(Qt::ToolButtonIconOnly);
        studioLightPreviewButton_->setIconSize(QSize(64, 64));
        studioLightPreviewButton_->setSizePolicy(QSizePolicy::Expanding,
                                                 QSizePolicy::Expanding);
        studioLightPreviewButton_->setStyleSheet(
            QStringLiteral("QToolButton { background: transparent; border: 0; }"));
        previewLayout->addWidget(studioLightPreviewButton_, 0, 0);
        connect(studioLightPreviewButton_, &QToolButton::clicked,
                this, [this] { showLightingPresetMenu(); });
        layout->addWidget(previewFrame);

        auto *rotationRow = new QWidget(shadingPopover_);
        auto *rotationLayout = new QHBoxLayout(rotationRow);
        rotationLayout->setContentsMargins(0, 0, 0, 0);
        rotationLayout->setSpacing(5);
        worldSpaceLightingButton_ = new QToolButton(rotationRow);
        worldSpaceLightingButton_->setObjectName(
            QStringLiteral("WorldSpaceLighting"));
        worldSpaceLightingButton_->setCheckable(true);
        worldSpaceLightingButton_->setFixedSize(21, 21);
        worldSpaceLightingButton_->setToolTip(
            QStringLiteral("World Space Lighting"));
        worldSpaceLightingButton_->setIcon(makeWorldLightingIcon());
        worldSpaceLightingButton_->setIconSize(QSize(15, 15));
        rotationLayout->addWidget(worldSpaceLightingButton_);
        connect(worldSpaceLightingButton_, &QToolButton::toggled,
                this, [this](bool enabled) {
                    viewportShadingSettings_.worldSpaceLighting = enabled;
                    refreshShadingPopover();
                    update();
                });

        studioLightRotationSlider_ = new QSlider(Qt::Horizontal, rotationRow);
        studioLightRotationSlider_->setObjectName(
            QStringLiteral("StudioLightRotation"));
        studioLightRotationSlider_->setRange(0, 360);
        studioLightRotationSlider_->setSingleStep(1);
        studioLightRotationSlider_->setPageStep(15);
        studioLightRotationSlider_->setToolTip(
            QStringLiteral("Studio Light Rotation"));
        rotationLayout->addWidget(studioLightRotationSlider_, 1);
        connect(studioLightRotationSlider_, &QSlider::valueChanged,
                this, [this](int degrees) {
                    viewportShadingSettings_.studioLightRotationDegrees = degrees;
                    if (studioLightRotationLabel_ != nullptr) {
                        studioLightRotationLabel_->setText(
                            QStringLiteral("%1°").arg(degrees));
                    }
                    updateStudioLightPreview();
                    update();
                });

        studioLightRotationLabel_ = new QLabel(rotationRow);
        studioLightRotationLabel_->setObjectName(
            QStringLiteral("StudioLightRotationValue"));
        studioLightRotationLabel_->setMinimumWidth(32);
        studioLightRotationLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        rotationLayout->addWidget(studioLightRotationLabel_);
        layout->addWidget(rotationRow);

        const auto addHeading = [layout, this](const QString &title,
                                               const QString &objectName) {
            auto *label = new QLabel(QStringLiteral("▾  %1").arg(title),
                                     shadingPopover_);
            label->setObjectName(objectName);
            layout->addWidget(label);
            return label;
        };
        const auto addSegmentRow = [layout, this](
                                       const QString &labelText,
                                       const QStringList &names,
                                       QButtonGroup **groupOut,
                                       const QString &prefix) {
            auto *row = new QWidget(shadingPopover_);
            auto *rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(0, 0, 0, 0);
            rowLayout->setSpacing(1);
            auto *label = new QLabel(labelText, row);
            label->setMinimumWidth(54);
            if (labelText.isEmpty()) {
                label->setVisible(false);
            }
            rowLayout->addWidget(label);
            auto *group = new QButtonGroup(row);
            group->setExclusive(true);
            QVector<QToolButton *> buttons;
            buttons.reserve(names.size());
            for (int index = 0; index < names.size(); ++index) {
                auto *button = new QToolButton(row);
                button->setObjectName(QStringLiteral("%1%2")
                                          .arg(prefix, names[index]));
                button->setText(names[index]);
                button->setCheckable(true);
                button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
                button->setFixedHeight(22);
                group->addButton(button, index);
                buttons.append(button);
                rowLayout->addWidget(button, 1);
            }
            layout->addWidget(row);
            if (groupOut != nullptr) {
                *groupOut = group;
            }
            return buttons;
        };

        addHeading(QStringLiteral("Color"), QStringLiteral("ColorHeading"));
        auto *wireLabel = new QLabel(QStringLiteral("Wireframe"), shadingPopover_);
        layout->addWidget(wireLabel);
        wireColorButtons_ = addSegmentRow(
            QString(), {QStringLiteral("Theme"), QStringLiteral("Object"),
                        QStringLiteral("Random")},
            &wireColorGroup_, QStringLiteral("WireColor"));
        wireColorButtons_[0]->setChecked(true);
        for (int index = 0; index < wireColorButtons_.size(); ++index) {
            connect(wireColorButtons_[index], &QToolButton::clicked, this,
                    [this, index] {
                        viewportShadingSettings_.wireColorMode =
                            static_cast<ViewportWireColorMode>(index);
                        update();
                    });
        }

        auto *objectLabel = new QLabel(QStringLiteral("Object"), shadingPopover_);
        layout->addWidget(objectLabel);
        solidColorButtons_ = addSegmentRow(
            QString(), {QStringLiteral("Material"), QStringLiteral("Object"),
                        QStringLiteral("Random")},
            &solidColorGroup_, QStringLiteral("SolidColor"));
        auto *attributeRow = new QWidget(shadingPopover_);
        auto *attributeLayout = new QHBoxLayout(attributeRow);
        attributeLayout->setContentsMargins(55, 0, 0, 0);
        attributeLayout->setSpacing(1);
        for (const QString &name : {QStringLiteral("Attribute"),
                                    QStringLiteral("Texture"),
                                    QStringLiteral("Custom")}) {
            auto *button = new QToolButton(attributeRow);
            button->setObjectName(QStringLiteral("SolidColor%1").arg(name));
            button->setText(name);
            button->setCheckable(true);
            button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            button->setFixedHeight(22);
            solidColorButtons_.append(button);
            attributeLayout->addWidget(button, 1);
        }
        layout->addWidget(attributeRow);
        solidColorGroup_->addButton(solidColorButtons_[3], 3);
        solidColorGroup_->addButton(solidColorButtons_[4], 4);
        solidColorGroup_->addButton(solidColorButtons_[5], 5);
        for (int index = 0; index < solidColorButtons_.size(); ++index) {
            connect(solidColorButtons_[index], &QToolButton::clicked, this,
                    [this, index] {
                        viewportShadingSettings_.colorMode =
                            static_cast<ViewportColorMode>(index);
                        if (index == static_cast<int>(ViewportColorMode::Custom)) {
                            const QColor chosen = QColorDialog::getColor(
                                viewportShadingSettings_.customColor,
                                this, QStringLiteral("Solid Viewport Color"),
                                QColorDialog::ShowAlphaChannel);
                            if (chosen.isValid()) {
                                viewportShadingSettings_.customColor = chosen;
                            }
                        }
                        update();
                    });
        }

        auto *backgroundLabel = new QLabel(QStringLiteral("Background"),
                                           shadingPopover_);
        layout->addWidget(backgroundLabel);
        backgroundButtons_ = addSegmentRow(
            QString(), {QStringLiteral("Theme"), QStringLiteral("World"),
                        QStringLiteral("Custom")},
            &backgroundGroup_, QStringLiteral("Background"));
        for (int index = 0; index < backgroundButtons_.size(); ++index) {
            connect(backgroundButtons_[index], &QToolButton::clicked, this,
                    [this, index] {
                        viewportShadingSettings_.backgroundMode =
                            static_cast<ViewportBackgroundMode>(index);
                        if (index == static_cast<int>(
                                         ViewportBackgroundMode::Custom)) {
                            const QColor chosen = QColorDialog::getColor(
                                viewportShadingSettings_.customBackgroundColor,
                                this, QStringLiteral("Viewport Background"));
                            if (chosen.isValid()) {
                                viewportShadingSettings_.customBackgroundColor =
                                    chosen;
                            }
                        }
                        update();
                    });
        }

        addHeading(QStringLiteral("Options"),
                   QStringLiteral("OptionsHeading"));
        const auto addCheckBox = [layout, this](const QString &title,
                                                const QString &objectName,
                                                bool ViewportShadingSettings::*field) {
            auto *box = new QCheckBox(title, shadingPopover_);
            box->setObjectName(objectName);
            layout->addWidget(box);
            connect(box, &QCheckBox::toggled, this, [this, field](bool enabled) {
                viewportShadingSettings_.*field = enabled;
                refreshShadingPopover();
                update();
            });
            return box;
        };
        backfaceCullingCheck_ = addCheckBox(
            QStringLiteral("Backface Culling"),
            QStringLiteral("BackfaceCulling"),
            &ViewportShadingSettings::backfaceCulling);

        auto *outlineRow = new QWidget(shadingPopover_);
        auto *outlineLayout = new QHBoxLayout(outlineRow);
        outlineLayout->setContentsMargins(0, 0, 0, 0);
        outline_ = new QCheckBox(QStringLiteral("Outline"), outlineRow);
        outline_->setObjectName(QStringLiteral("ObjectOutline"));
        outlineLayout->addWidget(outline_);
        outlineLayout->addStretch(1);
        outlineColorButton_ = new QToolButton(outlineRow);
        outlineColorButton_->setObjectName(QStringLiteral("OutlineColor"));
        outlineColorButton_->setFixedSize(76, 20);
        outlineLayout->addWidget(outlineColorButton_);
        layout->addWidget(outlineRow);
        connect(outline_, &QCheckBox::toggled, this, [this](bool enabled) {
            viewportShadingSettings_.outline = enabled;
            refreshShadingPopover();
            update();
        });
        connect(outlineColorButton_, &QToolButton::clicked, this, [this] {
            const QColor chosen = QColorDialog::getColor(
                viewportShadingSettings_.outlineColor, this,
                QStringLiteral("Object Outline Color"));
            if (chosen.isValid()) {
                viewportShadingSettings_.outlineColor = chosen;
                refreshShadingPopover();
                update();
            }
        });

        specularLightingCheck_ = addCheckBox(
            QStringLiteral("Specular Lighting"),
            QStringLiteral("SpecularLighting"),
            &ViewportShadingSettings::specularLighting);
        auto *xrayRow = new QWidget(shadingPopover_);
        auto *xrayLayout = new QHBoxLayout(xrayRow);
        xrayLayout->setContentsMargins(0, 0, 0, 0);
        xrayCheck_ = new QCheckBox(QStringLiteral("X-Ray"), xrayRow);
        xrayCheck_->setObjectName(QStringLiteral("SolidXRay"));
        xrayLayout->addWidget(xrayCheck_);
        xrayAlphaSlider_ = new QSlider(Qt::Horizontal, xrayRow);
        xrayAlphaSlider_->setObjectName(QStringLiteral("XRayAlpha"));
        xrayAlphaSlider_->setRange(0, 1000);
        xrayAlphaSlider_->setValue(500);
        xrayLayout->addWidget(xrayAlphaSlider_, 1);
        xrayAlphaLabel_ = new QLabel(QStringLiteral("0.500"), xrayRow);
        xrayAlphaLabel_->setMinimumWidth(32);
        xrayLayout->addWidget(xrayAlphaLabel_);
        layout->addWidget(xrayRow);
        connect(xrayCheck_, &QCheckBox::toggled, this, [this](bool enabled) {
            viewportShadingSettings_.xray = enabled;
            refreshShadingPopover();
            update();
        });
        connect(xrayAlphaSlider_, &QSlider::valueChanged, this, [this](int value) {
            viewportShadingSettings_.xrayAlpha = value / 1000.0;
            xrayAlphaLabel_->setText(QString::number(
                viewportShadingSettings_.xrayAlpha, 'f', 3));
            refreshShadingPopover();
            update();
        });

        auto *shadowRow = new QWidget(shadingPopover_);
        auto *shadowLayout = new QHBoxLayout(shadowRow);
        shadowLayout->setContentsMargins(0, 0, 0, 0);
        shadowsCheck_ = new QCheckBox(QStringLiteral("Shadow"), shadowRow);
        shadowsCheck_->setObjectName(QStringLiteral("WorkbenchShadows"));
        shadowLayout->addWidget(shadowsCheck_);
        shadowIntensitySlider_ = new QSlider(Qt::Horizontal, shadowRow);
        shadowIntensitySlider_->setObjectName(QStringLiteral("ShadowIntensity"));
        shadowIntensitySlider_->setRange(0, 1000);
        shadowIntensitySlider_->setValue(500);
        shadowLayout->addWidget(shadowIntensitySlider_, 1);
        shadowIntensityLabel_ = new QLabel(QStringLiteral("0.500"), shadowRow);
        shadowIntensityLabel_->setMinimumWidth(32);
        shadowLayout->addWidget(shadowIntensityLabel_);
        shadowSettingsButton_ = new QToolButton(shadowRow);
        shadowSettingsButton_->setObjectName(QStringLiteral("ShadowSettings"));
        shadowSettingsButton_->setText(QString::fromUtf8("⚙"));
        shadowSettingsButton_->setFixedSize(22, 22);
        shadowLayout->addWidget(shadowSettingsButton_);
        layout->addWidget(shadowRow);
        connect(shadowsCheck_, &QCheckBox::toggled, this, [this](bool enabled) {
            viewportShadingSettings_.shadows = enabled;
            refreshShadingPopover();
            update();
        });
        connect(shadowIntensitySlider_, &QSlider::valueChanged,
                this, [this](int value) {
                    viewportShadingSettings_.shadowIntensity = value / 1000.0;
                    shadowIntensityLabel_->setText(QString::number(
                        viewportShadingSettings_.shadowIntensity, 'f', 3));
                    update();
                });

        connect(shadowSettingsButton_, &QToolButton::clicked, this, [this] {
            QDialog dialog(this);
            dialog.setWindowTitle(QStringLiteral("Shadow Settings"));
            auto *form = new QFormLayout(&dialog);
            QWidget *directionRow = new QWidget(&dialog);
            auto *directionLayout = new QHBoxLayout(directionRow);
            directionLayout->setContentsMargins(0, 0, 0, 0);
            std::array<QDoubleSpinBox *, 3> directionFields{};
            const QStringList axes{QStringLiteral("X"), QStringLiteral("Y"),
                                   QStringLiteral("Z")};
            for (int axis = 0; axis < 3; ++axis) {
                directionFields[axis] = new QDoubleSpinBox(directionRow);
                directionFields[axis]->setRange(-1.0, 1.0);
                directionFields[axis]->setDecimals(3);
                directionFields[axis]->setPrefix(axes[axis] + QStringLiteral(" "));
                directionFields[axis]->setValue(
                    viewportShadingSettings_.shadowDirection[axis]);
                directionLayout->addWidget(directionFields[axis]);
            }
            form->addRow(QStringLiteral("Direction"), directionRow);
            auto *offset = new QDoubleSpinBox(&dialog);
            offset->setRange(0.0, 1.0);
            offset->setDecimals(3);
            offset->setSingleStep(0.01);
            offset->setValue(viewportShadingSettings_.shadowOffset);
            form->addRow(QStringLiteral("Offset"), offset);
            auto *focus = new QDoubleSpinBox(&dialog);
            focus->setRange(0.0, 1.0);
            focus->setDecimals(3);
            focus->setSingleStep(0.01);
            focus->setValue(viewportShadingSettings_.shadowFocus);
            form->addRow(QStringLiteral("Focus"), focus);
            auto *buttons = new QDialogButtonBox(
                QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
            form->addRow(buttons);
            connect(buttons, &QDialogButtonBox::accepted,
                    &dialog, &QDialog::accept);
            connect(buttons, &QDialogButtonBox::rejected,
                    &dialog, &QDialog::reject);
            if (dialog.exec() == QDialog::Accepted) {
                viewportShadingSettings_.shadowDirection = QVector3D(
                    directionFields[0]->value(), directionFields[1]->value(),
                    directionFields[2]->value());
                viewportShadingSettings_.shadowOffset = offset->value();
                viewportShadingSettings_.shadowFocus = focus->value();
                update();
            }
        });

        depthOfFieldCheck_ = addCheckBox(
            QStringLiteral("Depth of Field"),
            QStringLiteral("ViewportDepthOfField"),
            &ViewportShadingSettings::depthOfField);
        auto *cavityRow = new QWidget(shadingPopover_);
        auto *cavityLayout = new QHBoxLayout(cavityRow);
        cavityLayout->setContentsMargins(0, 0, 0, 0);
        cavityCheck_ = new QCheckBox(QStringLiteral("Cavity"), cavityRow);
        cavityCheck_->setObjectName(QStringLiteral("WorkbenchCavity"));
        cavityLayout->addWidget(cavityCheck_);
        auto *cavityTypeButton = new QToolButton(cavityRow);
        cavityTypeButton->setObjectName(QStringLiteral("CavityType"));
        cavityTypeButton->setText(QStringLiteral("Type: Screen"));
        cavityTypeButton->setPopupMode(QToolButton::InstantPopup);
        auto *cavityMenu = new QMenu(cavityTypeButton);
        const QStringList cavityNames{QStringLiteral("Screen"),
                                      QStringLiteral("World"),
                                      QStringLiteral("Both")};
        for (int index = 0; index < cavityNames.size(); ++index) {
            QAction *action = cavityMenu->addAction(cavityNames[index]);
            action->setCheckable(true);
            action->setData(index);
            action->setChecked(index == static_cast<int>(
                                           viewportShadingSettings_.cavityType));
            connect(action, &QAction::triggered, this, [this, action,
                                                        cavityTypeButton] {
                viewportShadingSettings_.cavityType =
                    static_cast<ViewportCavityType>(action->data().toInt());
                cavityTypeButton->setText(QStringLiteral("Type: %1")
                                              .arg(action->text()));
                update();
            });
        }
        cavityTypeButton->setMenu(cavityMenu);
        cavityLayout->addWidget(cavityTypeButton);
        layout->addWidget(cavityRow);
        connect(cavityCheck_, &QCheckBox::toggled, this, [this](bool enabled) {
            viewportShadingSettings_.cavity = enabled;
            refreshShadingPopover();
            update();
        });

        refreshShadingPopover();
        shadingPopover_->hide();
    }

    static QIcon makeWorldLightingIcon()
    {
        QPixmap pixmap(20, 20);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(QColor(210, 210, 210), 1.2));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(QRectF(2.5, 2.5, 15, 15));
        painter.drawEllipse(QRectF(7, 2.5, 6, 15));
        painter.drawLine(QPointF(3.2, 7.2), QPointF(16.8, 7.2));
        painter.drawLine(QPointF(3.2, 12.8), QPointF(16.8, 12.8));
        return QIcon(pixmap);
    }

    QPixmap lightingPreviewPixmap(int size,
                                  ViewportLightingMode mode,
                                  const QString &studioPreset,
                                  const QString &matcapPreset) const
    {
        QImage image(size, size, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        const float center = (size - 1) * 0.5f;
        const float radius = size * 0.405f;
        const QVector3D baseColor = workbenchDefaultSolidMaterialDiffuseColor();
        WorkbenchStudioLighting lighting =
            workbenchStudioLightingPreset(studioPreset);
        lighting = workbenchStudioLightingForView(
            lighting,
            viewportShadingSettings_.studioLightRotationDegrees,
            viewportShadingSettings_.worldSpaceLighting,
            QVector3D(1.0f, 0.0f, 0.0f),
            QVector3D(0.0f, 1.0f, 0.0f),
            QVector3D(0.0f, 0.0f, 1.0f));

        for (int y = 0; y < size; ++y) {
            QRgb *row = reinterpret_cast<QRgb *>(image.scanLine(y));
            for (int x = 0; x < size; ++x) {
                const float nx = (x - center) / radius;
                const float ny = (center - y) / radius;
                const float radiusSquared = nx * nx + ny * ny;
                if (radiusSquared > 1.0f) {
                    continue;
                }
                const QVector3D normal(nx, ny,
                                       std::sqrt(std::max(0.0f,
                                                          1.0f - radiusSquared)));
                QVector3D color;
                if (mode == ViewportLightingMode::Studio) {
                    color = workbenchStudioShade(baseColor,
                                                 normal,
                                                 QVector3D(0.0f, 0.0f, 1.0f),
                                                 161.0f / 255.0f,
                                                 0.0f,
                                                 lighting);
                } else if (mode == ViewportLightingMode::MatCap) {
                    color = workbenchMatcapShade(matcapPreset,
                                                 QVector3D(1.0f, 1.0f, 1.0f),
                                                 normal,
                                                 QVector3D(0.0f, 0.0f, 1.0f));
                } else {
                    color = baseColor;
                }
                const QVector3D srgb = workbenchSceneLinearToAgxSrgb(color);
                row[x] = QColor::fromRgbF(std::clamp(srgb.x(), 0.0f, 1.0f),
                                          std::clamp(srgb.y(), 0.0f, 1.0f),
                                          std::clamp(srgb.z(), 0.0f, 1.0f),
                                          1.0f).rgba();
            }
        }
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(QColor(25, 25, 25, 180), 0.7));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(QRectF(center - radius, center - radius,
                                   radius * 2.0f, radius * 2.0f));
        return QPixmap::fromImage(image);
    }

    void updateStudioLightPreview()
    {
        if (studioLightPreviewButton_ == nullptr) {
            return;
        }
        studioLightPreviewButton_->setIcon(QIcon(lightingPreviewPixmap(
            96, viewportShadingSettings_.lightingMode,
            viewportShadingSettings_.studioLightPreset,
            viewportShadingSettings_.matcapPreset)));
        studioLightPreviewButton_->setEnabled(
            viewportShadingSettings_.lightingMode != ViewportLightingMode::Flat);
    }

    void refreshShadingPopover()
    {
        if (shadingPopover_ == nullptr) {
            return;
        }
        const int selectedMode =
            viewportShadingSettings_.lightingMode == ViewportLightingMode::Studio
                ? 0
                : viewportShadingSettings_.lightingMode == ViewportLightingMode::MatCap
                      ? 1
                      : 2;
        for (int index = 0; index < 3; ++index) {
            const QSignalBlocker blocker(lightingModeButtons_[index]);
            lightingModeButtons_[index]->setChecked(index == selectedMode);
        }
        {
            const QSignalBlocker blocker(worldSpaceLightingButton_);
            worldSpaceLightingButton_->setChecked(
                viewportShadingSettings_.worldSpaceLighting);
        }
        {
            const QSignalBlocker blocker(studioLightRotationSlider_);
            studioLightRotationSlider_->setValue(
                viewportShadingSettings_.studioLightRotationDegrees);
        }
        studioLightRotationLabel_->setText(
            QStringLiteral("%1°")
                .arg(viewportShadingSettings_.studioLightRotationDegrees));
        studioLightRotationSlider_->setEnabled(
            viewportShadingSettings_.worldSpaceLighting &&
            viewportShadingSettings_.lightingMode == ViewportLightingMode::Studio);
        const auto setModeButton = [](const QVector<QToolButton *> &buttons,
                                      int current) {
            for (int index = 0; index < buttons.size(); ++index) {
                const QSignalBlocker blocker(buttons[index]);
                buttons[index]->setChecked(index == current);
            }
        };
        setModeButton(wireColorButtons_,
                      static_cast<int>(viewportShadingSettings_.wireColorMode));
        setModeButton(solidColorButtons_,
                      static_cast<int>(viewportShadingSettings_.colorMode));
        setModeButton(backgroundButtons_,
                      static_cast<int>(viewportShadingSettings_.backgroundMode));
        const auto setChecked = [](QCheckBox *box, bool checked) {
            if (box == nullptr) {
                return;
            }
            const QSignalBlocker blocker(box);
            box->setChecked(checked);
        };
        setChecked(backfaceCullingCheck_,
                   viewportShadingSettings_.backfaceCulling);
        setChecked(outline_, viewportShadingSettings_.outline);
        setChecked(specularLightingCheck_,
                   viewportShadingSettings_.specularLighting);
        setChecked(xrayCheck_, viewportShadingSettings_.xray);
        setChecked(shadowsCheck_, viewportShadingSettings_.shadows);
        setChecked(depthOfFieldCheck_,
                   viewportShadingSettings_.depthOfField);
        setChecked(cavityCheck_, viewportShadingSettings_.cavity);
        {
            const QSignalBlocker blocker(xrayAlphaSlider_);
            xrayAlphaSlider_->setValue(qRound(
                std::clamp<qreal>(viewportShadingSettings_.xrayAlpha, 0.0, 1.0) *
                1000.0));
        }
        xrayAlphaLabel_->setText(QString::number(
            viewportShadingSettings_.xrayAlpha, 'f', 3));
        xrayAlphaSlider_->setEnabled(viewportShadingSettings_.xray);
        {
            const QSignalBlocker blocker(shadowIntensitySlider_);
            shadowIntensitySlider_->setValue(qRound(
                std::clamp<qreal>(viewportShadingSettings_.shadowIntensity,
                                 0.0, 1.0) * 1000.0));
        }
        shadowIntensityLabel_->setText(QString::number(
            viewportShadingSettings_.shadowIntensity, 'f', 3));
        const bool xrayActive = viewportShadingSettings_.xray &&
                                viewportShadingSettings_.xrayAlpha != 1.0;
        shadowsCheck_->setEnabled(!xrayActive);
        shadowIntensitySlider_->setEnabled(viewportShadingSettings_.shadows &&
                                           !xrayActive);
        shadowSettingsButton_->setEnabled(!xrayActive);
        depthOfFieldCheck_->setEnabled(!xrayActive);
        cavityCheck_->setEnabled(!xrayActive);
        outlineColorButton_->setEnabled(viewportShadingSettings_.outline);
        outlineColorButton_->setStyleSheet(QStringLiteral(
            "QToolButton { background: %1; border: 1px solid #111; }")
            .arg(viewportShadingSettings_.outlineColor.name()));
        backfaceCullingCheck_->setEnabled(
            viewportShadingSettings_.mode == ViewportShadingMode::Solid);
        specularLightingCheck_->setEnabled(
            viewportShadingSettings_.mode == ViewportShadingMode::Solid &&
            viewportShadingSettings_.lightingMode != ViewportLightingMode::Flat);
        updateStudioLightPreview();
    }

    void setViewportLightingMode(ViewportLightingMode mode)
    {
        viewportShadingSettings_.lightingMode = mode;
        refreshShadingPopover();
        update();
    }

    void showShadingPopover()
    {
        if (shadingPopover_ == nullptr) {
            return;
        }
        refreshShadingPopover();
        shadingPopover_->adjustSize();
        shadingPopover_->setMaximumHeight(std::max(220, height() - 16));
        shadingPopover_->adjustSize();
        const QRectF button = viewportShadingSettingsButtonRect(size());
        const QPoint globalTopRight = mapToGlobal(button.topRight().toPoint());
        const QPoint globalBottomRight = mapToGlobal(button.bottomRight().toPoint());
        QPoint position(globalBottomRight.x() - shadingPopover_->width(),
                        globalTopRight.y() - shadingPopover_->height() - 5);
        if (position.y() < 4) {
            position.setY(globalBottomRight.y() + 5);
        }
        shadingPopover_->move(position);
        shadingPopover_->show();
        shadingPopover_->raise();
        update();
    }

    void showLightingPresetMenu()
    {
        if (viewportShadingSettings_.lightingMode == ViewportLightingMode::Flat) {
            return;
        }
        auto *menu = new QMenu(this);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        const bool studio = viewportShadingSettings_.lightingMode ==
                            ViewportLightingMode::Studio;
        const QStringList presets = studio
            ? QStringList{QStringLiteral("Default"), QStringLiteral("Basic"),
                          QStringLiteral("Outdoor"), QStringLiteral("Paint"),
                          QStringLiteral("Rim"), QStringLiteral("Studio")}
            : workbenchMatcapPresets();
        for (const QString &preset : presets) {
            const QString label = preset == QStringLiteral("Default")
                                      ? preset
                                      : QString(preset).replace(QLatin1Char('_'),
                                                                QLatin1Char(' '));
            QAction *action = menu->addAction(
                QIcon(lightingPreviewPixmap(32,
                                            studio ? ViewportLightingMode::Studio
                                                   : ViewportLightingMode::MatCap,
                                            studio ? preset : viewportShadingSettings_.studioLightPreset,
                                            studio ? viewportShadingSettings_.matcapPreset
                                                   : preset)),
                label);
            action->setData(preset);
            const QString activePreset = studio
                ? viewportShadingSettings_.studioLightPreset
                : viewportShadingSettings_.matcapPreset;
            action->setCheckable(true);
            action->setChecked(preset.compare(activePreset,
                                               Qt::CaseInsensitive) == 0);
            connect(action, &QAction::triggered, this, [this, action, studio] {
                const QString selected = action->data().toString();
                if (studio) {
                    viewportShadingSettings_.studioLightPreset = selected;
                } else {
                    viewportShadingSettings_.matcapPreset = selected;
                }
                refreshShadingPopover();
                update();
            });
        }
        const QPoint menuPosition = studioLightPreviewButton_->mapToGlobal(
            QPoint(0, studioLightPreviewButton_->height()));
        menu->popup(menuPosition);
    }

    void drawViewportShadingControls(QPainter &painter)
    {
        static const QPixmap xrayIcon(QStringLiteral(":/blender-shading/xray.png"));
        static const QPixmap wireframeIcon(
            QStringLiteral(":/blender-shading/shading_wire.png"));
        static const QPixmap solidIcon(
            QStringLiteral(":/blender-shading/shading_solid.png"));
        const QPixmap *icons[] = {&xrayIcon, &wireframeIcon, &solidIcon};
        const QRectF panel = viewportShadingPanelRect(size());
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(QColor(20, 21, 23, 245), 1.0));
        painter.setBrush(QColor(31, 32, 34, 248));
        painter.drawRoundedRect(panel, 3.0, 3.0);

        for (int index = 0; index < 3; ++index) {
            const QRectF button = viewportShadingButtonRect(size(), index);
            const bool active = index == 0
                                    ? viewportShadingSettings_.xrayEnabled()
                                    : index == 1
                                          ? viewportShadingSettings_.mode ==
                                                ViewportShadingMode::Wireframe
                                          : viewportShadingSettings_.mode ==
                                                ViewportShadingMode::Solid;
            const bool hovered = index == shadingControlHover_;
            const QColor buttonColor = active
                                           ? QColor(58, 126, 184, 255)
                                           : hovered
                                                 ? QColor(83, 85, 88, 255)
                                                 : QColor(60, 61, 63, 255);
            painter.setPen(QPen(active ? QColor(76, 142, 197, 255)
                                       : QColor(42, 43, 45, 255),
                                1.0));
            painter.setBrush(buttonColor);
            painter.drawRoundedRect(button, 2.0, 2.0);

            const QPointF center = button.center();
            const QRectF iconRect(center.x() - 8.0, center.y() - 8.0,
                                  16.0, 16.0);
            painter.drawPixmap(iconRect, *icons[index],
                               QRectF(icons[index]->rect()));
        }
        const QRectF settingsButton = viewportShadingSettingsButtonRect(size());
        const bool settingsHovered = shadingControlHover_ == 3;
        painter.setPen(QPen(settingsHovered ? QColor(76, 142, 197, 255)
                                            : QColor(42, 43, 45, 255),
                            1.0));
        painter.setBrush(shadingPopover_ != nullptr && shadingPopover_->isVisible()
                             ? QColor(58, 126, 184, 255)
                             : settingsHovered ? QColor(83, 85, 88, 255)
                                               : QColor(60, 61, 63, 255));
        painter.drawRoundedRect(settingsButton, 2.0, 2.0);
        painter.setPen(QColor(220, 220, 220));
        painter.drawText(settingsButton, Qt::AlignCenter,
                         QString::fromUtf8("⌄"));
        painter.restore();
    }

    void drawComponentModeControls(QPainter &painter)
    {
        const QRectF panel = componentModePanelRect(size());
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(QColor(20, 21, 23, 245), 1.0));
        painter.setBrush(QColor(31, 32, 34, 248));
        painter.drawRoundedRect(panel, 3.0, 3.0);
        for (int index = 0; index < 3; ++index) {
            const QRectF button = componentModeButtonRect(size(), index);
            const bool active = static_cast<int>(componentSelectionMode_) == index;
            const bool hovered = index == componentModeHover_;
            painter.setPen(QPen(active ? QColor(76, 142, 197, 255)
                                      : QColor(42, 43, 45, 255), 1.0));
            painter.setBrush(active ? QColor(58, 126, 184, 255)
                                    : hovered ? QColor(83, 85, 88, 255)
                                              : QColor(60, 61, 63, 255));
            painter.drawRoundedRect(button, 2.0, 2.0);
            const QRectF iconRect(button.center().x() - 8.0,
                                  button.center().y() - 8.0, 16.0, 16.0);
            const QPixmap &icon = blenderSelectionModeIcon(index, active);
            if (!icon.isNull()) painter.drawPixmap(iconRect, icon, icon.rect());
        }
        painter.restore();
    }

    int componentModeControlAt(const QPointF &position) const
    {
        for (int i = 0; i < 3; ++i) {
            if (componentModeButtonRect(size(), i).contains(position)) return i;
        }
        return -1;
    }

    void setComponentSelectionMode(ComponentSelectionMode mode)
    {
        if (componentSelectionMode_ == mode) return;
        componentSelectionMode_ = mode;
        controlPointsVisible_ = mode == ComponentSelectionMode::Vertex;
        update();
    }

    QSet<int> &activeComponentSelection()
    {
        return componentSelections_[static_cast<int>(componentSelectionMode_)];
    }

    int &activeComponentIndex()
    {
        return activeComponentIndices_[static_cast<int>(componentSelectionMode_)];
    }

    void clearComponentSelections()
    {
        for (QSet<int> &selection : componentSelections_) selection.clear();
        for (int &activeIndex : activeComponentIndices_) activeIndex = -1;
    }

    int viewportShadingControlAt(const QPointF &position) const
    {
        for (int index = 0; index < 3; ++index) {
            if (viewportShadingButtonRect(size(), index).contains(position)) {
                return index;
            }
        }
        if (viewportShadingSettingsButtonRect(size()).contains(position)) {
            return 3;
        }
        return -1;
    }

    void activateViewportShadingControl(int index)
    {
        if (index == 0) {
            viewportShadingSettings_.toggleXray();
        } else if (index == 1) {
            viewportShadingSettings_.mode = ViewportShadingMode::Wireframe;
        } else if (index == 2) {
            viewportShadingSettings_.mode = ViewportShadingMode::Solid;
        } else if (index == 3) {
            showShadingPopover();
            update();
            return;
        } else {
            return;
        }
        update();
    }

    void paintViewport(QPainter &painter,
                       BlenderGridRenderer *nativeRenderer,
                       ViewportSceneRenderer *sceneRenderer,
                       ViewportSceneRenderer *previewRenderer,
                       ViewportControlPointRenderer *controlPointRenderer,
                       ViewportSurfaceRenderer *surfaceRenderer)
    {
        invalidateEraseGeometryCacheForView();
        if (nativeRenderer != nullptr) {
            nativeRenderer->setSurfaceTessellationCache(
                &surfaceTessellationCache_);
        } else {
            blenderGridRenderer_.setSurfaceTessellationCache(
                &surfaceTessellationCache_);
        }
        updateAssociativeDimensions(document_, curveSampler_);
        const qreal baseGridStep = documentGridSpacingInMillimeters(document_.settings());
        viewportRenderer_.setGridBaseStep(baseGridStep);
        viewportRenderer_.setGridAppearance(gridAppearance_);
        viewportRenderer_.setShadingSettings(viewportShadingSettings_);
        const ViewportRenderFrame renderFrame = viewportRenderFrame();
        const QVector<ViewportRenderObject> &visibleShapes = renderFrame.objects;
        const ToolPreview &activeToolPreview = renderFrame.activeToolPreview;
        QVector<ViewportControlPointHandle> gpuControlPointHandles;
        if ((controlPointsVisible_ ||
             componentSelectionMode_ == ComponentSelectionMode::Vertex) &&
            controlPointRenderer != nullptr) {
            const QColor handleOutline(QStringLiteral("#77b7e6"));
            const QColor handleFill(QStringLiteral("#263b4b"));
            const QColor activeHandle(QStringLiteral("#f0a45a"));
            for (const int shapeIndex : controlPointShapeIndices()) {
                if (shapeIndex < 0 || shapeIndex >= shapes_.size()) {
                    continue;
                }

                const ObjectId objectId = shapes_.objectIdAt(shapeIndex);
                const ViewportRenderObject *renderObject = renderFrame.find(objectId);
                if (renderObject == nullptr) {
                    continue;
                }

                const QVector<Point3D> controlPoints =
                    controlPointWorldPositions(*renderObject);
                for (int pointIndex = 0; pointIndex < controlPoints.size();
                     ++pointIndex) {
                    const bool active = controlPointSelectionDragActive() &&
                                        objectId == selectedShapeIndex_ &&
                                        pointIndex == controlPointIndex_;
                    ViewportControlPointHandle handle;
                    handle.worldPosition = QVector3D(
                        static_cast<float>(controlPoints[pointIndex].x),
                        static_cast<float>(controlPoints[pointIndex].y),
                        static_cast<float>(controlPoints[pointIndex].z));
                    handle.fillColor = active ? activeHandle : handleFill;
                    handle.outlineColor = active ? activeHandle : handleOutline;
                    // The old QPainter square is 8 logical pixels with a
                    // 1.5-pixel outline straddling its edges.
                    handle.diameterPixels = 9.5f;
                    handle.outlineWidthPixels = 1.5f;
                    handle.shape = ViewportControlPointShape::Square;
                    gpuControlPointHandles.append(handle);
                }
            }
        }
        bool gpuControlPointsDrawn = false;
        QImage gpuViewportBackground;
        QImage committedSceneLayer;
        QPainter rasterScenePainter;
        if (nativeRenderer == nullptr) {
            gpuViewportBackground = blenderGridRenderer_.render(
                renderFrame.camera, renderFrame.viewportSize,
                devicePixelRatioF(), visibleShapes,
                baseGridStep, gridAppearance_);
            const qreal devicePixelRatio =
                std::max<qreal>(devicePixelRatioF(), 1.0);
            const QSize scenePixelSize(
                qRound(renderFrame.viewportSize.width() * devicePixelRatio),
                qRound(renderFrame.viewportSize.height() * devicePixelRatio));
            committedSceneLayer = QImage(scenePixelSize,
                                         QImage::Format_ARGB32_Premultiplied);
            committedSceneLayer.setDevicePixelRatio(devicePixelRatio);
            committedSceneLayer.fill(Qt::transparent);
            rasterScenePainter.begin(&committedSceneLayer);
            rasterScenePainter.setRenderHint(QPainter::Antialiasing, true);
        } else {
            painter.beginNativePainting();
            const bool backgroundDrawn =
                nativeRenderer->renderBackgroundToCurrentFramebuffer(
                    renderFrame.viewportSize, devicePixelRatioF(),
                    viewportShadingBackgroundColor(
                        viewportShadingSettings_));
            painter.endNativePainting();
            if (!backgroundDrawn) {
                fillViewportBackground(
                    painter, rect(),
                    viewportShadingBackgroundColor(viewportShadingSettings_));
            }
        }
        QPainter &scenePainter = nativeRenderer == nullptr
                                     ? rasterScenePainter
                                     : painter;
        QVector<ViewportSceneStroke> gpuStrokes;
        QVector<ViewportRenderObject> gpuVertexSelectedFaces;
        QVector<TransientPreviewStroke> gpuPreviewGeometry;
        QVector<TransientPreviewPicture> gpuPreviewPictures;
        gpuPreviewGeometry.reserve(duplicateTool_.previewShapes().size() +
                                   mirrorTool_.sourceObjectIds().size() + 16);
        gpuPreviewPictures.reserve(shapes_.size() +
                                   duplicateTool_.previewShapes().size() +
                                   mirrorTool_.sourceObjectIds().size() + 1);
        QVector<bool> gpuDuplicatePreviewHandled(
            duplicateTool_.previewShapes().size(), false);
        QVector<ObjectId> gpuMirrorPreviewHandled;
        bool gpuActiveToolPreview = false;
        bool gpuActivePicturePreviewRendered = false;
        for (const ViewportRenderObject &renderObject : visibleShapes) {
            const int index = renderObject.objectIndex;
            const ObjectId objectId = renderObject.objectId;
            const Shape &visibleShape = renderObject.shape;
            if (isDimensionGeometryType(visibleShape.geometryType)) {
                continue;
            }
            const QColor &layerColor = renderObject.layerColor;
            const QString &layerLineType = renderObject.layerLineType;
            const qreal layerLineWeightMm = renderObject.layerLineWeightMm;
            const bool selected = renderObject.selected;
            const bool scalePreview = renderObject.scalePreview;
            const bool rotatePreview = renderObject.rotatePreview;
            const GeometryType geometryType = visibleShape.geometryType;
            if (viewportShadingSettings_.mode == ViewportShadingMode::Solid &&
                (geometryType == GeometryType::NurbsSurface ||
                 geometryType == GeometryType::NurbsSolid)) {
                if (sceneRenderer != nullptr) {
                    auto cage = QSharedPointer<ViewportDepthGeometry>::create(
                        selectedSurfaceCage(visibleShape));
                    if (selected && componentSelectionObject_ == objectId &&
                        (componentSelectionMode_ == ComponentSelectionMode::Vertex ||
                         componentSelectionMode_ == ComponentSelectionMode::Edge) &&
                        !activeComponentSelection().isEmpty()) {
                        QVector<NurbsSurface3D> selectableFaces;
                        if (geometryType == GeometryType::NurbsSolid) {
                            const QVector<NurbsSurface3D> solidFaces =
                                shapeSurfaceFaces(visibleShape);
                            if (solidFaces.size() >= 2) {
                                selectableFaces.append(solidFaces[0]);
                                selectableFaces.append(solidFaces[1]);
                            }

                            // A swept boundary loop is stored as one exact
                            // NURBS wall, but its degree-one knot spans are
                            // separate planar edit faces, like Blender's
                            // individual sides of a cube.
                            WorkPlaneFrame baseFrame;
                            qreal u0 = 0.0;
                            qreal u1 = 0.0;
                            qreal v0 = 0.0;
                            qreal v1 = 0.0;
                            const NurbsSurface3D &base =
                                visibleShape.nurbsSolid.baseSurface;
                            if (nurbsSolidBaseFrame(base, &baseFrame) &&
                                nurbsSurfaceParameterDomains(
                                    base, &u0, &u1, &v0, &v1)) {
                                const Point3D uAxis{
                                    base.controlPoints[2].x - base.controlPoints[0].x,
                                    base.controlPoints[2].y - base.controlPoints[0].y,
                                    base.controlPoints[2].z - base.controlPoints[0].z};
                                const Point3D vAxis{
                                    base.controlPoints[1].x - base.controlPoints[0].x,
                                    base.controlPoints[1].y - base.controlPoints[0].y,
                                    base.controlPoints[1].z - base.controlPoints[0].z};
                                QVector<NurbsSurfaceTrimLoop> boundaryLoops =
                                    base.trimLoops;
                                if (boundaryLoops.isEmpty()) {
                                    NurbsSurfaceTrimLoop perimeter;
                                    perimeter.curve.degree = 1;
                                    perimeter.curve.order = 2;
                                    perimeter.curve.controlPoints = {
                                        {u0, v0}, {u1, v0}, {u1, v1},
                                        {u0, v1}, {u0, v0}};
                                    perimeter.curve.weights = {1, 1, 1, 1, 1};
                                    perimeter.curve.knots = {0, 1, 2, 3, 4};
                                    boundaryLoops.append(std::move(perimeter));
                                }
                                for (const NurbsSurfaceTrimLoop &loop :
                                     boundaryLoops) {
                                    const NurbsCurve2D &boundary = loop.curve;
                                    if (boundary.degree != 1 ||
                                        boundary.controlPoints.size() < 3) {
                                        continue;
                                    }
                                    for (int index = 0;
                                         index + 1 < boundary.controlPoints.size();
                                         ++index) {
                                        const auto toFramePoint =
                                            [&](const QPointF &uv) {
                                                const qreal a =
                                                    (uv.x() - u0) / (u1 - u0);
                                                const qreal b =
                                                    (uv.y() - v0) / (v1 - v0);
                                                const Point3D world{
                                                    baseFrame.origin.x + a * uAxis.x +
                                                        b * vAxis.x,
                                                    baseFrame.origin.y + a * uAxis.y +
                                                        b * vAxis.y,
                                                    baseFrame.origin.z + a * uAxis.z +
                                                        b * vAxis.z};
                                                return worldPointToWorkPlaneFrame(
                                                    world, baseFrame);
                                            };
                                        NurbsCurve2D segment;
                                        segment.degree = 1;
                                        segment.order = 2;
                                        segment.controlPoints = {
                                            toFramePoint(
                                                boundary.controlPoints[index]),
                                            toFramePoint(
                                                boundary.controlPoints[index + 1])};
                                        segment.weights = {1.0, 1.0};
                                        segment.knots = {0.0, 1.0};
                                        NurbsSurface3D panel;
                                        if (makeNurbsExtrusionSurface(
                                                segment, baseFrame,
                                                visibleShape.nurbsSolid.displacement,
                                                &panel)) {
                                            selectableFaces.append(std::move(panel));
                                        }
                                    }
                                }
                            }
                        } else {
                            selectableFaces = shapeSurfaceFaces(visibleShape);
                        }
                        for (const NurbsSurface3D &face : selectableFaces) {
                            Shape faceShape;
                            faceShape.geometryType = GeometryType::NurbsSurface;
                            faceShape.nurbsSurface = face;
                            const ViewportDepthGeometry faceCage =
                                selectedSurfaceCage(faceShape);
                            const bool vertexMode =
                                componentSelectionMode_ ==
                                ComponentSelectionMode::Vertex;
                            bool fullySelected = vertexMode
                                ? faceCage.pointVertices.size() >= 3
                                : faceCage.preciseLineVertices.size() >= 6;
                            if (vertexMode) {
                                for (const QVector3D &point : faceCage.pointVertices) {
                                    int vertex = -1;
                                    for (int index = 0;
                                         index < cage->pointVertices.size(); ++index) {
                                        if ((cage->pointVertices[index] - point)
                                                .lengthSquared() <= 1.0e-12f) {
                                            vertex = index;
                                            break;
                                        }
                                    }
                                    if (vertex < 0 ||
                                        !activeComponentSelection().contains(vertex)) {
                                        fullySelected = false;
                                        break;
                                    }
                                }
                            } else {
                                for (int faceEdge = 0;
                                     fullySelected &&
                                     faceEdge + 1 <
                                         faceCage.preciseLineVertices.size();
                                     faceEdge += 2) {
                                    const QVector3D &faceA =
                                        faceCage.lineVertices[faceEdge];
                                    const QVector3D &faceB =
                                        faceCage.lineVertices[faceEdge + 1];
                                    int edgeIndex = -1;
                                    for (int edge = 0;
                                         edge * 2 + 1 < cage->lineVertices.size();
                                         ++edge) {
                                        const QVector3D &a =
                                            cage->lineVertices[edge * 2];
                                        const QVector3D &b =
                                            cage->lineVertices[edge * 2 + 1];
                                        if (((a - faceA).lengthSquared() <= 1.0e-12f &&
                                             (b - faceB).lengthSquared() <= 1.0e-12f) ||
                                            ((a - faceB).lengthSquared() <= 1.0e-12f &&
                                             (b - faceA).lengthSquared() <= 1.0e-12f)) {
                                            edgeIndex = edge;
                                            break;
                                        }
                                    }
                                    if (edgeIndex < 0 ||
                                        !activeComponentSelection().contains(edgeIndex)) {
                                        fullySelected = false;
                                    }
                                }
                            }
                            if (fullySelected) {
                                ViewportRenderObject selectedFace;
                                selectedFace.shape = std::move(faceShape);
                                selectedFace.objectId = objectId;
                                selectedFace.placementTranslation =
                                    renderObject.placementTranslation;
                                selectedFace.preparedGeometryOffset =
                                    renderObject.preparedGeometryOffset;
                                ViewportDepthGeometry selectionGeometry;
                                const bool sweptPanel =
                                    geometryType == GeometryType::NurbsSolid &&
                                    face.trimLoops.isEmpty() &&
                                    face.controlVertexCountU == 2 &&
                                    face.controlVertexCountV == 2 &&
                                    face.controlPoints.size() == 4;
                                const ViewportDepthGeometry *sourceGeometry =
                                    renderObject.preparedDepthGeometry.data();
                                if (sweptPanel && sourceGeometry != nullptr &&
                                    sourceGeometry->surfaceVertices.size() ==
                                        sourceGeometry->surfaceNormals.size()) {
                                    const QVector3D origin = QVector3D(
                                        float(face.controlPoints[0].x),
                                        float(face.controlPoints[0].y),
                                        float(face.controlPoints[0].z));
                                    const QVector3D panelU = QVector3D(
                                        float(face.controlPoints[2].x -
                                              face.controlPoints[0].x),
                                        float(face.controlPoints[2].y -
                                              face.controlPoints[0].y),
                                        float(face.controlPoints[2].z -
                                              face.controlPoints[0].z));
                                    const QVector3D panelV = QVector3D(
                                        float(face.controlPoints[1].x -
                                              face.controlPoints[0].x),
                                        float(face.controlPoints[1].y -
                                              face.controlPoints[0].y),
                                        float(face.controlPoints[1].z -
                                              face.controlPoints[0].z));
                                    const float uu = QVector3D::dotProduct(
                                        panelU, panelU);
                                    const float vv = QVector3D::dotProduct(
                                        panelV, panelV);
                                    const QVector3D panelNormal =
                                        QVector3D::crossProduct(panelU, panelV)
                                            .normalized();
                                    const float spatialTolerance =
                                        std::max(std::sqrt(uu), std::sqrt(vv)) *
                                        1.0e-5f;
                                    const auto insidePanel =
                                        [&](const QVector3D &point) {
                                            const QVector3D relative = point - origin;
                                            const float u =
                                                QVector3D::dotProduct(relative,
                                                                     panelU) / uu;
                                            const float v =
                                                QVector3D::dotProduct(relative,
                                                                     panelV) / vv;
                                            const float planeDistance =
                                                std::abs(QVector3D::dotProduct(
                                                    relative, panelNormal));
                                            constexpr float parameterTolerance =
                                                1.0e-5f;
                                            return planeDistance <= spatialTolerance &&
                                                u >= -parameterTolerance &&
                                                u <= 1.0f + parameterTolerance &&
                                                v >= -parameterTolerance &&
                                                v <= 1.0f + parameterTolerance;
                                        };
                                    if (uu > 1.0e-20f && vv > 1.0e-20f &&
                                        panelNormal.lengthSquared() > 0.5f) {
                                        for (int triangle = 0;
                                             triangle + 2 <
                                                 sourceGeometry->surfaceVertices.size();
                                             triangle += 3) {
                                            const QVector3D &a =
                                                sourceGeometry->surfaceVertices[triangle];
                                            const QVector3D &b =
                                                sourceGeometry->surfaceVertices[triangle + 1];
                                            const QVector3D &c =
                                                sourceGeometry->surfaceVertices[triangle + 2];
                                            if (!insidePanel(a) || !insidePanel(b) ||
                                                !insidePanel(c)) {
                                                continue;
                                            }
                                            selectionGeometry.surfaceVertices
                                                << a << b << c;
                                            selectionGeometry.surfaceNormals
                                                << sourceGeometry->surfaceNormals[triangle]
                                                << sourceGeometry->surfaceNormals[triangle + 1]
                                                << sourceGeometry->surfaceNormals[triangle + 2];
                                        }
                                    }
                                }
                                if (!sweptPanel &&
                                    selectionGeometry.surfaceVertices.isEmpty()) {
                                    selectionGeometry = buildViewportDepthGeometry(
                                        selectedFace,
                                        &surfaceTessellationCache_);
                                }
                                selectedFace.preparedDepthGeometry =
                                    QSharedPointer<ViewportDepthGeometry>::create(
                                        std::move(selectionGeometry));
                                selectedFace.selected = true;
                                gpuVertexSelectedFaces.append(std::move(selectedFace));
                            }
                        }
                    }
                    ViewportSceneStroke edgeCage;
                    edgeCage.shape = &visibleShape;
                    const bool hasComponentSelection =
                        selected && componentSelectionObject_ == objectId &&
                        !activeComponentSelection().isEmpty();
                    edgeCage.color = selected && !hasComponentSelection
                                         ? viewportSelectionColor()
                                         : QColor(20, 20, 20);
                    edgeCage.width = 1.0f;
                    edgeCage.editModeWire = true;
                    edgeCage.objectId = objectId;
                    edgeCage.geometryRevision = renderObject.geometryRevision;
                    edgeCage.cacheableGeometry = renderObject.cacheable;
                    edgeCage.worldOffset = renderObject.placementTranslation;
                    edgeCage.preparedDepthGeometry = cage;
                    if (hasComponentSelection &&
                        componentSelectionMode_ == ComponentSelectionMode::Vertex) {
                        edgeCage.color = Qt::white;
                        for (const QVector3D &endpoint : cage->lineVertices) {
                            bool endpointSelected = false;
                            for (int vertex = 0; vertex < cage->pointVertices.size(); ++vertex) {
                                if ((cage->pointVertices[vertex] - endpoint).lengthSquared() <= 1.0e-12f &&
                                    activeComponentSelection().contains(vertex)) {
                                    endpointSelected = true;
                                    break;
                                }
                            }
                            const QColor color = endpointSelected
                                ? QColor(QStringLiteral("#ff9900")) : QColor(Qt::black);
                            edgeCage.lineVertexColors.append(QVector4D(
                                color.redF(), color.greenF(), color.blueF(), 1.0f));
                        }
                    }
                    gpuStrokes.append(std::move(edgeCage));
                    ViewportSceneStroke vertexCage;
                    vertexCage.shape = &visibleShape;
                    vertexCage.color = selected && !hasComponentSelection
                                           ? QColor(QStringLiteral("#ff7a00"))
                                           : QColor(12, 12, 12);
                    vertexCage.pointDiameter =
                        componentSelectionMode_ == ComponentSelectionMode::Vertex
                            ? 4.0f : 0.0f;
                    vertexCage.editModeWire = true;
                    vertexCage.objectId = objectId;
                    vertexCage.geometryRevision = renderObject.geometryRevision;
                    vertexCage.cacheableGeometry = renderObject.cacheable;
                    vertexCage.worldOffset = renderObject.placementTranslation;
                    vertexCage.preparedDepthGeometry = cage;
                    if (vertexCage.pointDiameter > 0.0f)
                        gpuStrokes.append(std::move(vertexCage));

                    if (selected && componentSelectionObject_ == objectId &&
                        !activeComponentSelection().isEmpty()) {
                        const auto appendComponentStroke = [&](ViewportDepthGeometry geometry,
                                                               const QColor &color,
                                                               float width,
                                                               float pointDiameter,
                                                               QVector<QVector4D> lineVertexColors) {
                            if (geometry.pointVertices.isEmpty() &&
                                geometry.lineVertices.isEmpty()) return;
                            QByteArray geometryKeyBytes;
                            QDataStream geometryKeyStream(&geometryKeyBytes,
                                                          QIODevice::WriteOnly);
                            geometryKeyStream << quint32(geometry.pointVertices.size());
                            for (const QVector3D &point : geometry.pointVertices) {
                                geometryKeyStream << point.x() << point.y() << point.z();
                            }
                            geometryKeyStream << quint32(geometry.lineVertices.size());
                            for (const QVector3D &point : geometry.lineVertices) {
                                geometryKeyStream << point.x() << point.y() << point.z();
                            }
                            quint64 dynamicGeometryRevision = 1469598103934665603ULL;
                            for (const char byte : geometryKeyBytes) {
                                dynamicGeometryRevision ^=
                                    static_cast<unsigned char>(byte);
                                dynamicGeometryRevision *= 1099511628211ULL;
                            }
                            dynamicGeometryRevision ^=
                                renderObject.geometryRevision * 1099511628211ULL;
                            if (dynamicGeometryRevision == 0) dynamicGeometryRevision = 1;
                            ViewportSceneStroke stroke;
                            stroke.shape = &visibleShape;
                            stroke.color = color;
                            stroke.width = width;
                            stroke.pointDiameter = pointDiameter;
                            stroke.lineVertexColors = std::move(lineVertexColors);
                            stroke.editModeWire = true;
                            stroke.objectId = objectId;
                            stroke.geometryRevision = dynamicGeometryRevision;
                            stroke.cacheableGeometry = renderObject.cacheable;
                            stroke.worldOffset = renderObject.placementTranslation;
                            stroke.preparedDepthGeometry =
                                QSharedPointer<ViewportDepthGeometry>::create(
                                    std::move(geometry));
                            gpuStrokes.append(std::move(stroke));
                        };
                        if (componentSelectionMode_ == ComponentSelectionMode::Vertex) {
                            const QColor selectedVertexColor(QStringLiteral("#ff7a00"));
                            QMap<QRgb, ViewportDepthGeometry> fadedPoints;
                            for (int component : activeComponentSelection()) {
                                if (component < 0 || component >= cage->pointVertices.size()) continue;
                                const QVector3D &point = cage->pointVertices[component];
                                const QColor baseColor = component == activeComponentIndex()
                                                             ? QColor(Qt::white)
                                                             : selectedVertexColor;
                                fadedPoints[baseColor.rgba()].pointVertices.append(point);
                            }
                            for (auto it = fadedPoints.begin(); it != fadedPoints.end(); ++it) {
                                appendComponentStroke(std::move(it.value()),
                                                      QColor::fromRgba(it.key()),
                                                      1.0f, 4.0f, {});
                            }
                        } else if (componentSelectionMode_ == ComponentSelectionMode::Edge) {
                            QMap<QRgb, ViewportDepthGeometry> selectedEdges;
                            for (int component : activeComponentSelection()) {
                                const int first = component * 2;
                                if (first + 1 >= cage->lineVertices.size()) continue;
                                const QColor color = component == activeComponentIndex()
                                                         ? QColor(Qt::white)
                                                         : QColor(QStringLiteral("#ff7a00"));
                                ViewportDepthGeometry &componentGeometry =
                                    selectedEdges[color.rgba()];
                                componentGeometry.lineVertices
                                    << cage->lineVertices[first]
                                    << cage->lineVertices[first + 1];
                                componentGeometry.preciseLineVertices
                                    << cage->preciseLineVertices[first]
                                    << cage->preciseLineVertices[first + 1];
                            }
                            for (auto it = selectedEdges.begin();
                                 it != selectedEdges.end(); ++it) {
                                appendComponentStroke(std::move(it.value()),
                                                      QColor::fromRgba(it.key()),
                                                      2.0f, 0.0f, {});
                            }
                        } else {
                            ViewportDepthGeometry componentGeometry;
                            const QVector<NurbsSurface3D> faces = shapeSurfaceFaces(visibleShape);
                            for (int component : activeComponentSelection()) {
                                if (component < 0 || component >= faces.size()) continue;
                                Shape faceShape;
                                faceShape.geometryType = GeometryType::NurbsSurface;
                                faceShape.nurbsSurface = faces[component];
                                const ViewportDepthGeometry faceCage = selectedSurfaceCage(faceShape);
                                componentGeometry.lineVertices += faceCage.lineVertices;
                                componentGeometry.preciseLineVertices += faceCage.preciseLineVertices;
                            }
                            appendComponentStroke(std::move(componentGeometry),
                                                  QColor(QStringLiteral("#ff8a00")),
                                                  2.0f, 0.0f, {});
                        }
                    }
                }
                continue;
            }
            if (sceneRenderer != nullptr && previewRenderer != nullptr &&
                geometryType == GeometryType::Picture &&
                !scalePreview && !rotatePreview &&
                !visibleShape.pictureImage.isNull() &&
                pictureFrameCorners(visibleShape).size() == 4) {
                gpuPreviewPictures.append(
                    {visibleShape,
                     pictureFrameOutline(visibleShape),
                     -1,
                     index,
                     ObjectId::invalid(),
                     false,
                     1.0f,
                     viewportSelectionColor()});
                if (selected) {
                    ViewportSceneStroke outlineStroke{
                        &gpuPreviewPictures.back().frame,
                        viewportSelectionColor(),
                        1.5f,
                        false};
                    outlineStroke.objectId = objectId;
                    outlineStroke.geometryRevision =
                        renderObject.geometryRevision;
                    outlineStroke.cacheableGeometry = renderObject.cacheable;
                    gpuStrokes.append(outlineStroke);
                }
                continue;
            }
            if (sceneRenderer != nullptr && previewRenderer != nullptr &&
                geometryType == GeometryType::Picture &&
                (scalePreview || rotatePreview) &&
                !visibleShape.pictureImage.isNull() &&
                pictureFrameCorners(visibleShape).size() == 4) {
                gpuPreviewPictures.append(
                    {visibleShape,
                     pictureFrameOutline(visibleShape),
                     -1,
                     index,
                     ObjectId::invalid(),
                     false,
                     1.0f,
                     viewportSelectionColor()});
                ViewportSceneStroke outlineStroke{
                    &gpuPreviewPictures.back().frame,
                    viewportSelectionColor(),
                    1.5f,
                    false};
                outlineStroke.objectId = objectId;
                outlineStroke.geometryRevision = renderObject.geometryRevision;
                outlineStroke.cacheableGeometry = renderObject.cacheable;
                gpuStrokes.append(outlineStroke);
                continue;
            }
            ViewportSceneStroke sceneStroke;
            ViewportSceneStroke controlGuide;
            if (makeViewportSceneStrokes(renderObject,
                                         sceneRenderer != nullptr,
                                         &sceneStroke,
                                         &controlGuide)) {
                if (controlGuide.shape != nullptr) {
                    gpuStrokes.append(std::move(controlGuide));
                }
                gpuStrokes.append(sceneStroke);
                continue;
            }
            if (scalePreview || rotatePreview) {
                drawShape(scenePainter,
                          visibleShape,
                          false,
                          true,
                          true,
                          layerColor,
                          layerLineType,
                          layerLineWeightMm,
                          ObjectId::invalid(),
                          0,
                          renderObject.preparedDepthGeometry.data(),
                          renderObject.preparedGeometryOffset);
            } else {
                drawShape(scenePainter,
                          visibleShape,
                          false,
                          selected,
                          true,
                          layerColor,
                          layerLineType,
                          layerLineWeightMm,
                          objectId,
                          renderObject.geometryRevision,
                          renderObject.preparedDepthGeometry.data(),
                          renderObject.preparedGeometryOffset);
            }
        }

        const Layer *activeDrawingLayer =
            document_.layer(document_.activeLayerId());
        const QColor arcPreviewColor =
            activeDrawingLayer != nullptr && activeDrawingLayer->color.isValid()
                ? activeDrawingLayer->color
                : QColor(QStringLiteral("#d28b45"));
        if (sceneRenderer != nullptr && previewRenderer != nullptr) {
            const auto addPreviewShape =
                [&gpuPreviewGeometry, this](const Shape &shape,
                                     const QColor &color,
                                     float width,
                                     bool controlGuide,
                                     float pointDiameter,
                                     bool dashed,
                                     bool pointOutline,
                                     const Point3D &worldOffset = Point3D{}) {
                    const GeometryType type = shape.geometryType;
                    const bool supportedType =
                        type == GeometryType::Point || type == GeometryType::Line ||
                        type == GeometryType::Rectangle || type == GeometryType::Polygon ||
                        type == GeometryType::Circle || type == GeometryType::Ellipse ||
                        type == GeometryType::Arc || type == GeometryType::PolyCurve ||
                        type == GeometryType::Bezier || type == GeometryType::Nurbs ||
                        type == GeometryType::NurbsSolid ||
                        type == GeometryType::NurbsSurface;
                    if (!supportedType || isDimensionGeometryType(type) ||
                        type == GeometryType::Picture) {
                        return false;
                    }
                    if ((type == GeometryType::Point && shape.points.isEmpty()) ||
                        ((type == GeometryType::Circle || type == GeometryType::Ellipse ||
                          type == GeometryType::Arc || type == GeometryType::Bezier ||
                          type == GeometryType::Nurbs) &&
                         !validateNurbsCurve(shape.nurbs)) ||
                        (type == GeometryType::PolyCurve && shape.components.isEmpty()) ||
                        (type == GeometryType::NurbsSolid && !validateNurbsSolid(shape.nurbsSolid))) {
                        return false;
                    }
                    gpuPreviewGeometry.append(
                        {shape, color, width, controlGuide, pointDiameter,
                         dashed, pointOutline, worldOffset,
                         activeTool_ == Tool::PointExtrude});
                    return true;
                };

            const auto addPicturePreview =
                [&gpuPreviewPictures](const Shape &picture,
                                      int duplicateIndex,
                                      bool activeToolPreview,
                                      ObjectId mirrorObjectId) {
                    if (picture.geometryType != GeometryType::Picture ||
                        picture.pictureImage.isNull() ||
                        pictureFrameCorners(picture).size() != 4) {
                        return false;
                    }
                    gpuPreviewPictures.append(
                        {picture,
                         pictureFrameOutline(picture),
                         duplicateIndex,
                         -1,
                         mirrorObjectId,
                         activeToolPreview,
                         0.78f,
                         QColor(QStringLiteral("#e6b85c"))});
                    return true;
                };

            const auto addPreviewLine =
                [&](const QPointF &first,
                    const QPointF &second,
                    const QColor &color,
                    float width) {
                    Shape line;
                    line.geometryType = GeometryType::Line;
                    line.points = {first, second};
                    line.workPlane = viewportTransform_.workPlane();
                    line.workPlaneOffset = viewportTransform_.workPlaneOffset();
                    line.workPlaneFrame = viewportTransform_.workPlaneFrame();
                    return addPreviewShape(line, color, width, false, 0.0f,
                                           true, false);
                };

            const auto addSolidPreviewLine =
                [&](const QPointF &first,
                    const QPointF &second,
                    const QColor &color,
                    float width) {
                    Shape line;
                    line.geometryType = GeometryType::Line;
                    line.points = {first, second};
                    line.workPlane = viewportTransform_.workPlane();
                    line.workPlaneOffset = viewportTransform_.workPlaneOffset();
                    line.workPlaneFrame = viewportTransform_.workPlaneFrame();
                    return addPreviewShape(line, color, width, false, 0.0f,
                                           false, false);
                };

            const auto addPreviewPoint =
                [&](const QPointF &point,
                    const QColor &color,
                    float diameter,
                    bool outline) {
                    Shape marker;
                    marker.geometryType = GeometryType::Point;
                    marker.points = {point};
                    marker.workPlane = viewportTransform_.workPlane();
                    marker.workPlaneOffset = viewportTransform_.workPlaneOffset();
                    marker.workPlaneFrame = viewportTransform_.workPlaneFrame();
                    return addPreviewShape(marker, color, 1.5f, false, diameter,
                                           false, outline);
                };

            const QColor previewColor(QStringLiteral("#e6b85c"));
            const QColor activeToolPreviewColor =
                activeTool_ == Tool::PointExtrude || activeTool_ == Tool::Arc ||
                        isCircleConstructionTool(activeTool_) ||
                        isEllipseTool(activeTool_) || isPolygonTool(activeTool_) ||
                        isRectangleTool(activeTool_)
                    ? arcPreviewColor
                    : previewColor;
            for (int index = 0;
                 index < duplicateTool_.previewShapes().size();
                 ++index) {
                const Shape &preview = duplicateTool_.previewShapes()[index];
                if (preview.geometryType == GeometryType::Picture) {
                    addPicturePreview(preview, index, false,
                                      ObjectId::invalid());
                } else {
                    gpuDuplicatePreviewHandled[index] = addPreviewShape(
                        preview, previewColor, 1.5f, false, 0.0f, false, false,
                        duplicateTool_.previewPlacementTranslations().value(index));
                }
            }

            if (activeTool_ == Tool::Mirror && mirrorTool_.hasAxisStart() &&
                cursorValid_) {
                for (const ObjectId objectId : mirrorTool_.sourceObjectIds()) {
                    const int shapeIndex = objectIndex(objectId);
                    if (shapeIndex < 0 || shapeIndex >= shapes_.size() ||
                        !document_.isObjectVisible(objectId)) {
                        continue;
                    }
                    Shape mirroredShape;
                    Shape sourceShape = shapes_[shapeIndex];
                    const SceneObject *sourceObject = document_.object(objectId);
                    if (sourceObject != nullptr &&
                        (sourceObject->placementTranslation.x != 0.0 ||
                         sourceObject->placementTranslation.y != 0.0 ||
                         sourceObject->placementTranslation.z != 0.0)) {
                        bakeShapePlacementTranslation(
                            &sourceShape, sourceObject->placementTranslation);
                    }
                    if (mirrorShapeAcrossLine(sourceShape,
                                              mirrorTool_.axisStart(),
                                              cursorWorld_,
                                              &mirroredShape)) {
                        if (mirroredShape.geometryType == GeometryType::Picture) {
                            addPicturePreview(mirroredShape, -1, false, objectId);
                        } else if (addPreviewShape(mirroredShape,
                                                   previewColor,
                                                   1.5f,
                                                   false,
                                                   0.0f,
                                                   false,
                                                   false)) {
                            gpuMirrorPreviewHandled.append(objectId);
                        }
                    }
                }
            }

            if (activeToolPreview.hasShape &&
                !isDimensionGeometryType(activeToolPreview.shape.geometryType)) {
                gpuActiveToolPreview |= addPreviewShape(
                    activeToolPreview.shape, activeToolPreviewColor,
                    1.5f, false, 0.0f,
                    false, false);
            }
            if (activeTool_ == Tool::PointExtrude) {
                for (const Shape &previewShape : activeToolPreview.shapes) {
                    gpuActiveToolPreview |= addPreviewShape(
                        previewShape, activeToolPreviewColor,
                        1.5f, false, 0.0f, false, false);
                }
            }

            if (isRectangleTool(activeTool_)) {
                const QColor markerColor(QStringLiteral("#101010"));
                for (const ToolPreviewGuide &guide : activeToolPreview.guides) {
                    gpuActiveToolPreview |= addSolidPreviewLine(
                        guide.line.p1(), guide.line.p2(), guide.color, 1.5f);
                }
                for (const QPointF &point : pendingPoints_) {
                    gpuActiveToolPreview |= addPreviewPoint(point, markerColor, 5.0f, false);
                }
                if (cursorValid_) {
                    gpuActiveToolPreview |= addPreviewPoint(cursorWorld_, markerColor, 5.0f, false);
                }
                if (activeToolPreview.hasShape &&
                    activeToolPreview.shape.geometryType == GeometryType::Rectangle) {
                    for (const QPointF &point : rectangleVertices(activeToolPreview.shape)) {
                        gpuActiveToolPreview |= addPreviewPoint(point, markerColor, 5.0f, false);
                    }
                }
            } else if (isCircleConstructionTool(activeTool_)) {
                const WorkPlaneFrame frame = viewportTransform_.workPlaneFrame();
                const auto circleGuideColor = [&](const QPointF &delta) {
                    const qreal worldX = frame.xAxis.x * delta.x() +
                                         frame.yAxis.x * delta.y();
                    const qreal worldY = frame.xAxis.y * delta.x() +
                                         frame.yAxis.y * delta.y();
                    const qreal worldZ = frame.xAxis.z * delta.x() +
                                         frame.yAxis.z * delta.y();
                    const qreal magnitude = std::sqrt(worldX * worldX +
                                                      worldY * worldY +
                                                      worldZ * worldZ);
                    if (magnitude <= 1.0e-9) {
                        return QColor(128, 128, 128, 180);
                    }
                    const qreal x = std::abs(worldX / magnitude);
                    const qreal y = std::abs(worldY / magnitude);
                    const qreal z = std::abs(worldZ / magnitude);
                    if (x > 0.9999) return QColor(255, 26, 26);
                    if (y > 0.9999) return QColor(26, 179, 26);
                    if (z > 0.9999) return QColor(51, 128, 255);
                    return QColor(128, 128, 128, 180);
                };
                const QColor markerColor(QStringLiteral("#101010"));
                if (pendingPoints_.isEmpty()) {
                    if (cursorValid_) {
                        gpuActiveToolPreview |= addPreviewPoint(
                            cursorWorld_, markerColor, 5.0f, false);
                    }
                } else {
                    for (const QPointF &point : pendingPoints_) {
                        gpuActiveToolPreview |= addPreviewPoint(
                            point, markerColor, 5.0f, false);
                    }
                    if (cursorValid_) {
                        gpuActiveToolPreview |= addPreviewPoint(
                            cursorWorld_, markerColor, 5.0f, false);
                        if (activeTool_ == Tool::CircleThreePoint &&
                            pendingPoints_.size() >= 2) {
                            gpuActiveToolPreview |= addSolidPreviewLine(
                                pendingPoints_.first(), pendingPoints_[1],
                                circleGuideColor(pendingPoints_[1] -
                                                 pendingPoints_.first()),
                                1.0f);
                            gpuActiveToolPreview |= addSolidPreviewLine(
                                pendingPoints_[1], cursorWorld_,
                                circleGuideColor(cursorWorld_ - pendingPoints_[1]),
                                1.0f);
                        } else {
                            gpuActiveToolPreview |= addSolidPreviewLine(
                                pendingPoints_.first(), cursorWorld_,
                                circleGuideColor(cursorWorld_ -
                                                 pendingPoints_.first()),
                                1.0f);
                        }
                    }
                }
            } else if (isPointCreationTool(activeTool_) &&
                       activeTool_ != Tool::Point) {
                const QColor pointColor(QStringLiteral("#101010"));
                const QColor arcColor(QStringLiteral("#33cc33"));
                for (const Shape &preview : activeToolPreview.shapes) {
                    if (preview.geometryType == GeometryType::Point) continue;
                    const bool isArc = preview.geometryType == GeometryType::Arc ||
                                       preview.geometryType == GeometryType::Circle;
                    gpuActiveToolPreview |= addPreviewShape(
                        preview, isArc ? arcColor : pointColor,
                        isArc ? 2.0f : 1.5f, false,
                        0.0f, false, false);
                }
                for (const ToolPreviewGuide &guide : activeToolPreview.guides) {
                    Shape line;
                    line.geometryType = GeometryType::Line;
                    line.points = {guide.line.p1(), guide.line.p2()};
                    line.workPlane = WorkPlane::XY;
                    line.workPlaneOffset = guide.hasWorkPlaneFrame
                        ? guide.workPlaneFrame.origin.z
                        : activeToolPreview.workPlaneFrame.origin.z;
                    line.workPlaneFrame = guide.hasWorkPlaneFrame
                        ? guide.workPlaneFrame : activeToolPreview.workPlaneFrame;
                    gpuActiveToolPreview |= addPreviewShape(
                        line, guide.color, 1.25f, false, 0.0f,
                        guide.dashed, false);
                }
            }

            if (activeTool_ == Tool::Point && cursorValid_) {
                Shape pointPreview;
                if (makeToolShape(Tool::Point,
                                  {cursorWorld_},
                                  arcState().mode,
                                  arcState().previewSweepAngle,
                                  &pointPreview)) {
                    gpuActiveToolPreview |= addPreviewShape(
                        pointPreview, previewColor, 1.5f, false, 9.0f,
                        false, false);
                }
            } else if (activeTool_ == Tool::Line) {
                for (const Shape &preview : activeToolPreview.shapes) {
                    gpuActiveToolPreview |= addPreviewShape(
                        preview, QColor(QStringLiteral("#000000")),
                        1.0f, false, 0.0f, false, false);
                }
            } else if (activeTool_ != Tool::Picture &&
                       activeTool_ != Tool::Mirror &&
                       activeTool_ != Tool::Rotate &&
                       activeTool_ != Tool::Scale &&
                       !isPointCreationTool(activeTool_) &&
                       !isCurveCreationTool(activeTool_) &&
                       !isTwoCurveLineTool(activeTool_) &&
                       !isEllipseTool(activeTool_) &&
                       !isCircleConstructionTool(activeTool_) &&
                       !isPolygonTool(activeTool_) &&
                       !isRectangleTool(activeTool_) &&
                       !pendingPoints_.isEmpty() && cursorValid_) {
                QVector<QPointF> candidatePoints = pendingPoints_;
                candidatePoints.append(cursorWorld_);
                Shape toolPreview;
                if (makeToolShape(activeTool_,
                                  candidatePoints,
                                  arcState().mode,
                                  arcState().previewSweepAngle,
                                  &toolPreview)) {
                    gpuActiveToolPreview |= addPreviewShape(
                        toolPreview,
                        activeToolPreviewColor,
                        2.0f,
                        false,
                        toolPreview.geometryType == GeometryType::Point ? 9.0f : 0.0f,
                        false,
                        false);
                }
            }

            if (activeTool_ == Tool::Picture && pendingPoints_.size() == 1 &&
                cursorValid_ && !pendingPictureImage_.isNull()) {
                const Shape picture = pictureShapeForCorner(cursorWorld_);
                addPicturePreview(picture, -1, true, ObjectId::invalid());
            }

            if (activeTool_ == Tool::Rotate && cursorValid_) {
                const QColor rotateColor(QStringLiteral("#e6b85c"));
                const QColor guideColor(QStringLiteral("#8aa7c7"));
                const QColor pointColor(QStringLiteral("#f0a45a"));
                if (rotateState().stage == 0) {
                    gpuActiveToolPreview |= addPreviewPoint(
                        cursorWorld_, pointColor, 12.0f, true);
                } else {
                    gpuActiveToolPreview |= addPreviewPoint(
                        rotateState().basePoint, pointColor, 12.0f, true);
                    if (rotateState().stage >= 1) {
                        gpuActiveToolPreview |= addPreviewLine(
                            rotateState().basePoint, cursorWorld_, guideColor, 1.0f);
                    }
                    if (rotateState().stage >= 2) {
                        gpuActiveToolPreview |= addPreviewLine(
                            rotateState().basePoint, rotateState().referencePoint, rotateColor,
                            1.0f);
                    }
                }
                gpuActiveToolPreview |= addPreviewPoint(
                    cursorWorld_, pointColor, 8.0f, false);
            }

            if (activeTool_ == Tool::Scale && scaleState().stage == 2 && cursorValid_) {
                QPointF guideEnd = cursorWorld_;
                if (scaleState().mode == ScaleMode::OneD && !scaleState().usingTypedFactor) {
                    const QPointF offset = cursorWorld_ - scaleState().basePoint;
                    guideEnd = scaleState().basePoint +
                               scaleState().axisDirection *
                                   QPointF::dotProduct(offset,
                                                       scaleState().axisDirection);
                }
                const QColor guideColor(QStringLiteral("#8aa7c7"));
                gpuActiveToolPreview |= addPreviewLine(
                    scaleState().basePoint, guideEnd, guideColor, 1.0f);
                gpuActiveToolPreview |= addPreviewPoint(
                    scaleState().basePoint, previewColor, 8.0f, true);
            }
        }

        QVector<ViewportSceneStroke> gpuPreviewStrokes;
        gpuPreviewStrokes.reserve(gpuPreviewGeometry.size());
        QVector<ViewportRenderObject> gpuPreviewSurfaceObjects;
        gpuPreviewSurfaceObjects.reserve(gpuPreviewGeometry.size());
        QVector<ViewportRenderObject> gpuOpaquePreviewSurfaceObjects;
        gpuOpaquePreviewSurfaceObjects.reserve(gpuPreviewGeometry.size());
        for (const TransientPreviewStroke &preview : gpuPreviewGeometry) {
            if (viewportShadingSettings_.mode == ViewportShadingMode::Solid &&
                (preview.shape.geometryType == GeometryType::NurbsSurface ||
                 preview.shape.geometryType == GeometryType::NurbsSolid)) {
                ViewportRenderObject surfaceObject;
                surfaceObject.shape = preview.shape;
                surfaceObject.placementTranslation = preview.worldOffset;
                surfaceObject.preparedGeometryOffset = preview.worldOffset;
                surfaceObject.cacheable = false;
                surfaceObject.selected = false;
                const Layer *previewLayer =
                    document_.layer(document_.activeLayerId());
                if (previewLayer != nullptr) {
                    surfaceObject.layerColor = previewLayer->color;
                }
                surfaceObject.preparedDepthGeometry =
                    QSharedPointer<ViewportDepthGeometry>::create(
                        buildViewportDepthGeometry(surfaceObject,
                                                   &surfaceTessellationCache_));
                if (preview.opaqueSurface) {
                    gpuOpaquePreviewSurfaceObjects.append(
                        std::move(surfaceObject));
                } else {
                    gpuPreviewSurfaceObjects.append(std::move(surfaceObject));
                }
                continue;
            }
            gpuPreviewStrokes.append({&preview.shape,
                                      preview.color,
                                      preview.width,
                                      preview.controlGuide,
                                      preview.pointDiameter,
                                      preview.dashed,
                                      preview.pointOutline});
            gpuPreviewStrokes.last().worldOffset = preview.worldOffset;
            if (!preview.controlGuide &&
                (preview.shape.geometryType == GeometryType::NurbsSurface ||
                 preview.shape.geometryType == GeometryType::NurbsSolid)) {
                ViewportRenderObject object;
                object.shape = preview.shape;
                object.placementTranslation = preview.worldOffset;
                object.cacheable = false;
                gpuPreviewStrokes.last().preparedDepthGeometry =
                    QSharedPointer<ViewportDepthGeometry>::create(
                        buildViewportDepthGeometry(object,
                                                   &surfaceTessellationCache_));
            }
        }
        gpuPreviewStrokes.reserve(gpuPreviewStrokes.size() +
                                  gpuPreviewPictures.size());

        const bool gpuArcToolPreview =
            activeTool_ == Tool::Arc &&
            (arcState().mode == ArcMode::OnePoint || arcState().mode == ArcMode::TwoPoint ||
             arcState().mode == ArcMode::ThreePoint);
        const qreal arcHudPanelWidth = arcState().mode != ArcMode::OnePoint
                                           ? 750.0
                                           : 570.0;
        const ArcHudDisplay arcHudDisplay =
            arcState().mode == ArcMode::OnePoint
                ? onePointArcHudDisplay()
                : arcState().mode == ArcMode::TwoPoint
                      ? twoPointArcHudDisplay()
                      : threePointArcHudDisplay();
        const QImage arcHudText = nativeRenderer != nullptr &&
                                          gpuArcToolPreview
                                      ? arcHudTextImage(arcHudDisplay,
                                                        devicePixelRatioF(),
                                                        arcHudPanelWidth)
                                      : QImage{};
        bool gpuPreviewRendered = false;
        bool gpuOpaqueExtrudePreviewRendered = false;
        bool gpuArcOverlayRendered = false;
        QVector<const Shape *> failedScenePicturePreviews;
        if (nativeRenderer != nullptr) {
            painter.beginNativePainting();
            if (sceneRenderer != nullptr) {
                for (const TransientPreviewPicture &preview : gpuPreviewPictures) {
                    if (preview.sceneShapeIndex >= 0 &&
                        !sceneRenderer->drawPicture(preview.image,
                                                    renderFrame.camera,
                                                    renderFrame.viewportSize,
                                                    devicePixelRatioF(),
                                                    preview.opacity)) {
                        failedScenePicturePreviews.append(&preview.image);
                    }
                }
            }
            QVector<ViewportRenderObject> surfaceDrawObjects = visibleShapes;
            if (!activeComponentSelection().isEmpty()) {
                for (ViewportRenderObject &object : surfaceDrawObjects) {
                    if (object.objectId == componentSelectionObject_) {
                        // Suppress the object tint only in the surface pass.
                        // Component overlays still need the real selection state.
                        object.selected = false;
                    }
                }
            }
            const bool surfacesDrawn = surfaceRenderer == nullptr ||
                surfaceRenderer->draw(surfaceDrawObjects,
                                      renderFrame.camera,
                                      renderFrame.viewportSize,
                                      devicePixelRatioF(),
                                      viewportShadingSettings_);
            if (surfaceRenderer != nullptr && !gpuVertexSelectedFaces.isEmpty()) {
                surfaceRenderer->draw(gpuVertexSelectedFaces,
                                      renderFrame.camera,
                                      renderFrame.viewportSize,
                                      devicePixelRatioF(),
                                      viewportShadingSettings_,
                                      false,
                                      false,
                                      true);
            }
            const bool xrayEnabled = viewportShadingSettings_.xrayEnabled();
            bool sceneDrawn = sceneRenderer == nullptr;
            if (sceneRenderer != nullptr && xrayEnabled) {
                // Blender draws edit wires through X-Ray, fading the part
                // behind the surface to half opacity, then draws visible
                // wires at full opacity against the surface depth buffer.
                const bool backWiresDrawn = sceneRenderer->draw(
                    gpuStrokes, renderFrame.camera, renderFrame.viewportSize,
                    devicePixelRatioF(), false, 0.5,
                    smoothWiresOverlay_, smoothWiresEditMode_);
                const bool frontWiresDrawn = sceneRenderer->draw(
                    gpuStrokes, renderFrame.camera, renderFrame.viewportSize,
                    devicePixelRatioF(), true, 1.0,
                    smoothWiresOverlay_, smoothWiresEditMode_);
                sceneDrawn = backWiresDrawn && frontWiresDrawn;
            } else if (sceneRenderer != nullptr) {
                sceneDrawn = sceneRenderer->draw(
                    gpuStrokes, renderFrame.camera, renderFrame.viewportSize,
                    devicePixelRatioF(), true, 1.0,
                    smoothWiresOverlay_, smoothWiresEditMode_);
            }
            const bool gridDrawn = nativeRenderer->renderToCurrentFramebuffer(
                renderFrame.camera, renderFrame.viewportSize,
                devicePixelRatioF(),
                visibleShapes, baseGridStep, gridAppearance_);
            if (previewRenderer != nullptr) {
                for (const TransientPreviewPicture &preview : gpuPreviewPictures) {
                    if (preview.sceneShapeIndex >= 0) {
                        continue;
                    }
                    if (!previewRenderer->drawPicture(preview.image,
                                                      renderFrame.camera,
                                                      renderFrame.viewportSize,
                                                      devicePixelRatioF(),
                                                      preview.opacity)) {
                        continue;
                    }
                    if (preview.duplicateIndex >= 0 &&
                        preview.duplicateIndex < gpuDuplicatePreviewHandled.size()) {
                        gpuDuplicatePreviewHandled[preview.duplicateIndex] = true;
                    }
                    if (preview.mirrorObjectId.isValid()) {
                        gpuMirrorPreviewHandled.append(preview.mirrorObjectId);
                    }
                    gpuActivePicturePreviewRendered |= preview.activeToolPreview;
                    gpuPreviewStrokes.append({&preview.frame,
                                              preview.frameColor,
                                              preview.sceneShapeIndex >= 0 ? 1.5f
                                                                           : 1.25f,
                                              false,
                                              0.0f,
                                              preview.sceneShapeIndex < 0,
                                              false});
                }
            }
            if (surfaceRenderer != nullptr &&
                !gpuPreviewSurfaceObjects.isEmpty()) {
                surfaceRenderer->draw(gpuPreviewSurfaceObjects,
                                     renderFrame.camera,
                                     renderFrame.viewportSize,
                                     devicePixelRatioF(),
                                     viewportShadingSettings_,
                                     true,
                                     false);
            }
            if (surfaceRenderer != nullptr &&
                !gpuOpaquePreviewSurfaceObjects.isEmpty()) {
                gpuOpaqueExtrudePreviewRendered = surfaceRenderer->draw(
                    gpuOpaquePreviewSurfaceObjects,
                    renderFrame.camera,
                    renderFrame.viewportSize,
                    devicePixelRatioF(),
                    viewportShadingSettings_,
                    false,
                    false);
            }
            gpuPreviewRendered = previewRenderer != nullptr &&
                                 previewRenderer->draw(gpuPreviewStrokes,
                                                       renderFrame.camera,
                                                       renderFrame.viewportSize,
                                                       devicePixelRatioF());
            if (controlPointsVisible_ && controlPointRenderer != nullptr) {
                gpuControlPointsDrawn = controlPointRenderer->draw(
                    gpuControlPointHandles, renderFrame.camera,
                    renderFrame.viewportSize,
                    devicePixelRatioF());
            }
            if (gpuArcToolPreview && previewRenderer != nullptr) {
                gpuArcOverlayRendered =
                    previewRenderer->drawArcToolOverlay(
                        renderFrame.camera,
                        renderFrame.viewportSize,
                        devicePixelRatioF(),
                        viewportTransform_.workPlaneFrame(),
                        pendingPoints_,
                        cursorWorld_,
                        cursorValid_,
                        arcState().mode,
                        arcState().previewSweepAngle,
                        arcPreviewColor,
                        currentSnap_,
                        arcCompassRotation(),
                        arcHudText,
                        arcHudPanelWidth,
                        !gpuActiveToolPreview || !gpuPreviewRendered,
                        viewportOverlay_.snapLabelsVisible());
            }
            painter.endNativePainting();
            if (!sceneDrawn) {
                for (const ViewportSceneStroke &stroke : gpuStrokes) {
                    if (stroke.shape != nullptr && !stroke.controlGuide) {
                        drawShape(painter, *stroke.shape, false,
                                  stroke.color == viewportSelectionColor(),
                                  true, stroke.color, QString(), 0.0,
                                  ObjectId::invalid(), 0, nullptr,
                                  stroke.worldOffset);
                    }
                }
            }
            if (!surfacesDrawn) {
                for (const ViewportRenderObject &renderObject : visibleShapes) {
                    if (renderObject.shape.geometryType == GeometryType::NurbsSurface ||
                        renderObject.shape.geometryType == GeometryType::NurbsSolid) {
                        drawShape(painter, renderObject.shape, false,
                                  renderObject.selected, true,
                                  renderObject.layerColor,
                                  renderObject.layerLineType,
                                  renderObject.layerLineWeightMm,
                                  ObjectId::invalid(), 0,
                                  renderObject.preparedDepthGeometry.data(),
                                  renderObject.preparedGeometryOffset);
                    }
                }
            }
            if (!gridDrawn) {
                drawGrid(painter);
                drawOrigin(painter);
            }
            for (const Shape *failedPicture : failedScenePicturePreviews) {
                if (failedPicture != nullptr) {
                    drawShape(painter, *failedPicture, false, true);
                }
            }
        } else {
            rasterScenePainter.end();
            fillViewportBackground(
                painter, rect(),
                viewportShadingBackgroundColor(viewportShadingSettings_));
            if (gpuViewportBackground.isNull()) {
                drawGrid(painter);
                drawOrigin(painter);
            }
            painter.drawImage(QPoint(0, 0), committedSceneLayer);
            if (!gpuViewportBackground.isNull()) {
                painter.drawImage(QPoint(0, 0), gpuViewportBackground);
            }
        }

        for (const ViewportRenderObject &renderObject : visibleShapes) {
            const ObjectId objectId = renderObject.objectId;
            const int index = renderObject.objectIndex;
            const Shape &shape = shapes_[index];
            if (isDimensionGeometryType(shape.geometryType)) {
                const bool selected = renderObject.selected;
                drawShape(painter,
                          shape,
                          false,
                          selected,
                          true,
                          renderObject.layerColor,
                          renderObject.layerLineType,
                          renderObject.layerLineWeightMm);
            }
            if (!subdivisionTool_.isActive() || objectId != subdivisionTool_.targetObjectId()) {
                drawSubdivisionPoints(painter,
                                      shape,
                                      shape.subdivisionParameters,
                                      false);
            }
        }

        if (duplicateTool_.isActive()) {
            for (int index = 0;
                 index < duplicateTool_.previewShapes().size();
                 ++index) {
                if (!gpuPreviewRendered || !gpuDuplicatePreviewHandled.value(index)) {
                    drawShape(painter,
                              duplicateTool_.previewShapes()[index],
                              true, false, true, QColor(), QString(), 0.0,
                              ObjectId::invalid(), 0, nullptr,
                              duplicateTool_.previewPlacementTranslations().value(index));
                }
            }
        }

        if (isEraseLikeTool(activeTool_) &&
            (!eraseLikeScreenPath().isEmpty() || eraseTool_.strokeActive())) {
            for (const ObjectId objectId : eraseCandidates()) {
                const int shapeIndex = objectIndex(objectId);
                if (shapeIndex >= 0) {
                    drawEraseCandidatePreview(painter, shapeIndex);
                }
            }
        }

        const int subdivisionIndex = objectIndex(subdivisionTool_.targetObjectId());
        if (subdivisionTool_.isActive() && subdivisionIndex >= 0) {
            drawSubdivisionPoints(painter,
                                  shapes_[subdivisionIndex],
                                  subdivisionTool_.previewParameters(),
                                  true);
        }

        if (controlPointsVisible_) {
            for (const int shapeIndex : controlPointShapeIndices()) {
                if (shapeIndex >= 0 && shapeIndex < shapes_.size()) {
                    const ObjectId objectId = shapes_.objectIdAt(shapeIndex);
                    const ViewportRenderObject *renderObject =
                        renderFrame.find(objectId);
                    if (renderObject != nullptr) {
                        drawControlPoints(painter,
                                          *renderObject,
                                          !gpuControlPointsDrawn);
                    }
                }
            }
        }

        if (activeTool_ == Tool::Picture && pendingPoints_.size() == 1 &&
            cursorValid_ && !pendingPictureImage_.isNull()) {
            const Shape picturePreview = pictureShapeForCorner(cursorWorld_);
            if (picturePreview.points.size() == 4 &&
                (!gpuActivePicturePreviewRendered || !gpuPreviewRendered)) {
                drawShape(painter, picturePreview, true, false, false);
            }
        } else if (activeTool_ == Tool::TangentFromCurve) {
            drawLineToolPreview(painter,
                                !gpuActiveToolPreview || !gpuPreviewRendered);
            if (!pendingPoints_.isEmpty()) {
                drawSnapMarker(painter, SnapType::Tangent, pendingPoints_.first());
            }
        } else if (activeTool_ == Tool::PerpendicularFromCurve) {
            drawLineToolPreview(painter,
                                !gpuActiveToolPreview || !gpuPreviewRendered);
            if (!pendingPoints_.isEmpty()) {
                drawSnapMarker(painter,
                               SnapType::Perpendicular,
                               pendingPoints_.first());
            }
        } else if (activeTool_ == Tool::Line && lineCommandActive_) {
            drawLineToolPreview(painter,
                                !gpuActiveToolPreview || !gpuPreviewRendered);
        } else if (activeTool_ == Tool::Mirror) {
            drawMirrorToolPreview(
                painter,
                gpuPreviewRendered ? gpuMirrorPreviewHandled : QVector<ObjectId>{});
            drawLineToolPreview(painter,
                                !gpuActiveToolPreview || !gpuPreviewRendered);
        } else if (activeTool_ == Tool::Arc) {
            if (!gpuArcOverlayRendered) {
                drawArcToolPreview(painter,
                                   !gpuActiveToolPreview || !gpuPreviewRendered,
                                   arcPreviewColor);
            }
        } else if (isCircleConstructionTool(activeTool_)) {
            drawCircleToolPreview(painter,
                                  !gpuActiveToolPreview || !gpuPreviewRendered);
        } else if (isCircleTangentTool(activeTool_)) {
            drawCircleTangentToolPreview(painter,
                                         !gpuActiveToolPreview ||
                                             !gpuPreviewRendered);
        } else if (isEllipseTool(activeTool_)) {
            drawEllipseToolPreview(painter,
                                   !gpuActiveToolPreview || !gpuPreviewRendered);
        } else if (isRectangleTool(activeTool_)) {
            drawRectangleToolPreview(painter,
                                     !gpuActiveToolPreview ||
                                         !gpuPreviewRendered);
        } else if (isPolygonTool(activeTool_)) {
            drawPolygonToolPreview(painter,
                                   !gpuActiveToolPreview || !gpuPreviewRendered);
        } else if (activeTool_ == Tool::Point) {
            drawPointToolPreview(painter,
                                 !gpuActiveToolPreview || !gpuPreviewRendered);
        } else if (isPointCreationTool(activeTool_)) {
            drawPointConstructionToolPreview(
                painter, !gpuActiveToolPreview || !gpuPreviewRendered);
        } else if (activeTool_ == Tool::Rotate) {
            drawRotateToolPreview(painter,
                                  !gpuActiveToolPreview || !gpuPreviewRendered);
        } else if (activeTool_ == Tool::Scale && scaleState().stage == 2) {
            drawScaleToolGuide(painter,
                               !gpuActiveToolPreview || !gpuPreviewRendered);
        } else if (activeTool_ == Tool::PointExtrude) {
            for (const Shape &previewShape : activeToolPreview.shapes) {
                const bool surfacePreview =
                    previewShape.geometryType == GeometryType::NurbsSurface ||
                    previewShape.geometryType == GeometryType::NurbsSolid;
                if (surfacePreview && !gpuOpaqueExtrudePreviewRendered) {
                    const Layer *previewLayer =
                        document_.layer(document_.activeLayerId());
                    drawShape(painter, previewShape, false, false, false,
                              previewLayer != nullptr ? previewLayer->color
                                                      : QColor());
                } else if (!surfacePreview &&
                           (!gpuActiveToolPreview || !gpuPreviewRendered)) {
                    drawShape(painter, previewShape, true, false, false,
                              arcPreviewColor);
                }
            }
        } else if (activeToolPreview.hasShape &&
                   (isPointCreationTool(activeTool_) ||
                    isCurveCreationTool(activeTool_) ||
                    isTwoCurveLineTool(activeTool_))) {
            if (!gpuActiveToolPreview || !gpuPreviewRendered) {
                drawShape(painter, activeToolPreview.shape, true);
            }
        } else if (isDimensionTool(activeTool_)) {
            if (activeToolPreview.hasShape) {
                drawShape(painter, activeToolPreview.shape, true);
            }
            if (currentSnap_.isValid()) {
                drawSnapMarker(painter, currentSnap_);
            }
        } else if (isEraseLikeTool(activeTool_)) {
            drawErasePreview(painter);
        } else if (!pendingPoints_.isEmpty()) {
            Shape previewShape;
            previewShape.geometryType = geometryTypeForTool(activeTool_);
            previewShape.points = pendingPoints_;
            previewShape.workPlane = viewportTransform_.workPlane();
            previewShape.workPlaneOffset = viewportTransform_.workPlaneOffset();
            previewShape.workPlaneFrame = viewportTransform_.workPlaneFrame();
            if (!gpuActiveToolPreview || !gpuPreviewRendered) {
                drawShape(painter, previewShape, true);
            }
        }

        if ((grabTool_.isActive() || duplicateTool_.isActive() ||
             activeTool_ == Tool::Scale ||
             activeTool_ == Tool::PointExtrude ||
             activeTool_ == Tool::Picture) &&
            currentSnap_.isValid()) {
            drawSnapMarker(painter, currentSnap_);
        }
        if ((objectSelectionDragActive() || controlPointSelectionDragActive()) &&
            currentDragSnap_.isValid()) {
            drawSnapMarker(painter,
                           currentDragSnap_.type,
                           currentDragSnap_.targetPoint);
        }

        if (selectionBoxOverlayActive()) {
            viewportOverlay_.drawSelectionBox(painter,
                                              selectionBoxStartPosition(),
                                              selectionBoxCurrentPosition());
        }
        ViewportHudState hudState;
        hudState.activeTool = activeTool_;
        hudState.arcMode = arcState().mode;
        hudState.subdivisionActive = subdivisionTool_.isActive();
        hudState.subdivisionSections = subdivisionTool_.sections();
        hudState.joinActive = joinTool_.isActive();
        hudState.joinCount = joinTool_.selectedCount();
        hudState.lineCommandActive = lineCommandActive_;
        hudState.lineCommandStatus = toolStatus_.text;
        hudState.pointToolInstructions = activeToolPreview.hudInstructionsLine;
        hudState.rotateStep = rotateState().stage;
        hudState.rotateAngleSnapEnabled = rotateState().angleSnapEnabled;
        hudState.rotateAngleInputActive = rotateState().angleInputActive;
        hudState.rotateAngleSnapIncrementDegrees =
            rotateSnapIncrementDegrees();
        hudState.rotateAngleInputInRadians = rotateToolPreferences_.useRadians;
        hudState.grabActive = grabTool_.isActive();
        hudState.grabPickingBasePoint = grabTool_.isPickingBasePoint();
        hudState.grabHasBasePoint = grabTool_.hasBasePoint();
        hudState.duplicateActive = duplicateTool_.isActive();
        hudState.duplicatePickingBasePoint = duplicateTool_.isPickingBasePoint();
        hudState.duplicateHasBasePoint = duplicateTool_.hasBasePoint();
        viewportHudRenderer_.draw(painter, size(), hudState);
        navigationGizmo_.draw(
            painter, size(), navigationController_.hoverPosition());
        drawViewportShadingControls(painter);
        drawComponentModeControls(painter);
    }

    void dispatchSyntheticMouseEvent(QEvent::Type type,
                                     const QPointF &position,
                                     Qt::MouseButton button,
                                     Qt::MouseButtons buttons,
                                     Qt::KeyboardModifiers modifiers)
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        QMouseEvent translated(type, position, position, button, buttons, modifiers);
#else
        QMouseEvent translated(type, position, button, buttons, modifiers);
#endif
        if (type == QEvent::MouseButtonPress) {
            mousePressEvent(&translated);
        } else if (type == QEvent::MouseButtonRelease) {
            mouseReleaseEvent(&translated);
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        navigationController_.stopAnimation();
        const QPointF screenPosition = eventPosition(event);
        if (event->button() == Qt::LeftButton) {
            const int componentMode = componentModeControlAt(screenPosition);
            if (componentMode >= 0) {
                setComponentSelectionMode(
                    static_cast<ComponentSelectionMode>(componentMode));
                event->accept();
                return;
            }
            const int shadingControl = viewportShadingControlAt(screenPosition);
            if (shadingControl >= 0) {
                shadingControlPressed_ = shadingControl;
                shadingControlHover_ = shadingControl;
                event->accept();
                update();
                return;
            }
            shadingControlPressed_ = -1;
        }
        if (event->button() == Qt::LeftButton &&
            navigationController_.handleGizmoPress(screenPosition, size())) {
            event->accept();
            return;
        }
        if (event->button() == Qt::RightButton &&
            navigationController_.panButton() != Qt::RightButton &&
            activeTool_ == Tool::Select && !grabTool_.isActive() &&
            !duplicateTool_.isActive() && !subdivisionTool_.isActive() &&
            !joinTool_.isActive()) {
            rightButtonSelectionGestureActive_ = true;
            dispatchSyntheticMouseEvent(QEvent::MouseButtonPress,
                                        screenPosition,
                                        Qt::LeftButton,
                                        Qt::LeftButton,
                                        event->modifiers());
            event->accept();
            return;
        }
        updateDrawingWorkPlaneFromHover(screenPosition);
        if (activeTool_ == Tool::Arc &&
            arcTool_.inputStage() == ArcInputStage::Complete &&
            arcState().mode != ArcMode::OnePoint) {
            updateArcTwoPointWorkPlaneForView();
        } else if (activeTool_ == Tool::Arc &&
                   arcTool_.inputStage() == ArcInputStage::SecondPoint &&
                   arcState().mode != ArcMode::OnePoint &&
                   !(arcState().mode == ArcMode::TwoPoint &&
                     arcState().perpendicularPlaneActive)) {
            restoreArcChordReferencePlaneForEndpointPick();
        }
        QPointF rawWorldPosition;
        const bool worldPositionValid = viewportTransform_.screenToWorkPlane(
            screenPosition,
            size(),
            viewportTransform_.workPlaneFrame(),
            &rawWorldPosition);
        if (!worldPositionValid) {
            rawWorldPosition = {};
        }
        const QPointF worldPosition = worldPositionValid
            ? constrainLinePoint(
                  rawWorldPosition,
                  event->modifiers().testFlag(Qt::AltModifier),
                  &screenPosition)
            : rawWorldPosition;
        if (!worldPositionValid) currentSnap_ = SnapResult{};
        DebugLog::instance().write(
            QStringLiteral("mousePress button=%1 screen=%2 worldRaw=%3 worldUsed=%4 tool=%5 lineActive=%6 ortho=%7 panButton=%8 modifiers=0x%9 snap=%10")
                .arg(inputButtonName(event->button()))
                .arg(pointText(screenPosition))
                .arg(pointText(rawWorldPosition))
                .arg(pointText(worldPosition))
                .arg(toolName(activeTool_))
                .arg(lineCommandActive_)
                .arg(orthoEnabled_)
                .arg(inputButtonName(navigationController_.panButton()))
                .arg(static_cast<int>(event->modifiers()), 0, 16)
                .arg(snapTypeName(currentSnap_.type)));

        if (!worldPositionValid && event->button() == Qt::LeftButton &&
            event->button() != navigationController_.panButton() &&
            !(activeTool_ == Tool::Line && !pendingPoints_.isEmpty()) &&
            activeTool_ != Tool::PointExtrude &&
            activeTool_ != Tool::Select) {
            event->ignore();
            return;
        }

        const ToolInput translatedInput = ToolInputTranslator::fromMouseEvent(
            *event, activeTool_, screenPosition, rawWorldPosition,
            worldPosition, viewportTransform_.workPlaneFrame(),
            orthoEnabled_, size(), currentSnap_);

        if (duplicateTool_.isActive()) {
            rawCursorWorld_ = rawWorldPosition;
            cursorWorld_ = rawWorldPosition;
            lastWorldPosition_ = rawWorldPosition;
            cursorValid_ = true;
            if (event->button() == Qt::LeftButton) {
                if (duplicateTool_.isPickingBasePoint()) {
                    const SnapResult baseSnap = findDuplicateBasePointSnap(rawWorldPosition);
                    duplicateTool_.chooseBasePoint(
                        rawWorldPosition,
                        baseSnap.isValid() ? baseSnap.point : rawWorldPosition);
                    currentSnap_ = baseSnap;
                    setCursor(Qt::SizeAllCursor);
                    updateDuplicatePreview(rawWorldPosition);
                } else {
                    updateDuplicatePreview(rawWorldPosition);
                    finishDuplicate();
                }
                update();
                emitCoordinateUpdate();
            } else if (event->button() == Qt::RightButton) {
                cancelDuplicate();
            }
            return;
        }

        if (grabTool_.isActive()) {
            rawCursorWorld_ = rawWorldPosition;
            cursorWorld_ = rawWorldPosition;
            lastWorldPosition_ = rawWorldPosition;
            cursorValid_ = true;
            if (event->button() == Qt::LeftButton) {
                if (grabTool_.isPickingBasePoint()) {
                    const SnapResult baseSnap = findGrabBasePointSnap(rawWorldPosition);
                    if (baseSnap.isValid()) {
                        const QPointF cursorOffset = rawWorldPosition - baseSnap.point;
                        const Point3D basePointWorld = baseSnap.hasWorldPoint
                            ? baseSnap.worldPoint
                            : workPlaneFramePointToWorld(
                                  baseSnap.point,
                                  viewportTransform_.workPlaneFrame());
                        QPointF basePointScreen = worldToScreen(baseSnap.point);
                        viewportTransform_.worldPointToScreen(
                            basePointWorld, size(), &basePointScreen);
                        const QPointF cursorOffsetScreen =
                            screenPosition - basePointScreen;
                        QPointF basePointDragPlane = baseSnap.point;
                        viewportTransform_.screenToWorkPlaneUnclipped(
                            basePointScreen,
                            size(),
                            viewportTransform_.workPlaneFrame(),
                            &basePointDragPlane);
                        grabTool_.acceptBasePoint(baseSnap.point,
                                                  cursorOffset,
                                                  basePointWorld,
                                                  basePointDragPlane,
                                                  cursorOffsetScreen);
                        grabAxisStartScreen_ = screenPosition;
                        currentSnap_ = baseSnap;
                        setCursor(Qt::SizeAllCursor);
                        DebugLog::instance().write(
                            QStringLiteral("grab base-point selected type=%1 point=%2 world=%3 offset=%4 offsetPx=%5")
                                .arg(snapTypeName(baseSnap.type))
                                .arg(pointText(grabTool_.basePoint()))
                                .arg(QStringLiteral("(%1,%2,%3)")
                                         .arg(basePointWorld.x, 0, 'g', 12)
                                         .arg(basePointWorld.y, 0, 'g', 12)
                                         .arg(basePointWorld.z, 0, 'g', 12))
                                .arg(pointText(cursorOffset))
                                .arg(pointText(cursorOffsetScreen)));
                    } else {
                        DebugLog::instance().write(
                            QStringLiteral("grab base-point click ignored no snap candidate"));
                    }
                    update();
                    emitCoordinateUpdate();
                } else {
                    if (grabTool_.hasBasePoint()) {
                        updateGrabPosition(selectionDraggedObjectIds(),
                                           screenPosition);
                    }
                    finishGrab();
                }
            } else if (event->button() == Qt::RightButton) {
                cancelGrab();
            }
            return;
        }

        if (subdivisionTool_.isActive()) {
            switch (subdivisionTool_.handleMousePress(translatedInput)) {
            case SubdivisionInputAction::Apply:
                applySubdivision(subdivisionTool_.sections());
                break;
            case SubdivisionInputAction::Cancel:
                cancelSubdivisionPreview();
                break;
            case SubdivisionInputAction::Unhandled:
                break;
            }
            return;
        }

        // While drawing a connected line, right-click is the command's
        // finish action. This takes priority over right-button panning.
        if (event->button() == Qt::RightButton && activeTool_ == Tool::Line &&
            lineCommandActive_) {
            DebugLog::instance().write(QStringLiteral("mousePress branch=finish-line points=%1")
                                           .arg(pendingPoints_.size()));
            if (activeToolController_ == nullptr ||
                activeToolController_->dispatchMousePress(translatedInput,
                                                          toolContext_) ==
                    InteractionTool::EventResult::Unhandled) {
                finishLineCommand();
            }
            return;
        }

        if (event->button() == Qt::RightButton &&
            (activeTool_ == Tool::PointByLine ||
             activeTool_ == Tool::PointByArcs ||
             activeTool_ == Tool::CurveInterpolate ||
             activeTool_ == Tool::CurveFreehand ||
             isRectangleTool(activeTool_) ||
             isTwoCurveLineTool(activeTool_) ||
             activeTool_ == Tool::PointExtrude) &&
            activeToolController_ != nullptr) {
            activeToolController_->dispatchMousePress(translatedInput,
                                                      toolContext_);
            return;
        }

        if (event->button() == Qt::RightButton && isEraseLikeTool(activeTool_)) {
            exitEraseLikeTool();
            return;
        }

        if (event->button() == Qt::RightButton &&
            (activeTool_ == Tool::Rotate || activeTool_ == Tool::Scale ||
             activeTool_ == Tool::Mirror) &&
            activeToolController_ != nullptr) {
            const Tool dispatchedTool = activeTool_;
            if (activeToolController_->dispatchMousePress(translatedInput,
                                                          toolContext_) ==
                InteractionTool::EventResult::Handled) {
                if (dispatchedTool == Tool::Scale) {
                    scaleTool_.takeLastDispatchResult();
                } else if (dispatchedTool == Tool::Mirror) {
                    mirrorTool_.takeLastCommitResult();
                }
                currentSnap_ = SnapResult{};
                DebugLog::instance().write(
                    QStringLiteral("%1 canceled")
                        .arg(toolName(dispatchedTool).toLower()));
                update();
            }
            return;
        }

        if (event->button() == Qt::RightButton && activeTool_ == Tool::Picture) {
            cancelPicturePlacement();
            return;
        }

        if (event->button() == Qt::RightButton && activeTool_ == Tool::Arc) {
            if (worldPositionValid) {
                cursorWorld_ = worldPosition;
                lastWorldPosition_ = worldPosition;
                cursorValid_ = true;
            }
            dispatchArcClick(translatedInput, worldPositionValid);
            update();
            emitCoordinateUpdate();
            return;
        }

        if (navigationController_.handlePanPress(event->button(),
                                                 event->modifiers(),
                                                 screenPosition,
                                                 size())) {
            lastMousePosition_ = screenPosition.toPoint();
            DebugLog::instance().write(
                QStringLiteral("mousePress branch=start-%1 at=%2")
                    .arg(event->button() == Qt::MiddleButton &&
                                 !event->modifiers().testFlag(Qt::ShiftModifier)
                             ? QStringLiteral("orbit")
                             : QStringLiteral("pan"))
                    .arg(pointText(screenPosition)));
            return;
        }

        if (event->button() == Qt::LeftButton && activeToolController_ != nullptr &&
            activeTool_ != Tool::Select && activeTool_ != Tool::Arc) {
            const Tool dispatchedTool = activeTool_;
            rawCursorWorld_ = rawWorldPosition;
            lastWorldPosition_ = worldPosition;
            cursorWorld_ = worldPosition;
            cursorValid_ = true;
            if (activeToolController_->dispatchMousePress(translatedInput,
                                                          toolContext_) ==
                InteractionTool::EventResult::Handled) {
                if (dispatchedTool == Tool::Mirror) {
                    const MirrorCommitResult result =
                        mirrorTool_.takeLastCommitResult();
                    if (result.sourceCount > 0) {
                        const QString message = result.committed
                                                    ? QStringLiteral("mirror committed sourceCount=%1 copyCount=%2 axis=%3->%4")
                                                          .arg(result.sourceCount)
                                                          .arg(result.createdObjectIds.size())
                                                          .arg(pointText(result.axisStart),
                                                               pointText(result.axisEnd))
                                                    : QStringLiteral("mirror rejected axis=%1->%2 objects=%3")
                                                          .arg(pointText(result.axisStart),
                                                               pointText(result.axisEnd))
                                                          .arg(result.sourceCount);
                        DebugLog::instance().write(message);
                    }
                } else if (dispatchedTool == Tool::Scale) {
                    const ScaleDispatchResult result =
                        scaleTool_.takeLastDispatchResult();
                    if (result.point.action ==
                        ScalePointAction::BasePointCaptured) {
                        pendingPoints_ = {result.point.point};
                        publishScalePrompt();
                    } else if (result.point.action ==
                               ScalePointAction::ReferenceCaptured) {
                        publishScalePrompt();
                    } else if (result.commitAttempted && result.committed) {
                        currentSnap_ = SnapResult{};
                        const bool changed =
                            std::abs(result.point.factor - 1.0) > 1.0e-12;
                        DebugLog::instance().write(
                            QStringLiteral("scale committed mode=%1 factor=%2 base=%3 objects=%4 changed=%5")
                                .arg(scaleModeName(result.mode))
                                .arg(result.point.factor, 0, 'g', 10)
                                .arg(pointText(result.basePoint))
                                .arg(result.sourceCount)
                                .arg(changed));
                    }
                }
                update();
                emitCoordinateUpdate();
                return;
            }
        }

        if (event->button() == Qt::LeftButton && activeTool_ == Tool::Rotate) {
            rawCursorWorld_ = rawWorldPosition;
            cursorWorld_ = worldPosition;
            lastWorldPosition_ = worldPosition;
            cursorValid_ = true;
            handleRotatePoint(translatedInput);
            emitCoordinateUpdate();
            return;
        }

        if (joinTool_.isActive() && event->button() == Qt::LeftButton) {
            const int shapeIndex = hitTestShape(screenPosition);
            const ObjectId objectId = shapes_.objectIdAt(shapeIndex);
            const bool joinable = shapeIndex >= 0 &&
                                  shapeIndex < shapes_.size() &&
                                  isJoinableShape(shapes_[shapeIndex]);
            const JoinClickAction action = joinTool_.handleObjectClick(
                objectId, joinable);
            if (action == JoinClickAction::InvalidTarget) {
                notifyJoinStatus(QStringLiteral("Join: click a line or curve"));
                DebugLog::instance().write(QStringLiteral("join click ignored shape=%1")
                                               .arg(shapeIndex));
                return;
            }
            if (action == JoinClickAction::AlreadySelected) {
                notifyJoinStatus(QStringLiteral("Join: curve already selected"));
                return;
            }
            if (action != JoinClickAction::Added &&
                action != JoinClickAction::ReadyToJoin) {
                return;
            }
            if (!selectedShapeIndices_.contains(objectId)) {
                selection_.add(objectId);
            }
            selection_.setPrimaryObjectId(objectId);
            DebugLog::instance().write(QStringLiteral("join selected shape=%1 total=%2")
                                           .arg(shapeIndex)
                                           .arg(joinTool_.selectedCount()));
            update();
            if (action == JoinClickAction::ReadyToJoin) {
                applyJoin();
            } else {
                notifyJoinStatus();
            }
            return;
        }

        if (event->button() == Qt::LeftButton && activeTool_ == Tool::Erase) {
            prepareEraseGeometryCache();
            if (eraseTargetShapeIndices_.isEmpty()) {
                DebugLog::instance().write(
                    QStringLiteral("erase stroke ignored no selected curve targets"));
                update();
                return;
            }

            eraseTool_.beginStroke(screenPosition);
            rawCursorWorld_ = rawWorldPosition;
            cursorWorld_ = rawWorldPosition;
            lastWorldPosition_ = rawWorldPosition;
            cursorValid_ = true;
            eraseTool_.clearCandidates();
            eraseAlongScreenSegment(screenPosition, screenPosition);
            updateErasePreviewIntervals(false);
            setCursor(Qt::CrossCursor);
            DebugLog::instance().write(QStringLiteral("erase stroke start screen=%1")
                                           .arg(pointText(screenPosition)));
            update();
            emitCoordinateUpdate();
            return;
        }

        if (event->button() == Qt::LeftButton && activeTool_ == Tool::Trim) {
            eraseTool_.setCursorPressed(false);
            rawCursorWorld_ = rawWorldPosition;
            cursorWorld_ = rawWorldPosition;
            lastWorldPosition_ = rawWorldPosition;
            cursorValid_ = true;
            trimTool_.clearCandidates();
            trimTool_.clearScreenPath();
            trimTool_.invalidateHover();
            beginSelectionBox(screenPosition, false);
            return;
        }

        if (event->button() == Qt::LeftButton && activeTool_ == Tool::Select) {
            int componentShapeIndex = -1;
            const int componentIndex = hitTestComponent(screenPosition,
                                                       &componentShapeIndex);
            if (componentIndex >= 0 && componentShapeIndex >= 0) {
                const ObjectId componentObject =
                    shapes_.objectIdAt(componentShapeIndex);
                if (!selection_.contains(componentObject)) {
                    selection_.setObjectIds({componentObject}, componentObject);
                } else {
                    selection_.setPrimaryObjectId(componentObject);
                }
                if (componentSelectionObject_ != componentObject ||
                    !event->modifiers().testFlag(Qt::ShiftModifier)) {
                    if (componentSelectionObject_ != componentObject)
                        clearComponentSelections();
                    else {
                        activeComponentSelection().clear();
                        activeComponentIndex() = -1;
                    }
                }
                componentSelectionObject_ = componentObject;
                if (activeComponentSelection().contains(componentIndex)) {
                    activeComponentSelection().remove(componentIndex);
                    if (activeComponentIndex() == componentIndex) {
                        activeComponentIndex() = activeComponentSelection().isEmpty()
                                                     ? -1
                                                     : *activeComponentSelection().cbegin();
                    }
                } else {
                    activeComponentSelection().insert(componentIndex);
                    activeComponentIndex() = componentIndex;
                }
                update();
                event->accept();
                return;
            }
            const int hitShapeIndex = hitTestShape(screenPosition);
            if (componentSelectionMode_ == ComponentSelectionMode::Vertex ||
                componentSelectionMode_ == ComponentSelectionMode::Edge) {
                ObjectId boxObject = componentSelectionObject_;
                if (!boxObject.isValid() || objectIndex(boxObject) < 0) {
                    boxObject = selectedShapeIndex_;
                }
                if ((!boxObject.isValid() || objectIndex(boxObject) < 0) &&
                    hitShapeIndex >= 0 && hitShapeIndex < shapes_.size()) {
                    boxObject = shapes_.objectIdAt(hitShapeIndex);
                }
                const int boxShapeIndex = objectIndex(boxObject);
                const bool isSurfaceObject =
                    boxShapeIndex >= 0 &&
                    (shapes_[boxShapeIndex].geometryType == GeometryType::NurbsSurface ||
                     shapes_[boxShapeIndex].geometryType == GeometryType::NurbsSolid);
                // Vertex and edge modes route every remaining click/drag
                // through component selection. With no active object, the box
                // finish searches eligible surface cages for enclosed items.
                componentBoxSelectionActive_ = true;
                componentBoxSelectionObject_ = isSurfaceObject
                                                   ? boxObject
                                                   : ObjectId::invalid();
                componentBoxStartedOnBlank_ = hitShapeIndex < 0;
                beginSelectionBox(
                    screenPosition,
                    event->modifiers().testFlag(Qt::ShiftModifier));
                event->accept();
                return;
            }
            if (hitShapeIndex >= 0 && hitShapeIndex < shapes_.size()) {
                const GeometryType type = shapes_[hitShapeIndex].geometryType;
                if (type == GeometryType::NurbsSurface ||
                    type == GeometryType::NurbsSolid) {
                    // Edge and face mode do not convert empty body clicks to
                    // object selection either; component hits are handled above.
                    event->accept();
                    return;
                }
            }
            clearComponentSelections();
            selectionDragViewPlaneAnchorValid_ = false;
            dragSnapViewPlaneAnchorValid_ = false;
            rawCursorWorld_ = rawWorldPosition;
            cursorWorld_ = rawWorldPosition;
            lastWorldPosition_ = rawWorldPosition;
            cursorValid_ = true;
            InteractionTool *selectionTool = activeToolController_ != nullptr
                                                 ? activeToolController_
                                                 : toolRegistry_.find(Tool::Select);
            if (selectionTool != nullptr &&
                selectionTool->dispatchMousePress(translatedInput,
                                                  toolContext_) ==
                    InteractionTool::EventResult::Handled) {
                if (objectSelectionDragActive()) {
                    beginSelectionViewPlaneDrag(
                        selectionDraggedObjectIds(), screenPosition);
                }
                update();
                emitCoordinateUpdate();
            }
            return;
        }

        if (event->button() != Qt::LeftButton || activeTool_ == Tool::Select) {
            DebugLog::instance().write(QStringLiteral("mousePress branch=ignored"));
            return;
        }

        QPointF rawInCurrentFrame;
        rawCursorWorld_ = viewportTransform_.screenToWorkPlane(
                              screenPosition,
                              size(),
                              viewportTransform_.workPlaneFrame(),
                              &rawInCurrentFrame)
                          ? rawInCurrentFrame
                          : rawWorldPosition;
        lastWorldPosition_ = worldPosition;
        cursorWorld_ = worldPosition;
        cursorValid_ = true;

        if (activeTool_ == Tool::Arc) {
            dispatchArcClick(translatedInput, worldPositionValid);
            update();
            emitCoordinateUpdate();
            return;
        }

        if (activeTool_ == Tool::Picture) {
            if (pendingPictureImage_.isNull()) {
                cancelPicturePlacement();
                return;
            }
            if (pendingPoints_.isEmpty()) {
                pendingPoints_.append(worldPosition);
                DebugLog::instance().write(
                    QStringLiteral("picture first corner=%1 image=%2x%3")
                        .arg(pointText(worldPosition))
                        .arg(pendingPictureImage_.width())
                        .arg(pendingPictureImage_.height()));
            } else {
                Shape picture = pictureShapeForCorner(worldPosition);
                if (picture.points.size() == 4 &&
                    std::hypot(picture.points[1].x() - picture.points[0].x(),
                               picture.points[1].y() - picture.points[0].y()) > 1.0e-9 &&
                    std::hypot(picture.points[2].x() - picture.points[1].x(),
                               picture.points[2].y() - picture.points[1].y()) > 1.0e-9) {
                    if (!commitShape(picture)) {
                        return;
                    }
                    DebugLog::instance().write(
                        QStringLiteral("picture committed image=%1 frameCorners=%2")
                            .arg(pendingPicturePath_)
                            .arg(picture.points.size()));
                    setTool(Tool::Select);
                    if (commandFinished_) {
                        commandFinished_(Tool::Select);
                    }
                }
            }
            update();
            emitCoordinateUpdate();
            return;
        }

        if (activeTool_ == Tool::Line) {
            pendingPoints_.append(lastWorldPosition_);
            DebugLog::instance().write(QStringLiteral("line point planted index=%1 world=%2 total=%3")
                                           .arg(pendingPoints_.size() - 1)
                                           .arg(pointText(lastWorldPosition_))
                                           .arg(pendingPoints_.size()));
            update();
            return;
        }

        if (pendingPoints_.size() == requiredPoints(activeTool_)) {
            Shape completedShape;
            completedShape.geometryType = geometryTypeForTool(activeTool_);
            completedShape.points = pendingPoints_;
            completedShape.workPlane = viewportTransform_.workPlane();
            completedShape.workPlaneOffset = viewportTransform_.workPlaneOffset();
            completedShape.workPlaneFrame = viewportTransform_.workPlaneFrame();
            if (isRectangleTool(activeTool_)) {
                completedShape.points = makeRectanglePoints(
                    rectangleModeForTool(activeTool_), pendingPoints_);
            } else if (isPolygonTool(activeTool_)) {
                completedShape.points = makeRegularPolygonPoints(
                    polygonModeForTool(activeTool_), pendingPoints_, polygonSideCount_);
                if (completedShape.points.size() >= 3) {
                    QVector<QPointF> closedPoints = completedShape.points;
                    closedPoints.append(closedPoints.first());
                    completedShape.nurbs = makeDegreeOneNurbs(closedPoints);
                }
            }
            if (activeTool_ == Tool::Bezier || activeTool_ == Tool::Nurbs) {
                completedShape.nurbs = makeBezierNurbs(completedShape.points);
            } else if (activeTool_ == Tool::Circle) {
                completedShape.nurbs = makeCircleNurbs(completedShape.points);
            }
            if (!commitShape(completedShape)) {
                pendingPoints_.clear();
                return;
            }
            QString commitMessage = QStringLiteral("placeholder shape committed tool=%1 points=%2")
                                        .arg(toolName(activeTool_))
                                        .arg(pendingPoints_.size());
            if (activeTool_ == Tool::Bezier || activeTool_ == Tool::Nurbs ||
                activeTool_ == Tool::Circle) {
                commitMessage += QStringLiteral(" nurbsDimension=%1 nurbsDegree=%2 nurbsOrder=%3 rational=%4 controlPoints=%5 weights=%6 knots=%7")
                                     .arg(completedShape.nurbs.dimension)
                                     .arg(completedShape.nurbs.degree)
                                     .arg(completedShape.nurbs.order)
                                     .arg(completedShape.nurbs.rational)
                                     .arg(completedShape.nurbs.controlPoints.size())
                                     .arg(completedShape.nurbs.weights.size())
                                     .arg(completedShape.nurbs.knots.size());
            }
            commitMessage += QStringLiteral(" shapes=%1").arg(shapes_.size());
            DebugLog::instance().write(commitMessage);
            pendingPoints_.clear();

            setTool(Tool::Select);
            if (commandFinished_) {
                commandFinished_(Tool::Select);
            }
        }

        update();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const QPointF screenPosition = eventPosition(event);
        const QPointF pickPosition(qRound(screenPosition.x()),
                                   qRound(screenPosition.y()));
        for (ComponentPickCycleState &cycleState : componentPickCycleStates_) {
            if (cycleState.valid && cycleState.position != pickPosition) {
                cycleState.valid = false;
            }
        }
        const int componentMode = componentModeControlAt(screenPosition);
        if (componentMode >= 0) {
            if (componentMode != componentModeHover_) {
                componentModeHover_ = componentMode;
                const QStringList tips{QStringLiteral("Vertex Select (1)"),
                                       QStringLiteral("Edge Select (2)"),
                                       QStringLiteral("Face Select (3)")};
                setToolTip(tips[componentMode]);
                update();
            }
            event->accept();
            return;
        }
        if (componentModeHover_ >= 0) {
            componentModeHover_ = -1;
            setToolTip(QString());
            update();
        }
        const int shadingControl = viewportShadingControlAt(screenPosition);
        if (shadingControlPressed_ >= 0) {
            if (shadingControl != shadingControlHover_) {
                shadingControlHover_ = shadingControl;
                update();
            }
            event->accept();
            return;
        }
        if (shadingControl >= 0) {
            if (shadingControl != shadingControlHover_) {
                shadingControlHover_ = shadingControl;
                const QStringList tips{
                    QStringLiteral("X-Ray (Alt+Z)"),
                    QStringLiteral("Wireframe"),
                    QStringLiteral("Solid"),
                    QStringLiteral("Viewport Shading Settings")};
                setToolTip(tips[shadingControl]);
                update();
            }
            event->accept();
            return;
        }
        if (shadingControlHover_ >= 0) {
            shadingControlHover_ = -1;
            setToolTip(QString());
            update();
        }
        if (navigationController_.handleGizmoMove(screenPosition,
                                                  event->buttons(),
                                                  size())) {
            event->accept();
            return;
        }
        if (navigationController_.handlePanMove(screenPosition, size())) {
            lastMousePosition_ = screenPosition.toPoint();
            return;
        }
        if (navigationController_.updateGizmoHover(
                screenPosition,
                size(),
                !grabTool_.isActive() && !duplicateTool_.isActive())) {
            return;
        }
        eraseTool_.setCursorScreenPosition(screenPosition);
        updateDrawingWorkPlaneFromHover(screenPosition);
        if (activeTool_ == Tool::Arc &&
            arcTool_.inputStage() == ArcInputStage::Complete &&
            arcState().mode != ArcMode::OnePoint) {
            updateArcTwoPointWorkPlaneForView();
        } else if (activeTool_ == Tool::Arc &&
                   arcTool_.inputStage() == ArcInputStage::SecondPoint &&
                   arcState().mode != ArcMode::OnePoint &&
                   !(arcState().mode == ArcMode::TwoPoint &&
                     arcState().perpendicularPlaneActive)) {
            restoreArcChordReferencePlaneForEndpointPick();
        }
        const WorkPlaneFrame cursorWorkPlaneFrame =
            controlPointSelectionDragActive() && controlPointDragFrameValid_
                ? controlPointDragFrame_
                : viewportTransform_.workPlaneFrame();
        if (!viewportTransform_.screenToWorkPlane(screenPosition,
                                                  size(),
                                                  cursorWorkPlaneFrame,
                                                  &rawCursorWorld_)) {
            cursorValid_ = false;
            currentSnap_ = SnapResult{};
            if (activeTool_ == Tool::Select && objectSelectionDragActive()) {
                const QVector<ObjectId> dragIndices =
                    selectionDraggedObjectIds().isEmpty()
                        ? QVector<ObjectId>{selectedShapeIndex_}
                        : selectionDraggedObjectIds();
                if (grabTool_.isActive()) {
                    updateGrabPosition(dragIndices, screenPosition);
                } else if (selectionDragStarted() &&
                           dragAxisLock_ == DragAxisLock::Z) {
                    updateWorldZAxisDrag(dragIndices, screenPosition);
                } else {
                    updateSelectionObjectDragWithoutWorkPlane(dragIndices,
                                                              screenPosition);
                }
            }
            if (activeToolController_ != nullptr &&
                ((activeTool_ == Tool::Line && !pendingPoints_.isEmpty()) ||
                 activeTool_ == Tool::PointExtrude)) {
                const ToolInput translatedInput =
                    ToolInputTranslator::fromMouseEvent(
                        *event, activeTool_, screenPosition,
                        lastWorldPosition_, lastWorldPosition_,
                        viewportTransform_.workPlaneFrame(), orthoEnabled_,
                        size(), currentSnap_);
                activeToolController_->dispatchMouseMove(translatedInput,
                                                         toolContext_);
            }
            update();
            emitCoordinateUpdate();
            return;
        }
        cursorWorld_ = constrainLinePoint(
            rawCursorWorld_,
            event->modifiers().testFlag(Qt::AltModifier),
            &screenPosition);
        lastWorldPosition_ = cursorWorld_;
        cursorValid_ = true;
        if (activeToolController_ != nullptr && activeTool_ != Tool::Arc) {
            const ToolInput translatedInput =
                ToolInputTranslator::fromMouseEvent(
                    *event, activeTool_, screenPosition, rawCursorWorld_,
                    cursorWorld_, viewportTransform_.workPlaneFrame(),
                    orthoEnabled_, size(), currentSnap_);
            activeToolController_->dispatchMouseMove(translatedInput,
                                                     toolContext_);
        }
        const bool pointPreviewActive = activeTool_ == Tool::Point;
        const bool picturePreviewActive = activeTool_ == Tool::Picture &&
                                          pendingPoints_.size() == 1;
        const bool circlePreviewActive = isCircleConstructionTool(activeTool_) &&
                                         !pendingPoints_.isEmpty();
        const bool tangentCirclePreviewActive = isCircleTangentTool(activeTool_);
        const bool ellipsePreviewActive = isEllipseTool(activeTool_);
        const bool rectanglePreviewActive = isRectangleTool(activeTool_);
        const bool polygonPreviewActive = isPolygonTool(activeTool_);
        const bool arcPreviewActive = activeTool_ == Tool::Arc;
        const bool mirrorPreviewActive = activeTool_ == Tool::Mirror;

        if (!navigationController_.isPanning() && activeTool_ == Tool::Arc && arcState().mode == ArcMode::OnePoint &&
            pendingPoints_.size() >= 2) {
            updateArcPreviewTracking(cursorWorld_);
        }

        if (selectionBoxOverlayActive()) {
            updateSelectionBoxPosition(screenPosition);
            if (trimTool_.boxSelectionActive()) {
                updateTrimBoxPreview();
            }
            update();
            emitCoordinateUpdate();
            return;
        }

        if (eraseTool_.strokeActive()) {
            eraseAlongScreenSegment(eraseTool_.lastScreenPosition(),
                                    screenPosition);
            eraseTool_.appendStrokeScreenPosition(screenPosition);
            updateErasePreviewIntervals(false);
            update();
            emitCoordinateUpdate();
            return;
        }

        if (activeTool_ == Tool::Trim && !navigationController_.isPanning()) {
            updateTrimHover(screenPosition);
            update();
            emitCoordinateUpdate();
            return;
        }

        if (activeTool_ == Tool::Scale) {
            updateScalePreview(cursorWorld_);
            update();
            emitCoordinateUpdate();
            return;
        }

        if (activeTool_ == Tool::Rotate) {
            if (!navigationController_.isPanning() && rotateState().stage == 1) {
                updateRotateReferencePreview(cursorWorld_);
            } else if (!navigationController_.isPanning() && rotateState().stage == 2) {
                updateRotatePreview(cursorWorld_);
            }
            update();
            emitCoordinateUpdate();
            return;
        }

        if (activeTool_ == Tool::Mirror) {
            update();
            emitCoordinateUpdate();
            return;
        }

        if (duplicateTool_.isActive()) {
            if (duplicateTool_.isPickingBasePoint()) {
                currentSnap_ = findDuplicateBasePointSnap(rawCursorWorld_);
                cursorWorld_ = currentSnap_.isValid()
                                   ? currentSnap_.point
                                   : rawCursorWorld_;
                lastWorldPosition_ = cursorWorld_;
                update();
                emitCoordinateUpdate();
            } else {
                updateDuplicatePreview(rawCursorWorld_);
                emitCoordinateUpdate();
            }
            return;
        }

        if (grabTool_.isActive() && grabTool_.isPickingBasePoint()) {
            currentSnap_ = findGrabBasePointSnap(rawCursorWorld_);
            cursorWorld_ = currentSnap_.isValid() ? currentSnap_.point : rawCursorWorld_;
            lastWorldPosition_ = cursorWorld_;
            update();
            emitCoordinateUpdate();
            return;
        }

        if ((objectSelectionDragActive() || controlPointSelectionDragActive()) &&
            !selectionDragStarted()) {
            constexpr qreal dragStartThresholdPixels = 4.0;
            const QPointF screenDelta =
                screenPosition - selectionDragStartScreenPosition();
            if (std::hypot(screenDelta.x(), screenDelta.y()) >= dragStartThresholdPixels) {
                setSelectionDragStarted(true);
            }
        }

        const int selectedIndex = objectIndex(selectedShapeIndex_);
        if (controlPointSelectionDragActive() && selectionDragStarted() &&
            selectedIndex >= 0 &&
            controlPointIndex_ >= 0) {
            const QPointF cursorStepScreen =
                screenPosition - controlPointLastCursorScreen_;
            controlPointLastCursorScreen_ = screenPosition;
            const QVector<QPointF> controlPoints =
                controlPointsForShape(shapes_[selectedIndex]);
            const bool hasControlPoint =
                controlPointIndex_ < controlPoints.size();
            const QPointF currentControlPoint = hasControlPoint
                                                   ? controlPoints[controlPointIndex_]
                                                   : QPointF{};
            QPointF desiredControlPoint = currentControlPoint;
            const QPointF desiredControlPointScreen =
                screenPosition + controlPointCursorOffsetScreen_;
            const bool hasDesiredControlPoint =
                hasControlPoint && controlPointDragFrameValid_ &&
                viewportTransform_.screenToWorkPlane(
                    desiredControlPointScreen,
                    size(),
                    controlPointDragFrame_,
                    &desiredControlPoint);
            if (!hasDesiredControlPoint && hasControlPoint) {
                desiredControlPoint = currentControlPoint +
                    (rawCursorWorld_ -
                     selectionLastControlPointWorldPosition());
            }
            const QPointF delta = hasControlPoint
                                      ? desiredControlPoint - currentControlPoint
                                      : QPointF{};
            if (!qFuzzyIsNull(delta.x()) || !qFuzzyIsNull(delta.y())) {
                qint64 snapEvaluationMicroseconds = -1;
                const qreal cursorDistanceFromSnap =
                    std::hypot(screenPosition.x() - dragSnapCursorScreen_.x(),
                               screenPosition.y() - dragSnapCursorScreen_.y());

                if (dragSnapLocked_ &&
                    cursorDistanceFromSnap <= kDragSnapBreakawayPixels) {
                    DebugLog::instance().write(
                        QStringLiteral("control point snap-hold shape=%1 index=%2 cursorDistance=%3 breakaway=%4")
                            .arg(selectedIndex)
                            .arg(controlPointIndex_)
                            .arg(cursorDistanceFromSnap, 0, 'f', 2)
                            .arg(kDragSnapBreakawayPixels, 0, 'f', 2));
                } else if (dragSnapLocked_) {
                    beginDragHistory();
                    translateControlPoint(selectedShapeIndex_,
                                          controlPointIndex_,
                                          delta);
                    currentDragSnap_ = DragSnapResult{};
                    dragSnapLocked_ = false;
                    DebugLog::instance().write(
                        QStringLiteral("control point snap-breakaway shape=%1 index=%2 cursorDistance=%3")
                            .arg(selectedIndex)
                            .arg(controlPointIndex_)
                            .arg(cursorDistanceFromSnap, 0, 'f', 2));
                } else {
                    beginDragHistory();
                    translateControlPoint(selectedShapeIndex_, controlPointIndex_, delta);

                    const QVector<QPointF> movedControlPoints =
                        controlPointsForShape(shapes_[selectedIndex]);
                    if (controlPointIndex_ < movedControlPoints.size()) {
                        QElapsedTimer snapTimer;
                        snapTimer.start();
                        currentDragSnap_ = findControlPointSnap(
                            selectedShapeIndex_,
                            controlPointIndex_,
                            movedControlPoints[controlPointIndex_]);
                        snapEvaluationMicroseconds = snapTimer.nsecsElapsed() / 1000;
                        if (currentDragSnap_.isValid()) {
                            translateControlPoint(selectedShapeIndex_,
                                                  controlPointIndex_,
                                                  currentDragSnap_.translation);
                            dragSnapLocked_ = true;
                            dragSnapCursorWorld_ = rawCursorWorld_;
                            dragSnapCursorScreen_ = screenPosition;
                            // Keep the original mouse-to-handle offset. A snap
                            // temporarily holds the CV at its target; release
                            // restores the cursor-relative drag position.
                            DebugLog::instance().write(
                                QStringLiteral("control point snapped shape=%1 index=%2 type=%3 target=%4")
                                    .arg(selectedIndex)
                                    .arg(controlPointIndex_)
                                    .arg(snapTypeName(currentDragSnap_.type))
                                    .arg(pointText(currentDragSnap_.targetPoint)));
                        }
                    }
                }

                setSelectionLastControlPointWorldPosition(rawCursorWorld_);
                qreal snapCorrectionPixels = -1.0;
                if (currentDragSnap_.isValid()) {
                    const QPointF sourceScreen =
                        worldToScreen(currentDragSnap_.sourcePoint);
                    const QPointF targetScreen =
                        worldToScreen(currentDragSnap_.targetPoint);
                    snapCorrectionPixels =
                        std::hypot(targetScreen.x() - sourceScreen.x(),
                                   targetScreen.y() - sourceScreen.y());
                }
                const quint64 traceSequence = ++dragSnapTraceSequence_;
                DebugLog::instance().write(
                    QStringLiteral("drag-snap-trace seq=%1 mode=control-point shape=%2 index=%3 cursorScreen=%4 cursorStepPx=%5 cursorWorld=%6 worldDelta=%7 snapEvalUs=%8 locked=%9 snap=%10 snapSource=%11 snapTarget=%12 snapTranslation=%13 snapCorrectionPx=%14")
                        .arg(traceSequence)
                        .arg(selectedIndex)
                        .arg(controlPointIndex_)
                        .arg(precisePointText(screenPosition))
                        .arg(precisePointText(cursorStepScreen))
                        .arg(precisePointText(rawCursorWorld_))
                        .arg(precisePointText(delta))
                        .arg(snapEvaluationMicroseconds)
                        .arg(dragSnapLocked_)
                        .arg(snapTypeName(currentDragSnap_.type))
                        .arg(precisePointText(currentDragSnap_.sourcePoint))
                        .arg(precisePointText(currentDragSnap_.targetPoint))
                        .arg(precisePointText(currentDragSnap_.translation))
                        .arg(snapCorrectionPixels, 0, 'f', 4));
            }
        } else if (objectSelectionDragActive() && selectionDragStarted() &&
                   selectedIndex >= 0) {
            const QVector<ObjectId> dragIndices = selectionDraggedObjectIds().isEmpty()
                                                     ? QVector<ObjectId>{selectedShapeIndex_}
                                                     : selectionDraggedObjectIds();
            if (grabTool_.isActive()) {
                updateGrabPosition(dragIndices, screenPosition);
            } else {
                const bool groupDrag = dragIndices.size() > 1;
                const QPointF previousDragCursorWorld =
                    selectionLastDragWorldPosition();
                const QPointF previousViewPlaneScreen =
                    selectionDragLastScreenPosition_;
                Point3D viewPlaneWorldDelta;
                const bool hasViewPlaneDelta =
                    updateSelectionViewPlaneDrag(screenPosition,
                                                 &viewPlaneWorldDelta);
                const bool useViewPlaneDelta =
                    hasViewPlaneDelta &&
                    selectionDragViewPlaneAnchorValid_ &&
                    dragAxisLock_ == DragAxisLock::None;
                const QPointF cursorStepScreen =
                    selectionDragViewPlaneAnchorValid_
                        ? screenPosition - previousViewPlaneScreen
                        : screenPosition -
                              worldToScreen(previousDragCursorWorld);
                const QPointF rawDelta = useViewPlaneDelta
                    ? projectWorldDeltaToWorkPlane(viewPlaneWorldDelta)
                    : rawCursorWorld_ - previousDragCursorWorld;
                const QPointF delta = constrainDragDelta(rawDelta);
                const bool hasMovement = useViewPlaneDelta
                    ? !isZeroWorldDelta(viewPlaneWorldDelta)
                    : !qFuzzyIsNull(rawDelta.x()) ||
                          !qFuzzyIsNull(rawDelta.y());
                if (hasMovement) {
                    qint64 snapEvaluationMicroseconds = -1;
                    if (dragAxisLock_ != DragAxisLock::None) {
                        if (dragAxisLock_ == DragAxisLock::Z) {
                            updateWorldZAxisDrag(dragIndices, screenPosition);
                        } else if (!qFuzzyIsNull(delta.x()) ||
                                   !qFuzzyIsNull(delta.y())) {
                            beginDragHistory();
                            translateShapes(dragIndices, delta);
                        }
                        // Axis locking takes priority over object snapping so the
                        // move remains exactly on the chosen world axis.
                        currentDragSnap_ = DragSnapResult{};
                        dragSnapLocked_ = false;
                        dragSnapViewPlaneAnchorValid_ = false;
                    } else {
                        if (dragSnapLocked_ && currentDragSnap_.type == SnapType::Near) {
                            // Track the snapped point on the moving selection, not
                            // the mouse cursor: the cursor can be far from that
                            // point when dragging a whole curve. Keep an
                            // unconstrained copy so small moves away from the rail
                            // accumulate instead of being snapped back forever.
                            constexpr qreal nearSnapReleaseRadiusPixels = 12.0;
                            const QPointF movedSourcePoint =
                                currentDragSnap_.targetPoint + delta;
                            const QPointF freeSourcePoint =
                                (nearDragFreeSourcePointValid_
                                     ? nearDragFreeSourcePoint_
                                     : currentDragSnap_.targetPoint) + delta;
                            nearDragFreeSourcePoint_ = freeSourcePoint;
                            nearDragFreeSourcePointValid_ = true;
                            const int nearTargetShapeIndex =
                                currentDragSnap_.targetShapeIndex;
                            const int nearTargetComponentIndex =
                                currentDragSnap_.targetComponentIndex;
                            beginDragHistory();
                            translateSelectionDrag(dragIndices, delta,
                                                   viewPlaneWorldDelta,
                                                   useViewPlaneDelta);
                            QElapsedTimer snapTimer;
                            snapTimer.start();
                            // A Near lock must not mask a more specific target
                            // that enters range later in the same drag. Check
                            // explicit OSnaps first; only keep riding the Near
                            // rail when none of them currently applies.
                            const DragSnapResult specificDragSnap =
                                findDragSnap(dragIndices, false, false);
                            if (specificDragSnap.isValid()) {
                                currentDragSnap_ = specificDragSnap;
                                applyObjectDragSnap(dragIndices, specificDragSnap);
                                dragSnapCursorWorld_ = rawCursorWorld_;
                                if (useViewPlaneDelta) {
                                    dragSnapViewPlaneWorld_ =
                                        selectionDragCurrentViewPlaneWorld_;
                                    selectionDragSnapScreen_ = screenPosition;
                                    dragSnapViewPlaneAnchorValid_ = true;
                                }
                                nearDragFreeSourcePointValid_ = false;
                                DebugLog::instance().write(
                                    QStringLiteral("selection drag near-promoted snap=%1 source=%2 target=%3")
                                        .arg(snapTypeName(specificDragSnap.type))
                                        .arg(pointText(specificDragSnap.sourcePoint))
                                        .arg(pointText(specificDragSnap.targetPoint)));
                            } else {
                                currentDragSnap_ = trackNearDragSnap(
                                    dragIndices,
                                    freeSourcePoint,
                                    nearTargetShapeIndex,
                                    nearTargetComponentIndex,
                                    nearSnapReleaseRadiusPixels);
                            }
                            snapEvaluationMicroseconds =
                                snapTimer.nsecsElapsed() / 1000;
                            if (currentDragSnap_.isValid()) {
                                if (currentDragSnap_.type == SnapType::Near) {
                                    // The tracked result's source is the free point;
                                    // correct from the selection's actual moved point.
                                    currentDragSnap_.sourcePoint = movedSourcePoint;
                                    currentDragSnap_.translation =
                                        currentDragSnap_.targetPoint - movedSourcePoint;
                                    translateShapes(dragIndices,
                                                    currentDragSnap_.translation);
                                    dragSnapCursorWorld_ = rawCursorWorld_;
                                }
                            } else {
                                // Leave the rail at the accumulated free position.
                                translateShapes(dragIndices,
                                                freeSourcePoint - movedSourcePoint);
                                currentDragSnap_ = DragSnapResult{};
                                dragSnapLocked_ = false;
                                dragSnapViewPlaneAnchorValid_ = false;
                                nearDragFreeSourcePointValid_ = false;
                            }
                        } else {
                            const QPointF snapScreen =
                                useViewPlaneDelta &&
                                        dragSnapViewPlaneAnchorValid_
                                    ? selectionDragSnapScreen_
                                    : worldToScreen(dragSnapCursorWorld_);
                            const qreal cursorDistanceFromSnap = std::hypot(
                                screenPosition.x() - snapScreen.x(),
                                screenPosition.y() - snapScreen.y());

                            if (dragSnapLocked_ &&
                                cursorDistanceFromSnap <=
                                    kDragSnapBreakawayPixels) {
                                // Keep the geometry attached while the cursor is still near
                                // the snap point for non-Near snaps.
                                DebugLog::instance().write(
                                    QStringLiteral("selection drag snap-hold shape=%1 cursorDistance=%2 breakaway=%3")
                                        .arg(selectedIndex)
                                        .arg(cursorDistanceFromSnap, 0, 'f', 2)
                                        .arg(kDragSnapBreakawayPixels, 0, 'f', 2));
                            } else if (dragSnapLocked_) {
                                // Release from the snap using the complete cursor movement
                                // since the snap was acquired, so the line leaves cleanly.
                                beginDragHistory();
                                if (useViewPlaneDelta &&
                                    dragSnapViewPlaneAnchorValid_) {
                                    const Point3D detachDelta{
                                        selectionDragCurrentViewPlaneWorld_.x -
                                            dragSnapViewPlaneWorld_.x,
                                        selectionDragCurrentViewPlaneWorld_.y -
                                            dragSnapViewPlaneWorld_.y,
                                        selectionDragCurrentViewPlaneWorld_.z -
                                            dragSnapViewPlaneWorld_.z};
                                    translateShapesWorldDelta(dragIndices,
                                                              detachDelta);
                                } else {
                                    const QPointF detachDelta =
                                        rawCursorWorld_ - dragSnapCursorWorld_;
                                    translateShapes(dragIndices, detachDelta);
                                }
                                currentDragSnap_ = DragSnapResult{};
                                dragSnapLocked_ = false;
                                dragSnapViewPlaneAnchorValid_ = false;
                                DebugLog::instance().write(
                                    QStringLiteral("selection drag snap-breakaway shape=%1 cursorDistance=%2")
                                        .arg(selectedIndex)
                                        .arg(cursorDistanceFromSnap, 0, 'f', 2));
                            } else {
                                beginDragHistory();
                                translateSelectionDrag(dragIndices, delta,
                                                       viewPlaneWorldDelta,
                                                       useViewPlaneDelta);
                                QElapsedTimer snapTimer;
                                snapTimer.start();
                                currentDragSnap_ = groupDrag
                                                        ? findDragSnap(dragIndices)
                                                        : findDragSnap(selectedShapeIndex_);
                                snapEvaluationMicroseconds =
                                    snapTimer.nsecsElapsed() / 1000;
                                if (currentDragSnap_.isValid()) {
                                    applyObjectDragSnap(dragIndices, currentDragSnap_);
                                    dragSnapLocked_ = true;
                                    dragSnapCursorWorld_ = rawCursorWorld_;
                                    if (useViewPlaneDelta) {
                                        dragSnapViewPlaneWorld_ =
                                            selectionDragCurrentViewPlaneWorld_;
                                        selectionDragSnapScreen_ = screenPosition;
                                        dragSnapViewPlaneAnchorValid_ = true;
                                    }
                                    nearDragFreeSourcePointValid_ =
                                        currentDragSnap_.type == SnapType::Near;
                                    if (nearDragFreeSourcePointValid_) {
                                        // This is the geometry point placed on the
                                        // target rail, independent of the grab location.
                                        nearDragFreeSourcePoint_ =
                                            currentDragSnap_.targetPoint;
                                    }
                                }
                            }
                        }
                    }

                    setSelectionLastDragWorldPosition(rawCursorWorld_);

                    qreal snapCorrectionPixels = -1.0;
                    if (currentDragSnap_.isValid()) {
                        const QPointF sourceScreen =
                            worldToScreen(currentDragSnap_.sourcePoint);
                        const QPointF targetScreen =
                            worldToScreen(currentDragSnap_.targetPoint);
                        snapCorrectionPixels =
                            std::hypot(targetScreen.x() - sourceScreen.x(),
                                       targetScreen.y() - sourceScreen.y());
                    }
                    const quint64 traceSequence = ++dragSnapTraceSequence_;
                    DebugLog::instance().write(
                        QStringLiteral("drag-snap-trace seq=%1 mode=object shape=%2 selectedCount=%3 sceneCount=%4 cursorScreen=%5 cursorStepPx=%6 cursorWorld=%7 worldDelta=%8 snapEvalUs=%9 locked=%10 snap=%11 snapSource=%12 snapTarget=%13 snapTranslation=%14 snapCorrectionPx=%15 axisLock=%16")
                            .arg(traceSequence)
                            .arg(selectedIndex)
                            .arg(dragIndices.size())
                            .arg(shapes_.size())
                            .arg(precisePointText(screenPosition))
                            .arg(precisePointText(cursorStepScreen))
                            .arg(precisePointText(rawCursorWorld_))
                            .arg(precisePointText(delta))
                            .arg(snapEvaluationMicroseconds)
                            .arg(dragSnapLocked_)
                            .arg(snapTypeName(currentDragSnap_.type))
                            .arg(precisePointText(currentDragSnap_.sourcePoint))
                            .arg(precisePointText(currentDragSnap_.targetPoint))
                            .arg(precisePointText(currentDragSnap_.translation))
                            .arg(snapCorrectionPixels, 0, 'f', 4)
                            .arg(dragAxisLockName(dragAxisLock_)));
                    if (groupDrag) {
                        DebugLog::instance().write(
                            QStringLiteral("group selection drag count=%1")
                                .arg(dragIndices.size()));
                    }
                }
            }
        }

        if (pointPreviewActive || picturePreviewActive || lineCommandActive_ ||
            arcPreviewActive || circlePreviewActive ||
            tangentCirclePreviewActive || ellipsePreviewActive ||
            rectanglePreviewActive || polygonPreviewActive || mirrorPreviewActive || navigationController_.isPanning() ||
            objectSelectionDragActive() ||
            controlPointSelectionDragActive()) {
            update();
        }

        if (pointPreviewActive || picturePreviewActive || lineCommandActive_ ||
            arcPreviewActive || circlePreviewActive ||
            tangentCirclePreviewActive || ellipsePreviewActive ||
            rectanglePreviewActive || polygonPreviewActive || mirrorPreviewActive ||
            activeTool_ == Tool::Erase || navigationController_.isPanning() ||
            objectSelectionDragActive() || controlPointSelectionDragActive()) {
            DebugLog::instance().write(
                QStringLiteral("mouseMove screen=%1 worldRaw=%2 worldUsed=%3 lineActive=%4 points=%5 panning=%6 dragging=%7 ortho=%8 pan=%9 zoom=%10 buttons=0x%11 arcMode=%12 arcSweep=%13 snap=%14")
                    .arg(pointText(screenPosition))
                    .arg(pointText(rawCursorWorld_))
                    .arg(pointText(cursorWorld_))
                    .arg(lineCommandActive_)
                    .arg(pendingPoints_.size())
                    .arg(navigationController_.isPanning())
                    .arg(objectSelectionDragActive())
                    .arg(orthoEnabled_)
                    .arg(pointText(pan_))
                    .arg(zoom_, 0, 'f', 4)
                    .arg(static_cast<int>(event->buttons()), 0, 16)
                    .arg(activeTool_ == Tool::Arc ? arcModeName(arcState().mode) : QStringLiteral("None"))
                    .arg(arcState().previewSweepAngle, 0, 'f', 4)
                    .arg(snapTypeName(currentSnap_.type)));
        }

        emitCoordinateUpdate();
    }

    void leaveEvent(QEvent *event) override
    {
        navigationController_.pointerLeave();
        if (shadingControlPressed_ < 0 && shadingControlHover_ >= 0) {
            shadingControlHover_ = -1;
            setToolTip(QString());
            update();
        }
        QWidget::leaveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::RightButton &&
            rightButtonSelectionGestureActive_) {
            rightButtonSelectionGestureActive_ = false;
            dispatchSyntheticMouseEvent(QEvent::MouseButtonRelease,
                                        eventPosition(event),
                                        Qt::LeftButton,
                                        Qt::NoButton,
                                        event->modifiers());
            event->accept();
            return;
        }
        if (event->button() == Qt::LeftButton &&
            shadingControlPressed_ >= 0) {
            const int pressed = shadingControlPressed_;
            shadingControlPressed_ = -1;
            if (viewportShadingControlAt(eventPosition(event)) == pressed) {
                activateViewportShadingControl(pressed);
            } else {
                update();
            }
            event->accept();
            return;
        }
        if (navigationController_.handleGizmoRelease(event->button(),
                                                     eventPosition(event),
                                                     size())) {
            event->accept();
            return;
        }

        const bool rightPanClickSelect =
            navigationController_.isPanning() &&
            event->button() == navigationController_.panButton() &&
            event->button() == Qt::RightButton &&
            !navigationController_.panMoved() &&
            activeTool_ == Tool::Select;

        DebugLog::instance().write(QStringLiteral("mouseRelease button=%1 screen=%2 panningBefore=%3 panMoved=%4 draggingBefore=%5 clickSelect=%6")
                                       .arg(inputButtonName(event->button()))
                                       .arg(pointText(eventPosition(event)))
                                       .arg(navigationController_.isPanning())
                                       .arg(navigationController_.panMoved())
                                       .arg(objectSelectionDragActive())
                                       .arg(rightPanClickSelect));
        if (grabTool_.isActive()) {
            return;
        }

        const bool releaseEraseCursor =
            isEraseLikeTool(activeTool_) && event->button() == Qt::LeftButton;
        if (releaseEraseCursor) {
            eraseTool_.setCursorPressed(false);
        }
        if (navigationController_.finishPointerRelease(event->button())) {
            DebugLog::instance().write(QStringLiteral("mouseRelease branch=end-pan-orbit"));
        }

        if (rightPanClickSelect) {
            dispatchSyntheticMouseEvent(QEvent::MouseButtonPress,
                                        eventPosition(event),
                                        Qt::LeftButton,
                                        Qt::LeftButton,
                                        event->modifiers());
            dispatchSyntheticMouseEvent(QEvent::MouseButtonRelease,
                                        eventPosition(event),
                                        Qt::LeftButton,
                                        Qt::NoButton,
                                        event->modifiers());
            event->accept();
            return;
        }

        if (eraseTool_.strokeActive() && event->button() == Qt::LeftButton) {
            applyEraseCandidates();
            eraseTool_.finishStroke();
            eraseTool_.clearCandidates();
            eraseTargetShapeIndices_.clear();
            eraseSceneCurveCaches_.clear();
            eraseTargetCurveCaches_.clear();
            eraseGeometryCachePrepared_ = false;
            setCursor(Qt::CrossCursor);
            DebugLog::instance().write(QStringLiteral("mouseRelease branch=end-erase-stroke"));
            update();
            return;
        }

        if (selectionBoxOverlayActive() && event->button() == Qt::LeftButton) {
            if (trimTool_.boxSelectionActive()) {
                updateSelectionBoxPosition(eventPosition(event));
                finishTrimBoxSelection();
            } else {
                updateSelectionBoxPosition(eventPosition(event));
                finishSelectionBox();
            }
            return;
        }

        if (controlPointSelectionDragActive() &&
            event->button() == Qt::LeftButton) {
            clearSelectionDragState();
            selection_.clearActiveControlPoint();
            dragHistoryRecorded_ = false;
            currentDragSnap_ = DragSnapResult{};
            dragSnapLocked_ = false;
            dragAxisLock_ = DragAxisLock::None;
            setCursor(activeTool_ == Tool::Select ? Qt::ArrowCursor : Qt::CrossCursor);
            DebugLog::instance().write(QStringLiteral("mouseRelease branch=end-control-point-drag shape=%1")
                                           .arg(objectIndex(selectedShapeIndex_)));
            update();
        } else if (objectSelectionDragActive() &&
                   event->button() == Qt::LeftButton) {
            clearSelectionDragState();
            dragHistoryRecorded_ = false;
            currentDragSnap_ = DragSnapResult{};
            dragSnapLocked_ = false;
            dragAxisLock_ = DragAxisLock::None;
            setCursor(activeTool_ == Tool::Select ? Qt::ArrowCursor : Qt::CrossCursor);
            DebugLog::instance().write(QStringLiteral("mouseRelease branch=end-selection-drag shape=%1")
                                           .arg(objectIndex(selectedShapeIndex_)));
            update();
        }

        if (releaseEraseCursor) {
            update();
        }
    }

    void wheelEvent(QWheelEvent *event) override
    {
        navigationController_.stopAnimation();
        if (subdivisionTool_.isActive()) {
            const ToolInput input = ToolInputTranslator::fromWheelEvent(
                *event,
                eventPosition(event),
                viewportTransform_.workPlaneFrame(),
                size());
            const SubdivisionWheelResult result = subdivisionTool_.handleWheel(
                input, maxSubdivisionSections, toolContext_);
            if (result.sectionsChanged) {
                notifySubdivisionStatus();
                update();
                DebugLog::instance().write(
                    QStringLiteral("subdivision wheel sections=%1 points=%2")
                        .arg(subdivisionTool_.sections())
                        .arg(subdivisionTool_.sections() - 1));
            }

            DebugLog::instance().write(
                QStringLiteral("subdivision wheel angleDelta=%1 pixelDelta=%2 phase=%3 logicalSteps=%4 angleRemainder=%5 pixelRemainder=%6 sections=%7")
                    .arg(input.wheelAngleDelta)
                    .arg(input.wheelPixelDelta)
                    .arg(static_cast<int>(event->phase()))
                    .arg(result.logicalSteps)
                    .arg(subdivisionTool_.wheelAngleAccumulator())
                    .arg(subdivisionTool_.wheelPixelAccumulator(), 0, 'f', 2)
                    .arg(subdivisionTool_.sections()));
            event->accept();
            return;
        }

        if (isPolygonTool(activeTool_) && activeToolController_ != nullptr) {
            const ToolInput input = ToolInputTranslator::fromWheelEvent(
                *event, eventPosition(event),
                viewportTransform_.workPlaneFrame(), size());
            if (activeToolController_->dispatchWheel(input, toolContext_) ==
                InteractionTool::EventResult::Handled) {
                update();
                emitCoordinateUpdate();
            }
            event->accept();
            return;
        }

        const QPointF screenPosition = eventPosition(event);
        const int pixelDelta = event->pixelDelta().y();
        const int angleDelta = event->angleDelta().y();
        const WheelZoomResult zoomResult = navigationController_.zoomFromWheel(
            screenPosition, angleDelta, pixelDelta, size());
        if (!zoomResult.changed) {
            event->accept();
            return;
        }

        DebugLog::instance().write(QStringLiteral("wheel screen=%1 angleDeltaY=%2 pixelDeltaY=%3 zoom=%4->%5 worldBefore=%6 worldAfter=%7 pan=%8")
                                       .arg(pointText(screenPosition))
                                       .arg(angleDelta)
                                       .arg(pixelDelta)
                                       .arg(zoomResult.oldZoom, 0, 'f', 4)
                                       .arg(zoom_, 0, 'f', 4)
                                       .arg(pointText(zoomResult.worldBefore))
                                       .arg(pointText(zoomResult.worldAfter))
                                       .arg(pointText(pan_)));

        event->accept();
    }

    bool event(QEvent *event) override
    {
        if (event != nullptr && event->type() == QEvent::ShortcutOverride &&
            activeTool_ == Tool::Arc) {
            const auto *keyEvent = static_cast<const QKeyEvent *>(event);
            if (keyEvent->modifiers() == Qt::NoModifier &&
                (keyEvent->key() == Qt::Key_A || keyEvent->key() == Qt::Key_R)) {
                event->accept();
                return true;
            }
        }
        return QWidget::event(event);
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        navigationController_.stopAnimation();
        DebugLog::instance().write(QStringLiteral("keyPress key=%1 text=%2 tool=%3 lineActive=%4 points=%5")
                                       .arg(event->key())
                                       .arg(event->text())
                                       .arg(toolName(activeTool_))
                                       .arg(lineCommandActive_)
                                       .arg(pendingPoints_.size()));
        if (!event->isAutoRepeat() && event->key() == Qt::Key_Z &&
            event->modifiers().testFlag(Qt::AltModifier)) {
            activateViewportShadingControl(0);
            event->accept();
            return;
        }
        if (activeTool_ == Tool::Select && !event->isAutoRepeat() &&
            event->modifiers() == Qt::NoModifier &&
            (event->key() == Qt::Key_1 || event->key() == Qt::Key_2 ||
             event->key() == Qt::Key_3)) {
            setComponentSelectionMode(static_cast<ComponentSelectionMode>(
                event->key() - Qt::Key_1));
            event->accept();
            return;
        }
        ToolInput keyInput = makeKeyToolInput(*event);
        if (activeTool_ == Tool::Rotate) {
            keyInput = makeRotateToolInput();
            keyInput.key = event->key();
            keyInput.text = event->text();
            keyInput.modifiers = event->modifiers();
            keyInput.autoRepeat = event->isAutoRepeat();
        }
        if (activeTool_ == Tool::Scale) {
            // Scale's numeric Enter uses the same resolved cursor point as its
            // preview, including any snap or point constraint already applied.
            keyInput.worldPosition = cursorWorld_;
            if (scaleState().stage == 0 &&
                (event->key() == Qt::Key_Return ||
                 event->key() == Qt::Key_Enter)) {
                keyInput.worldPosition = scaleSelectionBoundsCenter();
            }
        }
        if (selectionBoxOverlayActive() && event->key() == Qt::Key_Escape) {
            cancelSelectionBox();
            return;
        }

        if (activeTool_ == Tool::Picture && event->key() == Qt::Key_Escape) {
            cancelPicturePlacement();
            return;
        }

        const JoinKeyAction joinKeyAction = joinTool_.handleKey(event->key());
        if (joinKeyAction == JoinKeyAction::Complete) {
            applyJoin();
            return;
        }
        if (joinKeyAction == JoinKeyAction::Cancel) {
            cancelJoinMode();
            return;
        }

        const SubdivisionInputAction subdivisionAction =
            subdivisionTool_.handleKey(event->key());
        if (subdivisionAction == SubdivisionInputAction::Apply) {
            applySubdivision(subdivisionTool_.sections());
            return;
        }
        if (subdivisionAction == SubdivisionInputAction::Cancel) {
            cancelSubdivisionPreview();
            return;
        }

        if (grabTool_.isActive() && event->key() == Qt::Key_Escape) {
            cancelGrab();
            return;
        }

        if (duplicateTool_.isActive() && event->key() == Qt::Key_Escape) {
            cancelDuplicate();
            return;
        }

        if (grabTool_.isActive() && !event->isAutoRepeat() &&
            event->modifiers() == Qt::NoModifier && event->key() == Qt::Key_B) {
            beginGrabBasePointMode();
            return;
        }

        if (selectionShortcutsAvailable() &&
            !event->isAutoRepeat() && event->modifiers() == Qt::NoModifier &&
            event->key() == Qt::Key_G) {
            beginGrab();
            return;
        }

        if (selectionShortcutsAvailable() &&
            !event->isAutoRepeat() && event->modifiers() == Qt::NoModifier &&
            event->key() == Qt::Key_F) {
            executeCommand(ViewportCommand::Fill);
            event->accept();
            return;
        }

        if (activeTool_ == Tool::Select && objectSelectionDragActive() &&
            !event->isAutoRepeat() && event->modifiers() == Qt::NoModifier &&
            (event->key() == Qt::Key_X || event->key() == Qt::Key_Y ||
             event->key() == Qt::Key_Z)) {
            const DragAxisLock requestedLock =
                event->key() == Qt::Key_X ? DragAxisLock::X
                : event->key() == Qt::Key_Y ? DragAxisLock::Y
                                            : DragAxisLock::Z;
            dragAxisLock_ = dragAxisLock_ == requestedLock
                                ? DragAxisLock::None
                                : requestedLock;
            dragAxisPositionValid_ = false;
            if (dragAxisLock_ == DragAxisLock::Z) {
                const QPointF pointerScreen = mapFromGlobal(QCursor::pos());
                dragAxisPositionValid_ = worldZAxisPositionAtScreen(
                    pointerScreen, &dragAxisLastPosition_);
            }
            currentDragSnap_ = DragSnapResult{};
            dragSnapLocked_ = false;
            dragSnapViewPlaneAnchorValid_ = false;
            if (grabTool_.isActive()) {
                updateGrabPosition(selectionDraggedObjectIds().isEmpty()
                                       ? QVector<ObjectId>{selectedShapeIndex_}
                                       : selectionDraggedObjectIds(),
                                   mapFromGlobal(QCursor::pos()));
            }
            DebugLog::instance().write(
                QStringLiteral("selection drag axis lock=%1")
                    .arg(dragAxisLockName(dragAxisLock_)));
            update();
            emitCoordinateUpdate();
            return;
        }

        if (isEraseLikeTool(activeTool_) && event->key() == Qt::Key_Escape) {
            exitEraseLikeTool();
            return;
        }

        if (activeTool_ == Tool::Arc && handleArcKeyInput(keyInput, event)) {
            return;
        }

        if (activeTool_ == Tool::Arc && !event->isAutoRepeat() &&
            event->modifiers() == Qt::NoModifier &&
            (event->key() == Qt::Key_X || event->key() == Qt::Key_Y ||
             event->key() == Qt::Key_Z)) {
            handleArcAxisKey(keyInput);
            event->accept();
            update();
            emitCoordinateUpdate();
            return;
        }

        const bool selectAllKeyBlocked =
            activeTool_ == Tool::Select && event->key() == Qt::Key_A &&
            event->modifiers() == Qt::NoModifier &&
            !selectionShortcutsAvailable();
        if (activeToolController_ != nullptr && !selectAllKeyBlocked) {
            if (activeTool_ == Tool::Arc && !pendingPoints_.isEmpty() &&
                event->key() == Qt::Key_L) {
                event->accept();
                return;
            }
            const Tool dispatchedTool = activeTool_;
            if (activeToolController_->dispatchKey(keyInput, toolContext_) ==
                InteractionTool::EventResult::Handled) {
                if (dispatchedTool == Tool::Scale) {
                    const ScaleKeyDispatchResult result =
                        scaleTool_.takeLastKeyDispatchResult();
                    if (result.cancelled) {
                        currentSnap_ = SnapResult{};
                        DebugLog::instance().write(
                            QStringLiteral("scale canceled"));
                        update();
                    }
                    if (result.point.action ==
                        ScalePointAction::BasePointCaptured) {
                        pendingPoints_ = {result.point.point};
                    }
                    if (result.promptChanged) {
                        publishScalePrompt();
                    }
                    if (result.commitAttempted && result.committed) {
                        currentSnap_ = SnapResult{};
                        logScaleCommit(result.mode, result.factor,
                                       result.basePoint, result.sourceCount);
                    }
                    if (result.redrawRequested) {
                        update();
                    }
                    event->accept();
                    return;
                }
                if (dispatchedTool == Tool::Rotate) {
                    const RotateKeyResult result =
                        rotateTool_.takeLastKeyDispatchResult();
                    applyRotateFrameResult(result.frame);
                    if (result.updateCursor) {
                        cursorWorld_ = result.cursorPoint;
                    }
                    if (result.updateRawCursor) {
                        rawCursorWorld_ = result.cursorPoint;
                        lastWorldPosition_ = result.cursorPoint;
                    }
                    if (result.cancelled) {
                        currentSnap_ = SnapResult{};
                        DebugLog::instance().write(
                            QStringLiteral("rotate canceled"));
                    } else if (result.commitRequested) {
                        commitRotate(result.commitAngle);
                    }
                    update();
                    event->accept();
                    return;
                }
                if (dispatchedTool == Tool::Mirror) {
                    currentSnap_ = SnapResult{};
                    DebugLog::instance().write(
                        QStringLiteral("mirror canceled"));
                    update();
                    event->accept();
                    return;
                }
                update();
                emitCoordinateUpdate();
                return;
            }
        }

        if (activeTool_ == Tool::Select &&
            event->key() == Qt::Key_A &&
            event->modifiers() == Qt::NoModifier &&
            selectionShortcutsAvailable()) {
            InteractionTool *selectionTool = toolRegistry_.find(Tool::Select);
            if (selectionTool != nullptr &&
                selectionTool->dispatchKey(keyInput, toolContext_) ==
                    InteractionTool::EventResult::Handled) {
                update();
                emitCoordinateUpdate();
                DebugLog::instance().write(
                    QStringLiteral("select all count=%1")
                        .arg(selectedShapeIndices_.size()));
            }
            return;
        }

        if (activeTool_ == Tool::Select &&
            (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) &&
            selectionShortcutsAvailable() &&
            !selectedShapeIndices_.isEmpty()) {
            deleteSelectedShapes();
            return;
        }

        if (event->key() == Qt::Key_Escape) {
            if (activeTool_ == Tool::Arc) {
                arcTool_.clearInputPoints();
            }
            pendingPoints_.clear();
            resetArcPreviewTracking();
            DebugLog::instance().write(QStringLiteral("keyPress branch=cancel-input"));

            if (activeTool_ == Tool::Line && lineCommandActive_) {
                setTool(Tool::Select);
                if (commandFinished_) {
                    commandFinished_(Tool::Select);
                }
            } else if (activeTool_ == Tool::Arc &&
                       arcState().mode == ArcMode::OnePoint) {
                setTool(Tool::Select);
                if (commandFinished_) {
                    commandFinished_(Tool::Select);
                }
            } else {
                lineCommandActive_ = false;
            }

            update();
            return;
        }

        QWidget::keyPressEvent(event);
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (activeTool_ != Tool::CircleTangentThree ||
            event->type() != QEvent::KeyPress) {
            return false;
        }

        QWidget *targetWidget = qobject_cast<QWidget *>(watched);
        if (targetWidget == nullptr || targetWidget->window() != window()) {
            return false;
        }

        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() != Qt::Key_Tab && keyEvent->key() != Qt::Key_Backtab) {
            return false;
        }

        const ToolInput input = makeKeyToolInput(*keyEvent);
        if (activeToolController_ != nullptr &&
            activeToolController_->dispatchKey(input, toolContext_) ==
                InteractionTool::EventResult::Handled) {
            keyEvent->accept();
            update();
            emitCoordinateUpdate();
            return true;
        }
        return false;
    }

private:
    void configureCommandRouter()
    {
        CommandRouter::Handlers handlers;
        handlers.undo = [this](int) {
            if (duplicateTool_.isActive()) {
                cancelDuplicate();
            }
            const bool accepted = history_.canUndo();
            undo();
            return ApplicationCommandResult{accepted, 0};
        };
        handlers.redo = [this](int) {
            if (duplicateTool_.isActive()) {
                cancelDuplicate();
            }
            const bool accepted = history_.canRedo();
            redo();
            return ApplicationCommandResult{accepted, 0};
        };
        handlers.beginSubdivision = [this](int) {
            return ApplicationCommandResult{beginSubdivisionWheelMode(), 0};
        };
        handlers.cancelSubdivision = [this](int) {
            cancelSubdivisionWheelMode();
            return ApplicationCommandResult{true, 0};
        };
        handlers.applySubdivision = [this](int sections) {
            return ApplicationCommandResult{applySubdivision(sections), 0};
        };
        handlers.beginJoin = [this](int) {
            return ApplicationCommandResult{beginJoinMode(), 0};
        };
        handlers.explode = [this](int) {
            const int count = explodeSelectedShapes();
            return ApplicationCommandResult{count > 0, count};
        };
        handlers.fill = [this](int) {
            const int count = fillSelectedClosedCurves();
            return ApplicationCommandResult{count > 0, count};
        };
        handlers.beginRotate = [this](int) {
            return ApplicationCommandResult{beginRotate(), 0};
        };
        handlers.beginScale = [this](int mode) {
            if (mode < static_cast<int>(ScaleMode::OneD) ||
                mode > static_cast<int>(ScaleMode::TwoD)) {
                return ApplicationCommandResult{};
            }
            return ApplicationCommandResult{
                beginScale(static_cast<ScaleMode>(mode)), 0};
        };
        handlers.beginMirror = [this](int) {
            return ApplicationCommandResult{beginMirror(), 0};
        };
        handlers.beginDuplicate = [this](int) {
            return ApplicationCommandResult{beginDuplicate(), 0};
        };
        handlers.duplicateInPlace = [this](int) {
            return ApplicationCommandResult{duplicateInPlace(), 0};
        };
        handlers.beginPointExtrude = [this](int) {
            const int count = beginPointExtrude();
            return ApplicationCommandResult{count > 0, count};
        };
        commandRouter_.setHandlers(std::move(handlers));
    }

    ArcTool::InteractionState &arcState()
    {
        return arcTool_.interactionState();
    }

    const ArcTool::InteractionState &arcState() const
    {
        return arcTool_.interactionState();
    }

    RotateTool::InteractionState &rotateState()
    {
        return rotateTool_.interactionState();
    }

    const RotateTool::InteractionState &rotateState() const
    {
        return rotateTool_.interactionState();
    }

    const ScaleTool::InteractionState &scaleState() const
    {
        return scaleTool_.interactionState();
    }

    void beginOrbitAt(const QPointF &screenPosition)
    {
        const ViewportNavigationPreferences preferences =
            viewportTransform_.navigationPreferences();
        Point3D depthPoint;
        if (preferences.useMouseDepthNavigate) {
            const ViewportRenderFrame renderFrame = viewportRenderFrame();
            const bool gpuDepthHit =
                gpuSurface_ != nullptr &&
                gpuSurface_->pickScenePoint(screenPosition,
                                            renderFrame.camera,
                                            renderFrame.viewportSize,
                                            renderFrame.objects,
                                            &depthPoint);
            const bool cpuDepthHit = !gpuDepthHit &&
                curveHitTester_.hitTestVisibleDepth(document_,
                                                     screenPosition,
                                                     viewportTransform_,
                                                     size(),
                                                     &depthPoint);
            if (gpuDepthHit || cpuDepthHit) {
                viewportTransform_.setOrbitPivotPreservingView(depthPoint);
                return;
            }
        }

        if (!preferences.orbitAroundActive || !selectedShapeIndex_.isValid()) {
            return;
        }
        const int shapeIndex = objectIndex(selectedShapeIndex_);
        if (shapeIndex < 0 || shapeIndex >= shapes_.size()) {
            return;
        }
        const Shape &shape = shapes_[shapeIndex];
        bool hasBounds = false;
        qreal minimumX = 0.0;
        qreal minimumY = 0.0;
        qreal maximumX = 0.0;
        qreal maximumY = 0.0;
        const auto includePoints = [&](const QVector<QPointF> &points) {
            for (const QPointF &point : points) {
                if (!std::isfinite(point.x()) || !std::isfinite(point.y())) {
                    continue;
                }
                if (!hasBounds) {
                    minimumX = maximumX = point.x();
                    minimumY = maximumY = point.y();
                    hasBounds = true;
                } else {
                    minimumX = std::min(minimumX, point.x());
                    minimumY = std::min(minimumY, point.y());
                    maximumX = std::max(maximumX, point.x());
                    maximumY = std::max(maximumY, point.y());
                }
            }
        };
        includePoints(shape.points);
        includePoints(shape.nurbs.controlPoints);
        for (const Shape::NurbsCurve2D &component : shape.components) {
            includePoints(component.controlPoints);
        }
        if (!hasBounds) {
            return;
        }
        const QPointF center((minimumX + maximumX) * 0.5,
                             (minimumY + maximumY) * 0.5);
        viewportTransform_.setOrbitPivotPreservingView(
            shapePointToWorld(shape, center));
    }

    void resetForDocumentReplacement()
    {
        history_.clear();
        pendingPoints_.clear();
        pendingPictureImage_ = QImage();
        pendingPictureImageData_.clear();
        pendingPicturePath_.clear();
        toolPreview_ = ToolPreview{};
        resetArcPreviewTracking();
        selection_.clear();
        resetSelectionBoxState();
        clearSelectionDragState();
        dragHistoryRecorded_ = false;
        currentDragSnap_ = DragSnapResult{};
        dragSnapLocked_ = false;
        eraseTool_.resetInteraction();
        trimTool_.clearCandidates();
        eraseTool_.clearCandidates();
        eraseTargetShapeIndices_.clear();
        eraseSceneCurveCaches_.clear();
        eraseTargetCurveCaches_.clear();
        eraseGeometryCachePrepared_ = false;
        trimTool_.invalidateHover();
        currentSnap_ = SnapResult{};
        joinTool_.cancel();
        resetScaleInteraction();
        resetRotateInteraction();
        resetMirrorInteraction();
        subdivisionTool_.finish();
        lineCommandActive_ = false;
        activeTool_ = Tool::Select;
    }

    void updateSnapEngineSettings()
    {
        snapEngine_.setSettings(SnapSettings{osnapEnabled_,
                                             endpointSnapEnabled_,
                                             midpointSnapEnabled_,
                                             intersectionSnapEnabled_,
                                             centerSnapEnabled_,
                                             perpendicularSnapEnabled_,
                                             tangentSnapEnabled_,
                                             nearSnapEnabled_,
                                             controlPointSnapEnabled_});
    }

    int objectIndex(ObjectId objectId) const
    {
        return document_.indexOf(objectId);
    }

    bool isJoinableShape(const Shape &shape) const
    {
        if (shape.geometryType == GeometryType::PolyCurve) {
            if (shape.components.isEmpty()) {
                return false;
            }
            for (const Shape::NurbsCurve2D &component : shape.components) {
                if (!isValidNurbsCurve(component)) {
                    return false;
                }
            }
            return true;
        }

        return shape.geometryType == GeometryType::Line ||
               shape.geometryType == GeometryType::Arc ||
               shape.geometryType == GeometryType::Bezier ||
               shape.geometryType == GeometryType::Nurbs ||
               shape.geometryType == GeometryType::Ellipse;
    }

    bool nurbsCurveEndpoints(const Shape::NurbsCurve2D &curve,
                             QPointF *start,
                             QPointF *end) const
    {
        return classiCAD::nurbsCurveEndpoints(curve, start, end);
    }

    qreal joinEndpointTolerance() const
    {
        constexpr qreal minimumTolerance = 1.0e-5;
        constexpr qreal screenTolerancePixels = 3.0;
        return std::max(minimumTolerance,
                        screenTolerancePixels /
                            std::max(viewportTransform_.viewScalePixelsPerWorldUnit(
                                         size()),
                                     1.0e-9));
    }

    qreal selectedJoinEndpointTolerance() const
    {
        constexpr qreal minimumTolerance = 1.0e-5;
        constexpr qreal screenTolerancePixels = 10.0;
        return std::max(minimumTolerance,
                        screenTolerancePixels /
                            std::max(viewportTransform_.viewScalePixelsPerWorldUnit(
                                         size()),
                            1.0e-9));
    }

    Shape::NurbsCurve2D reversedNurbsCurve(const Shape::NurbsCurve2D &curve) const
    {
        Shape::NurbsCurve2D reversed;
        return classiCAD::reverseNurbsCurve(curve, &reversed)
                   ? reversed
                   : Shape::NurbsCurve2D{};
    }

    bool orderJoinComponents(const QVector<Shape::NurbsCurve2D> &input,
                             QVector<Shape::NurbsCurve2D> *ordered,
                             qreal tolerance) const
    {
        return orderConnectedNurbsCurves(input, ordered, tolerance);
    }

    bool joinComponentsAreContinuousInWorld(
        const QVector<Shape::NurbsCurve2D> &components,
        const QVector<WorkPlaneFrame> &frames,
        qreal tolerance) const
    {
        return connectedNurbsCurvesAreContinuousInWorld(components,
                                                         frames,
                                                         tolerance);
    }

    bool nurbsCurvePointAtFraction(const Shape::NurbsCurve2D &curve,
                                   qreal fraction,
                                   QPointF *point) const
    {
        if (!isValidNurbsCurve(curve) || point == nullptr) {
            return false;
        }

        const QVector<double> fullKnots = expandedKnotVector(curve);
        if (fullKnots.size() <= curve.controlPoints.size()) {
            return false;
        }

        const qreal firstParameter = fullKnots[curve.degree];
        const qreal lastParameter = fullKnots[curve.controlPoints.size()];
        return evaluateNurbsPoint(curve,
                                  firstParameter +
                                      (lastParameter - firstParameter) *
                                          std::clamp(fraction, 0.0, 1.0),
                                  point);
    }

    QVector<QPointF> polyCurvePoints(
        const QVector<Shape::NurbsCurve2D> &components) const
    {
        QVector<QPointF> points;
        for (int index = 0; index < components.size(); ++index) {
            QPointF start;
            QPointF end;
            if (!nurbsCurveEndpoints(components[index], &start, &end)) {
                continue;
            }
            if (index == 0) {
                points.append(start);
            }
            points.append(end);
        }
        return points;
    }


    Shape polyCurveShapeForComponents(
        const Shape &source,
        const QVector<Shape::NurbsCurve2D> &components) const
    {
        Shape result = source;
        result.geometryType = GeometryType::PolyCurve;
        result.points = polyCurvePoints(components);
        result.nurbs = Shape::NurbsCurve2D{};
        result.arcMode = ArcMode::TwoPoint;
        result.arcSweep = 0.0;
        result.subdivisionParameters.clear();
        result.components = components;
        return result;
    }

    void normalizeDisconnectedPolyCurveObjects()
    {
        QVector<SceneObject> normalizedObjects;
        normalizedObjects.reserve(document_.size());
        bool changed = false;
        int splitCount = 0;
        for (const SceneObject &sceneObject : document_.objects()) {
            const Shape &source = sceneObject.geometry;
            if (source.geometryType != GeometryType::PolyCurve ||
                source.components.size() < 2) {
                normalizedObjects.append(sceneObject);
                continue;
            }
            if (!source.componentWorkPlaneFrames.isEmpty()) {
                // A mixed-plane joined curve already has world-space seam
                // continuity; the legacy planar grouping compares local 2D
                // coordinates and must not flatten or split these components.
                normalizedObjects.append(sceneObject);
                continue;
            }

            const auto connectedGroups = connectedNurbsCurveGroups(
                source.components, joinEndpointTolerance());
            QVector<QVector<Shape::NurbsCurve2D>> validGroups;
            for (const auto &group : connectedGroups) {
                QVector<Shape::NurbsCurve2D> ordered;
                if (group.size() > 1 &&
                    !orderJoinComponents(group,
                                         &ordered,
                                         joinEndpointTolerance())) {
                    // A connected branch is not one continuous spline. Keep
                    // each branch as its own selectable curve object.
                    for (const Shape::NurbsCurve2D &curve : group) {
                        validGroups.append({curve});
                    }
                } else {
                    validGroups.append(group.size() > 1 ? ordered : group);
                }
            }

            if (validGroups.size() <= 1) {
                normalizedObjects.append(sceneObject);
                continue;
            }

            changed = true;
            ++splitCount;
            SceneObject firstPiece = sceneObject;
            firstPiece.geometry = polyCurveShapeForComponents(source, validGroups.first());
            normalizedObjects.append(firstPiece);
            for (int groupIndex = 1; groupIndex < validGroups.size(); ++groupIndex) {
                SceneObject piece = sceneObject;
                piece.id = ObjectId::invalid();
                piece.geometry = polyCurveShapeForComponents(source, validGroups[groupIndex]);
                normalizedObjects.append(piece);
            }
        }

        if (changed) {
            document_.replaceObjects(normalizedObjects);
            DebugLog::instance().write(
                QStringLiteral("disconnected PolyCurve repair splitObjects=%1 objects=%2")
                    .arg(splitCount)
                    .arg(document_.size()));
        }
    }

    void notifyJoinStatus(const QString &message = QString())
    {
        if (joinStatusUpdate_) {
            joinStatusUpdate_(message.isEmpty() ? joinStatusText() : message);
        }
    }

    bool isShapeSelected(int shapeIndex) const
    {
        const ObjectId objectId = shapes_.objectIdAt(shapeIndex);
        return objectId.isValid() && selectedShapeIndices_.contains(objectId);
    }

    SelectTool *selectionToolController() const
    {
        return dynamic_cast<SelectTool *>(toolRegistry_.find(Tool::Select));
    }

    bool objectSelectionDragActive() const
    {
        const SelectTool *selectTool = selectionToolController();
        return selectTool != nullptr && selectTool->isObjectDragActive();
    }

    bool controlPointSelectionDragActive() const
    {
        const SelectTool *selectTool = selectionToolController();
        return selectTool != nullptr && selectTool->isControlPointDragActive();
    }

    bool selectionDragStarted() const
    {
        const SelectTool *selectTool = selectionToolController();
        return selectTool != nullptr && selectTool->dragGestureStarted();
    }

    void setSelectionDragStarted(bool started)
    {
        if (SelectTool *selectTool = selectionToolController()) {
            selectTool->setDragGestureStarted(started);
        }
    }

    const QVector<ObjectId> &selectionDraggedObjectIds() const
    {
        static const QVector<ObjectId> empty;
        const SelectTool *selectTool = selectionToolController();
        return selectTool != nullptr ? selectTool->draggedObjectIds() : empty;
    }

    QPointF selectionDragStartScreenPosition() const
    {
        const SelectTool *selectTool = selectionToolController();
        return selectTool != nullptr ? selectTool->dragStartScreenPosition()
                                    : QPointF{};
    }

    QPointF selectionLastDragWorldPosition() const
    {
        const SelectTool *selectTool = selectionToolController();
        return selectTool != nullptr ? selectTool->lastDragWorldPosition()
                                    : QPointF{};
    }

    void setSelectionLastDragWorldPosition(const QPointF &position)
    {
        if (SelectTool *selectTool = selectionToolController()) {
            selectTool->setLastDragWorldPosition(position);
        }
    }

    QPointF selectionLastControlPointWorldPosition() const
    {
        const SelectTool *selectTool = selectionToolController();
        return selectTool != nullptr ? selectTool->lastControlPointWorldPosition()
                                    : QPointF{};
    }

    void setSelectionLastControlPointWorldPosition(const QPointF &position)
    {
        if (SelectTool *selectTool = selectionToolController()) {
            selectTool->setLastControlPointWorldPosition(position);
        }
    }

    WorkPlaneFrame controlPointWorkPlaneFrame(ObjectId objectId,
                                               int controlPointIndex) const
    {
        const Shape *shape = document_.shape(objectId);
        if (shape == nullptr) {
            return {};
        }
        const auto withObjectPlacement = [&](WorkPlaneFrame frame) {
            const SceneObject *sceneObject = document_.object(objectId);
            if (sceneObject != nullptr) {
                frame.origin.x += sceneObject->placementTranslation.x;
                frame.origin.y += sceneObject->placementTranslation.y;
                frame.origin.z += sceneObject->placementTranslation.z;
            }
            return frame;
        };
        if (shape->geometryType != GeometryType::PolyCurve) {
            return withObjectPlacement(shapeWorkPlaneFrame(*shape));
        }

        int remainingControlPointIndex = controlPointIndex;
        for (int componentIndex = 0;
             componentIndex < shape->components.size();
             ++componentIndex) {
            const Shape::NurbsCurve2D &component =
                shape->components[componentIndex];
            if (remainingControlPointIndex < component.controlPoints.size()) {
                return withObjectPlacement(
                    shapeComponentWorkPlaneFrame(*shape, componentIndex));
            }
            remainingControlPointIndex -= component.controlPoints.size();
        }
        return withObjectPlacement(shapeWorkPlaneFrame(*shape));
    }

    void beginSelectionObjectDrag(const QVector<ObjectId> &objectIds,
                                  const QPointF &screenPosition,
                                  const QPointF &worldPosition,
                                  bool gestureStarted = false)
    {
        if (SelectTool *selectTool = selectionToolController()) {
            selectTool->beginObjectDrag(objectIds,
                                        screenPosition,
                                        worldPosition,
                                        gestureStarted);
        }
    }

    void clearSelectionDragState()
    {
        controlPointDragFrameValid_ = false;
        selectionDragViewPlaneFrame_ = WorkPlaneFrame{};
        selectionDragViewPlaneAnchorValid_ = false;
        dragSnapViewPlaneAnchorValid_ = false;
        nearDragFreeSourcePointValid_ = false;
        if (SelectTool *selectTool = selectionToolController()) {
            selectTool->clearDragState();
        }
    }

    void resetTrimSelectionBox()
    {
        trimTool_.clearBoxSelection();
    }

    QVector<ObjectId> &eraseCandidates()
    {
        return activeTool_ == Tool::Trim
                   ? trimTool_.candidateObjectIds()
                   : eraseTool_.candidateObjectIds();
    }

    const QVector<ObjectId> &eraseCandidates() const
    {
        return activeTool_ == Tool::Trim
                   ? trimTool_.candidateObjectIds()
                   : eraseTool_.candidateObjectIds();
    }

    QVector<QPointF> &eraseLikeScreenPath()
    {
        return activeTool_ == Tool::Trim
                   ? trimTool_.screenPath()
                   : eraseTool_.screenPath();
    }

    const QVector<QPointF> &eraseLikeScreenPath() const
    {
        return activeTool_ == Tool::Trim
                   ? trimTool_.screenPath()
                   : eraseTool_.screenPath();
    }

    void resetSelectionBoxState()
    {
        if (SelectTool *selectTool = selectionToolController()) {
            selectTool->cancelSelectionBox();
        }
        resetTrimSelectionBox();
    }

    bool selectionShortcutsAvailable() const
    {
        return activeTool_ == Tool::Select &&
               !selectionBoxOverlayActive() &&
               !objectSelectionDragActive() &&
               !controlPointSelectionDragActive() &&
               !grabTool_.isActive() && !duplicateTool_.isActive() &&
               !joinTool_.isActive() && !subdivisionTool_.isActive();
    }

    bool selectionBoxOverlayActive() const
    {
        const SelectTool *selectTool = selectionToolController();
        return trimTool_.boxSelectionActive() ||
               (selectTool != nullptr && selectTool->hasSelectionBox());
    }

    QPointF selectionBoxStartPosition() const
    {
        if (trimTool_.boxSelectionActive()) {
            return trimTool_.boxStartPosition();
        }
        const SelectTool *selectTool = selectionToolController();
        return selectTool != nullptr ? selectTool->selectionBoxStart() : QPointF{};
    }

    QPointF selectionBoxCurrentPosition() const
    {
        if (trimTool_.boxSelectionActive()) {
            return trimTool_.boxCurrentPosition();
        }
        const SelectTool *selectTool = selectionToolController();
        return selectTool != nullptr ? selectTool->selectionBoxCurrent() : QPointF{};
    }

    void updateSelectionBoxPosition(const QPointF &screenPosition)
    {
        if (trimTool_.boxSelectionActive()) {
            trimTool_.updateBoxSelection(screenPosition);
            return;
        }
        if (SelectTool *selectTool = selectionToolController()) {
            selectTool->updateSelectionBox(screenPosition);
        }
    }

    bool deleteSelectedShapes()
    {
        QVector<ObjectId> liveObjectIds;
        liveObjectIds.reserve(selectedShapeIndices_.size());
        for (const ObjectId objectId : selectedShapeIndices_) {
            if (objectIndex(objectId) >= 0 && !liveObjectIds.contains(objectId)) {
                liveObjectIds.append(objectId);
            }
        }
        if (liveObjectIds.isEmpty()) {
            clearSelection();
            return false;
        }

        DocumentTransaction transaction = session_.beginTransaction();
        int deletedCount = 0;
        if (!DeleteCommand::apply(document_,
                                  transaction,
                                  liveObjectIds,
                                  &deletedCount)) {
            return false;
        }
        if (!session_.commitTransaction(transaction)) {
            return false;
        }

        clearSelection();
        clearSelectionDragState();
        selection_.clearActiveControlPoint();
        dragHistoryRecorded_ = false;
        currentDragSnap_ = DragSnapResult{};
        dragSnapLocked_ = false;
        update();
        DebugLog::instance().write(QStringLiteral("delete selection count=%1 shapes=%2")
                                       .arg(deletedCount)
                                       .arg(shapes_.size()));
        return true;
    }

    void clearSelection()
    {
        selection_.clear();
    }

    void pruneSelectionToEditableLayers()
    {
        selection_.prune(document_);
        clearSelectionDragState();
        selection_.clearActiveControlPoint();
        dragHistoryRecorded_ = false;
        currentDragSnap_ = DragSnapResult{};
        dragSnapLocked_ = false;
    }

    QRectF selectionBoundsForShape(const Shape &shape,
                                   bool *hasProjectedPoints = nullptr,
                                   const Point3D &worldOffset = {}) const
    {
        const ProjectedShapeBoundsResult result = queryProjectedShapeBounds(
            shape, curveHitTester_, viewportTransform_, size(), worldOffset);
        if (hasProjectedPoints != nullptr) {
            *hasProjectedPoints = result.hasProjectedPoints;
        }
        return result.bounds;
    }

    bool shapeMatchesSelectionBox(const Shape &shape,
                                  const QRectF &box,
                                  bool crossingSelection,
                                  const Point3D &worldOffset = {}) const
    {
        const QRectF selectionRect = box.normalized();
        constexpr qreal crossingTolerancePixels = 2.0;
        const QRectF hitRect = crossingSelection
                                   ? selectionRect.adjusted(-crossingTolerancePixels,
                                                            -crossingTolerancePixels,
                                                            crossingTolerancePixels,
                                                            crossingTolerancePixels)
                                   : selectionRect;

        const SelectionBoxGeometryResult geometryHit =
            queryCurveOrPointSelectionBox(shape,
                                          selectionRect,
                                          crossingSelection,
                                          curveSampler_,
                                          viewportTransform_,
                                          size(),
                                          worldOffset);
        if (geometryHit.applies) {
            return geometryHit.matches;
        }

        // Non-curve geometry such as pictures and dimensions has no NURBS
        // path to sample, so retain its existing projected-bounds selection.
        bool hasProjectedPoints = false;
        const QRectF bounds = selectionBoundsForShape(shape, &hasProjectedPoints,
                                                      worldOffset)
                                  .normalized();
        if (!hasProjectedPoints) {
            return false;
        }

        // CAD-style selection windows use containment when dragged from
        // left to right and curve intersection when dragged from right to
        // left.
        if (!crossingSelection) {
            return bounds.left() >= selectionRect.left() &&
                   bounds.right() <= selectionRect.right() &&
                   bounds.top() >= selectionRect.top() &&
                   bounds.bottom() <= selectionRect.bottom();
        }

        return bounds.left() <= hitRect.right() &&
               hitRect.left() <= bounds.right() &&
               bounds.top() <= hitRect.bottom() &&
               hitRect.top() <= bounds.bottom();
    }

    void beginSelectionBox(const QPointF &screenPosition, bool additive)
    {
        if (activeTool_ == Tool::Trim) {
            trimTool_.beginBoxSelection(screenPosition);
        } else if (SelectTool *selectTool = selectionToolController()) {
            resetTrimSelectionBox();
            selectTool->beginSelectionBox(screenPosition, additive);
        }
        clearSelectionDragState();
        selection_.clearActiveControlPoint();
        dragHistoryRecorded_ = false;
        currentDragSnap_ = DragSnapResult{};
        dragSnapLocked_ = false;
        setCursor(Qt::CrossCursor);
        update();
        emitCoordinateUpdate();
        DebugLog::instance().write(QStringLiteral("selection box start additive=%1 at=%2")
                                       .arg(additive)
                                       .arg(pointText(screenPosition)));
    }

    void finishSelectionBox()
    {
        SelectTool *selectTool = selectionToolController();
        if (selectTool == nullptr || !selectTool->hasSelectionBox()) {
            return;
        }

        const QPointF boxStart = selectTool->selectionBoxStart();
        const QPointF boxCurrent = selectTool->selectionBoxCurrent();
        const QRectF selectionBox = QRectF(boxStart, boxCurrent).normalized();
        const bool crossingSelection =
            boxCurrent.x() < boxStart.x();
        const bool moved = selectTool->selectionBoxMoved() ||
                           selectionBox.width() >= 3.0 ||
                           selectionBox.height() >= 3.0;
        const bool additive = selectTool->selectionBoxAdditive();
        if (componentBoxSelectionActive_) {
            const ObjectId requestedObjectId = componentBoxSelectionObject_;
            componentBoxSelectionActive_ = false;
            componentBoxSelectionObject_ = ObjectId::invalid();
            selectTool->cancelSelectionBox();
            if (!moved && componentBoxStartedOnBlank_ && !additive) {
                clearComponentSelections();
                componentSelectionObject_ = ObjectId::invalid();
                selection_.clear();
            }
            componentBoxStartedOnBlank_ = false;
            if (moved) {
                ObjectId matchedObjectId = ObjectId::invalid();
                QSet<int> boxedComponents;
                for (int index = 0; index < shapes_.size(); ++index) {
                    const ObjectId candidateId = shapes_.objectIdAt(index);
                    if (requestedObjectId.isValid() &&
                        candidateId != requestedObjectId) {
                        continue;
                    }
                    if (!document_.isObjectVisible(candidateId) ||
                        !document_.isObjectEditable(candidateId)) {
                        continue;
                    }
                    const GeometryType type = shapes_[index].geometryType;
                    if (type != GeometryType::NurbsSolid &&
                        type != GeometryType::NurbsSurface) {
                        continue;
                    }
                    const SceneObject *sceneObject = document_.object(candidateId);
                    const Point3D offset = sceneObject != nullptr
                                               ? sceneObject->placementTranslation
                                               : Point3D{};
                    const ViewportDepthGeometry cage =
                        selectedSurfaceCage(shapes_[index]);
                    QSet<int> candidateComponents;
                    qreal cageScale = 1.0;
                    if (!cage.pointVertices.isEmpty()) {
                        const QVector3D origin = cage.pointVertices.first();
                        for (const QVector3D &cagePoint : cage.pointVertices)
                            cageScale = qMax(cageScale, qreal((cagePoint - origin).length()));
                    }
                    const qreal depthTolerance = qMax(1.0e-5, cageScale * 1.0e-5);
                    const auto visibleComponentPoint =
                        [&](const Point3D &worldPoint, const QPointF &projected) {
                            if (viewportShadingSettings_.xrayEnabled()) return true;
                            Point3D visiblePoint;
                            Point3D visibleNormal;
                            int visibleShapeIndex = -1;
                            if (!curveHitTester_.hitTestVisibleSurface(
                                    document_, projected, viewportTransform_, size(),
                                    &visiblePoint, &visibleNormal, nullptr,
                                    &visibleShapeIndex) ||
                                visibleShapeIndex != index) {
                                return false;
                            }
                            const qreal componentDepth =
                                viewportTransform_.worldDirectionToView(worldPoint)
                                    .towardCamera;
                            const qreal surfaceDepth =
                                viewportTransform_.worldDirectionToView(visiblePoint)
                                    .towardCamera;
                            return std::isfinite(componentDepth) &&
                                   std::isfinite(surfaceDepth) &&
                                   std::abs(componentDepth - surfaceDepth) <=
                                       depthTolerance;
                        };
                    if (componentSelectionMode_ == ComponentSelectionMode::Vertex) {
                        for (int vertex = 0;
                             vertex < cage.pointVertices.size(); ++vertex) {
                            const QVector3D &point = cage.pointVertices[vertex];
                            const Point3D worldPoint{
                                point.x() + offset.x, point.y() + offset.y,
                                point.z() + offset.z};
                            QPointF projected;
                            if (!viewportTransform_.worldPointToScreen(
                                    worldPoint, size(), &projected) ||
                                !selectionBox.contains(projected) ||
                                !visibleComponentPoint(worldPoint, projected)) {
                                continue;
                            }
                            candidateComponents.insert(vertex);
                        }
                    } else {
                        const auto segmentIntersectsSelectionBox =
                            [&selectionBox](const QLineF &segment) {
                                if (selectionBox.contains(segment.p1()) ||
                                    selectionBox.contains(segment.p2())) {
                                    return true;
                                }
                                const QPointF corners[] = {
                                    selectionBox.topLeft(), selectionBox.topRight(),
                                    selectionBox.bottomRight(), selectionBox.bottomLeft()};
                                for (int side = 0; side < 4; ++side) {
                                    QPointF intersection;
                                    const QLineF boundary(corners[side],
                                                         corners[(side + 1) % 4]);
                                    if (segment.intersects(
                                            boundary, &intersection) ==
                                        QLineF::BoundedIntersection) {
                                        return true;
                                    }
                                }
                                return false;
                            };
                        for (int edge = 0;
                             edge * 2 + 1 < cage.preciseLineVertices.size();
                             ++edge) {
                            const Point3D &a = cage.preciseLineVertices[edge * 2];
                            const Point3D &b = cage.preciseLineVertices[edge * 2 + 1];
                            const Point3D worldA{a.x + offset.x, a.y + offset.y,
                                                 a.z + offset.z};
                            const Point3D worldB{b.x + offset.x, b.y + offset.y,
                                                 b.z + offset.z};
                            QPointF screenA;
                            QPointF screenB;
                            if (!viewportTransform_.worldPointToScreen(
                                    worldA, size(), &screenA) ||
                                !viewportTransform_.worldPointToScreen(
                                    worldB, size(), &screenB)) {
                                continue;
                            }
                            const QLineF screenEdge(screenA, screenB);
                            const bool intersectsBox =
                                segmentIntersectsSelectionBox(screenEdge);
                            const bool selectedByBox = crossingSelection
                                                           ? intersectsBox
                                                           : selectionBox.contains(screenA) &&
                                                                 selectionBox.contains(screenB);
                            if (!selectedByBox) continue;
                            bool visible = viewportShadingSettings_.xrayEnabled();
                            if (!visible) {
                                // Check interior edge points against their
                                // actual perspective projections. Projecting
                                // the edge in screen space with the same t is
                                // only correct for orthographic views.
                                for (qreal t : {0.25, 0.5, 0.75}) {
                                    const Point3D sample{
                                        worldA.x + (worldB.x - worldA.x) * t,
                                        worldA.y + (worldB.y - worldA.y) * t,
                                        worldA.z + (worldB.z - worldA.z) * t};
                                    QPointF projected;
                                    if (viewportTransform_.worldPointToScreen(
                                            sample, size(), &projected) &&
                                        visibleComponentPoint(sample, projected)) {
                                        visible = true;
                                        break;
                                    }
                                }
                            }
                            if (visible) candidateComponents.insert(edge);
                        }
                    }
                    if (!candidateComponents.isEmpty()) {
                        matchedObjectId = candidateId;
                        boxedComponents = std::move(candidateComponents);
                        break;
                    }
                }

                if (matchedObjectId.isValid()) {
                    if (!additive || componentSelectionObject_ != matchedObjectId) {
                        clearComponentSelections();
                        selection_.setObjectIds({matchedObjectId}, matchedObjectId);
                    } else {
                        selection_.add(matchedObjectId);
                        selection_.setPrimaryObjectId(matchedObjectId);
                    }
                    componentSelectionObject_ = matchedObjectId;
                    for (int component : boxedComponents)
                        activeComponentSelection().insert(component);
                    // Box selection has no active component. Keep every
                    // boxed vertex or edge in the normal selection color.
                    activeComponentIndex() = -1;
                } else if (!additive) {
                    clearComponentSelections();
                    componentSelectionObject_ = ObjectId::invalid();
                    selection_.clear();
                }
            }
            update();
            emitCoordinateUpdate();
            return;
        }
        QVector<ObjectId> boxSelection;
        if (moved) {
            for (int index = 0; index < shapes_.size(); ++index) {
                const ObjectId objectId = shapes_.objectIdAt(index);
                if (!document_.isObjectEditable(objectId)) {
                    continue;
                }
                const SceneObject *sceneObject = document_.object(objectId);
                if (shapeMatchesSelectionBox(shapes_[index],
                                              selectionBox,
                                              crossingSelection,
                                              sceneObject != nullptr
                                                  ? sceneObject->placementTranslation
                                                  : Point3D{})) {
                    boxSelection.append(shapes_.objectIdAt(index));
                }
            }
        }

        selectTool->finishBoxSelection(boxSelection, toolContext_);

        DebugLog::instance().write(QStringLiteral("selection box finish moved=%1 crossing=%2 additive=%3 selected=%4")
                                       .arg(moved)
                                       .arg(crossingSelection)
                                       .arg(additive)
                                       .arg(selectedShapeIndices_.size()));
        resetTrimSelectionBox();
        setCursor(joinTool_.isActive() ? Qt::CrossCursor : Qt::ArrowCursor);
        update();
        emitCoordinateUpdate();
    }

    void cancelSelectionBox()
    {
        if (!selectionBoxOverlayActive()) {
            return;
        }

        const bool cancelingTrimBox = trimTool_.boxSelectionActive();
        resetSelectionBoxState();
        if (cancelingTrimBox) {
            trimTool_.clearCandidates();
            trimTool_.clearScreenPath();
            eraseTargetShapeIndices_.clear();
            eraseSceneCurveCaches_.clear();
            eraseTargetCurveCaches_.clear();
            eraseGeometryCachePrepared_ = false;
            trimTool_.invalidateHover();
        }
        setCursor(joinTool_.isActive() || activeTool_ == Tool::Trim
                      ? Qt::CrossCursor
                      : Qt::ArrowCursor);
        update();
        DebugLog::instance().write(QStringLiteral("selection box canceled"));
    }

    void notifySubdivisionStatus()
    {
        if (subdivisionStatusUpdate_) {
            subdivisionStatusUpdate_(subdivisionStatusText());
        }
    }

    void cancelSubdivisionPreview()
    {
        if (!subdivisionTool_.isActive()) {
            return;
        }

        subdivisionTool_.finish();
        notifySubdivisionStatus();
        update();
        DebugLog::instance().write(QStringLiteral("cancelSubdivisionPreview"));
    }

    void notifyHistoryChanged()
    {
        session_.notifyHistoryChanged();
        if (historyChanged_) {
            historyChanged_();
        }
    }

    void notifyLayersChanged()
    {
        session_.notifyLayersChanged();
        if (layersChanged_) {
            layersChanged_();
        }
    }

    void recordGeometryChange()
    {
        recordGeometrySnapshot(document_.snapshot());
    }

    bool commitShape(const Shape &shape)
    {
        DocumentTransaction transaction = session_.beginTransaction();
        if (!transaction.addShape(shape).isValid()) {
            return false;
        }
        return session_.commitTransaction(transaction);
    }

    void recordGeometrySnapshot(const Document::Snapshot &snapshot)
    {
        history_.record(snapshot);
        document_.invalidateAllGeometry();
        notifyHistoryChanged();
        QTimer::singleShot(0, this, [this]() {
            notifyLayersChanged();
        });
        DebugLog::instance().write(QStringLiteral("history record shapes=%1 undoAvailable=%2 redoCleared")
                                       .arg(shapes_.size())
                                       .arg(history_.undoCount()));
    }

    void beginDragHistory()
    {
        if (grabTool_.isActive()) {
            return;
        }
        if (!dragHistoryRecorded_) {
            recordGeometryChange();
            dragHistoryRecorded_ = true;
        }
    }

    void resetGrabInteraction()
    {
        grabTool_.reset();
        grabViewPlaneFrame_ = WorkPlaneFrame{};
        grabViewPlaneStartWorld_ = {};
        grabViewPlaneAnchorValid_ = false;
    }

    void resetDuplicateInteraction()
    {
        duplicateTool_.reset();
    }

    void resetInteractionAfterHistory()
    {
        pendingPoints_.clear();
        selection_.clear();
        resetSelectionBoxState();
        clearSelectionDragState();
        dragHistoryRecorded_ = false;
        currentSnap_ = SnapResult{};
        currentDragSnap_ = DragSnapResult{};
        dragSnapLocked_ = false;
        dragAxisLock_ = DragAxisLock::None;
        resetGrabInteraction();
        selection_.clearActiveControlPoint();
        eraseTool_.resetInteraction();
        trimTool_.clearCandidates();
        eraseTool_.clearCandidates();
        eraseTargetShapeIndices_.clear();
        eraseSceneCurveCaches_.clear();
        eraseTargetCurveCaches_.clear();
        eraseGeometryCachePrepared_ = false;
        trimTool_.invalidateHover();
        joinTool_.cancel();
        resetScaleInteraction();
        resetRotateInteraction();
        resetMirrorInteraction();
        subdivisionTool_.finish();
        lineCommandActive_ = false;
        setTool(Tool::Select);
        if (commandFinished_) {
            commandFinished_(Tool::Select);
        }
        update();
    }

    void finishLineCommand()
    {
        DebugLog::instance().write(QStringLiteral("finishLineCommand begin points=%1")
                                       .arg(pendingPoints_.size()));
        if (pendingPoints_.size() >= 2) {
            const Shape::NurbsCurve2D curve = makeDegreeOneNurbs(pendingPoints_);
            Shape line;
            line.geometryType = GeometryType::Line;
            line.points = pendingPoints_;
            line.nurbs = curve;
            line.workPlane = viewportTransform_.workPlane();
            line.workPlaneOffset = viewportTransform_.workPlaneOffset();
            line.workPlaneFrame = viewportTransform_.workPlaneFrame();
            if (!commitShape(line)) {
                return;
            }
            DebugLog::instance().write(
                QStringLiteral("finishLineCommand committed dimension=%1 degree=%2 order=%3 rational=%4 controlPoints=%5 weights=%6 knots=%7 shapes=%8")
                    .arg(curve.dimension)
                    .arg(curve.degree)
                    .arg(curve.order)
                    .arg(curve.rational)
                    .arg(curve.controlPoints.size())
                    .arg(curve.weights.size())
                    .arg(curve.knots.size())
                    .arg(shapes_.size()));
        } else {
            DebugLog::instance().write(QStringLiteral("finishLineCommand discarded insufficient points"));
        }

        pendingPoints_.clear();
        setTool(Tool::Select);
        if (commandFinished_) {
            commandFinished_(Tool::Select);
        }
        DebugLog::instance().write(QStringLiteral("finishLineCommand end tool=%1 lineActive=%2 points=%3")
                                       .arg(toolName(activeTool_))
                                       .arg(lineCommandActive_)
                                       .arg(pendingPoints_.size()));
    }

    bool makeArcSnapGeometry(const Shape &shape,
                             QPointF *centerScreen,
                             qreal *radius,
                             qreal *startAngle,
                             qreal *sweepAngle) const
    {
        if (shape.geometryType != GeometryType::Arc || shape.points.size() < 3) {
            return false;
        }

        if (shape.arcMode != ArcMode::OnePoint) {
            CircularArc2D arc;
            if (!makeCircularArcThroughPoint(shape.points[0],
                                             shape.points[1],
                                             shape.points[2],
                                             &arc)) {
                return false;
            }

            const QPointF center = worldToScreen(arc.center);
            const QPointF start = worldToScreen(shape.points[0]);
            const QPointF screenX = worldToScreen(arc.center + QPointF(1.0, 0.0)) - center;
            const QPointF screenY = worldToScreen(arc.center + QPointF(0.0, 1.0)) - center;
            const qreal orientation = screenX.x() * screenY.y() -
                                      screenX.y() * screenY.x();
            if (std::abs(orientation) <= 1.0e-12) {
                return false;
            }
            if (centerScreen != nullptr) {
                *centerScreen = center;
            }
            if (radius != nullptr) {
                *radius = std::hypot(start.x() - center.x(),
                                     start.y() - center.y());
            }
            if (startAngle != nullptr) {
                *startAngle = std::atan2(start.y() - center.y(),
                                         start.x() - center.x());
            }
            if (sweepAngle != nullptr) {
                *sweepAngle = orientation < 0.0 ? -arc.sweepAngle : arc.sweepAngle;
            }
            return true;
        }

        const QPointF localCenter = shape.points[0];
        const QPointF localStart = shape.points[1];
        const QPointF startVector = localStart - localCenter;
        const qreal localRadius = std::hypot(startVector.x(), startVector.y());
        if (localRadius <= 1e-9) {
            return false;
        }

        constexpr qreal pi = 3.14159265358979323846;
        constexpr qreal twoPi = 2.0 * pi;
        const QPointF center = worldToScreen(localCenter);
        const QPointF start = worldToScreen(localStart);
        const QPointF screenX = worldToScreen(localCenter + QPointF(1.0, 0.0)) - center;
        const QPointF screenY = worldToScreen(localCenter + QPointF(0.0, 1.0)) - center;
        const qreal orientation = screenX.x() * screenY.y() -
                                  screenX.y() * screenY.x();
        if (std::abs(orientation) <= 1.0e-12) {
            return false;
        }
        const qreal firstAngle = std::atan2(start.y() - center.y(),
                                            start.x() - center.x());
        qreal selectedSweep = shape.arcSweep;
        if (std::abs(selectedSweep) <= 1e-9) {
            const QPointF endVector = shape.points[2] - localCenter;
            const qreal endAngle = std::atan2(endVector.y(), endVector.x());
            const qreal localStartAngle = std::atan2(startVector.y(),
                                                     startVector.x());
            selectedSweep = endAngle - localStartAngle;
            if (selectedSweep > pi) {
                selectedSweep -= twoPi;
            } else if (selectedSweep < -pi) {
                selectedSweep += twoPi;
            }
        }

        if (centerScreen != nullptr) {
            *centerScreen = center;
        }
        if (radius != nullptr) {
            *radius = std::hypot(start.x() - center.x(),
                                 start.y() - center.y());
        }
        if (startAngle != nullptr) {
            *startAngle = firstAngle;
        }
        if (sweepAngle != nullptr) {
            *sweepAngle = orientation < 0.0 ? -selectedSweep : selectedSweep;
        }
        return true;
    }

    bool arcAngleIsOnSweep(qreal startAngle, qreal sweepAngle, qreal angle) const
    {
        constexpr qreal twoPi = 6.28318530717958647692;
        constexpr qreal epsilon = 1e-7;
        if (std::abs(sweepAngle) >= twoPi - epsilon) {
            return true;
        }

        const auto positiveAngle = [twoPi](qreal value) {
            value = std::fmod(value, twoPi);
            if (value < 0.0) {
                value += twoPi;
            }
            return value;
        };

        if (sweepAngle >= 0.0) {
            return positiveAngle(angle - startAngle) <= sweepAngle + epsilon;
        }

        return positiveAngle(startAngle - angle) <= -sweepAngle + epsilon;
    }

    bool arcSnapPointAtFraction(const Shape &shape,
                                qreal fraction,
                                QPointF *point) const
    {
        qreal parameterStart = 0.0;
        qreal parameterEnd = 0.0;
        if (isValidNurbsCurve(shape.nurbs) &&
            nurbsParameterDomain(shape.nurbs, &parameterStart, &parameterEnd)) {
            return point != nullptr &&
                   evaluateNurbsPoint(shape.nurbs,
                                      parameterStart +
                                          std::clamp(fraction, 0.0, 1.0) *
                                              (parameterEnd - parameterStart),
                                      point);
        }

        QPointF center;
        qreal radius = 0.0;
        qreal startAngle = 0.0;
        qreal sweepAngle = 0.0;
        if (!makeArcSnapGeometry(shape,
                                 &center,
                                 &radius,
                                 &startAngle,
                                 &sweepAngle)) {
            return false;
        }

        const qreal angle = startAngle + sweepAngle * fraction;
        if (point != nullptr) {
            *point = screenToWorld(QPointF(center.x() + radius * std::cos(angle),
                                           center.y() + radius * std::sin(angle)));
        }
        return true;
    }

    Shape::NurbsCurve2D makeArcNurbsCurve(const Shape &shape) const
    {
        if (shape.arcMode == ArcMode::OnePoint) {
            if (shape.points.size() < 3) {
                return {};
            }
            const QPointF radiusVector = shape.points[1] - shape.points[0];
            const qreal radius = std::hypot(radiusVector.x(),
                                            radiusVector.y());
            qreal sweepAngle = shape.arcSweep;
            if (std::abs(sweepAngle) <= 1.0e-9) {
                constexpr qreal pi = 3.14159265358979323846;
                constexpr qreal twoPi = 2.0 * pi;
                const QPointF endVector = shape.points[2] - shape.points[0];
                const qreal startAngle = std::atan2(radiusVector.y(),
                                                    radiusVector.x());
                sweepAngle = std::atan2(endVector.y(), endVector.x()) - startAngle;
                if (sweepAngle > pi) {
                    sweepAngle -= twoPi;
                } else if (sweepAngle < -pi) {
                    sweepAngle += twoPi;
                }
            }
            CircularArc2D arc;
            return makeCircularArcFromCenterSweep(
                       shape.points[0],
                       radius,
                       std::atan2(radiusVector.y(), radiusVector.x()),
                       sweepAngle,
                       &arc)
                       ? arc.curve
                       : Shape::NurbsCurve2D{};
        }

        if (shape.arcMode != ArcMode::OnePoint) {
            if (shape.points.size() < 3) {
                return {};
            }
            CircularArc2D arc;
            return makeCircularArcThroughPoint(shape.points[0],
                                               shape.points[1],
                                               shape.points[2],
                                               &arc)
                       ? arc.curve
                       : Shape::NurbsCurve2D{};
        }

        Shape::NurbsCurve2D curve;
        curve.dimension = 2;
        curve.degree = 2;
        curve.order = 3;
        curve.rational = true;

        QPointF centerScreen;
        qreal radius = 0.0;
        qreal startAngle = 0.0;
        qreal sweepAngle = 0.0;
        if (!makeArcSnapGeometry(shape,
                                 &centerScreen,
                                 &radius,
                                 &startAngle,
                                 &sweepAngle) ||
            radius <= 1e-9 || std::abs(sweepAngle) <= 1e-9) {
            return curve;
        }

        constexpr qreal pi = 3.14159265358979323846;
        constexpr qreal halfPi = pi / 2.0;
        const int spanCount = std::max(1, static_cast<int>(std::ceil(
            std::abs(sweepAngle) / halfPi)));
        const qreal spanSweep = sweepAngle / spanCount;

        const auto screenPointAt = [centerScreen, radius](qreal angle) {
            return QPointF(centerScreen.x() + radius * std::cos(angle),
                           centerScreen.y() + radius * std::sin(angle));
        };

        curve.controlPoints.reserve(spanCount * 2 + 1);
        curve.weights.reserve(spanCount * 2 + 1);
        for (int span = 0; span < spanCount; ++span) {
            const qreal spanStart = startAngle + spanSweep * span;
            const qreal spanEnd = spanStart + spanSweep;
            const qreal spanMiddle = (spanStart + spanEnd) * 0.5;
            const qreal middleWeight = std::cos(std::abs(spanSweep) * 0.5);

            if (span == 0) {
                curve.controlPoints.append(screenToWorld(screenPointAt(spanStart)));
                curve.weights.append(1.0);
            }

            // A circular span is an exact rational quadratic Bezier. The
            // middle CV lies outside the circle and its weight controls the
            // pull back onto the circle. Adjacent spans share their endpoint.
            const qreal middleRadius = radius / middleWeight;
            const QPointF middleScreen(
                centerScreen.x() + middleRadius * std::cos(spanMiddle),
                centerScreen.y() + middleRadius * std::sin(spanMiddle));
            curve.controlPoints.append(screenToWorld(middleScreen));
            curve.weights.append(middleWeight);
            curve.controlPoints.append(screenToWorld(screenPointAt(spanEnd)));
            curve.weights.append(1.0);
        }

        // Clamped knot vector for piecewise rational quadratic Bezier spans:
        // endpoint multiplicity 3 and internal knot multiplicity 2. The
        // first and last redundant entries are omitted, as in openNURBS.
        const qreal knotDelta = std::abs(spanSweep);
        curve.knots.reserve(curve.controlPoints.size() + curve.degree - 1);
        curve.knots.append(0.0);
        curve.knots.append(0.0);
        for (int knot = 1; knot < spanCount; ++knot) {
            const double parameter = knotDelta * knot;
            curve.knots.append(parameter);
            curve.knots.append(parameter);
        }
        const double endParameter = std::abs(sweepAngle);
        curve.knots.append(endParameter);
        curve.knots.append(endParameter);

        return curve;
    }

    QVector<SnapCandidate> snapCandidatesForShape(const Shape &shape) const
    {
        return snapEngine_.snapCandidatesForShape(shape,
                                                  viewportTransform_,
                                                  size());

        QVector<SnapCandidate> candidates;
        if (shape.points.isEmpty()) {
            return candidates;
        }

        Shape::NurbsCurve2D subdivisionCurveData;
        if (subdivisionCurveForShape(shape, &subdivisionCurveData)) {
            for (const double parameter : shape.subdivisionParameters) {
                QPointF point;
                if (evaluateNurbsPoint(subdivisionCurveData, parameter, &point)) {
                    candidates.append(SnapCandidate{SnapType::Endpoint, point});
                }
            }
        }

        if (shape.geometryType == GeometryType::Point) {
            candidates.append(SnapCandidate{SnapType::Endpoint, shape.points.first()});
            return candidates;
        }

        if (shape.geometryType == GeometryType::PolyCurve) {
            for (const Shape::NurbsCurve2D &component : shape.components) {
                QPointF start;
                QPointF end;
                if (nurbsCurveEndpoints(component, &start, &end)) {
                    candidates.append(SnapCandidate{SnapType::Endpoint, start});
                    candidates.append(SnapCandidate{SnapType::Endpoint, end});
                }
                QPointF midpoint;
                if (nurbsCurvePointAtFraction(component, 0.5, &midpoint)) {
                    candidates.append(SnapCandidate{SnapType::Midpoint, midpoint});
                }
            }
            return candidates;
        }

        if (shape.geometryType == GeometryType::Circle) {
            candidates.append(SnapCandidate{SnapType::Center, shape.points.first()});
            return candidates;
        }

        if (shape.geometryType == GeometryType::Rectangle) {
            const QVector<QPointF> vertices = rectangleVertices(shape);
            for (const QPointF &vertex : vertices) {
                candidates.append(SnapCandidate{SnapType::Endpoint, vertex});
            }
            for (int index = 0; index < vertices.size(); ++index) {
                candidates.append(SnapCandidate{
                    SnapType::Midpoint,
                    (vertices[index] + vertices[(index + 1) % vertices.size()]) / 2.0});
            }
            return candidates;
        }

        if (shape.geometryType == GeometryType::Arc && shape.points.size() >= 3) {
            QPointF start = shape.arcMode == ArcMode::OnePoint
                                ? shape.points[1]
                                : shape.points[0];
            QPointF end = shape.arcMode == ArcMode::OnePoint
                              ? shape.points[2]
                              : shape.points[1];
            // The stored NURBS is the rendered source of truth. In
            // particular, this remains correct after a control point edit,
            // when the legacy three-point arc definition no longer describes
            // the displayed curve exactly.
            if (!nurbsCurveEndpoints(shape.nurbs, &start, &end)) {
                QPointF evaluatedEndpoint;
                if (arcSnapPointAtFraction(shape, 0.0, &evaluatedEndpoint)) {
                    start = evaluatedEndpoint;
                }
                if (arcSnapPointAtFraction(shape, 1.0, &evaluatedEndpoint)) {
                    end = evaluatedEndpoint;
                }
            }
            candidates.append(SnapCandidate{SnapType::Endpoint, start});
            candidates.append(SnapCandidate{SnapType::Endpoint, end});

            QPointF midpoint;
            if (arcSnapPointAtFraction(shape, 0.5, &midpoint)) {
                candidates.append(SnapCandidate{SnapType::Midpoint, midpoint});
            }

            QPointF center;
            if (makeArcSnapGeometry(shape, &center, nullptr, nullptr, nullptr)) {
                candidates.append(SnapCandidate{SnapType::Center, screenToWorld(center)});
            }
            return candidates;
        }

        if (shape.geometryType != GeometryType::Line) {
            return candidates;
        }

        QVector<LineSegment> segments;
        // A moved object may use any of its snap points as the source. The
        // OSnap toggles control which target types are eligible below.
        for (const QPointF &point : shape.points) {
            candidates.append(SnapCandidate{SnapType::Endpoint, point});
        }

        for (int index = 0; index + 1 < shape.points.size(); ++index) {
            const QPointF start = shape.points[index];
            const QPointF end = shape.points[index + 1];
            segments.append(LineSegment{start, end});

            candidates.append(SnapCandidate{SnapType::Midpoint, (start + end) / 2.0});
        }

        for (int first = 0; first < segments.size(); ++first) {
            for (int second = first + 1; second < segments.size(); ++second) {
                QPointF intersection;
                if (segmentIntersection(segments[first].start,
                                        segments[first].end,
                                        segments[second].start,
                                        segments[second].end,
                                        &intersection)) {
                    candidates.append(SnapCandidate{SnapType::Intersection, intersection});
                }
            }
        }

        return candidates;
    }

    QVector<SnapCandidate> snapCandidatesForScene(int excludedShapeIndex = -1) const
    {
        const QVector<int> excludedShapeIndices = excludedShapeIndex >= 0
                                                      ? QVector<int>{excludedShapeIndex}
                                                      : QVector<int>{};
        return snapCandidatesForScene(excludedShapeIndices);
    }

    QVector<SnapCandidate> snapCandidatesForScene(
        const QVector<int> &excludedShapeIndices) const
    {
        return snapEngine_.snapCandidatesForScene(document_,
                                                  excludedShapeIndices,
                                                  viewportTransform_,
                                                  size());

        QVector<SnapCandidate> candidates;
        QVector<LineSegment> segments;

        for (int shapeIndex = 0; shapeIndex < shapes_.size(); ++shapeIndex) {
            if (excludedShapeIndices.contains(shapeIndex)) {
                continue;
            }

            const Shape &shape = shapes_[shapeIndex];
            if (shape.points.isEmpty()) {
                continue;
            }

            if (endpointSnapEnabled_) {
                Shape::NurbsCurve2D subdivisionCurveData;
                if (subdivisionCurveForShape(shape, &subdivisionCurveData)) {
                    for (const double parameter : shape.subdivisionParameters) {
                        QPointF point;
                        if (evaluateNurbsPoint(subdivisionCurveData, parameter, &point)) {
                            candidates.append(SnapCandidate{SnapType::Endpoint, point});
                        }
                    }
                }
            }

            if (shape.geometryType == GeometryType::Point) {
                if (endpointSnapEnabled_) {
                    candidates.append(SnapCandidate{SnapType::Endpoint, shape.points.first()});
                }
                continue;
            }

            if (shape.geometryType == GeometryType::PolyCurve) {
                for (const Shape::NurbsCurve2D &component : shape.components) {
                    QPointF start;
                    QPointF end;
                    if (endpointSnapEnabled_ &&
                        nurbsCurveEndpoints(component, &start, &end)) {
                        candidates.append(SnapCandidate{SnapType::Endpoint, start});
                        candidates.append(SnapCandidate{SnapType::Endpoint, end});
                    }
                    if (midpointSnapEnabled_) {
                        QPointF midpoint;
                        if (nurbsCurvePointAtFraction(component, 0.5, &midpoint)) {
                            candidates.append(SnapCandidate{SnapType::Midpoint, midpoint});
                        }
                    }
                }
                continue;
            }

            if (shape.geometryType == GeometryType::Circle) {
                if (centerSnapEnabled_) {
                    candidates.append(SnapCandidate{SnapType::Center, shape.points.first()});
                }
                continue;
            }

            if (shape.geometryType == GeometryType::Rectangle) {
                const QVector<QPointF> vertices = rectangleVertices(shape);
                if (endpointSnapEnabled_) {
                    for (const QPointF &vertex : vertices) {
                        candidates.append(SnapCandidate{SnapType::Endpoint, vertex});
                    }
                }
                for (int index = 0; index < vertices.size(); ++index) {
                    const QPointF start = vertices[index];
                    const QPointF end = vertices[(index + 1) % vertices.size()];
                    segments.append(LineSegment{start, end});
                    if (midpointSnapEnabled_) {
                        candidates.append(SnapCandidate{
                            SnapType::Midpoint,
                            (start + end) / 2.0});
                    }
                }
                continue;
            }

            if (shape.geometryType == GeometryType::Arc && shape.points.size() >= 3) {
                QPointF start = shape.arcMode == ArcMode::OnePoint
                                    ? shape.points[1]
                                    : shape.points[0];
                QPointF end = shape.arcMode == ArcMode::OnePoint
                                  ? shape.points[2]
                                  : shape.points[1];
                // Use the same NURBS data that drawShape() renders. The
                // legacy arc construction points may be stale after control
                // point editing.
                if (!nurbsCurveEndpoints(shape.nurbs, &start, &end)) {
                    QPointF evaluatedEndpoint;
                    if (arcSnapPointAtFraction(shape, 0.0, &evaluatedEndpoint)) {
                        start = evaluatedEndpoint;
                    }
                    if (arcSnapPointAtFraction(shape, 1.0, &evaluatedEndpoint)) {
                        end = evaluatedEndpoint;
                    }
                }

                if (endpointSnapEnabled_) {
                    candidates.append(SnapCandidate{SnapType::Endpoint, start});
                    candidates.append(SnapCandidate{SnapType::Endpoint, end});
                }

                if (midpointSnapEnabled_) {
                    QPointF midpoint;
                    if (arcSnapPointAtFraction(shape, 0.5, &midpoint)) {
                        candidates.append(SnapCandidate{SnapType::Midpoint, midpoint});
                    }
                }

                if (centerSnapEnabled_) {
                    QPointF center;
                    if (makeArcSnapGeometry(shape, &center, nullptr, nullptr, nullptr)) {
                        candidates.append(SnapCandidate{
                            SnapType::Center,
                            screenToWorld(center)});
                    }
                }

                continue;
            }

            if (shape.geometryType != GeometryType::Line) {
                continue;
            }

            if (endpointSnapEnabled_) {
                for (const QPointF &point : shape.points) {
                    candidates.append(SnapCandidate{SnapType::Endpoint, point});
                }
            }

            for (int index = 0; index + 1 < shape.points.size(); ++index) {
                const QPointF start = shape.points[index];
                const QPointF end = shape.points[index + 1];
                segments.append(LineSegment{start, end});

                if (midpointSnapEnabled_) {
                    candidates.append(SnapCandidate{SnapType::Midpoint, (start + end) / 2.0});
                }
            }
        }

        if (intersectionSnapEnabled_) {
            for (int first = 0; first < segments.size(); ++first) {
                for (int second = first + 1; second < segments.size(); ++second) {
                    QPointF intersection;
                    if (segmentIntersection(segments[first].start,
                                            segments[first].end,
                                            segments[second].start,
                                            segments[second].end,
                                            &intersection)) {
                        candidates.append(SnapCandidate{SnapType::Intersection, intersection});
                    }
                }
            }
        }

        return candidates;
    }

    QVector<SnapCandidate> perpendicularCandidates(const QPointF &origin,
                                                    const QPointF &cursor) const
    {
        return snapEngine_.perpendicularCandidates(document_,
                                                   origin,
                                                   cursor,
                                                   viewportTransform_,
                                                   size());

        QVector<SnapCandidate> candidates;
        if (!perpendicularSnapEnabled_) {
            return candidates;
        }

        constexpr qreal epsilon = 1e-9;

        for (const Shape &shape : shapes_) {
            if (shape.points.isEmpty()) {
                continue;
            }

            if (shape.geometryType == GeometryType::Circle && shape.points.size() >= 2) {
                const QPointF center = shape.points[0];
                const QPointF edge = shape.points[1];
                const qreal radius = std::hypot(edge.x() - center.x(),
                                                edge.y() - center.y());
                if (radius <= epsilon) {
                    continue;
                }

                const QPointF fromCenter = origin - center;
                const qreal distanceFromCenter =
                    std::hypot(fromCenter.x(), fromCenter.y());

                if (distanceFromCenter <= epsilon) {
                    // From a circle center, every radius is perpendicular to
                    // the circumference. Use the cursor direction to choose
                    // which point on the circumference to target.
                    const QPointF towardCursor = cursor - center;
                    const qreal cursorDistance =
                        std::hypot(towardCursor.x(), towardCursor.y());
                    if (cursorDistance > epsilon) {
                        candidates.append(SnapCandidate{
                            SnapType::Perpendicular,
                            center + towardCursor * (radius / cursorDistance)});
                    }
                } else {
                    const QPointF radialDirection = fromCenter / distanceFromCenter;
                    candidates.append(SnapCandidate{
                        SnapType::Perpendicular,
                        center + radialDirection * radius});
                    candidates.append(SnapCandidate{
                        SnapType::Perpendicular,
                        center - radialDirection * radius});
                }

                continue;
            }

            if (shape.geometryType == GeometryType::Arc && shape.points.size() >= 3) {
                QPointF center;
                qreal radius = 0.0;
                qreal startAngle = 0.0;
                qreal sweepAngle = 0.0;
                if (!makeArcSnapGeometry(shape,
                                         &center,
                                         &radius,
                                         &startAngle,
                                         &sweepAngle)) {
                    continue;
                }

                const QPointF originScreen = worldToScreen(origin);
                const QPointF fromCenter = originScreen - center;
                const qreal distanceFromCenter =
                    std::hypot(fromCenter.x(), fromCenter.y());

                const auto appendIfOnArc = [&](const QPointF &candidateScreen) {
                    const qreal candidateAngle =
                        std::atan2(candidateScreen.y() - center.y(),
                                   candidateScreen.x() - center.x());
                    if (arcAngleIsOnSweep(startAngle, sweepAngle, candidateAngle)) {
                        candidates.append(SnapCandidate{
                            SnapType::Perpendicular,
                            screenToWorld(candidateScreen)});
                    }
                };

                if (distanceFromCenter <= epsilon) {
                    const QPointF cursorScreen = worldToScreen(cursor);
                    const QPointF towardCursor = cursorScreen - center;
                    const qreal cursorDistance =
                        std::hypot(towardCursor.x(), towardCursor.y());
                    if (cursorDistance > epsilon) {
                        appendIfOnArc(center + towardCursor * (radius / cursorDistance));
                    }
                } else {
                    const QPointF radialDirection = fromCenter / distanceFromCenter;
                    appendIfOnArc(center + radialDirection * radius);
                    appendIfOnArc(center - radialDirection * radius);
                }

                continue;
            }

            if (shape.geometryType != GeometryType::Line) {
                continue;
            }

            for (int index = 0; index + 1 < shape.points.size(); ++index) {
                const QPointF start = shape.points[index];
                const QPointF end = shape.points[index + 1];
                const QPointF direction = end - start;
                const qreal lengthSquared = direction.x() * direction.x() +
                                            direction.y() * direction.y();
                if (lengthSquared <= epsilon) {
                    continue;
                }

                const QPointF fromStart = origin - start;
                const qreal projection =
                    (fromStart.x() * direction.x() + fromStart.y() * direction.y()) /
                    lengthSquared;
                if (projection < -epsilon || projection > 1.0 + epsilon) {
                    continue;
                }

                const QPointF foot = start + direction * std::clamp(projection, 0.0, 1.0);
                if (std::hypot(origin.x() - foot.x(), origin.y() - foot.y()) <= epsilon) {
                    continue;
                }

                candidates.append(SnapCandidate{SnapType::Perpendicular, foot});
            }
        }

        return candidates;
    }

    QVector<SnapCandidate> tangentCandidates(const QPointF &origin) const
    {
        return snapEngine_.tangentCandidates(document_,
                                              origin,
                                              viewportTransform_,
                                              size());

        QVector<SnapCandidate> candidates;
        if (!tangentSnapEnabled_) {
            return candidates;
        }

        constexpr qreal epsilon = 1e-9;

        for (const Shape &shape : shapes_) {
            if (shape.geometryType == GeometryType::Circle && shape.points.size() >= 2) {
                const QPointF center = shape.points[0];
                const QPointF edge = shape.points[1];
                const qreal radius = std::hypot(edge.x() - center.x(),
                                                edge.y() - center.y());
                if (radius <= epsilon) {
                    continue;
                }

                const QPointF fromCenter = origin - center;
                const qreal distanceFromCenter =
                    std::hypot(fromCenter.x(), fromCenter.y());
                if (distanceFromCenter < radius - epsilon) {
                    // A point inside a circle has no real tangent points.
                    continue;
                }

                if (distanceFromCenter <= epsilon) {
                    continue;
                }

                const QPointF radialDirection = fromCenter / distanceFromCenter;
                const QPointF tangentDirection(-radialDirection.y(), radialDirection.x());
                const qreal radiusRatio = radius / distanceFromCenter;
                const qreal radialDistance = radius * radiusRatio;
                const qreal tangentDistance =
                    radius * std::sqrt(std::max(0.0, 1.0 - radiusRatio * radiusRatio));

                candidates.append(SnapCandidate{
                    SnapType::Tangent,
                    center + radialDirection * radialDistance + tangentDirection * tangentDistance});

                if (tangentDistance > epsilon) {
                    candidates.append(SnapCandidate{
                        SnapType::Tangent,
                        center + radialDirection * radialDistance - tangentDirection * tangentDistance});
                }
                continue;
            }

            if (shape.geometryType != GeometryType::Arc || shape.points.size() < 3) {
                continue;
            }

            QPointF centerScreen;
            qreal radius = 0.0;
            qreal startAngle = 0.0;
            qreal sweepAngle = 0.0;
            if (!makeArcSnapGeometry(shape,
                                     &centerScreen,
                                     &radius,
                                     &startAngle,
                                     &sweepAngle)) {
                continue;
            }

            const QPointF originScreen = worldToScreen(origin);
            const QPointF fromCenter = originScreen - centerScreen;
            const qreal distanceFromCenter =
                std::hypot(fromCenter.x(), fromCenter.y());
            if (distanceFromCenter < radius - epsilon ||
                distanceFromCenter <= epsilon) {
                continue;
            }

            const QPointF radialDirection = fromCenter / distanceFromCenter;
            const QPointF tangentDirection(-radialDirection.y(), radialDirection.x());
            const qreal radiusRatio = radius / distanceFromCenter;
            const qreal radialDistance = radius * radiusRatio;
            const qreal tangentDistance =
                radius * std::sqrt(std::max(0.0, 1.0 - radiusRatio * radiusRatio));

            const auto appendIfOnArc = [&](const QPointF &candidateScreen) {
                const qreal candidateAngle =
                    std::atan2(candidateScreen.y() - centerScreen.y(),
                               candidateScreen.x() - centerScreen.x());
                if (arcAngleIsOnSweep(startAngle, sweepAngle, candidateAngle)) {
                    candidates.append(SnapCandidate{
                        SnapType::Tangent,
                        screenToWorld(candidateScreen)});
                }
            };

            appendIfOnArc(centerScreen + radialDirection * radialDistance +
                          tangentDirection * tangentDistance);
            if (tangentDistance > epsilon) {
                appendIfOnArc(centerScreen + radialDirection * radialDistance -
                              tangentDirection * tangentDistance);
            }
        }

        return candidates;
    }

    SnapResult findSnapPoint(const QPointF &rawPoint) const
    {
        const bool traceSnaps = qEnvironmentVariable("CLASSICAD_SNAP_TRACE") ==
                                QStringLiteral("1");
        const bool traceAngularDimension =
            activeTool_ == Tool::AngularDimension;
        const auto traceSnapResult = [&](const SnapResult &result,
                                         bool spatial) {
            if (!traceSnaps && !traceAngularDimension) {
                return;
            }

            const WorkPlaneFrame activeFrame = viewportTransform_.workPlaneFrame();
            const QPointF cursorScreen = worldToScreen(rawPoint);
            QPointF markerScreen = cursorScreen;
            if (result.isValid()) {
                markerScreen = worldToScreen(result.point);
            }
            Point3D resultWorld = workPlaneFramePointToWorld(rawPoint, activeFrame);
            QPointF resultWorldScreen = markerScreen;
            if (result.isValid()) {
                resultWorld = result.hasWorldPoint
                                  ? result.worldPoint
                                  : workPlaneFramePointToWorld(result.point,
                                                               activeFrame);
                viewportTransform_.worldPointToScreen(resultWorld,
                                                      size(),
                                                      &resultWorldScreen);
            }
            const qreal markerTargetGap = result.isValid()
                ? std::hypot(markerScreen.x() - resultWorldScreen.x(),
                             markerScreen.y() - resultWorldScreen.y())
                : 0.0;
            const auto point3DText = [](const Point3D &point) {
                return QStringLiteral("(%1, %2, %3)")
                    .arg(point.x, 0, 'g', 12)
                    .arg(point.y, 0, 'g', 12)
                    .arg(point.z, 0, 'g', 12);
            };
            const auto frameText = [&point3DText](const WorkPlaneFrame &frame) {
                return QStringLiteral("origin=%1 x=%2 y=%3 n=%4")
                    .arg(point3DText(frame.origin),
                         point3DText(frame.xAxis),
                         point3DText(frame.yAxis),
                         point3DText(frame.normal));
            };

            DebugLog::instance().write(
                QStringLiteral("osnap-trace selected tool=%1 mode=%2 type=%3 rawLocal=%4 cursorPx=%5 resultLocal=%6 markerPx=%7 resultWorld=%8 resultWorldPx=%9 markerWorldGapPx=%10 activeFrame={%11}")
                    .arg(toolName(activeTool_),
                         spatial ? QStringLiteral("spatial")
                                 : QStringLiteral("planar"),
                         snapTypeName(result.type),
                         precisePointText(rawPoint),
                         precisePointText(cursorScreen),
                         precisePointText(result.point),
                         precisePointText(markerScreen),
                         point3DText(resultWorld),
                         precisePointText(resultWorldScreen))
                    .arg(markerTargetGap, 0, 'f', 4)
                    .arg(frameText(activeFrame)));

            const SnapSettings settings = snapEngine_.settings();
            const auto snapTypeEnabled = [&settings](SnapType type) {
                switch (type) {
                case SnapType::Endpoint: return settings.endpoint;
                case SnapType::Midpoint: return settings.midpoint;
                case SnapType::Intersection: return settings.intersection;
                case SnapType::Center: return settings.center;
                case SnapType::Perpendicular: return settings.perpendicular;
                case SnapType::Tangent: return settings.tangent;
                case SnapType::ControlPoint: return settings.controlPoint;
                case SnapType::Near: return settings.near;
                case SnapType::None: return false;
                }
                return false;
            };
            for (int shapeIndex = 0; shapeIndex < document_.size(); ++shapeIndex) {
                const Shape &shape = document_[shapeIndex];
                const ObjectId objectId = document_.objectIdAt(shapeIndex);
                const SceneObject *sceneObject = document_.object(objectId);
                const Point3D worldOffset = sceneObject != nullptr
                                                ? sceneObject->placementTranslation
                                                : Point3D{};
                const bool visible = document_.isObjectVisible(objectId);
                const WorkPlaneFrame targetFrame = shapeWorkPlaneFrame(shape);
                const bool frameMatches = workPlaneMatches(targetFrame, activeFrame);
                const QVector<SnapCandidate> candidates =
                    visible ? snapEngine_.snapCandidatesForShape(shape,
                                                                 viewportTransform_,
                                                                 size(),
                                                                 worldOffset)
                            : QVector<SnapCandidate>{};
                if (candidates.isEmpty()) {
                    DebugLog::instance().write(
                        QStringLiteral("osnap-trace shape shape=%1 geometry=%2 visible=%3 activeFrameMatch=%4 candidateCount=0 reason=%5 targetFrame={%6}")
                            .arg(shapeIndex)
                            .arg(geometryTypeName(shape.geometryType))
                            .arg(visible)
                            .arg(frameMatches)
                            .arg(!visible ? QStringLiteral("hidden")
                                          : QStringLiteral("no-supported-snap-candidates"))
                            .arg(frameText(targetFrame)));
                    continue;
                }
                for (const SnapCandidate &candidate : candidates) {
                    const Point3D candidateWorld =
                        workPlaneFramePointToWorld(candidate.point, targetFrame);
                    QPointF candidateScreen;
                    const bool projectable = viewportTransform_.worldPointToScreen(
                        candidateWorld, size(), &candidateScreen);
                    QPointF unclippedCandidateScreen;
                    const bool projectableWithoutClip =
                        viewportTransform_.worldPointToScreenUnclipped(
                            candidateWorld, size(), &unclippedCandidateScreen);
                    const QPointF diagnosticCandidateScreen = projectable
                        ? candidateScreen
                        : unclippedCandidateScreen;
                    const qreal cursorDistance =
                        (projectable || projectableWithoutClip)
                            ? std::hypot(diagnosticCandidateScreen.x() - cursorScreen.x(),
                                         diagnosticCandidateScreen.y() - cursorScreen.y())
                            : std::numeric_limits<qreal>::infinity();
                    const qreal resultCandidateDistance =
                        std::hypot(candidateWorld.x - resultWorld.x,
                                   candidateWorld.y - resultWorld.y,
                                   candidateWorld.z - resultWorld.z);
                    const bool selectedCandidate = result.isValid() &&
                        candidate.type == result.type &&
                        (spatial || frameMatches) &&
                        resultCandidateDistance <= 1.0e-7;
                    if (!selectedCandidate && cursorDistance > 36.0) {
                        continue;
                    }
                    const QPointF activePlanePoint =
                        worldPointToWorkPlaneFrame(candidateWorld, activeFrame);
                    const QPointF activePlaneScreen = worldToScreen(activePlanePoint);
                    const bool enabled = snapTypeEnabled(candidate.type);
                    const bool inRadius = cursorDistance <= 12.0;
                    const QString reason = !visible
                        ? QStringLiteral("hidden")
                        : (!frameMatches
                               ? QStringLiteral("workplane-mismatch")
                               : (!enabled
                                      ? QStringLiteral("snap-type-disabled")
                                      : (!projectable
                                             ? (projectableWithoutClip
                                                    ? QStringLiteral("outside-clip-range")
                                                    : QStringLiteral("projection-failed"))
                                             : (!inRadius
                                                    ? QStringLiteral("outside-snap-radius")
                                                    : (selectedCandidate
                                                           ? QStringLiteral("selected")
                                                           : QStringLiteral("eligible-not-selected"))))));
                    DebugLog::instance().write(
                        QStringLiteral("osnap-trace candidate shape=%1 geometry=%2 type=%3 selected=%4 enabled=%5 visible=%6 activeFrameMatch=%7 pointLocal=%8 pointWorld=%9 targetPx=%10 unclippedPx=%11 activePlanePx=%12 cursorDistancePx=%13 reason=%14 targetFrame={%15}")
                            .arg(shapeIndex)
                            .arg(geometryTypeName(shape.geometryType),
                                 snapTypeName(candidate.type),
                                 selectedCandidate ? QStringLiteral("1")
                                                   : QStringLiteral("0"),
                                 enabled ? QStringLiteral("1") : QStringLiteral("0"),
                                 visible ? QStringLiteral("1") : QStringLiteral("0"),
                                 frameMatches ? QStringLiteral("1")
                                              : QStringLiteral("0"),
                                 precisePointText(candidate.point),
                                 point3DText(candidateWorld),
                                 projectable ? precisePointText(candidateScreen)
                                             : QStringLiteral("unprojectable"),
                                 projectableWithoutClip
                                     ? precisePointText(unclippedCandidateScreen)
                                     : QStringLiteral("unprojectable"),
                                 precisePointText(activePlaneScreen))
                            .arg(cursorDistance, 0, 'f', 4)
                            .arg(reason, frameText(targetFrame)));
                }
            }
        };

        if (activeTool_ == Tool::Rotate) {
            const Point3D *anchor = nullptr;
            if (rotateState().stage == 1) {
                anchor = &rotateState().baseWorldPoint;
            } else if (rotateState().stage == 2) {
                anchor = &rotateState().referenceWorldPoint;
            }
            const SnapResult result = snapEngine_.findSpatialSnapPoint(
                document_, worldToScreen(rawPoint), anchor,
                viewportTransform_, size());
            traceSnapResult(result, true);
            return result;
        }

        const bool arcPlaneConstraintActive =
            activeTool_ == Tool::Arc &&
            (arcState().planeNormalLockKey != 0 || arcState().axisConstraintKey != 0 ||
             arcState().verticalOverrideAxis != 0 || arcState().perpendicularPlaneActive);
        if (arcPlaneConstraintActive) {
            Point3D anchorWorld;
            const Point3D *anchor = nullptr;
            const WorkPlaneFrame frame = viewportTransform_.workPlaneFrame();
            if (!pendingPoints_.isEmpty() && isValidWorkPlaneFrame(frame)) {
                anchorWorld = workPlaneFramePointToWorld(pendingPoints_.back(),
                                                         frame);
                anchor = &anchorWorld;
            }
            const SnapResult result = snapEngine_.findSpatialSnapPoint(
                document_, worldToScreen(rawPoint), anchor,
                viewportTransform_, size());
            traceSnapResult(result, true);
            return result;
        }

        if (activeTool_ == Tool::Line && lineCommandActive_) {
            const Point3D *anchor = toolPreview_.worldPoints.isEmpty()
                ? nullptr : &toolPreview_.worldPoints.back();
            const SnapResult result = snapEngine_.findSpatialSnapPoint(
                document_, worldToScreen(rawPoint), anchor,
                viewportTransform_, size(), toolPreview_.worldPoints);
            traceSnapResult(result, true);
            return result;
        }

        if (activeTool_ == Tool::PointEdgeCenter) {
            SnapEngine midpointSnapEngine = snapEngine_;
            midpointSnapEngine.setSettings(
                SnapSettings{true, false, true, false, false, false, false, false, false});
            const SnapResult result = midpointSnapEngine.findEdgeCenterSnapPoint(
                document_, worldToScreen(rawPoint), viewportTransform_, size());
            traceSnapResult(result, true);
            return result;
        }

        if (activeTool_ == Tool::PointByLine ||
            activeTool_ == Tool::PointByArcs) {
            const Point3D *anchor = activeTool_ == Tool::PointByLine &&
                                            !toolPreview_.worldPoints.isEmpty()
                                        ? &toolPreview_.worldPoints.back()
                                        : nullptr;
            const SnapResult result = snapEngine_.findSpatialSnapPoint(
                document_, worldToScreen(rawPoint), anchor,
                viewportTransform_, size(), toolPreview_.worldPoints);
            traceSnapResult(result, true);
            return result;
        }

        const bool serviceDrawingSnapActive =
            (activeTool_ == Tool::Line && lineCommandActive_) ||
            activeTool_ == Tool::Arc || activeTool_ == Tool::Point ||
            isCurveCreationTool(activeTool_) ||
            isCircleConstructionTool(activeTool_) ||
            isEllipseTool(activeTool_) || isRectangleTool(activeTool_) ||
            isPolygonTool(activeTool_) ||
            activeTool_ == Tool::Scale || activeTool_ == Tool::Picture ||
            activeTool_ == Tool::Mirror || isDimensionTool(activeTool_) ||
            activeTool_ == Tool::TangentFromCurve ||
            activeTool_ == Tool::PerpendicularFromCurve;
        const SnapResult result = snapEngine_.findSnapPoint(
            document_, rawPoint, serviceDrawingSnapActive, pendingPoints_,
            viewportTransform_, size());
        traceSnapResult(result, false);
        return result;

        SnapResult best;
        const bool drawingSnapActive =
            (activeTool_ == Tool::Line && lineCommandActive_) ||
            activeTool_ == Tool::Arc || activeTool_ == Tool::PointByArcs ||
            activeTool_ == Tool::CurveFreehand ||
            isCircleConstructionTool(activeTool_) ||
            activeTool_ == Tool::Point || activeTool_ == Tool::PointByLine ||
            activeTool_ == Tool::CurveInterpolate || activeTool_ == Tool::Rotate;
        if (!osnapEnabled_ || !drawingSnapActive) {
            return best;
        }

        const QPointF cursorScreen = worldToScreen(rawPoint);
        constexpr qreal snapRadiusPixels = 12.0;
        qreal bestDistance = snapRadiusPixels;

        const auto consider = [&](const SnapCandidate &candidate) {
            const QPointF candidateScreen = worldToScreen(candidate.point);
            const qreal distance = std::hypot(candidateScreen.x() - cursorScreen.x(),
                                               candidateScreen.y() - cursorScreen.y());
            if (distance <= bestDistance) {
                bestDistance = distance;
                best.type = candidate.type;
                best.point = candidate.point;
            }
        };

        for (const SnapCandidate &candidate : snapCandidatesForScene()) {
            consider(candidate);
        }

        if (!pendingPoints_.isEmpty()) {
            for (const SnapCandidate &candidate :
                 perpendicularCandidates(pendingPoints_.back(), rawPoint)) {
                consider(candidate);
            }

            for (const SnapCandidate &candidate :
                 tangentCandidates(pendingPoints_.back())) {
                consider(candidate);
            }
        }

        return best;
    }

    SnapResult closestSnapCandidate(const QPointF &rawPoint,
                                    const QVector<SnapCandidate> &candidates) const
    {
        SnapResult best;
        const QPointF cursorScreen = worldToScreen(rawPoint);
        constexpr qreal snapRadiusPixels = 12.0;
        qreal bestDistance = snapRadiusPixels;
        for (const SnapCandidate &candidate : candidates) {
            const Point3D candidateWorld = candidate.hasWorldPoint
                ? candidate.worldPoint
                : workPlaneFramePointToWorld(
                      candidate.point, viewportTransform_.workPlaneFrame());
            QPointF candidateScreen;
            if (!viewportTransform_.worldPointToScreen(candidateWorld,
                                                        size(),
                                                        &candidateScreen)) {
                continue;
            }
            const qreal distance = std::hypot(candidateScreen.x() - cursorScreen.x(),
                                              candidateScreen.y() - cursorScreen.y());
            if (distance <= bestDistance) {
                bestDistance = distance;
                best.type = candidate.type;
                best.point = worldPointToWorkPlaneFrame(
                    candidateWorld, viewportTransform_.workPlaneFrame());
                best.worldPoint = candidateWorld;
                best.hasWorldPoint = true;
            }
        }
        return best;
    }

    QVector<int> grabSelectedShapeIndices() const
    {
        QVector<int> selectedIndices;
        const QVector<ObjectId> &draggedObjectIds = selectionDraggedObjectIds();
        selectedIndices.reserve(draggedObjectIds.size());
        for (const ObjectId objectId : draggedObjectIds) {
            const int shapeIndex = objectIndex(objectId);
            if (shapeIndex >= 0) {
                selectedIndices.append(shapeIndex);
            }
        }
        return selectedIndices;
    }

    SnapResult findGrabBasePointSnap(const QPointF &rawPoint) const
    {
        QVector<int> excludedShapeIndices;
        const QVector<int> selectedIndices = grabSelectedShapeIndices();
        for (int shapeIndex = 0; shapeIndex < document_.size(); ++shapeIndex) {
            if (!selectedIndices.contains(shapeIndex)) {
                excludedShapeIndices.append(shapeIndex);
            }
        }
        const QVector<SnapCandidate> candidates =
            snapEngine_.snapCandidatesForScene(document_,
                                               excludedShapeIndices,
                                               viewportTransform_,
                                               size());
        return closestSnapCandidate(rawPoint, candidates);
    }

    QVector<int> duplicateSourceShapeIndices() const
    {
        QVector<int> selectedIndices;
        selectedIndices.reserve(duplicateTool_.sourceObjects().size());
        for (const SceneObject &source : duplicateTool_.sourceObjects()) {
            const int shapeIndex = objectIndex(source.id);
            if (shapeIndex >= 0) {
                selectedIndices.append(shapeIndex);
            }
        }
        return selectedIndices;
    }

    SnapResult findDuplicateBasePointSnap(const QPointF &rawPoint) const
    {
        QVector<int> excludedShapeIndices;
        const QVector<int> selectedIndices = duplicateSourceShapeIndices();
        for (int shapeIndex = 0; shapeIndex < document_.size(); ++shapeIndex) {
            if (!selectedIndices.contains(shapeIndex)) {
                excludedShapeIndices.append(shapeIndex);
            }
        }
        const QVector<SnapCandidate> candidates =
            snapEngine_.snapCandidatesForScene(document_,
                                               excludedShapeIndices,
                                               viewportTransform_,
                                               size());
        return closestSnapCandidate(rawPoint, candidates);
    }

    SnapResult findDuplicateDestinationSnap(const QPointF &rawPoint) const
    {
        const QVector<int> selectedIndices = duplicateSourceShapeIndices();
        bool selectedLine = false;
        for (const int shapeIndex : selectedIndices) {
            if (shapeIndex >= 0 && shapeIndex < document_.size() &&
                document_[shapeIndex].geometryType == GeometryType::Line) {
                selectedLine = true;
                break;
            }
        }
        return snapEngine_.findSnapPoint(document_,
                                         rawPoint,
                                         true,
                                         QVector<QPointF>{duplicateTool_.basePoint()},
                                         viewportTransform_,
                                         size(),
                                         selectedIndices,
                                         true,
                                         !selectedLine);
    }

    SnapResult findGrabDestinationSnap(const QPointF &rawPoint) const
    {
        const QVector<int> selectedIndices = grabSelectedShapeIndices();
        bool selectedLine = false;
        for (const int shapeIndex : selectedIndices) {
            if (shapeIndex >= 0 && shapeIndex < document_.size() &&
                document_[shapeIndex].geometryType == GeometryType::Line) {
                selectedLine = true;
                break;
            }
        }
        return snapEngine_.findSnapPoint(document_,
                                         rawPoint,
                                         true,
                                         QVector<QPointF>{grabTool_.basePoint()},
                                         viewportTransform_,
                                         size(),
                                         selectedIndices,
                                         true,
                                         !selectedLine);
    }

    DragSnapResult findDragSnap(ObjectId selectedObjectId,
                                bool forceEnabled = false) const
    {
        return findDragSnap(QVector<ObjectId>{selectedObjectId}, forceEnabled);
    }

    DragSnapResult findDragSnap(const QVector<ObjectId> &selectedObjectIds,
                                bool forceEnabled = false,
                                bool includeNear = true) const
    {
        QVector<int> serviceSelectedShapeIndices;
        serviceSelectedShapeIndices.reserve(selectedObjectIds.size());
        for (const ObjectId objectId : selectedObjectIds) {
            const int shapeIndex = objectIndex(objectId);
            if (shapeIndex >= 0) {
                serviceSelectedShapeIndices.append(shapeIndex);
            }
        }
        return snapEngine_.findDragSnap(document_,
                                        serviceSelectedShapeIndices,
                                        viewportTransform_,
                                        size(),
                                        forceEnabled,
                                        includeNear);
    }

    DragSnapResult trackNearDragSnap(const QVector<ObjectId> &selectedObjectIds,
                                     const QPointF &sourcePoint,
                                     int targetShapeIndex,
                                     int targetComponentIndex,
                                     qreal snapRadiusPixels) const
    {
        QVector<int> serviceSelectedShapeIndices;
        serviceSelectedShapeIndices.reserve(selectedObjectIds.size());
        for (const ObjectId objectId : selectedObjectIds) {
            const int shapeIndex = objectIndex(objectId);
            if (shapeIndex >= 0) {
                serviceSelectedShapeIndices.append(shapeIndex);
            }
        }
        return snapEngine_.trackNearDragSnap(document_,
                                             serviceSelectedShapeIndices,
                                             sourcePoint,
                                             targetShapeIndex,
                                             targetComponentIndex,
                                             viewportTransform_,
                                             size(),
                                             snapRadiusPixels);
    }

    DragSnapResult findControlPointSnap(ObjectId selectedObjectId,
                                        int selectedControlPointIndex,
                                        const QPointF &controlPoint) const
    {
        return snapEngine_.findControlPointSnap(document_,
                                                objectIndex(selectedObjectId),
                                                selectedControlPointIndex,
                                                controlPoint,
                                                viewportTransform_,
                                                size());
    }

    QPointF constrainLinePoint(const QPointF &rawPoint,
                               bool altModifier = false,
                               const QPointF *screenPosition = nullptr)
    {
        currentSnap_ = findSnapPoint(rawPoint);
        const QPointF snappedOrRawPoint = currentSnap_.isValid()
                                              ? currentSnap_.point
                                              : rawPoint;

        if (activeTool_ == Tool::Arc &&
            (arcState().mode == ArcMode::TwoPoint ||
             arcState().mode == ArcMode::ThreePoint) &&
            arcTool_.inputStage() == ArcInputStage::SecondPoint &&
            !navigationController_.isPanning()) {
            return constrainArcChordEndpoint(
                rawPoint,
                altModifier,
                currentArcScreenPosition(screenPosition));
        }

        if (activeTool_ == Tool::Arc && arcState().mode == ArcMode::TwoPoint &&
            arcTool_.inputStage() == ArcInputStage::Complete &&
            !navigationController_.isPanning()) {
            return arcTool_.constrainTwoPointThroughPoint(
                snappedOrRawPoint,
                currentSnap_.isValid(), altModifier);
        }

        if (activeTool_ == Tool::Arc && arcState().mode == ArcMode::ThreePoint &&
            arcTool_.inputStage() == ArcInputStage::Complete &&
            !navigationController_.isPanning()) {
            return snappedOrRawPoint;
        }

        if (activeTool_ == Tool::Arc && arcState().mode == ArcMode::OnePoint &&
            arcTool_.inputStage() == ArcInputStage::Complete &&
            arcState().angleValueLocked && !navigationController_.isPanning()) {
            const QPointF center = pendingPoints_[0];
            const QPointF radiusVector = pendingPoints_[1] - center;
            const qreal radius = std::hypot(radiusVector.x(), radiusVector.y());
            if (radius > 1.0e-9) {
                currentSnap_ = SnapResult{};
                const qreal startAngle = std::atan2(radiusVector.y(),
                                                    radiusVector.x());
                const qreal endAngle = startAngle + arcState().previewSweepAngle;
                return center + QPointF(radius * std::cos(endAngle),
                                        radius * std::sin(endAngle));
            }
        }

        if (activeToolController_ == nullptr &&
            isEllipseTool(activeTool_) && pendingPoints_.size() >= 2) {
            const EllipseMode mode = ellipseModeForTool(activeTool_);
            if (mode == EllipseMode::CenterAxisRadius ||
                mode == EllipseMode::AxisEndpoints) {
                const QPointF center = mode == EllipseMode::CenterAxisRadius
                                           ? pendingPoints_[0]
                                           : (pendingPoints_[0] + pendingPoints_[1]) * 0.5;
                const QPointF axis = pendingPoints_[1] - pendingPoints_[0];
                const qreal axisLength = std::hypot(axis.x(), axis.y());
                if (axisLength > 1.0e-9) {
                    const QPointF majorUnit = axis / axisLength;
                    const QPointF minorUnit(-majorUnit.y(), majorUnit.x());
                    return center + minorUnit *
                                        QPointF::dotProduct(snappedOrRawPoint - center,
                                                            minorUnit);
                }
            }
            if (mode == EllipseMode::FociPoint) {
                return snappedOrRawPoint;
            }
        }

        if (activeToolController_ == nullptr &&
            activeTool_ == Tool::RectangleThreePoint && pendingPoints_.size() >= 2) {
            const QPointF edge = pendingPoints_[1] - pendingPoints_[0];
            const qreal edgeLength = std::hypot(edge.x(), edge.y());
            if (edgeLength > 1.0e-9) {
                const QPointF edgeUnit = edge / edgeLength;
                const QPointF perpendicular(-edgeUnit.y(), edgeUnit.x());
                return pendingPoints_[1] + perpendicular *
                                                QPointF::dotProduct(
                                                    snappedOrRawPoint - pendingPoints_[1],
                                                    perpendicular);
            }
        }

        if (activeTool_ == Tool::Arc && arcState().mode == ArcMode::OnePoint &&
            arcTool_.inputStage() == ArcInputStage::SecondPoint &&
            !navigationController_.isPanning()) {
            const QPointF center = pendingPoints_.first();
            const QPointF target = currentSnap_.isValid()
                                       ? currentSnap_.point
                                       : rawPoint;
            const QPointF vector = target - center;
            const qreal radius = std::hypot(vector.x(), vector.y());
            if (radius <= 1.0e-9) {
                return target;
            }

            qreal angle = std::atan2(vector.y(), vector.x());
            if (currentSnap_.isValid()) {
                // Keep SnapEngine's exact target and temporarily bypass the
                // add-on's soft angular snap while geometry is acquired.
                return target;
            }

            if (orthoEnabled_) {
                constexpr qreal halfPi = 1.57079632679489661923;
                constexpr qreal pi = 3.14159265358979323846;
                if (std::abs(vector.x()) >= std::abs(vector.y())) {
                    angle = vector.x() < 0.0 ? pi : 0.0;
                } else {
                    angle = vector.y() < 0.0 ? -halfPi : halfPi;
                }
            } else if (arcState().angleSnapEnabled) {
                angle = ArcTool::snapPreviewAngle(angle);
            }
            return center + QPointF(radius * std::cos(angle),
                                    radius * std::sin(angle));
        }

        if (currentSnap_.isValid()) {
            return currentSnap_.point;
        }

        const bool drawingConstraintActive =
            (activeTool_ == Tool::Line && lineCommandActive_) ||
            (activeTool_ == Tool::LinearDimension && pendingPoints_.size() == 1) ||
            (activeTool_ == Tool::AngularDimension && !pendingPoints_.isEmpty()) ||
            activeTool_ == Tool::Arc || activeTool_ == Tool::Mirror ||
            isCircleConstructionTool(activeTool_) ||
            isPolygonTool(activeTool_) ||
            (isEllipseTool(activeTool_) &&
             ellipseModeForTool(activeTool_) != EllipseMode::Corners) ||
            (activeTool_ == Tool::RectangleThreePoint && pendingPoints_.size() == 1) ||
            activeTool_ == Tool::TangentFromCurve ||
            activeTool_ == Tool::PerpendicularFromCurve ||
            activeTool_ == Tool::PerpendicularFromEdge ||
            isTwoCurveLineTool(activeTool_) ||
            activeTool_ == Tool::PointByLine ||
            activeTool_ == Tool::CurveInterpolate;
        if (!orthoEnabled_ || navigationController_.isPanning() ||
            !drawingConstraintActive || pendingPoints_.isEmpty()) {
            return rawPoint;
        }

        if (activeTool_ == Tool::Arc && arcState().mode == ArcMode::OnePoint &&
            pendingPoints_.size() >= 2) {
            return arcTool_.constrainOnePointEndpoint(rawPoint);
        }

        const QPointF origin = activeTool_ == Tool::AngularDimension
                                   ? pendingPoints_.first()
                                   : pendingPoints_.back();
        return InputConstraintService::nearestPlanarAxis(rawPoint, origin);
    }

    void refreshCursorConstraint()
    {
        if (!cursorValid_) {
            currentSnap_ = SnapResult{};
            return;
        }

        const QPointF screenPosition = currentArcScreenPosition();
        if (activeTool_ == Tool::Arc &&
            arcTool_.inputStage() == ArcInputStage::SecondPoint &&
            arcState().mode != ArcMode::OnePoint &&
            !(arcState().mode == ArcMode::TwoPoint &&
              arcState().perpendicularPlaneActive)) {
            restoreArcChordReferencePlaneForEndpointPick();
            QPointF rawAtScreen;
            if (viewportTransform_.screenToWorkPlane(
                    screenPosition, size(), viewportTransform_.workPlaneFrame(),
                    &rawAtScreen)) {
                rawCursorWorld_ = rawAtScreen;
            }
        }
        cursorWorld_ = constrainLinePoint(rawCursorWorld_, false, &screenPosition);
        lastWorldPosition_ = cursorWorld_;
    }

    qreal distanceToSegment(const QPointF &point,
                            const QPointF &start,
                            const QPointF &end) const
    {
        return curveHitTester_.distanceToSegment(point, start, end);
    }

    QVector<QPointF> rectangleVertices(const Shape &shape) const
    {
        if (shape.geometryType == GeometryType::Rectangle &&
            validateNurbsCurve(shape.nurbs) && shape.nurbs.controlPoints.size() == 5) {
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

    QVector<QPointF> controlPointsForShape(const Shape &shape) const
    {
        return curveHitTester_.controlPointsForShape(shape);
    }

    QVector<Point3D> controlPointWorldPositions(
        const ViewportRenderObject &renderObject) const
    {
        const Shape &shape = renderObject.shape;
        QVector<Point3D> positions;
        if (shape.geometryType == GeometryType::PolyCurve) {
            for (int componentIndex = 0;
                 componentIndex < shape.components.size();
                 ++componentIndex) {
                const Shape::NurbsCurve2D &component =
                    shape.components[componentIndex];
                for (const QPointF &controlPoint : component.controlPoints) {
                    positions.append(shapeComponentPointToWorld(
                        shape, componentIndex, controlPoint));
                }
            }
        } else {
            const QVector<QPointF> controlPoints =
                controlPointsForShape(shape);
            positions.reserve(controlPoints.size());
            for (const QPointF &controlPoint : controlPoints) {
                positions.append(shapePointToWorld(shape, controlPoint));
            }
        }

        const Point3D &offset = renderObject.placementTranslation;
        for (Point3D &position : positions) {
            position.x += offset.x;
            position.y += offset.y;
            position.z += offset.z;
        }
        return positions;
    }

    QVector<int> controlPointShapeIndices() const
    {
        QVector<int> indices;
        for (const ObjectId objectId : selectedShapeIndices_) {
            const int index = objectIndex(objectId);
            if (index >= 0 && document_.isObjectVisible(objectId) &&
                document_.isObjectEditable(objectId) &&
                !indices.contains(index)) {
                indices.append(index);
            }
        }
        const int primaryIndex = objectIndex(selectedShapeIndex_);
        if (primaryIndex >= 0 && document_.isObjectVisible(selectedShapeIndex_) &&
            document_.isObjectEditable(selectedShapeIndex_) &&
            !indices.contains(primaryIndex)) {
            indices.append(primaryIndex);
        }
        return indices;
    }

    bool hitTestSelectedControlPoint(const QPointF &screenPosition,
                                     int *shapeIndex,
                                     int *controlPointIndex) const
    {
        return curveHitTester_.hitTestSelectedControlPoint(document_,
                                                           controlPointShapeIndices(),
                                                           screenPosition,
                                                           viewportTransform_,
                                                           size(),
                                                           shapeIndex,
                                                           controlPointIndex);
    }

    int hitTestComponent(const QPointF &screenPosition, int *shapeIndexOut)
    {
        if (shapeIndexOut != nullptr) *shapeIndexOut = -1;
        // Blender starts mesh component picking at ED_view3d_select_dist_px()
        // (75 * UI pixel size), and compares Manhattan screen distances.
        // Widget mouse coordinates are already in logical pixels.
        constexpr qreal selectionRadiusPixels = 75.0;
        constexpr qreal selectedItemBiasPixels = 5.0;
        constexpr qreal cycleThresholdPixels = 3.0;
        const bool xray = viewportShadingSettings_.xrayEnabled();
        const QPointF pickPosition(qRound(screenPosition.x()),
                                   qRound(screenPosition.y()));
        const auto insideViewport = [this](const QPointF &point) {
            return point.x() >= 0.0 && point.y() >= 0.0 &&
                   point.x() < size().width() && point.y() < size().height();
        };
        const auto projectPickPoint = [&](const Point3D &point,
                                          QPointF *projected) {
            return xray
                       ? viewportTransform_.worldPointToScreen(point, size(), projected)
                       : viewportTransform_.worldPointToScreenUnclipped(
                             point, size(), projected);
        };
        const auto clipXraySegment = [&](const Point3D &worldA,
                                         const Point3D &worldB,
                                         QPointF *screenA,
                                         QPointF *screenB,
                                         qreal *worldT0,
                                         qreal *worldT1) {
            qreal t0 = 0.0;
            qreal t1 = 1.0;
            const ViewportCameraPreferences preferences =
                viewportTransform_.cameraPreferences();
            const Point3D viewTarget = viewportTransform_.viewTarget();
            const auto dot = [](const Point3D &a, const Point3D &b) {
                return a.x * b.x + a.y * b.y + a.z * b.z;
            };
            const auto depthAt = [&](const Point3D &point) {
                if (viewportTransform_.isPerspectiveEnabled()) {
                    const Point3D eye = viewportTransform_.cameraPosition(size());
                    const Point3D axis{viewTarget.x - eye.x,
                                       viewTarget.y - eye.y,
                                       viewTarget.z - eye.z};
                    const qreal axisLength = std::sqrt(dot(axis, axis));
                    if (axisLength <= 1.0e-12) {
                        return std::numeric_limits<qreal>::quiet_NaN();
                    }
                    const Point3D fromEye{point.x - eye.x,
                                          point.y - eye.y,
                                          point.z - eye.z};
                    return dot(fromEye, axis) / axisLength;
                }
                const Point3D viewDirection = viewportTransform_.viewDirection();
                const Point3D fromTarget{point.x - viewTarget.x,
                                         point.y - viewTarget.y,
                                         point.z - viewTarget.z};
                return preferences.clipEnd - dot(fromTarget, viewDirection);
            };
            const qreal depthA = depthAt(worldA);
            const qreal depthB = depthAt(worldB);
            if (!std::isfinite(depthA) || !std::isfinite(depthB)) {
                return false;
            }
            const qreal depthDelta = depthB - depthA;
            const auto clipDepthLower = [&](qreal limit) {
                if (std::abs(depthDelta) <= 1.0e-12) return depthA >= limit;
                const qreal crossing = (limit - depthA) / depthDelta;
                if (depthDelta > 0.0) t0 = qMax(t0, crossing);
                else t1 = qMin(t1, crossing);
                return t0 <= t1;
            };
            const auto clipDepthUpper = [&](qreal limit) {
                if (std::abs(depthDelta) <= 1.0e-12) return depthA <= limit;
                const qreal crossing = (limit - depthA) / depthDelta;
                if (depthDelta > 0.0) t1 = qMin(t1, crossing);
                else t0 = qMax(t0, crossing);
                return t0 <= t1;
            };
            if (!clipDepthLower(preferences.clipStart) ||
                !clipDepthUpper(preferences.clipEnd)) {
                return false;
            }
            t0 = std::clamp(t0, 0.0, 1.0);
            t1 = std::clamp(t1, 0.0, 1.0);
            if (t0 > t1) return false;
            const auto interpolate = [](const Point3D &a,
                                        const Point3D &b,
                                        qreal t) {
                return Point3D{a.x + (b.x - a.x) * t,
                               a.y + (b.y - a.y) * t,
                               a.z + (b.z - a.z) * t};
            };
            const Point3D clippedWorldA = interpolate(worldA, worldB, t0);
            const Point3D clippedWorldB = interpolate(worldA, worldB, t1);
            if (!viewportTransform_.worldPointToScreen(
                    clippedWorldA, size(), screenA) ||
                !viewportTransform_.worldPointToScreen(
                    clippedWorldB, size(), screenB)) {
                return false;
            }

            qreal screenT0 = 0.0;
            qreal screenT1 = 1.0;
            const QPointF direction = *screenB - *screenA;
            const auto clipScreenBoundary = [&](qreal p, qreal q) {
                if (std::abs(p) <= 1.0e-12) return q >= 0.0;
                const qreal crossing = q / p;
                if (p < 0.0) {
                    if (crossing > screenT1) return false;
                    screenT0 = qMax(screenT0, crossing);
                } else {
                    if (crossing < screenT0) return false;
                    screenT1 = qMin(screenT1, crossing);
                }
                return true;
            };
            const qreal maxX = qMax(0, size().width() - 1);
            const qreal maxY = qMax(0, size().height() - 1);
            if (!clipScreenBoundary(-direction.x(), screenA->x()) ||
                !clipScreenBoundary(direction.x(), maxX - screenA->x()) ||
                !clipScreenBoundary(-direction.y(), screenA->y()) ||
                !clipScreenBoundary(direction.y(), maxY - screenA->y()) ||
                screenT0 > screenT1) {
                return false;
            }
            const QPointF originalScreenA = *screenA;
            *screenA = originalScreenA + direction * screenT0;
            *screenB = originalScreenA + direction * screenT1;
            *worldT0 = t0 + (t1 - t0) * screenT0;
            *worldT1 = t0 + (t1 - t0) * screenT1;
            return true;
        };
        struct PendingComponent {
            int shapeIndex = -1;
            int componentIndex = -1;
            bool edge = false;
            Point3D first;
            Point3D second;
            Point3D closestWorldPoint;
            QPointF closestScreenPoint;
            qreal distance = std::numeric_limits<qreal>::infinity();
            qreal depthTolerance = 1.0e-5;
        };
        const auto chooseXrayCandidate = [&](const QVector<PendingComponent> &items) {
            ComponentPickCycleState &cycleState = componentPickCycleStates_[
                static_cast<int>(componentSelectionMode_)];
            const bool cycle = cycleState.valid &&
                               cycleState.position == pickPosition;
            qreal bestActualDistance = selectionRadiusPixels;
            int bestCandidate = -1;
            int first = 0;
            while (first < items.size()) {
                const int shapeIndex = items[first].shapeIndex;
                const ObjectId objectId = shapes_.objectIdAt(shapeIndex);
                qreal bestBiasedDistance = bestActualDistance;
                int shapeBest = -1;
                int cycleCandidate = -1;
                for (int index = first; index < items.size() &&
                                          items[index].shapeIndex == shapeIndex;
                     ++index) {
                    const PendingComponent &candidate = items[index];
                    const bool alreadySelected =
                        componentSelectionObject_ == objectId &&
                        activeComponentSelection().contains(candidate.componentIndex);
                    const qreal biasedDistance = candidate.distance +
                        (alreadySelected ? selectedItemBiasPixels : 0.0);
                    if (cycle && cycleState.object == objectId &&
                        candidate.componentIndex > cycleState.componentIndex &&
                        biasedDistance < cycleThresholdPixels &&
                        cycleCandidate < 0) {
                        cycleCandidate = index;
                    }
                    if (biasedDistance < bestBiasedDistance) {
                        bestBiasedDistance = biasedDistance;
                        shapeBest = index;
                    }
                }
                const int shapeCandidate = cycleCandidate >= 0
                                               ? cycleCandidate
                                               : shapeBest;
                if (shapeCandidate >= 0 &&
                    items[shapeCandidate].distance < bestActualDistance) {
                    bestActualDistance = items[shapeCandidate].distance;
                    bestCandidate = shapeCandidate;
                }
                while (first < items.size() &&
                       items[first].shapeIndex == shapeIndex) {
                    ++first;
                }
            }
            if (bestCandidate < 0) {
                return -1;
            }
            const PendingComponent &candidate = items[bestCandidate];
            cycleState.valid = true;
            cycleState.position = pickPosition;
            cycleState.object = shapes_.objectIdAt(candidate.shapeIndex);
            cycleState.componentIndex = candidate.componentIndex;
            return bestCandidate;
        };
        if (componentSelectionMode_ == ComponentSelectionMode::Face) {
            if (xray) {
                QVector<PendingComponent> faceCandidates;
                for (int shapeIndex = 0; shapeIndex < shapes_.size(); ++shapeIndex) {
                    const ObjectId objectId = shapes_.objectIdAt(shapeIndex);
                    if (!document_.isObjectVisible(objectId) ||
                        !document_.isObjectEditable(objectId)) {
                        continue;
                    }
                    const Shape &shape = shapes_[shapeIndex];
                    if (shape.geometryType != GeometryType::NurbsSolid &&
                        shape.geometryType != GeometryType::NurbsSurface) {
                        continue;
                    }
                    const SceneObject *object = document_.object(objectId);
                    const Point3D offset = object != nullptr
                                               ? object->placementTranslation
                                               : Point3D{};
                    const QVector<NurbsSurface3D> faces = shapeSurfaceFaces(shape);
                    for (int faceIndex = 0; faceIndex < faces.size(); ++faceIndex) {
                        const NurbsSurface3D &face = faces[faceIndex];
                        PreparedNurbsSurfaceEvaluator evaluator;
                        qreal u0 = 0.0;
                        qreal u1 = 0.0;
                        qreal v0 = 0.0;
                        qreal v1 = 0.0;
                        Point3D center;
                        if (!evaluator.prepare(face) ||
                            !evaluator.parameterDomains(&u0, &u1, &v0, &v1) ||
                            !evaluator.evaluate((u0 + u1) * 0.5,
                                                (v0 + v1) * 0.5,
                                                &center)) {
                            continue;
                        }
                        center.x += offset.x;
                        center.y += offset.y;
                        center.z += offset.z;
                        QPointF projected;
                        if (!viewportTransform_.worldPointToScreen(
                                center, size(), &projected) ||
                            !insideViewport(projected)) {
                            continue;
                        }
                        PendingComponent candidate;
                        candidate.shapeIndex = shapeIndex;
                        candidate.componentIndex = faceIndex;
                        candidate.first = center;
                        candidate.closestWorldPoint = center;
                        candidate.closestScreenPoint = projected;
                        candidate.distance =
                            std::abs(projected.x() - pickPosition.x()) +
                            std::abs(projected.y() - pickPosition.y());
                        faceCandidates.append(candidate);
                    }
                }
                const int picked = chooseXrayCandidate(faceCandidates);
                if (picked < 0) return -1;
                if (shapeIndexOut != nullptr) {
                    *shapeIndexOut = faceCandidates[picked].shapeIndex;
                }
                return faceCandidates[picked].componentIndex;
            }

            Point3D point, normal;
            int faceIndex = -1;
            int faceShapeIndex = -1;
            if (!curveHitTester_.hitTestVisibleSurface(document_, screenPosition,
                                                       viewportTransform_, size(),
                                                       &point, &normal, &faceIndex,
                                                       &faceShapeIndex)) {
                return -1;
            }
            if (faceShapeIndex < 0 || faceShapeIndex >= shapes_.size()) return -1;
            const ObjectId objectId = shapes_.objectIdAt(faceShapeIndex);
            if (!document_.isObjectEditable(objectId)) return -1;
            const Shape &shape = shapes_[faceShapeIndex];
            if (shape.geometryType == GeometryType::NurbsSurface && faceIndex > 0) {
                return -1;
            }
            if (shapeIndexOut != nullptr) *shapeIndexOut = faceShapeIndex;
            return faceIndex;
        }

        QVector<PendingComponent> pending;

        for (int shapeIndex = 0; shapeIndex < shapes_.size(); ++shapeIndex) {
            const ObjectId objectId = shapes_.objectIdAt(shapeIndex);
            if (!document_.isObjectVisible(objectId) ||
                !document_.isObjectEditable(objectId)) {
                continue;
            }
            const Shape &shape = shapes_[shapeIndex];
            if (shape.geometryType != GeometryType::NurbsSolid &&
                shape.geometryType != GeometryType::NurbsSurface) {
                continue;
            }

            const SceneObject *object = document_.object(objectId);
            const Point3D offset = object != nullptr
                                       ? object->placementTranslation
                                       : Point3D{};
            const ViewportDepthGeometry cage = selectedSurfaceCage(shape);
            qreal cageScale = 1.0;
            if (!cage.pointVertices.isEmpty()) {
                const QVector3D origin = cage.pointVertices.first();
                for (const QVector3D &point : cage.pointVertices) {
                    cageScale = qMax(cageScale, qreal((point - origin).length()));
                }
            }
            const qreal depthTolerance = qMax(1.0e-5, cageScale * 1.0e-2);

            if (componentSelectionMode_ == ComponentSelectionMode::Vertex) {
                for (int vertex = 0; vertex < cage.pointVertices.size(); ++vertex) {
                    const QVector3D &sourcePoint = cage.pointVertices[vertex];
                    const Point3D worldPoint{sourcePoint.x() + offset.x,
                                             sourcePoint.y() + offset.y,
                                             sourcePoint.z() + offset.z};
                    QPointF projectedPoint;
                    if (!projectPickPoint(worldPoint, &projectedPoint) ||
                        (xray && !insideViewport(projectedPoint))) {
                        continue;
                    }
                    PendingComponent candidate;
                    candidate.shapeIndex = shapeIndex;
                    candidate.componentIndex = vertex;
                    candidate.first = worldPoint;
                    candidate.closestWorldPoint = worldPoint;
                    candidate.closestScreenPoint = projectedPoint;
                    candidate.distance =
                        std::abs(projectedPoint.x() - pickPosition.x()) +
                        std::abs(projectedPoint.y() - pickPosition.y());
                    candidate.depthTolerance = depthTolerance;
                    pending.append(candidate);
                }
            } else {
                for (int edge = 0;
                     edge * 2 + 1 < cage.preciseLineVertices.size(); ++edge) {
                    const Point3D &first = cage.preciseLineVertices[edge * 2];
                    const Point3D &second = cage.preciseLineVertices[edge * 2 + 1];
                    const Point3D worldA{first.x + offset.x,
                                         first.y + offset.y,
                                         first.z + offset.z};
                    const Point3D worldB{second.x + offset.x,
                                         second.y + offset.y,
                                         second.z + offset.z};
                    QPointF screenA;
                    QPointF screenB;
                    qreal segmentWorldT0 = 0.0;
                    qreal segmentWorldT1 = 1.0;
                    if (xray) {
                        if (!clipXraySegment(worldA, worldB, &screenA, &screenB,
                                             &segmentWorldT0, &segmentWorldT1)) {
                            continue;
                        }
                    } else if (!projectPickPoint(worldA, &screenA) ||
                               !projectPickPoint(worldB, &screenB)) {
                        continue;
                    }
                    const QPointF direction = screenB - screenA;
                    const qreal lengthSquared = QPointF::dotProduct(
                        direction, direction);
                    const qreal fraction = lengthSquared > 1.0e-12
                        ? std::clamp(QPointF::dotProduct(
                                         pickPosition - screenA, direction) /
                                         lengthSquared,
                                     0.0, 1.0)
                        : 0.0;
                    const QPointF projectedPoint = screenA + direction * fraction;
                    PendingComponent candidate;
                    candidate.shapeIndex = shapeIndex;
                    candidate.componentIndex = edge;
                    candidate.edge = true;
                    candidate.first = worldA;
                    candidate.second = worldB;
                    candidate.closestScreenPoint = projectedPoint;
                    candidate.distance =
                        std::abs(projectedPoint.x() - pickPosition.x()) +
                        std::abs(projectedPoint.y() - pickPosition.y());
                    const qreal worldFraction = segmentWorldT0 +
                        (segmentWorldT1 - segmentWorldT0) * fraction;
                    candidate.closestWorldPoint = {
                        worldA.x + (worldB.x - worldA.x) * worldFraction,
                        worldA.y + (worldB.y - worldA.y) * worldFraction,
                        worldA.z + (worldB.z - worldA.z) * worldFraction};
                    candidate.depthTolerance = depthTolerance;
                    pending.append(candidate);
                }
            }
        }

        if (xray) {
            const int picked = chooseXrayCandidate(pending);
            if (picked < 0) return -1;
            if (shapeIndexOut != nullptr) {
                *shapeIndexOut = pending[picked].shapeIndex;
            }
            return pending[picked].componentIndex;
        }
        if (gpuSurface_ != nullptr && !pending.isEmpty()) {
            QVector<ViewportComponentPickCandidate> pickCandidates;
            pickCandidates.reserve(pending.size());
            for (const PendingComponent &candidate : pending) {
                pickCandidates.append({candidate.first, candidate.second,
                                       candidate.componentIndex,
                                       candidate.shapeIndex, candidate.edge});
            }
            const ViewportRenderFrame renderFrame = viewportRenderFrame();
            int pickedCandidate = -1;
            if (gpuSurface_->pickComponentElement(
                    screenPosition, renderFrame.camera,
                    renderFrame.viewportSize, renderFrame.objects,
                    pickCandidates, &pickedCandidate)) {
                if (pickedCandidate < 0 || pickedCandidate >= pending.size()) {
                    return -1;
                }
                const PendingComponent &candidate = pending[pickedCandidate];
                if (shapeIndexOut != nullptr) {
                    *shapeIndexOut = candidate.shapeIndex;
                }
                return candidate.componentIndex;
            }
        }

        qreal bestDistance = selectionRadiusPixels;
        int bestCandidate = -1;
        for (int index = 0; index < pending.size(); ++index) {
            const PendingComponent &candidate = pending[index];
            const ObjectId objectId =
                shapes_.objectIdAt(candidate.shapeIndex);
            const bool alreadySelected =
                componentSelectionObject_ == objectId &&
                activeComponentSelection().contains(candidate.componentIndex);
            const qreal biasedDistance = candidate.distance +
                (xray && alreadySelected ? selectedItemBiasPixels : 0.0);
            if (biasedDistance >= bestDistance) {
                continue;
            }
            if (!xray) {
                const qreal componentDepth =
                    viewportTransform_.worldDirectionToView(
                        candidate.closestWorldPoint).towardCamera;
                bool visible = false;
                if (std::isfinite(componentDepth)) {
                    for (qreal yOffset : {-1.0, 0.0, 1.0}) {
                        for (qreal xOffset : {-1.0, 0.0, 1.0}) {
                            Point3D visiblePoint;
                            Point3D visibleNormal;
                            int visibleShapeIndex = -1;
                            if (!curveHitTester_.hitTestVisibleSurface(
                                    document_,
                                    candidate.closestScreenPoint +
                                        QPointF(xOffset, yOffset),
                                    viewportTransform_, size(),
                                    &visiblePoint, &visibleNormal, nullptr,
                                    &visibleShapeIndex) ||
                                visibleShapeIndex != candidate.shapeIndex) {
                                continue;
                            }
                            const qreal visibleDepth =
                                viewportTransform_.worldDirectionToView(
                                    visiblePoint).towardCamera;
                            if (std::isfinite(visibleDepth) &&
                                std::abs(componentDepth - visibleDepth) <=
                                    candidate.depthTolerance) {
                                visible = true;
                                break;
                            }
                        }
                        if (visible) break;
                    }
                }
                if (!visible) {
                    continue;
                }
            }
            bestDistance = biasedDistance;
            bestCandidate = index;
        }
        if (bestCandidate < 0) {
            return -1;
        }
        const PendingComponent &candidate = pending[bestCandidate];
        if (shapeIndexOut != nullptr) {
            *shapeIndexOut = candidate.shapeIndex;
        }
        return candidate.componentIndex;
    }

    int hitTestShape(const QPointF &screenPosition) const
    {
        const int hitShapeIndex = curveHitTester_.hitTestShape(document_,
                                                               screenPosition,
                                                               viewportTransform_,
                                                               size(),
                                                               true);
        if (qEnvironmentVariable("CLASSICAD_SNAP_TRACE") ==
            QStringLiteral("1")) {
            constexpr qreal selectionRadiusPixels = 9.0;
            const WorkPlaneFrame activeFrame = viewportTransform_.workPlaneFrame();
            for (int shapeIndex = 0; shapeIndex < document_.size(); ++shapeIndex) {
                const ObjectId objectId = document_.objectIdAt(shapeIndex);
                const bool visible = document_.isObjectVisible(objectId);
                const bool editable = document_.isObjectEditable(objectId);
                const Shape &shape = document_[shapeIndex];
                const SceneObject *sceneObject = document_.object(objectId);
                const WorkPlaneFrame targetFrame = shapeWorkPlaneFrame(shape);
                ViewportTransform shapeTransform = viewportTransform_;
                shapeTransform.setWorkPlaneFrame(targetFrame);
                const qreal distancePixels =
                    visible ? curveHitTester_.distanceToShape(screenPosition,
                                                             shape,
                                                             shapeTransform,
                                                             size(),
                                                             objectId,
                                                             document_.objectGeometryRevision(objectId),
                                                             sceneObject != nullptr
                                                                 ? sceneObject->placementTranslation
                                                                 : Point3D{})
                            : std::numeric_limits<qreal>::infinity();
                const QString reason = !visible
                    ? QStringLiteral("hidden")
                    : (!editable
                           ? QStringLiteral("not-editable")
                           : (distancePixels <= selectionRadiusPixels
                                  ? QStringLiteral("within-hit-radius")
                                  : QStringLiteral("outside-hit-radius")));
                DebugLog::instance().write(
                    QStringLiteral("selection-trace screen=%1 chosen=%2 candidate=%3 geometry=%4 visible=%5 editable=%6 activeFrameMatch=%7 distancePx=%8 reason=%9")
                        .arg(precisePointText(screenPosition))
                        .arg(hitShapeIndex)
                        .arg(shapeIndex)
                        .arg(geometryTypeName(shape.geometryType))
                        .arg(visible)
                        .arg(editable)
                        .arg(workPlaneMatches(targetFrame, activeFrame))
                        .arg(distancePixels, 0, 'f', 4)
                        .arg(reason));
            }
        }
        return hitShapeIndex;
    }


    QVector<qreal> eraseIntersectionParameters(
        int sourceShapeIndex,
        int sourceComponentIndex,
        const Shape::NurbsCurve2D &sourceCurve,
        const QVector<EraseCurveSampleCache> *sceneCache = nullptr,
        QVector<ObjectId> *intersectionObjectIds = nullptr,
        int *nurbsSeedSolves = nullptr,
        int *visiblePointChecks = nullptr) const
    {
        if (sourceShapeIndex < 0 || sourceShapeIndex >= shapes_.size() ||
            !isValidNurbsCurve(sourceCurve)) {
            return {};
        }

        const ObjectId sourceObjectId =
            shapes_.objectIdAt(sourceShapeIndex);
        const WorkPlaneFrame sourceFrame =
            shapeComponentWorkPlaneFrame(shapes_[sourceShapeIndex],
                                         sourceComponentIndex);
        if (!isValidWorkPlaneFrame(sourceFrame)) {
            return {};
        }

        const EraseIntersectionCandidates candidates =
            makeEraseIntersectionCandidates(document_, sceneCache);

        const EraseIntersectionParameterResult query =
            findEraseIntersectionParameters(sourceCurve,
                                            sourceFrame,
                                            sourceObjectId,
                                            sourceComponentIndex,
                                            candidates.curves,
                                            candidates.points,
                                            3.0 / std::max<qreal>(
                                                viewportTransform_.viewScalePixelsPerWorldUnit(size()),
                                                1.0e-9));
        if (intersectionObjectIds != nullptr) {
            for (const ObjectId objectId : query.intersectingObjectIds) {
                if (!intersectionObjectIds->contains(objectId)) {
                    intersectionObjectIds->append(objectId);
                }
            }
        }
        if (nurbsSeedSolves != nullptr) {
            *nurbsSeedSolves += query.nurbsSeedSolves;
        }
        if (visiblePointChecks != nullptr) {
            *visiblePointChecks += query.pointChecks;
        }
        return query.parameters;
    }
    QVector<int> eraseSelectionTargets() const
    {
        QVector<int> targets;
        for (const ObjectId objectId : selectedShapeIndices_) {
            const int shapeIndex = objectIndex(objectId);
            if (shapeIndex >= 0 && shapeIndex < shapes_.size() &&
                !targets.contains(shapeIndex)) {
                targets.append(shapeIndex);
            }
        }
        const int primaryIndex = objectIndex(selectedShapeIndex_);
        if (primaryIndex >= 0 && !targets.contains(primaryIndex)) {
            targets.append(primaryIndex);
        }
        return targets;
    }

    void invalidateEraseGeometryCacheForView()
    {
        if (!eraseGeometryCachePrepared_) {
            return;
        }
        const ViewportCameraState camera = viewportTransform_.cameraState();
        const ViewportCameraPreferences preferences = viewportTransform_.cameraPreferences();
        const ViewportCameraState &cached = eraseCacheCameraState_;
        if (eraseCacheViewportSize_ == size() &&
            camera.zoom == cached.zoom && camera.pan == cached.pan &&
            camera.perspective == cached.perspective &&
            camera.orbitPivot.x == cached.orbitPivot.x &&
            camera.orbitPivot.y == cached.orbitPivot.y &&
            camera.orbitPivot.z == cached.orbitPivot.z &&
            camera.orientation.w == cached.orientation.w &&
            camera.orientation.x == cached.orientation.x &&
            camera.orientation.y == cached.orientation.y &&
            camera.orientation.z == cached.orientation.z &&
            preferences.focalLengthMillimeters == eraseCacheCameraPreferences_.focalLengthMillimeters &&
            preferences.clipStart == eraseCacheCameraPreferences_.clipStart &&
            preferences.clipEnd == eraseCacheCameraPreferences_.clipEnd) {
            return;
        }

        eraseGeometryCachePrepared_ = false;
        trimTool_.invalidateHover();
        trimTool_.clearCandidates();
        eraseSceneCurveCaches_.clear();
        eraseTargetCurveCaches_.clear();
        eraseTargetShapeIndices_.clear();
        if (!eraseTool_.strokeActive() && !trimTool_.boxSelectionActive()) {
            eraseLikeScreenPath().clear();
        }
        DebugLog::instance().write(QStringLiteral("trim/erase projection cache invalidated by camera or viewport change"));
    }

    void prepareEraseGeometryCache()
    {
        QElapsedTimer totalTimer;
        totalTimer.start();
        eraseTargetShapeIndices_.clear();
        eraseSceneCurveCaches_.clear();
        eraseTargetCurveCaches_.clear();
        eraseGeometryCachePrepared_ = true;
        eraseCacheCameraState_ = viewportTransform_.cameraState();
        eraseCacheCameraPreferences_ = viewportTransform_.cameraPreferences();
        eraseCacheViewportSize_ = size();

        const QVector<int> selectedTargets = eraseSelectionTargets();
        QStringList selectedDescriptions;
        for (const int shapeIndex : selectedTargets) {
            if (shapeIndex < 0 || shapeIndex >= shapes_.size()) {
                continue;
            }
            const ObjectId objectId = shapes_.objectIdAt(shapeIndex);
            selectedDescriptions.append(
                QStringLiteral("%1:%2:%3")
                    .arg(shapeIndex)
                    .arg(objectId.value())
                    .arg(geometryTypeName(shapes_[shapeIndex].geometryType)));
        }
        DebugLog::instance().write(
            QStringLiteral("trim cache begin selected=[%1]")
                .arg(selectedDescriptions.join(',')));
        if (selectedTargets.isEmpty()) {
            DebugLog::instance().write(
                QStringLiteral("trim cache ready selected=0 sceneCurves=0 targetCurves=0 totalUs=%1")
                    .arg(totalTimer.nsecsElapsed() / 1000));
            return;
        }

        QElapsedTimer samplingTimer;
        samplingTimer.start();
        eraseSceneCurveCaches_ = curveSampler_.sampleDocument(document_,
                                                              viewportTransform_,
                                                              size());
        const qint64 samplingUs = samplingTimer.nsecsElapsed() / 1000;
        const EraseIntersectionCandidates sceneIntersectionCandidates =
            makeEraseIntersectionCandidates(document_,
                                            &eraseSceneCurveCaches_);

        qint64 boundaryUs = 0;
        int nurbsSeedSolves = 0;
        int visiblePointChecks = 0;
        for (const int shapeIndex : selectedTargets) {
            bool hasTargetCurve = false;
            for (const EraseCurveSampleCache &sceneCurve : eraseSceneCurveCaches_) {
                if (sceneCurve.shapeIndex != shapeIndex) {
                    continue;
                }

                EraseCurveSampleCache targetCurve = sceneCurve;
                QElapsedTimer boundaryTimer;
                boundaryTimer.start();
                const EraseIntersectionParameterResult intersections =
                    findEraseIntersectionParameters(
                        sceneCurve.curve,
                        shapeComponentWorkPlaneFrame(
                            shapes_[shapeIndex], sceneCurve.componentIndex),
                        shapes_.objectIdAt(shapeIndex),
                        sceneCurve.componentIndex,
                        sceneIntersectionCandidates.curves,
                        sceneIntersectionCandidates.points,
                        3.0 / std::max<qreal>(
                            viewportTransform_.viewScalePixelsPerWorldUnit(size()),
                            1.0e-9));
                targetCurve.intersectionParameters = intersections.parameters;
                targetCurve.intersectionObjectIds =
                    intersections.intersectingObjectIds;
                const qint64 targetBoundaryUs = boundaryTimer.nsecsElapsed() / 1000;
                boundaryUs += targetBoundaryUs;
                nurbsSeedSolves += intersections.nurbsSeedSolves;
                visiblePointChecks += intersections.pointChecks;

                QStringList intersectionIds;
                for (const ObjectId objectId : intersections.intersectingObjectIds) {
                    intersectionIds.append(QString::number(objectId.value()));
                }
                QStringList boundaryParameters;
                for (const qreal parameter : targetCurve.intersectionParameters) {
                    boundaryParameters.append(QString::number(parameter, 'g', 10));
                }
                const ObjectId objectId = shapes_.objectIdAt(shapeIndex);
                DebugLog::instance().write(
                    QStringLiteral("trim cache target shape=%1 object=%2 type=%3 component=%4 displaySamples=%5 boundaries=[%6] intersectingObjects=[%7] nurbsSeedSolves=%8 pointChecks=%9 boundaryUs=%10")
                        .arg(shapeIndex)
                        .arg(objectId.value())
                        .arg(geometryTypeName(shapes_[shapeIndex].geometryType))
                        .arg(sceneCurve.componentIndex)
                        .arg(sceneCurve.sampled.parameters.size())
                        .arg(boundaryParameters.join(','))
                        .arg(intersectionIds.join(','))
                        .arg(intersections.nurbsSeedSolves)
                        .arg(intersections.pointChecks)
                        .arg(targetBoundaryUs));
                eraseTargetCurveCaches_.append(targetCurve);
                hasTargetCurve = true;
            }
            if (hasTargetCurve) {
                eraseTargetShapeIndices_.append(shapes_.objectIdAt(shapeIndex));
            } else {
                const ObjectId objectId = shapes_.objectIdAt(shapeIndex);
                DebugLog::instance().write(
                    QStringLiteral("trim cache target unavailable shape=%1 object=%2 type=%3")
                        .arg(shapeIndex)
                        .arg(objectId.value())
                        .arg(geometryTypeName(shapes_[shapeIndex].geometryType)));
            }
        }

        int totalSamplePoints = 0;
        for (const EraseCurveSampleCache &sceneCurve : eraseSceneCurveCaches_) {
            totalSamplePoints += sceneCurve.sampled.parameters.size();
        }
        DebugLog::instance().write(
            QStringLiteral("trim cache ready selected=%1 sceneCurves=%2 targetCurves=%3 displaySamples=%4 samplingUs=%5 boundaryUs=%6 nurbsSeedSolves=%7 pointChecks=%8 totalUs=%9")
                .arg(eraseTargetShapeIndices_.size())
                .arg(eraseSceneCurveCaches_.size())
                .arg(eraseTargetCurveCaches_.size())
                .arg(totalSamplePoints)
                .arg(samplingUs)
                .arg(boundaryUs)
                .arg(nurbsSeedSolves)
                .arg(visiblePointChecks)
                .arg(totalTimer.nsecsElapsed() / 1000));
    }


    qreal distanceToCachedEraseShape(const QPointF &screenPosition,
                                     int shapeIndex,
                                     int *closestComponentIndex = nullptr) const
    {
        qreal distance = 1.0e9;
        if (closestComponentIndex != nullptr) {
            *closestComponentIndex = -1;
        }
        for (const EraseCurveSampleCache &targetCurve : eraseTargetCurveCaches_) {
            if (targetCurve.shapeIndex != shapeIndex) {
                continue;
            }

            const qreal componentDistance = distanceToNurbsCurveOnScreen(
                targetCurve.curve,
                targetCurve.workPlaneFrame,
                screenPosition,
                viewportTransform_,
                size());
            if (componentDistance < distance) {
                distance = componentDistance;
                if (closestComponentIndex != nullptr) {
                    *closestComponentIndex = targetCurve.componentIndex;
                }
            }
        }
        return distance;
    }

    void updateTrimHover(const QPointF &screenPosition)
    {
        QElapsedTimer hoverTimer;
        hoverTimer.start();
        invalidateEraseGeometryCacheForView();
        if (trimTool_.hoverPositionMatches(screenPosition) &&
            eraseGeometryCachePrepared_) {
            return;
        }
        trimTool_.setHoverPosition(screenPosition, -1);

        if (!eraseGeometryCachePrepared_) {
            prepareEraseGeometryCache();
        }

        trimTool_.setSinglePointScreenPath(screenPosition);
        trimTool_.clearCandidates();

        const TrimTool::HoverTarget target = trimTool_.updateHover(
            screenPosition,
            eraseTargetShapeIndices_,
            selectedShapeIndex_,
            10.0,
            [this, &screenPosition](ObjectId objectId, int *componentIndex) {
                const int shapeIndex = objectIndex(objectId);
                if (shapeIndex < 0) {
                    if (componentIndex != nullptr) {
                        *componentIndex = -1;
                    }
                    return std::numeric_limits<qreal>::infinity();
                }
                return distanceToCachedEraseShape(screenPosition,
                                                  shapeIndex,
                                                  componentIndex);
            });
        const int closestShapeIndex = objectIndex(target.objectId);
        const int closestComponentIndex = target.componentIndex;
        updateErasePreviewIntervals(true, closestShapeIndex, closestComponentIndex);

        const qint64 elapsedUs = hoverTimer.nsecsElapsed() / 1000;
        if (!trimHoverTimingWindow_.isValid()) {
            trimHoverTimingWindow_.start();
        }
        trimHoverWindowTotalUs_ += elapsedUs;
        trimHoverWindowMaxUs_ = std::max(trimHoverWindowMaxUs_, elapsedUs);
        ++trimHoverWindowEvents_;
        if (elapsedUs >= 20000 || trimHoverTimingWindow_.elapsed() >= 1000) {
            QString targetDescription = QStringLiteral("none");
            int previewIntervalCount = 0;
            if (closestShapeIndex >= 0 && closestShapeIndex < shapes_.size()) {
                const ObjectId objectId = shapes_.objectIdAt(closestShapeIndex);
                targetDescription =
                    QStringLiteral("%1:%2:%3 component=%4")
                        .arg(closestShapeIndex)
                        .arg(objectId.value())
                        .arg(geometryTypeName(shapes_[closestShapeIndex].geometryType))
                        .arg(closestComponentIndex);
                for (const EraseCurveSampleCache &targetCurve :
                     eraseTargetCurveCaches_) {
                    if (targetCurve.shapeIndex == closestShapeIndex &&
                        targetCurve.componentIndex == closestComponentIndex) {
                        previewIntervalCount = targetCurve.previewIntervals.size();
                        break;
                    }
                }
            }
            DebugLog::instance().write(
                QStringLiteral("trim hover window events=%1 totalUs=%2 averageUs=%3 maxUs=%4 target=%5 previewIntervals=%6 at=%7")
                    .arg(trimHoverWindowEvents_)
                    .arg(trimHoverWindowTotalUs_)
                    .arg(trimHoverWindowTotalUs_ /
                         std::max(1, trimHoverWindowEvents_))
                    .arg(trimHoverWindowMaxUs_)
                    .arg(targetDescription)
                    .arg(previewIntervalCount)
                    .arg(pointText(screenPosition)));
            trimHoverTimingWindow_.restart();
            trimHoverWindowTotalUs_ = 0;
            trimHoverWindowMaxUs_ = 0;
            trimHoverWindowEvents_ = 0;
        }
    }

    void trimAtScreenPosition(const QPointF &screenPosition)
    {
        QElapsedTimer clickTimer;
        clickTimer.start();
        updateTrimHover(screenPosition);
        if (trimTool_.candidateObjectIds().isEmpty()) {
            DebugLog::instance().write(
                QStringLiteral("trim click ignored target=none at=%1 selectedTargets=%2 sampledTargets=%3 elapsedUs=%4")
                    .arg(pointText(screenPosition))
                    .arg(eraseTargetShapeIndices_.size())
                    .arg(eraseTargetCurveCaches_.size())
                    .arg(clickTimer.nsecsElapsed() / 1000));
            return;
        }

        const ObjectId targetId = trimTool_.primaryCandidate();
        const int targetIndex = objectIndex(targetId);
        DebugLog::instance().write(
            QStringLiteral("trim click target shape=%1 object=%2 type=%3 component=%4 at=%5")
                .arg(targetIndex)
                .arg(targetId.value())
                .arg(targetIndex >= 0 && targetIndex < shapes_.size()
                         ? geometryTypeName(shapes_[targetIndex].geometryType)
                         : QStringLiteral("unknown"))
                .arg(trimTool_.hoverComponentIndex())
                .arg(pointText(screenPosition)));
        applyEraseCandidates(nullptr, trimTool_.hoverComponentIndex());
        DebugLog::instance().write(
            QStringLiteral("trim click complete object=%1 elapsedUs=%2")
                .arg(targetId.value())
                .arg(clickTimer.nsecsElapsed() / 1000));
        eraseTool_.cancelStroke();
        trimTool_.clearCandidates();
        trimTool_.clearScreenPath();
        eraseTargetShapeIndices_.clear();
        eraseSceneCurveCaches_.clear();
        eraseTargetCurveCaches_.clear();
        eraseGeometryCachePrepared_ = false;
        trimTool_.invalidateHover();
        update();
    }

    void updateTrimBoxPreview()
    {
        if (!trimTool_.boxSelectionActive()) {
            return;
        }
        invalidateEraseGeometryCacheForView();
        if (!eraseGeometryCachePrepared_) {
            prepareEraseGeometryCache();
        }

        const QRectF box =
            QRectF(trimTool_.boxStartPosition(),
                   trimTool_.boxCurrentPosition()).normalized();
        const QPointF drag = trimTool_.boxCurrentPosition() -
                             trimTool_.boxStartPosition();
        // Trim's window/crossing gesture follows the diagonal the user draws:
        // top-left to bottom-right contains; bottom-left to top-right crosses.
        const bool crossingSelection = drag.x() * drag.y() < 0.0;
        trimTool_.selectBoxCandidates(
            eraseTargetShapeIndices_,
            [this, &box, crossingSelection](ObjectId objectId) {
                const int shapeIndex = objectIndex(objectId);
                return shapeIndex >= 0 && shapeIndex < shapes_.size() &&
                       shapeMatchesSelectionBox(shapes_[shapeIndex],
                                                box,
                                                crossingSelection);
            });

        // A non-empty marker enables the existing orange trim preview; the
        // actual interval calculation and commit use the box region below.
        trimTool_.setSinglePointScreenPath(trimTool_.boxStartPosition());
        for (EraseCurveSampleCache &targetCurve : eraseTargetCurveCaches_) {
            targetCurve.previewStrokePointCount = 0;
            const ObjectId objectId = shapes_.objectIdAt(targetCurve.shapeIndex);
            if (!trimTool_.candidateObjectIds().contains(objectId)) {
                targetCurve.previewIntervals.clear();
                continue;
            }

            const QVector<ParameterInterval> hitIntervals =
                nurbsCurveIntervalsInsideScreenBox(targetCurve.curve,
                                                   targetCurve.sampled,
                                                   targetCurve.workPlaneFrame,
                                                   box,
                                                   viewportTransform_,
                                                   size());
            targetCurve.previewIntervals = boundCurveEraseIntervals(
                targetCurve.curve,
                hitIntervals,
                targetCurve.intersectionParameters);
        }
    }

    void finishTrimBoxSelection()
    {
        if (!trimTool_.boxSelectionActive()) {
            return;
        }

        const QRectF box =
            QRectF(trimTool_.boxStartPosition(),
                   trimTool_.boxCurrentPosition()).normalized();
        const QPointF drag = trimTool_.boxCurrentPosition() -
                             trimTool_.boxStartPosition();
        const bool crossingSelection = drag.x() * drag.y() < 0.0;
        const bool moved = trimTool_.boxSelectionMoved() || box.width() >= 3.0 ||
                           box.height() >= 3.0;
        const QPointF clickPosition = trimTool_.boxCurrentPosition();
        if (!moved) {
            resetTrimSelectionBox();
            trimTool_.invalidateHover();
            trimTool_.clearCandidates();
            trimTool_.clearScreenPath();
            trimAtScreenPosition(clickPosition);
            setCursor(Qt::CrossCursor);
            update();
            emitCoordinateUpdate();
            return;
        }

        updateTrimBoxPreview();
        const int candidateCount = trimTool_.candidateObjectIds().size();
        resetTrimSelectionBox();
        trimTool_.invalidateHover();
        if (!trimTool_.candidateObjectIds().isEmpty()) {
            applyEraseCandidates(&box);
        }

        eraseTool_.cancelStroke();
        trimTool_.clearCandidates();
        eraseTargetShapeIndices_.clear();
        eraseSceneCurveCaches_.clear();
        eraseTargetCurveCaches_.clear();
        eraseGeometryCachePrepared_ = false;
        trimTool_.invalidateHover();
        setCursor(Qt::CrossCursor);
        update();
        emitCoordinateUpdate();
        DebugLog::instance().write(
            QStringLiteral("trim box applied crossing=%1 candidates=%2")
                .arg(crossingSelection)
                .arg(candidateCount));
    }






    void updateErasePreviewIntervals(bool reset,
                                     int onlyShapeIndex = -1,
                                     int onlyComponentIndex = -1)
    {
        invalidateEraseGeometryCacheForView();
        if (!eraseGeometryCachePrepared_) {
            prepareEraseGeometryCache();
            if (eraseTool_.strokeActive() && !eraseLikeScreenPath().isEmpty()) {
                eraseAlongScreenSegment(eraseLikeScreenPath().first(),
                                        eraseLikeScreenPath().first());
                for (int point = 1; point < eraseLikeScreenPath().size(); ++point) {
                    eraseAlongScreenSegment(eraseLikeScreenPath()[point - 1],
                                            eraseLikeScreenPath()[point]);
                }
            }
        }
        if (eraseLikeScreenPath().isEmpty()) {
            return;
        }

        const int strokePointCount = eraseLikeScreenPath().size();
        for (EraseCurveSampleCache &targetCurve : eraseTargetCurveCaches_) {
            if (reset || targetCurve.previewStrokePointCount > strokePointCount) {
                targetCurve.previewIntervals.clear();
                targetCurve.previewStrokePointCount = 0;
            }
            if (!eraseCandidates().contains(
                    shapes_.objectIdAt(targetCurve.shapeIndex))) {
                continue;
            }
            if (onlyShapeIndex >= 0 &&
                (targetCurve.shapeIndex != onlyShapeIndex ||
                 targetCurve.componentIndex != onlyComponentIndex)) {
                targetCurve.previewIntervals.clear();
                targetCurve.previewStrokePointCount = strokePointCount;
                continue;
            }

            QVector<ParameterInterval> newHitIntervals;
            int firstStrokeSegment = targetCurve.previewStrokePointCount;
            if (targetCurve.previewStrokePointCount == 0) {
                newHitIntervals = nurbsEraseIntervalsForStrokeSegment(
                    targetCurve.curve,
                    targetCurve.workPlaneFrame,
                    eraseLikeScreenPath().first(),
                    eraseLikeScreenPath().first(),
                    viewportTransform_,
                    size());
                firstStrokeSegment = 1;
            }

            for (int strokeSegment = firstStrokeSegment;
                 strokeSegment < strokePointCount;
                 ++strokeSegment) {
                const QVector<ParameterInterval> segmentIntervals =
                    nurbsEraseIntervalsForStrokeSegment(
                        targetCurve.curve,
                        targetCurve.workPlaneFrame,
                        eraseLikeScreenPath()[strokeSegment - 1],
                        eraseLikeScreenPath()[strokeSegment],
                        viewportTransform_,
                        size());
                for (const ParameterInterval &interval : segmentIntervals) {
                    newHitIntervals.append(interval);
                }
            }

            if (!newHitIntervals.isEmpty()) {
                for (const ParameterInterval &interval : newHitIntervals) {
                    targetCurve.previewIntervals.append(interval);
                }
                targetCurve.previewIntervals = boundCurveEraseIntervals(
                    targetCurve.curve,
                    targetCurve.previewIntervals,
                    targetCurve.intersectionParameters);
            }
            targetCurve.previewStrokePointCount = strokePointCount;
        }
    }

    bool trimShapeAtEraserStroke(const Shape &shape,
                                 const QVector<QPointF> &stroke,
                                 QVector<Shape> *replacement,
                                 int sourceShapeIndex = -1,
                                 const QVector<EraseCurveSampleCache> *cachedTargets = nullptr,
                                 const QRectF *trimBox = nullptr,
                                 int onlyComponentIndex = -1) const
    {
        if (replacement == nullptr) {
            return false;
        }
        replacement->clear();
        TrimEraseShapeQueryResult queryResult;
        const EraseIntersectionResolver intersectionResolver =
            [this, sourceShapeIndex](int componentIndex,
                                     const NurbsCurve2D &curve,
                                     const WorkPlaneFrame &) {
                EraseIntersectionParameterResult intersections;
                intersections.parameters = eraseIntersectionParameters(
                    sourceShapeIndex,
                    componentIndex,
                    curve,
                    nullptr,
                    &intersections.intersectingObjectIds);
                return intersections;
            };
        if (!calculateTrimEraseReplacement(
                shape,
                sourceShapeIndex,
                stroke,
                trimBox,
                activeTool_ == Tool::Erase && trimBox == nullptr,
                onlyComponentIndex,
                cachedTargets,
                joinEndpointTolerance(),
                curveSampler_,
                viewportTransform_,
                size(),
                intersectionResolver,
                &queryResult)) {
            return false;
        }

        if (queryResult.erasedWholeObject) {
            const ObjectId objectId = sourceShapeIndex >= 0 &&
                                              sourceShapeIndex < shapes_.size()
                                          ? shapes_.objectIdAt(sourceShapeIndex)
                                          : ObjectId::invalid();
            DebugLog::instance().write(
                QStringLiteral("erase whole-object object=%1 shape=%2 type=%3 components=%4 reason=no-intersections")
                    .arg(objectId.value())
                    .arg(sourceShapeIndex)
                    .arg(geometryTypeName(shape.geometryType))
                    .arg(queryResult.components.size()));
        }
        if (activeTool_ == Tool::Trim &&
            (trimBox != nullptr || onlyComponentIndex >= 0)) {
            QStringList componentText;
            for (const TrimEraseComponentQueryResult &component :
                 queryResult.components) {
                if (trimBox == nullptr &&
                    component.componentIndex != onlyComponentIndex) {
                    continue;
                }
                QStringList intervalText;
                for (const ParameterInterval &interval : component.removedIntervals) {
                    intervalText.append(
                        QStringLiteral("%1..%2")
                            .arg(interval.start, 0, 'g', 10)
                            .arg(interval.end, 0, 'g', 10));
                }
                componentText.append(
                    QStringLiteral("component=%1 intervals=[%2] boundaries=%3 frameValid=%4")
                        .arg(component.componentIndex)
                        .arg(intervalText.join(','))
                        .arg(component.intersectionParameters.size())
                        .arg(isValidWorkPlaneFrame(component.workPlaneFrame)));
            }
            if (!componentText.isEmpty()) {
                const ObjectId objectId = sourceShapeIndex >= 0 &&
                                                  sourceShapeIndex < shapes_.size()
                                              ? shapes_.objectIdAt(sourceShapeIndex)
                                              : ObjectId::invalid();
                DebugLog::instance().write(
                    QStringLiteral("trim query object=%1 shape=%2 type=%3 mode=%4 %5")
                        .arg(objectId.value())
                        .arg(sourceShapeIndex)
                        .arg(geometryTypeName(shape.geometryType))
                        .arg(trimBox != nullptr ? QStringLiteral("box")
                                                : QStringLiteral("click"))
                        .arg(componentText.join(QStringLiteral("; "))));
            }
        }

        *replacement = std::move(queryResult.replacementShapes);
        return queryResult.changed;
    }

    void eraseAlongScreenSegment(const QPointF &start, const QPointF &end)
    {
        const int addedCandidates = eraseTool_.collectCandidatesAlongSegment(
            start,
            end,
            eraseTargetShapeIndices_,
            [this](const QPointF &cursor, ObjectId objectId) {
                const int shapeIndex = objectIndex(objectId);
                return shapeIndex >= 0 && shapeIndex < shapes_.size()
                           ? distanceToCachedEraseShape(cursor, shapeIndex)
                           : std::numeric_limits<qreal>::infinity();
            });

        if (addedCandidates > 0) {
            DebugLog::instance().write(
                QStringLiteral("erase candidates added=%1 total=%2")
                    .arg(addedCandidates)
                    .arg(eraseTool_.candidateObjectIds().size()));
        }
    }

    void applyEraseCandidates(const QRectF *trimBox = nullptr,
                              int onlyComponentIndex = -1)
    {
        if (eraseCandidates().isEmpty()) {
            return;
        }
        QElapsedTimer operationTimer;
        operationTimer.start();

        QVector<int> indices;
        for (const ObjectId objectId : eraseCandidates()) {
            const int index = objectIndex(objectId);
            if (index >= 0) {
                indices.append(index);
            }
        }
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());

        QVector<TrimEraseReplacement> changes;
        qint64 geometryEvaluationUs = 0;
        for (auto iterator = indices.crbegin(); iterator != indices.crend(); ++iterator) {
            if (*iterator < 0 || *iterator >= shapes_.size()) {
                continue;
            }

            QVector<Shape> replacement;
            QElapsedTimer evaluationTimer;
            evaluationTimer.start();
            const bool changed = trimShapeAtEraserStroke(shapes_[*iterator],
                                                         eraseLikeScreenPath(),
                                                         &replacement,
                                                         *iterator,
                                                         &eraseTargetCurveCaches_,
                                                         trimBox,
                                                         onlyComponentIndex);
            const qint64 evaluationUs = evaluationTimer.nsecsElapsed() / 1000;
            geometryEvaluationUs += evaluationUs;
            const ObjectId objectId = shapes_.objectIdAt(*iterator);
            if (activeTool_ == Tool::Trim) {
                DebugLog::instance().write(
                    QStringLiteral("trim evaluate target shape=%1 object=%2 type=%3 changed=%4 outputPieces=%5 elapsedUs=%6")
                        .arg(*iterator)
                        .arg(objectId.value())
                        .arg(geometryTypeName(shapes_[*iterator].geometryType))
                        .arg(changed)
                        .arg(replacement.size())
                        .arg(evaluationUs));
            }
            if (changed) {
                changes.append(TrimEraseReplacement{
                    shapes_.objectIdAt(*iterator), *iterator, replacement});
            }
        }

        if (changes.isEmpty()) {
            if (activeTool_ == Tool::Trim) {
                QStringList candidateDescriptions;
                for (const int shapeIndex : indices) {
                    const ObjectId objectId = shapes_.objectIdAt(shapeIndex);
                    candidateDescriptions.append(
                        QStringLiteral("%1:%2:%3")
                            .arg(shapeIndex)
                            .arg(objectId.value())
                            .arg(geometryTypeName(shapes_[shapeIndex].geometryType)));
                }
                DebugLog::instance().write(
                    QStringLiteral("trim commit no-change mode=%1 candidates=[%2] geometryUs=%3 totalUs=%4")
                        .arg(trimBox != nullptr ? QStringLiteral("box")
                                                : QStringLiteral("click"))
                        .arg(candidateDescriptions.join(','))
                        .arg(geometryEvaluationUs)
                        .arg(operationTimer.nsecsElapsed() / 1000));
            } else {
                DebugLog::instance().write(QStringLiteral("erase found no trim interval"));
            }
            return;
        }

        QElapsedTimer commitTimer;
        commitTimer.start();
        DocumentTransaction transaction = session_.beginTransaction();
        QStringList changedObjectDescriptions;
        for (const TrimEraseReplacement &change : changes) {
            const int shapeIndex = change.sourceIndex;
            if (shapeIndex < 0 || shapeIndex >= shapes_.size()) {
                continue;
            }

            const ObjectId objectId = change.sourceObjectId;
            changedObjectDescriptions.append(
                QStringLiteral("%1:%2:%3->%4")
                    .arg(shapeIndex)
                    .arg(objectId.value())
                    .arg(geometryTypeName(shapes_[shapeIndex].geometryType))
                    .arg(change.pieces.size()));
        }

        TrimEraseCommandResult commandResult;
        if (!TrimEraseCommand::apply(document_,
                                     transaction,
                                     changes,
                                     &commandResult) ||
            !session_.commitTransaction(transaction)) {
            return;
        }

        const QVector<ObjectId> oldSelectedShapeIndices = selectedShapeIndices_;
        const ObjectId oldSelectedShapeIndex = selectedShapeIndex_;
        QVector<ObjectId> remainingSelection;
        remainingSelection.reserve(oldSelectedShapeIndices.size());
        for (const ObjectId objectId : oldSelectedShapeIndices) {
            if (objectIndex(objectId) >= 0 &&
                !commandResult.removedObjectIds.contains(objectId)) {
                remainingSelection.append(objectId);
            }
        }
        const ObjectId remainingPrimary =
            objectIndex(oldSelectedShapeIndex) >= 0 &&
                    !commandResult.removedObjectIds.contains(oldSelectedShapeIndex)
                ? oldSelectedShapeIndex
                : ObjectId::invalid();
        selection_.setObjectIds(remainingSelection, remainingPrimary);
        clearSelectionDragState();
        selection_.clearActiveControlPoint();
        currentDragSnap_ = DragSnapResult{};
        dragSnapLocked_ = false;
        dragHistoryRecorded_ = false;
        if (activeTool_ == Tool::Trim) {
            QStringList removedIds;
            for (const ObjectId objectId : commandResult.removedObjectIds) {
                removedIds.append(QString::number(objectId.value()));
            }
            DebugLog::instance().write(
                QStringLiteral("trim commit complete mode=%1 candidates=%2 changedObjects=[%3] generatedPieces=%4 removedObjects=[%5] geometryUs=%6 commitUs=%7 totalUs=%8")
                    .arg(trimBox != nullptr ? QStringLiteral("box")
                                            : QStringLiteral("click"))
                    .arg(indices.size())
                    .arg(changedObjectDescriptions.join(','))
                    .arg(commandResult.generatedPieceCount)
                    .arg(removedIds.join(','))
                    .arg(geometryEvaluationUs)
                    .arg(commitTimer.nsecsElapsed() / 1000)
                    .arg(operationTimer.nsecsElapsed() / 1000));
        }
        DebugLog::instance().write(QStringLiteral("erase segment applied candidates=%1 changed=%2 removed=%3 shapes=%4")
                                       .arg(indices.size())
                                       .arg(changes.size())
                                       .arg(commandResult.removedCount)
                                       .arg(shapes_.size()));
        update();
    }

    void cancelEraseStroke()
    {
        eraseTool_.cancelStroke();
        eraseTool_.clearCandidates();
        update();
        DebugLog::instance().write(QStringLiteral("erase stroke canceled"));
    }

    void exitEraseLikeTool()
    {
        cancelEraseStroke();
        setTool(Tool::Select);
        if (commandFinished_) {
            commandFinished_(Tool::Select);
        }
        DebugLog::instance().write(QStringLiteral("erase-like tool exited"));
    }

    void resetScaleInteraction()
    {
        scaleTool_.resetInteraction();
    }

    QString scalePrompt() const
    {
        return scaleTool_.prompt();
    }

    void publishScalePrompt()
    {
        if (toolStatusUpdate_ != nullptr) {
            toolStatusUpdate_(scalePrompt());
        }
    }

    QPointF scaleSelectionBoundsCenter() const
    {
        QRectF combinedBounds;
        bool hasBounds = false;
        for (const ObjectId objectId : scaleState().sourceObjectIds) {
            const int shapeIndex = objectIndex(objectId);
            if (shapeIndex < 0) {
                continue;
            }
            const SceneObject *sceneObject = document_.object(objectId);
            const QRectF bounds = selectionBoundsForShape(
                shapes_[shapeIndex], nullptr,
                sceneObject != nullptr ? sceneObject->placementTranslation
                                       : Point3D{});
            if (bounds.isNull()) {
                continue;
            }
            combinedBounds = hasBounds ? combinedBounds.united(bounds) : bounds;
            hasBounds = true;
        }
        return hasBounds ? screenToWorld(combinedBounds.center()) : QPointF();
    }

    void scaleShapeGeometry(Shape *shape,
                            const QPointF &base,
                            const QPointF &axisDirection,
                            qreal factor,
                            ScaleMode mode) const
    {
        classiCAD::scaleShapeGeometry(shape,
                                      base,
                                      axisDirection,
                                      factor,
                                      mode == ScaleMode::OneD,
                                      viewportTransform_.workPlaneFrame());
    }

    void updateScalePreview(const QPointF &point)
    {
        if (activeTool_ != Tool::Scale || scaleState().stage != 2) {
            return;
        }
        scaleTool_.updatePreview(point);
    }

    void logScaleCommit(ScaleMode mode,
                        qreal factor,
                        const QPointF &basePoint,
                        int sourceCount) const
    {
        const bool changed = std::abs(factor - 1.0) > 1.0e-12;
        DebugLog::instance().write(
            QStringLiteral("scale committed mode=%1 factor=%2 base=%3 objects=%4 changed=%5")
                .arg(scaleModeName(mode))
                .arg(factor, 0, 'g', 10)
                .arg(pointText(basePoint))
                .arg(sourceCount)
                .arg(changed));
    }

    void resetRotateInteraction()
    {
        rotateTool_.resetInteraction(rotateToolPreferences_.angleSnapEnabled);
        currentSnap_ = SnapResult{};
    }

    qreal rotationAngleForPoint(const QPointF &worldPoint) const
    {
        return rotateTool_.angleForPoint(worldPoint);
    }

    qreal rotateSnapIncrementDegrees() const
    {
        return rotateToolPreferences_.useRadians
                   ? rotateToolPreferences_.angleSnapIncrementRadiansDegrees
                   : rotateToolPreferences_.angleSnapIncrementDegrees;
    }

    void updateRotateReferencePreview(const QPointF &point)
    {
        QPointF cursorPoint = cursorWorld_;
        if (rotateTool_.updateReferencePreview(
                point, currentSnap_, rotateSnapIncrementDegrees(),
                rotateToolPreferences_.angleSnapStrengthDegrees,
                &cursorPoint)) {
            cursorWorld_ = cursorPoint;
        }
    }

    void updateRotatePreview(const QPointF &point)
    {
        const bool manualAngle = rotateState().angleInputManual;
        QPointF cursorPoint = cursorWorld_;
        if (rotateTool_.updatePreview(
                point, currentSnap_, rotateSnapIncrementDegrees(),
                rotateToolPreferences_.angleSnapStrengthDegrees,
                &cursorPoint)) {
            cursorWorld_ = cursorPoint;
            if (manualAngle) {
                rawCursorWorld_ = cursorPoint;
                lastWorldPosition_ = cursorPoint;
            }
        }
    }

    bool setRotateTypedAngle(const QString &text)
    {
        QPointF cursorPoint = cursorWorld_;
        if (!rotateTool_.setTypedAngle(text, &cursorPoint)) {
            return false;
        }
        cursorWorld_ = cursorPoint;
        rawCursorWorld_ = cursorPoint;
        lastWorldPosition_ = cursorPoint;
        return true;
    }

    void rotateShapeGeometry(Shape *shape, qreal angle) const
    {
        classiCAD::rotateShapeGeometry(shape,
                                       rotateState().baseWorldPoint,
                                       rotateState().frame.normal,
                                       angle);
    }

    void commitRotate(qreal angle)
    {
        const RotateTool::InteractionState state = rotateState();
        if (!rotateTool_.commitAngle(angle,
                                     rotateToolPreferences_.angleSnapEnabled,
                                     toolContext_)) {
            return;
        }
        currentSnap_ = SnapResult{};
        DebugLog::instance().write(QStringLiteral("rotate committed shapes=%1 angleDegrees=%2 pivot=%3 reference=%4")
                                       .arg(state.sourceObjectIds.size())
                                       .arg(angle * 180.0 / 3.14159265358979323846, 0, 'f', 4)
                                       .arg(precisePoint3DText(state.baseWorldPoint))
                                       .arg(pointText(state.referencePoint)));
        update();
    }

    bool handleRotatePoint(const ToolInput &input)
    {
        if (activeTool_ != Tool::Rotate) {
            return false;
        }
        const int previousStage = rotateState().stage;
        const RotatePointResult result = rotateTool_.acceptPoint(
            input, rotateSnapIncrementDegrees(),
            rotateToolPreferences_.angleSnapStrengthDegrees, toolContext_);
        if (result.updateCursor) {
            cursorWorld_ = result.cursorPoint;
        }
        if (result.updateRawCursor) {
            rawCursorWorld_ = result.cursorPoint;
            lastWorldPosition_ = result.cursorPoint;
        }

        if (result.action == RotatePointAction::PivotCaptured) {
            currentSnap_ = SnapResult{};
            DebugLog::instance().write(QStringLiteral("rotate pivot=%1 planeNormal=(%2,%3,%4)")
                                           .arg(precisePoint3DText(rotateState().baseWorldPoint))
                                           .arg(rotateState().frame.normal.x, 0, 'g', 10)
                                           .arg(rotateState().frame.normal.y, 0, 'g', 10)
                                           .arg(rotateState().frame.normal.z, 0, 'g', 10));
            update();
            return true;
        }

        if (result.action == RotatePointAction::ReferenceCaptured) {
            DebugLog::instance().write(QStringLiteral("rotate reference=%1")
                                           .arg(pointText(rotateState().referencePoint)));
            update();
            return true;
        }

        if (result.action == RotatePointAction::CommitRequested) {
            commitRotate(result.angle);
            return true;
        }

        if (previousStage == 1) {
            DebugLog::instance().write(QStringLiteral("rotate reference ignored at center"));
        } else if (previousStage == 2 &&
                   std::hypot(input.worldPosition.x() - rotateState().basePoint.x(),
                              input.worldPosition.y() - rotateState().basePoint.y()) <=
                       1.0e-9 &&
                   std::abs(rotateState().previewAngle) <= 1.0e-9) {
            DebugLog::instance().write(QStringLiteral("rotate final point ignored at center"));
        }
        return false;
    }

    bool handleRotatePoint(const QPointF &worldPoint)
    {
        ToolInput input;
        input.rawWorldPosition = worldPoint;
        input.worldPosition = worldPoint;
        input.workPlaneFrame = viewportTransform_.workPlaneFrame();
        input.viewportSize = size();
        input.snapResult = currentSnap_;
        return handleRotatePoint(input);
    }

    ToolInput makeRotateToolInput() const
    {
        ToolInput input;
        input.screenPosition = lastMousePosition_;
        input.rawWorldPosition = rawCursorWorld_;
        input.worldPosition = cursorWorld_;
        input.workPlaneFrame = viewportTransform_.workPlaneFrame();
        input.viewportSize = size();
        input.snapResult = currentSnap_;
        input.snapType = currentSnap_.type;
        return input;
    }

    void applyRotateFrameResult(const RotateFrameResult &result)
    {
        if (result.restoreDrawingFrame) {
            updateDrawingWorkPlaneFromHover(QPointF(lastMousePosition_));
        }
        if (!result.changed || !result.updateCursor) {
            return;
        }
        cursorWorld_ = result.cursorPoint;
        rawCursorWorld_ = result.cursorPoint;
        lastWorldPosition_ = result.cursorPoint;
        currentSnap_ = result.snapResult;
    }

    void resetMirrorInteraction()
    {
        mirrorTool_.clearInteraction();
    }

    void translateControlPoint(ObjectId objectId,
                               int controlPointIndex,
                               const QPointF &delta)
    {
        if (controlPointIndex < 0 ||
            (qFuzzyIsNull(delta.x()) && qFuzzyIsNull(delta.y()))) {
            return;
        }

        document_.mutateGeometry(
            objectId,
            [&](Shape &shape) {
                if (shape.geometryType == GeometryType::PolyCurve) {
                    int remaining = controlPointIndex;
                    for (int componentIndex = 0;
                         componentIndex < shape.components.size();
                         ++componentIndex) {
                        Shape::NurbsCurve2D &component = shape.components[componentIndex];
                        if (remaining < component.controlPoints.size()) {
                            const QPointF oldPoint = component.controlPoints[remaining];
                            const WorkPlaneFrame componentFrame =
                                shapeComponentWorkPlaneFrame(shape, componentIndex);
                            const Point3D oldWorldPoint =
                                workPlaneFramePointToWorld(oldPoint, componentFrame);
                            const QPointF newPoint = oldPoint + delta;
                            const Point3D newWorldPoint =
                                workPlaneFramePointToWorld(newPoint, componentFrame);

                            const qreal seamTolerance = joinEndpointTolerance();
                            const auto worldDistance = [](const Point3D &first,
                                                          const Point3D &second) {
                                return std::hypot(
                                    std::hypot(first.x - second.x,
                                               first.y - second.y),
                                    first.z - second.z);
                            };
                            QPointF componentStart;
                            QPointF componentEnd;
                            const bool hasEndpoints =
                                (remaining == 0 ||
                                 remaining == component.controlPoints.size() - 1) &&
                                nurbsCurveEndpoints(component,
                                                   &componentStart,
                                                   &componentEnd);
                            const bool movesJoinedEndpoint = hasEndpoints &&
                                ((remaining == 0 &&
                                  worldDistance(
                                      oldWorldPoint,
                                      workPlaneFramePointToWorld(componentStart,
                                                                 componentFrame)) <=
                                      seamTolerance) ||
                                 (remaining == component.controlPoints.size() - 1 &&
                                 worldDistance(
                                      oldWorldPoint,
                                      workPlaneFramePointToWorld(componentEnd,
                                                                 componentFrame)) <=
                                      seamTolerance));
                            component.controlPoints[remaining] = newPoint;
                            if (movesJoinedEndpoint) {
                                for (int otherIndex = 0;
                                     otherIndex < shape.components.size();
                                     ++otherIndex) {
                                    if (otherIndex == componentIndex) {
                                        continue;
                                    }
                                    Shape::NurbsCurve2D &other =
                                        shape.components[otherIndex];
                                    if (other.controlPoints.isEmpty()) {
                                        continue;
                                    }
                                    const WorkPlaneFrame otherFrame =
                                        shapeComponentWorkPlaneFrame(shape,
                                                                     otherIndex);
                                    QPointF otherStart;
                                    QPointF otherEnd;
                                    if (!nurbsCurveEndpoints(other,
                                                             &otherStart,
                                                             &otherEnd)) {
                                        continue;
                                    }
                                    const int otherLastIndex =
                                        other.controlPoints.size() - 1;
                                    const auto moveMatchingEndpoint =
                                        [&](int controlPointIndex,
                                            const QPointF &curveEndpoint) {
                                            const Point3D endpointWorld =
                                                workPlaneFramePointToWorld(
                                                    curveEndpoint, otherFrame);
                                            const Point3D controlPointWorld =
                                                workPlaneFramePointToWorld(
                                                    other.controlPoints[
                                                        controlPointIndex],
                                                    otherFrame);
                                            if (worldDistance(oldWorldPoint,
                                                              endpointWorld) <=
                                                    seamTolerance &&
                                                worldDistance(endpointWorld,
                                                              controlPointWorld) <=
                                                    seamTolerance) {
                                                other.controlPoints[
                                                    controlPointIndex] =
                                                    worldPointToWorkPlaneFrame(
                                                        newWorldPoint, otherFrame);
                                            }
                                        };
                                    moveMatchingEndpoint(0, otherStart);
                                    if (otherLastIndex != 0) {
                                        moveMatchingEndpoint(otherLastIndex,
                                                             otherEnd);
                                    }
                                }
                            }
                            shape.points = polyCurvePoints(shape.components);
                            return true;
                        }
                        remaining -= component.controlPoints.size();
                    }
                    return false;
                }

                if (!shape.nurbs.controlPoints.isEmpty()) {
                    if (controlPointIndex >= shape.nurbs.controlPoints.size()) {
                        return false;
                    }

                    const QPointF newControlPoint =
                        shape.nurbs.controlPoints[controlPointIndex] + delta;
                    if (!setClosedNurbsSeamControlPoint(&shape,
                                                       controlPointIndex,
                                                       newControlPoint)) {
                        shape.nurbs.controlPoints[controlPointIndex] = newControlPoint;
                    }

                    // These curve types keep their source points in the same
                    // order as their NURBS CVs. Keep both synchronized. Arc
                    // and circle construction points intentionally remain
                    // unchanged; their stored NURBS is the edited geometry.
                    if ((shape.geometryType == GeometryType::Line ||
                         shape.geometryType == GeometryType::Bezier ||
                         shape.geometryType == GeometryType::Nurbs) &&
                        controlPointIndex < shape.points.size()) {
                        shape.points[controlPointIndex] += delta;
                    }
                    if (shape.geometryType == GeometryType::Rectangle) {
                        shape.points = rectangleVertices(shape);
                    }
                    return true;
                }

                if (controlPointIndex < shape.points.size()) {
                    shape.points[controlPointIndex] += delta;
                    return true;
                }
                return false;
            });
    }

    void translateShape(ObjectId objectId, const QPointF &delta)
    {
        if (qFuzzyIsNull(delta.x()) && qFuzzyIsNull(delta.y())) {
            return;
        }

        document_.mutateGeometry(
            objectId,
            [&](Shape &shape) {
                if (validateNurbsSurface(shapeBaseSurface(shape))) {
                    const WorkPlaneFrame inputFrame = viewportTransform_.workPlaneFrame();
                    const Point3D localOrigin = workPlaneFramePointToWorld({}, inputFrame);
                    const Point3D localEnd = workPlaneFramePointToWorld(delta, inputFrame);
                    const Point3D worldDelta{localEnd.x - localOrigin.x,
                                             localEnd.y - localOrigin.y,
                                             localEnd.z - localOrigin.z};
                    for (Point3D &point : shapeBaseSurface(shape).controlPoints) {
                        point.x += worldDelta.x;
                        point.y += worldDelta.y;
                        point.z += worldDelta.z;
                    }
                    return true;
                }
                const WorkPlaneFrame inputFrame = viewportTransform_.workPlaneFrame();
                const Point3D end = workPlaneFramePointToWorld(delta, inputFrame);
                WorkPlaneFrame frame = shapeWorkPlaneFrame(shape);
                const Point3D displacedOrigin{frame.origin.x + end.x - inputFrame.origin.x,
                                             frame.origin.y + end.y - inputFrame.origin.y,
                                             frame.origin.z + end.z - inputFrame.origin.z};
                if (shape.geometryType == GeometryType::PolyCurve &&
                    shape.componentWorkPlaneFrames.size() == shape.components.size()) {
                    const Point3D worldDelta{displacedOrigin.x - frame.origin.x,
                                             displacedOrigin.y - frame.origin.y,
                                             displacedOrigin.z - frame.origin.z};
                    frame.origin = displacedOrigin;
                    shape.workPlaneFrame = frame;
                    for (WorkPlaneFrame &componentFrame :
                         shape.componentWorkPlaneFrames) {
                        componentFrame.origin.x += worldDelta.x;
                        componentFrame.origin.y += worldDelta.y;
                        componentFrame.origin.z += worldDelta.z;
                    }
                    return true;
                }
                const QPointF localDelta = worldPointToWorkPlaneFrame(displacedOrigin, frame);
                translateShapeGeometry(shape, localDelta);
                const Point3D planarEnd = workPlaneFramePointToWorld(localDelta, frame);
                frame.origin.x += displacedOrigin.x - planarEnd.x;
                frame.origin.y += displacedOrigin.y - planarEnd.y;
                frame.origin.z += displacedOrigin.z - planarEnd.z;
                shape.workPlaneFrame = frame;
                return true;
            });
    }

    void translateShapeGeometry(Shape &shape, const QPointF &delta) const
    {
        classiCAD::translateShapeGeometry(
            &shape, delta, viewportTransform_.workPlaneFrame());
    }

    void translateShapes(const QVector<ObjectId> &objectIds, const QPointF &delta)
    {
        QVector<ObjectId> spatialObjects;
        spatialObjects.reserve(objectIds.size());
        QVector<ObjectId> geometryObjects;
        geometryObjects.reserve(objectIds.size());
        for (const ObjectId objectId : objectIds) {
            const Shape *shape = document_.shape(objectId);
            if (shape == nullptr) {
                continue;
            }
            if (shape->geometryType == GeometryType::NurbsSurface ||
                shape->geometryType == GeometryType::NurbsSolid) {
                spatialObjects.append(objectId);
            } else {
                geometryObjects.append(objectId);
            }
        }
        if (!spatialObjects.isEmpty()) {
            const WorkPlaneFrame frame = viewportTransform_.workPlaneFrame();
            const Point3D worldDelta{
                frame.xAxis.x * delta.x() + frame.yAxis.x * delta.y(),
                frame.xAxis.y * delta.x() + frame.yAxis.y * delta.y(),
                frame.xAxis.z * delta.x() + frame.yAxis.z * delta.y()};
            document_.translateObjects(spatialObjects, worldDelta);
        }
        for (const ObjectId objectId : geometryObjects) {
            translateShape(objectId, delta);
        }
    }

    static bool isZeroWorldDelta(const Point3D &delta)
    {
        return qFuzzyIsNull(delta.x) && qFuzzyIsNull(delta.y) &&
               qFuzzyIsNull(delta.z);
    }

    bool worldZAxisPositionAtScreen(const QPointF &screenPosition,
                                   Point3D *axisPosition) const
    {
        if (axisPosition == nullptr) {
            return false;
        }
        constexpr Point3D worldOrigin{};
        constexpr Point3D worldZAxis{0.0, 0.0, 1.0};
        if (viewportTransform_.screenToWorldAxis(screenPosition,
                                                 size(),
                                                 worldOrigin,
                                                 worldZAxis,
                                                 axisPosition)) {
            return true;
        }

        // When the camera looks exactly down world Z, that axis collapses to
        // one screen point. Use vertical mouse motion as the depth control so
        // Z remains usable in a top view too.
        const qreal pixelsPerUnit =
            viewportTransform_.viewScalePixelsPerWorldUnit(size());
        if (!std::isfinite(pixelsPerUnit) || pixelsPerUnit <= 1.0e-12) {
            return false;
        }
        const Point3D viewDirection = viewportTransform_.viewDirection();
        const qreal orientation = viewDirection.z < 0.0 ? -1.0 : 1.0;
        const qreal z = -screenPosition.y() / pixelsPerUnit * orientation;
        if (!std::isfinite(z)) {
            return false;
        }
        *axisPosition = {0.0, 0.0, z};
        return true;
    }

    void updateWorldZAxisDrag(const QVector<ObjectId> &objectIds,
                              const QPointF &screenPosition)
    {
        Point3D axisPosition;
        if (worldZAxisPositionAtScreen(screenPosition, &axisPosition)) {
            if (dragAxisPositionValid_) {
                const Point3D worldDelta{
                    axisPosition.x - dragAxisLastPosition_.x,
                    axisPosition.y - dragAxisLastPosition_.y,
                    axisPosition.z - dragAxisLastPosition_.z};
                if (!isZeroWorldDelta(worldDelta)) {
                    beginDragHistory();
                    translateShapesWorldDelta(objectIds, worldDelta);
                }
            }
            dragAxisLastPosition_ = axisPosition;
            dragAxisPositionValid_ = true;
        }
        currentDragSnap_ = DragSnapResult{};
        dragSnapLocked_ = false;
        setSelectionLastDragWorldPosition(rawCursorWorld_);
    }

    void translateShapesWorldDelta(const QVector<ObjectId> &objectIds,
                                   const Point3D &worldDelta)
    {
        if (isZeroWorldDelta(worldDelta)) {
            return;
        }

        QVector<ObjectId> spatialObjects;
        QVector<ObjectId> geometryObjects;
        spatialObjects.reserve(objectIds.size());
        geometryObjects.reserve(objectIds.size());
        for (const ObjectId objectId : objectIds) {
            const Shape *shape = document_.shape(objectId);
            if (shape == nullptr) {
                continue;
            }
            if (shape->geometryType == GeometryType::NurbsSurface ||
                shape->geometryType == GeometryType::NurbsSolid) {
                spatialObjects.append(objectId);
            } else {
                geometryObjects.append(objectId);
            }
        }
        if (!spatialObjects.isEmpty()) {
            document_.translateObjects(spatialObjects, worldDelta);
        }
        for (const ObjectId objectId : geometryObjects) {
            document_.mutateGeometry(
                objectId,
                [&](Shape &shape) {
                    WorkPlaneFrame frame = shapeWorkPlaneFrame(shape);
                    frame.origin.x += worldDelta.x;
                    frame.origin.y += worldDelta.y;
                    frame.origin.z += worldDelta.z;
                    shape.workPlaneFrame = frame;
                    if (validateNurbsSurface(shapeBaseSurface(shape))) {
                        for (Point3D &point : shapeBaseSurface(shape).controlPoints) {
                            point.x += worldDelta.x;
                            point.y += worldDelta.y;
                            point.z += worldDelta.z;
                        }
                    }
                    for (WorkPlaneFrame &componentFrame :
                         shape.componentWorkPlaneFrames) {
                        componentFrame.origin.x += worldDelta.x;
                        componentFrame.origin.y += worldDelta.y;
                        componentFrame.origin.z += worldDelta.z;
                    }
                    return true;
                });
        }
    }

    void applyObjectDragSnap(const QVector<ObjectId> &objectIds,
                             const DragSnapResult &snap)
    {
        if (!snap.hasWorldTranslation) {
            translateShapes(objectIds, snap.translation);
            return;
        }
        translateShapesWorldDelta(objectIds, snap.worldTranslation);
        DebugLog::instance().write(QStringLiteral("object-drag-snap applied targetShape=%1 worldDelta=(%2,%3,%4)")
            .arg(snap.targetShapeIndex).arg(snap.worldTranslation.x,0,'g',12)
            .arg(snap.worldTranslation.y,0,'g',12).arg(snap.worldTranslation.z,0,'g',12));
    }

    QPointF constrainDragDelta(const QPointF &delta) const
    {
        switch (dragAxisLock_) {
        case DragAxisLock::X:
            return QPointF(delta.x(), 0.0);
        case DragAxisLock::Y:
            return QPointF(0.0, delta.y());
        case DragAxisLock::Z:
            return {};
        case DragAxisLock::None:
            return delta;
        }

        return delta;
    }

    void updateGrabPosition(const QVector<ObjectId> &dragIndices,
                            const QPointF &screenPosition)
    {
        if (!grabTool_.isActive() || grabTool_.isPickingBasePoint()) {
            return;
        }

        const bool viewPlaneGrab = !grabTool_.hasBasePoint() &&
                                   dragAxisLock_ == DragAxisLock::None &&
                                   grabViewPlaneAnchorValid_;
        bool viewPlaneSnapBreakaway = false;
        Point3D viewPlaneSnapCorrection;
        if (dragSnapLocked_) {
            const QPointF snapScreen = viewPlaneGrab &&
                                               dragSnapViewPlaneAnchorValid_
                                           ? selectionDragSnapScreen_
                                           : worldToScreen(dragSnapCursorWorld_);
            const qreal cursorDistanceFromSnap = std::hypot(
                screenPosition.x() - snapScreen.x(),
                screenPosition.y() - snapScreen.y());
            if (cursorDistanceFromSnap <= kDragSnapBreakawayPixels) {
                return;
            }
            viewPlaneSnapBreakaway = viewPlaneGrab;
            if (viewPlaneSnapBreakaway && currentDragSnap_.hasWorldTranslation) {
                viewPlaneSnapCorrection = currentDragSnap_.worldTranslation;
            }
            dragSnapLocked_ = false;
            currentDragSnap_ = DragSnapResult{};
            dragSnapViewPlaneAnchorValid_ = false;
        }

        // Restore only the moving objects. Restoring the whole document here
        // invalidates every stationary object's render and query caches.
        grabTool_.restoreSourceGeometry(document_);
        QPointF totalDelta;
        QPointF freeDestination;
        Point3D worldSnapDelta;
        bool worldSnapDeltaValid = false;
        Point3D appliedWorldDelta;
        if (dragAxisLock_ == DragAxisLock::Z) {
            Point3D startAxisPosition;
            Point3D currentAxisPosition;
            currentSnap_ = SnapResult{};
            if (worldZAxisPositionAtScreen(grabAxisStartScreen_,
                                           &startAxisPosition) &&
                worldZAxisPositionAtScreen(screenPosition,
                                           &currentAxisPosition)) {
                const Point3D worldDelta{
                    currentAxisPosition.x - startAxisPosition.x,
                    currentAxisPosition.y - startAxisPosition.y,
                    currentAxisPosition.z - startAxisPosition.z};
                if (!isZeroWorldDelta(worldDelta)) {
                    translateShapesWorldDelta(dragIndices, worldDelta);
                    grabTool_.setMoved(true);
                } else {
                    grabTool_.setMoved(false);
                }
            } else {
                grabTool_.setMoved(false);
            }
            currentDragSnap_ = DragSnapResult{};
            setSelectionLastDragWorldPosition(rawCursorWorld_);
            return;
        }
        if (grabTool_.hasBasePoint()) {
            QPointF destinationCursor =
                rawCursorWorld_ - grabTool_.cursorOffset();
            const QPointF destinationScreen =
                screenPosition - grabTool_.cursorOffsetScreen();
            viewportTransform_.screenToWorkPlaneUnclipped(
                destinationScreen,
                size(),
                viewportTransform_.workPlaneFrame(),
                &destinationCursor);
            currentSnap_ = findGrabDestinationSnap(destinationCursor);
            const QPointF destination = currentSnap_.isValid()
                                            ? currentSnap_.point
                                            : destinationCursor;
            cursorWorld_ = destination;
            lastWorldPosition_ = destination;
            freeDestination = destinationCursor;
            const QPointF freeFrameDelta =
                destinationCursor - grabTool_.basePointDragPlane();
            totalDelta = currentSnap_.isValid()
                             ? destination - grabTool_.basePoint()
                             : freeFrameDelta;
            if (currentSnap_.isValid() && currentSnap_.hasWorldPoint) {
                const Point3D &baseWorld = grabTool_.basePointWorld();
                worldSnapDelta = {
                    currentSnap_.worldPoint.x - baseWorld.x,
                    currentSnap_.worldPoint.y - baseWorld.y,
                    currentSnap_.worldPoint.z - baseWorld.z};
                worldSnapDeltaValid = true;
                const WorkPlaneFrame &frame = viewportTransform_.workPlaneFrame();
                totalDelta = {
                    worldSnapDelta.x * frame.xAxis.x +
                        worldSnapDelta.y * frame.xAxis.y +
                        worldSnapDelta.z * frame.xAxis.z,
                    worldSnapDelta.x * frame.yAxis.x +
                        worldSnapDelta.y * frame.yAxis.y +
                        worldSnapDelta.z * frame.yAxis.z};
            }
        } else {
            if (dragAxisLock_ == DragAxisLock::None &&
                grabViewPlaneAnchorValid_) {
                QPointF destinationCoordinates;
                if (viewportTransform_.screenToWorkPlaneUnclipped(
                        screenPosition, size(), grabViewPlaneFrame_,
                        &destinationCoordinates)) {
                    const Point3D destination = workPlaneFramePointToWorld(
                        destinationCoordinates, grabViewPlaneFrame_);
                    const Point3D worldDelta{
                        destination.x - grabViewPlaneStartWorld_.x,
                        destination.y - grabViewPlaneStartWorld_.y,
                        destination.z - grabViewPlaneStartWorld_.z};
                    Point3D appliedDelta{
                        worldDelta.x + viewPlaneSnapCorrection.x,
                        worldDelta.y + viewPlaneSnapCorrection.y,
                        worldDelta.z + viewPlaneSnapCorrection.z};
                    if (!isZeroWorldDelta(appliedDelta)) {
                        translateShapesWorldDelta(dragIndices, appliedDelta);
                        grabTool_.setMoved(true);
                    } else {
                        grabTool_.setMoved(false);
                    }
                    currentSnap_ = SnapResult{};
                    currentDragSnap_ = DragSnapResult{};
                    setSelectionLastDragWorldPosition(rawCursorWorld_);
                    DebugLog::instance().write(
                        QStringLiteral("grab view-plane delta=(%1,%2,%3) snap=%4 cursorScreen=%5 pivot=(%6,%7,%8)")
                            .arg(appliedDelta.x, 0, 'g', 12)
                            .arg(appliedDelta.y, 0, 'g', 12)
                            .arg(appliedDelta.z, 0, 'g', 12)
                            .arg(viewPlaneSnapBreakaway
                                     ? QStringLiteral("breakaway")
                                     : QStringLiteral("none"))
                            .arg(pointText(screenPosition))
                            .arg(grabViewPlaneFrame_.origin.x, 0, 'g', 12)
                            .arg(grabViewPlaneFrame_.origin.y, 0, 'g', 12)
                            .arg(grabViewPlaneFrame_.origin.z, 0, 'g', 12));
                    if (!viewPlaneSnapBreakaway) {
                        currentDragSnap_ = findDragSnap(dragIndices);
                        if (currentDragSnap_.isValid()) {
                            applyObjectDragSnap(dragIndices, currentDragSnap_);
                            dragSnapLocked_ = true;
                            dragSnapCursorWorld_ = rawCursorWorld_;
                            dragSnapViewPlaneWorld_ = destination;
                            selectionDragSnapScreen_ = screenPosition;
                            dragSnapViewPlaneAnchorValid_ = true;
                            DebugLog::instance().write(
                                QStringLiteral("grab view-plane snap=%1 source=%2 target=%3")
                                    .arg(snapTypeName(currentDragSnap_.type))
                                    .arg(pointText(currentDragSnap_.sourcePoint))
                                    .arg(pointText(currentDragSnap_.targetPoint)));
                        }
                    }
                    return;
                }
            }
            currentSnap_ = SnapResult{};
            totalDelta = rawCursorWorld_ - grabTool_.startWorldPosition();
        }
        QPointF delta = constrainDragDelta(totalDelta);
        bool snapBlockedByAxisLock = false;
        if (worldSnapDeltaValid && dragAxisLock_ != DragAxisLock::None) {
            const WorkPlaneFrame &frame = viewportTransform_.workPlaneFrame();
            const auto dot = [](const Point3D &first, const Point3D &second) {
                return first.x * second.x + first.y * second.y +
                       first.z * second.z;
            };
            const qreal snapX = dot(worldSnapDelta, frame.xAxis);
            const qreal snapY = dot(worldSnapDelta, frame.yAxis);
            const qreal snapNormal = dot(worldSnapDelta, frame.normal);
            constexpr qreal constraintTolerance = 1.0e-7;
            snapBlockedByAxisLock =
                std::abs(snapNormal) > constraintTolerance ||
                (dragAxisLock_ == DragAxisLock::X &&
                 std::abs(snapY) > constraintTolerance) ||
                (dragAxisLock_ == DragAxisLock::Y &&
                 std::abs(snapX) > constraintTolerance);
        }
        if (grabTool_.hasBasePoint() && currentSnap_.isValid() &&
            (snapBlockedByAxisLock ||
             !qFuzzyIsNull(delta.x() - totalDelta.x()) ||
             !qFuzzyIsNull(delta.y() - totalDelta.y()))) {
            // Do not show a snap marker for a destination the axis lock makes
            // impossible to reach. The cursor still projects onto the locked
            // axis, as it does for an ordinary constrained Grab.
            currentSnap_ = SnapResult{};
            cursorWorld_ = freeDestination;
            lastWorldPosition_ = freeDestination;
            totalDelta = freeDestination - grabTool_.basePointDragPlane();
            delta = constrainDragDelta(totalDelta);
            worldSnapDeltaValid = false;
        }
        if (worldSnapDeltaValid && currentSnap_.isValid()) {
            appliedWorldDelta = worldSnapDelta;
            translateShapesWorldDelta(dragIndices, appliedWorldDelta);
            grabTool_.setMoved(!isZeroWorldDelta(appliedWorldDelta));
        } else if (grabTool_.hasBasePoint()) {
            const WorkPlaneFrame &frame = viewportTransform_.workPlaneFrame();
            appliedWorldDelta = {
                frame.xAxis.x * delta.x() + frame.yAxis.x * delta.y(),
                frame.xAxis.y * delta.x() + frame.yAxis.y * delta.y(),
                frame.xAxis.z * delta.x() + frame.yAxis.z * delta.y()};
            if (!isZeroWorldDelta(appliedWorldDelta)) {
                translateShapesWorldDelta(dragIndices, appliedWorldDelta);
                grabTool_.setMoved(true);
            } else {
                grabTool_.setMoved(false);
            }
        } else if (!qFuzzyIsNull(delta.x()) || !qFuzzyIsNull(delta.y())) {
            translateShapes(dragIndices, delta);
            const WorkPlaneFrame &frame = viewportTransform_.workPlaneFrame();
            appliedWorldDelta = {
                frame.xAxis.x * delta.x() + frame.yAxis.x * delta.y(),
                frame.xAxis.y * delta.x() + frame.yAxis.y * delta.y(),
                frame.xAxis.z * delta.x() + frame.yAxis.z * delta.y()};
            grabTool_.setMoved(true);
        } else {
            grabTool_.setMoved(false);
        }
        currentDragSnap_ = DragSnapResult{};
        if (grabTool_.hasBasePoint() && dragAxisLock_ == DragAxisLock::None) {
            constexpr int maximumTangentRefinements = 12;
            constexpr qreal tangentSnapPrecisionPixels = 0.01;
            QPointF totalTangentCorrection;
            for (int iteration = 0; iteration < maximumTangentRefinements; ++iteration) {
                const DragSnapResult dragSnap = findDragSnap(dragIndices, true);
                if (dragSnap.type != SnapType::Tangent) {
                    break;
                }

                currentDragSnap_ = dragSnap;
                const QPointF sourceScreen = worldToScreen(dragSnap.sourcePoint);
                const QPointF targetScreen = worldToScreen(dragSnap.targetPoint);
                const qreal remainingError =
                    std::hypot(targetScreen.x() - sourceScreen.x(),
                               targetScreen.y() - sourceScreen.y());
                if (remainingError <= tangentSnapPrecisionPixels) {
                    break;
                }

                translateShapes(dragIndices, dragSnap.translation);
                totalTangentCorrection += dragSnap.translation;
                grabTool_.setMoved(true);
            }

            const DragSnapResult finalTangentSnap = findDragSnap(dragIndices, true);
            bool preciselySnapped = finalTangentSnap.type == SnapType::Tangent;
            if (preciselySnapped) {
                const QPointF sourceScreen = worldToScreen(finalTangentSnap.sourcePoint);
                const QPointF targetScreen = worldToScreen(finalTangentSnap.targetPoint);
                preciselySnapped =
                    std::hypot(targetScreen.x() - sourceScreen.x(),
                               targetScreen.y() - sourceScreen.y()) <=
                    tangentSnapPrecisionPixels;
            }

            if (preciselySnapped) {
                currentDragSnap_ = finalTangentSnap;
                currentSnap_ = SnapResult{};
                grabTool_.setMoved(true);
                dragSnapLocked_ = true;
                dragSnapCursorWorld_ = rawCursorWorld_;
                DebugLog::instance().write(
                    QStringLiteral("grab endpoint tangent snap source=%1 target=%2 delta=%3")
                        .arg(pointText(currentDragSnap_.sourcePoint))
                        .arg(pointText(currentDragSnap_.targetPoint))
                        .arg(pointText(currentDragSnap_.translation)));
            } else {
                if (!qFuzzyIsNull(totalTangentCorrection.x()) ||
                    !qFuzzyIsNull(totalTangentCorrection.y())) {
                    translateShapes(dragIndices, -totalTangentCorrection);
                }
                currentDragSnap_ = DragSnapResult{};
            }
        }
        setSelectionLastDragWorldPosition(rawCursorWorld_);
        DebugLog::instance().write(
            QStringLiteral("grab move delta=%1 worldDelta=(%2,%3,%4) cursorWorld=%5 basePoint=%6 baseWorld=(%7,%8,%9) snap=%10 snapWorld=(%11,%12,%13) axisLock=%14")
                .arg(pointText(delta))
                .arg(appliedWorldDelta.x, 0, 'g', 12)
                .arg(appliedWorldDelta.y, 0, 'g', 12)
                .arg(appliedWorldDelta.z, 0, 'g', 12)
                .arg(pointText(rawCursorWorld_))
                .arg(grabTool_.hasBasePoint()
                         ? pointText(grabTool_.basePoint())
                         : QStringLiteral("none"))
                .arg(grabTool_.basePointWorld().x, 0, 'g', 12)
                .arg(grabTool_.basePointWorld().y, 0, 'g', 12)
                .arg(grabTool_.basePointWorld().z, 0, 'g', 12)
                .arg(snapTypeName(currentSnap_.type))
                .arg(currentSnap_.worldPoint.x, 0, 'g', 12)
                .arg(currentSnap_.worldPoint.y, 0, 'g', 12)
                .arg(currentSnap_.worldPoint.z, 0, 'g', 12)
                .arg(dragAxisLockName(dragAxisLock_)));
    }

    QPointF screenToWorld(const QPointF &screen) const
    {
        return viewportTransform_.screenToWorld(screen, size());
    }

    void resetArcInputState()
    {
        arcTool_.resetInputState();
    }

    void synchronizeArcInputPoints()
    {
        pendingPoints_ = arcTool_.inputPoints();
    }

    void appendArcInputPoint(const QPointF &point)
    {
        arcTool_.appendInputPoint(point);
        synchronizeArcInputPoints();
    }

    void setArcInputPoint(int index, const QPointF &point)
    {
        if (arcTool_.setInputPoint(index, point)) {
            synchronizeArcInputPoints();
        }
    }

    void applyArcTextInput()
    {
        const ArcTextInputUpdate inputUpdate = arcTool_.applyTextInput(
            document_.settings(), cursorWorld_, currentSnap_.isValid());
        pendingPoints_ = arcTool_.inputPoints();
        if (inputUpdate.hasLengthValue) {
            setTwoPointArcChordLength(inputUpdate.lengthValue);
        }
        if (inputUpdate.updateCursor) {
            cursorWorld_ = inputUpdate.cursorPoint;
            lastWorldPosition_ = cursorWorld_;
            cursorValid_ = true;
        }
        if (inputUpdate.updateRawCursor) {
            rawCursorWorld_ = inputUpdate.cursorPoint;
        }
        if (inputUpdate.clearGeometrySnap) {
            currentSnap_ = SnapResult{};
        }
        update();
        emitCoordinateUpdate();
    }

    bool handleArcKeyInput(const ToolInput &input, QKeyEvent *event)
    {
        if (event == nullptr || activeTool_ != Tool::Arc) {
            return false;
        }
        const ArcKeyResult result = arcTool_.handleKeyInput(
            input.key, input.text, input.modifiers, input.autoRepeat);
        if (!result.handled) {
            return false;
        }

        switch (result.action) {
        case ArcKeyAction::BeginTextInput:
        case ArcKeyAction::ChangeTextInput:
            update();
            break;
        case ArcKeyAction::ApplyTextInput:
            applyArcTextInput();
            event->accept();
            return true;
        case ArcKeyAction::CancelArc:
            setTool(Tool::Select);
            if (commandFinished_) {
                commandFinished_(Tool::Select);
            }
            update();
            event->accept();
            return true;
        case ArcKeyAction::ToggleAngleSnap:
            update();
            break;
        case ArcKeyAction::TogglePlaneLock:
            toggleArcPlaneLock();
            break;
        case ArcKeyAction::TogglePerpendicularPlane:
            toggleArcPerpendicularPlane();
            break;
        case ArcKeyAction::FinishArc:
            finishArcAt(cursorWorld_);
            event->accept();
            return true;
        case ArcKeyAction::Unhandled:
            return false;
        }

        event->accept();
        return true;
    }

    QPointF currentArcScreenPosition(const QPointF *screenPosition = nullptr) const
    {
        if (screenPosition != nullptr) {
            return *screenPosition;
        }
        if (cursorValid_ && rect().contains(lastMousePosition_)) {
            return lastMousePosition_;
        }
        const QPoint cursorPosition = mapFromGlobal(QCursor::pos());
        return rect().contains(cursorPosition) ? QPointF(cursorPosition)
                                               : QPointF(lastMousePosition_);
    }

    ArcClickResult dispatchArcClick(const ToolInput &input,
                                   bool worldPositionValid)
    {
        ArcClickResult result;
        if (activeTool_ != Tool::Arc) {
            return result;
        }

        result = arcTool_.handleClick(input, worldPositionValid, toolContext_);
        if (result.chordEndpointCaptured) {
            updateArcTwoPointWorkPlaneForView();
        }
        synchronizeArcInputPoints();
        logArcCommit(result.commit);
        return result;
    }

    void finishArcAt(const QPointF &cursorPoint)
    {
        if (activeTool_ != Tool::Arc ||
            !isValidWorkPlaneFrame(viewportTransform_.workPlaneFrame())) {
            return;
        }
        const ArcCommitResult result = arcTool_.commitAt(
            cursorPoint, currentSnap_.isValid(), toolContext_);
        logArcCommit(result);
        if (result.committed) {
            synchronizeArcInputPoints();
            update();
            emitCoordinateUpdate();
        }
    }

    void logArcCommit(const ArcCommitResult &result)
    {
        if (!result.committed) {
            return;
        }
        switch (result.mode) {
        case ArcMode::OnePoint:
            DebugLog::instance().write(
                QStringLiteral("one-point arc committed by finish action radius=%1 sweep=%2")
                    .arg(result.radius, 0, 'f', 4)
                    .arg(result.sweep, 0, 'f', 6));
            break;
        case ArcMode::TwoPoint:
            DebugLog::instance().write(
                QStringLiteral("two-point arc committed chord=%1 sagitta=%2")
                    .arg(result.chordLength, 0, 'f', 4)
                    .arg(result.sagitta, 0, 'f', 4));
            break;
        case ArcMode::ThreePoint:
            DebugLog::instance().write(
                QStringLiteral("three-point arc committed radius=%1")
                    .arg(result.radius, 0, 'f', 4));
            break;
        }
    }

    void restoreArcChordReferencePlaneForEndpointPick()
    {
        arcTool_.restoreChordReferenceFrame(toolContext_);
        synchronizeArcInputPoints();
    }

    void updateArcTwoPointWorkPlaneForView(
        const Point3D *previewEndpointWorld = nullptr)
    {
        arcTool_.updateChordWorkPlane(toolContext_, previewEndpointWorld);
        synchronizeArcInputPoints();
    }

    void setTwoPointArcChordLength(qreal chordLength)
    {
        ToolInput input;
        input.rawWorldPosition = rawCursorWorld_;
        input.worldPosition = cursorWorld_;
        input.workPlaneFrame = viewportTransform_.workPlaneFrame();
        input.viewportSize = size();
        const ArcChordLengthResult result = arcTool_.applyChordLength(
            chordLength, input, toolContext_);
        if (!result.applied) {
            return;
        }
        synchronizeArcInputPoints();
        cursorWorld_ = result.cursorPoint;
        rawCursorWorld_ = cursorWorld_;
        lastWorldPosition_ = cursorWorld_;
        cursorValid_ = true;
        currentSnap_ = SnapResult{};
    }

    void toggleArcPlaneLock()
    {
        const bool refreshDrawingPlane = arcTool_.togglePlaneLock(
            viewportTransform_.workPlaneFrame(),
            arcTool_.inputStage() == ArcInputStage::FirstPoint);
        if (refreshDrawingPlane) {
            updateDrawingWorkPlaneFromHover(currentArcScreenPosition());
        }
        update();
    }

    void toggleArcPerpendicularPlane()
    {
        if (activeTool_ != Tool::Arc) {
            return;
        }
        const ArcPlaneToggleResult result = arcTool_.togglePerpendicularPlane(
            cursorWorld_, toolContext_);
        if (!result.changed) {
            return;
        }
        synchronizeArcInputPoints();
        if (result.refreshCursor) {
            refreshArcCursorOnCurrentFrame(result.forceCursorRefresh);
        }
        if (result.rebaseOnePointPreview) {
            arcTool_.rebaseOnePointPreview(cursorWorld_);
        }
        update();
        emitCoordinateUpdate();
    }

    QPointF constrainArcChordEndpoint(const QPointF &rawPoint,
                                      bool altModifier,
                                      const QPointF &screenPosition)
    {
        ToolInput input;
        input.screenPosition = screenPosition;
        input.rawWorldPosition = rawPoint;
        input.worldPosition = rawPoint;
        input.workPlaneFrame = viewportTransform_.workPlaneFrame();
        input.viewportSize = size();
        input.snapResult = currentSnap_;
        input.snapType = currentSnap_.type;
        if (altModifier) {
            input.modifiers |= Qt::AltModifier;
        }
        const ArcEndpointConstraintResult result =
            arcTool_.constrainChordEndpoint(input, toolContext_);
        currentSnap_ = result.snapResult;
        if (result.updateRawCursor) {
            rawCursorWorld_ = result.rawCursorPoint;
        }
        return result.point;
    }

    void refreshArcCursorOnCurrentFrame(bool force = false)
    {
        if (!cursorValid_ && !force) {
            return;
        }
        const QPointF screenPosition = currentArcScreenPosition();
        QPointF rawPosition;
        if (!viewportTransform_.screenToWorkPlane(
                screenPosition,
                size(),
                viewportTransform_.workPlaneFrame(),
                &rawPosition)) {
            return;
        }
        rawCursorWorld_ = rawPosition;
        cursorWorld_ = constrainLinePoint(rawPosition, false, &screenPosition);
        lastWorldPosition_ = cursorWorld_;
    }

    bool handleArcAxisKey(const ToolInput &input)
    {
        if (activeTool_ != Tool::Arc) {
            return false;
        }
        const ArcAxisKeyResult result = arcTool_.handleAxisKey(
            input.key, input, toolContext_);
        if (!result.handled) {
            return false;
        }
        if (result.drawingFrameChanged) {
            toolDrawingFrame_ = result.drawingFrame;
            toolDrawingPlaneLocked_ = result.drawingPlaneLocked;
        }
        synchronizeArcInputPoints();
        if (result.refreshCursor) {
            refreshArcCursorOnCurrentFrame();
        }
        return true;
    }

    DrawingPlaneResolutionRequest drawingPlaneRequest(bool drawingShape,
                                                       bool selectingObject) const
    {
        DrawingPlaneResolutionRequest request;
        request.activeTool = activeTool_;
        request.drawingShape = drawingShape;
        request.selectingObject = selectingObject;
        request.controlPointsVisible = controlPointsVisible_;
        request.perspectiveEnabled = viewportTransform_.isPerspectiveEnabled();
        request.viewPreset = viewportTransform_.viewPreset();
        request.viewDirection = viewportTransform_.viewDirection();
        request.viewUp = viewportTransform_.viewUp();
        request.viewTarget = viewportTransform_.viewTarget();
        return request;
    }

    void applyDrawingPlaneResolution(
        const DrawingPlaneResolution &resolution,
        const QPointF &screenPosition,
        int hoveredShapeIndex = -1)
    {
        if (!resolution.hasFrame()) {
            return;
        }

        WorkPlaneFrame frame = resolution.frame;
        if (resolution.kind ==
            DrawingPlaneResolutionKind::EdgeOnSelectionDrag) {
            Point3D pickWorld = frame.origin;
            curveHitTester_.hitTestVisibleDepth(document_,
                                                screenPosition,
                                                viewportTransform_,
                                                size(),
                                                &pickWorld);
            frame.origin = pickWorld;
        }

        if (resolution.usesPrincipalPlane()) {
            const bool frameChanged = !workPlaneFramesMatch(
                viewportTransform_.workPlaneFrame(), frame);
            const bool planeChanged = !workPlaneMatches(
                viewportTransform_.workPlane(),
                viewportTransform_.workPlaneOffset(),
                resolution.plane,
                0.0);
            if (frameChanged || planeChanged) {
                viewportTransform_.setWorkPlane(resolution.plane, 0.0);
                notifyViewStateChanged();
            }
        } else {
            viewportTransform_.setWorkPlaneFrame(frame);
        }

        if (resolution.kind == DrawingPlaneResolutionKind::HoveredObject &&
            hoveredShapeIndex >= 0 && hoveredShapeIndex < shapes_.size() &&
            qEnvironmentVariable("CLASSICAD_SNAP_TRACE") ==
                QStringLiteral("1")) {
            const auto point3DText = [](const Point3D &point) {
                return QStringLiteral("(%1, %2, %3)")
                    .arg(point.x, 0, 'g', 12)
                    .arg(point.y, 0, 'g', 12)
                    .arg(point.z, 0, 'g', 12);
            };
            DebugLog::instance().write(
                QStringLiteral("osnap-trace hover-frame shape=%1 geometry=%2 screen=%3 origin=%4 x=%5 y=%6 normal=%7")
                    .arg(hoveredShapeIndex)
                    .arg(geometryTypeName(
                        shapes_[hoveredShapeIndex].geometryType))
                    .arg(precisePointText(screenPosition))
                    .arg(point3DText(frame.origin))
                    .arg(point3DText(frame.xAxis))
                    .arg(point3DText(frame.yAxis))
                    .arg(point3DText(frame.normal)));
        }
    }

    static bool usesHoveredFaceDrawingPlane(ToolId tool)
    {
        if (isRectangleTool(tool) || isPolygonTool(tool) ||
            isCircleConstructionTool(tool) || isEllipseTool(tool)) {
            return true;
        }
        switch (tool) {
        case Tool::Line:
        case Tool::PerpendicularFromCurve:
        case Tool::PerpendicularFromEdge:
        case Tool::TangentFromCurve:
        case Tool::TangentToTwoCurves:
        case Tool::PerpendicularToTwoCurves:
        case Tool::CurveInterpolate:
        case Tool::CurveFreehand:
        case Tool::PointByLine:
        case Tool::PointByArcs:
            return true;
        default:
            return false;
        }
    }

    bool setWorkPlaneFromVisibleSurface(const QPointF &screenPosition)
    {
        Point3D facePoint;
        Point3D faceNormal;
        if (!curveHitTester_.hitTestVisibleSurface(
                document_, screenPosition, viewportTransform_, size(),
                &facePoint, &faceNormal)) {
            return false;
        }

        const Point3D reference = std::abs(faceNormal.x) < 0.99
                                      ? Point3D{1.0, 0.0, 0.0}
                                      : Point3D{0.0, 1.0, 0.0};
        const auto cross = [](const Point3D &first, const Point3D &second) {
            return Point3D{
                first.y * second.z - first.z * second.y,
                first.z * second.x - first.x * second.z,
                first.x * second.y - first.y * second.x};
        };
        const Point3D yAxis = cross(faceNormal, reference);
        const Point3D xAxis = cross(yAxis, faceNormal);
        const WorkPlaneFrame faceFrame = makeWorkPlaneFrameFromNormal(
            facePoint, faceNormal, xAxis);
        if (!isValidWorkPlaneFrame(faceFrame)) {
            return false;
        }

        viewportTransform_.setWorkPlaneFrame(faceFrame);
        return true;
    }

    void updateDrawingWorkPlaneFromHover(const QPointF &screenPosition)
    {
        if (objectSelectionDragActive() || controlPointSelectionDragActive() ||
            grabTool_.isActive() || duplicateTool_.isActive()) {
            return;
        }
        if (activeTool_ == Tool::Arc &&
            arcTool_.inputStage() == ArcInputStage::FirstPoint &&
            arcState().planeLocked && arcState().lockedFrameValid) {
            viewportTransform_.setWorkPlaneFrame(arcState().lockedFrame);
            return;
        }
        if (activeTool_ == Tool::Rotate) {
            if (rotateState().stage > 0 && isValidWorkPlaneFrame(rotateState().frame)) {
                viewportTransform_.setWorkPlaneFrame(rotateState().frame);
                return;
            }
            if ((rotateState().axisLockKey != 0 ||
                 rotateState().prePivotPerpendicularActive) &&
                isValidWorkPlaneFrame(rotateState().prePivotPlaneFrame)) {
                viewportTransform_.setWorkPlaneFrame(rotateState().prePivotPlaneFrame);
                return;
            }

            // The add-on's shared one-point tool updates its plane normal
            // from the face hit under the cursor until the pivot is clicked.
            // An object's stored workplane cannot represent individual solid
            // faces, so resolve the visible tessellated face here first.
            Point3D facePoint;
            Point3D faceNormal;
            if (curveHitTester_.hitTestVisibleSurface(
                    document_, screenPosition, viewportTransform_, size(),
                    &facePoint, &faceNormal)) {
                const Point3D reference = std::abs(faceNormal.x) < 0.99
                                              ? Point3D{1.0, 0.0, 0.0}
                                              : Point3D{0.0, 1.0, 0.0};
                const auto cross = [](const Point3D &first,
                                      const Point3D &second) {
                    return Point3D{
                        first.y * second.z - first.z * second.y,
                        first.z * second.x - first.x * second.z,
                        first.x * second.y - first.y * second.x};
                };
                const Point3D yAxis = cross(faceNormal, reference);
                const Point3D xAxis = cross(yAxis, faceNormal);
                const WorkPlaneFrame faceFrame = makeWorkPlaneFrameFromNormal(
                    facePoint, faceNormal, xAxis);
                if (isValidWorkPlaneFrame(faceFrame)) {
                    viewportTransform_.setWorkPlaneFrame(faceFrame);
                    return;
                }
            }

            DrawingPlaneResolutionRequest request =
                drawingPlaneRequest(true, false);
            const int shapeIndex = curveHitTester_.hitTestShapeOnAnyWorkPlane(
                document_, screenPosition, viewportTransform_, size());
            if (shapeIndex >= 0 && shapeIndex < shapes_.size()) {
                request.hasHoveredPlane = true;
                request.hoveredPlane = shapeWorkPlaneFrame(shapes_[shapeIndex]);
            }
            applyDrawingPlaneResolution(DrawingPlaneResolver::resolve(request),
                                        screenPosition,
                                        shapeIndex);
            return;
        }
        if (activeTool_ != Tool::Line && toolDrawingPlaneLocked_ &&
            isValidWorkPlaneFrame(toolDrawingFrame_)) {
            viewportTransform_.setWorkPlaneFrame(toolDrawingFrame_);
            return;
        }
        if (usesHoveredFaceDrawingPlane(activeTool_) &&
            pendingPoints_.isEmpty() &&
            setWorkPlaneFromVisibleSurface(screenPosition)) {
            return;
        }
        if (activeTool_ == Tool::Arc &&
            (arcState().mode == ArcMode::OnePoint ||
             arcState().mode == ArcMode::TwoPoint ||
             arcState().mode == ArcMode::ThreePoint) &&
            arcTool_.inputStage() == ArcInputStage::FirstPoint) {
            // Arc modes orient their drawing plane to the hovered face before
            // the first endpoint. Object workplanes lose the individual face
            // normal on solids, so use the visible face hit.
            Point3D facePoint;
            Point3D faceNormal;
            if (curveHitTester_.hitTestVisibleSurface(
                    document_, screenPosition, viewportTransform_, size(),
                    &facePoint, &faceNormal)) {
                const Point3D reference = std::abs(faceNormal.x) < 0.99
                                              ? Point3D{1.0, 0.0, 0.0}
                                              : Point3D{0.0, 1.0, 0.0};
                const auto cross = [](const Point3D &first,
                                      const Point3D &second) {
                    return Point3D{
                        first.y * second.z - first.z * second.y,
                        first.z * second.x - first.x * second.z,
                        first.x * second.y - first.y * second.x};
                };
                const Point3D yAxis = cross(faceNormal, reference);
                const Point3D xAxis = cross(yAxis, faceNormal);
                const WorkPlaneFrame faceFrame = makeWorkPlaneFrameFromNormal(
                    facePoint, faceNormal, xAxis);
                if (isValidWorkPlaneFrame(faceFrame)) {
                    viewportTransform_.setWorkPlaneFrame(faceFrame);
                    return;
                }
            }
        }
        const bool unlockedPointInput =
            (activeTool_ == Tool::PointByLine ||
             activeTool_ == Tool::PointByArcs) && !toolDrawingPlaneLocked_;
        if ((activeTool_ == Tool::Line && toolPreview_.planeLocked) ||
            (activeTool_ != Tool::Line && !pendingPoints_.isEmpty() &&
             !unlockedPointInput)) {
            return;
        }

        const bool drawingShape =
            geometryTypeForTool(activeTool_) != GeometryType::Invalid;
        const bool selectingObject = activeTool_ == Tool::Select;
        DrawingPlaneResolutionRequest request =
            drawingPlaneRequest(drawingShape, selectingObject);
        if (!drawingShape && !selectingObject) {
            return;
        }

        const int shapeIndex = selectingObject
            ? curveHitTester_.hitTestShape(document_,
                                           screenPosition,
                                           viewportTransform_,
                                           size())
            : curveHitTester_.hitTestShapeOnAnyWorkPlane(
                  document_, screenPosition, viewportTransform_, size());
        if (shapeIndex >= 0 && shapeIndex < shapes_.size()) {
            request.hasHoveredPlane = true;
            request.hoveredPlane = shapeWorkPlaneFrame(shapes_[shapeIndex]);
            if (drawingShape) {
                QPointF planePoint;
                request.hoveredPlaneUsable = viewportTransform_.screenToWorkPlane(
                    screenPosition,
                    size(),
                    request.hoveredPlane,
                    &planePoint);
            }
        }

        applyDrawingPlaneResolution(DrawingPlaneResolver::resolve(request),
                                    screenPosition,
                                    shapeIndex);
    }

    QPointF worldToScreen(const QPointF &world) const
    {
        return viewportTransform_.worldToScreen(world, size());
    }

    void drawSnapMarker(QPainter &painter,
                        SnapType type,
                        const QPointF &worldPoint)
    {
        viewportOverlay_.drawSnapMarker(painter, type, worldPoint, size());
    }

    void drawSnapMarker(QPainter &painter, const SnapResult &snap)
    {
        if (snap.hasWorldPoint) {
            viewportOverlay_.drawWorldSnapMarker(
                painter, snap.type, snap.worldPoint, size());
        } else {
            drawSnapMarker(painter, snap.type, snap.point);
        }
    }

    void drawLineToolPreview(QPainter &painter, bool drawCurve = true)
    {
        if (activeTool_ == Tool::Line && !toolPreview_.worldPoints.isEmpty()) {
            viewportOverlay_.toolPreviewRenderer().drawWorldLinePreview(painter,
                toolPreview_.worldPoints,
                toolPreview_.worldCursorPoint, cursorValid_, size(), drawCurve,
                toolPreview_.workPlaneFrame);
            if (currentSnap_.isValid()) {
                drawSnapMarker(painter, currentSnap_);
            }
            return;
        }
        viewportOverlay_.toolPreviewRenderer().drawLinePreview(painter,
                                         pendingPoints_,
                                         cursorWorld_,
                                         cursorValid_,
                                         currentSnap_,
                                         size(),
                                         drawCurve,
                                         activeTool_ == Tool::Line
                                             ? toolPreview_.workPlaneFrame
                                             : WorkPlaneFrame{});
    }

    void drawMirrorToolPreview(
        QPainter &painter,
        const QVector<ObjectId> &gpuHandledObjects = {})
    {
        if (!mirrorTool_.hasAxisStart() || !cursorValid_) {
            return;
        }

        const QPointF axisStart = mirrorTool_.axisStart();
        const QPointF axisEnd = cursorWorld_;
        for (const ObjectId objectId : mirrorTool_.sourceObjectIds()) {
            const int shapeIndex = objectIndex(objectId);
            if (shapeIndex < 0 || shapeIndex >= shapes_.size() ||
                !document_.isObjectVisible(objectId)) {
                continue;
            }

            Shape sourceShape = shapes_[shapeIndex];
            const SceneObject *sourceObject = document_.object(objectId);
            if (sourceObject != nullptr &&
                (sourceObject->placementTranslation.x != 0.0 ||
                 sourceObject->placementTranslation.y != 0.0 ||
                 sourceObject->placementTranslation.z != 0.0)) {
                bakeShapePlacementTranslation(
                    &sourceShape, sourceObject->placementTranslation);
            }
            Shape mirroredShape;
            if (mirrorShapeAcrossLine(sourceShape,
                                      axisStart,
                                      axisEnd,
                                      &mirroredShape)) {
                if (!gpuHandledObjects.contains(objectId)) {
                    drawShape(painter, mirroredShape, true, false, false);
                }
                if (!subdivisionTool_.isActive() ||
                    objectId != subdivisionTool_.targetObjectId()) {
                    drawSubdivisionPoints(painter,
                                          mirroredShape,
                                          mirroredShape.subdivisionParameters,
                                          false);
                }
            }
        }
    }

    void resetArcPreviewTracking()
    {
        arcTool_.resetPreviewTracking();
    }

    void initializeArcPreviewTracking()
    {
        arcTool_.initializePreviewTracking();
    }

    void updateArcPreviewTracking(const QPointF &cursorWorld)
    {
        arcTool_.updatePreviewTracking(cursorWorld, currentSnap_.isValid());
    }

    qreal arcCompassRotation() const
    {
        qreal rotation = 0.0;
        if (arcState().mode == ArcMode::OnePoint && !pendingPoints_.isEmpty()) {
            const QPointF center = pendingPoints_.first();
            const QPointF radiusVector = pendingPoints_.size() >= 2
                                             ? pendingPoints_[1] - center
                                             : cursorWorld_ - center;
            if ((pendingPoints_.size() >= 2 || cursorValid_) &&
                std::hypot(radiusVector.x(), radiusVector.y()) > 1.0e-9) {
                rotation = std::atan2(radiusVector.y(), radiusVector.x());
            }
        }
        return rotation;
    }

    ArcHudDisplay onePointArcHudDisplay() const
    {
        constexpr qreal pi = 3.14159265358979323846;
        qreal radius = 0.0;
        if (pendingPoints_.size() >= 2) {
            const QPointF vector = pendingPoints_[1] - pendingPoints_[0];
            radius = std::hypot(vector.x(), vector.y());
        } else if (pendingPoints_.size() == 1 && cursorValid_) {
            const QPointF vector = cursorWorld_ - pendingPoints_[0];
            radius = std::hypot(vector.x(), vector.y());
        }
        const DocumentSettings settings = document_.settings();
        const qreal unitsPerMillimeter =
            1.0 / millimetersPerDocumentUnit(settings.lengthUnit);
        const QString radiusText = arcTool_.textInputMode() == ArcTextInputMode::Radius
                                       ? arcTool_.textInput() + QLatin1Char('|')
                                       : QStringLiteral("%1 %2")
                                             .arg(radius * unitsPerMillimeter,
                                                  0,
                                                  'f',
                                                  3)
                                             .arg(arcLengthUnitSuffix(settings.lengthUnit));
        const QString angleText = arcTool_.textInputMode() == ArcTextInputMode::Angle
                                      ? arcTool_.textInput() + QLatin1Char('|')
                                      : QStringLiteral("%1°")
                                            .arg(-arcState().previewSweepAngle * 180.0 / pi,
                                                 0,
                                                 'f',
                                                 1);
        const QString stageHint = arcTool_.textInputMode() != ArcTextInputMode::None
                                      ? QStringLiteral("Enter applies value")
                                      : pendingPoints_.isEmpty()
                                            ? QStringLiteral("Click center")
                                            : pendingPoints_.size() == 1
                                                  ? QStringLiteral("Click radius")
                                                  : QStringLiteral("Click sweep to finish");
        return {QStringLiteral("R: %1    ∠ %2").arg(radiusText, angleText),
                QStringLiteral("%1  •  Esc exits  •  C snap %2  •  R radius  •  A angle  •  P perp  •  L plane lock")
                    .arg(stageHint,
                         arcState().angleSnapEnabled ? QStringLiteral("on")
                                              : QStringLiteral("off"))};
    }

    ArcHudDisplay twoPointArcHudDisplay() const
    {
        qreal chordLength = 0.0;
        qreal height = 0.0;
        if (pendingPoints_.size() == 1 && cursorValid_) {
            const QPointF chord = cursorWorld_ - pendingPoints_[0];
            chordLength = std::hypot(chord.x(), chord.y());
        } else if (pendingPoints_.size() >= 2) {
            const QPointF chord = pendingPoints_[1] - pendingPoints_[0];
            chordLength = std::hypot(chord.x(), chord.y());
            if (cursorValid_ && chordLength > 1.0e-9) {
                const QPointF midpoint = (pendingPoints_[0] + pendingPoints_[1]) * 0.5;
                const QPointF perpendicular(-chord.y() / chordLength,
                                            chord.x() / chordLength);
                height = std::abs(QPointF::dotProduct(cursorWorld_ - midpoint,
                                                      perpendicular));
            }
        }

        const DocumentSettings settings = document_.settings();
        const qreal unitsPerMillimeter =
            1.0 / millimetersPerDocumentUnit(settings.lengthUnit);
        const QString chordText = arcTool_.textInputMode() == ArcTextInputMode::ChordLength
                                       ? arcTool_.textInput() + QLatin1Char('|')
                                       : QStringLiteral("%1 %2")
                                             .arg(chordLength * unitsPerMillimeter,
                                                  0, 'f', 3)
                                             .arg(arcLengthUnitSuffix(settings.lengthUnit));
        const QString heightText = arcTool_.textInputMode() == ArcTextInputMode::Sagitta
                                       ? arcTool_.textInput() + QLatin1Char('|')
                                       : QStringLiteral("%1 %2")
                                             .arg(height * unitsPerMillimeter,
                                                  0, 'f', 3)
                                             .arg(arcLengthUnitSuffix(settings.lengthUnit));

        QString stageHint;
        if (arcTool_.textInputMode() != ArcTextInputMode::None) {
            stageHint = QStringLiteral("Enter applies value");
        } else if (pendingPoints_.isEmpty()) {
            stageHint = QStringLiteral("Click first endpoint  •  L locks plane");
        } else if (pendingPoints_.size() == 1) {
            stageHint = QStringLiteral("Click chord end  •  D length  •  X/Y/Z axis  •  P perp  •  Alt bypass");
        } else {
            stageHint = QStringLiteral("Click arc height  •  H sagitta  •  P perpendicular plane %1  •  Alt bypass half-circle snap")
                            .arg(arcState().perpendicularPlaneActive
                                     ? QStringLiteral("ON")
                                     : QStringLiteral("OFF"));
            if (arcState().verticalOverrideAxis != 0) {
                stageHint += QStringLiteral("  •  %1 plane")
                                 .arg(arcState().verticalOverrideAxis == Qt::Key_X
                                          ? QStringLiteral("X")
                                          : QStringLiteral("Y"));
            }
        }
        const QString dimensionsLine = pendingPoints_.size() >= 2
                                           ? QStringLiteral("D: %1    H: %2")
                                                 .arg(chordText, heightText)
                                           : QStringLiteral("D: %1")
                                                 .arg(chordText);
        return {dimensionsLine,
                QStringLiteral("%1  •  Esc exits")
                    .arg(stageHint)};
    }

    ArcHudDisplay threePointArcHudDisplay() const
    {
        qreal chordLength = 0.0;
        qreal radius = 0.0;
        if (pendingPoints_.size() == 1 && cursorValid_) {
            const QPointF chord = cursorWorld_ - pendingPoints_[0];
            chordLength = std::hypot(chord.x(), chord.y());
        } else if (pendingPoints_.size() >= 2) {
            const QPointF chord = pendingPoints_[1] - pendingPoints_[0];
            chordLength = std::hypot(chord.x(), chord.y());
            if (cursorValid_) {
                CircularArc2D arc;
                if (makeCircularArcThroughPoint(pendingPoints_[0],
                                                pendingPoints_[1],
                                                cursorWorld_,
                                                &arc)) {
                    radius = arc.radius;
                }
            }
        }

        const DocumentSettings settings = document_.settings();
        const qreal unitsPerMillimeter =
            1.0 / millimetersPerDocumentUnit(settings.lengthUnit);
        const QString chordText = QStringLiteral("%1 %2")
                                      .arg(chordLength * unitsPerMillimeter,
                                           0, 'f', 3)
                                      .arg(arcLengthUnitSuffix(settings.lengthUnit));
        const QString radiusText = radius > 0.0
                                       ? QStringLiteral("%1 %2")
                                             .arg(radius * unitsPerMillimeter,
                                                  0, 'f', 3)
                                             .arg(arcLengthUnitSuffix(settings.lengthUnit))
                                       : QStringLiteral("—");

        QString stageHint;
        if (pendingPoints_.isEmpty()) {
            stageHint = QStringLiteral("Click first point  •  L locks plane");
        } else if (pendingPoints_.size() == 1) {
            stageHint = QStringLiteral("Click arc end  •  X/Y/Z axis  •  Alt bypass");
        } else {
            stageHint = QStringLiteral("Click point on arc  •  P perpendicular plane %1")
                            .arg(arcState().perpendicularPlaneActive
                                     ? QStringLiteral("ON")
                                     : QStringLiteral("OFF"));
            if (arcState().verticalOverrideAxis != 0) {
                stageHint += QStringLiteral("  •  %1 plane")
                                 .arg(arcState().verticalOverrideAxis == Qt::Key_X
                                          ? QStringLiteral("X")
                                          : QStringLiteral("Y"));
            }
        }
        const QString dimensionsLine = pendingPoints_.size() >= 2
                                           ? QStringLiteral("R: %1")
                                                 .arg(radiusText)
                                           : QStringLiteral("Chord: %1")
                                                 .arg(chordText);
        return {dimensionsLine,
                QStringLiteral("%1  •  Esc exits")
                    .arg(stageHint)};
    }

    void drawArcHudPanel(QPainter &painter,
                         const ArcHudDisplay &display,
                         qreal preferredWidth) const
    {
        painter.save();
        const QRectF panel(12.0,
                           std::max<qreal>(12.0, height() - 58.0),
                           std::max<qreal>(1.0,
                                           std::min<qreal>(preferredWidth,
                                                           width() - 24.0)),
                           46.0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(20, 20, 20, 170));
        painter.drawRoundedRect(panel, 4.0, 4.0);
        painter.setFont(QFont(QStringLiteral("Sans"), 9));
        painter.setPen(QColor(225, 225, 225));
        painter.drawText(QPointF(20.0, panel.top() + 17.0),
                         display.dimensionsLine);
        painter.setPen(QColor(170, 170, 170));
        painter.drawText(QPointF(20.0, panel.top() + 35.0),
                         display.instructionsLine);
        painter.restore();
    }

    QImage arcHudTextImage(const ArcHudDisplay &display,
                           qreal devicePixelRatio,
                           qreal panelWidth) const
    {
        const qreal dpr = std::max<qreal>(devicePixelRatio, 1.0);
        const qreal logicalWidth = std::max<qreal>(
            1.0,
            std::min<qreal>(panelWidth, width() - 24.0));
        QImage image(QSize(qRound(logicalWidth * dpr), qRound(46.0 * dpr)),
                     QImage::Format_RGBA8888);
        image.setDevicePixelRatio(dpr);
        image.fill(Qt::transparent);
        QPainter textPainter(&image);
        textPainter.setRenderHint(QPainter::TextAntialiasing, true);
        textPainter.setFont(QFont(QStringLiteral("Sans"), 9));
        textPainter.setPen(QColor(225, 225, 225));
        textPainter.drawText(QPointF(8.0, 17.0), display.dimensionsLine);
        textPainter.setPen(QColor(170, 170, 170));
        textPainter.drawText(QPointF(8.0, 35.0), display.instructionsLine);
        return image;
    }

    void drawArcToolPreview(QPainter &painter,
                            bool drawCurve,
                            const QColor &curveColor)
    {
        viewportOverlay_.toolPreviewRenderer().drawArcPreview(painter,
                                        pendingPoints_,
                                        arcState().mode,
                                        cursorWorld_,
                                        cursorValid_,
                                        arcState().previewSweepAngle,
                                        currentSnap_,
                                        size(),
                                        viewportTransform_.workPlaneFrame(),
                                        arcCompassRotation(),
                                        curveColor,
                                        drawCurve);
        if (arcState().mode == ArcMode::OnePoint) {
            const ArcHudDisplay display = onePointArcHudDisplay();
            drawArcHudPanel(painter, display, 570.0);
        } else if (arcState().mode == ArcMode::TwoPoint) {
            drawArcHudPanel(painter, twoPointArcHudDisplay(), 750.0);
        } else if (arcState().mode == ArcMode::ThreePoint) {
            drawArcHudPanel(painter, threePointArcHudDisplay(), 750.0);
        }
    }

    void drawCircleToolPreview(QPainter &painter, bool drawCurve = true)
    {
        const Layer *activeLayer = document_.layer(document_.activeLayerId());
        const QColor previewColor =
            activeLayer != nullptr && activeLayer->color.isValid()
                ? activeLayer->color
                : QColor(QStringLiteral("#d28b45"));
        const WorkPlaneFrame inputFrame = viewportTransform_.workPlaneFrame();
        const bool previewUsesSeparateFrame =
            toolPreview_.hasShape &&
            isValidWorkPlaneFrame(toolPreview_.shape.workPlaneFrame) &&
            isValidWorkPlaneFrame(inputFrame) &&
            !workPlaneFramesMatch(toolPreview_.shape.workPlaneFrame,
                                  inputFrame);
        if (drawCurve && previewUsesSeparateFrame) {
            drawShape(painter, toolPreview_.shape, true);
        }
        if (drawCurve) {
            viewportOverlay_.toolPreviewRenderer().drawCirclePreview(painter,
                                               activeTool_,
                                               pendingPoints_,
                                               cursorWorld_,
                                               cursorValid_,
                                               currentSnap_,
                                               size(),
                                               inputFrame,
                                               previewColor,
                                               !previewUsesSeparateFrame);
        } else if (currentSnap_.isValid()) {
            drawSnapMarker(painter, currentSnap_);
        }
        drawArcHudPanel(painter,
                        {toolPreview_.hudDimensionsLine,
                         toolPreview_.hudInstructionsLine},
                        750.0);
    }

    void drawCircleTangentToolPreview(QPainter &painter,
                                      bool drawPreviewGeometry = true)
    {
        if (toolPreview_.hasShape && drawPreviewGeometry) {
            drawShape(painter, toolPreview_.shape, true);
        }
    }

    void drawEllipseToolPreview(QPainter &painter, bool drawCurve = true)
    {
        if (drawCurve && toolPreview_.hasShape) {
            drawShape(painter, toolPreview_.shape, true);
        }
        viewportOverlay_.toolPreviewRenderer().drawEllipsePreview(painter,
                                             pendingPoints_,
                                             cursorWorld_,
                                             cursorValid_,
                                             toolPreview_.guides,
                                             currentSnap_,
                                             size());
        drawArcHudPanel(painter,
                        {toolPreview_.hudDimensionsLine,
                         toolPreview_.hudInstructionsLine},
                        750.0);
    }

    void drawRectangleToolPreview(QPainter &painter, bool drawCurve = true)
    {
        if (drawCurve) {
            if (toolPreview_.hasShape) {
                const Layer *layer = document_.layer(document_.activeLayerId());
                const QColor color = layer != nullptr && layer->color.isValid()
                    ? layer->color : QColor(QStringLiteral("#000000"));
                drawShape(painter, toolPreview_.shape, true, false, true, color);
            }
            QVector<QPointF> markers = pendingPoints_;
            if (toolPreview_.hasShape &&
                toolPreview_.shape.geometryType == GeometryType::Rectangle) {
                markers += rectangleVertices(toolPreview_.shape);
            }
            painter.save();
            painter.setRenderHint(QPainter::Antialiasing, true);
            for (const ToolPreviewGuide &guide : toolPreview_.guides) {
                painter.setPen(QPen(guide.color, 1.5));
                painter.drawLine(worldToScreen(guide.line.p1()),worldToScreen(guide.line.p2()));
            }
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(QStringLiteral("#101010")));
            for (const QPointF &point : markers) {
                painter.drawEllipse(worldToScreen(point), 2.5, 2.5);
            }
            if (cursorValid_) painter.drawEllipse(worldToScreen(cursorWorld_), 2.5, 2.5);
            painter.restore();
        }
        if (currentSnap_.isValid()) drawSnapMarker(painter, currentSnap_);
        drawArcHudPanel(painter,
                        {toolPreview_.hudDimensionsLine,
                         toolPreview_.hudInstructionsLine},
                        750.0);
    }

    void drawPolygonToolPreview(QPainter &painter, bool drawCurve = true)
    {
        if (drawCurve && toolPreview_.hasShape) {
            const Layer *activeLayer = document_.layer(document_.activeLayerId());
            const QColor previewColor =
                activeLayer != nullptr && activeLayer->color.isValid()
                    ? activeLayer->color
                    : QColor(QStringLiteral("#d28b45"));
            drawShape(painter, toolPreview_.shape, true, false, true,
                      previewColor);
        }
        viewportOverlay_.toolPreviewRenderer().drawPolygonPreview(painter,
                                            activeTool_,
                                            polygonSideCount_,
                                            pendingPoints_,
                                            cursorWorld_,
                                            cursorValid_,
                                            currentSnap_,
                                            size(),
                                            false);
        drawArcHudPanel(painter,
                        {toolPreview_.hudDimensionsLine,
                         toolPreview_.hudInstructionsLine},
                        750.0);
    }

    void drawControlPoints(QPainter &painter,
                           const ViewportRenderObject &renderObject,
                           bool drawMarkers = true)
    {
        Shape placedShape = renderObject.shape;
        const Point3D &offset = renderObject.placementTranslation;
        const auto addPlacement = [&offset](WorkPlaneFrame frame) {
            frame.origin.x += offset.x;
            frame.origin.y += offset.y;
            frame.origin.z += offset.z;
            return frame;
        };
        placedShape.workPlaneFrame =
            addPlacement(shapeWorkPlaneFrame(renderObject.shape));
        if (placedShape.geometryType == GeometryType::PolyCurve) {
            placedShape.componentWorkPlaneFrames.resize(
                placedShape.components.size());
            for (int componentIndex = 0;
                 componentIndex < placedShape.components.size();
                 ++componentIndex) {
                placedShape.componentWorkPlaneFrames[componentIndex] =
                    addPlacement(shapeComponentWorkPlaneFrame(
                        renderObject.shape, componentIndex));
            }
        }

        const WorkPlaneFrame previousFrame = viewportTransform_.workPlaneFrame();
        viewportTransform_.setWorkPlaneFrame(placedShape.workPlaneFrame);
        viewportOverlay_.drawControlPoints(painter,
                                           placedShape,
                                           size(),
                                           renderObject.objectId,
                                           selectedShapeIndex_,
                                           controlPointSelectionDragActive(),
                                           controlPointIndex_,
                                           drawMarkers);
        viewportTransform_.setWorkPlaneFrame(previousFrame);
    }

    void drawGrid(QPainter &painter)
    {
        viewportRenderer_.drawGrid(painter, size());
    }

    void drawOrigin(QPainter &painter)
    {
        viewportRenderer_.drawOrigin(painter, size());
    }

    QVector<double> expandedKnotVector(const Shape::NurbsCurve2D &curve) const
    {
        return expandedNurbsKnotVector(curve);
    }

    bool isValidNurbsCurve(const Shape::NurbsCurve2D &curve) const
    {
        return validateNurbsCurve(curve);
    }

    bool evaluateNurbsPoint(const Shape::NurbsCurve2D &curve,
                            qreal parameter,
                            QPointF *point) const
    {
        return classiCAD::evaluateNurbsPoint(curve, parameter, point);
    }

    void drawSubdivisionPoints(QPainter &painter,
                               const Shape &shape,
                               const QVector<double> &parameters,
                               bool preview)
    {
        const WorkPlaneFrame previousFrame = viewportTransform_.workPlaneFrame();
        viewportTransform_.setWorkPlaneFrame(shapeWorkPlaneFrame(shape));
        viewportOverlay_.drawSubdivisionPoints(painter,
                                               shape,
                                               parameters,
                                               size(),
                                               preview);
        viewportTransform_.setWorkPlaneFrame(previousFrame);
    }

    void drawPointToolPreview(QPainter &painter, bool drawPoint = true)
    {
        viewportOverlay_.toolPreviewRenderer().drawPointPreview(painter,
                                          cursorWorld_,
                                          cursorValid_,
                                          currentSnap_,
                                          size(),
                                          drawPoint);
    }

    void drawPointConstructionToolPreview(QPainter &painter,
                                          bool drawPreviewGeometry = true)
    {
        if (drawPreviewGeometry) {
            const QColor arcColor(QStringLiteral("#33cc33"));
            for (const Shape &preview : toolPreview_.shapes) {
                if (preview.geometryType == GeometryType::Point) continue;
                const bool isArc = preview.geometryType == GeometryType::Arc ||
                                   preview.geometryType == GeometryType::Circle;
                const bool pointByArcsArc = activeTool_ == Tool::PointByArcs &&
                                            isArc;
                drawShape(painter, preview, true, false, true,
                          pointByArcsArc ? arcColor : QColor(Qt::black));
            }
            for (const ToolPreviewGuide &guide : toolPreview_.guides) {
                Shape line;
                line.geometryType = GeometryType::Line;
                line.points = {guide.line.p1(), guide.line.p2()};
                line.workPlane = WorkPlane::XY;
                line.workPlaneOffset = guide.hasWorkPlaneFrame
                    ? guide.workPlaneFrame.origin.z
                    : toolPreview_.workPlaneFrame.origin.z;
                line.workPlaneFrame = guide.hasWorkPlaneFrame
                    ? guide.workPlaneFrame : toolPreview_.workPlaneFrame;
                drawShape(painter, line, true, false, true, guide.color);
            }
        }

        const QColor pointColor(Qt::black);
        const auto drawPointCross = [&](const Point3D &worldPoint,
                                        const WorkPlaneFrame &frame,
                                        qreal markerSize) {
            if (!isValidWorkPlaneFrame(frame)) return;
            QPointF centerScreen;
            if (!viewportTransform_.worldPointToScreen(worldPoint, size(),
                                                       &centerScreen)) {
                return;
            }
            const Point3D axes[] = {{1.0, 0.0, 0.0},
                                    {0.0, 1.0, 0.0},
                                    {0.0, 0.0, 1.0}};
            painter.save();
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(pointColor, 2.0, Qt::SolidLine,
                                Qt::RoundCap, Qt::RoundJoin));
            for (const Point3D &axis : axes) {
                QPointF axisScreens[2];
                const Point3D samples[] = {
                    {worldPoint.x - axis.x, worldPoint.y - axis.y,
                     worldPoint.z - axis.z},
                    {worldPoint.x + axis.x, worldPoint.y + axis.y,
                     worldPoint.z + axis.z}};
                const bool visible[2] = {
                    viewportTransform_.worldPointToScreen(samples[0], size(),
                                                          &axisScreens[0]),
                    viewportTransform_.worldPointToScreen(samples[1], size(),
                                                          &axisScreens[1])};
                QPointF projectedDirection;
                if (visible[0] && visible[1]) {
                    projectedDirection = axisScreens[1] - axisScreens[0];
                } else if (visible[0]) {
                    projectedDirection = centerScreen - axisScreens[0];
                } else if (visible[1]) {
                    projectedDirection = axisScreens[1] - centerScreen;
                }
                const qreal projectedLength = std::hypot(projectedDirection.x(),
                                                          projectedDirection.y());
                if (projectedLength <= 1.0e-4) continue;
                projectedDirection /= projectedLength;
                const qreal halfSize = markerSize * 0.5;
                painter.drawLine(centerScreen - projectedDirection * halfSize,
                                 centerScreen + projectedDirection * halfSize);
            }
            painter.restore();
        };
        for (const Shape &preview : toolPreview_.shapes) {
            if (preview.geometryType != GeometryType::Point ||
                preview.points.isEmpty()) {
                continue;
            }
            const WorkPlaneFrame frame = shapeWorkPlaneFrame(preview);
            const qreal markerSize = activeTool_ == Tool::PointByArcs &&
                                             toolPreview_.activeStage == 5
                                         ? 3.0 : 5.0;
            drawPointCross(workPlaneFramePointToWorld(preview.points.first(), frame),
                           frame, markerSize);
        }
        if (currentSnap_.isValid()) {
            drawSnapMarker(painter, currentSnap_);
        }
    }

    void drawRotateToolPreview(QPainter &painter, bool drawGeometry = true)
    {
        viewportOverlay_.toolPreviewRenderer().drawRotatePreview(painter,
                                           cursorWorld_,
                                           cursorValid_,
                                           rotateState().stage,
                                           isValidWorkPlaneFrame(rotateState().frame)
                                               ? rotateState().frame
                                               : viewportTransform_.workPlaneFrame(),
                                           rotateState().basePoint,
                                           rotateState().referencePoint,
                                           rotateState().previewAngle,
                                           rotateState().angleInput,
                                           rotateState().angleInputActive,
                                           rotateState().angleSnapEnabled,
                                           rotateSnapIncrementDegrees(),
                                           rotateToolPreferences_.useRadians,
                                           currentSnap_,
                                           size(),
                                           drawGeometry);
    }

    void drawScaleToolGuide(QPainter &painter, bool drawGuide = true)
    {
        if (!drawGuide || !cursorValid_ || scaleState().stage != 2) {
            return;
        }

        QPointF guideEnd = cursorWorld_;
        if (scaleState().mode == ScaleMode::OneD && !scaleState().usingTypedFactor) {
            const QPointF offset = cursorWorld_ - scaleState().basePoint;
            guideEnd = scaleState().basePoint +
                       scaleState().axisDirection *
                           QPointF::dotProduct(offset, scaleState().axisDirection);
        }

        painter.save();
        painter.setPen(QPen(QColor(QStringLiteral("#8aa7c7")), 1.0, Qt::DashLine));
        painter.drawLine(worldToScreen(scaleState().basePoint), worldToScreen(guideEnd));
        painter.setPen(QPen(QColor(QStringLiteral("#e6b85c")), 1.5));
        painter.setBrush(QColor(QStringLiteral("#282828")));
        painter.drawEllipse(worldToScreen(scaleState().basePoint), 4.0, 4.0);
        painter.restore();
    }

    void drawEraseCandidatePreview(QPainter &painter, int shapeIndex)
    {
        eraseOverlayRenderer_.drawCandidatePreview(painter,
                                                  eraseTargetCurveCaches_,
                                                  shapeIndex,
                                                  !eraseLikeScreenPath().isEmpty());
    }

    void drawErasePreview(QPainter &painter)
    {
        eraseOverlayRenderer_.drawCursorPreview(painter,
                                               activeTool_,
                                               eraseTool_.cursorScreenPosition(),
                                               cursorValid_,
                                               eraseTool_.cursorPressed(),
                                               eraseCandidates().size());
    }

    void drawShape(QPainter &painter,
                   const Shape &shape,
                   bool preview,
                   bool selected = false,
                   bool drawPreviewPoints = true,
                   const QColor &layerColor = QColor(),
                   const QString &layerLineType = QString(),
                   qreal layerLineWeightMm = 0.0,
                   ObjectId shapeObjectId = ObjectId::invalid(),
                   quint64 geometryRevision = 0,
                   const ViewportDepthGeometry *preparedGeometry = nullptr,
                   const Point3D &preparedOffset = {})
    {
        const WorkPlaneFrame previousFrame = viewportTransform_.workPlaneFrame();
        viewportTransform_.setWorkPlaneFrame(shapeWorkPlaneFrame(shape));
        viewportRenderer_.drawShape(painter,
                                    shape,
                                    size(),
                                    preview,
                                    selected,
                                    drawPreviewPoints,
                                    layerColor,
                                    layerLineType,
                                    layerLineWeightMm,
                                    shapeObjectId,
                                    geometryRevision,
                                    preparedGeometry, preparedOffset);
        viewportTransform_.setWorkPlaneFrame(previousFrame);
    }

    void emitCoordinateUpdate()
    {
        if (coordinateUpdate_) {
            coordinateUpdate_(coordinateText());
        }
    }

    void notifyViewStateChanged()
    {
        if (viewStateUpdate_) {
            viewStateUpdate_(viewportTransform_.workPlane(),
                             viewportTransform_.workPlaneOffset(),
                             viewportTransform_.viewPreset());
        }
    }

private:
    Shape pictureShapeForCorner(const QPointF &cursorCorner) const
    {
        Shape picture;
        picture.geometryType = GeometryType::Picture;
        picture.workPlane = viewportTransform_.workPlane();
        picture.workPlaneOffset = viewportTransform_.workPlaneOffset();
        picture.workPlaneFrame = viewportTransform_.workPlaneFrame();
        if (pendingPictureImage_.isNull() || pendingPoints_.isEmpty()) {
            return picture;
        }
        const qreal aspectRatio = static_cast<qreal>(pendingPictureImage_.width()) /
                                  pendingPictureImage_.height();
        picture.points = makePictureFramePoints(pendingPoints_.first(),
                                                cursorCorner,
                                                aspectRatio);
        picture.pictureImage = pendingPictureImage_;
        picture.pictureImageData = pendingPictureImageData_;
        return picture;
    }

    void cancelPicturePlacement()
    {
        if (activeTool_ != Tool::Picture) {
            return;
        }
        pendingPoints_.clear();
        setTool(Tool::Select);
        if (commandFinished_) {
            commandFinished_(Tool::Select);
        }
        update();
    }

    ToolInput makeKeyToolInput(const QKeyEvent &event) const
    {
        const WorkPlaneFrame frame = viewportTransform_.workPlaneFrame();
        QPointF screenPosition = mapFromGlobal(QCursor::pos());
        if (!rect().contains(screenPosition.toPoint())) {
            screenPosition = lastMousePosition_;
        }
        QPointF rawWorldPosition;
        if (!viewportTransform_.screenToWorkPlane(screenPosition,
                                                  size(),
                                                  frame,
                                                  &rawWorldPosition)) {
            rawWorldPosition = rawCursorWorld_;
        }
        return ToolInputTranslator::fromKeyEvent(event, screenPosition,
                                                 rawWorldPosition, frame, size());
    }

    bool makeToolShape(ToolId tool,
                       const QVector<QPointF> &points,
                       ArcMode arcMode,
                       qreal arcSweep,
                       Shape *shape) const
    {
        if (shape == nullptr || points.size() < requiredPoints(tool)) {
            return false;
        }

        Shape result;
        result.geometryType = geometryTypeForTool(tool);
        result.points = points;
        result.arcMode = arcMode;
        result.arcSweep = arcSweep;
        result.workPlane = viewportTransform_.workPlane();
        result.workPlaneOffset = viewportTransform_.workPlaneOffset();
        result.workPlaneFrame = viewportTransform_.workPlaneFrame();
        if (isRectangleTool(tool)) {
            result.points = makeRectanglePoints(rectangleModeForTool(tool), points);
            if (result.points.size() != 4) {
                return false;
            }
            QVector<QPointF> closedPoints = result.points;
            closedPoints.append(closedPoints.first());
            result.nurbs = makeDegreeOneNurbs(closedPoints);
        } else if (isPolygonTool(tool)) {
            result.points = makeRegularPolygonPoints(
                polygonModeForTool(tool), points, polygonSideCount_);
            if (result.points.size() < 3) {
                return false;
            }
            QVector<QPointF> closedPoints = result.points;
            closedPoints.append(closedPoints.first());
            result.nurbs = makeDegreeOneNurbs(closedPoints);
        } else if (isCircleConstructionTool(tool)) {
            if (tool == ToolId::CircleDiameter) {
                if (!makeCircleDefinitionFromDiameter(points[0],
                                                      points[1],
                                                      &result.points)) {
                    return false;
                }
            } else if (tool == ToolId::CircleThreePoint) {
                if (!makeCircleDefinitionFromThreePoints(points[0],
                                                         points[1],
                                                         points[2],
                                                         &result.points)) {
                    return false;
                }
            }
        }

        if (tool == Tool::Line) {
            result.nurbs = makeDegreeOneNurbs(result.points);
        } else if (tool == Tool::Arc) {
            result.nurbs = makeArcNurbsCurve(result);
            if (!isValidNurbsCurve(result.nurbs)) {
                return false;
            }
        } else if (tool == Tool::Bezier || tool == Tool::Nurbs) {
            result.nurbs = makeBezierNurbs(result.points);
        } else if (isCircleConstructionTool(tool)) {
            result.nurbs = makeCircleNurbs(result.points);
            if (!isValidNurbsCurve(result.nurbs)) {
                return false;
            }
        } else if (isEllipseTool(tool)) {
            result.nurbs = makeEllipseNurbs(ellipseModeForTool(tool), points);
            if (!isValidNurbsCurve(result.nurbs)) {
                return false;
            }
            const EllipseMode mode = ellipseModeForTool(tool);
            const QPointF center = mode == EllipseMode::CenterAxisRadius
                                       ? points[0]
                                       : (points[0] + points[1]) * 0.5;
            result.points = {center};
        }

        *shape = result;
        return true;
    }

    static constexpr int maxSubdivisionSections = 10000;
    ToolId activeTool_ = ToolId::Select;
    bool rightButtonSelectionGestureActive_ = false;
    bool controlPointsVisible_ = false;
    bool smoothWiresOverlay_ = true;
    bool smoothWiresEditMode_ = true;
    ComponentSelectionMode componentSelectionMode_ = ComponentSelectionMode::Vertex;
    bool componentBoxSelectionActive_ = false;
    bool componentBoxStartedOnBlank_ = false;
    ObjectId componentBoxSelectionObject_ = ObjectId::invalid();
    ComponentPickCycleState componentPickCycleStates_[3];
    QSet<int> componentSelections_[3];
    ObjectId componentSelectionObject_ = ObjectId::invalid();
    int activeComponentIndices_[3] = {-1, -1, -1};
    std::unique_ptr<ApplicationSession> ownedSession_;
    ApplicationSession &session_;
    Document &document_;
    SelectionModel &selection_;
    History &history_;
    ViewportTransform viewportTransform_;
    CurveSampler curveSampler_;
    SurfaceTessellationCache surfaceTessellationCache_;
    ViewportGeometryCache viewportGeometryCache_;
    CurveHitTester curveHitTester_;
    SnapEngine snapEngine_;
    ViewportRenderer viewportRenderer_;
    ViewportShadingSettings viewportShadingSettings_;
    QFrame *shadingPopover_ = nullptr;
    QButtonGroup *lightingModeGroup_ = nullptr;
    std::array<QToolButton *, 3> lightingModeButtons_{};
    QToolButton *studioLightPreviewButton_ = nullptr;
    QToolButton *worldSpaceLightingButton_ = nullptr;
    QSlider *studioLightRotationSlider_ = nullptr;
    QLabel *studioLightRotationLabel_ = nullptr;
    QButtonGroup *wireColorGroup_ = nullptr;
    QButtonGroup *solidColorGroup_ = nullptr;
    QButtonGroup *backgroundGroup_ = nullptr;
    QVector<QToolButton *> wireColorButtons_;
    QVector<QToolButton *> solidColorButtons_;
    QVector<QToolButton *> backgroundButtons_;
    QCheckBox *backfaceCullingCheck_ = nullptr;
    QCheckBox *outline_ = nullptr;
    QToolButton *outlineColorButton_ = nullptr;
    QCheckBox *specularLightingCheck_ = nullptr;
    QCheckBox *xrayCheck_ = nullptr;
    QSlider *xrayAlphaSlider_ = nullptr;
    QLabel *xrayAlphaLabel_ = nullptr;
    QCheckBox *shadowsCheck_ = nullptr;
    QToolButton *shadowSettingsButton_ = nullptr;
    QSlider *shadowIntensitySlider_ = nullptr;
    QLabel *shadowIntensityLabel_ = nullptr;
    QCheckBox *depthOfFieldCheck_ = nullptr;
    QCheckBox *cavityCheck_ = nullptr;
    int componentModeHover_ = -1;
    int shadingControlHover_ = -1;
    int shadingControlPressed_ = -1;
    BlenderGridRenderer blenderGridRenderer_;
    ViewportGpuSurface *gpuSurface_ = nullptr;
    ViewportOverlay viewportOverlay_;
    ViewportEraseOverlayRenderer eraseOverlayRenderer_;
    ViewportHudRenderer viewportHudRenderer_;
    ViewportNavigationGizmo navigationGizmo_;
    NavigationController navigationController_;
    GrabTool grabTool_;
    DuplicateTool duplicateTool_;
    BlenderGridAppearance gridAppearance_;
    ToolContext toolContext_;
    ToolRegistry &toolRegistry_;
    ArcTool &arcTool_;
    RotateTool &rotateTool_;
    MirrorTool &mirrorTool_;
    ScaleTool &scaleTool_;
    TrimTool &trimTool_;
    EraseTool &eraseTool_;
    JoinTool joinTool_;
    SubdivisionTool subdivisionTool_;
    CommandRouter commandRouter_;
    InteractionTool *activeToolController_ = nullptr;
    ToolStatus toolStatus_;
    ToolPreview toolPreview_;
    WorkPlaneFrame toolDrawingFrame_;
    bool toolDrawingPlaneLocked_ = false;
    // Temporary source-compatibility view. Document owns the storage and
    // identity; the alias will disappear once viewport responsibilities are
    // extracted into tools and services.
    Document &shapes_;
    QVector<QPointF> pendingPoints_;
    QImage pendingPictureImage_;
    QByteArray pendingPictureImageData_;
    QString pendingPicturePath_;
    int polygonSideCount_ = 32;
    QPointF &pan_;
    QPointF lastWorldPosition_{0.0, 0.0};
    QPointF rawCursorWorld_{0.0, 0.0};
    QPoint lastMousePosition_;
    QPointF cursorWorld_{0.0, 0.0};
    SnapResult currentSnap_;
    DragSnapResult currentDragSnap_;
    // Legacy member names remain during this incremental migration, but the
    // values are references into SelectionModel, and therefore stable object
    // IDs, never container indexes. These aliases keep the current viewport
    // implementation source-compatible while selection behavior migrates.
    const QVector<ObjectId> &selectedShapeIndices_;
    const ObjectId &selectedShapeIndex_;
    const int &controlPointIndex_;
    bool dragHistoryRecorded_ = false;
    bool dragSnapLocked_ = false;
    quint64 dragSnapTraceSequence_ = 0;
    DragAxisLock dragAxisLock_ = DragAxisLock::None;
    QPointF grabAxisStartScreen_;
    WorkPlaneFrame grabViewPlaneFrame_;
    Point3D grabViewPlaneStartWorld_;
    bool grabViewPlaneAnchorValid_ = false;
    WorkPlaneFrame selectionDragViewPlaneFrame_;
    Point3D selectionDragLastViewPlaneWorld_;
    Point3D selectionDragCurrentViewPlaneWorld_;
    QPointF selectionDragLastScreenPosition_;
    bool selectionDragViewPlaneAnchorValid_ = false;
    Point3D dragSnapViewPlaneWorld_;
    QPointF selectionDragSnapScreen_;
    bool dragSnapViewPlaneAnchorValid_ = false;
    Point3D dragAxisLastPosition_;
    bool dragAxisPositionValid_ = false;
    WorkPlaneFrame controlPointDragFrame_;
    bool controlPointDragFrameValid_ = false;
    QPointF controlPointLastCursorScreen_;
    QPointF controlPointCursorOffsetScreen_;
    QPointF dragSnapCursorWorld_{0.0, 0.0};
    QPointF dragSnapCursorScreen_;
    QPointF nearDragFreeSourcePoint_{0.0, 0.0};
    bool nearDragFreeSourcePointValid_ = false;
    QVector<ObjectId> eraseTargetShapeIndices_;
    QVector<EraseCurveSampleCache> eraseSceneCurveCaches_;
    QVector<EraseCurveSampleCache> eraseTargetCurveCaches_;
    bool eraseGeometryCachePrepared_ = false;
    ViewportCameraState eraseCacheCameraState_;
    ViewportCameraPreferences eraseCacheCameraPreferences_;
    QSize eraseCacheViewportSize_;
    QElapsedTimer trimHoverTimingWindow_;
    qint64 trimHoverWindowTotalUs_ = 0;
    qint64 trimHoverWindowMaxUs_ = 0;
    int trimHoverWindowEvents_ = 0;
    qreal &zoom_;
    bool lineCommandActive_ = false;
    bool cursorValid_ = false;
    bool orthoEnabled_ = false;
    bool osnapEnabled_ = false;
    bool endpointSnapEnabled_ = true;
    bool midpointSnapEnabled_ = true;
    bool intersectionSnapEnabled_ = true;
    bool centerSnapEnabled_ = true;
    bool perpendicularSnapEnabled_ = false;
    bool tangentSnapEnabled_ = false;
    bool nearSnapEnabled_ = false;
    bool controlPointSnapEnabled_ = false;
    RotateToolPreferences rotateToolPreferences_;
};

ViewportWidgetApi *createViewportWidget(QWidget *parent)
{
    return new ViewportWidget(nullptr, parent);
}

ViewportWidgetApi *createViewportWidget(ApplicationSession &session,
                                        QWidget *parent)
{
    return new ViewportWidget(&session, parent);
}

} // namespace classiCAD
