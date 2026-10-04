#include "viewport_widget_api.h"
#include "../core/document/document.h"
#include "../core/document/document_settings.h"
#include "../core/document/selection_model.h"
#include "../core/debug_log.h"
#include "../core/geometry/circle_construction.h"
#include "../core/geometry/arc_curve_factory.h"
#include "../core/geometry/curve_evaluator.h"
#include "../core/geometry/geometry_transform.h"
#include "../core/geometry/work_plane.h"
#include "../core/history/history.h"
#include "../core/serialization/document_serializer.h"
#include "../core/serialization/blender_project_file.h"
#include "../core/serialization/rhino3dm_interchange.h"
#include "../services/hit_testing/curve_hit_tester.h"
#include "../services/dimensions/dimension_association.h"
#include "../services/sampling/curve_sampler.h"
#include "../services/snapping/snap_engine.h"
#include "../services/viewport/viewport_transform.h"
#include "../tools/tool_context.h"
#include "../tools/tool_input.h"
#include "../tools/tool_registry.h"
#include "input_helpers.h"
#include "viewport/blender_grid_renderer.h"
#include "viewport/viewport_control_point_renderer.h"
#include "viewport/viewport_gpu_surface.h"
#include "viewport/line_type_style.h"
#include "viewport/viewport_scene_renderer.h"
#include "viewport/viewport_overlay.h"
#include "viewport/viewport_renderer.h"

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QCursor>
#include <QDateTime>
#include <QDebug>
#include <QEasingCurve>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineF>
#include <QImageReader>
#include <QKeyEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QPixmap>
#include <QRadialGradient>
#include <QSettings>
#include <QStringList>
#include <QTextStream>
#include <QTimer>
#include <QToolTip>
#include <QVariantAnimation>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <functional>

namespace classiCAD {

namespace {

constexpr qreal kDragSnapBreakawayPixels = 36.0;

void fillViewportBackground(QPainter &painter, const QRect &bounds)
{
    if (bounds.isEmpty()) {
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

ViewportSceneLineStyle viewportSceneLineStyleForLayerPattern(
    LayerGpuLinePatternKind kind)
{
    switch (kind) {
    case LayerGpuLinePatternKind::Dashed:
        return ViewportSceneLineStyle::Dashed;
    case LayerGpuLinePatternKind::Dotted:
        return ViewportSceneLineStyle::Dotted;
    case LayerGpuLinePatternKind::Pattern:
        return ViewportSceneLineStyle::Pattern;
    case LayerGpuLinePatternKind::Solid:
    case LayerGpuLinePatternKind::Unsupported:
    default:
        return ViewportSceneLineStyle::Solid;
    }
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

Point3D arcAxisDirection(int key)
{
    switch (key) {
    case Qt::Key_X:
        return {1.0, 0.0, 0.0};
    case Qt::Key_Y:
        return {0.0, 1.0, 0.0};
    case Qt::Key_Z:
        return {0.0, 0.0, 1.0};
    default:
        return {};
    }
}

qreal arcVectorDot(const Point3D &first, const Point3D &second)
{
    return first.x * second.x + first.y * second.y + first.z * second.z;
}

qreal arcVectorLength(const Point3D &vector)
{
    return std::sqrt(arcVectorDot(vector, vector));
}

QJsonObject point3DToJson(const Point3D &point)
{
    return {{QStringLiteral("x"), point.x},
            {QStringLiteral("y"), point.y},
            {QStringLiteral("z"), point.z}};
}

bool point3DFromJson(const QJsonValue &value, Point3D *point)
{
    if (point == nullptr || !value.isObject()) {
        return false;
    }

    const QJsonObject object = value.toObject();
    const QJsonValue xValue = object.value(QStringLiteral("x"));
    const QJsonValue yValue = object.value(QStringLiteral("y"));
    const QJsonValue zValue = object.value(QStringLiteral("z"));
    if (!xValue.isDouble() || !yValue.isDouble() || !zValue.isDouble()) {
        return false;
    }

    const Point3D restored{xValue.toDouble(), yValue.toDouble(), zValue.toDouble()};
    if (!std::isfinite(restored.x) || !std::isfinite(restored.y) ||
        !std::isfinite(restored.z)) {
        return false;
    }

    *point = restored;
    return true;
}

QJsonObject workPlaneFrameToJson(const WorkPlaneFrame &frame)
{
    return {{QStringLiteral("origin"), point3DToJson(frame.origin)},
            {QStringLiteral("xAxis"), point3DToJson(frame.xAxis)},
            {QStringLiteral("yAxis"), point3DToJson(frame.yAxis)},
            {QStringLiteral("normal"), point3DToJson(frame.normal)},
            {QStringLiteral("valid"), frame.valid}};
}

bool workPlaneFrameFromJson(const QJsonValue &value, WorkPlaneFrame *frame)
{
    if (frame == nullptr || !value.isObject()) {
        return false;
    }

    const QJsonObject object = value.toObject();
    WorkPlaneFrame restored;
    if (!point3DFromJson(object.value(QStringLiteral("origin")), &restored.origin) ||
        !point3DFromJson(object.value(QStringLiteral("xAxis")), &restored.xAxis) ||
        !point3DFromJson(object.value(QStringLiteral("yAxis")), &restored.yAxis) ||
        !point3DFromJson(object.value(QStringLiteral("normal")), &restored.normal) ||
        !object.value(QStringLiteral("valid")).toBool(false)) {
        return false;
    }
    restored.valid = true;
    if (!isValidWorkPlaneFrame(restored)) {
        return false;
    }

    *frame = restored;
    return true;
}

QJsonObject viewportCameraStateToJson(const ViewportCameraState &state)
{
    QJsonObject orientation;
    orientation.insert(QStringLiteral("w"), state.orientation.w);
    orientation.insert(QStringLiteral("x"), state.orientation.x);
    orientation.insert(QStringLiteral("y"), state.orientation.y);
    orientation.insert(QStringLiteral("z"), state.orientation.z);

    QJsonObject object;
    object.insert(QStringLiteral("zoom"), state.zoom);
    object.insert(QStringLiteral("pan"), pointToJson(state.pan));
    object.insert(QStringLiteral("orbitPivot"), point3DToJson(state.orbitPivot));
    object.insert(QStringLiteral("yawRadians"), state.yawRadians);
    object.insert(QStringLiteral("pitchRadians"), state.pitchRadians);
    object.insert(QStringLiteral("perspective"), state.perspective);
    object.insert(QStringLiteral("preset"), static_cast<int>(state.preset));
    object.insert(QStringLiteral("gridViewDistance"), state.gridViewDistance);
    object.insert(QStringLiteral("orientation"), orientation);
    object.insert(QStringLiteral("hasOrientation"), state.hasOrientation);
    return object;
}

bool viewportCameraStateFromJson(const QJsonValue &value,
                                 ViewportCameraState *state)
{
    if (state == nullptr || !value.isObject()) {
        return false;
    }

    const QJsonObject object = value.toObject();
    const auto finiteNumber = [&object](const QString &key, qreal *number) {
        const QJsonValue value = object.value(key);
        if (number == nullptr || !value.isDouble()) {
            return false;
        }
        const qreal restored = value.toDouble();
        if (!std::isfinite(restored)) {
            return false;
        }
        *number = restored;
        return true;
    };

    ViewportCameraState restored;
    int preset = -1;
    if (!finiteNumber(QStringLiteral("zoom"), &restored.zoom) ||
        restored.zoom <= 0.0 ||
        !pointFromJson(object.value(QStringLiteral("pan")), &restored.pan) ||
        !point3DFromJson(object.value(QStringLiteral("orbitPivot")),
                         &restored.orbitPivot) ||
        !finiteNumber(QStringLiteral("yawRadians"), &restored.yawRadians) ||
        !finiteNumber(QStringLiteral("pitchRadians"), &restored.pitchRadians) ||
        !finiteNumber(QStringLiteral("gridViewDistance"),
                      &restored.gridViewDistance) ||
        restored.gridViewDistance <= 0.0 ||
        !object.value(QStringLiteral("preset")).isDouble() ||
        !object.value(QStringLiteral("perspective")).isBool() ||
        !object.value(QStringLiteral("hasOrientation")).isBool()) {
        return false;
    }
    preset = object.value(QStringLiteral("preset")).toInt(-1);
    if (preset < static_cast<int>(ViewportViewPreset::Top) ||
        preset > static_cast<int>(ViewportViewPreset::Left)) {
        return false;
    }
    restored.preset = static_cast<ViewportViewPreset>(preset);
    restored.perspective = object.value(QStringLiteral("perspective")).toBool();
    restored.hasOrientation = object.value(QStringLiteral("hasOrientation")).toBool();

    const QJsonObject orientation = object.value(QStringLiteral("orientation")).toObject();
    const QJsonValue orientationW = orientation.value(QStringLiteral("w"));
    const QJsonValue orientationX = orientation.value(QStringLiteral("x"));
    const QJsonValue orientationY = orientation.value(QStringLiteral("y"));
    const QJsonValue orientationZ = orientation.value(QStringLiteral("z"));
    if (!orientationW.isDouble() || !orientationX.isDouble() ||
        !orientationY.isDouble() || !orientationZ.isDouble()) {
        return false;
    }
    restored.orientation = {orientationW.toDouble(),
                            orientationX.toDouble(),
                            orientationY.toDouble(),
                            orientationZ.toDouble()};
    const qreal orientationLengthSquared = restored.orientation.dot(restored.orientation);
    if (!std::isfinite(orientationLengthSquared) ||
        (restored.hasOrientation && orientationLengthSquared <= 1.0e-12)) {
        return false;
    }

    *state = restored;
    return true;
}

Point3D arcVectorAdd(const Point3D &first, const Point3D &second)
{
    return {first.x + second.x, first.y + second.y, first.z + second.z};
}

Point3D arcVectorSubtract(const Point3D &first, const Point3D &second)
{
    return {first.x - second.x, first.y - second.y, first.z - second.z};
}

Point3D arcVectorScale(const Point3D &vector, qreal scale)
{
    return {vector.x * scale, vector.y * scale, vector.z * scale};
}

Point3D arcVectorCross(const Point3D &first, const Point3D &second)
{
    return {first.y * second.z - first.z * second.y,
            first.z * second.x - first.x * second.z,
            first.x * second.y - first.y * second.x};
}

Point3D rotateWorldVector(const Point3D &vector,
                          const Point3D &axis,
                          qreal angle)
{
    const qreal axisLength = arcVectorLength(axis);
    if (axisLength <= 1.0e-12 || std::abs(angle) <= 1.0e-15) {
        return vector;
    }
    const Point3D unitAxis = arcVectorScale(axis, 1.0 / axisLength);
    const qreal cosine = std::cos(angle);
    const qreal sine = std::sin(angle);
    return arcVectorAdd(
        arcVectorAdd(arcVectorScale(vector, cosine),
                     arcVectorScale(arcVectorCross(unitAxis, vector), sine)),
        arcVectorScale(unitAxis,
                       arcVectorDot(unitAxis, vector) * (1.0 - cosine)));
}

Point3D rotateWorldPoint(const Point3D &point,
                         const Point3D &pivot,
                         const Point3D &axis,
                         qreal angle)
{
    return arcVectorAdd(pivot,
                        rotateWorldVector(arcVectorSubtract(point, pivot),
                                          axis,
                                          angle));
}

WorkPlaneFrame rotateWorkPlaneFrame(const WorkPlaneFrame &frame,
                                    const Point3D &pivot,
                                    const Point3D &axis,
                                    qreal angle)
{
    WorkPlaneFrame result = frame;
    result.origin = rotateWorldPoint(frame.origin, pivot, axis, angle);
    result.xAxis = rotateWorldVector(frame.xAxis, axis, angle);
    result.yAxis = rotateWorldVector(frame.yAxis, axis, angle);
    result.normal = rotateWorldVector(frame.normal, axis, angle);
    result.valid = isValidWorkPlaneFrame(result);
    return result;
}

qreal snapOnePointArcAngle(qreal angle)
{
    constexpr qreal pi = 3.14159265358979323846;
    constexpr qreal twoPi = 2.0 * pi;
    constexpr qreal angleIncrement = pi / 12.0;
    constexpr qreal snapTolerance = 6.0 * pi / 180.0;
    const qreal nearest = std::round(angle / angleIncrement) * angleIncrement;
    return std::abs(std::remainder(angle - nearest, twoPi)) <= snapTolerance
               ? nearest
               : angle;
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

} // namespace

struct TransientPreviewStroke {
    Shape shape;
    QColor color;
    float width = 1.5f;
    bool controlGuide = false;
    float pointDiameter = 0.0f;
    bool dashed = false;
    bool pointOutline = false;
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
};

QString dragAxisLockName(DragAxisLock lock)
{
    switch (lock) {
    case DragAxisLock::X:
        return QStringLiteral("X");
    case DragAxisLock::Y:
        return QStringLiteral("Y");
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
    explicit ViewportWidget(QWidget *parent = nullptr)
        : ViewportWidgetApi(parent)
        , history_(document_)
        , viewportRenderer_(viewportTransform_, curveHitTester_)
        , viewportOverlay_(viewportRenderer_, viewportTransform_)
        , toolContext_(document_,
                       selection_,
                       history_,
                       viewportTransform_,
                       curveSampler_,
                       curveHitTester_,
                       snapEngine_)
        , toolRegistry_()
        , shapes_(document_)
        , pan_(viewportTransform_.pan())
        , selectedShapeIndices_(selection_.objectIds())
        , selectedShapeIndex_(selection_.primaryObjectId())
        , controlPointIndex_(selection_.activeControlPointIndex())
        , zoom_(viewportTransform_.zoom())
    {
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
            recordGeometryChange();
            Shape committedShape = shape;
            if (!isValidWorkPlaneFrame(committedShape.workPlaneFrame)) {
                committedShape.workPlane = viewportTransform_.workPlane();
                committedShape.workPlaneOffset = viewportTransform_.workPlaneOffset();
                committedShape.workPlaneFrame = viewportTransform_.workPlaneFrame();
            }
            shapes_.append(committedShape);
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
        toolContext_.setShapesCommitter([this](ToolId, const QVector<Shape> &shapes) {
            if (shapes.isEmpty()) return false;
            for (const Shape &shape : shapes) {
                if (!isValidWorkPlaneFrame(shape.workPlaneFrame)) return false;
                if (shape.geometryType == GeometryType::Point) {
                    if (shape.points.size() != 1 ||
                        !std::isfinite(shape.points.first().x()) ||
                        !std::isfinite(shape.points.first().y())) return false;
                } else if (!validateNurbsCurve(shape.nurbs)) {
                    return false;
                }
            }
            recordGeometryChange();
            for (const Shape &shape : shapes) shapes_.append(shape);
            return true;
        });
        toolContext_.setPreviewPublisher([this](const ToolPreview &preview) {
            // Rendering remains in the viewport for now, but the pending
            // points themselves are owned by the active tool.
            // Arc is still driven by the viewport's established multi-stage
            // interaction below. Its registered ShapeCreationTool is only a
            // lifecycle bridge and has no Arc click state, so accepting that
            // controller's empty preview would erase the Arc's input points.
            if (activeTool_ != Tool::Arc) {
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
                linePreviewFrame_ = preview.workPlaneFrame;
                linePreviewWorldPoints_ = preview.worldPoints;
                linePreviewWorldCursor_ = preview.worldCursorPoint;
                linePreviewShapes_ = preview.shapes;
                linePreviewPlaneLocked_ = preview.planeLocked;
                if (preview.overridesSnap) currentSnap_ = preview.snap;
                if (preview.hasCursorPoint) {
                    cursorWorld_ = preview.cursorPoint;
                    cursorValid_ = true;
                }
            }
            if (activeTool_ == Tool::PointExtrude && preview.overridesSnap) {
                pointExtrudePreviewShapes_ = preview.shapes;
                currentSnap_ = preview.snap;
                if (preview.hasCursorPoint &&
                    isValidWorkPlaneFrame(viewportTransform_.workPlaneFrame())) {
                    cursorWorld_ = worldPointToWorkPlaneFrame(
                        preview.worldCursorPoint,
                        viewportTransform_.workPlaneFrame());
                    cursorValid_ = true;
                }
            }
            controllerPreviewShape_ = preview.shape;
            if (preview.hasShape &&
                !isValidWorkPlaneFrame(controllerPreviewShape_.workPlaneFrame)) {
                controllerPreviewShape_.workPlane = viewportTransform_.workPlane();
                controllerPreviewShape_.workPlaneOffset =
                    viewportTransform_.workPlaneOffset();
                controllerPreviewShape_.workPlaneFrame =
                    viewportTransform_.workPlaneFrame();
            }
            controllerPreviewShapeVisible_ = preview.hasShape;
            if (isCircleConstructionTool(activeTool_)) {
                circleHudDimensionsLine_ = preview.hudDimensionsLine;
                circleHudInstructionsLine_ = preview.hudInstructionsLine;
                if (preview.hasCursorPoint) {
                    cursorWorld_ = preview.cursorPoint;
                    cursorValid_ = true;
                }
            }
            if (isEllipseTool(activeTool_)) {
                ellipsePreviewGuides_ = preview.guides;
                ellipseHudDimensionsLine_ = preview.hudDimensionsLine;
                ellipseHudInstructionsLine_ = preview.hudInstructionsLine;
                cursorValid_ = preview.hasCursorPoint;
                if (preview.hasCursorPoint) {
                    cursorWorld_ = preview.cursorPoint;
                }
            }
            if (isRectangleTool(activeTool_)) {
                rectangleHudDimensionsLine_ = preview.hudDimensionsLine;
                rectangleHudInstructionsLine_ = preview.hudInstructionsLine;
                rectanglePreviewGuides_ = preview.guides;
                cursorValid_ = preview.hasCursorPoint;
                if (preview.hasCursorPoint) cursorWorld_ = preview.cursorPoint;
            }
            if (isPolygonTool(activeTool_)) {
                polygonHudDimensionsLine_ = preview.hudDimensionsLine;
                polygonHudInstructionsLine_ = preview.hudInstructionsLine;
                cursorValid_ = preview.hasCursorPoint;
                if (preview.hasCursorPoint) {
                    cursorWorld_ = preview.cursorPoint;
                }
            }
            if (isPointCreationTool(activeTool_) && activeTool_ != Tool::Point) {
                if (activeTool_ == Tool::PointByLine && preview.overridesSnap) {
                    currentSnap_ = preview.snap;
                }
                pointPreviewShapes_ = preview.shapes;
                pointPreviewGuides_ = preview.guides;
                pointPreviewFrame_ = preview.workPlaneFrame;
                pointPreviewWorldPoints_ = preview.worldPoints;
                pointHudInstructionsLine_ = preview.hudInstructionsLine;
                pointPreviewStage_ = preview.activeStage;
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
            return arcMode_;
        });
        toolContext_.setArcSweepProvider([this] {
            return arcPreviewSweepAngle_;
        });
        toolContext_.setPolygonSideCountCallbacks(
            [this] { return polygonSideCount_; },
            [this](int sideCount) {
                polygonSideCount_ = std::clamp(sideCount, 3, 1001);
            });
        setMinimumSize(480, 320);
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        qApp->installEventFilter(this);
        setCursor(Qt::CrossCursor);
        navigationAnimation_ = new QVariantAnimation(this);
        navigationAnimation_->setDuration(200);
        navigationAnimation_->setStartValue(0.0);
        navigationAnimation_->setEndValue(1.0);
        navigationAnimation_->setEasingCurve(QEasingCurve::Linear);
        connect(navigationAnimation_, &QVariantAnimation::valueChanged,
                this, [this](const QVariant &value) {
                    const qreal time = value.toReal();
                    // Blender's smooth view uses 3*t*t - 2*t*t*t.
                    const qreal progress = time * time * (3.0 - 2.0 * time);
                    ViewportCameraState state;
                    const qreal startDistance = 1.0 / navigationAnimationStart_.zoom;
                    const qreal endDistance = 1.0 / navigationAnimationEnd_.zoom;
                    state.zoom = 1.0 / (startDistance * (1.0 - progress) +
                                        endDistance * progress);
                    state.gridViewDistance =
                        navigationAnimationStart_.gridViewDistance * (1.0 - progress) +
                        navigationAnimationEnd_.gridViewDistance * progress;
                    state.pan = navigationAnimationStart_.pan * (1.0 - progress) +
                                navigationAnimationEnd_.pan * progress;
                    state.orbitPivot = {
                        navigationAnimationStart_.orbitPivot.x * (1.0 - progress) +
                            navigationAnimationEnd_.orbitPivot.x * progress,
                        navigationAnimationStart_.orbitPivot.y * (1.0 - progress) +
                            navigationAnimationEnd_.orbitPivot.y * progress,
                        navigationAnimationStart_.orbitPivot.z * (1.0 - progress) +
                            navigationAnimationEnd_.orbitPivot.z * progress};
                    state.orientation = ViewportOrientation::slerp(
                        navigationAnimationStart_.orientation,
                        navigationAnimationEnd_.orientation,
                        progress);
                    state.hasOrientation = true;
                    state.perspective = navigationAnimationStart_.perspective;
                    state.preset = progress >= 1.0
                                       ? navigationAnimationEnd_.preset
                                       : ViewportViewPreset::Custom;
                    viewportTransform_.setCameraState(state);
                    update();
                    emitCoordinateUpdate();
                });
        connect(navigationAnimation_, &QVariantAnimation::finished,
                this, [this]() {
                    viewportTransform_.setCameraState(navigationAnimationEnd_);
                    notifyViewStateChanged();
                    update();
                    emitCoordinateUpdate();
                });
        if (ViewportGpuSurface::isSupported()) {
            gpuSurface_ = new ViewportGpuSurface(this);
            gpuSurface_->setGeometry(rect());
            gpuSurface_->setAntiAliasingSamples(
                blenderGridRenderer_.antiAliasingSamples());
            gpuSurface_->setDrawCallback(
                [this](QPainter &painter, BlenderGridRenderer &gridRenderer,
                       ViewportSceneRenderer &sceneRenderer,
                       ViewportSceneRenderer &previewRenderer,
                       ViewportControlPointRenderer &controlPointRenderer) {
                    paintViewport(painter, &gridRenderer, &sceneRenderer,
                                  &previewRenderer, &controlPointRenderer);
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
        if (duplicateActive_) {
            cancelDuplicate();
        }
        if (grabActive_ && tool != Tool::Select) {
            cancelGrab();
        }
        const Tool previousTool = activeTool_;
        if (previousTool == Tool::Picture && tool != Tool::Picture) {
            pendingPictureImage_ = QImage();
            pendingPictureImageData_.clear();
            pendingPicturePath_.clear();
            repeatTool_ = Tool::Select;
        }
        if (activeToolController_ != nullptr && activeToolController_->id() != tool) {
            activeToolController_->cancel(toolContext_);
        }
        if (subdivisionActive_ && tool != Tool::Select) {
            cancelSubdivisionPreview();
        }
        if (joinActive_ && tool != Tool::Select) {
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
        toolDrawingFrame_ = {};
        toolDrawingPlaneLocked_ = false;
        linePreviewFrame_ = {};
        linePreviewWorldPoints_.clear();
        linePreviewShapes_.clear();
        linePreviewPlaneLocked_ = false;
        controllerPreviewShape_ = Shape{};
        controllerPreviewShapeVisible_ = false;
        circleHudDimensionsLine_.clear();
        circleHudInstructionsLine_.clear();
        ellipseHudDimensionsLine_.clear();
        ellipseHudInstructionsLine_.clear();
        ellipsePreviewGuides_.clear();
        pointPreviewShapes_.clear();
        pointExtrudePreviewShapes_.clear();
        pointPreviewGuides_.clear();
        pointPreviewWorldPoints_.clear();
        pointHudInstructionsLine_.clear();
        pointPreviewStage_ = -1;
        pointPreviewFrame_ = {};
        polygonHudDimensionsLine_.clear();
        polygonHudInstructionsLine_.clear();
        rectangleHudDimensionsLine_.clear();
        rectangleHudInstructionsLine_.clear();
        rectanglePreviewGuides_.clear();
        resetArcPreviewTracking();
        resetArcInputState();
        lineCommandActive_ = tool == Tool::Line;
        if (!isEraseLikeTool(tool) || previousTool != tool) {
            eraseStrokeActive_ = false;
            eraseCursorPressed_ = false;
            eraseCandidateShapeIndices_.clear();
            eraseStrokeScreenPath_.clear();
            eraseTargetShapeIndices_.clear();
            eraseSceneCurveCaches_.clear();
            eraseTargetCurveCaches_.clear();
            eraseGeometryCachePrepared_ = false;
            trimHoverPositionValid_ = false;
            trimHoverComponentIndex_ = -1;
        }

        if (tool != Tool::Select) {
            repeatTool_ = tool;
            if (!isEraseLikeTool(tool) && tool != Tool::Rotate &&
                tool != Tool::Scale && tool != Tool::Mirror) {
                if (tool != Tool::PointCenter && tool != Tool::PointExtrude) {
                    selectedShapeIndices_.clear();
                    selectedShapeIndex_ = ObjectId::invalid();
                }
            }
            selectionBoxActive_ = false;
            selectionBoxMoved_ = false;
            selectionBoxAdditive_ = false;
            trimBoxSelectionActive_ = false;
            draggingSelected_ = false;
            dragGestureStarted_ = false;
            draggingShapeIndices_.clear();
            draggingControlPoint_ = false;
            controlPointIndex_ = -1;
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
        switch (command) {
        case ViewportCommand::Undo: {
            if (duplicateActive_) {
                cancelDuplicate();
            }
            const bool accepted = history_.canUndo();
            undo();
            return {accepted, 0};
        }
        case ViewportCommand::Redo: {
            if (duplicateActive_) {
                cancelDuplicate();
            }
            const bool accepted = history_.canRedo();
            redo();
            return {accepted, 0};
        }
        case ViewportCommand::BeginSubdivision:
            return {beginSubdivisionWheelMode(), 0};
        case ViewportCommand::CancelSubdivision:
            cancelSubdivisionWheelMode();
            return {true, 0};
        case ViewportCommand::ApplySubdivision:
            return {applySubdivision(argument), 0};
        case ViewportCommand::BeginJoin:
            return {beginJoinMode(), 0};
        case ViewportCommand::Explode: {
            const int explodedComponents = explodeSelectedShapes();
            return {explodedComponents > 0, explodedComponents};
        }
        case ViewportCommand::BeginRotate:
            return {beginRotate(), 0};
        case ViewportCommand::BeginScale:
            if (argument < static_cast<int>(ScaleMode::OneD) ||
                argument > static_cast<int>(ScaleMode::TwoD)) {
                return {};
            }
            return {beginScale(static_cast<ScaleMode>(argument)), 0};
        case ViewportCommand::BeginMirror:
            return {beginMirror(), 0};
        case ViewportCommand::BeginDuplicate:
            return {beginDuplicate(), 0};
        case ViewportCommand::DuplicateInPlace:
            return {duplicateInPlace(), 0};
        case ViewportCommand::BeginPointExtrude:
        {
            const int pointCount = beginPointExtrude();
            return {pointCount > 0, pointCount};
        }
        }

        return {};
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
        const auto hasEditableAlternative = [this](LayerId excludedLayer) {
            for (const Layer &layer : document_.layers()) {
                if (layer.id != excludedLayer && layer.visible && !layer.frozen &&
                    !layer.locked) {
                    return true;
                }
            }
            return false;
        };

        switch (request.command) {
        case ViewportLayerCommand::Create: {
            recordLayerChange();
            const LayerId layerId = document_.createLayer(request.name);
            if (!document_.setActiveLayer(layerId)) {
                return {};
            }
            notifyLayersChanged();
            update();
            return {true, layerId, 0};
        }
        case ViewportLayerCommand::Remove: {
            const Layer *layer = document_.layer(request.layerId);
            if (layer == nullptr || document_.layers().size() <= 1 ||
                !layer->objectIds.isEmpty() ||
                (document_.activeLayerId() == request.layerId &&
                 !hasEditableAlternative(request.layerId))) {
                return {};
            }
            recordLayerChange();
            if (!document_.removeLayer(request.layerId)) {
                return {};
            }
            pruneSelectionToEditableLayers();
            notifyLayersChanged();
            update();
            return {true, document_.activeLayerId(), 0};
        }
        case ViewportLayerCommand::Activate: {
            if (document_.activeLayerId() == request.layerId) {
                return {true, request.layerId, 0};
            }
            if (!document_.isLayerEditable(request.layerId)) {
                return {};
            }
            recordLayerChange();
            if (!document_.setActiveLayer(request.layerId)) {
                return {};
            }
            notifyLayersChanged();
            update();
            return {true, request.layerId, 0};
        }
        case ViewportLayerCommand::SetVisible: {
            const Layer *layer = document_.layer(request.layerId);
            if (layer == nullptr || layer->visible == request.enabled) {
                return layer == nullptr ? ViewportLayerCommandResult{}
                                        : ViewportLayerCommandResult{true, request.layerId, 0};
            }
            if (!request.enabled && document_.activeLayerId() == request.layerId &&
                !hasEditableAlternative(request.layerId)) {
                return {};
            }
            recordLayerChange();
            if (!document_.setLayerVisible(request.layerId, request.enabled)) {
                return {};
            }
            pruneSelectionToEditableLayers();
            notifyLayersChanged();
            update();
            return {true, request.layerId, 0};
        }
        case ViewportLayerCommand::SetLocked: {
            const Layer *layer = document_.layer(request.layerId);
            if (layer == nullptr || layer->locked == request.enabled) {
                return layer == nullptr ? ViewportLayerCommandResult{}
                                        : ViewportLayerCommandResult{true, request.layerId, 0};
            }
            if (request.enabled && document_.activeLayerId() == request.layerId &&
                !hasEditableAlternative(request.layerId)) {
                return {};
            }
            recordLayerChange();
            if (!document_.setLayerLocked(request.layerId, request.enabled)) {
                return {};
            }
            pruneSelectionToEditableLayers();
            notifyLayersChanged();
            update();
            return {true, request.layerId, 0};
        }
        case ViewportLayerCommand::SetFrozen: {
            const Layer *layer = document_.layer(request.layerId);
            if (layer == nullptr || layer->frozen == request.enabled) {
                return layer == nullptr ? ViewportLayerCommandResult{}
                                        : ViewportLayerCommandResult{true, request.layerId, 0};
            }
            if (request.enabled && document_.activeLayerId() == request.layerId &&
                !hasEditableAlternative(request.layerId)) {
                return {};
            }
            recordLayerChange();
            if (!document_.setLayerFrozen(request.layerId, request.enabled)) {
                return {};
            }
            pruneSelectionToEditableLayers();
            notifyLayersChanged();
            update();
            return {true, request.layerId, 0};
        }
        case ViewportLayerCommand::SetColor: {
            const Layer *layer = document_.layer(request.layerId);
            if (layer == nullptr || !request.color.isValid()) {
                return {};
            }
            if (layer->color == request.color) {
                return {true, request.layerId, 0};
            }
            recordLayerChange();
            if (!document_.setLayerColor(request.layerId, request.color)) {
                return {};
            }
            notifyLayersChanged();
            update();
            return {true, request.layerId, 0};
        }
        case ViewportLayerCommand::SetLineType: {
            const Layer *layer = document_.layer(request.layerId);
            const QString lineType = request.name.trimmed();
            if (layer == nullptr || lineType.isEmpty()) {
                return {};
            }
            if (layer->lineType == lineType) {
                return {true, request.layerId, 0};
            }
            recordLayerChange();
            if (!document_.setLayerLineType(request.layerId, lineType)) {
                return {};
            }
            notifyLayersChanged();
            update();
            return {true, request.layerId, 0};
        }
        case ViewportLayerCommand::SetLineWeight: {
            const Layer *layer = document_.layer(request.layerId);
            if (layer == nullptr || layer->lineWeightMm == request.lineWeightMm) {
                return layer == nullptr ? ViewportLayerCommandResult{}
                                        : ViewportLayerCommandResult{true, request.layerId, 0};
            }
            recordLayerChange();
            if (!document_.setLayerLineWeight(request.layerId, request.lineWeightMm)) {
                return {};
            }
            notifyLayersChanged();
            update();
            return {true, request.layerId, 0};
        }
        case ViewportLayerCommand::SetPlotted: {
            const Layer *layer = document_.layer(request.layerId);
            if (layer == nullptr || layer->plotted == request.enabled) {
                return layer == nullptr ? ViewportLayerCommandResult{}
                                        : ViewportLayerCommandResult{true, request.layerId, 0};
            }
            recordLayerChange();
            if (!document_.setLayerPlotted(request.layerId, request.enabled)) {
                return {};
            }
            notifyLayersChanged();
            update();
            return {true, request.layerId, 0};
        }
        case ViewportLayerCommand::SetDescription: {
            const Layer *layer = document_.layer(request.layerId);
            if (layer == nullptr) {
                return {};
            }
            if (layer->description == request.name) {
                return {true, request.layerId, 0};
            }
            recordLayerChange();
            if (!document_.setLayerDescription(request.layerId, request.name)) {
                return {};
            }
            notifyLayersChanged();
            return {true, request.layerId, 0};
        }
        case ViewportLayerCommand::Rename: {
            const Layer *layer = document_.layer(request.layerId);
            const QString trimmedName = request.name.trimmed();
            if (layer == nullptr || trimmedName.isEmpty()) {
                return {};
            }
            if (layer->name == trimmedName) {
                return {true, request.layerId, 0};
            }
            recordLayerChange();
            if (!document_.renameLayer(request.layerId, trimmedName)) {
                return {};
            }
            notifyLayersChanged();
            update();
            return {true, request.layerId, 0};
        }
        case ViewportLayerCommand::Move: {
            if (document_.layer(request.layerId) == nullptr || request.index < 0 ||
                request.index >= document_.layers().size()) {
                return {};
            }
            recordLayerChange();
            if (!document_.moveLayer(request.layerId, request.index)) {
                return {};
            }
            notifyLayersChanged();
            update();
            return {true, request.layerId, 0};
        }
        case ViewportLayerCommand::MoveSelectedObjects: {
            if (!document_.isLayerEditable(request.layerId)) {
                return {};
            }

            QVector<ObjectId> movableObjects;
            for (const ObjectId objectId : selectedShapeIndices_) {
                if (document_.isObjectEditable(objectId) &&
                    document_.object(objectId)->layerId != request.layerId) {
                    movableObjects.append(objectId);
                }
            }
            if (movableObjects.isEmpty()) {
                return {};
            }

            recordLayerChange();
            int movedCount = 0;
            for (const ObjectId objectId : movableObjects) {
                if (document_.moveObjectToLayer(objectId, request.layerId)) {
                    ++movedCount;
                }
            }
            if (movedCount == 0) {
                return {};
            }
            notifyLayersChanged();
            update();
            return {true, request.layerId, movedCount};
        }
        }

        return {};
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

    void repeatLastTool()
    {
        if (repeatTool_ == Tool::Select) {
            DebugLog::instance().write(QStringLiteral("repeatTool ignored no-last-tool"));
            return;
        }

        const ToolId tool = repeatTool_;
        DebugLog::instance().write(QStringLiteral("repeatTool tool=%1")
                                       .arg(toolName(tool)));
        if (tool == Tool::Rotate) {
            if (beginRotate() && toolRepeated_) {
                toolRepeated_(tool);
            }
            return;
        }
        if (tool == Tool::Scale) {
            if (beginScale(scaleMode_) && toolRepeated_) {
                toolRepeated_(tool);
            }
            return;
        }
        if (tool == Tool::Mirror) {
            if (beginMirror() && toolRepeated_) {
                toolRepeated_(tool);
            }
            return;
        }
        setTool(tool);
        if (toolRepeated_) {
            toolRepeated_(tool);
        }
    }

    ToolId activeTool() const
    {
        return activeTool_;
    }

    void setArcMode(ArcMode mode)
    {
        arcMode_ = mode;
        pendingPoints_.clear();
        resetArcPreviewTracking();
        resetArcInputState();
        currentSnap_ = SnapResult{};
        DebugLog::instance().write(QStringLiteral("setArcMode mode=%1")
                                       .arg(arcModeName(arcMode_)));
        update();
    }

    ArcMode arcMode() const
    {
        return arcMode_;
    }

    void setWorkPlane(WorkPlane plane, qreal offset = 0.0) override
    {
        stopNavigationAnimation();
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
        controlPointIndex_ = -1;
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
        animateCameraChange([this, preset]() {
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
            draggingControlPoint_ = false;
            controlPointIndex_ = -1;
            dragHistoryRecorded_ = false;
            currentDragSnap_ = DragSnapResult{};
            dragSnapLocked_ = false;
        }
        DebugLog::instance().write(QStringLiteral("setControlPointsVisible=%1 selectedShape=%2")
                                       .arg(controlPointsVisible_)
                                       .arg(objectIndex(selectedShapeIndex_)));
        update();
    }

    bool controlPointsVisible() const
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
        rotateAngleSnapEnabled_ = preferences.angleSnapEnabled;
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
        const Document::Snapshot previous = document_.snapshot();
        if (!document_.setSettings(settings)) {
            return false;
        }
        const qreal gridSpacing = documentGridSpacingInMillimeters(settings);
        viewportTransform_.setGridSpacing(gridSpacing);
        viewportRenderer_.setGridBaseStep(gridSpacing);
        recordGeometrySnapshot(previous);
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

        panButton_ = button;
        DebugLog::instance().write(QStringLiteral("setPanButton applied=%1")
                                       .arg(inputButtonName(button)));
        update();
    }

    Qt::MouseButton panButton() const
    {
        return panButton_;
    }

    void setOrthoEnabled(bool enabled)
    {
        orthoEnabled_ = enabled;
        refreshCursorConstraint();

        if (activeTool_ == Tool::Arc && arcMode_ == ArcMode::OnePoint &&
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
        if (selectedIndex < 0 || !isSubdividableShape(shapes_[selectedIndex])) {
            DebugLog::instance().write(QStringLiteral("beginSubdivisionWheelMode ignored selectedShape=%1")
                                           .arg(selectedIndex));
            return false;
        }

        subdivisionShapeIndex_ = selectedShapeIndex_;
        subdivisionSections_ = std::clamp(
            static_cast<int>(shapes_[selectedIndex].subdivisionParameters.size()) + 1,
            2,
            maxSubdivisionSections);
        resetSubdivisionWheelTracking();
        subdivisionActive_ = true;
        setFocus(Qt::OtherFocusReason);
        notifySubdivisionStatus();
        update();
        DebugLog::instance().write(QStringLiteral("beginSubdivisionWheelMode shape=%1 sections=%2")
                                       .arg(selectedIndex)
                                       .arg(subdivisionSections_));
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

        const ObjectId shapeId = subdivisionActive_ ? subdivisionShapeIndex_ : selectedShapeIndex_;
        const int shapeIndex = objectIndex(shapeId);
        if (shapeIndex < 0 ||
            !isSubdividableShape(shapes_[shapeIndex])) {
            DebugLog::instance().write(QStringLiteral("applySubdivision ignored selectedShape=%1")
                                           .arg(shapeIndex));
            return false;
        }

        const QVector<double> parameters = subdivisionParametersForSections(
            shapes_[shapeIndex], sections);
        if (parameters.size() != sections - 1) {
            DebugLog::instance().write(QStringLiteral("applySubdivision failed shape=%1 sections=%2 generated=%3")
                                           .arg(shapeIndex)
                                           .arg(sections)
                                           .arg(parameters.size()));
            return false;
        }

        if (shapes_[shapeIndex].subdivisionParameters != parameters) {
            recordGeometryChange();
            shapes_[shapeIndex].subdivisionParameters = parameters;
        }

        subdivisionActive_ = false;
        subdivisionShapeIndex_ = ObjectId::invalid();
        subdivisionSections_ = 2;
        resetSubdivisionWheelTracking();
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
        return subdivisionActive_;
    }

    QString subdivisionStatusText() const
    {
        if (!subdivisionActive_) {
            return QString();
        }

        return QStringLiteral("Subdivide: %1 sections  •  Scroll to change  •  Click/Enter to apply  •  Esc to cancel")
            .arg(subdivisionSections_);
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
            }
        }
        if (pointCount == 0) {
            DebugLog::instance().write(
                QStringLiteral("beginPointExtrude ignored selectionCount=%1 noEditablePoints")
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
        if (joinActive_) {
            selected = joinShapeIndices_;
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

        resetRotateInteraction();
        setTool(Tool::Rotate);
        rotateShapeIndices_ = validSelection;
        setFocus(Qt::OtherFocusReason);
        setCursor(Qt::CrossCursor);
        update();
        DebugLog::instance().write(QStringLiteral("beginRotate shapes=%1")
                                       .arg(rotateShapeIndices_.size()));
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

        resetScaleInteraction();
        setTool(Tool::Scale);
        scaleMode_ = mode;
        scaleShapeIds_ = validSelection;
        scaleStep_ = 0;
        setFocus(Qt::OtherFocusReason);
        setCursor(Qt::CrossCursor);
        publishScalePrompt();
        update();
        DebugLog::instance().write(QStringLiteral("beginScale mode=%1 shapes=%2")
                                       .arg(scaleModeName(scaleMode_))
                                       .arg(scaleShapeIds_.size()));
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
        mirrorShapeIndices_ = validSelection;
        pendingPoints_.clear();
        setFocus(Qt::OtherFocusReason);
        setCursor(Qt::CrossCursor);
        update();
        DebugLog::instance().write(QStringLiteral("beginMirror shapes=%1")
                                       .arg(mirrorShapeIndices_.size()));
        return true;
    }

    bool beginGrab()
    {
        if (activeTool_ != Tool::Select || grabActive_ || draggingSelected_) {
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

        grabStartSnapshot_ = document_.snapshot();
        grabActive_ = true;
        grabMoved_ = false;
        grabPickingBasePoint_ = false;
        grabHasBasePoint_ = false;
        draggingSelected_ = true;
        dragGestureStarted_ = true;
        draggingShapeIndices_ = validSelection;
        if (!selectedShapeIndex_.isValid() || !validSelection.contains(selectedShapeIndex_)) {
            selectedShapeIndex_ = validSelection.back();
        }
        dragHistoryRecorded_ = false;
        currentDragSnap_ = DragSnapResult{};
        dragSnapLocked_ = false;
        dragAxisLock_ = DragAxisLock::None;
        currentSnap_ = SnapResult{};

        const QPoint localCursor = mapFromGlobal(QCursor::pos());
        if (rect().contains(localCursor)) {
            rawCursorWorld_ = screenToWorld(localCursor);
            cursorWorld_ = rawCursorWorld_;
            cursorValid_ = true;
        }
        dragStartScreen_ = localCursor;
        grabStartWorld_ = rawCursorWorld_;
        lastDragWorld_ = grabStartWorld_;
        setFocus(Qt::OtherFocusReason);
        setCursor(Qt::SizeAllCursor);
        update();
        DebugLog::instance().write(QStringLiteral("beginGrab shapes=%1")
                                       .arg(draggingShapeIndices_.size()));
        return true;
    }

    void beginGrabBasePointMode()
    {
        if (!grabActive_) {
            return;
        }

        if (grabMoved_) {
            document_.restoreSnapshot(grabStartSnapshot_);
            notifyLayersChanged();
        }
        grabMoved_ = false;
        grabPickingBasePoint_ = true;
        grabHasBasePoint_ = false;
        grabCursorOffset_ = QPointF();
        currentSnap_ = SnapResult{};
        currentDragSnap_ = DragSnapResult{};
        dragSnapLocked_ = false;
        setCursor(Qt::CrossCursor);
        update();
        DebugLog::instance().write(QStringLiteral("grab base-point selection started"));
    }

    void finishGrab()
    {
        if (!grabActive_) {
            return;
        }

        if (grabMoved_) {
            recordGeometrySnapshot(grabStartSnapshot_);
        }
        const bool moved = grabMoved_;
        resetGrabInteraction();
        draggingSelected_ = false;
        draggingShapeIndices_.clear();
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
        if (!grabActive_) {
            return;
        }

        const bool moved = grabMoved_;
        if (moved) {
            document_.restoreSnapshot(grabStartSnapshot_);
            notifyLayersChanged();
        }
        resetGrabInteraction();
        draggingSelected_ = false;
        draggingShapeIndices_.clear();
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
        if (activeTool_ != Tool::Select || duplicateActive_ || grabActive_ ||
            draggingSelected_ || joinActive_ || subdivisionActive_) {
            return false;
        }

        QVector<ObjectId> selected = selectedShapeIndices_;
        if (selectedShapeIndex_.isValid() && !selected.contains(selectedShapeIndex_)) {
            selected.append(selectedShapeIndex_);
        }

        duplicateSourceObjects_.clear();
        duplicateSourceObjects_.reserve(selected.size());
        for (const ObjectId objectId : selected) {
            const SceneObject *sceneObject = document_.object(objectId);
            if (sceneObject != nullptr && document_.isObjectEditable(objectId)) {
                duplicateSourceObjects_.append(*sceneObject);
            }
        }
        if (duplicateSourceObjects_.isEmpty()) {
            return false;
        }

        duplicateActive_ = true;
        duplicatePickingBasePoint_ = true;
        duplicateHasBasePoint_ = false;
        duplicateBasePoint_ = QPointF();
        duplicateCursorOffset_ = QPointF();
        duplicateDestination_ = QPointF();
        duplicatePreviewShapes_.clear();
        currentSnap_ = SnapResult{};
        setFocus(Qt::OtherFocusReason);
        setCursor(Qt::CrossCursor);
        update();
        DebugLog::instance().write(
            QStringLiteral("beginDuplicate objects=%1")
                .arg(duplicateSourceObjects_.size()));
        return true;
    }

    bool duplicateInPlace()
    {
        if (!beginDuplicate()) {
            return false;
        }

        duplicatePickingBasePoint_ = false;
        duplicateHasBasePoint_ = true;
        duplicateBasePoint_ = QPointF();
        duplicateCursorOffset_ = QPointF();
        duplicateDestination_ = QPointF();
        duplicatePreviewShapes_.reserve(duplicateSourceObjects_.size());
        for (const SceneObject &source : duplicateSourceObjects_) {
            duplicatePreviewShapes_.append(source.geometry);
        }
        finishDuplicate();
        return true;
    }

    void updateDuplicatePreview(const QPointF &rawPoint)
    {
        if (!duplicateActive_ || !duplicateHasBasePoint_) {
            return;
        }

        const QPointF destinationCursor = rawPoint - duplicateCursorOffset_;
        currentSnap_ = findDuplicateDestinationSnap(destinationCursor);
        duplicateDestination_ = currentSnap_.isValid()
                                    ? currentSnap_.point
                                    : destinationCursor;
        cursorWorld_ = duplicateDestination_;
        lastWorldPosition_ = duplicateDestination_;

        const QPointF delta = duplicateDestination_ - duplicateBasePoint_;
        duplicatePreviewShapes_.clear();
        duplicatePreviewShapes_.reserve(duplicateSourceObjects_.size());
        for (const SceneObject &source : duplicateSourceObjects_) {
            Shape preview = source.geometry;
            translateShapeGeometry(preview, delta);
            duplicatePreviewShapes_.append(preview);
        }
        update();
    }

    void finishDuplicate()
    {
        if (!duplicateActive_ || !duplicateHasBasePoint_ ||
            duplicatePreviewShapes_.isEmpty()) {
            return;
        }

        const Document::Snapshot before = document_.snapshot();
        QVector<ObjectId> duplicateIds;
        duplicateIds.reserve(duplicatePreviewShapes_.size());
        for (int index = 0; index < duplicatePreviewShapes_.size(); ++index) {
            SceneObject duplicate = duplicateSourceObjects_[index];
            duplicate.id = ObjectId::invalid();
            duplicate.geometry = duplicatePreviewShapes_[index];
            duplicateIds.append(shapes_.insertObject(shapes_.size(), duplicate));
        }

        for (int duplicateIndex = 0; duplicateIndex < duplicateIds.size(); ++duplicateIndex) {
            Shape *duplicateGeometry = document_.shape(duplicateIds[duplicateIndex]);
            if (duplicateGeometry == nullptr ||
                !isDimensionGeometryType(duplicateGeometry->geometryType)) {
                continue;
            }
            for (DimensionAnchorReference &anchor : duplicateGeometry->dimensionAnchors) {
                bool remapped = false;
                for (int sourceIndex = 0;
                     sourceIndex < duplicateSourceObjects_.size();
                     ++sourceIndex) {
                    if (anchor.objectId == duplicateSourceObjects_[sourceIndex].id) {
                        anchor.objectId = duplicateIds[sourceIndex];
                        remapped = true;
                        break;
                    }
                }
                if (!remapped) {
                    // A copied dimension without its referenced geometry is a
                    // positioned copy, not a second annotation tied to the
                    // original objects.
                    anchor = DimensionAnchorReference{};
                }
            }
        }

        recordGeometrySnapshot(before);
        selection_.setObjectIds(duplicateIds,
                                duplicateIds.isEmpty() ? ObjectId::invalid()
                                                       : duplicateIds.back());
        selectedShapeIndex_ = selection_.primaryObjectId();
        const int duplicateCount = duplicateIds.size();
        const QPointF committedDelta = duplicateDestination_ - duplicateBasePoint_;
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
        if (!duplicateActive_) {
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
        if (subdivisionActive_) {
            cancelSubdivisionPreview();
        }

        const QVector<ObjectId> preselectedShapes = selectedShapeIndices_;
        setTool(Tool::Select);
        joinActive_ = true;
        joinShapeIndices_ = preselectedShapes;
        if (joinShapeIndices_.isEmpty()) {
            selectedShapeIndices_.clear();
            selectedShapeIndex_ = ObjectId::invalid();
        } else {
            selectedShapeIndex_ = joinShapeIndices_.back();
        }
        selectionBoxActive_ = false;
        selectionBoxMoved_ = false;
        selectionBoxAdditive_ = false;
        trimBoxSelectionActive_ = false;
        draggingSelected_ = false;
        draggingShapeIndices_.clear();
        draggingControlPoint_ = false;
        controlPointIndex_ = -1;
        setFocus(Qt::OtherFocusReason);
        setCursor(Qt::CrossCursor);
        update();
        DebugLog::instance().write(
            QStringLiteral("beginJoinMode preselected=%1")
                .arg(joinShapeIndices_.size()));
        if (!joinShapeIndices_.isEmpty()) {
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
        if (!joinActive_) {
            return;
        }

        const QVector<ObjectId> joinedSelection = joinShapeIndices_;
        joinActive_ = false;
        joinShapeIndices_.clear();
        selectedShapeIndices_ = joinedSelection;
        selectedShapeIndex_ = selectedShapeIndices_.isEmpty()
                                  ? ObjectId::invalid()
                                  : selectedShapeIndices_.back();
        setCursor(Qt::ArrowCursor);
        if (notifyStatus) {
            notifyJoinStatus();
        }
        update();
        DebugLog::instance().write(QStringLiteral("cancelJoinMode"));
    }

    QString joinStatusText() const
    {
        if (!joinActive_) {
            return QString();
        }

        return QStringLiteral("Join: %1 curves selected  •  Click connected curves to join  •  Esc to cancel")
            .arg(joinShapeIndices_.size());
    }

    bool applyJoin()
    {
        if (!joinActive_) {
            return false;
        }

        if (joinShapeIndices_.isEmpty()) {
            notifyJoinStatus(QStringLiteral("Join needs at least two curves"));
            return false;
        }

        const int referenceShapeIndex = objectIndex(joinShapeIndices_.first());
        if (referenceShapeIndex < 0 || referenceShapeIndex >= shapes_.size()) {
            notifyJoinStatus(QStringLiteral("Join failed — selected curves are unavailable"));
            return false;
        }
        const Shape &referenceShape = shapes_[referenceShapeIndex];
        const SceneObject *referenceObject =
            document_.object(joinShapeIndices_.first());
        if (referenceObject == nullptr) {
            notifyJoinStatus(QStringLiteral("Join failed — selected curves are unavailable"));
            return false;
        }
        const LayerId joinedLayerId = referenceObject->layerId;
        const WorkPlaneFrame joinFrame = shapeWorkPlaneFrame(referenceShape);
        const WorkPlane joinWorkPlane = referenceShape.workPlane;
        const qreal joinWorkPlaneOffset = referenceShape.workPlaneOffset;
        QVector<Shape::NurbsCurve2D> components;
        QVector<WorkPlaneFrame> componentFrames;
        bool mixedPlanes = false;
        for (const ObjectId objectId : joinShapeIndices_) {
            const int shapeIndex = objectIndex(objectId);
            if (shapeIndex < 0 || shapeIndex >= shapes_.size()) {
                notifyJoinStatus(QStringLiteral("Join failed — select lines or curves only"));
                return false;
            }
            const Shape &sourceShape = shapes_[shapeIndex];
            const WorkPlaneFrame sourceFrame = shapeWorkPlaneFrame(sourceShape);
            const int firstComponent = components.size();
            if (!appendJoinComponents(sourceShape, &components)) {
                notifyJoinStatus(QStringLiteral("Join failed — select lines or curves only"));
                return false;
            }
            for (int componentIndex = firstComponent;
                 componentIndex < components.size();
                 ++componentIndex) {
                const int sourceComponentIndex = componentIndex - firstComponent;
                const WorkPlaneFrame componentFrame =
                    sourceShape.geometryType == GeometryType::PolyCurve
                        ? shapeComponentWorkPlaneFrame(sourceShape,
                                                       sourceComponentIndex)
                        : sourceFrame;
                componentFrames.append(componentFrame);
                mixedPlanes = mixedPlanes ||
                              !workPlaneFramesCoplanar(joinFrame, componentFrame);
            }
        }

        if (components.size() < 2) {
            notifyJoinStatus(QStringLiteral("Join failed — select at least two curves"));
            return false;
        }

        const qreal selectedJoinTolerance = selectedJoinEndpointTolerance();
        if (mixedPlanes) {
            fuseOverlappingLineComponentsInWorld(&components,
                                                  &componentFrames,
                                                  selectedJoinTolerance);
            QVector<Shape::NurbsCurve2D> orderedComponents;
            QVector<WorkPlaneFrame> orderedFrames;
            if (!orderJoinComponentsInWorld(components,
                                            componentFrames,
                                            &orderedComponents,
                                            &orderedFrames,
                                            selectedJoinTolerance)) {
                notifyJoinStatus(QStringLiteral("Join failed — selected curves are not connected"));
                DebugLog::instance().write(
                    QStringLiteral("applyJoin rejected mixed-plane components=%1 tolerance=%2")
                        .arg(components.size())
                        .arg(selectedJoinTolerance, 0, 'f', 6));
                return false;
            }
            components = std::move(orderedComponents);
            componentFrames = std::move(orderedFrames);
            if (!closeJoinGapsInWorld(&components,
                                      &componentFrames,
                                      selectedJoinTolerance) ||
                !joinComponentsAreContinuousInWorld(components,
                                                    componentFrames,
                                                    selectedJoinTolerance)) {
                notifyJoinStatus(QStringLiteral("Join failed — selected curves are not connected"));
                return false;
            }
        } else {
            for (int componentIndex = 0;
                 componentIndex < components.size();
                 ++componentIndex) {
                for (QPointF &controlPoint :
                     components[componentIndex].controlPoints) {
                    controlPoint = worldPointToWorkPlaneFrame(
                        workPlaneFramePointToWorld(
                            controlPoint, componentFrames[componentIndex]),
                        joinFrame);
                }
            }
            componentFrames.clear();
            fuseOverlappingLineComponents(&components, selectedJoinTolerance);
        }
        if (!mixedPlanes &&
            !joinComponentsAreContinuous(components, selectedJoinTolerance)) {
            QVector<Shape::NurbsCurve2D> orderedComponents;
            if (!orderJoinComponents(components,
                                     &orderedComponents,
                                     selectedJoinTolerance)) {
                notifyJoinStatus(QStringLiteral("Join failed — selected curves are not connected"));
                qreal nearestEndpointGap =
                    std::numeric_limits<qreal>::infinity();
                for (int first = 0; first < components.size(); ++first) {
                    QPointF firstStart;
                    QPointF firstEnd;
                    if (!nurbsCurveEndpoints(components[first],
                                             &firstStart,
                                             &firstEnd)) {
                        continue;
                    }
                    for (int second = first + 1;
                         second < components.size();
                         ++second) {
                        QPointF secondStart;
                        QPointF secondEnd;
                        if (!nurbsCurveEndpoints(components[second],
                                                 &secondStart,
                                                 &secondEnd)) {
                            continue;
                        }
                        for (const QPointF &firstEndPoint : {firstStart, firstEnd}) {
                            for (const QPointF &secondEndPoint : {secondStart,
                                                                  secondEnd}) {
                                nearestEndpointGap = std::min(
                                    nearestEndpointGap,
                                    std::hypot(firstEndPoint.x() -
                                                   secondEndPoint.x(),
                                               firstEndPoint.y() -
                                                   secondEndPoint.y()));
                            }
                        }
                    }
                }
                const qreal viewScale =
                    viewportTransform_.viewScalePixelsPerWorldUnit(size());
                DebugLog::instance().write(
                    QStringLiteral("applyJoin rejected disconnected components=%1 tolerance=%2 nearestEndpointGap=%3 nearestGapPixels=%4")
                        .arg(components.size())
                        .arg(selectedJoinTolerance, 0, 'f', 6)
                        .arg(nearestEndpointGap, 0, 'f', 6)
                        .arg(nearestEndpointGap * viewScale, 0, 'f', 2));
                return false;
            }
            components = orderedComponents;
            DebugLog::instance().write(QStringLiteral("applyJoin reordered/reversed connected components=%1")
                                           .arg(components.size()));
        }

        if (!mixedPlanes &&
            (!closeJoinGaps(&components, selectedJoinTolerance) ||
             !joinComponentsAreContinuous(components, selectedJoinTolerance))) {
            notifyJoinStatus(QStringLiteral("Join failed — selected curves are not connected"));
            DebugLog::instance().write(QStringLiteral("applyJoin rejected reordered components=%1")
                                           .arg(components.size()));
            return false;
        }

        int insertIndex = shapes_.size();
        for (const ObjectId objectId : joinShapeIndices_) {
            const int shapeIndex = objectIndex(objectId);
            if (shapeIndex >= 0) {
                insertIndex = std::min(insertIndex, shapeIndex);
            }
        }
        const int sourceShapeCount = joinShapeIndices_.size();
        recordGeometryChange();

        QVector<int> indicesToRemove;
        for (const ObjectId objectId : joinShapeIndices_) {
            const int shapeIndex = objectIndex(objectId);
            if (shapeIndex >= 0) {
                indicesToRemove.append(shapeIndex);
            }
        }
        std::sort(indicesToRemove.begin(), indicesToRemove.end());
        for (auto index = indicesToRemove.crbegin(); index != indicesToRemove.crend(); ++index) {
            shapes_.removeAt(*index);
        }

        Shape joined;
        const WorkPlaneFrame resultFrame = mixedPlanes && components.size() == 1
                                               ? componentFrames.first()
                                               : joinFrame;
        if (components.size() == 1) {
            joined.geometryType = GeometryType::Nurbs;
            joined.nurbs = components.first();
            joined.points = joined.nurbs.controlPoints;
        } else {
            joined.geometryType = GeometryType::PolyCurve;
            if (mixedPlanes) {
                for (int index = 0; index < components.size(); ++index) {
                    QPointF start;
                    QPointF end;
                    if (!nurbsCurveEndpoints(components[index], &start, &end)) {
                        continue;
                    }
                    if (index == 0) {
                        joined.points.append(worldPointToWorkPlaneFrame(
                            workPlaneFramePointToWorld(start, componentFrames[index]),
                            joinFrame));
                    }
                    joined.points.append(worldPointToWorkPlaneFrame(
                        workPlaneFramePointToWorld(end, componentFrames[index]),
                        joinFrame));
                }
                joined.componentWorkPlaneFrames = componentFrames;
            } else {
                joined.points = polyCurvePoints(components);
            }
            joined.components = components;
        }
        joined.workPlane = joinWorkPlane;
        joined.workPlaneOffset = joinWorkPlaneOffset;
        joined.workPlaneFrame = resultFrame;
        SceneObject joinedObject;
        joinedObject.layerId = joinedLayerId;
        joinedObject.geometry = joined;
        const ObjectId joinedObjectId =
            shapes_.insertObject(insertIndex, joinedObject);

        joinActive_ = false;
        joinShapeIndices_.clear();
        selectedShapeIndices_ = {joinedObjectId};
        selectedShapeIndex_ = joinedObjectId;
        setCursor(Qt::ArrowCursor);
        notifyJoinStatus(QStringLiteral("Joined %1 curves into one PolyCurve")
                             .arg(sourceShapeCount));
        update();
        DebugLog::instance().write(QStringLiteral("applyJoin committed components=%1 shapes=%2")
                                       .arg(components.size())
                                       .arg(shapes_.size()));
        return true;
    }

    int explodeSelectedShapes()
    {
        if (joinActive_) {
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

        const auto selectedContains = [&selected](ObjectId objectId) {
            return selected.contains(objectId);
        };

        int explodeableShapeCount = 0;
        int explodedComponentCount = 0;
        for (const ObjectId objectId : selected) {
            const int shapeIndex = objectIndex(objectId);
            if (shapeIndex < 0 || shapeIndex >= shapes_.size()) {
                continue;
            }

            const Shape &shape = shapes_[shapeIndex];
            if (shape.geometryType != GeometryType::PolyCurve || shape.components.isEmpty()) {
                continue;
            }

            bool validComponents = true;
            for (const Shape::NurbsCurve2D &component : shape.components) {
                if (!isValidNurbsCurve(component)) {
                    validComponents = false;
                    break;
                }
            }
            if (validComponents) {
                ++explodeableShapeCount;
                explodedComponentCount += shape.components.size();
            }
        }

        if (explodeableShapeCount == 0) {
            DebugLog::instance().write(QStringLiteral("explode ignored no PolyCurve selection"));
            return 0;
        }

        recordGeometryChange();

        QVector<SceneObject> explodedObjects;
        QVector<int> explodedSelectionIndices;
        explodedObjects.reserve(shapes_.size() + explodedComponentCount - explodeableShapeCount);

        for (int sourceIndex = 0; sourceIndex < shapes_.size(); ++sourceIndex) {
            const SceneObject sourceObject = document_.objects()[sourceIndex];
            const Shape &source = sourceObject.geometry;
            const bool selectedSource = selectedContains(sourceObject.id);
            const bool canExplode = selectedSource &&
                                    source.geometryType == GeometryType::PolyCurve &&
                                    !source.components.isEmpty();
            bool validComponents = canExplode;
            if (validComponents) {
                for (const Shape::NurbsCurve2D &component : source.components) {
                    if (!isValidNurbsCurve(component)) {
                        validComponents = false;
                        break;
                    }
                }
            }

            if (!validComponents) {
                const int newIndex = explodedObjects.size();
                explodedObjects.append(sourceObject);
                if (selectedSource) {
                    explodedSelectionIndices.append(newIndex);
                }
                continue;
            }

            for (int componentIndex = 0;
                 componentIndex < source.components.size();
                 ++componentIndex) {
                const Shape::NurbsCurve2D &component =
                    source.components[componentIndex];
                const WorkPlaneFrame componentFrame =
                    shapeComponentWorkPlaneFrame(source, componentIndex);
                const int newIndex = explodedObjects.size();
                SceneObject componentObject;
                componentObject.layerId = sourceObject.layerId;
                componentObject.geometry = Shape{
                    GeometryType::PolyCurve,
                    polyCurvePoints({component}),
                    Shape::NurbsCurve2D{},
                    ArcMode::TwoPoint,
                    0.0,
                    {},
                    {component}};
                componentObject.geometry.workPlane = source.workPlane;
                componentObject.geometry.workPlaneOffset = source.workPlaneOffset;
                componentObject.geometry.workPlaneFrame = componentFrame;
                explodedObjects.append(componentObject);
                explodedSelectionIndices.append(newIndex);
            }
        }

        document_.replaceObjects(explodedObjects);
        selectedShapeIndices_.clear();
        for (const int selectedIndex : explodedSelectionIndices) {
            const ObjectId objectId = document_.objectIdAt(selectedIndex);
            if (objectId.isValid()) {
                selectedShapeIndices_.append(objectId);
            }
        }
        selectedShapeIndex_ = selectedShapeIndices_.isEmpty()
                                  ? ObjectId::invalid()
                                  : selectedShapeIndices_.back();
        draggingSelected_ = false;
        draggingShapeIndices_.clear();
        draggingControlPoint_ = false;
        controlPointIndex_ = -1;
        dragHistoryRecorded_ = false;
        currentDragSnap_ = DragSnapResult{};
        dragSnapLocked_ = false;
        update();
        DebugLog::instance().write(
            QStringLiteral("explode committed shapes=%1 components=%2")
                .arg(explodeableShapeCount)
                .arg(explodedComponentCount));
        return explodedComponentCount;
    }

    bool saveUpdateSession(const QString &path) const
    {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            DebugLog::instance().write(QStringLiteral("saveUpdateSession failed path=%1 error=%2")
                                           .arg(path, file.errorString()));
            return false;
        }

        QJsonObject root;
        // Version 4 also carries the complete camera/workplane state so an
        // update restart reopens the same viewport, not just the same scene.
        // The version 1/2 restore path below still reads legacy shape-only
        // sessions, and version 3 keeps its original scene-only behavior.
        root.insert(QStringLiteral("version"), 4);
        root.insert(QStringLiteral("zoom"), zoom_);
        root.insert(QStringLiteral("pan"), pointToJson(pan_));
        root.insert(QStringLiteral("document"), documentToJson(document_));

        root.insert(QStringLiteral("viewportCamera"),
                    viewportCameraStateToJson(viewportTransform_.cameraState()));
        root.insert(QStringLiteral("workPlane"),
                    static_cast<int>(viewportTransform_.workPlane()));
        root.insert(QStringLiteral("workPlaneOffset"),
                    viewportTransform_.workPlaneOffset());
        root.insert(QStringLiteral("workPlaneFrame"),
                    workPlaneFrameToJson(viewportTransform_.workPlaneFrame()));

        const ViewportCameraPreferences cameraPreferences =
            viewportTransform_.cameraPreferences();
        QJsonObject cameraPreferencesJson;
        cameraPreferencesJson.insert(QStringLiteral("focalLengthMillimeters"),
                                     cameraPreferences.focalLengthMillimeters);
        cameraPreferencesJson.insert(QStringLiteral("clipStart"),
                                     cameraPreferences.clipStart);
        cameraPreferencesJson.insert(QStringLiteral("clipEnd"),
                                     cameraPreferences.clipEnd);
        root.insert(QStringLiteral("cameraPreferences"), cameraPreferencesJson);

        QJsonArray selectedObjectIds;
        for (const ObjectId objectId : selectedShapeIndices_) {
            selectedObjectIds.append(QString::number(objectId.value()));
        }
        root.insert(QStringLiteral("selectedObjectIds"), selectedObjectIds);
        root.insert(QStringLiteral("primaryObjectId"),
                    QString::number(selectedShapeIndex_.value()));
        const ControlPointReference activeControlPoint =
            selection_.activeControlPoint();
        if (activeControlPoint.isValid()) {
            QJsonObject controlPoint;
            controlPoint.insert(QStringLiteral("objectId"),
                                QString::number(activeControlPoint.objectId.value()));
            controlPoint.insert(QStringLiteral("index"), activeControlPoint.index);
            root.insert(QStringLiteral("activeControlPoint"), controlPoint);
        }
        root.insert(QStringLiteral("controlPointsVisible"), controlPointsVisible_);

        const QByteArray data = QJsonDocument(root).toJson(QJsonDocument::Compact);
        if (file.write(data) != data.size()) {
            DebugLog::instance().write(QStringLiteral("saveUpdateSession write failed path=%1 error=%2")
                                           .arg(path, file.errorString()));
            return false;
        }

        DebugLog::instance().write(QStringLiteral("saveUpdateSession path=%1 shapes=%2 layers=%3 version=4 preset=%4 perspective=%5 zoom=%6 pan=%7")
                                       .arg(path)
                                       .arg(shapes_.size())
                                       .arg(document_.layers().size())
                                       .arg(static_cast<int>(viewportTransform_.viewPreset()))
                                       .arg(viewportTransform_.isPerspectiveEnabled())
                                       .arg(zoom_, 0, 'g', 17)
                                       .arg(precisePointText(pan_)));
        return true;
    }

    bool restoreUpdateSession(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            DebugLog::instance().write(QStringLiteral("restoreUpdateSession failed path=%1 error=%2")
                                           .arg(path, file.errorString()));
            return false;
        }

        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            DebugLog::instance().write(QStringLiteral("restoreUpdateSession parse failed path=%1 error=%2")
                                           .arg(path, parseError.errorString()));
            return false;
        }

        const QJsonObject root = document.object();
        const int version = root.value(QStringLiteral("version")).toInt(-1);
        if (version < 1 || version > 4) {
            DebugLog::instance().write(QStringLiteral("restoreUpdateSession unsupported version=%1 path=%2")
                                           .arg(version)
                                           .arg(path));
            return false;
        }

        QPointF restoredPan;
        if (!pointFromJson(root.value(QStringLiteral("pan")), &restoredPan)) {
            DebugLog::instance().write(QStringLiteral("restoreUpdateSession invalid pan path=%1")
                                           .arg(path));
            return false;
        }

        const QJsonValue zoomValue = root.value(QStringLiteral("zoom"));
        const qreal restoredZoom = zoomValue.toDouble(1.0);
        if (!zoomValue.isDouble() || !std::isfinite(restoredZoom) || restoredZoom <= 1e-9) {
            DebugLog::instance().write(QStringLiteral("restoreUpdateSession invalid zoom path=%1")
                                           .arg(path));
            return false;
        }

        ViewportCameraState restoredCameraState;
        WorkPlane restoredWorkPlane = WorkPlane::XY;
        qreal restoredWorkPlaneOffset = 0.0;
        WorkPlaneFrame restoredWorkPlaneFrame;
        ViewportCameraPreferences restoredCameraPreferences =
            viewportTransform_.cameraPreferences();
        QVector<ObjectId> restoredSelectedObjectIds;
        ObjectId restoredPrimaryObjectId = ObjectId::invalid();
        ControlPointReference restoredActiveControlPoint;
        bool restoredControlPointsVisible = controlPointsVisible_;
        if (version >= 4) {
            if (!viewportCameraStateFromJson(
                    root.value(QStringLiteral("viewportCamera")),
                    &restoredCameraState)) {
                DebugLog::instance().write(
                    QStringLiteral("restoreUpdateSession invalid viewport camera path=%1")
                        .arg(path));
                return false;
            }

            WorkPlane parsedWorkPlane;
            const QJsonValue workPlaneValue = root.value(QStringLiteral("workPlane"));
            const QJsonValue workPlaneOffsetValue =
                root.value(QStringLiteral("workPlaneOffset"));
            restoredWorkPlaneOffset = workPlaneOffsetValue.toDouble(
                std::numeric_limits<qreal>::quiet_NaN());
            if (!workPlaneValue.isDouble() ||
                !workPlaneFromValue(workPlaneValue.toInt(-1), &parsedWorkPlane) ||
                !std::isfinite(restoredWorkPlaneOffset) ||
                !workPlaneFrameFromJson(root.value(QStringLiteral("workPlaneFrame")),
                                        &restoredWorkPlaneFrame)) {
                DebugLog::instance().write(
                    QStringLiteral("restoreUpdateSession invalid workplane path=%1")
                        .arg(path));
                return false;
            }
            restoredWorkPlane = parsedWorkPlane;

            const QJsonObject cameraPreferences =
                root.value(QStringLiteral("cameraPreferences")).toObject();
            const QJsonValue focalLengthValue =
                cameraPreferences.value(QStringLiteral("focalLengthMillimeters"));
            const QJsonValue clipStartValue =
                cameraPreferences.value(QStringLiteral("clipStart"));
            const QJsonValue clipEndValue =
                cameraPreferences.value(QStringLiteral("clipEnd"));
            restoredCameraPreferences = {focalLengthValue.toDouble(
                                             std::numeric_limits<qreal>::quiet_NaN()),
                                         clipStartValue.toDouble(
                                             std::numeric_limits<qreal>::quiet_NaN()),
                                         clipEndValue.toDouble(
                                             std::numeric_limits<qreal>::quiet_NaN())};
            if (!focalLengthValue.isDouble() || !clipStartValue.isDouble() ||
                !clipEndValue.isDouble() ||
                !std::isfinite(restoredCameraPreferences.focalLengthMillimeters) ||
                restoredCameraPreferences.focalLengthMillimeters < 1.0 ||
                restoredCameraPreferences.focalLengthMillimeters > 2000.0 ||
                !std::isfinite(restoredCameraPreferences.clipStart) ||
                restoredCameraPreferences.clipStart < 0.000001 ||
                !std::isfinite(restoredCameraPreferences.clipEnd) ||
                restoredCameraPreferences.clipEnd <= restoredCameraPreferences.clipStart ||
                restoredCameraPreferences.clipEnd > 1.0e9) {
                DebugLog::instance().write(
                    QStringLiteral("restoreUpdateSession invalid camera preferences path=%1")
                        .arg(path));
                return false;
            }

            const QJsonValue selectedIdsValue =
                root.value(QStringLiteral("selectedObjectIds"));
            if (!selectedIdsValue.isArray()) {
                DebugLog::instance().write(
                    QStringLiteral("restoreUpdateSession invalid selection path=%1")
                        .arg(path));
                return false;
            }
            for (const QJsonValue &idValue : selectedIdsValue.toArray()) {
                bool idOk = false;
                const quint64 id = idValue.toString().toULongLong(&idOk);
                if (!idValue.isString() || !idOk || id == 0) {
                    DebugLog::instance().write(
                        QStringLiteral("restoreUpdateSession invalid selection id path=%1")
                            .arg(path));
                    return false;
                }
                restoredSelectedObjectIds.append(ObjectId::fromValue(id));
            }

            bool primaryIdOk = false;
            const quint64 primaryId =
                root.value(QStringLiteral("primaryObjectId")).toString()
                    .toULongLong(&primaryIdOk);
            if (!primaryIdOk) {
                DebugLog::instance().write(
                    QStringLiteral("restoreUpdateSession invalid primary selection path=%1")
                        .arg(path));
                return false;
            }
            restoredPrimaryObjectId = ObjectId::fromValue(primaryId);

            const QJsonValue activeControlPointValue =
                root.value(QStringLiteral("activeControlPoint"));
            if (!activeControlPointValue.isUndefined()) {
                const QJsonObject activeControlPoint =
                    activeControlPointValue.toObject();
                bool objectIdOk = false;
                const quint64 objectId =
                    activeControlPoint.value(QStringLiteral("objectId"))
                        .toString()
                        .toULongLong(&objectIdOk);
                const QJsonValue indexValue =
                    activeControlPoint.value(QStringLiteral("index"));
                const int index = indexValue.toInt(-1);
                if (!activeControlPointValue.isObject() || !objectIdOk ||
                    objectId == 0 || !indexValue.isDouble() || index < 0) {
                    DebugLog::instance().write(
                        QStringLiteral("restoreUpdateSession invalid active control point path=%1")
                            .arg(path));
                    return false;
                }
                restoredActiveControlPoint = {
                    ObjectId::fromValue(objectId), index};
            }

            const QJsonValue controlPointsValue =
                root.value(QStringLiteral("controlPointsVisible"));
            if (!controlPointsValue.isBool()) {
                DebugLog::instance().write(
                    QStringLiteral("restoreUpdateSession invalid control-point visibility path=%1")
                        .arg(path));
                return false;
            }
            restoredControlPointsVisible = controlPointsValue.toBool();
        }

        if (version >= 3) {
            QString documentError;
            if (!documentFromJson(root.value(QStringLiteral("document")),
                                  &document_,
                                  &documentError)) {
                DebugLog::instance().write(
                    QStringLiteral("restoreUpdateSession invalid document path=%1 error=%2")
                        .arg(path, documentError));
                return false;
            }
        } else {
            const QJsonValue shapesValue = root.value(QStringLiteral("shapes"));
            if (!shapesValue.isArray()) {
                DebugLog::instance().write(QStringLiteral("restoreUpdateSession invalid shapes path=%1")
                                               .arg(path));
                return false;
            }

            QVector<Shape> restoredShapes;
            const QJsonArray shapes = shapesValue.toArray();
            restoredShapes.reserve(shapes.size());
            for (const QJsonValue &shapeValue : shapes) {
                Shape shape{GeometryType::Invalid,
                            {},
                            Shape::NurbsCurve2D{},
                            ArcMode::TwoPoint,
                            0.0,
                            {},
                            {}};
                if (!shapeFromJson(shapeValue, &shape)) {
                    DebugLog::instance().write(QStringLiteral("restoreUpdateSession invalid shape path=%1")
                                                   .arg(path));
                    return false;
                }
                restoredShapes.append(shape);
            }
            shapes_ = restoredShapes;
        }
        normalizeDisconnectedPolyCurveObjects();
        history_.clear();
        pendingPoints_.clear();
        resetArcPreviewTracking();
        selectedShapeIndices_.clear();
        selectedShapeIndex_ = ObjectId::invalid();
        selectionBoxActive_ = false;
        selectionBoxMoved_ = false;
        selectionBoxAdditive_ = false;
        draggingSelected_ = false;
        draggingShapeIndices_.clear();
        draggingControlPoint_ = false;
        controlPointIndex_ = -1;
        dragHistoryRecorded_ = false;
        currentDragSnap_ = DragSnapResult{};
        dragSnapLocked_ = false;
        eraseStrokeActive_ = false;
        eraseCursorPressed_ = false;
        eraseCandidateShapeIndices_.clear();
        eraseStrokeScreenPath_.clear();
        eraseTargetShapeIndices_.clear();
        eraseSceneCurveCaches_.clear();
        eraseTargetCurveCaches_.clear();
        eraseGeometryCachePrepared_ = false;
        trimHoverPositionValid_ = false;
        currentSnap_ = SnapResult{};
        joinActive_ = false;
        joinShapeIndices_.clear();
        resetScaleInteraction();
        resetRotateInteraction();
        resetMirrorInteraction();
        subdivisionActive_ = false;
        subdivisionShapeIndex_ = ObjectId::invalid();
        subdivisionSections_ = 2;
        resetSubdivisionWheelTracking();
        lineCommandActive_ = false;
        activeTool_ = Tool::Select;
        repeatTool_ = Tool::Select;
        pan_ = restoredPan;
        zoom_ = restoredZoom;
        if (version >= 4) {
            viewportTransform_.setGridSpacing(
                documentGridSpacingInMillimeters(document_.settings()));
            viewportTransform_.setCameraPreferences(restoredCameraPreferences);
            viewportTransform_.setWorkPlane(restoredWorkPlane,
                                             restoredWorkPlaneOffset);
            viewportTransform_.setWorkPlaneFrame(restoredWorkPlaneFrame);
            viewportTransform_.setCameraState(restoredCameraState);
            selection_.setObjectIds(restoredSelectedObjectIds,
                                    restoredPrimaryObjectId);
            selection_.prune(document_);
            if (restoredActiveControlPoint.isValid() &&
                selection_.contains(restoredActiveControlPoint.objectId)) {
                selection_.setActiveControlPoint(
                    restoredActiveControlPoint.objectId,
                    restoredActiveControlPoint.index);
            }
            controlPointsVisible_ = restoredControlPointsVisible;
            notifyViewStateChanged();
        }
        setCursor(Qt::ArrowCursor);
        update();
        emitCoordinateUpdate();
        notifyHistoryChanged();
        notifyLayersChanged();

        DebugLog::instance().write(QStringLiteral("restoreUpdateSession path=%1 shapes=%2 layers=%3 version=%4 preset=%5 perspective=%6 zoom=%7 pan=%8")
                                       .arg(path)
                                       .arg(shapes_.size())
                                       .arg(document_.layers().size())
                                       .arg(version)
                                       .arg(static_cast<int>(viewportTransform_.viewPreset()))
                                       .arg(viewportTransform_.isPerspectiveEnabled())
                                       .arg(zoom_, 0, 'g', 17)
                                       .arg(precisePointText(pan_)));
        return true;
    }

    bool saveVignolaDocument(const QString &path, QString *errorMessage) const override
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
        return classiCAD::saveVignolaDocument(path,
                                              document_,
                                              cameraSettings,
                                              errorMessage);
    }

    bool loadVignolaDocument(const QString &path, QString *errorMessage) override
    {
        Document restoredDocument;
        ProjectViewportCameraSettings cameraSettings;
        if (!classiCAD::loadVignolaDocument(path,
                                            &restoredDocument,
                                            &cameraSettings,
                                            errorMessage)) {
            return false;
        }

        if (cameraSettings.storedInProject) {
            const ViewportCameraPreferences preferences{
                cameraSettings.focalLengthMillimeters,
                cameraSettings.clipStart,
                cameraSettings.clipEnd};
            if (!viewportTransform_.setCameraPreferences(preferences)) {
                if (errorMessage != nullptr) {
                    *errorMessage = QStringLiteral(
                        "The project contains invalid viewport camera settings.");
                }
                return false;
            }
        }

        document_ = std::move(restoredDocument);
        normalizeDisconnectedPolyCurveObjects();
        resetForDocumentReplacement();
        viewportTransform_.resetView();
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
        notifyHistoryChanged();
        notifyLayersChanged();
        return true;
    }

    bool importRhino3dmDocument(const QString &path,
                                Rhino3dmImportReport *report,
                                QString *errorMessage) override
    {
        const Document::Snapshot beforeImport = document_.snapshot();
        if (!classiCAD::importRhino3dmDocument(path,
                                              &document_,
                                              report,
                                              errorMessage)) {
            return false;
        }

        normalizeDisconnectedPolyCurveObjects();
        recordGeometrySnapshot(beforeImport);
        update();
        return true;
    }

    void createNewDocument() override
    {
        document_ = Document{};
        resetForDocumentReplacement();
        viewportTransform_.resetView();
        notifyViewStateChanged();
        setCursor(Qt::ArrowCursor);
        update();
        emitCoordinateUpdate();
        notifyHistoryChanged();
        notifyLayersChanged();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        if (gpuSurface_ != nullptr) {
            return;
        }
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        paintViewport(painter, nullptr, nullptr, nullptr, nullptr);
    }

    void resizeEvent(QResizeEvent *event) override
    {
        if (gpuSurface_ != nullptr) {
            gpuSurface_->setGeometry(rect());
        }
        QWidget::resizeEvent(event);
    }

    QVector<Shape> visibleDepthShapes() const
    {
        QVector<Shape> visibleShapes;
        visibleShapes.reserve(shapes_.size());
        for (int index = 0; index < shapes_.size(); ++index) {
            const ObjectId objectId = shapes_.objectIdAt(index);
            if (!document_.isObjectVisible(objectId)) {
                continue;
            }
            Shape depthShape = shapes_[index];
            if (activeTool_ == Tool::Scale && scalePreviewValid_ &&
                scaleShapeIds_.contains(objectId)) {
                scaleShapeGeometry(&depthShape,
                                   scaleBaseWorld_,
                                   scalePreviewAxis_,
                                   scalePreviewFactor_,
                                   scaleMode_);
            } else if (activeTool_ == Tool::Rotate && rotateStep_ == 2 &&
                       rotateShapeIndices_.contains(objectId)) {
                rotateShapeGeometry(&depthShape, rotatePreviewAngle_);
            }
            visibleShapes.append(std::move(depthShape));
        }
        return visibleShapes;
    }

    void paintViewport(QPainter &painter,
                       BlenderGridRenderer *nativeRenderer,
                       ViewportSceneRenderer *sceneRenderer,
                       ViewportSceneRenderer *previewRenderer,
                       ViewportControlPointRenderer *controlPointRenderer)
    {
        invalidateEraseGeometryCacheForView();
        updateAssociativeDimensions(document_, curveSampler_);
        const qreal baseGridStep = documentGridSpacingInMillimeters(document_.settings());
        viewportRenderer_.setGridBaseStep(baseGridStep);
        viewportRenderer_.setGridAppearance(gridAppearance_);
        const QVector<Shape> visibleShapes = visibleDepthShapes();
        QVector<ViewportControlPointHandle> gpuControlPointHandles;
        if (controlPointsVisible_ && controlPointRenderer != nullptr) {
            const QColor handleOutline(QStringLiteral("#77b7e6"));
            const QColor handleFill(QStringLiteral("#263b4b"));
            const QColor activeHandle(QStringLiteral("#f0a45a"));
            for (const int shapeIndex : controlPointShapeIndices()) {
                if (shapeIndex < 0 || shapeIndex >= shapes_.size()) {
                    continue;
                }

                Shape handleShape = shapes_[shapeIndex];
                const ObjectId objectId = shapes_.objectIdAt(shapeIndex);
                if (activeTool_ == Tool::Scale && scalePreviewValid_ &&
                    scaleShapeIds_.contains(objectId)) {
                    scaleShapeGeometry(&handleShape,
                                       scaleBaseWorld_,
                                       scalePreviewAxis_,
                                       scalePreviewFactor_,
                                       scaleMode_);
                } else if (activeTool_ == Tool::Rotate && rotateStep_ == 2 &&
                           rotateShapeIndices_.contains(objectId)) {
                    rotateShapeGeometry(&handleShape, rotatePreviewAngle_);
                }

                const QVector<QPointF> controlPoints =
                    controlPointsForShape(handleShape);
                for (int pointIndex = 0; pointIndex < controlPoints.size();
                     ++pointIndex) {
                    const Point3D world = shapePointToWorld(
                        handleShape, controlPoints[pointIndex]);
                    const bool active = draggingControlPoint_ &&
                                        objectId == selectedShapeIndex_ &&
                                        pointIndex == controlPointIndex_;
                    ViewportControlPointHandle handle;
                    handle.worldPosition = QVector3D(
                        static_cast<float>(world.x),
                        static_cast<float>(world.y),
                        static_cast<float>(world.z));
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
                viewportTransform_, size(), devicePixelRatioF(), visibleShapes,
                baseGridStep, gridAppearance_);
            const qreal devicePixelRatio =
                std::max<qreal>(devicePixelRatioF(), 1.0);
            const QSize scenePixelSize(qRound(size().width() * devicePixelRatio),
                                       qRound(size().height() * devicePixelRatio));
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
                    size(), devicePixelRatioF());
            painter.endNativePainting();
            if (!backgroundDrawn) {
                fillViewportBackground(painter, rect());
            }
        }
        QPainter &scenePainter = nativeRenderer == nullptr
                                     ? rasterScenePainter
                                     : painter;
        QVector<ViewportSceneStroke> gpuStrokes;
        QVector<TransientPreviewStroke> gpuPreviewGeometry;
        QVector<TransientPreviewPicture> gpuPreviewPictures;
        gpuPreviewGeometry.reserve(duplicatePreviewShapes_.size() +
                                   mirrorShapeIndices_.size() + 16);
        gpuPreviewPictures.reserve(shapes_.size() +
                                   duplicatePreviewShapes_.size() +
                                   mirrorShapeIndices_.size() + 1);
        QVector<bool> gpuDuplicatePreviewHandled(duplicatePreviewShapes_.size(), false);
        QVector<ObjectId> gpuMirrorPreviewHandled;
        bool gpuActiveToolPreview = false;
        bool gpuActivePicturePreviewRendered = false;
        int visibleShapeIndex = 0;

        for (int index = 0; index < shapes_.size(); ++index) {
            const ObjectId objectId = shapes_.objectIdAt(index);
            if (!document_.isObjectVisible(objectId)) {
                continue;
            }
            const Shape &visibleShape = visibleShapes[visibleShapeIndex++];
            if (isDimensionGeometryType(shapes_[index].geometryType)) {
                continue;
            }
            QColor layerColor;
            QString layerLineType;
            qreal layerLineWeightMm = 0.0;
            const SceneObject *sceneObject = document_.object(objectId);
            const Layer *objectLayer = sceneObject == nullptr
                                           ? nullptr
                                           : document_.layer(sceneObject->layerId);
            if (objectLayer != nullptr) {
                layerColor = objectLayer->color;
                layerLineType = objectLayer->lineType;
                layerLineWeightMm = objectLayer->lineWeightMm;
            }
            const LayerGpuLinePattern gpuLayerPattern =
                layerGpuLinePattern(layerLineType);
            const bool selected = selectedShapeIndices_.contains(objectId) ||
                                  objectId == selectedShapeIndex_ ||
                                  joinShapeIndices_.contains(objectId);
            const bool scalePreview = activeTool_ == Tool::Scale &&
                                      scalePreviewValid_ &&
                                      scaleShapeIds_.contains(objectId);
            const bool rotatePreview = activeTool_ == Tool::Rotate &&
                                        rotateStep_ == 2 &&
                                        rotateShapeIndices_.contains(objectId);
            const GeometryType geometryType = visibleShape.geometryType;
            const bool gpuStrokeType =
                geometryType == GeometryType::Point ||
                geometryType == GeometryType::Line ||
                geometryType == GeometryType::Rectangle ||
                geometryType == GeometryType::Polygon ||
                geometryType == GeometryType::Circle ||
                geometryType == GeometryType::Ellipse ||
                geometryType == GeometryType::Arc ||
                geometryType == GeometryType::PolyCurve ||
                geometryType == GeometryType::Bezier ||
                geometryType == GeometryType::Nurbs;
            const bool nativeCurve = sceneRenderer != nullptr &&
                                     gpuStrokeType &&
                                     (geometryType == GeometryType::Point ||
                                      selected || scalePreview || rotatePreview ||
                                      gpuLayerPattern.kind !=
                                          LayerGpuLinePatternKind::Unsupported) &&
                                     (geometryType != GeometryType::Arc ||
                                      validateNurbsCurve(visibleShape.nurbs)) &&
                                     (geometryType != GeometryType::Circle ||
                                      validateNurbsCurve(visibleShape.nurbs)) &&
                                     (geometryType != GeometryType::Ellipse ||
                                      validateNurbsCurve(visibleShape.nurbs)) &&
                                     ((geometryType != GeometryType::Bezier &&
                                       geometryType != GeometryType::Nurbs) ||
                                      validateNurbsCurve(visibleShape.nurbs)) &&
                                     (geometryType != GeometryType::PolyCurve ||
                                      !visibleShape.components.isEmpty());
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
                     QColor(QStringLiteral("#5da9e9"))});
                if (selected) {
                    gpuStrokes.append({&gpuPreviewPictures.back().frame,
                                       QColor(QStringLiteral("#5da9e9")),
                                       1.5f,
                                       false});
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
                     QColor(QStringLiteral("#5da9e9"))});
                gpuStrokes.append({&gpuPreviewPictures.back().frame,
                                   QColor(QStringLiteral("#5da9e9")),
                                   1.5f,
                                   false});
                continue;
            }
            if (nativeCurve) {
                const bool highlighted = selected || scalePreview || rotatePreview;
                const qreal storedWidth = layerLineWeightMm > 0.0
                                              ? std::clamp(layerLineWeightMm * 6.0,
                                                           1.0, 10.0)
                                              : 2.0;
                if (geometryType == GeometryType::Bezier ||
                    geometryType == GeometryType::Nurbs) {
                    gpuStrokes.append({&visibleShape,
                                       QColor(QStringLiteral("#8aa7c7")),
                                       1.0f, true});
                }
                ViewportSceneStroke sceneStroke{
                    &visibleShape,
                    highlighted ? QColor(QStringLiteral("#5da9e9"))
                                : layerColor.isValid()
                                      ? layerColor
                                      : QColor(QStringLiteral("#d28b45")),
                    static_cast<float>(highlighted ? 3.5 : storedWidth),
                    false,
                    geometryType == GeometryType::Point
                        ? (highlighted ? 10.0f : 9.0f)
                        : 0.0f};
                if (!highlighted) {
                    sceneStroke.lineStyle = viewportSceneLineStyleForLayerPattern(
                        gpuLayerPattern.kind);
                    sceneStroke.linePatternScale =
                        static_cast<float>(gpuLayerPattern.scale);
                    sceneStroke.linePatternSegmentCount = std::min(
                        static_cast<int>(gpuLayerPattern.segments.size()),
                        static_cast<int>(sceneStroke.linePatternSegmentsWidthUnits.size()));
                    for (int segmentIndex = 0;
                         segmentIndex < sceneStroke.linePatternSegmentCount;
                         ++segmentIndex) {
                        sceneStroke.linePatternSegmentsWidthUnits[
                            static_cast<std::size_t>(segmentIndex)] =
                            static_cast<float>(gpuLayerPattern.segments[segmentIndex]);
                    }
                }
                gpuStrokes.append(sceneStroke);
                continue;
            }
            if (scalePreview) {
                Shape previewShape = shapes_[index];
                scaleShapeGeometry(&previewShape,
                                   scaleBaseWorld_,
                                   scalePreviewAxis_,
                                   scalePreviewFactor_,
                                   scaleMode_);
                drawShape(scenePainter,
                          previewShape,
                          false,
                          true,
                          true,
                          layerColor,
                          layerLineType,
                          layerLineWeightMm);
            } else if (rotatePreview) {
                Shape previewShape = shapes_[index];
                rotateShapeGeometry(&previewShape, rotatePreviewAngle_);
                drawShape(scenePainter,
                          previewShape,
                          false,
                          true,
                          true,
                          layerColor,
                          layerLineType,
                          layerLineWeightMm);
            } else {
                drawShape(scenePainter,
                          shapes_[index],
                          false,
                          selected,
                          true,
                          layerColor,
                          layerLineType,
                          layerLineWeightMm);
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
                [&gpuPreviewGeometry](const Shape &shape,
                                     const QColor &color,
                                     float width,
                                     bool controlGuide,
                                     float pointDiameter,
                                     bool dashed,
                                     bool pointOutline) {
                    const GeometryType type = shape.geometryType;
                    const bool supportedType =
                        type == GeometryType::Point || type == GeometryType::Line ||
                        type == GeometryType::Rectangle || type == GeometryType::Polygon ||
                        type == GeometryType::Circle || type == GeometryType::Ellipse ||
                        type == GeometryType::Arc || type == GeometryType::PolyCurve ||
                        type == GeometryType::Bezier || type == GeometryType::Nurbs;
                    if (!supportedType || isDimensionGeometryType(type) ||
                        type == GeometryType::Picture) {
                        return false;
                    }
                    if ((type == GeometryType::Point && shape.points.isEmpty()) ||
                        ((type == GeometryType::Circle || type == GeometryType::Ellipse ||
                          type == GeometryType::Arc || type == GeometryType::Bezier ||
                          type == GeometryType::Nurbs) &&
                         !validateNurbsCurve(shape.nurbs)) ||
                        (type == GeometryType::PolyCurve && shape.components.isEmpty())) {
                        return false;
                    }
                    gpuPreviewGeometry.append(
                        {shape, color, width, controlGuide, pointDiameter,
                         dashed, pointOutline});
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
                activeTool_ == Tool::Arc || isCircleConstructionTool(activeTool_) ||
                        isEllipseTool(activeTool_) || isPolygonTool(activeTool_) ||
                        isRectangleTool(activeTool_)
                    ? arcPreviewColor
                    : previewColor;
            for (int index = 0; index < duplicatePreviewShapes_.size(); ++index) {
                const Shape &preview = duplicatePreviewShapes_[index];
                if (preview.geometryType == GeometryType::Picture) {
                    addPicturePreview(preview, index, false,
                                      ObjectId::invalid());
                } else {
                    gpuDuplicatePreviewHandled[index] = addPreviewShape(
                        preview, previewColor, 1.5f, false, 0.0f, false, false);
                }
            }

            if (activeTool_ == Tool::Mirror && !pendingPoints_.isEmpty() &&
                cursorValid_) {
                for (const ObjectId objectId : mirrorShapeIndices_) {
                    const int shapeIndex = objectIndex(objectId);
                    if (shapeIndex < 0 || shapeIndex >= shapes_.size() ||
                        !document_.isObjectVisible(objectId)) {
                        continue;
                    }
                    Shape mirroredShape;
                    if (mirrorShapeAcrossLine(shapes_[shapeIndex],
                                              pendingPoints_.first(),
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

            if (controllerPreviewShapeVisible_ &&
                !isDimensionGeometryType(controllerPreviewShape_.geometryType)) {
                gpuActiveToolPreview |= addPreviewShape(
                    controllerPreviewShape_, activeToolPreviewColor,
                    1.5f, false, 0.0f,
                    false, false);
            }
            if (activeTool_ == Tool::PointExtrude) {
                for (const Shape &previewShape : pointExtrudePreviewShapes_) {
                    gpuActiveToolPreview |= addPreviewShape(
                        previewShape, activeToolPreviewColor,
                        1.5f, false, 0.0f, false, false);
                }
            }

            if (isRectangleTool(activeTool_)) {
                const QColor markerColor(QStringLiteral("#101010"));
                for (const ToolPreviewGuide &guide : rectanglePreviewGuides_) {
                    gpuActiveToolPreview |= addSolidPreviewLine(
                        guide.line.p1(), guide.line.p2(), guide.color, 1.5f);
                }
                for (const QPointF &point : pendingPoints_) {
                    gpuActiveToolPreview |= addPreviewPoint(point, markerColor, 5.0f, false);
                }
                if (cursorValid_) {
                    gpuActiveToolPreview |= addPreviewPoint(cursorWorld_, markerColor, 5.0f, false);
                }
                if (controllerPreviewShapeVisible_ &&
                    controllerPreviewShape_.geometryType == GeometryType::Rectangle) {
                    for (const QPointF &point : rectangleVertices(controllerPreviewShape_)) {
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
                for (const Shape &preview : pointPreviewShapes_) {
                    if (preview.geometryType == GeometryType::Point) continue;
                    const bool isArc = preview.geometryType == GeometryType::Arc ||
                                       preview.geometryType == GeometryType::Circle;
                    gpuActiveToolPreview |= addPreviewShape(
                        preview, isArc ? arcColor : pointColor,
                        isArc ? 2.0f : 1.5f, false,
                        0.0f, false, false);
                }
                for (const ToolPreviewGuide &guide : pointPreviewGuides_) {
                    Shape line;
                    line.geometryType = GeometryType::Line;
                    line.points = {guide.line.p1(), guide.line.p2()};
                    line.workPlane = WorkPlane::XY;
                    line.workPlaneOffset = guide.hasWorkPlaneFrame
                        ? guide.workPlaneFrame.origin.z
                        : pointPreviewFrame_.origin.z;
                    line.workPlaneFrame = guide.hasWorkPlaneFrame
                        ? guide.workPlaneFrame : pointPreviewFrame_;
                    gpuActiveToolPreview |= addPreviewShape(
                        line, guide.color, 1.25f, false, 0.0f,
                        guide.dashed, false);
                }
            }

            if (activeTool_ == Tool::Point && cursorValid_) {
                Shape pointPreview;
                if (makeToolShape(Tool::Point,
                                  {cursorWorld_},
                                  arcMode_,
                                  arcPreviewSweepAngle_,
                                  &pointPreview)) {
                    gpuActiveToolPreview |= addPreviewShape(
                        pointPreview, previewColor, 1.5f, false, 9.0f,
                        false, false);
                }
            } else if (activeTool_ == Tool::Line) {
                for (const Shape &preview : linePreviewShapes_) {
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
                                  arcMode_,
                                  arcPreviewSweepAngle_,
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
                if (rotateStep_ == 0) {
                    gpuActiveToolPreview |= addPreviewPoint(
                        cursorWorld_, pointColor, 12.0f, true);
                } else {
                    gpuActiveToolPreview |= addPreviewPoint(
                        rotateBaseWorld_, pointColor, 12.0f, true);
                    if (rotateStep_ >= 1) {
                        gpuActiveToolPreview |= addPreviewLine(
                            rotateBaseWorld_, cursorWorld_, guideColor, 1.0f);
                    }
                    if (rotateStep_ >= 2) {
                        gpuActiveToolPreview |= addPreviewLine(
                            rotateBaseWorld_, rotateReferenceWorld_, rotateColor,
                            1.0f);
                    }
                }
                gpuActiveToolPreview |= addPreviewPoint(
                    cursorWorld_, pointColor, 8.0f, false);
            }

            if (activeTool_ == Tool::Scale && scaleStep_ == 2 && cursorValid_) {
                QPointF guideEnd = cursorWorld_;
                if (scaleMode_ == ScaleMode::OneD && !scaleUsingTypedFactor_) {
                    const QPointF offset = cursorWorld_ - scaleBaseWorld_;
                    guideEnd = scaleBaseWorld_ +
                               scaleAxisDirection_ *
                                   QPointF::dotProduct(offset,
                                                       scaleAxisDirection_);
                }
                const QColor guideColor(QStringLiteral("#8aa7c7"));
                gpuActiveToolPreview |= addPreviewLine(
                    scaleBaseWorld_, guideEnd, guideColor, 1.0f);
                gpuActiveToolPreview |= addPreviewPoint(
                    scaleBaseWorld_, previewColor, 8.0f, true);
            }
        }

        QVector<ViewportSceneStroke> gpuPreviewStrokes;
        gpuPreviewStrokes.reserve(gpuPreviewGeometry.size());
        for (const TransientPreviewStroke &preview : gpuPreviewGeometry) {
            gpuPreviewStrokes.append({&preview.shape,
                                      preview.color,
                                      preview.width,
                                      preview.controlGuide,
                                      preview.pointDiameter,
                                      preview.dashed,
                                      preview.pointOutline});
        }
        gpuPreviewStrokes.reserve(gpuPreviewStrokes.size() +
                                  gpuPreviewPictures.size());

        const bool gpuArcToolPreview =
            activeTool_ == Tool::Arc &&
            (arcMode_ == ArcMode::OnePoint || arcMode_ == ArcMode::TwoPoint ||
             arcMode_ == ArcMode::ThreePoint);
        const qreal arcHudPanelWidth = arcMode_ != ArcMode::OnePoint
                                           ? 750.0
                                           : 570.0;
        const ArcHudDisplay arcHudDisplay =
            arcMode_ == ArcMode::OnePoint
                ? onePointArcHudDisplay()
                : arcMode_ == ArcMode::TwoPoint
                      ? twoPointArcHudDisplay()
                      : threePointArcHudDisplay();
        const QImage arcHudText = nativeRenderer != nullptr &&
                                          gpuArcToolPreview
                                      ? arcHudTextImage(arcHudDisplay,
                                                        devicePixelRatioF(),
                                                        arcHudPanelWidth)
                                      : QImage{};
        bool gpuPreviewRendered = false;
        bool gpuArcOverlayRendered = false;
        QVector<const Shape *> failedScenePicturePreviews;
        if (nativeRenderer != nullptr) {
            painter.beginNativePainting();
            if (sceneRenderer != nullptr) {
                for (const TransientPreviewPicture &preview : gpuPreviewPictures) {
                    if (preview.sceneShapeIndex >= 0 &&
                        !sceneRenderer->drawPicture(preview.image,
                                                    viewportTransform_, size(),
                                                    devicePixelRatioF(),
                                                    preview.opacity)) {
                        failedScenePicturePreviews.append(&preview.image);
                    }
                }
            }
            const bool sceneDrawn = sceneRenderer == nullptr ||
                                    sceneRenderer->draw(gpuStrokes,
                                                        viewportTransform_, size(),
                                                        devicePixelRatioF());
            const bool gridDrawn = nativeRenderer->renderToCurrentFramebuffer(
                viewportTransform_, size(), devicePixelRatioF(),
                visibleShapes, baseGridStep, gridAppearance_);
            if (previewRenderer != nullptr) {
                for (const TransientPreviewPicture &preview : gpuPreviewPictures) {
                    if (preview.sceneShapeIndex >= 0) {
                        continue;
                    }
                    if (!previewRenderer->drawPicture(preview.image,
                                                      viewportTransform_, size(),
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
            gpuPreviewRendered = previewRenderer != nullptr &&
                                 previewRenderer->draw(gpuPreviewStrokes,
                                                       viewportTransform_, size(),
                                                       devicePixelRatioF());
            if (controlPointsVisible_ && controlPointRenderer != nullptr) {
                gpuControlPointsDrawn = controlPointRenderer->draw(
                    gpuControlPointHandles, viewportTransform_, size(),
                    devicePixelRatioF());
            }
            if (gpuArcToolPreview && previewRenderer != nullptr) {
                gpuArcOverlayRendered =
                    previewRenderer->drawArcToolOverlay(
                        viewportTransform_,
                        size(),
                        devicePixelRatioF(),
                        viewportTransform_.workPlaneFrame(),
                        pendingPoints_,
                        cursorWorld_,
                        cursorValid_,
                        arcMode_,
                        arcPreviewSweepAngle_,
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
                                  stroke.color == QColor(QStringLiteral("#5da9e9")),
                                  true, stroke.color);
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
            fillViewportBackground(painter, rect());
            if (gpuViewportBackground.isNull()) {
                drawGrid(painter);
                drawOrigin(painter);
            }
            painter.drawImage(QPoint(0, 0), committedSceneLayer);
            if (!gpuViewportBackground.isNull()) {
                painter.drawImage(QPoint(0, 0), gpuViewportBackground);
            }
        }

        for (int index = 0; index < shapes_.size(); ++index) {
            const ObjectId objectId = shapes_.objectIdAt(index);
            if (!document_.isObjectVisible(objectId)) {
                continue;
            }
            const Shape &shape = shapes_[index];
            if (isDimensionGeometryType(shape.geometryType)) {
                QColor layerColor;
                QString layerLineType;
                qreal layerLineWeightMm = 0.0;
                const SceneObject *sceneObject = document_.object(objectId);
                const Layer *objectLayer = sceneObject == nullptr
                                               ? nullptr
                                               : document_.layer(sceneObject->layerId);
                if (objectLayer != nullptr) {
                    layerColor = objectLayer->color;
                    layerLineType = objectLayer->lineType;
                    layerLineWeightMm = objectLayer->lineWeightMm;
                }
                const bool selected = selectedShapeIndices_.contains(objectId) ||
                                      objectId == selectedShapeIndex_ ||
                                      joinShapeIndices_.contains(objectId);
                drawShape(painter,
                          shape,
                          false,
                          selected,
                          true,
                          layerColor,
                          layerLineType,
                          layerLineWeightMm);
            }
            if (!subdivisionActive_ || objectId != subdivisionShapeIndex_) {
                drawSubdivisionPoints(painter,
                                      shape,
                                      shape.subdivisionParameters,
                                      false);
            }
        }

        if (duplicateActive_) {
            for (int index = 0; index < duplicatePreviewShapes_.size(); ++index) {
                if (!gpuPreviewRendered || !gpuDuplicatePreviewHandled.value(index)) {
                    drawShape(painter, duplicatePreviewShapes_[index], true);
                }
            }
        }

        if (isEraseLikeTool(activeTool_) &&
            (!eraseStrokeScreenPath_.isEmpty() || eraseStrokeActive_)) {
            for (const ObjectId objectId : eraseCandidateShapeIndices_) {
                const int shapeIndex = objectIndex(objectId);
                if (shapeIndex >= 0) {
                    drawEraseCandidatePreview(painter, shapeIndex);
                }
            }
        }

        const int subdivisionIndex = objectIndex(subdivisionShapeIndex_);
        if (subdivisionActive_ && subdivisionIndex >= 0) {
            const QVector<double> previewParameters = subdivisionParametersForSections(
                shapes_[subdivisionIndex], subdivisionSections_);
            drawSubdivisionPoints(painter,
                                  shapes_[subdivisionIndex],
                                  previewParameters,
                                  true);
        }

        if (controlPointsVisible_) {
            for (const int shapeIndex : controlPointShapeIndices()) {
                if (shapeIndex >= 0 && shapeIndex < shapes_.size()) {
                    if (activeTool_ == Tool::Scale && scalePreviewValid_ &&
                        scaleShapeIds_.contains(shapes_.objectIdAt(shapeIndex))) {
                        Shape previewShape = shapes_[shapeIndex];
                        scaleShapeGeometry(&previewShape,
                                           scaleBaseWorld_,
                                           scalePreviewAxis_,
                                           scalePreviewFactor_,
                                           scaleMode_);
                        drawControlPoints(painter, previewShape, shapeIndex,
                                          !gpuControlPointsDrawn);
                    } else if (activeTool_ == Tool::Rotate && rotateStep_ == 2 &&
                        rotateShapeIndices_.contains(shapes_.objectIdAt(shapeIndex))) {
                        Shape previewShape = shapes_[shapeIndex];
                        rotateShapeGeometry(&previewShape,
                                            rotatePreviewAngle_);
                        drawControlPoints(painter, previewShape, shapeIndex,
                                          !gpuControlPointsDrawn);
                    } else {
                        drawControlPoints(painter, shapes_[shapeIndex], shapeIndex,
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
        } else if (activeTool_ == Tool::Scale && scaleStep_ == 2) {
            drawScaleToolGuide(painter,
                               !gpuActiveToolPreview || !gpuPreviewRendered);
        } else if (activeTool_ == Tool::PointExtrude) {
            if (!gpuActiveToolPreview || !gpuPreviewRendered) {
                for (const Shape &previewShape : pointExtrudePreviewShapes_) {
                    drawShape(painter, previewShape, true);
                }
            }
        } else if (controllerPreviewShapeVisible_ &&
                   (isPointCreationTool(activeTool_) ||
                    isCurveCreationTool(activeTool_) ||
                    isTwoCurveLineTool(activeTool_))) {
            if (!gpuActiveToolPreview || !gpuPreviewRendered) {
                drawShape(painter, controllerPreviewShape_, true);
            }
        } else if (isDimensionTool(activeTool_)) {
            if (controllerPreviewShapeVisible_) {
                drawShape(painter, controllerPreviewShape_, true);
            }
            if (currentSnap_.isValid()) {
                drawSnapMarker(painter, currentSnap_.type, currentSnap_.point);
            }
        } else if (isEraseLikeTool(activeTool_)) {
            drawErasePreview(painter);
        } else if (!pendingPoints_.isEmpty()) {
            Shape previewShape{geometryTypeForTool(activeTool_),
                               pendingPoints_,
                               Shape::NurbsCurve2D{},
                               ArcMode::TwoPoint,
                               0.0,
                               {},
                               {}};
            previewShape.workPlane = viewportTransform_.workPlane();
            previewShape.workPlaneOffset = viewportTransform_.workPlaneOffset();
            previewShape.workPlaneFrame = viewportTransform_.workPlaneFrame();
            if (!gpuActiveToolPreview || !gpuPreviewRendered) {
                drawShape(painter, previewShape, true);
            }
        }

        if ((grabActive_ || duplicateActive_ || activeTool_ == Tool::Scale ||
             activeTool_ == Tool::PointExtrude ||
             activeTool_ == Tool::Picture) &&
            currentSnap_.isValid()) {
            drawSnapMarker(painter, currentSnap_.type, currentSnap_.point);
        }
        if ((draggingSelected_ || draggingControlPoint_) && currentDragSnap_.isValid()) {
            drawSnapMarker(painter,
                           currentDragSnap_.type,
                           currentDragSnap_.targetPoint);
        }

        if (selectionBoxActive_) {
            viewportOverlay_.drawSelectionBox(painter,
                                               selectionBoxStartScreen_,
                                               selectionBoxCurrentScreen_);
        }
        viewportOverlay_.drawToolStatus(painter,
                                         size(),
                                         activeTool_,
                                         arcMode_,
                                         subdivisionActive_,
                                         subdivisionSections_,
                                         joinActive_,
                                         joinShapeIndices_.size(),
                                         lineCommandActive_,
                                         toolStatus_.text,
                                         pointHudInstructionsLine_,
                                         rotateStep_,
                                         rotateAngleSnapEnabled_,
                                         rotateAngleInputActive_,
                                         rotateSnapIncrementDegrees(),
                                         rotateToolPreferences_.useRadians,
                                         grabActive_,
                                         grabPickingBasePoint_,
                                         grabHasBasePoint_,
                                         duplicateActive_,
                                         duplicatePickingBasePoint_,
                                         duplicateHasBasePoint_);
        viewportOverlay_.drawBlenderNavigationGizmo(
            painter, size(), navigationHoverPosition_);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        stopNavigationAnimation();
        const QPointF screenPosition = eventPosition(event);
        if (event->button() == Qt::LeftButton) {
            const BlenderNavigationHit hit =
                viewportOverlay_.blenderNavigationGizmoHitAt(screenPosition, size());
            if (hit.action != BlenderNavigationAction::None) {
                navigationPressedAction_ = hit.action;
                navigationPressHit_ = hit;
                navigationPressPosition_ = screenPosition;
                navigationLastPosition_ = screenPosition;
                navigationMoved_ = false;
                if (hit.action == BlenderNavigationAction::Orbit ||
                    hit.action == BlenderNavigationAction::Axis) {
                    beginOrbitAt(screenPosition);
                    if (viewportTransform_.navigationPreferences().orbitMethod ==
                        ViewportOrbitMethod::Trackball) {
                        viewportTransform_.beginOrbitGesture(screenPosition, size());
                    }
                }
                setCursor(hit.action == BlenderNavigationAction::Camera ||
                                  hit.action == BlenderNavigationAction::Projection
                              ? Qt::PointingHandCursor
                              : Qt::OpenHandCursor);
                update();
                event->accept();
                return;
            }
        }
        updateDrawingWorkPlaneFromHover(screenPosition);
        if (activeTool_ == Tool::Arc &&
            (arcMode_ == ArcMode::TwoPoint ||
             arcMode_ == ArcMode::ThreePoint) &&
            pendingPoints_.size() >= 2) {
            updateArcTwoPointWorkPlaneForView();
        } else if (activeTool_ == Tool::Arc &&
                   (arcMode_ == ArcMode::TwoPoint ||
                    arcMode_ == ArcMode::ThreePoint) &&
                   pendingPoints_.size() == 1 &&
                   !(arcMode_ == ArcMode::TwoPoint &&
                     arcPerpendicularPlaneActive_)) {
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
                .arg(inputButtonName(panButton_))
                .arg(static_cast<int>(event->modifiers()), 0, 16)
                .arg(snapTypeName(currentSnap_.type)));

        if (!worldPositionValid && event->button() == Qt::LeftButton &&
            event->button() != panButton_ &&
            !(activeTool_ == Tool::Line && !pendingPoints_.isEmpty()) &&
            activeTool_ != Tool::PointExtrude) {
            event->ignore();
            return;
        }

        if (duplicateActive_) {
            rawCursorWorld_ = rawWorldPosition;
            cursorWorld_ = rawWorldPosition;
            lastWorldPosition_ = rawWorldPosition;
            cursorValid_ = true;
            if (event->button() == Qt::LeftButton) {
                if (duplicatePickingBasePoint_) {
                    const SnapResult baseSnap = findDuplicateBasePointSnap(rawWorldPosition);
                    duplicateBasePoint_ = baseSnap.isValid()
                                              ? baseSnap.point
                                              : rawWorldPosition;
                    duplicateCursorOffset_ = rawWorldPosition - duplicateBasePoint_;
                    duplicateHasBasePoint_ = true;
                    duplicatePickingBasePoint_ = false;
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

        if (grabActive_) {
            rawCursorWorld_ = rawWorldPosition;
            cursorWorld_ = rawWorldPosition;
            lastWorldPosition_ = rawWorldPosition;
            cursorValid_ = true;
            if (event->button() == Qt::LeftButton) {
                if (grabPickingBasePoint_) {
                    const SnapResult baseSnap = findGrabBasePointSnap(rawWorldPosition);
                    if (baseSnap.isValid()) {
                        grabBasePoint_ = baseSnap.point;
                        grabCursorOffset_ = rawWorldPosition - grabBasePoint_;
                        grabHasBasePoint_ = true;
                        grabPickingBasePoint_ = false;
                        currentSnap_ = baseSnap;
                        setCursor(Qt::SizeAllCursor);
                        DebugLog::instance().write(
                            QStringLiteral("grab base-point selected type=%1 point=%2 offset=%3")
                                .arg(snapTypeName(baseSnap.type))
                                .arg(pointText(grabBasePoint_))
                                .arg(pointText(grabCursorOffset_)));
                    } else {
                        DebugLog::instance().write(
                            QStringLiteral("grab base-point click ignored no snap candidate"));
                    }
                    update();
                    emitCoordinateUpdate();
                } else {
                    if (grabHasBasePoint_) {
                        updateGrabPosition(draggingShapeIndices_);
                    }
                    finishGrab();
                }
            } else if (event->button() == Qt::RightButton) {
                cancelGrab();
            }
            return;
        }

        if (subdivisionActive_) {
            if (event->button() == Qt::LeftButton) {
                applySubdivision(subdivisionSections_);
            } else if (event->button() == Qt::RightButton) {
                cancelSubdivisionPreview();
            }
            return;
        }

        // While drawing a connected line, right-click is the command's
        // finish action. This takes priority over right-button panning.
        if (event->button() == Qt::RightButton && activeTool_ == Tool::Line &&
            lineCommandActive_) {
            DebugLog::instance().write(QStringLiteral("mousePress branch=finish-line points=%1")
                                           .arg(pendingPoints_.size()));
            const ToolInput input = makeToolInput(event,
                                                  screenPosition,
                                                  rawWorldPosition,
                                                  worldPosition);
            if (activeToolController_ == nullptr ||
                !activeToolController_->handleMousePress(input, toolContext_)) {
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
            const ToolInput input = makeToolInput(event, screenPosition,
                                                  rawWorldPosition, worldPosition);
            activeToolController_->handleMousePress(input, toolContext_);
            return;
        }

        if (event->button() == Qt::RightButton && isEraseLikeTool(activeTool_)) {
            exitEraseLikeTool();
            return;
        }

        if (event->button() == Qt::RightButton && activeTool_ == Tool::Rotate) {
            cancelRotate();
            return;
        }

        if (event->button() == Qt::RightButton && activeTool_ == Tool::Scale) {
            cancelScale();
            return;
        }

        if (event->button() == Qt::RightButton && activeTool_ == Tool::Mirror) {
            cancelMirror();
            return;
        }

        if (event->button() == Qt::RightButton && activeTool_ == Tool::Picture) {
            cancelPicturePlacement();
            return;
        }

        if (event->button() == Qt::RightButton && activeTool_ == Tool::Arc &&
            (arcMode_ == ArcMode::TwoPoint ||
             arcMode_ == ArcMode::ThreePoint)) {
            if (worldPositionValid && pendingPoints_.size() >= 2) {
                cursorWorld_ = worldPosition;
                lastWorldPosition_ = worldPosition;
                cursorValid_ = true;
                if (arcMode_ == ArcMode::TwoPoint) {
                    finishTwoPointArcAt(worldPosition);
                } else {
                    finishThreePointArcAt(worldPosition);
                }
            }
            if (activeTool_ == Tool::Arc) {
                pendingPoints_.clear();
                setTool(Tool::Select);
                if (commandFinished_) {
                    commandFinished_(Tool::Select);
                }
            }
            update();
            emitCoordinateUpdate();
            return;
        }

        if (event->button() == Qt::RightButton && activeTool_ == Tool::Arc &&
            arcMode_ == ArcMode::OnePoint) {
            if (worldPositionValid && pendingPoints_.size() >= 2) {
                cursorWorld_ = worldPosition;
                lastWorldPosition_ = worldPosition;
                cursorValid_ = true;
                finishOnePointArcAt(worldPosition);
            }
            if (activeTool_ == Tool::Arc) {
                pendingPoints_.clear();
                setTool(Tool::Select);
                if (commandFinished_) {
                    commandFinished_(Tool::Select);
                }
            }
            update();
            emitCoordinateUpdate();
            return;
        }

        const bool middleMouseNavigation = event->button() == Qt::MiddleButton;
        const bool configuredPanButton = event->button() == panButton_;
        const bool altLeftNavigation =
            event->button() == Qt::LeftButton &&
            event->modifiers().testFlag(Qt::AltModifier);
        if (middleMouseNavigation || configuredPanButton || altLeftNavigation) {
            panning_ = true;
            if (middleMouseNavigation) {
                // Blender-style navigation: MMB orbits; Shift+MMB pans.
                orbiting_ = !event->modifiers().testFlag(Qt::ShiftModifier);
            } else {
                orbiting_ = configuredPanButton &&
                            event->modifiers().testFlag(Qt::ShiftModifier);
            }
            if (orbiting_) {
                beginOrbitAt(screenPosition);
                if (viewportTransform_.navigationPreferences().orbitMethod ==
                    ViewportOrbitMethod::Trackball) {
                    viewportTransform_.beginOrbitGesture(screenPosition, size());
                }
            }
            panMoved_ = false;
            panStartPosition_ = screenPosition.toPoint();
            lastMousePosition_ = screenPosition.toPoint();
            DebugLog::instance().write(
                QStringLiteral("mousePress branch=start-%1 at=%2")
                    .arg(orbiting_ ? QStringLiteral("orbit") : QStringLiteral("pan"))
                    .arg(pointText(screenPosition)));
            setCursor(Qt::ClosedHandCursor);
            return;
        }

        if (event->button() == Qt::LeftButton && activeToolController_ != nullptr &&
            activeTool_ != Tool::Select && activeTool_ != Tool::Arc) {
            rawCursorWorld_ = rawWorldPosition;
            lastWorldPosition_ = worldPosition;
            cursorWorld_ = worldPosition;
            cursorValid_ = true;
            const ToolInput input = makeToolInput(event,
                                                  screenPosition,
                                                  rawWorldPosition,
                                                  worldPosition);
            if (activeToolController_->handleMousePress(input, toolContext_)) {
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
            handleRotatePoint(worldPosition);
            emitCoordinateUpdate();
            return;
        }

        if (event->button() == Qt::LeftButton && activeTool_ == Tool::Scale) {
            rawCursorWorld_ = rawWorldPosition;
            cursorWorld_ = worldPosition;
            lastWorldPosition_ = worldPosition;
            cursorValid_ = true;
            handleScalePoint(worldPosition);
            emitCoordinateUpdate();
            return;
        }

        if (event->button() == Qt::LeftButton && activeTool_ == Tool::Mirror) {
            rawCursorWorld_ = rawWorldPosition;
            cursorWorld_ = worldPosition;
            lastWorldPosition_ = worldPosition;
            cursorValid_ = true;
            handleMirrorPoint(worldPosition);
            emitCoordinateUpdate();
            return;
        }

        if (joinActive_ && event->button() == Qt::LeftButton) {
            const int shapeIndex = hitTestShape(screenPosition);
            const ObjectId objectId = shapes_.objectIdAt(shapeIndex);
            if (shapeIndex < 0 || !objectId.isValid() || !isJoinableShape(shapes_[shapeIndex])) {
                notifyJoinStatus(QStringLiteral("Join: click a line or curve"));
                DebugLog::instance().write(QStringLiteral("join click ignored shape=%1")
                                               .arg(shapeIndex));
                return;
            }

            if (joinShapeIndices_.contains(objectId)) {
                notifyJoinStatus(QStringLiteral("Join: curve already selected"));
                return;
            }

            joinShapeIndices_.append(objectId);
            if (!selectedShapeIndices_.contains(objectId)) {
                selectedShapeIndices_.append(objectId);
            }
            selectedShapeIndex_ = objectId;
            DebugLog::instance().write(QStringLiteral("join selected shape=%1 total=%2")
                                           .arg(shapeIndex)
                                           .arg(joinShapeIndices_.size()));
            update();
            if (joinShapeIndices_.size() >= 2) {
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

            eraseCursorPressed_ = true;
            rawCursorWorld_ = rawWorldPosition;
            cursorWorld_ = rawWorldPosition;
            lastWorldPosition_ = rawWorldPosition;
            cursorValid_ = true;
            eraseCursorScreen_ = screenPosition;
            lastEraseScreen_ = screenPosition;
            eraseStrokeScreenPath_.clear();
            eraseStrokeScreenPath_.append(screenPosition);
            eraseCandidateShapeIndices_.clear();
            eraseStrokeActive_ = true;
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
            eraseCursorPressed_ = false;
            rawCursorWorld_ = rawWorldPosition;
            cursorWorld_ = rawWorldPosition;
            lastWorldPosition_ = rawWorldPosition;
            cursorValid_ = true;
            eraseCursorScreen_ = screenPosition;
            eraseCandidateShapeIndices_.clear();
            eraseStrokeScreenPath_.clear();
            trimHoverPositionValid_ = false;
            trimHoverComponentIndex_ = -1;
            beginSelectionBox(screenPosition, false);
            trimBoxSelectionActive_ = true;
            return;
        }

        if (event->button() == Qt::LeftButton && activeTool_ == Tool::Select) {
            rawCursorWorld_ = rawWorldPosition;
            cursorWorld_ = rawWorldPosition;
            lastWorldPosition_ = rawWorldPosition;
            cursorValid_ = true;
            dragGestureStarted_ = false;
            dragStartScreen_ = screenPosition;
            const bool shiftPressed = event->modifiers().testFlag(Qt::ShiftModifier);

            if (!shiftPressed && controlPointsVisible_) {
                int grabbedShapeIndex = -1;
                int grabbedControlPoint = -1;
                if (hitTestSelectedControlPoint(screenPosition,
                                                &grabbedShapeIndex,
                                                &grabbedControlPoint)) {
                    selectedShapeIndex_ = shapes_.objectIdAt(grabbedShapeIndex);
                    draggingControlPoint_ = true;
                    draggingSelected_ = false;
                    selection_.setActiveControlPoint(selectedShapeIndex_, grabbedControlPoint);
                    lastControlPointWorld_ = rawWorldPosition;
                    dragHistoryRecorded_ = false;
                    currentDragSnap_ = DragSnapResult{};
                    dragSnapLocked_ = false;
                    dragAxisLock_ = DragAxisLock::None;
                    setCursor(Qt::SizeAllCursor);
                    DebugLog::instance().write(
                        QStringLiteral("control point drag start shape=%1 index=%2 world=%3")
                            .arg(objectIndex(selectedShapeIndex_))
                            .arg(controlPointIndex_)
                            .arg(pointText(lastControlPointWorld_)));
                    update();
                    emitCoordinateUpdate();
                    return;
                }
            }

            const int clickedShapeIndex = hitTestShape(screenPosition);
            draggingControlPoint_ = false;
            controlPointIndex_ = -1;
            dragHistoryRecorded_ = false;
            currentDragSnap_ = DragSnapResult{};
            dragSnapLocked_ = false;
            dragAxisLock_ = DragAxisLock::None;
            draggingSelected_ = false;

            if (shiftPressed) {
                if (clickedShapeIndex >= 0) {
                    toggleShapeSelection(clickedShapeIndex);
                    DebugLog::instance().write(
                        QStringLiteral("shift selection toggle shape=%1 selected=%2")
                            .arg(clickedShapeIndex)
                            .arg(selectedShapeIndices_.size()));
                    update();
                    emitCoordinateUpdate();
                } else {
                    beginSelectionBox(screenPosition, true);
                }
                return;
            }

            if (clickedShapeIndex >= 0) {
                const ObjectId clickedObjectId = shapes_.objectIdAt(clickedShapeIndex);
                if (!selectedShapeIndices_.contains(clickedObjectId)) {
                    setSingleSelection(clickedShapeIndex);
                } else {
                    // Clicking an already-selected shape starts a group drag
                    // without collapsing the current multi-selection.
                    selectedShapeIndex_ = clickedObjectId;
                }
                draggingShapeIndices_ = selectedShapeIndices_;
                draggingSelected_ = true;
                setFocus(Qt::MouseFocusReason);
                lastDragWorld_ = rawWorldPosition;
                setCursor(Qt::SizeAllCursor);
                DebugLog::instance().write(
                    QStringLiteral("selection hit shape=%1 tool=%2 dragStart=%3")
                        .arg(objectIndex(selectedShapeIndex_))
                        .arg(geometryTypeName(shapes_[objectIndex(selectedShapeIndex_)].geometryType))
                        .arg(pointText(lastDragWorld_)));
            } else {
                draggingSelected_ = false;
                DebugLog::instance().write(QStringLiteral("selection miss at=%1")
                                               .arg(pointText(screenPosition)));
                beginSelectionBox(screenPosition, false);
            }

            update();
            emitCoordinateUpdate();
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
                    recordGeometryChange();
                    shapes_.append(picture);
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

        if (activeTool_ == Tool::Arc &&
            (arcMode_ == ArcMode::TwoPoint ||
             arcMode_ == ArcMode::ThreePoint) &&
            pendingPoints_.size() >= 2) {
            if (arcMode_ == ArcMode::TwoPoint) {
                finishTwoPointArcAt(worldPosition);
            } else {
                finishThreePointArcAt(worldPosition);
            }
            update();
            emitCoordinateUpdate();
            return;
        }

        if (activeTool_ == Tool::Arc && arcMode_ == ArcMode::OnePoint &&
            pendingPoints_.size() == 2) {
            // A click can arrive without a final mouse-move event. Include
            // that last position before saving the arc's unwrapped sweep.
            updateArcPreviewTracking(lastWorldPosition_);
        }

        if (activeTool_ == Tool::Arc && pendingPoints_.isEmpty()) {
            captureArcReferenceForFirstPoint(lastWorldPosition_);
            pendingPoints_.append(lastWorldPosition_);
        } else if (activeTool_ == Tool::Arc &&
                   (arcMode_ == ArcMode::TwoPoint ||
                    arcMode_ == ArcMode::ThreePoint) &&
                   pendingPoints_.size() == 1) {
            arcSecondPointWorld_ = arcResolvedChordPointValid_
                                       ? arcResolvedChordPointWorld_
                                       : workPlaneFramePointToWorld(
                                             lastWorldPosition_,
                                             viewportTransform_.workPlaneFrame());
            arcChordWorldPointsValid_ = arcReferenceFrameValid_;
            if (arcChordWorldPointsValid_) {
                updateArcTwoPointWorkPlaneForView();
                pendingPoints_[0] = worldPointToWorkPlaneFrame(
                    arcFirstPointWorld_, viewportTransform_.workPlaneFrame());
                pendingPoints_.append(worldPointToWorkPlaneFrame(
                    arcSecondPointWorld_, viewportTransform_.workPlaneFrame()));
            } else {
                pendingPoints_.append(lastWorldPosition_);
            }
            arcAxisConstraintKey_ = 0;
            arcResolvedChordPointValid_ = false;
        } else {
            pendingPoints_.append(lastWorldPosition_);
        }

        if (activeTool_ == Tool::Arc && arcMode_ == ArcMode::OnePoint &&
            pendingPoints_.size() == 2) {
            initializeArcPreviewTracking();
        }

        if (pendingPoints_.size() == requiredPoints(activeTool_)) {
            const ArcMode completedArcMode = activeTool_ == Tool::Arc
                                                 ? arcMode_
                                                 : ArcMode::TwoPoint;
            Shape completedShape{geometryTypeForTool(activeTool_),
                                 pendingPoints_,
                                 Shape::NurbsCurve2D{},
                                 completedArcMode,
                                 0.0,
                                 {},
                                 {}};
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
            if (activeTool_ == Tool::Arc && arcMode_ == ArcMode::OnePoint) {
                constexpr qreal twoPi = 6.28318530717958647692;
                completedShape.arcSweep = std::clamp(arcPreviewSweepAngle_,
                                                     -twoPi + 1.0e-6,
                                                     twoPi - 1.0e-6);
                if (completedShape.points.size() >= 3) {
                    const QPointF radiusVector = completedShape.points[1] -
                                                 completedShape.points[0];
                    const qreal radius = std::hypot(radiusVector.x(),
                                                    radiusVector.y());
                    const qreal startAngle = std::atan2(radiusVector.y(),
                                                        radiusVector.x());
                    const qreal endAngle = startAngle + completedShape.arcSweep;
                    completedShape.points[2] = completedShape.points[0] +
                        QPointF(radius * std::cos(endAngle),
                                radius * std::sin(endAngle));
                }
            }
            if (activeTool_ == Tool::Arc) {
                completedShape.nurbs = makeArcNurbsCurve(completedShape);
                if (!isValidNurbsCurve(completedShape.nurbs)) {
                    pendingPoints_.clear();
                    resetArcPreviewTracking();
                    DebugLog::instance().write(
                        QStringLiteral("arc creation rejected: points do not define a valid circular arc"));
                    update();
                    return;
                }
            } else if (activeTool_ == Tool::Bezier || activeTool_ == Tool::Nurbs) {
                completedShape.nurbs = makeBezierNurbs(completedShape.points);
            } else if (activeTool_ == Tool::Circle) {
                completedShape.nurbs = makeCircleNurbs(completedShape.points);
            }
            recordGeometryChange();
            shapes_.append(completedShape);
            QString commitMessage = QStringLiteral("placeholder shape committed tool=%1 points=%2")
                                        .arg(toolName(activeTool_))
                                        .arg(pendingPoints_.size());
            if (activeTool_ == Tool::Arc) {
                commitMessage += QStringLiteral(" mode=%1 p0=%2 p1=%3 p2=%4 sweep=%5")
                                     .arg(arcModeName(completedShape.arcMode))
                                     .arg(pointText(completedShape.points[0]))
                                     .arg(pointText(completedShape.points[1]))
                                     .arg(pointText(completedShape.points[2]))
                                     .arg(completedShape.arcSweep, 0, 'f', 4);
                commitMessage += QStringLiteral(" nurbsDimension=%1 nurbsDegree=%2 nurbsOrder=%3 rational=%4 controlPoints=%5 weights=%6 knots=%7")
                                     .arg(completedShape.nurbs.dimension)
                                     .arg(completedShape.nurbs.degree)
                                     .arg(completedShape.nurbs.order)
                                     .arg(completedShape.nurbs.rational)
                                     .arg(completedShape.nurbs.controlPoints.size())
                                     .arg(completedShape.nurbs.weights.size())
                                     .arg(completedShape.nurbs.knots.size());
            } else if (activeTool_ == Tool::Bezier || activeTool_ == Tool::Nurbs ||
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
        navigationHoverPosition_ = screenPosition;
        if (navigationPressedAction_ != BlenderNavigationAction::None &&
            (event->buttons() & Qt::LeftButton) != 0) {
            const QPointF delta = screenPosition - navigationLastPosition_;
            if (QLineF(navigationPressPosition_, screenPosition).length() >= 3.0) {
                navigationMoved_ = true;
            }
            if (navigationMoved_) {
                setCursor(Qt::ClosedHandCursor);
                const ViewportViewPreset previousPreset = viewportTransform_.viewPreset();
                const bool previousPerspective =
                    viewportTransform_.isPerspectiveEnabled();
                switch (navigationPressedAction_) {
                case BlenderNavigationAction::Orbit:
                case BlenderNavigationAction::Axis:
                    if (viewportTransform_.navigationPreferences().orbitMethod ==
                        ViewportOrbitMethod::Trackball) {
                        viewportTransform_.orbitToPosition(screenPosition, size());
                    } else {
                        viewportTransform_.orbitByPixels(delta);
                    }
                    break;
                case BlenderNavigationAction::Zoom: {
                    const ViewportNavigationPreferences preferences =
                        viewportTransform_.navigationPreferences();
                    qreal zoomDelta = preferences.zoomAxis == ViewportZoomAxis::Vertical
                                          ? delta.y()
                                          : delta.x();
                    if (preferences.invertMouseZoom) {
                        zoomDelta = -zoomDelta;
                    }
                    const qreal factor = std::exp(-zoomDelta * 0.012);
                    viewportTransform_.zoomAt(QPointF(width() * 0.5,
                                                      height() * 0.5),
                                              factor,
                                              size());
                    break;
                }
                case BlenderNavigationAction::Pan:
                    viewportTransform_.panByPixels(delta, size());
                    break;
                case BlenderNavigationAction::Camera:
                case BlenderNavigationAction::Projection:
                case BlenderNavigationAction::None:
                    break;
                }
                if (previousPreset != viewportTransform_.viewPreset() ||
                    previousPerspective !=
                        viewportTransform_.isPerspectiveEnabled()) {
                    notifyViewStateChanged();
                }
                update();
                emitCoordinateUpdate();
            }
            navigationLastPosition_ = screenPosition;
            update();
            event->accept();
            return;
        }
        if (panning_) {
            const QPoint current = screenPosition.toPoint();
            const QPoint delta = current - lastMousePosition_;
            const QPoint totalDelta = current - panStartPosition_;
            if (std::hypot(totalDelta.x(), totalDelta.y()) >= 3.0) {
                panMoved_ = true;
            }
            if (orbiting_) {
                const ViewportViewPreset previousPreset = viewportTransform_.viewPreset();
                const bool previousPerspective =
                    viewportTransform_.isPerspectiveEnabled();
                if (viewportTransform_.navigationPreferences().orbitMethod ==
                    ViewportOrbitMethod::Trackball) {
                    viewportTransform_.orbitToPosition(screenPosition, size());
                } else {
                    viewportTransform_.orbitByPixels(QPointF(delta));
                }
                if (previousPreset != viewportTransform_.viewPreset() ||
                    previousPerspective !=
                        viewportTransform_.isPerspectiveEnabled()) {
                    notifyViewStateChanged();
                }
            } else {
                viewportTransform_.panByPixels(QPointF(delta), size());
            }
            lastMousePosition_ = current;
            update();
            emitCoordinateUpdate();
            return;
        }
        const BlenderNavigationHit navigationHit =
            viewportOverlay_.blenderNavigationGizmoHitAt(screenPosition, size());
        if (navigationHit.action != BlenderNavigationAction::None) {
            setToolTip(navigationTooltip(navigationHit));
            setCursor(navigationHit.action == BlenderNavigationAction::Camera ||
                              navigationHit.action == BlenderNavigationAction::Projection
                          ? Qt::PointingHandCursor
                          : Qt::OpenHandCursor);
            update();
            return;
        }
        setToolTip(QString());
        if (!grabActive_ && !duplicateActive_) {
            setCursor(activeTool_ == Tool::Select ? Qt::ArrowCursor : Qt::CrossCursor);
        }
        eraseCursorScreen_ = screenPosition;
        updateDrawingWorkPlaneFromHover(screenPosition);
        if (activeTool_ == Tool::Arc &&
            (arcMode_ == ArcMode::TwoPoint ||
             arcMode_ == ArcMode::ThreePoint) &&
            pendingPoints_.size() >= 2) {
            updateArcTwoPointWorkPlaneForView();
        } else if (activeTool_ == Tool::Arc &&
                   (arcMode_ == ArcMode::TwoPoint ||
                    arcMode_ == ArcMode::ThreePoint) &&
                   pendingPoints_.size() == 1 &&
                   !(arcMode_ == ArcMode::TwoPoint &&
                     arcPerpendicularPlaneActive_)) {
            restoreArcChordReferencePlaneForEndpointPick();
        }
        if (!viewportTransform_.screenToWorkPlane(screenPosition,
                                                  size(),
                                                  viewportTransform_.workPlaneFrame(),
                                                  &rawCursorWorld_)) {
            cursorValid_ = false;
            currentSnap_ = SnapResult{};
            if (activeToolController_ != nullptr &&
                ((activeTool_ == Tool::Line && !pendingPoints_.isEmpty()) ||
                 activeTool_ == Tool::PointExtrude)) {
                const ToolInput input = makeToolInput(event, screenPosition,
                                                      lastWorldPosition_, lastWorldPosition_);
                activeToolController_->handleMouseMove(input, toolContext_);
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
            const ToolInput input = makeToolInput(event,
                                                  screenPosition,
                                                  rawCursorWorld_,
                                                  cursorWorld_);
            activeToolController_->handleMouseMove(input, toolContext_);
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

        if (!panning_ && activeTool_ == Tool::Arc && arcMode_ == ArcMode::OnePoint &&
            pendingPoints_.size() >= 2) {
            updateArcPreviewTracking(cursorWorld_);
        }

        if (selectionBoxActive_) {
            selectionBoxCurrentScreen_ = screenPosition;
            const QPointF totalDelta = screenPosition - selectionBoxStartScreen_;
            if (std::hypot(totalDelta.x(), totalDelta.y()) >= 3.0) {
                selectionBoxMoved_ = true;
            }
            if (trimBoxSelectionActive_) {
                updateTrimBoxPreview();
            }
            update();
            emitCoordinateUpdate();
            return;
        }

        if (eraseStrokeActive_) {
            eraseAlongScreenSegment(lastEraseScreen_, screenPosition);
            lastEraseScreen_ = screenPosition;
            if (eraseStrokeScreenPath_.isEmpty() ||
                eraseStrokeScreenPath_.back() != screenPosition) {
                eraseStrokeScreenPath_.append(screenPosition);
            }
            updateErasePreviewIntervals(false);
            update();
            emitCoordinateUpdate();
            return;
        }

        if (activeTool_ == Tool::Trim && !panning_) {
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
            if (!panning_ && rotateStep_ == 1) {
                updateRotateReferencePreview(cursorWorld_);
            } else if (!panning_ && rotateStep_ == 2) {
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

        if (duplicateActive_) {
            if (duplicatePickingBasePoint_) {
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

        if (grabActive_ && grabPickingBasePoint_) {
            currentSnap_ = findGrabBasePointSnap(rawCursorWorld_);
            cursorWorld_ = currentSnap_.isValid() ? currentSnap_.point : rawCursorWorld_;
            lastWorldPosition_ = cursorWorld_;
            update();
            emitCoordinateUpdate();
            return;
        }

        if ((draggingSelected_ || draggingControlPoint_) && !dragGestureStarted_) {
            constexpr qreal dragStartThresholdPixels = 4.0;
            const QPointF screenDelta = screenPosition - dragStartScreen_;
            if (std::hypot(screenDelta.x(), screenDelta.y()) >= dragStartThresholdPixels) {
                dragGestureStarted_ = true;
            }
        }

        const int selectedIndex = objectIndex(selectedShapeIndex_);
        if (draggingControlPoint_ && dragGestureStarted_ && selectedIndex >= 0 &&
            controlPointIndex_ >= 0) {
            const QPointF previousControlPointCursorWorld = lastControlPointWorld_;
            const QPointF cursorStepScreen =
                screenPosition - worldToScreen(previousControlPointCursorWorld);
            const QPointF delta = rawCursorWorld_ - lastControlPointWorld_;
            if (!qFuzzyIsNull(delta.x()) || !qFuzzyIsNull(delta.y())) {
                qint64 snapEvaluationMicroseconds = -1;
                const qreal cursorDistanceFromSnap =
                    std::hypot(screenPosition.x() - worldToScreen(dragSnapCursorWorld_).x(),
                               screenPosition.y() - worldToScreen(dragSnapCursorWorld_).y());

                if (dragSnapLocked_ &&
                    cursorDistanceFromSnap <= kDragSnapBreakawayPixels) {
                    DebugLog::instance().write(
                        QStringLiteral("control point snap-hold shape=%1 index=%2 cursorDistance=%3 breakaway=%4")
                            .arg(selectedIndex)
                            .arg(controlPointIndex_)
                            .arg(cursorDistanceFromSnap, 0, 'f', 2)
                            .arg(kDragSnapBreakawayPixels, 0, 'f', 2));
                } else if (dragSnapLocked_) {
                    const QPointF detachDelta = rawCursorWorld_ - dragSnapCursorWorld_;
                    beginDragHistory();
                    translateControlPoint(selectedShapeIndex_, controlPointIndex_, detachDelta);
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

                    const QVector<QPointF> controlPoints =
                        controlPointsForShape(shapes_[selectedIndex]);
                    if (controlPointIndex_ < controlPoints.size()) {
                        QElapsedTimer snapTimer;
                        snapTimer.start();
                        currentDragSnap_ = findControlPointSnap(
                            selectedShapeIndex_,
                            controlPointIndex_,
                            controlPoints[controlPointIndex_]);
                        snapEvaluationMicroseconds = snapTimer.nsecsElapsed() / 1000;
                        if (currentDragSnap_.isValid()) {
                            translateControlPoint(selectedShapeIndex_,
                                                  controlPointIndex_,
                                                  currentDragSnap_.translation);
                            dragSnapLocked_ = true;
                            dragSnapCursorWorld_ = rawCursorWorld_;
                            DebugLog::instance().write(
                                QStringLiteral("control point snapped shape=%1 index=%2 type=%3 target=%4")
                                    .arg(selectedIndex)
                                    .arg(controlPointIndex_)
                                    .arg(snapTypeName(currentDragSnap_.type))
                                    .arg(pointText(currentDragSnap_.targetPoint)));
                        }
                    }
                }

                lastControlPointWorld_ = rawCursorWorld_;
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
        } else if (draggingSelected_ && dragGestureStarted_ && selectedIndex >= 0) {
            const QVector<ObjectId> dragIndices = draggingShapeIndices_.isEmpty()
                                                     ? QVector<ObjectId>{selectedShapeIndex_}
                                                     : draggingShapeIndices_;
            if (grabActive_) {
                updateGrabPosition(dragIndices);
            } else {
                const bool groupDrag = dragIndices.size() > 1;
                const QPointF previousDragCursorWorld = lastDragWorld_;
                const QPointF cursorStepScreen =
                    screenPosition - worldToScreen(previousDragCursorWorld);
                const QPointF rawDelta = rawCursorWorld_ - lastDragWorld_;
                const QPointF delta = constrainDragDelta(rawDelta);
                if (!qFuzzyIsNull(rawDelta.x()) || !qFuzzyIsNull(rawDelta.y())) {
                    qint64 snapEvaluationMicroseconds = -1;
                    if (dragAxisLock_ != DragAxisLock::None) {
                        if (!qFuzzyIsNull(delta.x()) || !qFuzzyIsNull(delta.y())) {
                            beginDragHistory();
                            translateShapes(dragIndices, delta);
                        }
                        // Axis locking takes priority over object snapping so the
                        // move remains exactly horizontal or vertical.
                        currentDragSnap_ = DragSnapResult{};
                        dragSnapLocked_ = false;
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
                            translateShapes(dragIndices, delta);
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
                                nearDragFreeSourcePointValid_ = false;
                            }
                        } else {
                            const qreal cursorDistanceFromSnap =
                                std::hypot(
                                    screenPosition.x() -
                                        worldToScreen(dragSnapCursorWorld_).x(),
                                    screenPosition.y() -
                                        worldToScreen(dragSnapCursorWorld_).y());

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
                                const QPointF detachDelta = rawCursorWorld_ - dragSnapCursorWorld_;
                                beginDragHistory();
                                translateShapes(dragIndices, detachDelta);
                                currentDragSnap_ = DragSnapResult{};
                                dragSnapLocked_ = false;
                                DebugLog::instance().write(
                                    QStringLiteral("selection drag snap-breakaway shape=%1 cursorDistance=%2")
                                        .arg(selectedIndex)
                                        .arg(cursorDistanceFromSnap, 0, 'f', 2));
                            } else {
                                beginDragHistory();
                                translateShapes(dragIndices, delta);
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

                    lastDragWorld_ = rawCursorWorld_;

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
            rectanglePreviewActive || polygonPreviewActive || mirrorPreviewActive || panning_ ||
            draggingSelected_ ||
            draggingControlPoint_) {
            update();
        }

        if (pointPreviewActive || picturePreviewActive || lineCommandActive_ ||
            arcPreviewActive || circlePreviewActive ||
            tangentCirclePreviewActive || ellipsePreviewActive ||
            rectanglePreviewActive || polygonPreviewActive || mirrorPreviewActive ||
            activeTool_ == Tool::Erase || panning_ ||
            draggingSelected_ || draggingControlPoint_) {
            DebugLog::instance().write(
                QStringLiteral("mouseMove screen=%1 worldRaw=%2 worldUsed=%3 lineActive=%4 points=%5 panning=%6 dragging=%7 ortho=%8 pan=%9 zoom=%10 buttons=0x%11 arcMode=%12 arcSweep=%13 snap=%14")
                    .arg(pointText(screenPosition))
                    .arg(pointText(rawCursorWorld_))
                    .arg(pointText(cursorWorld_))
                    .arg(lineCommandActive_)
                    .arg(pendingPoints_.size())
                    .arg(panning_)
                    .arg(draggingSelected_)
                    .arg(orthoEnabled_)
                    .arg(pointText(pan_))
                    .arg(zoom_, 0, 'f', 4)
                    .arg(static_cast<int>(event->buttons()), 0, 16)
                    .arg(activeTool_ == Tool::Arc ? arcModeName(arcMode_) : QStringLiteral("None"))
                    .arg(arcPreviewSweepAngle_, 0, 'f', 4)
                    .arg(snapTypeName(currentSnap_.type)));
        }

        emitCoordinateUpdate();
    }

    void leaveEvent(QEvent *event) override
    {
        navigationHoverPosition_ = QPointF(-1000.0, -1000.0);
        if (navigationPressedAction_ == BlenderNavigationAction::None) {
            setToolTip(QString());
        }
        update();
        QWidget::leaveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton &&
            navigationPressedAction_ != BlenderNavigationAction::None) {
            const BlenderNavigationAction action = navigationPressedAction_;
            const BlenderNavigationHit hit = navigationPressHit_;
            const QPointF releasePosition = eventPosition(event);
            viewportTransform_.endOrbitGesture();
            if (!navigationMoved_) {
                switch (action) {
                case BlenderNavigationAction::Axis: {
                    Point3D targetDirection = hit.direction;
                    if (directionDot(viewportTransform_.viewDirection(),
                                     targetDirection) > 0.999) {
                        targetDirection.x = -targetDirection.x;
                        targetDirection.y = -targetDirection.y;
                        targetDirection.z = -targetDirection.z;
                    }
                    animateCameraChange([this, targetDirection]() {
                        viewportTransform_.setViewDirection(targetDirection);
                    });
                    break;
                }
                case BlenderNavigationAction::Projection:
                    viewportTransform_.setPerspectiveEnabled(
                        !viewportTransform_.isPerspectiveEnabled());
                    notifyViewStateChanged();
                    update();
                    emitCoordinateUpdate();
                    break;
                case BlenderNavigationAction::Camera:
                    QToolTip::showText(
                        mapToGlobal(releasePosition.toPoint()),
                        QStringLiteral("No active scene camera is available."),
                        this);
                    break;
                case BlenderNavigationAction::Orbit:
                case BlenderNavigationAction::Zoom:
                case BlenderNavigationAction::Pan:
                case BlenderNavigationAction::None:
                    break;
                }
            }
            navigationPressedAction_ = BlenderNavigationAction::None;
            navigationPressHit_ = {};
            navigationMoved_ = false;
            const BlenderNavigationHit hoverHit =
                viewportOverlay_.blenderNavigationGizmoHitAt(releasePosition, size());
            if (hoverHit.action == BlenderNavigationAction::None) {
                setCursor(activeTool_ == Tool::Select ? Qt::ArrowCursor
                                                      : Qt::CrossCursor);
                setToolTip(QString());
            }
            update();
            event->accept();
            return;
        }

        const bool repeatToolOnRelease =
            panning_ && event->button() == panButton_ &&
            event->button() != Qt::MiddleButton && !panMoved_ &&
            activeTool_ == Tool::Select && repeatTool_ != Tool::Select;

        DebugLog::instance().write(QStringLiteral("mouseRelease button=%1 screen=%2 panningBefore=%3 panMoved=%4 draggingBefore=%5 repeat=%6")
                                       .arg(inputButtonName(event->button()))
                                       .arg(pointText(eventPosition(event)))
                                       .arg(panning_)
                                       .arg(panMoved_)
                                       .arg(draggingSelected_)
                                       .arg(repeatToolOnRelease));
        if (grabActive_) {
            return;
        }

        const bool releaseEraseCursor =
            isEraseLikeTool(activeTool_) && event->button() == Qt::LeftButton;
        if (releaseEraseCursor) {
            eraseCursorPressed_ = false;
        }
        if (panning_ && (event->button() == panButton_ ||
                         event->button() == Qt::MiddleButton ||
                         event->button() == Qt::LeftButton)) {
            if (orbiting_) {
                viewportTransform_.endOrbitGesture();
            }
            panning_ = false;
            orbiting_ = false;
            setCursor(activeTool_ == Tool::Select ? Qt::ArrowCursor : Qt::CrossCursor);
            DebugLog::instance().write(QStringLiteral("mouseRelease branch=end-pan-orbit"));
        }
        panMoved_ = false;

        if (repeatToolOnRelease) {
            DebugLog::instance().write(QStringLiteral("mouseRelease branch=repeat-tool"));
            repeatLastTool();
        }

        if (eraseStrokeActive_ && event->button() == Qt::LeftButton) {
            applyEraseCandidates();
            eraseStrokeActive_ = false;
            eraseCandidateShapeIndices_.clear();
            eraseStrokeScreenPath_.clear();
            eraseTargetShapeIndices_.clear();
            eraseSceneCurveCaches_.clear();
            eraseTargetCurveCaches_.clear();
            eraseGeometryCachePrepared_ = false;
            setCursor(Qt::CrossCursor);
            DebugLog::instance().write(QStringLiteral("mouseRelease branch=end-erase-stroke"));
            update();
            return;
        }

        if (selectionBoxActive_ && event->button() == Qt::LeftButton) {
            if (trimBoxSelectionActive_) {
                selectionBoxCurrentScreen_ = eventPosition(event);
                const QPointF totalDelta =
                    selectionBoxCurrentScreen_ - selectionBoxStartScreen_;
                if (std::hypot(totalDelta.x(), totalDelta.y()) >= 3.0) {
                    selectionBoxMoved_ = true;
                }
                finishTrimBoxSelection();
            } else {
                finishSelectionBox();
            }
            return;
        }

        if (draggingControlPoint_ && event->button() == Qt::LeftButton) {
            draggingControlPoint_ = false;
            dragGestureStarted_ = false;
            controlPointIndex_ = -1;
            dragHistoryRecorded_ = false;
            currentDragSnap_ = DragSnapResult{};
            dragSnapLocked_ = false;
            dragAxisLock_ = DragAxisLock::None;
            setCursor(activeTool_ == Tool::Select ? Qt::ArrowCursor : Qt::CrossCursor);
            DebugLog::instance().write(QStringLiteral("mouseRelease branch=end-control-point-drag shape=%1")
                                           .arg(objectIndex(selectedShapeIndex_)));
            update();
        } else if (draggingSelected_ && event->button() == Qt::LeftButton) {
            draggingSelected_ = false;
            dragGestureStarted_ = false;
            draggingShapeIndices_.clear();
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
        stopNavigationAnimation();
        if (subdivisionActive_) {
            const int angleDelta = event->angleDelta().y();
            const int pixelDelta = event->pixelDelta().y();
            const int logicalSteps = subdivisionWheelStepsFromEvent(angleDelta, pixelDelta);
            applySubdivisionWheelSteps(logicalSteps);

            DebugLog::instance().write(
                QStringLiteral("subdivision wheel angleDelta=%1 pixelDelta=%2 phase=%3 logicalSteps=%4 angleRemainder=%5 pixelRemainder=%6 sections=%7")
                    .arg(angleDelta)
                    .arg(pixelDelta)
                    .arg(static_cast<int>(event->phase()))
                    .arg(logicalSteps)
                    .arg(subdivisionWheelAccumulator_)
                    .arg(subdivisionPixelAccumulator_, 0, 'f', 2)
                    .arg(subdivisionSections_));
            event->accept();
            return;
        }

        if (isPolygonTool(activeTool_) && activeToolController_ != nullptr) {
            ToolInput input;
            input.workPlaneFrame = viewportTransform_.workPlaneFrame();
            input.screenPosition = eventPosition(event);
            input.viewportSize = size();
            input.modifiers = event->modifiers();
            input.wheelAngleDelta = event->angleDelta().y();
            input.wheelPixelDelta = event->pixelDelta().y();
            if (activeToolController_->handleWheel(input, toolContext_)) {
                update();
                emitCoordinateUpdate();
            }
            event->accept();
            return;
        }

        const QPointF screenPosition = eventPosition(event);
        const QPointF beforeZoom = screenToWorld(screenPosition);
        const qreal oldZoom = zoom_;
        const int pixelDelta = event->pixelDelta().y();
        const int angleDelta = event->angleDelta().y();
        qreal wheelSteps = viewportWheelStepsFromDeltas(angleDelta,
                                                        pixelDelta);
        if (wheelSteps == 0.0) {
            event->accept();
            return;
        }
        if (viewportTransform_.navigationPreferences().invertZoomWheel) {
            wheelSteps = -wheelSteps;
        }
        wheelSteps = std::clamp(wheelSteps, -24.0, 24.0);
        // Blender's view_zoom_apply_step uses a 1.2 distance ratio per notch.
        const qreal factor = std::exp(std::log(1.2) * wheelSteps);
        viewportTransform_.zoomAt(screenPosition, factor, size());

        const QPointF afterZoom = screenToWorld(screenPosition);

        DebugLog::instance().write(QStringLiteral("wheel screen=%1 angleDeltaY=%2 pixelDeltaY=%3 zoom=%4->%5 worldBefore=%6 worldAfter=%7 pan=%8")
                                       .arg(pointText(screenPosition))
                                       .arg(angleDelta)
                                       .arg(pixelDelta)
                                       .arg(oldZoom, 0, 'f', 4)
                                       .arg(zoom_, 0, 'f', 4)
                                       .arg(pointText(beforeZoom))
                                       .arg(pointText(afterZoom))
                                       .arg(pointText(pan_)));

        update();
        emitCoordinateUpdate();
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
        stopNavigationAnimation();
        DebugLog::instance().write(QStringLiteral("keyPress key=%1 text=%2 tool=%3 lineActive=%4 points=%5")
                                       .arg(event->key())
                                       .arg(event->text())
                                       .arg(toolName(activeTool_))
                                       .arg(lineCommandActive_)
                                       .arg(pendingPoints_.size()));
        if (selectionBoxActive_ && event->key() == Qt::Key_Escape) {
            cancelSelectionBox();
            return;
        }

        if (activeTool_ == Tool::Picture && event->key() == Qt::Key_Escape) {
            cancelPicturePlacement();
            return;
        }

        if (joinActive_ &&
            (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
            applyJoin();
            return;
        }

        if (joinActive_ && event->key() == Qt::Key_Escape) {
            cancelJoinMode();
            return;
        }

        if (subdivisionActive_ &&
            (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
            applySubdivision(subdivisionSections_);
            return;
        }

        if (subdivisionActive_ && event->key() == Qt::Key_Escape) {
            cancelSubdivisionPreview();
            return;
        }

        if (activeTool_ == Tool::Rotate && event->key() == Qt::Key_Escape) {
            cancelRotate();
            return;
        }

        if (activeTool_ == Tool::Rotate && handleRotateKey(event)) {
            return;
        }

        if (activeTool_ == Tool::Scale && event->key() == Qt::Key_Escape) {
            cancelScale();
            return;
        }

        if (activeTool_ == Tool::Scale &&
            (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
            if (scaleStep_ == 0) {
                handleScalePoint(scaleSelectionBoundsCenter());
            } else if (scaleStep_ == 1 && !scaleFactorText_.isEmpty()) {
                bool validFactor = false;
                const qreal factor = scaleFactorText_.toDouble(&validFactor);
                if (validFactor && std::isfinite(factor)) {
                    scaleFactorText_.clear();
                    if (scaleMode_ == ScaleMode::OneD) {
                        scaleUsingTypedFactor_ = true;
                        scaleTypedFactor_ = factor;
                        scaleStep_ = 2;
                        updateScalePreview(cursorWorld_);
                        publishScalePrompt();
                        update();
                    } else {
                        commitScale(factor, QPointF());
                    }
                }
            } else if (scaleStep_ == 2) {
                updateScalePreview(cursorWorld_);
                if (scalePreviewValid_) {
                    commitScale(scalePreviewFactor_, scalePreviewAxis_);
                }
            }
            event->accept();
            return;
        }

        if (activeTool_ == Tool::Scale && scaleStep_ == 1) {
            if (event->key() == Qt::Key_Backspace) {
                scaleFactorText_.chop(1);
                publishScalePrompt();
                event->accept();
                return;
            }
            QString typedText = event->text();
            if (typedText == QStringLiteral(",")) {
                typedText = QStringLiteral(".");
            }
            if (typedText.size() == 1) {
                const QChar character = typedText.front();
                const bool digit = character.isDigit();
                const bool decimal = character == QLatin1Char('.') &&
                                     !scaleFactorText_.contains(QLatin1Char('.'));
                const bool sign = character == QLatin1Char('-') &&
                                  scaleFactorText_.isEmpty();
                if (digit || decimal || sign) {
                    scaleFactorText_.append(character);
                    publishScalePrompt();
                    event->accept();
                    return;
                }
            }
        }

        if (activeTool_ == Tool::Mirror && event->key() == Qt::Key_Escape) {
            cancelMirror();
            return;
        }

        if (grabActive_ && event->key() == Qt::Key_Escape) {
            cancelGrab();
            return;
        }

        if (duplicateActive_ && event->key() == Qt::Key_Escape) {
            cancelDuplicate();
            return;
        }

        if (grabActive_ && !event->isAutoRepeat() &&
            event->modifiers() == Qt::NoModifier && event->key() == Qt::Key_B) {
            beginGrabBasePointMode();
            return;
        }

        if (activeTool_ == Tool::Select && !grabActive_ &&
            !event->isAutoRepeat() && event->modifiers() == Qt::NoModifier &&
            event->key() == Qt::Key_G) {
            beginGrab();
            return;
        }

        if (activeTool_ == Tool::Select && draggingSelected_ &&
            !event->isAutoRepeat() && event->modifiers() == Qt::NoModifier &&
            (event->key() == Qt::Key_X || event->key() == Qt::Key_Y)) {
            const DragAxisLock requestedLock = event->key() == Qt::Key_X
                                                   ? DragAxisLock::X
                                                   : DragAxisLock::Y;
            dragAxisLock_ = dragAxisLock_ == requestedLock
                                ? DragAxisLock::None
                                : requestedLock;
            currentDragSnap_ = DragSnapResult{};
            dragSnapLocked_ = false;
            if (grabActive_) {
                updateGrabPosition(draggingShapeIndices_.isEmpty()
                                       ? QVector<ObjectId>{selectedShapeIndex_}
                                       : draggingShapeIndices_);
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

        if (activeTool_ == Tool::Arc && event->key() == Qt::Key_Escape) {
            setTool(Tool::Select);
            if (commandFinished_) {
                commandFinished_(Tool::Select);
            }
            update();
            event->accept();
            return;
        }

        if (activeTool_ == Tool::Arc && arcMode_ == ArcMode::TwoPoint &&
            handleTwoPointArcKey(event)) {
            return;
        }

        if (activeTool_ == Tool::Arc && arcMode_ == ArcMode::ThreePoint &&
            handleThreePointArcKey(event)) {
            return;
        }

        if (activeTool_ == Tool::Arc && arcMode_ == ArcMode::OnePoint &&
            handleOnePointArcKey(event)) {
            return;
        }

        if (activeTool_ == Tool::Arc && !event->isAutoRepeat() &&
            event->modifiers() == Qt::NoModifier &&
            (event->key() == Qt::Key_X || event->key() == Qt::Key_Y ||
             event->key() == Qt::Key_Z)) {
            handleArcAxisKey(event->key());
            event->accept();
            update();
            emitCoordinateUpdate();
            return;
        }

        if (activeToolController_ != nullptr) {
            if (activeTool_ == Tool::Arc && !pendingPoints_.isEmpty() &&
                event->key() == Qt::Key_L) {
                event->accept();
                return;
            }
            const ToolInput input = makeKeyToolInput(event);
            if (activeToolController_->handleKey(input, toolContext_)) {
                update();
                emitCoordinateUpdate();
                return;
            }
        }

        if (activeTool_ == Tool::Select &&
            event->key() == Qt::Key_A &&
            event->modifiers() == Qt::NoModifier) {
            selectedShapeIndices_.clear();
            selectedShapeIndices_.reserve(shapes_.size());
            for (int index = 0; index < shapes_.size(); ++index) {
                const ObjectId objectId = shapes_.objectIdAt(index);
                if (document_.isObjectEditable(objectId)) {
                    selectedShapeIndices_.append(objectId);
                }
            }
            selectedShapeIndex_ = selectedShapeIndices_.isEmpty()
                                      ? ObjectId::invalid()
                                      : selectedShapeIndices_.back();
            draggingSelected_ = false;
            draggingShapeIndices_.clear();
            draggingControlPoint_ = false;
            controlPointIndex_ = -1;
            update();
            emitCoordinateUpdate();
            DebugLog::instance().write(
                QStringLiteral("select all count=%1")
                    .arg(selectedShapeIndices_.size()));
            return;
        }

        if (activeTool_ == Tool::Select &&
            (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) &&
            !selectedShapeIndices_.isEmpty()) {
            deleteSelectedShapes();
            return;
        }

        if (event->key() == Qt::Key_Escape) {
            pendingPoints_.clear();
            resetArcPreviewTracking();
            DebugLog::instance().write(QStringLiteral("keyPress branch=cancel-input"));

            if (activeTool_ == Tool::Line && lineCommandActive_) {
                setTool(Tool::Select);
                if (commandFinished_) {
                    commandFinished_(Tool::Select);
                }
            } else if (activeTool_ == Tool::Arc &&
                       arcMode_ == ArcMode::OnePoint) {
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

        const ToolInput input = makeKeyToolInput(keyEvent);
        if (activeToolController_ != nullptr &&
            activeToolController_->handleKey(input, toolContext_)) {
            keyEvent->accept();
            update();
            emitCoordinateUpdate();
            return true;
        }
        return false;
    }

private:
    enum class ArcTextInputMode {
        None,
        Radius,
        Angle,
        ChordLength,
        Sagitta,
    };

    void beginOrbitAt(const QPointF &screenPosition)
    {
        const ViewportNavigationPreferences preferences =
            viewportTransform_.navigationPreferences();
        Point3D depthPoint;
        if (preferences.useMouseDepthNavigate) {
            const QVector<Shape> depthShapes = visibleDepthShapes();
            const bool gpuDepthHit =
                gpuSurface_ != nullptr &&
                gpuSurface_->pickScenePoint(screenPosition,
                                            viewportTransform_,
                                            size(),
                                            depthShapes,
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

    static qreal directionDot(const Point3D &first, const Point3D &second)
    {
        return first.x * second.x + first.y * second.y + first.z * second.z;
    }

    void stopNavigationAnimation()
    {
        if (navigationAnimation_ != nullptr) {
            navigationAnimation_->stop();
        }
    }

    void animateCameraChange(const std::function<void()> &setTarget)
    {
        stopNavigationAnimation();
        const ViewportCameraState original = viewportTransform_.cameraState();
        setTarget();
        const ViewportCameraState target = viewportTransform_.cameraState();
        constexpr qreal angleTolerance = 1.0e-7;
        const bool poseChanged =
            std::abs(original.zoom - target.zoom) > 1.0e-8 ||
            std::abs(original.gridViewDistance - target.gridViewDistance) > 1.0e-8 ||
            std::hypot(original.pan.x() - target.pan.x(),
                       original.pan.y() - target.pan.y()) > 1.0e-8 ||
            std::abs(original.orbitPivot.x - target.orbitPivot.x) > 1.0e-8 ||
            std::abs(original.orbitPivot.y - target.orbitPivot.y) > 1.0e-8 ||
            std::abs(original.orbitPivot.z - target.orbitPivot.z) > 1.0e-8 ||
            1.0 - std::abs(original.orientation.dot(target.orientation)) >
                angleTolerance;

        ViewportCameraState start = original;
        start.perspective = target.perspective;
        if (!poseChanged || navigationAnimation_ == nullptr) {
            viewportTransform_.setCameraState(target);
            notifyViewStateChanged();
            update();
            emitCoordinateUpdate();
            return;
        }
        navigationAnimationStart_ = start;
        navigationAnimationEnd_ = target;
        viewportTransform_.setCameraState(start);
        navigationAnimation_->start();
    }

    QString navigationTooltip(const BlenderNavigationHit &hit) const
    {
        switch (hit.action) {
        case BlenderNavigationAction::Axis: {
            const qreal component = std::abs(hit.direction.x) > 0.5
                                        ? hit.direction.x
                                        : std::abs(hit.direction.y) > 0.5
                                              ? hit.direction.y
                                              : hit.direction.z;
            const QString axis = std::abs(hit.direction.x) > 0.5
                                     ? QStringLiteral("X")
                                     : std::abs(hit.direction.y) > 0.5
                                           ? QStringLiteral("Y")
                                           : QStringLiteral("Z");
            return QStringLiteral("Align to %1%2 view; drag to orbit")
                .arg(component >= 0.0 ? QStringLiteral("+") : QStringLiteral("-"),
                     axis);
        }
        case BlenderNavigationAction::Orbit:
            return QStringLiteral("Drag to orbit the view");
        case BlenderNavigationAction::Zoom:
            return QStringLiteral("Drag to zoom the view");
        case BlenderNavigationAction::Pan:
            return QStringLiteral("Drag to pan the view");
        case BlenderNavigationAction::Camera:
            return QStringLiteral("Camera view (no active scene camera)");
        case BlenderNavigationAction::Projection:
            return viewportTransform_.isPerspectiveEnabled()
                       ? QStringLiteral("Switch to orthographic projection")
                       : QStringLiteral("Switch to perspective projection");
        case BlenderNavigationAction::None:
            break;
        }
        return {};
    }

    void resetForDocumentReplacement()
    {
        history_.clear();
        pendingPoints_.clear();
        pendingPictureImage_ = QImage();
        pendingPictureImageData_.clear();
        pendingPicturePath_.clear();
        controllerPreviewShape_ = Shape{};
        controllerPreviewShapeVisible_ = false;
        pointExtrudePreviewShapes_.clear();
        resetArcPreviewTracking();
        selectedShapeIndices_.clear();
        selectedShapeIndex_ = ObjectId::invalid();
        controlPointIndex_ = -1;
        selectionBoxActive_ = false;
        selectionBoxMoved_ = false;
        selectionBoxAdditive_ = false;
        trimBoxSelectionActive_ = false;
        draggingSelected_ = false;
        dragGestureStarted_ = false;
        draggingShapeIndices_.clear();
        draggingControlPoint_ = false;
        dragHistoryRecorded_ = false;
        currentDragSnap_ = DragSnapResult{};
        dragSnapLocked_ = false;
        eraseStrokeActive_ = false;
        eraseCursorPressed_ = false;
        eraseCandidateShapeIndices_.clear();
        eraseStrokeScreenPath_.clear();
        eraseTargetShapeIndices_.clear();
        eraseSceneCurveCaches_.clear();
        eraseTargetCurveCaches_.clear();
        eraseGeometryCachePrepared_ = false;
        trimHoverPositionValid_ = false;
        currentSnap_ = SnapResult{};
        joinActive_ = false;
        joinShapeIndices_.clear();
        resetScaleInteraction();
        resetRotateInteraction();
        resetMirrorInteraction();
        subdivisionActive_ = false;
        subdivisionShapeIndex_ = ObjectId::invalid();
        subdivisionSections_ = 2;
        resetSubdivisionWheelTracking();
        lineCommandActive_ = false;
        activeTool_ = Tool::Select;
        repeatTool_ = Tool::Select;
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

    bool appendJoinComponents(const Shape &shape,
                              QVector<Shape::NurbsCurve2D> *components) const
    {
        if (components == nullptr || !isJoinableShape(shape)) {
            return false;
        }

        if (shape.geometryType == GeometryType::PolyCurve) {
            *components += shape.components;
            return true;
        }

        if (isValidNurbsCurve(shape.nurbs)) {
            components->append(shape.nurbs);
            return true;
        }

        if (shape.geometryType == GeometryType::Line && shape.points.size() >= 2) {
            const Shape::NurbsCurve2D line = makeDegreeOneNurbs(shape.points);
            if (isValidNurbsCurve(line)) {
                components->append(line);
                return true;
            }
        }

        return false;
    }

    void fuseOverlappingLineComponents(
        QVector<Shape::NurbsCurve2D> *components,
        qreal tolerance) const
    {
        if (components == nullptr || components->isEmpty()) {
            return;
        }

        // Compare each straight span of a degree-1 spline, including bent
        // polylines. Keep the original knot domains and rational data when
        // splitting; only a fused union receives a new line representation.
        QVector<Shape::NurbsCurve2D> spans;
        for (const Shape::NurbsCurve2D &curve : *components) {
            if (curve.degree != 1 || curve.controlPoints.size() <= 2) {
                spans.append(curve);
                continue;
            }
            const QVector<double> knots = expandedKnotVector(curve);
            QVector<Shape::NurbsCurve2D> curveSpans;
            bool splitValid = true;
            for (int knotIndex = curve.degree;
                 knotIndex < curve.controlPoints.size();
                 ++knotIndex) {
                if (knots[knotIndex + 1] <= knots[knotIndex]) {
                    continue;
                }
                Shape::NurbsCurve2D span;
                if (!trimNurbsCurve(curve,
                                    knots[knotIndex],
                                    knots[knotIndex + 1],
                                    &span)) {
                    splitValid = false;
                    break;
                }
                curveSpans.append(span);
            }
            if (splitValid && !curveSpans.isEmpty()) {
                spans += curveSpans;
            } else {
                spans.append(curve);
            }
        }
        *components = spans;

        bool merged = true;
        while (merged) {
            merged = false;
            for (int firstIndex = 0;
                 firstIndex < components->size() && !merged;
                 ++firstIndex) {
                const Shape::NurbsCurve2D &firstCurve =
                    components->at(firstIndex);
                if (firstCurve.degree != 1 ||
                    firstCurve.controlPoints.size() != 2) {
                    continue;
                }

                QPointF firstStart;
                QPointF firstEnd;
                if (!nurbsCurveEndpoints(firstCurve, &firstStart, &firstEnd)) {
                    continue;
                }
                const QPointF firstDirection = firstEnd - firstStart;
                const qreal firstLength = std::hypot(firstDirection.x(),
                                                     firstDirection.y());
                if (firstLength <= tolerance) {
                    continue;
                }
                const QPointF axis = firstDirection / firstLength;

                for (int secondIndex = firstIndex + 1;
                     secondIndex < components->size();
                     ++secondIndex) {
                    const Shape::NurbsCurve2D &secondCurve =
                        components->at(secondIndex);
                    if (secondCurve.degree != 1 ||
                        secondCurve.controlPoints.size() != 2) {
                        continue;
                    }

                    QPointF secondStart;
                    QPointF secondEnd;
                    if (!nurbsCurveEndpoints(secondCurve,
                                             &secondStart,
                                             &secondEnd)) {
                        continue;
                    }
                    const QPointF secondDirection = secondEnd - secondStart;
                    const qreal secondLength = std::hypot(secondDirection.x(),
                                                          secondDirection.y());
                    if (secondLength <= tolerance) {
                        continue;
                    }
                    const QPointF secondAxis = secondDirection / secondLength;
                    const qreal directionCross =
                        axis.x() * secondAxis.y() - axis.y() * secondAxis.x();
                    if (std::abs(directionCross) > 1.0e-6) {
                        continue;
                    }

                    const auto perpendicularDistance =
                        [&](const QPointF &point) {
                            const QPointF delta = point - firstStart;
                            return std::abs(axis.x() * delta.y() -
                                            axis.y() * delta.x());
                        };
                    if (perpendicularDistance(secondStart) > tolerance ||
                        perpendicularDistance(secondEnd) > tolerance) {
                        continue;
                    }

                    const qreal firstProjection =
                        QPointF::dotProduct(firstEnd - firstStart, axis);
                    const qreal secondStartProjection =
                        QPointF::dotProduct(secondStart - firstStart, axis);
                    const qreal secondEndProjection =
                        QPointF::dotProduct(secondEnd - firstStart, axis);
                    const qreal firstLow = std::min<qreal>(0.0, firstProjection);
                    const qreal firstHigh = std::max<qreal>(0.0, firstProjection);
                    const qreal secondLow = std::min(secondStartProjection,
                                                     secondEndProjection);
                    const qreal secondHigh = std::max(secondStartProjection,
                                                      secondEndProjection);
                    if (secondLow > firstHigh + tolerance ||
                        firstLow > secondHigh + tolerance) {
                        continue;
                    }

                    const qreal unionLow = std::min(firstLow, secondLow);
                    const qreal unionHigh = std::max(firstHigh, secondHigh);
                    const QPointF fusedStart = firstStart + axis * unionLow;
                    const QPointF fusedEnd = firstStart + axis * unionHigh;
                    (*components)[firstIndex] =
                        makeDegreeOneNurbs({fusedStart, fusedEnd});
                    components->removeAt(secondIndex);
                    merged = true;
                    DebugLog::instance().write(
                        QStringLiteral("applyJoin fused overlapping line components=%1,%2 gapOrOverlapTolerance=%3")
                            .arg(firstIndex)
                            .arg(secondIndex)
                            .arg(tolerance, 0, 'f', 6));
                    break;
                }
            }
        }
    }

    void fuseOverlappingLineComponentsInWorld(
        QVector<Shape::NurbsCurve2D> *components,
        QVector<WorkPlaneFrame> *frames,
        qreal tolerance) const
    {
        // Split bent degree-1 curves in their own planes before comparing
        // spans. A straight span can belong to several different planes.
        QVector<Shape::NurbsCurve2D> spans;
        QVector<WorkPlaneFrame> spanFrames;
        for (int index = 0; index < components->size(); ++index) {
            QVector<Shape::NurbsCurve2D> pieces{components->at(index)};
            // The planar helper also performs the domain-preserving split.
            fuseOverlappingLineComponents(&pieces, tolerance);
            for (const auto &piece : pieces) {
                spans.append(piece);
                spanFrames.append(frames->at(index));
            }
        }
        *components = spans;
        *frames = spanFrames;
        bool merged = true;
        while (merged) {
            merged = false;
            for (int first = 0; first < components->size() && !merged; ++first) {
                if (components->at(first).degree != 1 ||
                    components->at(first).controlPoints.size() != 2) {
                    continue;
                }
                for (int second = first + 1; second < components->size(); ++second) {
                    if (components->at(second).degree != 1 ||
                        components->at(second).controlPoints.size() != 2) {
                        continue;
                    }
                    auto mapped = components->at(second);
                    bool inPlane = true;
                    for (QPointF &point : mapped.controlPoints) {
                        const Point3D world = workPlaneFramePointToWorld(point, frames->at(second));
                        point = worldPointToWorkPlaneFrame(world, frames->at(first));
                        const Point3D projected = workPlaneFramePointToWorld(point, frames->at(first));
                        inPlane = inPlane && std::hypot(
                            std::hypot(world.x - projected.x, world.y - projected.y),
                            world.z - projected.z) <= 1.0e-7;
                    }
                    if (!inPlane) {
                        continue;
                    }
                    QVector<Shape::NurbsCurve2D> pair{components->at(first), mapped};
                    fuseOverlappingLineComponents(&pair, tolerance);
                    if (pair.size() == 1) {
                        (*components)[first] = pair.first();
                        components->removeAt(second);
                        frames->removeAt(second);
                        merged = true;
                        break;
                    }
                }
            }
        }
    }

    bool nurbsCurveEndpoints(const Shape::NurbsCurve2D &curve,
                             QPointF *start,
                             QPointF *end) const
    {
        if (!isValidNurbsCurve(curve) || (start == nullptr && end == nullptr)) {
            return false;
        }

        const QVector<double> fullKnots = expandedKnotVector(curve);
        if (fullKnots.size() <= curve.controlPoints.size()) {
            return false;
        }

        const bool startValid = start == nullptr ||
                                evaluateNurbsPoint(curve, fullKnots[curve.degree], start);
        const bool endValid = end == nullptr ||
                              evaluateNurbsPoint(curve,
                                                 fullKnots[curve.controlPoints.size()],
                                                 end);
        return startValid && endValid;
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
        if (!isValidNurbsCurve(curve)) {
            return {};
        }

        Shape::NurbsCurve2D reversed = curve;
        std::reverse(reversed.controlPoints.begin(), reversed.controlPoints.end());
        if (!reversed.weights.isEmpty()) {
            std::reverse(reversed.weights.begin(), reversed.weights.end());
        }

        const QVector<double> fullKnots = expandedKnotVector(curve);
        const qreal domainStart = fullKnots[curve.degree];
        const qreal domainEnd = fullKnots[curve.controlPoints.size()];
        reversed.knots.clear();
        reversed.knots.reserve(curve.knots.size());
        for (int index = 1; index + 1 < fullKnots.size(); ++index) {
            reversed.knots.append(domainStart + domainEnd -
                                  fullKnots[fullKnots.size() - 1 - index]);
        }
        return reversed;
    }

    bool orderJoinComponents(const QVector<Shape::NurbsCurve2D> &input,
                             QVector<Shape::NurbsCurve2D> *ordered,
                             qreal tolerance) const
    {
        if (ordered == nullptr || input.isEmpty()) {
            return false;
        }

        QVector<QPointF> starts;
        QVector<QPointF> ends;
        starts.reserve(input.size());
        ends.reserve(input.size());
        for (const Shape::NurbsCurve2D &curve : input) {
            QPointF start;
            QPointF end;
            if (!nurbsCurveEndpoints(curve, &start, &end)) {
                return false;
            }
            starts.append(start);
            ends.append(end);
        }

        const auto endpointsMatch = [tolerance](const QPointF &first,
                                                  const QPointF &second) {
            return std::hypot(first.x() - second.x(), first.y() - second.y()) <= tolerance;
        };

        QVector<int> componentOrder;
        QVector<bool> componentReversed;
        QVector<char> used(input.size(), false);

        const auto makeResult = [&]() {
            ordered->clear();
            ordered->reserve(componentOrder.size());
            for (int position = 0; position < componentOrder.size(); ++position) {
                const int componentIndex = componentOrder[position];
                ordered->append(componentReversed[position]
                                    ? reversedNurbsCurve(input[componentIndex])
                                    : input[componentIndex]);
            }
        };

        std::function<bool(const QPointF &)> extendChain;
        extendChain = [&](const QPointF &currentEnd) {
            if (componentOrder.size() == input.size()) {
                return true;
            }

            for (int candidate = 0; candidate < input.size(); ++candidate) {
                if (used[candidate]) {
                    continue;
                }

                if (endpointsMatch(currentEnd, starts[candidate])) {
                    used[candidate] = true;
                    componentOrder.append(candidate);
                    componentReversed.append(false);
                    if (extendChain(ends[candidate])) {
                        return true;
                    }
                    componentReversed.removeLast();
                    componentOrder.removeLast();
                    used[candidate] = false;
                }

                if (endpointsMatch(currentEnd, ends[candidate])) {
                    used[candidate] = true;
                    componentOrder.append(candidate);
                    componentReversed.append(true);
                    if (extendChain(starts[candidate])) {
                        return true;
                    }
                    componentReversed.removeLast();
                    componentOrder.removeLast();
                    used[candidate] = false;
                }
            }

            return false;
        };

        for (int first = 0; first < input.size(); ++first) {
            for (const bool reverseFirst : {false, true}) {
                std::fill(used.begin(), used.end(), false);
                componentOrder.clear();
                componentReversed.clear();
                used[first] = true;
                componentOrder.append(first);
                componentReversed.append(reverseFirst);
                const QPointF firstEnd = reverseFirst ? starts[first] : ends[first];
                if (extendChain(firstEnd)) {
                    makeResult();
                    return true;
                }
            }
        }

        return false;
    }

    bool orderJoinComponentsInWorld(
        const QVector<Shape::NurbsCurve2D> &input,
        const QVector<WorkPlaneFrame> &frames,
        QVector<Shape::NurbsCurve2D> *ordered,
        QVector<WorkPlaneFrame> *orderedFrames,
        qreal tolerance) const
    {
        if (ordered == nullptr || orderedFrames == nullptr || input.isEmpty() ||
            input.size() != frames.size()) {
            return false;
        }

        const auto distance = [](const Point3D &first, const Point3D &second) {
            return std::hypot(std::hypot(first.x - second.x,
                                         first.y - second.y),
                              first.z - second.z);
        };
        QVector<Point3D> starts;
        QVector<Point3D> ends;
        starts.reserve(input.size());
        ends.reserve(input.size());
        for (int index = 0; index < input.size(); ++index) {
            QPointF start;
            QPointF end;
            if (!isValidWorkPlaneFrame(frames[index]) ||
                !nurbsCurveEndpoints(input[index], &start, &end)) {
                return false;
            }
            starts.append(workPlaneFramePointToWorld(start, frames[index]));
            ends.append(workPlaneFramePointToWorld(end, frames[index]));
        }

        QVector<int> componentOrder;
        QVector<bool> componentReversed;
        QVector<bool> used(input.size(), false);
        const auto makeResult = [&]() {
            ordered->clear();
            orderedFrames->clear();
            ordered->reserve(componentOrder.size());
            orderedFrames->reserve(componentOrder.size());
            for (int position = 0; position < componentOrder.size(); ++position) {
                const int componentIndex = componentOrder[position];
                ordered->append(componentReversed[position]
                                    ? reversedNurbsCurve(input[componentIndex])
                                    : input[componentIndex]);
                orderedFrames->append(frames[componentIndex]);
            }
        };

        std::function<bool(const Point3D &)> extendChain;
        extendChain = [&](const Point3D &currentEnd) {
            if (componentOrder.size() == input.size()) {
                return true;
            }
            for (int candidate = 0; candidate < input.size(); ++candidate) {
                if (used[candidate]) {
                    continue;
                }
                for (const bool reverseCandidate : {false, true}) {
                    const Point3D &candidateStart = reverseCandidate
                                                        ? ends[candidate]
                                                        : starts[candidate];
                    const Point3D &candidateEnd = reverseCandidate
                                                      ? starts[candidate]
                                                      : ends[candidate];
                    if (distance(currentEnd, candidateStart) > tolerance) {
                        continue;
                    }
                    used[candidate] = true;
                    componentOrder.append(candidate);
                    componentReversed.append(reverseCandidate);
                    if (extendChain(candidateEnd)) {
                        return true;
                    }
                    componentReversed.removeLast();
                    componentOrder.removeLast();
                    used[candidate] = false;
                }
            }
            return false;
        };

        for (int first = 0; first < input.size(); ++first) {
            for (const bool reverseFirst : {false, true}) {
                std::fill(used.begin(), used.end(), false);
                componentOrder.clear();
                componentReversed.clear();
                used[first] = true;
                componentOrder.append(first);
                componentReversed.append(reverseFirst);
                const Point3D &firstEnd = reverseFirst ? starts[first]
                                                       : ends[first];
                if (extendChain(firstEnd)) {
                    makeResult();
                    return true;
                }
            }
        }
        return false;
    }

    bool closeJoinGapsInWorld(QVector<Shape::NurbsCurve2D> *components,
                              QVector<WorkPlaneFrame> *frames,
                              qreal tolerance) const
    {
        if (components == nullptr || frames == nullptr || components->isEmpty() ||
            components->size() != frames->size()) {
            return false;
        }
        for (int index = 0; index + 1 < components->size(); ++index) {
            QPointF previousEnd;
            QPointF nextStart;
            if (!nurbsCurveEndpoints(components->at(index), nullptr, &previousEnd) ||
                !nurbsCurveEndpoints(components->at(index + 1), &nextStart, nullptr)) {
                return false;
            }
            const Point3D previousWorld = workPlaneFramePointToWorld(
                previousEnd, frames->at(index));
            const Point3D nextWorld = workPlaneFramePointToWorld(
                nextStart, frames->at(index + 1));
            const Point3D delta{previousWorld.x - nextWorld.x,
                                previousWorld.y - nextWorld.y,
                                previousWorld.z - nextWorld.z};
            const qreal gap = std::hypot(std::hypot(delta.x, delta.y), delta.z);
            if (gap > tolerance) {
                return false;
            }
            WorkPlaneFrame &nextFrame = (*frames)[index + 1];
            nextFrame.origin.x += delta.x;
            nextFrame.origin.y += delta.y;
            nextFrame.origin.z += delta.z;
        }
        return true;
    }

    bool joinComponentsAreContinuousInWorld(
        const QVector<Shape::NurbsCurve2D> &components,
        const QVector<WorkPlaneFrame> &frames,
        qreal tolerance) const
    {
        if (components.size() != frames.size()) {
            return false;
        }
        for (int index = 0; index + 1 < components.size(); ++index) {
            QPointF previousEnd;
            QPointF nextStart;
            if (!nurbsCurveEndpoints(components[index], nullptr, &previousEnd) ||
                !nurbsCurveEndpoints(components[index + 1], &nextStart, nullptr)) {
                return false;
            }
            const Point3D first = workPlaneFramePointToWorld(previousEnd,
                                                              frames[index]);
            const Point3D second = workPlaneFramePointToWorld(nextStart,
                                                               frames[index + 1]);
            const qreal gap = std::hypot(
                std::hypot(first.x - second.x, first.y - second.y),
                first.z - second.z);
            if (gap > tolerance) {
                return false;
            }
        }
        return true;
    }

    bool closeJoinGaps(QVector<Shape::NurbsCurve2D> *components,
                       qreal tolerance) const
    {
        if (components == nullptr || components->isEmpty()) {
            return false;
        }

        for (int index = 0; index + 1 < components->size(); ++index) {
            QPointF previousEnd;
            QPointF nextStart;
            if (!nurbsCurveEndpoints(components->at(index), nullptr, &previousEnd) ||
                !nurbsCurveEndpoints(components->at(index + 1), &nextStart, nullptr)) {
                return false;
            }

            const QPointF delta = previousEnd - nextStart;
            if (std::hypot(delta.x(), delta.y()) > tolerance) {
                return false;
            }

            // Move only the next component. That closes this seam without
            // disturbing a seam that was already closed earlier in the chain.
            // The component's degree, weights, knots, and parameter domain
            // remain unchanged.
            for (QPointF &controlPoint : (*components)[index + 1].controlPoints) {
                controlPoint += delta;
            }
        }

        return true;
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

    bool joinComponentsAreContinuous(const QVector<Shape::NurbsCurve2D> &components,
                                     qreal joinTolerance) const
    {
        for (int index = 0; index + 1 < components.size(); ++index) {
            QPointF firstEnd;
            QPointF nextStart;
            if (!nurbsCurveEndpoints(components[index], nullptr, &firstEnd) ||
                !nurbsCurveEndpoints(components[index + 1], &nextStart, nullptr)) {
                return false;
            }

            if (std::hypot(firstEnd.x() - nextStart.x(),
                           firstEnd.y() - nextStart.y()) > joinTolerance) {
                return false;
            }
        }

        return true;
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

    QVector<QVector<Shape::NurbsCurve2D>> connectedCurveGroups(
        const QVector<Shape::NurbsCurve2D> &curves) const
    {
        QVector<QPointF> starts;
        QVector<QPointF> ends;
        starts.reserve(curves.size());
        ends.reserve(curves.size());
        qreal coordinateScale = 1.0;
        for (const Shape::NurbsCurve2D &curve : curves) {
            QPointF start;
            QPointF end;
            if (!nurbsCurveEndpoints(curve, &start, &end)) {
                starts.append(QPointF());
                ends.append(QPointF());
                continue;
            }
            starts.append(start);
            ends.append(end);
            coordinateScale = std::max({coordinateScale,
                                        std::abs(start.x()),
                                        std::abs(start.y()),
                                        std::abs(end.x()),
                                        std::abs(end.y())});
        }

        const qreal endpointTolerance = std::max<qreal>(1.0e-8,
                                                        coordinateScale * 1.0e-12);
        const auto endpointsMatch = [endpointTolerance](const QPointF &first,
                                                        const QPointF &second) {
            return std::hypot(first.x() - second.x(),
                              first.y() - second.y()) <= endpointTolerance;
        };
        QVector<QVector<Shape::NurbsCurve2D>> groups;
        QVector<bool> grouped(curves.size(), false);
        for (int seed = 0; seed < curves.size(); ++seed) {
            if (grouped[seed]) {
                continue;
            }

            QVector<int> connectedIndices{seed};
            grouped[seed] = true;
            for (int cursor = 0; cursor < connectedIndices.size(); ++cursor) {
                const int current = connectedIndices[cursor];
                if (!isValidNurbsCurve(curves[current])) {
                    continue;
                }
                for (int candidate = 0; candidate < curves.size(); ++candidate) {
                    if (grouped[candidate] || !isValidNurbsCurve(curves[candidate])) {
                        continue;
                    }
                    if (endpointsMatch(starts[current], starts[candidate]) ||
                        endpointsMatch(starts[current], ends[candidate]) ||
                        endpointsMatch(ends[current], starts[candidate]) ||
                        endpointsMatch(ends[current], ends[candidate])) {
                        grouped[candidate] = true;
                        connectedIndices.append(candidate);
                    }
                }
            }

            std::sort(connectedIndices.begin(), connectedIndices.end());
            QVector<Shape::NurbsCurve2D> group;
            group.reserve(connectedIndices.size());
            for (const int index : connectedIndices) {
                group.append(curves[index]);
            }
            QVector<Shape::NurbsCurve2D> ordered;
            if (group.size() > 1 &&
                orderJoinComponents(group, &ordered, joinEndpointTolerance())) {
                group = ordered;
            }
            groups.append(group);
        }
        return groups;
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

            const auto connectedGroups = connectedCurveGroups(source.components);
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

    bool deleteSelectedShapes()
    {
        QVector<int> indices;
        for (const ObjectId objectId : selectedShapeIndices_) {
            const int index = objectIndex(objectId);
            if (index >= 0 && index < shapes_.size() && !indices.contains(index)) {
                indices.append(index);
            }
        }

        if (indices.isEmpty()) {
            clearSelection();
            return false;
        }

        std::sort(indices.begin(), indices.end());
        const int deletedCount = indices.size();
        recordGeometryChange();

        for (auto iterator = indices.crbegin(); iterator != indices.crend(); ++iterator) {
            shapes_.removeAt(*iterator);
        }

        clearSelection();
        draggingSelected_ = false;
        draggingShapeIndices_.clear();
        draggingControlPoint_ = false;
        controlPointIndex_ = -1;
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
        QVector<ObjectId> editableSelection;
        editableSelection.reserve(selectedShapeIndices_.size());
        for (const ObjectId objectId : selectedShapeIndices_) {
            if (document_.isObjectEditable(objectId)) {
                editableSelection.append(objectId);
            }
        }

        const ObjectId primaryObject = editableSelection.contains(selectedShapeIndex_)
                                           ? selectedShapeIndex_
                                           : ObjectId::invalid();
        selection_.setObjectIds(editableSelection, primaryObject);
        selectedShapeIndex_ = selection_.primaryObjectId();
        draggingSelected_ = false;
        draggingShapeIndices_.clear();
        draggingControlPoint_ = false;
        controlPointIndex_ = -1;
        dragHistoryRecorded_ = false;
        currentDragSnap_ = DragSnapResult{};
        dragSnapLocked_ = false;
    }

    void setSingleSelection(int shapeIndex)
    {
        selection_.clear();
        if (shapeIndex >= 0 && shapeIndex < shapes_.size()) {
            const ObjectId objectId = shapes_.objectIdAt(shapeIndex);
            selection_.add(objectId);
        }
    }

    void toggleShapeSelection(int shapeIndex)
    {
        if (shapeIndex < 0 || shapeIndex >= shapes_.size()) {
            return;
        }

        const ObjectId objectId = shapes_.objectIdAt(shapeIndex);
        selection_.toggle(objectId);
    }

    QRectF selectionBoundsForShape(const Shape &shape,
                                   bool *hasProjectedPoints = nullptr) const
    {
        if (hasProjectedPoints != nullptr) {
            *hasProjectedPoints = false;
        }
        QVector<QPointF> points = controlPointsForShape(shape);
        // Keep the source points in the selection bounds as well as the
        // stored NURBS CVs. This covers endpoints that are represented by
        // the shape record but are not present in a malformed/legacy CV
        // array, while the NURBS data remains the rendering source of truth.
        for (const QPointF &point : shape.points) {
            if (!points.contains(point)) {
                points.append(point);
            }
        }

        qreal minX = 0.0;
        qreal maxX = 0.0;
        qreal minY = 0.0;
        qreal maxY = 0.0;
        bool initialized = false;
        const WorkPlaneFrame frame = shapeWorkPlaneFrame(shape);
        for (const QPointF &point : points) {
            if (!std::isfinite(point.x()) || !std::isfinite(point.y())) {
                continue;
            }

            QPointF screenPoint;
            if (!viewportTransform_.worldPointToScreen(
                    workPlaneFramePointToWorld(point, frame), size(), &screenPoint)) {
                continue;
            }
            if (!initialized) {
                minX = maxX = screenPoint.x();
                minY = maxY = screenPoint.y();
                initialized = true;
            } else {
                minX = std::min(minX, screenPoint.x());
                maxX = std::max(maxX, screenPoint.x());
                minY = std::min(minY, screenPoint.y());
                maxY = std::max(maxY, screenPoint.y());
            }
        }

        if (!initialized) {
            return {};
        }
        if (hasProjectedPoints != nullptr) {
            *hasProjectedPoints = true;
        }

        // Window selection uses the geometry's true projected bounds. Padding
        // these bounds makes a fully enclosed object fail when it lies close
        // to the selection rectangle's edge.
        return QRectF(QPointF(minX, minY), QPointF(maxX, maxY));
    }

    bool shapeMatchesSelectionBox(const Shape &shape,
                                  const QRectF &box,
                                  bool crossingSelection) const
    {
        const QRectF selectionRect = box.normalized();
        constexpr qreal crossingTolerancePixels = 2.0;
        const QRectF hitRect = crossingSelection
                                   ? selectionRect.adjusted(-crossingTolerancePixels,
                                                            -crossingTolerancePixels,
                                                            crossingTolerancePixels,
                                                            crossingTolerancePixels)
                                   : selectionRect;

        const auto pointInside = [&hitRect](const QPointF &point) {
            return hitRect.left() <= point.x() && point.x() <= hitRect.right() &&
                   hitRect.top() <= point.y() && point.y() <= hitRect.bottom();
        };
        const auto segmentIntersectsRect = [&hitRect](const QPointF &start,
                                                       const QPointF &end) {
            // Clipped samples have NaN coordinates. Comparisons in the
            // clipping algorithm otherwise fall through and report a hit.
            if (!std::isfinite(start.x()) || !std::isfinite(start.y()) ||
                !std::isfinite(end.x()) || !std::isfinite(end.y())) {
                return false;
            }
            // Liang-Barsky clipping checks the actual projected segment. This
            // avoids selecting a distant diagonal curve just because its
            // axis-aligned bounding box overlaps the crossing window.
            const qreal dx = end.x() - start.x();
            const qreal dy = end.y() - start.y();
            const qreal p[] = {-dx, dx, -dy, dy};
            const qreal q[] = {start.x() - hitRect.left(),
                               hitRect.right() - start.x(),
                               start.y() - hitRect.top(),
                               hitRect.bottom() - start.y()};
            qreal firstFraction = 0.0;
            qreal lastFraction = 1.0;
            for (int edge = 0; edge < 4; ++edge) {
                if (std::abs(p[edge]) <= 1.0e-12) {
                    if (q[edge] < 0.0) {
                        return false;
                    }
                    continue;
                }
                const qreal fraction = q[edge] / p[edge];
                if (p[edge] < 0.0) {
                    firstFraction = std::max(firstFraction, fraction);
                } else {
                    lastFraction = std::min(lastFraction, fraction);
                }
                if (firstFraction > lastFraction) {
                    return false;
                }
            }
            return true;
        };

        const QVector<Shape::NurbsCurve2D> curves =
            curveSampler_.curvesForShape(shape);
        if (!curves.isEmpty()) {
            bool sampledGeometry = false;
            bool intersects = false;
            for (int componentIndex = 0; componentIndex < curves.size(); ++componentIndex) {
                const Shape::NurbsCurve2D &curve = curves[componentIndex];
                const WorkPlaneFrame frame =
                    shape.geometryType == GeometryType::PolyCurve
                        ? shapeComponentWorkPlaneFrame(shape, componentIndex)
                        : shapeWorkPlaneFrame(shape);
                SampledNurbsCurve2D sampled;
                if (!curveSampler_.sampleNurbsCurve(curve,
                                                     frame,
                                                     viewportTransform_,
                                                     size(),
                                                     &sampled)) {
                    return false;
                }
                sampledGeometry = true;
                for (const QPointF &point : sampled.screenPoints) {
                    if (!crossingSelection && !pointInside(point)) {
                        return false;
                    }
                    if (crossingSelection && pointInside(point)) {
                        intersects = true;
                    }
                }
                if (crossingSelection) {
                    for (int index = 1;
                         index < sampled.screenPoints.size();
                         ++index) {
                        if (segmentIntersectsRect(sampled.screenPoints[index - 1],
                                                  sampled.screenPoints[index])) {
                            intersects = true;
                            break;
                        }
                    }
                }
            }
            if (!sampledGeometry) {
                return false;
            }
            return crossingSelection ? intersects : true;
        }

        if (shape.geometryType == GeometryType::Point && !shape.points.isEmpty()) {
            const WorkPlaneFrame frame = shapeWorkPlaneFrame(shape);
            QPointF screenPoint;
            if (!viewportTransform_.worldPointToScreen(
                    workPlaneFramePointToWorld(shape.points.first(), frame),
                    size(),
                    &screenPoint)) {
                return false;
            }
            return pointInside(screenPoint);
        }

        // Non-curve geometry such as pictures and dimensions has no NURBS
        // path to sample, so retain its existing projected-bounds selection.
        bool hasProjectedPoints = false;
        const QRectF bounds = selectionBoundsForShape(shape, &hasProjectedPoints)
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
        selectionBoxActive_ = true;
        selectionBoxMoved_ = false;
        selectionBoxAdditive_ = additive;
        trimBoxSelectionActive_ = false;
        selectionBoxStartScreen_ = screenPosition;
        selectionBoxCurrentScreen_ = screenPosition;
        draggingSelected_ = false;
        draggingShapeIndices_.clear();
        draggingControlPoint_ = false;
        controlPointIndex_ = -1;
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
        if (!selectionBoxActive_) {
            return;
        }

        const QRectF selectionBox =
            QRectF(selectionBoxStartScreen_, selectionBoxCurrentScreen_).normalized();
        const bool crossingSelection =
            selectionBoxCurrentScreen_.x() < selectionBoxStartScreen_.x();
        const bool moved = selectionBoxMoved_ ||
                           selectionBox.width() >= 3.0 ||
                           selectionBox.height() >= 3.0;
        QVector<ObjectId> boxSelection;
        if (moved) {
            for (int index = 0; index < shapes_.size(); ++index) {
                const ObjectId objectId = shapes_.objectIdAt(index);
                if (!document_.isObjectEditable(objectId)) {
                    continue;
                }
                if (shapeMatchesSelectionBox(shapes_[index],
                                              selectionBox,
                                              crossingSelection)) {
                    boxSelection.append(shapes_.objectIdAt(index));
                }
            }
        }

        if (moved) {
            if (selectionBoxAdditive_) {
                for (const ObjectId objectId : boxSelection) {
                    if (!selectedShapeIndices_.contains(objectId)) {
                        selectedShapeIndices_.append(objectId);
                    }
                }
                if (!boxSelection.isEmpty()) {
                    selectedShapeIndex_ = boxSelection.back();
                }
            } else {
                selectedShapeIndices_ = boxSelection;
                selectedShapeIndex_ = boxSelection.isEmpty()
                                          ? ObjectId::invalid()
                                          : boxSelection.back();
            }
        } else if (!selectionBoxAdditive_) {
            clearSelection();
        }

        DebugLog::instance().write(QStringLiteral("selection box finish moved=%1 crossing=%2 additive=%3 selected=%4")
                                       .arg(moved)
                                       .arg(crossingSelection)
                                       .arg(selectionBoxAdditive_)
                                       .arg(selectedShapeIndices_.size()));
        selectionBoxActive_ = false;
        selectionBoxMoved_ = false;
        selectionBoxAdditive_ = false;
        trimBoxSelectionActive_ = false;
        setCursor(joinActive_ ? Qt::CrossCursor : Qt::ArrowCursor);
        update();
        emitCoordinateUpdate();
    }

    void cancelSelectionBox()
    {
        if (!selectionBoxActive_) {
            return;
        }

        const bool cancelingTrimBox = trimBoxSelectionActive_;
        selectionBoxActive_ = false;
        selectionBoxMoved_ = false;
        selectionBoxAdditive_ = false;
        trimBoxSelectionActive_ = false;
        if (cancelingTrimBox) {
            eraseCandidateShapeIndices_.clear();
            eraseStrokeScreenPath_.clear();
            eraseTargetShapeIndices_.clear();
            eraseSceneCurveCaches_.clear();
            eraseTargetCurveCaches_.clear();
            eraseGeometryCachePrepared_ = false;
            trimHoverPositionValid_ = false;
        }
        setCursor(joinActive_ || activeTool_ == Tool::Trim
                      ? Qt::CrossCursor
                      : Qt::ArrowCursor);
        update();
        DebugLog::instance().write(QStringLiteral("selection box canceled"));
    }

    bool isSubdividableShape(const Shape &shape) const
    {
        if (shape.geometryType == GeometryType::Line) {
            return shape.points.size() >= 2 &&
                   (isValidNurbsCurve(shape.nurbs) || !shape.points.isEmpty());
        }

        return (shape.geometryType == GeometryType::Arc ||
                shape.geometryType == GeometryType::Bezier ||
                shape.geometryType == GeometryType::Nurbs ||
                shape.geometryType == GeometryType::Circle ||
                shape.geometryType == GeometryType::Ellipse) &&
               isValidNurbsCurve(shape.nurbs);
    }

    bool subdivisionCurve(const Shape &shape, Shape::NurbsCurve2D *curve) const
    {
        if (curve == nullptr || !isSubdividableShape(shape)) {
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

    QVector<double> subdivisionParametersForSections(const Shape &shape,
                                                      int sections) const
    {
        QVector<double> parameters;
        if (sections < 2) {
            return parameters;
        }

        Shape::NurbsCurve2D curve;
        if (!subdivisionCurve(shape, &curve)) {
            return parameters;
        }

        const QVector<double> fullKnots = expandedKnotVector(curve);
        if (fullKnots.size() <= curve.degree + 1 ||
            curve.controlPoints.isEmpty()) {
            return parameters;
        }

        const qreal firstParameter = fullKnots[curve.degree];
        const qreal lastParameter = fullKnots[curve.controlPoints.size()];
        if (!std::isfinite(firstParameter) || !std::isfinite(lastParameter) ||
            lastParameter <= firstParameter) {
            return parameters;
        }

        int nonZeroSpans = 0;
        for (int index = curve.degree; index < curve.controlPoints.size(); ++index) {
            if (fullKnots[index + 1] > fullKnots[index]) {
                ++nonZeroSpans;
            }
        }

        const int sampleCount = std::clamp(std::max(128, nonZeroSpans * 64), 128, 4096);
        QVector<qreal> sampleParameters;
        QVector<qreal> cumulativeLengths;
        sampleParameters.reserve(sampleCount + 1);
        cumulativeLengths.reserve(sampleCount + 1);

        QPointF previousPoint;
        if (!evaluateNurbsPoint(curve, firstParameter, &previousPoint)) {
            return parameters;
        }

        sampleParameters.append(firstParameter);
        cumulativeLengths.append(0.0);
        qreal totalLength = 0.0;
        for (int sample = 1; sample <= sampleCount; ++sample) {
            const qreal fraction = static_cast<qreal>(sample) / sampleCount;
            const qreal parameter = firstParameter +
                                    (lastParameter - firstParameter) * fraction;
            QPointF currentPoint;
            if (!evaluateNurbsPoint(curve, parameter, &currentPoint)) {
                return QVector<double>();
            }

            totalLength += std::hypot(currentPoint.x() - previousPoint.x(),
                                      currentPoint.y() - previousPoint.y());
            sampleParameters.append(parameter);
            cumulativeLengths.append(totalLength);
            previousPoint = currentPoint;
        }

        if (totalLength <= 1e-9) {
            return parameters;
        }

        parameters.reserve(sections - 1);
        for (int division = 1; division < sections; ++division) {
            const qreal targetLength = totalLength * division / sections;
            const auto upper = std::lower_bound(cumulativeLengths.cbegin(),
                                                cumulativeLengths.cend(),
                                                targetLength);
            const int upperIndex = static_cast<int>(upper - cumulativeLengths.cbegin());
            if (upperIndex <= 0) {
                parameters.append(sampleParameters.first());
                continue;
            }
            if (upperIndex >= cumulativeLengths.size()) {
                parameters.append(sampleParameters.last());
                continue;
            }

            const qreal lowerLength = cumulativeLengths[upperIndex - 1];
            const qreal upperLength = cumulativeLengths[upperIndex];
            const qreal span = upperLength - lowerLength;
            const qreal localFraction = span > 1e-12
                                            ? (targetLength - lowerLength) / span
                                            : 0.0;
            parameters.append(sampleParameters[upperIndex - 1] +
                               (sampleParameters[upperIndex] -
                                sampleParameters[upperIndex - 1]) * localFraction);
        }

        return parameters;
    }

    void resetSubdivisionWheelTracking()
    {
        subdivisionWheelAccumulator_ = 0;
        subdivisionPixelAccumulator_ = 0.0;
    }

    int subdivisionWheelStepsFromEvent(int angleDelta, int pixelDelta)
    {
        if (angleDelta != 0) {
            // Qt can split one physical wheel detent into several smooth
            // angle deltas. Blender's modal tools receive normalized wheel
            // events, so retain only the incomplete part here and emit one
            // logical step for each complete 120-unit detent.
            subdivisionPixelAccumulator_ = 0.0;
            if (subdivisionWheelAccumulator_ != 0 &&
                ((subdivisionWheelAccumulator_ > 0) != (angleDelta > 0))) {
                subdivisionWheelAccumulator_ = 0;
            }

            subdivisionWheelAccumulator_ += angleDelta;
            const int steps = subdivisionWheelAccumulator_ / 120;
            subdivisionWheelAccumulator_ -= steps * 120;
            return steps;
        }

        if (pixelDelta != 0) {
            // A pure pixel-delta stream has no platform-independent detent
            // size. Keep the same discrete behavior with a conservative
            // screen-pixel threshold for touch/high-resolution devices.
            subdivisionWheelAccumulator_ = 0;
            if (subdivisionPixelAccumulator_ != 0.0 &&
                ((subdivisionPixelAccumulator_ > 0.0) != (pixelDelta > 0))) {
                subdivisionPixelAccumulator_ = 0.0;
            }

            subdivisionPixelAccumulator_ += pixelDelta;
            constexpr qreal pixelsPerWheelStep = 40.0;
            const int magnitude = static_cast<int>(std::floor(
                std::abs(subdivisionPixelAccumulator_) / pixelsPerWheelStep));
            const int steps = subdivisionPixelAccumulator_ > 0.0 ? magnitude : -magnitude;
            subdivisionPixelAccumulator_ -= steps * pixelsPerWheelStep;
            return steps;
        }

        return 0;
    }

    void applySubdivisionWheelSteps(int steps)
    {
        if (!subdivisionActive_ || steps == 0) {
            return;
        }

        const int previousSections = subdivisionSections_;
        subdivisionSections_ = std::clamp(subdivisionSections_ + steps,
                                          2,
                                          maxSubdivisionSections);
        if (subdivisionSections_ != previousSections) {
            notifySubdivisionStatus();
            update();
            DebugLog::instance().write(
                QStringLiteral("subdivision wheel sections=%1 points=%2")
                    .arg(subdivisionSections_)
                    .arg(subdivisionSections_ - 1));
        }
    }

    void notifySubdivisionStatus()
    {
        if (subdivisionStatusUpdate_) {
            subdivisionStatusUpdate_(subdivisionStatusText());
        }
    }

    void cancelSubdivisionPreview()
    {
        if (!subdivisionActive_) {
            return;
        }

        subdivisionActive_ = false;
        subdivisionShapeIndex_ = ObjectId::invalid();
        subdivisionSections_ = 2;
        resetSubdivisionWheelTracking();
        notifySubdivisionStatus();
        update();
        DebugLog::instance().write(QStringLiteral("cancelSubdivisionPreview"));
    }

    void notifyHistoryChanged()
    {
        if (historyChanged_) {
            historyChanged_();
        }
    }

    void notifyLayersChanged()
    {
        if (layersChanged_) {
            layersChanged_();
        }
    }

    void recordLayerChange()
    {
        history_.record();
        notifyHistoryChanged();
    }

    void recordGeometryChange()
    {
        recordGeometrySnapshot(document_.snapshot());
    }

    void recordGeometrySnapshot(const Document::Snapshot &snapshot)
    {
        history_.record(snapshot);
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
        if (grabActive_) {
            return;
        }
        if (!dragHistoryRecorded_) {
            recordGeometryChange();
            dragHistoryRecorded_ = true;
        }
    }

    void resetGrabInteraction()
    {
        grabActive_ = false;
        grabMoved_ = false;
        grabPickingBasePoint_ = false;
        grabHasBasePoint_ = false;
        grabBasePoint_ = QPointF();
        grabCursorOffset_ = QPointF();
    }

    void resetDuplicateInteraction()
    {
        duplicateActive_ = false;
        duplicatePickingBasePoint_ = false;
        duplicateHasBasePoint_ = false;
        duplicateSourceObjects_.clear();
        duplicatePreviewShapes_.clear();
        duplicateBasePoint_ = QPointF();
        duplicateCursorOffset_ = QPointF();
        duplicateDestination_ = QPointF();
    }

    void resetInteractionAfterHistory()
    {
        pendingPoints_.clear();
        selectedShapeIndices_.clear();
        selectedShapeIndex_ = ObjectId::invalid();
        selectionBoxActive_ = false;
        selectionBoxMoved_ = false;
        selectionBoxAdditive_ = false;
        trimBoxSelectionActive_ = false;
        draggingSelected_ = false;
        draggingShapeIndices_.clear();
        dragHistoryRecorded_ = false;
        currentSnap_ = SnapResult{};
        currentDragSnap_ = DragSnapResult{};
        dragSnapLocked_ = false;
        dragAxisLock_ = DragAxisLock::None;
        resetGrabInteraction();
        draggingControlPoint_ = false;
        controlPointIndex_ = -1;
        eraseStrokeActive_ = false;
        eraseCursorPressed_ = false;
        eraseCandidateShapeIndices_.clear();
        eraseStrokeScreenPath_.clear();
        eraseTargetShapeIndices_.clear();
        eraseSceneCurveCaches_.clear();
        eraseTargetCurveCaches_.clear();
        eraseGeometryCachePrepared_ = false;
        trimHoverPositionValid_ = false;
        joinActive_ = false;
        joinShapeIndices_.clear();
        resetScaleInteraction();
        resetRotateInteraction();
        resetMirrorInteraction();
        subdivisionActive_ = false;
        subdivisionShapeIndex_ = ObjectId::invalid();
        subdivisionSections_ = 2;
        resetSubdivisionWheelTracking();
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
            recordGeometryChange();
            Shape line{GeometryType::Line,
                       pendingPoints_,
                       curve,
                       ArcMode::TwoPoint,
                       0.0,
                       {},
                       {}};
            line.workPlane = viewportTransform_.workPlane();
            line.workPlaneOffset = viewportTransform_.workPlaneOffset();
            line.workPlaneFrame = viewportTransform_.workPlaneFrame();
            shapes_.append(line);
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
        if (subdivisionCurve(shape, &subdivisionCurveData)) {
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
                if (subdivisionCurve(shape, &subdivisionCurveData)) {
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
                const bool visible =
                    document_.isObjectVisible(document_.objectIdAt(shapeIndex));
                const WorkPlaneFrame targetFrame = shapeWorkPlaneFrame(shape);
                const bool frameMatches = workPlaneMatches(targetFrame, activeFrame);
                const QVector<SnapCandidate> candidates =
                    visible ? snapEngine_.snapCandidatesForShape(shape,
                                                                 viewportTransform_,
                                                                 size())
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
            if (rotateStep_ == 1) {
                anchor = &rotateBaseWorldPoint_;
            } else if (rotateStep_ == 2) {
                anchor = &rotateReferenceWorldPoint_;
            }
            const SnapResult result = snapEngine_.findSpatialSnapPoint(
                document_, worldToScreen(rawPoint), anchor,
                viewportTransform_, size());
            traceSnapResult(result, true);
            return result;
        }

        const bool arcPlaneConstraintActive =
            activeTool_ == Tool::Arc &&
            (arcPlaneNormalLockKey_ != 0 || arcAxisConstraintKey_ != 0 ||
             arcVerticalOverrideAxis_ != 0 || arcPerpendicularPlaneActive_);
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
            const Point3D *anchor = linePreviewWorldPoints_.isEmpty()
                ? nullptr : &linePreviewWorldPoints_.back();
            const SnapResult result = snapEngine_.findSpatialSnapPoint(
                document_, worldToScreen(rawPoint), anchor,
                viewportTransform_, size(), linePreviewWorldPoints_);
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
                                            !pointPreviewWorldPoints_.isEmpty()
                                        ? &pointPreviewWorldPoints_.back()
                                        : nullptr;
            const SnapResult result = snapEngine_.findSpatialSnapPoint(
                document_, worldToScreen(rawPoint), anchor,
                viewportTransform_, size(), pointPreviewWorldPoints_);
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
            const QPointF candidateScreen = worldToScreen(candidate.point);
            const qreal distance = std::hypot(candidateScreen.x() - cursorScreen.x(),
                                              candidateScreen.y() - cursorScreen.y());
            if (distance <= bestDistance) {
                bestDistance = distance;
                best.type = candidate.type;
                best.point = candidate.point;
            }
        }
        return best;
    }

    QVector<int> grabSelectedShapeIndices() const
    {
        QVector<int> selectedIndices;
        selectedIndices.reserve(draggingShapeIndices_.size());
        for (const ObjectId objectId : draggingShapeIndices_) {
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
        selectedIndices.reserve(duplicateSourceObjects_.size());
        for (const SceneObject &source : duplicateSourceObjects_) {
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
                                         QVector<QPointF>{duplicateBasePoint_},
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
                                         QVector<QPointF>{grabBasePoint_},
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
            (arcMode_ == ArcMode::TwoPoint ||
             arcMode_ == ArcMode::ThreePoint) &&
            pendingPoints_.size() == 1 && !panning_) {
            return constrainArcChordEndpoint(
                rawPoint,
                altModifier,
                currentArcScreenPosition(screenPosition));
        }

        if (activeTool_ == Tool::Arc && arcMode_ == ArcMode::TwoPoint &&
            pendingPoints_.size() >= 2 && !panning_) {
            return constrainTwoPointArcThroughPoint(snappedOrRawPoint,
                                                    currentSnap_.isValid(),
                                                    altModifier);
        }

        if (activeTool_ == Tool::Arc && arcMode_ == ArcMode::ThreePoint &&
            pendingPoints_.size() >= 2 && !panning_) {
            return snappedOrRawPoint;
        }

        if (activeTool_ == Tool::Arc && arcMode_ == ArcMode::OnePoint &&
            pendingPoints_.size() >= 2 && arcAngleValueLocked_ && !panning_) {
            const QPointF center = pendingPoints_[0];
            const QPointF radiusVector = pendingPoints_[1] - center;
            const qreal radius = std::hypot(radiusVector.x(), radiusVector.y());
            if (radius > 1.0e-9) {
                currentSnap_ = SnapResult{};
                const qreal startAngle = std::atan2(radiusVector.y(),
                                                    radiusVector.x());
                const qreal endAngle = startAngle + arcPreviewSweepAngle_;
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

        if (activeTool_ == Tool::Arc && arcMode_ == ArcMode::OnePoint &&
            pendingPoints_.size() == 1 && !panning_) {
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
            } else if (arcAngleSnapEnabled_) {
                angle = snapOnePointArcAngle(angle);
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
        if (!orthoEnabled_ || panning_ || !drawingConstraintActive || pendingPoints_.isEmpty()) {
            return rawPoint;
        }

        if (activeTool_ == Tool::Arc && arcMode_ == ArcMode::OnePoint &&
            pendingPoints_.size() >= 2) {
            return constrainOnePointArcEndpoint(rawPoint);
        }

        const QPointF origin = activeTool_ == Tool::AngularDimension
                                   ? pendingPoints_.first()
                                   : pendingPoints_.back();
        const qreal deltaX = rawPoint.x() - origin.x();
        const qreal deltaY = rawPoint.y() - origin.y();

        if (std::abs(deltaX) >= std::abs(deltaY)) {
            return QPointF(rawPoint.x(), origin.y());
        }

        return QPointF(origin.x(), rawPoint.y());
    }

    QPointF constrainOnePointArcEndpoint(const QPointF &rawPoint) const
    {
        const QPointF center = pendingPoints_[0];
        const QPointF startVector = pendingPoints_[1] - center;
        const qreal radius = std::hypot(startVector.x(), startVector.y());
        if (radius <= 1e-9) {
            return rawPoint;
        }

        constexpr qreal pi = 3.14159265358979323846;
        constexpr qreal halfPi = pi / 2.0;
        constexpr qreal twoPi = 2.0 * pi;
        const qreal startAngle = std::atan2(startVector.y(), startVector.x());
        const QPointF rawVector = rawPoint - center;
        const qreal rawAngle = std::atan2(rawVector.y(), rawVector.x());

        // Use the previous constrained angle as the reference for the next
        // cursor sample. This keeps the sweep unwrapped while the cursor
        // crosses +/-180 degrees, so 270 and 360 degree quarter-turns remain
        // reachable instead of jumping back to the principal angle.
        qreal candidateSweep = rawAngle - startAngle;
        if (arcPreviewInitialized_) {
            qreal delta = rawAngle - arcPreviewPreviousAngle_;
            if (delta > pi) {
                delta -= twoPi;
            } else if (delta < -pi) {
                delta += twoPi;
            }
            candidateSweep = arcPreviewSweepAngle_ + delta;
        } else {
            if (candidateSweep > pi) {
                candidateSweep -= twoPi;
            } else if (candidateSweep < -pi) {
                candidateSweep += twoPi;
            }
        }

        const qreal snappedSweep = std::round(candidateSweep / halfPi) * halfPi;
        const qreal snappedAngle = startAngle + snappedSweep;
        return center + QPointF(radius * std::cos(snappedAngle),
                                radius * std::sin(snappedAngle));
    }

    QPointF constrainTwoPointArcThroughPoint(const QPointF &cursorPoint,
                                             bool geometrySnap,
                                             bool altModifier) const
    {
        // radCAD's 2-point mode treats the final pick as the arc's height:
        // only its distance perpendicular to the endpoint chord matters.
        // Keep this calculation in the captured workplane coordinates so a
        // perspective or orthographic view cannot skew the committed circle.
        const QPointF chord = pendingPoints_[1] - pendingPoints_[0];
        const qreal chordLength = std::hypot(chord.x(), chord.y());
        if (!std::isfinite(chordLength) || chordLength <= 1e-9) {
            return cursorPoint;
        }

        const QPointF midpoint = (pendingPoints_[0] + pendingPoints_[1]) / 2.0;
        const QPointF perpendicular(-chord.y() / chordLength,
                                    chord.x() / chordLength);
        qreal height = QPointF::dotProduct(cursorPoint - midpoint, perpendicular);
        const qreal halfChord = chordLength / 2.0;
        if (!geometrySnap && !altModifier) {
            constexpr qreal defaultSnapStrengthPercent = 6.0;
            const qreal heightSnapTolerance =
                chordLength * defaultSnapStrengthPercent / 100.0;
            if (std::abs(std::abs(height) - halfChord) < heightSnapTolerance) {
                height = std::copysign(halfChord, height);
            }
        }

        return midpoint + perpendicular * height;
    }

    void refreshCursorConstraint()
    {
        if (!cursorValid_) {
            currentSnap_ = SnapResult{};
            return;
        }

        const QPointF screenPosition = currentArcScreenPosition();
        if (activeTool_ == Tool::Arc &&
            (arcMode_ == ArcMode::TwoPoint ||
             arcMode_ == ArcMode::ThreePoint) &&
            pendingPoints_.size() == 1 &&
            !(arcMode_ == ArcMode::TwoPoint &&
              arcPerpendicularPlaneActive_)) {
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
                const WorkPlaneFrame targetFrame = shapeWorkPlaneFrame(shape);
                ViewportTransform shapeTransform = viewportTransform_;
                shapeTransform.setWorkPlaneFrame(targetFrame);
                const qreal distancePixels =
                    visible ? curveHitTester_.distanceToShape(screenPosition,
                                                             shape,
                                                             shapeTransform,
                                                             size())
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

    bool insertNurbsKnot(QVector<HomogeneousControlPoint2D> *controlPoints,
                         QVector<double> *knots,
                         int degree,
                         qreal parameter) const
    {
        if (controlPoints == nullptr || knots == nullptr ||
            controlPoints->isEmpty() || knots->isEmpty() || degree < 1) {
            return false;
        }

        const auto sameKnot = [](qreal first, qreal second) {
            const qreal tolerance = 1.0e-9 *
                                     std::max<qreal>(1.0,
                                                     std::max(std::abs(first),
                                                              std::abs(second)));
            return std::abs(first - second) <= tolerance;
        };

        const int n = controlPoints->size() - 1;
        const int m = knots->size() - 1;
        if (m != n + degree + 1) {
            return false;
        }

        int span = degree;
        if (parameter >= knots->at(n + 1)) {
            span = n;
        } else {
            int low = degree;
            int high = n + 1;
            int middle = (low + high) / 2;
            while (parameter < knots->at(middle) || parameter >= knots->at(middle + 1)) {
                if (parameter < knots->at(middle)) {
                    high = middle;
                } else {
                    low = middle;
                }
                middle = (low + high) / 2;
            }
            span = middle;
        }

        int multiplicity = 0;
        for (const double knot : *knots) {
            if (sameKnot(knot, parameter)) {
                ++multiplicity;
            }
        }
        if (multiplicity >= degree) {
            return false;
        }

        QVector<HomogeneousControlPoint2D> insertedControlPoints(n + 2);
        for (int index = 0; index <= span - degree; ++index) {
            insertedControlPoints[index] = controlPoints->at(index);
        }
        for (int index = span - multiplicity; index <= n; ++index) {
            insertedControlPoints[index + 1] = controlPoints->at(index);
        }
        for (int index = span - degree + 1; index <= span - multiplicity; ++index) {
            const qreal denominator = knots->at(index + degree) - knots->at(index);
            if (std::abs(denominator) <= 1.0e-12) {
                return false;
            }
            const qreal alpha = (parameter - knots->at(index)) / denominator;
            insertedControlPoints[index] = blendHomogeneousControlPoints(
                controlPoints->at(index - 1),
                controlPoints->at(index),
                alpha);
        }

        QVector<double> insertedKnots(m + 2);
        for (int index = 0; index <= span; ++index) {
            insertedKnots[index] = knots->at(index);
        }
        insertedKnots[span + 1] = parameter;
        for (int index = span + 1; index <= m; ++index) {
            insertedKnots[index + 1] = knots->at(index);
        }

        *controlPoints = insertedControlPoints;
        *knots = insertedKnots;
        return true;
    }

    bool rationalBezierSpansForCurve(
        const Shape::NurbsCurve2D &curve,
        QVector<RationalBezierSpan2D> *spans) const
    {
        if (spans == nullptr || !isValidNurbsCurve(curve)) {
            return false;
        }

        spans->clear();
        QVector<HomogeneousControlPoint2D> controlPoints;
        controlPoints.reserve(curve.controlPoints.size());
        for (int index = 0; index < curve.controlPoints.size(); ++index) {
            const qreal weight = curve.rational ? curve.weights[index] : 1.0;
            controlPoints.append(HomogeneousControlPoint2D{
                curve.controlPoints[index] * weight,
                weight});
        }

        QVector<double> knots = expandedKnotVector(curve);
        const qreal domainStart = knots[curve.degree];
        const qreal domainEnd = knots[curve.controlPoints.size()];
        const auto sameKnot = [](qreal first, qreal second) {
            const qreal tolerance = 1.0e-9 *
                                     std::max<qreal>(1.0,
                                                     std::max(std::abs(first),
                                                              std::abs(second)));
            return std::abs(first - second) <= tolerance;
        };

        QVector<double> internalKnots;
        for (const double knot : knots) {
            if (knot <= domainStart || knot >= domainEnd) {
                continue;
            }
            if (internalKnots.isEmpty() || !sameKnot(internalKnots.back(), knot)) {
                internalKnots.append(knot);
            }
        }

        for (const double internalKnot : internalKnots) {
            int multiplicity = 0;
            for (const double knot : knots) {
                if (sameKnot(knot, internalKnot)) {
                    ++multiplicity;
                }
            }
            while (multiplicity < curve.degree) {
                if (!insertNurbsKnot(&controlPoints,
                                     &knots,
                                     curve.degree,
                                     internalKnot)) {
                    return false;
                }
                ++multiplicity;
            }
        }

        const qreal knotTolerance = 1.0e-10;
        for (int spanIndex = curve.degree;
             spanIndex < controlPoints.size();
             ++spanIndex) {
            if (knots[spanIndex + 1] - knots[spanIndex] <= knotTolerance) {
                continue;
            }

            RationalBezierSpan2D span;
            span.startParameter = knots[spanIndex];
            span.endParameter = knots[spanIndex + 1];
            span.controlPoints.reserve(curve.degree + 1);
            for (int controlIndex = spanIndex - curve.degree;
                 controlIndex <= spanIndex;
                 ++controlIndex) {
                span.controlPoints.append(controlPoints[controlIndex]);
            }
            spans->append(span);
        }

        return !spans->isEmpty();
    }

    bool splitRationalBezierSpan(
        const QVector<HomogeneousControlPoint2D> &source,
        qreal fraction,
        QVector<HomogeneousControlPoint2D> *left,
        QVector<HomogeneousControlPoint2D> *right) const
    {
        if (source.size() < 2 || left == nullptr || right == nullptr ||
            fraction <= 0.0 || fraction >= 1.0) {
            return false;
        }

        QVector<HomogeneousControlPoint2D> working = source;
        left->resize(source.size());
        right->resize(source.size());
        (*left)[0] = working.first();
        (*right)[source.size() - 1] = working.last();

        for (int level = 1; level < source.size(); ++level) {
            for (int index = 0; index + 1 < working.size(); ++index) {
                working[index] = blendHomogeneousControlPoints(
                    working[index],
                    working[index + 1],
                    fraction);
            }
            (*left)[level] = working.first();
            (*right)[source.size() - 1 - level] = working[source.size() - 1 - level];
        }
        return true;
    }

    bool trimNurbsCurve(const Shape::NurbsCurve2D &source,
                        qreal startParameter,
                        qreal endParameter,
                        Shape::NurbsCurve2D *trimmed) const
    {
        if (trimmed == nullptr || !isValidNurbsCurve(source)) {
            return false;
        }

        const QVector<double> fullKnots = expandedKnotVector(source);
        const qreal domainStart = fullKnots[source.degree];
        const qreal domainEnd = fullKnots[source.controlPoints.size()];
        const qreal domainTolerance =
            std::max<qreal>(1.0e-9, std::abs(domainEnd - domainStart) * 1.0e-9);
        const qreal start = std::clamp(startParameter, domainStart, domainEnd);
        const qreal end = std::clamp(endParameter, domainStart, domainEnd);
        if (end - start <= domainTolerance) {
            return false;
        }

        QVector<RationalBezierSpan2D> spans;
        if (!rationalBezierSpansForCurve(source, &spans)) {
            return false;
        }

        QVector<RationalBezierSpan2D> trimmedSpans;
        for (const RationalBezierSpan2D &span : spans) {
            const qreal overlapStart = std::max(start, span.startParameter);
            const qreal overlapEnd = std::min(end, span.endParameter);
            if (overlapEnd - overlapStart <= domainTolerance) {
                continue;
            }

            const qreal spanLength = span.endParameter - span.startParameter;
            qreal localStart = (overlapStart - span.startParameter) / spanLength;
            qreal localEnd = (overlapEnd - span.startParameter) / spanLength;
            QVector<HomogeneousControlPoint2D> working = span.controlPoints;

            if (localStart > 1.0e-10) {
                QVector<HomogeneousControlPoint2D> discarded;
                QVector<HomogeneousControlPoint2D> remaining;
                if (!splitRationalBezierSpan(working,
                                              localStart,
                                              &discarded,
                                              &remaining)) {
                    return false;
                }
                working = remaining;
                localEnd = (localEnd - localStart) / (1.0 - localStart);
            }

            if (localEnd < 1.0 - 1.0e-10) {
                QVector<HomogeneousControlPoint2D> remaining;
                QVector<HomogeneousControlPoint2D> discarded;
                if (!splitRationalBezierSpan(working,
                                              localEnd,
                                              &remaining,
                                              &discarded)) {
                    return false;
                }
                working = remaining;
            }

            RationalBezierSpan2D trimmedSpan;
            trimmedSpan.startParameter = overlapStart;
            trimmedSpan.endParameter = overlapEnd;
            trimmedSpan.controlPoints = working;
            trimmedSpans.append(trimmedSpan);
        }

        if (trimmedSpans.isEmpty()) {
            return false;
        }

        Shape::NurbsCurve2D result;
        result.dimension = source.dimension;
        result.degree = source.degree;
        result.order = source.order;
        result.rational = source.rational;

        for (int spanIndex = 0; spanIndex < trimmedSpans.size(); ++spanIndex) {
            const QVector<HomogeneousControlPoint2D> &spanPoints =
                trimmedSpans[spanIndex].controlPoints;
            const int firstPoint = spanIndex == 0 ? 0 : 1;
            for (int pointIndex = firstPoint; pointIndex < spanPoints.size(); ++pointIndex) {
                const HomogeneousControlPoint2D &point = spanPoints[pointIndex];
                if (std::abs(point.weight) <= 1.0e-12) {
                    return false;
                }
                result.controlPoints.append(point.weightedPosition / point.weight);
                result.weights.append(source.rational ? point.weight : 1.0);
            }
        }

        QVector<double> resultFullKnots;
        const auto appendRepeated = [&resultFullKnots](qreal value, int count) {
            for (int index = 0; index < count; ++index) {
                resultFullKnots.append(value);
            }
        };
        appendRepeated(trimmedSpans.first().startParameter, source.degree + 1);
        for (int spanIndex = 0; spanIndex + 1 < trimmedSpans.size(); ++spanIndex) {
            appendRepeated(trimmedSpans[spanIndex].endParameter, source.degree);
        }
        appendRepeated(trimmedSpans.last().endParameter, source.degree + 1);

        if (resultFullKnots.size() < 2) {
            return false;
        }
        result.knots = resultFullKnots;
        result.knots.removeFirst();
        result.knots.removeLast();
        if (!isValidNurbsCurve(result)) {
            return false;
        }

        *trimmed = result;
        return true;
    }

    qreal distanceToEraserStroke(const QPointF &screenPoint,
                                 const QVector<QPointF> &stroke) const
    {
        if (stroke.isEmpty()) {
            return 1.0e9;
        }
        if (stroke.size() == 1) {
            return distanceToSegment(screenPoint, stroke.first(), stroke.first());
        }

        qreal distance = 1.0e9;
        for (int index = 1; index < stroke.size(); ++index) {
            distance = std::min(distance,
                                distanceToSegment(screenPoint,
                                                  stroke[index - 1],
                                                  stroke[index]));
        }
        return distance;
    }

    bool sampleNurbsCurveForErase(const Shape::NurbsCurve2D &curve,
                                  const WorkPlaneFrame &workPlaneFrame,
                                  SampledNurbsCurve2D *sampled) const
    {
        return curveSampler_.sampleNurbsCurve(curve,
                                              workPlaneFrame,
                                              viewportTransform_,
                                              size(),
                                              sampled);
    }

    QVector<Shape::NurbsCurve2D> eraseIntersectionCurvesForShape(
        const Shape &shape) const
    {
        if (shape.geometryType == GeometryType::PolyCurve) {
            return shape.components;
        }

        if (isValidNurbsCurve(shape.nurbs)) {
            return {shape.nurbs};
        }

        if (shape.geometryType == GeometryType::Line && shape.points.size() >= 2) {
            return {makeDegreeOneNurbs(shape.points)};
        }

        if ((shape.geometryType == GeometryType::Bezier || shape.geometryType == GeometryType::Nurbs) &&
            shape.points.size() >= 2) {
            return {makeBezierNurbs(shape.points)};
        }

        if (shape.geometryType == GeometryType::Polygon ||
            (shape.geometryType == GeometryType::Rectangle &&
             shape.points.size() >= 2)) {
            const QVector<QPointF> vertices = shape.geometryType == GeometryType::Polygon
                                                  ? polygonVerticesForShape(shape)
                                                  : rectangleVertices(shape);
            const int minimumVertices = shape.geometryType == GeometryType::Polygon ? 3 : 4;
            if (vertices.size() < minimumVertices) {
                return {};
            }

            QVector<Shape::NurbsCurve2D> curves;
            curves.reserve(vertices.size());
            for (int index = 0; index < vertices.size(); ++index) {
                curves.append(makeDegreeOneNurbs(
                    {vertices[index], vertices[(index + 1) % vertices.size()]}));
            }
            return curves;
        }

        return {};
    }

    QVector<qreal> eraseIntersectionParameters(
        int sourceShapeIndex,
        int sourceComponentIndex,
        const Shape::NurbsCurve2D &sourceCurve,
        const QVector<EraseCurveSampleCache> *sceneCache = nullptr,
        QVector<quint64> *intersectionObjectIds = nullptr,
        int *nurbsSeedSolves = nullptr,
        int *visiblePointChecks = nullptr) const
    {
        QVector<qreal> parameters;
        if (sourceShapeIndex < 0 || sourceShapeIndex >= shapes_.size() ||
            !isValidNurbsCurve(sourceCurve)) {
            return parameters;
        }

        const WorkPlaneFrame sourceFrame =
            shapeWorkPlaneFrame(shapes_[sourceShapeIndex]);
        if (!isValidWorkPlaneFrame(sourceFrame)) {
            return parameters;
        }

        qreal sourceDomainStart = 0.0;
        qreal sourceDomainEnd = 0.0;
        if (!nurbsParameterDomain(sourceCurve,
                                  &sourceDomainStart,
                                  &sourceDomainEnd)) {
            return parameters;
        }
        const qreal sourceDomainLength = sourceDomainEnd - sourceDomainStart;
        if (sourceDomainLength <= 0.0) {
            return parameters;
        }

        const auto appendUniqueParameter = [&](qreal parameter) {
            const qreal parameterTolerance =
                std::max<qreal>(1.0e-12, sourceDomainLength * 1.0e-9);
            for (const qreal existingParameter : parameters) {
                if (std::abs(existingParameter - parameter) <=
                    parameterTolerance) {
                    return;
                }
            }
            parameters.append(std::clamp(parameter,
                                         sourceDomainStart,
                                         sourceDomainEnd));
        };

        struct ParameterSpan {
            qreal start = 0.0;
            qreal end = 0.0;
            qreal minimumX = 0.0;
            qreal minimumY = 0.0;
            qreal maximumX = 0.0;
            qreal maximumY = 0.0;
        };
        struct CurveCandidate {
            int shapeIndex = -1;
            int componentIndex = -1;
            WorkPlaneFrame frame;
            Shape::NurbsCurve2D curve;
        };

        const auto curvePointInSourceFrame =
            [&](const Shape::NurbsCurve2D &curve,
                const WorkPlaneFrame &frame,
                qreal parameter,
                QPointF *point) {
                QPointF localPoint;
                if (point == nullptr ||
                    !evaluateNurbsPoint(curve, parameter, &localPoint)) {
                    return false;
                }
                *point = worldPointToWorkPlaneFrame(
                    workPlaneFramePointToWorld(localPoint, frame), sourceFrame);
                return std::isfinite(point->x()) && std::isfinite(point->y());
            };
        const auto curveDerivativeInSourceFrame =
            [&](const Shape::NurbsCurve2D &curve,
                const WorkPlaneFrame &frame,
                qreal parameter,
                QPointF *derivative) {
                QPointF localDerivative;
                if (derivative == nullptr ||
                    !evaluateNurbsDerivative(curve,
                                             parameter,
                                             &localDerivative)) {
                    return false;
                }
                const Point3D worldDerivative{
                    frame.xAxis.x * localDerivative.x() +
                        frame.yAxis.x * localDerivative.y(),
                    frame.xAxis.y * localDerivative.x() +
                        frame.yAxis.y * localDerivative.y(),
                    frame.xAxis.z * localDerivative.x() +
                        frame.yAxis.z * localDerivative.y()};
                *derivative = QPointF(
                    worldDerivative.x * sourceFrame.xAxis.x +
                        worldDerivative.y * sourceFrame.xAxis.y +
                        worldDerivative.z * sourceFrame.xAxis.z,
                    worldDerivative.x * sourceFrame.yAxis.x +
                        worldDerivative.y * sourceFrame.yAxis.y +
                        worldDerivative.z * sourceFrame.yAxis.z);
                return std::isfinite(derivative->x()) &&
                       std::isfinite(derivative->y());
            };
        const auto parameterSpans =
            [&](const Shape::NurbsCurve2D &curve,
                const WorkPlaneFrame &frame) {
                QVector<ParameterSpan> spans;
                if (!isValidNurbsCurve(curve) ||
                    !isValidWorkPlaneFrame(frame)) {
                    return spans;
                }
                const QVector<double> fullKnots =
                    expandedNurbsKnotVector(curve);
                for (int knotIndex = curve.degree;
                     knotIndex < curve.controlPoints.size();
                     ++knotIndex) {
                    const qreal spanStart = fullKnots[knotIndex];
                    const qreal spanEnd = fullKnots[knotIndex + 1];
                    if (spanEnd - spanStart <= 1.0e-12) {
                        continue;
                    }

                    ParameterSpan span;
                    span.start = spanStart;
                    span.end = spanEnd;
                    span.minimumX = std::numeric_limits<qreal>::infinity();
                    span.minimumY = std::numeric_limits<qreal>::infinity();
                    span.maximumX = -std::numeric_limits<qreal>::infinity();
                    span.maximumY = -std::numeric_limits<qreal>::infinity();
                    for (int controlPointIndex =
                             knotIndex - curve.degree;
                         controlPointIndex <= knotIndex;
                         ++controlPointIndex) {
                        const QPointF point = worldPointToWorkPlaneFrame(
                            workPlaneFramePointToWorld(
                                curve.controlPoints[controlPointIndex], frame),
                            sourceFrame);
                        span.minimumX = std::min(span.minimumX, point.x());
                        span.minimumY = std::min(span.minimumY, point.y());
                        span.maximumX = std::max(span.maximumX, point.x());
                        span.maximumY = std::max(span.maximumY, point.y());
                    }
                    spans.append(span);
                }
                return spans;
            };
        const auto hullsOverlap = [](const ParameterSpan &first,
                                     const ParameterSpan &second,
                                     qreal tolerance) {
            return first.maximumX + tolerance >= second.minimumX &&
                   second.maximumX + tolerance >= first.minimumX &&
                   first.maximumY + tolerance >= second.minimumY &&
                   second.maximumY + tolerance >= first.minimumY;
        };

        QVector<CurveCandidate> otherCurves;
        if (sceneCache != nullptr) {
            otherCurves.reserve(sceneCache->size());
            for (const EraseCurveSampleCache &cachedCurve : *sceneCache) {
                if (cachedCurve.shapeIndex < 0 ||
                    cachedCurve.shapeIndex >= shapes_.size()) {
                    continue;
                }
                CurveCandidate candidate;
                candidate.shapeIndex = cachedCurve.shapeIndex;
                candidate.componentIndex = cachedCurve.componentIndex;
                candidate.frame = isValidWorkPlaneFrame(
                                      cachedCurve.workPlaneFrame)
                                      ? cachedCurve.workPlaneFrame
                                      : shapeWorkPlaneFrame(
                                            shapes_[cachedCurve.shapeIndex]);
                candidate.curve = cachedCurve.curve;
                otherCurves.append(candidate);
            }
        } else {
            for (int shapeIndex = 0; shapeIndex < shapes_.size(); ++shapeIndex) {
                if (!document_.isObjectVisible(shapes_.objectIdAt(shapeIndex))) {
                    continue;
                }
                const QVector<Shape::NurbsCurve2D> curves =
                    eraseIntersectionCurvesForShape(shapes_[shapeIndex]);
                const WorkPlaneFrame frame =
                    shapeWorkPlaneFrame(shapes_[shapeIndex]);
                for (int componentIndex = 0;
                     componentIndex < curves.size();
                     ++componentIndex) {
                    CurveCandidate candidate;
                    candidate.shapeIndex = shapeIndex;
                    candidate.componentIndex = componentIndex;
                    candidate.frame = frame;
                    candidate.curve = curves[componentIndex];
                    otherCurves.append(candidate);
                }
            }
        }

        constexpr qreal seedFractions[] = {0.0, 0.25, 0.5, 0.75, 1.0};
        const auto refineNurbsIntersection =
            [&](const Shape::NurbsCurve2D &otherCurve,
                const WorkPlaneFrame &otherFrame,
                const ParameterSpan &sourceSpan,
                const ParameterSpan &otherSpan,
                qreal sourceSeed,
                qreal otherSeed,
                qreal geometryTolerance,
                qreal *sourceParameter) {
                qreal sourceFraction = sourceSeed;
                qreal otherFraction = otherSeed;
                qreal damping = 1.0e-4;
                const qreal sourceSpanLength =
                    sourceSpan.end - sourceSpan.start;
                const qreal otherSpanLength =
                    otherSpan.end - otherSpan.start;
                const auto evaluatePair =
                    [&](qreal sourceFractionValue,
                        qreal otherFractionValue,
                        QPointF *sourcePoint,
                        QPointF *otherPoint) {
                        return curvePointInSourceFrame(
                                   sourceCurve,
                                   sourceFrame,
                                   sourceSpan.start +
                                       sourceSpanLength * sourceFractionValue,
                                   sourcePoint) &&
                               curvePointInSourceFrame(
                                   otherCurve,
                                   otherFrame,
                                   otherSpan.start +
                                       otherSpanLength * otherFractionValue,
                                   otherPoint);
                    };

                for (int iteration = 0; iteration < 40; ++iteration) {
                    const qreal currentSourceParameter =
                        sourceSpan.start + sourceSpanLength * sourceFraction;
                    const qreal currentOtherParameter =
                        otherSpan.start + otherSpanLength * otherFraction;
                    QPointF sourcePoint;
                    QPointF otherPoint;
                    QPointF sourceDerivative;
                    QPointF otherDerivative;
                    if (!evaluatePair(sourceFraction,
                                      otherFraction,
                                      &sourcePoint,
                                      &otherPoint)) {
                        return false;
                    }
                    const QPointF residual = sourcePoint - otherPoint;
                    const qreal currentDistanceSquared =
                        QPointF::dotProduct(residual, residual);
                    if (currentDistanceSquared <=
                        geometryTolerance * geometryTolerance) {
                        *sourceParameter = currentSourceParameter;
                        return true;
                    }
                    if (!curveDerivativeInSourceFrame(sourceCurve,
                                                      sourceFrame,
                                                      currentSourceParameter,
                                                      &sourceDerivative) ||
                        !curveDerivativeInSourceFrame(otherCurve,
                                                      otherFrame,
                                                      currentOtherParameter,
                                                      &otherDerivative)) {
                        return false;
                    }

                    const QPointF sourceJacobian =
                        sourceDerivative * sourceSpanLength;
                    const QPointF otherJacobian =
                        otherDerivative * -otherSpanLength;
                    const qreal h00 =
                        QPointF::dotProduct(sourceJacobian,
                                            sourceJacobian);
                    const qreal h01 =
                        QPointF::dotProduct(sourceJacobian,
                                            otherJacobian);
                    const qreal h11 =
                        QPointF::dotProduct(otherJacobian,
                                            otherJacobian);
                    const qreal g0 =
                        QPointF::dotProduct(sourceJacobian, residual);
                    const qreal g1 =
                        QPointF::dotProduct(otherJacobian, residual);
                    const qreal hessianScale =
                        std::max<qreal>({h00, h11, 1.0e-24});

                    bool acceptedStep = false;
                    for (int dampingAttempt = 0;
                         dampingAttempt < 8 && !acceptedStep;
                         ++dampingAttempt) {
                        const qreal diagonal00 =
                            h00 + damping * hessianScale;
                        const qreal diagonal11 =
                            h11 + damping * hessianScale;
                        const qreal determinant =
                            diagonal00 * diagonal11 - h01 * h01;
                        if (std::abs(determinant) <= 1.0e-30) {
                            damping *= 10.0;
                            continue;
                        }
                        const qreal sourceStep =
                            (-g0 * diagonal11 + h01 * g1) / determinant;
                        const qreal otherStep =
                            (h01 * g0 - diagonal00 * g1) / determinant;
                        if (!std::isfinite(sourceStep) ||
                            !std::isfinite(otherStep)) {
                            damping *= 10.0;
                            continue;
                        }

                        for (int lineSearch = 0;
                             lineSearch < 9;
                             ++lineSearch) {
                            const qreal fraction =
                                std::ldexp(1.0, -lineSearch);
                            const qreal candidateSourceFraction =
                                std::clamp(sourceFraction +
                                               sourceStep * fraction,
                                           0.0,
                                           1.0);
                            const qreal candidateOtherFraction =
                                std::clamp(otherFraction +
                                               otherStep * fraction,
                                           0.0,
                                           1.0);
                            if (candidateSourceFraction == sourceFraction &&
                                candidateOtherFraction == otherFraction) {
                                continue;
                            }
                            QPointF candidateSourcePoint;
                            QPointF candidateOtherPoint;
                            if (!evaluatePair(candidateSourceFraction,
                                              candidateOtherFraction,
                                              &candidateSourcePoint,
                                              &candidateOtherPoint)) {
                                continue;
                            }
                            const QPointF candidateResidual =
                                candidateSourcePoint - candidateOtherPoint;
                            const qreal candidateDistanceSquared =
                                QPointF::dotProduct(candidateResidual,
                                                    candidateResidual);
                            if (candidateDistanceSquared <
                                    currentDistanceSquared ||
                                candidateDistanceSquared <=
                                    geometryTolerance * geometryTolerance) {
                                sourceFraction =
                                    candidateSourceFraction;
                                otherFraction =
                                    candidateOtherFraction;
                                damping = std::max<qreal>(
                                    1.0e-12, damping * 0.25);
                                acceptedStep = true;
                                break;
                            }
                        }
                        if (!acceptedStep) {
                            damping *= 10.0;
                        }
                    }
                    if (!acceptedStep) {
                        return false;
                    }
                }

                QPointF sourcePoint;
                QPointF otherPoint;
                if (!evaluatePair(sourceFraction,
                                  otherFraction,
                                  &sourcePoint,
                                  &otherPoint) ||
                    std::hypot(sourcePoint.x() - otherPoint.x(),
                               sourcePoint.y() - otherPoint.y()) >
                        geometryTolerance) {
                    return false;
                }
                *sourceParameter =
                    sourceSpan.start + sourceSpanLength * sourceFraction;
                return true;
            };

        const QVector<ParameterSpan> sourceSpans =
            parameterSpans(sourceCurve, sourceFrame);
        for (const CurveCandidate &other : otherCurves) {
            if (other.shapeIndex == sourceShapeIndex &&
                other.componentIndex == sourceComponentIndex) {
                continue;
            }
            if (!workPlaneFramesCoplanar(sourceFrame, other.frame)) {
                continue;
            }
            const QVector<ParameterSpan> otherSpans =
                parameterSpans(other.curve, other.frame);
            for (const ParameterSpan &sourceSpan : sourceSpans) {
                for (const ParameterSpan &otherSpan : otherSpans) {
                    qreal coordinateScale = 1.0;
                    for (const qreal value : {
                             sourceSpan.minimumX,
                             sourceSpan.minimumY,
                             sourceSpan.maximumX,
                             sourceSpan.maximumY,
                             otherSpan.minimumX,
                             otherSpan.minimumY,
                             otherSpan.maximumX,
                             otherSpan.maximumY}) {
                        coordinateScale =
                            std::max(coordinateScale, std::abs(value));
                    }
                    const qreal geometryTolerance =
                        std::max<qreal>(1.0e-7,
                                        coordinateScale * 1.0e-10);
                    if (!hullsOverlap(sourceSpan,
                                      otherSpan,
                                      geometryTolerance)) {
                        continue;
                    }

                    for (const qreal sourceSeed : seedFractions) {
                        for (const qreal otherSeed : seedFractions) {
                            if (nurbsSeedSolves != nullptr) {
                                ++*nurbsSeedSolves;
                            }
                            qreal intersectionParameter = 0.0;
                            if (refineNurbsIntersection(
                                    other.curve,
                                    other.frame,
                                    sourceSpan,
                                    otherSpan,
                                    sourceSeed,
                                    otherSeed,
                                    geometryTolerance,
                                    &intersectionParameter)) {
                                appendUniqueParameter(intersectionParameter);
                                if (intersectionObjectIds != nullptr &&
                                    other.shapeIndex != sourceShapeIndex) {
                                    const quint64 objectId =
                                        shapes_.objectIdAt(other.shapeIndex).value();
                                    if (!intersectionObjectIds->contains(objectId)) {
                                        intersectionObjectIds->append(objectId);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        const auto squaredDistanceAtParameter =
            [&](qreal parameter, const QPointF &point) {
                QPointF curvePoint;
                if (!evaluateNurbsPoint(sourceCurve,
                                        parameter,
                                        &curvePoint)) {
                    return std::numeric_limits<qreal>::infinity();
                }
                const QPointF delta = curvePoint - point;
                return QPointF::dotProduct(delta, delta);
            };
        const auto goldenMinimum =
            [&](qreal low, qreal high, const QPointF &point) {
                constexpr qreal ratio = 0.6180339887498948482;
                qreal first = high - (high - low) * ratio;
                qreal second = low + (high - low) * ratio;
                qreal firstValue =
                    squaredDistanceAtParameter(first, point);
                qreal secondValue =
                    squaredDistanceAtParameter(second, point);
                for (int iteration = 0; iteration < 36; ++iteration) {
                    if (firstValue <= secondValue) {
                        high = second;
                        second = first;
                        secondValue = firstValue;
                        first = high - (high - low) * ratio;
                        firstValue =
                            squaredDistanceAtParameter(first, point);
                    } else {
                        low = first;
                        first = second;
                        firstValue = secondValue;
                        second = low + (high - low) * ratio;
                        secondValue =
                            squaredDistanceAtParameter(second, point);
                    }
                }
                return (low + high) * 0.5;
            };

        for (int shapeIndex = 0; shapeIndex < shapes_.size(); ++shapeIndex) {
            if (shapeIndex == sourceShapeIndex ||
                !document_.isObjectVisible(shapes_.objectIdAt(shapeIndex))) {
                continue;
            }
            const Shape &pointShape = shapes_[shapeIndex];
            if (pointShape.geometryType != GeometryType::Point ||
                pointShape.points.isEmpty()) {
                continue;
            }
            if (visiblePointChecks != nullptr) {
                ++*visiblePointChecks;
            }

            const Point3D pointWorld =
                shapePointToWorld(pointShape, pointShape.points.first());
            const qreal worldCoordinateScale = std::max<qreal>(
                {1.0, std::abs(pointWorld.x), std::abs(pointWorld.y),
                 std::abs(pointWorld.z), std::abs(sourceFrame.origin.x),
                 std::abs(sourceFrame.origin.y),
                 std::abs(sourceFrame.origin.z)});
            const qreal planeTolerance =
                std::max<qreal>(1.0e-7,
                                worldCoordinateScale * 1.0e-12);
            const qreal planeDistance =
                signedDistanceFromWorkPlaneFrame(pointWorld, sourceFrame);
            if (!std::isfinite(planeDistance) ||
                std::abs(planeDistance) > planeTolerance) {
                continue;
            }
            const QPointF pointLocal =
                worldPointToWorkPlaneFrame(pointWorld, sourceFrame);
            qreal localCoordinateScale =
                std::max<qreal>({1.0, std::abs(pointLocal.x()),
                                 std::abs(pointLocal.y())});
            for (const QPointF &controlPoint : sourceCurve.controlPoints) {
                localCoordinateScale =
                    std::max<qreal>({localCoordinateScale,
                                     std::abs(controlPoint.x()),
                                     std::abs(controlPoint.y())});
            }
            const qreal geometryTolerance =
                std::max<qreal>(1.0e-7,
                                localCoordinateScale * 1.0e-9);

            qreal closestDistanceSquared =
                std::numeric_limits<qreal>::infinity();
            qreal closestParameter = sourceDomainStart;
            constexpr int pointSearchSubdivisions = 8;
            for (const ParameterSpan &span : sourceSpans) {
                qreal sampleParameters[pointSearchSubdivisions + 1];
                qreal sampleDistances[pointSearchSubdivisions + 1];
                for (int sample = 0;
                     sample <= pointSearchSubdivisions;
                     ++sample) {
                    const qreal fraction =
                        static_cast<qreal>(sample) /
                        pointSearchSubdivisions;
                    sampleParameters[sample] =
                        span.start + (span.end - span.start) * fraction;
                    sampleDistances[sample] = squaredDistanceAtParameter(
                        sampleParameters[sample], pointLocal);
                    if (sampleDistances[sample] < closestDistanceSquared) {
                        closestDistanceSquared = sampleDistances[sample];
                        closestParameter = sampleParameters[sample];
                    }
                }
                for (int sample = 0;
                     sample <= pointSearchSubdivisions;
                     ++sample) {
                    const qreal previousDistance =
                        sample > 0 ? sampleDistances[sample - 1]
                                   : std::numeric_limits<qreal>::infinity();
                    const qreal nextDistance =
                        sample < pointSearchSubdivisions
                            ? sampleDistances[sample + 1]
                            : std::numeric_limits<qreal>::infinity();
                    if (sampleDistances[sample] > previousDistance ||
                        sampleDistances[sample] > nextDistance) {
                        continue;
                    }
                    const qreal low =
                        sampleParameters[std::max(0, sample - 1)];
                    const qreal high =
                        sampleParameters[std::min(pointSearchSubdivisions,
                                                  sample + 1)];
                    if (high - low <= 1.0e-14) {
                        continue;
                    }
                    const qreal candidateParameter =
                        goldenMinimum(low, high, pointLocal);
                    const qreal candidateDistanceSquared =
                        squaredDistanceAtParameter(candidateParameter,
                                                   pointLocal);
                    if (candidateDistanceSquared < closestDistanceSquared) {
                        closestDistanceSquared = candidateDistanceSquared;
                        closestParameter = candidateParameter;
                    }
                }
            }

            if (closestDistanceSquared <=
                geometryTolerance * geometryTolerance) {
                appendUniqueParameter(closestParameter);
                if (intersectionObjectIds != nullptr) {
                    const quint64 objectId =
                        shapes_.objectIdAt(shapeIndex).value();
                    if (!intersectionObjectIds->contains(objectId)) {
                        intersectionObjectIds->append(objectId);
                    }
                }
            }
        }

        return parameters;
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
        trimHoverPositionValid_ = false;
        trimHoverComponentIndex_ = -1;
        eraseCandidateShapeIndices_.clear();
        eraseSceneCurveCaches_.clear();
        eraseTargetCurveCaches_.clear();
        eraseTargetShapeIndices_.clear();
        if (!eraseStrokeActive_ && !trimBoxSelectionActive_) {
            eraseStrokeScreenPath_.clear();
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
                QVector<quint64> intersectionObjectIds;
                int targetSeedSolves = 0;
                int targetPointChecks = 0;
                targetCurve.intersectionParameters = eraseIntersectionParameters(
                    shapeIndex,
                    sceneCurve.componentIndex,
                    sceneCurve.curve,
                    &eraseSceneCurveCaches_,
                    &intersectionObjectIds,
                    &targetSeedSolves,
                    &targetPointChecks);
                targetCurve.intersectionObjectIds = intersectionObjectIds;
                const qint64 targetBoundaryUs = boundaryTimer.nsecsElapsed() / 1000;
                boundaryUs += targetBoundaryUs;
                nurbsSeedSolves += targetSeedSolves;
                visiblePointChecks += targetPointChecks;

                QStringList intersectionIds;
                for (const quint64 objectId : intersectionObjectIds) {
                    intersectionIds.append(QString::number(objectId));
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
                        .arg(targetSeedSolves)
                        .arg(targetPointChecks)
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

            const qreal componentDistance = closestNurbsScreenDistance(
                targetCurve.curve,
                targetCurve.workPlaneFrame,
                screenPosition);
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
        if (trimHoverPositionValid_ && eraseGeometryCachePrepared_ &&
            trimHoverScreenPosition_ == screenPosition) {
            return;
        }
        trimHoverScreenPosition_ = screenPosition;
        trimHoverPositionValid_ = true;

        if (!eraseGeometryCachePrepared_) {
            prepareEraseGeometryCache();
        }

        eraseStrokeScreenPath_.clear();
        eraseStrokeScreenPath_.append(screenPosition);
        eraseCandidateShapeIndices_.clear();

        constexpr qreal trimHitRadiusPixels = 10.0;
        qreal closestDistance = trimHitRadiusPixels;
        int closestShapeIndex = -1;
        int closestComponentIndex = -1;
        for (const ObjectId objectId : eraseTargetShapeIndices_) {
            const int shapeIndex = objectIndex(objectId);
            if (shapeIndex < 0) {
                continue;
            }
            int componentIndex = -1;
            const qreal distance = distanceToCachedEraseShape(screenPosition,
                                                              shapeIndex,
                                                              &componentIndex);
            const bool closer = distance < closestDistance - 1.0e-6;
            const bool tieOnActiveSelection =
                std::abs(distance - closestDistance) <= 1.0e-6 &&
                objectId == selectedShapeIndex_;
            if (closer || tieOnActiveSelection) {
                closestDistance = distance;
                closestShapeIndex = shapeIndex;
                closestComponentIndex = componentIndex;
            }
        }

        if (closestShapeIndex >= 0) {
            eraseCandidateShapeIndices_.append(shapes_.objectIdAt(closestShapeIndex));
        }
        trimHoverComponentIndex_ = closestComponentIndex;
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
        if (eraseCandidateShapeIndices_.isEmpty()) {
            DebugLog::instance().write(
                QStringLiteral("trim click ignored target=none at=%1 selectedTargets=%2 sampledTargets=%3 elapsedUs=%4")
                    .arg(pointText(screenPosition))
                    .arg(eraseTargetShapeIndices_.size())
                    .arg(eraseTargetCurveCaches_.size())
                    .arg(clickTimer.nsecsElapsed() / 1000));
            return;
        }

        const ObjectId targetId = eraseCandidateShapeIndices_.first();
        const int targetIndex = objectIndex(targetId);
        DebugLog::instance().write(
            QStringLiteral("trim click target shape=%1 object=%2 type=%3 component=%4 at=%5")
                .arg(targetIndex)
                .arg(targetId.value())
                .arg(targetIndex >= 0 && targetIndex < shapes_.size()
                         ? geometryTypeName(shapes_[targetIndex].geometryType)
                         : QStringLiteral("unknown"))
                .arg(trimHoverComponentIndex_)
                .arg(pointText(screenPosition)));
        applyEraseCandidates(nullptr, trimHoverComponentIndex_);
        DebugLog::instance().write(
            QStringLiteral("trim click complete object=%1 elapsedUs=%2")
                .arg(targetId.value())
                .arg(clickTimer.nsecsElapsed() / 1000));
        eraseStrokeActive_ = false;
        eraseCandidateShapeIndices_.clear();
        eraseStrokeScreenPath_.clear();
        eraseTargetShapeIndices_.clear();
        eraseSceneCurveCaches_.clear();
        eraseTargetCurveCaches_.clear();
        eraseGeometryCachePrepared_ = false;
        trimHoverPositionValid_ = false;
        trimHoverComponentIndex_ = -1;
        update();
    }

    void updateTrimBoxPreview()
    {
        if (!trimBoxSelectionActive_) {
            return;
        }
        invalidateEraseGeometryCacheForView();
        if (!eraseGeometryCachePrepared_) {
            prepareEraseGeometryCache();
        }

        const QRectF box =
            QRectF(selectionBoxStartScreen_, selectionBoxCurrentScreen_).normalized();
        const QPointF drag = selectionBoxCurrentScreen_ - selectionBoxStartScreen_;
        // Trim's window/crossing gesture follows the diagonal the user draws:
        // top-left to bottom-right contains; bottom-left to top-right crosses.
        const bool crossingSelection = drag.x() * drag.y() < 0.0;
        eraseCandidateShapeIndices_.clear();
        for (const ObjectId objectId : eraseTargetShapeIndices_) {
            const int shapeIndex = objectIndex(objectId);
            if (shapeIndex >= 0 && shapeIndex < shapes_.size() &&
                shapeMatchesSelectionBox(shapes_[shapeIndex], box, crossingSelection)) {
                eraseCandidateShapeIndices_.append(objectId);
            }
        }

        // A non-empty marker enables the existing orange trim preview; the
        // actual interval calculation and commit use the box region below.
        eraseStrokeScreenPath_.clear();
        eraseStrokeScreenPath_.append(selectionBoxStartScreen_);
        for (EraseCurveSampleCache &targetCurve : eraseTargetCurveCaches_) {
            targetCurve.previewStrokePointCount = 0;
            const ObjectId objectId = shapes_.objectIdAt(targetCurve.shapeIndex);
            if (!eraseCandidateShapeIndices_.contains(objectId)) {
                targetCurve.previewIntervals.clear();
                continue;
            }

            const QVector<ParameterInterval> hitIntervals =
                curveIntervalsInsideScreenBox(targetCurve.curve,
                                              targetCurve.sampled,
                                              targetCurve.workPlaneFrame,
                                              box);
            targetCurve.previewIntervals = boundEraseIntervals(
                targetCurve.curve,
                hitIntervals,
                targetCurve.intersectionParameters);
        }
    }

    void finishTrimBoxSelection()
    {
        if (!selectionBoxActive_ || !trimBoxSelectionActive_) {
            return;
        }

        const QRectF box =
            QRectF(selectionBoxStartScreen_, selectionBoxCurrentScreen_).normalized();
        const QPointF drag = selectionBoxCurrentScreen_ - selectionBoxStartScreen_;
        const bool crossingSelection = drag.x() * drag.y() < 0.0;
        const bool moved = selectionBoxMoved_ || box.width() >= 3.0 ||
                           box.height() >= 3.0;
        const QPointF clickPosition = selectionBoxCurrentScreen_;
        if (!moved) {
            selectionBoxActive_ = false;
            selectionBoxMoved_ = false;
            selectionBoxAdditive_ = false;
            trimBoxSelectionActive_ = false;
            trimHoverComponentIndex_ = -1;
            eraseCandidateShapeIndices_.clear();
            eraseStrokeScreenPath_.clear();
            trimHoverPositionValid_ = false;
            trimAtScreenPosition(clickPosition);
            setCursor(Qt::CrossCursor);
            update();
            emitCoordinateUpdate();
            return;
        }

        updateTrimBoxPreview();
        const int candidateCount = eraseCandidateShapeIndices_.size();
        selectionBoxActive_ = false;
        selectionBoxMoved_ = false;
        selectionBoxAdditive_ = false;
        trimBoxSelectionActive_ = false;
        trimHoverComponentIndex_ = -1;
        if (!eraseCandidateShapeIndices_.isEmpty()) {
            applyEraseCandidates(&box);
        }

        eraseStrokeActive_ = false;
        eraseCursorPressed_ = false;
        eraseCandidateShapeIndices_.clear();
        eraseStrokeScreenPath_.clear();
        eraseTargetShapeIndices_.clear();
        eraseSceneCurveCaches_.clear();
        eraseTargetCurveCaches_.clear();
        eraseGeometryCachePrepared_ = false;
        trimHoverPositionValid_ = false;
        setCursor(Qt::CrossCursor);
        update();
        emitCoordinateUpdate();
        DebugLog::instance().write(
            QStringLiteral("trim box applied crossing=%1 candidates=%2")
                .arg(crossingSelection)
                .arg(candidateCount));
    }

    qreal closestNurbsScreenDistance(
        const Shape::NurbsCurve2D &curve,
        const WorkPlaneFrame &workPlaneFrame,
        const QPointF &screenPosition) const
    {
        if (!isValidNurbsCurve(curve) ||
            !isValidWorkPlaneFrame(workPlaneFrame)) {
            return std::numeric_limits<qreal>::infinity();
        }

        const QVector<double> fullKnots = expandedKnotVector(curve);
        const auto squaredDistanceAt = [&](qreal parameter) {
            QPointF localPoint;
            if (!evaluateNurbsPoint(curve, parameter, &localPoint)) {
                return std::numeric_limits<qreal>::infinity();
            }
            const QPointF curveScreen =
                viewportTransform_.workPlaneToScreen(localPoint,
                                                     size(),
                                                     workPlaneFrame);
            const QPointF delta = curveScreen - screenPosition;
            return QPointF::dotProduct(delta, delta);
        };
        const auto goldenMinimum = [&](qreal low, qreal high) {
            constexpr qreal ratio = 0.6180339887498948482;
            qreal first = high - (high - low) * ratio;
            qreal second = low + (high - low) * ratio;
            qreal firstValue = squaredDistanceAt(first);
            qreal secondValue = squaredDistanceAt(second);
            for (int iteration = 0; iteration < 32; ++iteration) {
                if (firstValue <= secondValue) {
                    high = second;
                    second = first;
                    secondValue = firstValue;
                    first = high - (high - low) * ratio;
                    firstValue = squaredDistanceAt(first);
                } else {
                    low = first;
                    first = second;
                    firstValue = secondValue;
                    second = low + (high - low) * ratio;
                    secondValue = squaredDistanceAt(second);
                }
            }
            return (low + high) * 0.5;
        };

        qreal closestDistanceSquared =
            std::numeric_limits<qreal>::infinity();
        for (int knotIndex = curve.degree;
             knotIndex < curve.controlPoints.size();
             ++knotIndex) {
            const qreal spanStart = fullKnots[knotIndex];
            const qreal spanEnd = fullKnots[knotIndex + 1];
            if (spanEnd - spanStart <= 1.0e-12) {
                continue;
            }

            const int subdivisions = std::max(24, curve.degree * 16);
            QVector<qreal> sampleParameters;
            QVector<qreal> sampleDistances;
            sampleParameters.reserve(subdivisions + 1);
            sampleDistances.reserve(subdivisions + 1);
            for (int sample = 0; sample <= subdivisions; ++sample) {
                const qreal fraction =
                    static_cast<qreal>(sample) / subdivisions;
                const qreal parameter =
                    spanStart + (spanEnd - spanStart) * fraction;
                const qreal distanceSquared =
                    squaredDistanceAt(parameter);
                sampleParameters.append(parameter);
                sampleDistances.append(distanceSquared);
                closestDistanceSquared =
                    std::min(closestDistanceSquared, distanceSquared);
            }

            for (int sample = 0; sample <= subdivisions; ++sample) {
                const qreal previousDistance =
                    sample > 0 ? sampleDistances[sample - 1]
                               : std::numeric_limits<qreal>::infinity();
                const qreal nextDistance =
                    sample < subdivisions
                        ? sampleDistances[sample + 1]
                        : std::numeric_limits<qreal>::infinity();
                if (sampleDistances[sample] > previousDistance ||
                    sampleDistances[sample] > nextDistance) {
                    continue;
                }

                const qreal low =
                    sampleParameters[std::max(0, sample - 1)];
                const qreal high =
                    sampleParameters[std::min(subdivisions, sample + 1)];
                if (high - low <= 1.0e-14) {
                    continue;
                }
                const qreal candidate =
                    goldenMinimum(low, high);
                closestDistanceSquared =
                    std::min(closestDistanceSquared,
                             squaredDistanceAt(candidate));
            }
        }
        return std::sqrt(closestDistanceSquared);
    }

    QVector<ParameterInterval> eraserIntervalsForNurbsStrokeSegment(
        const Shape::NurbsCurve2D &curve,
        const WorkPlaneFrame &workPlaneFrame,
        const QPointF &strokeStart,
        const QPointF &strokeEnd) const
    {
        QVector<ParameterInterval> intervals;
        if (!isValidNurbsCurve(curve) ||
            !isValidWorkPlaneFrame(workPlaneFrame)) {
            return intervals;
        }

        const QVector<double> fullKnots = expandedKnotVector(curve);
        const qreal domainStart = fullKnots[curve.degree];
        const qreal domainEnd = fullKnots[curve.controlPoints.size()];
        const qreal domainLength = domainEnd - domainStart;
        if (domainLength <= 1.0e-12) {
            return intervals;
        }

        constexpr qreal eraserRadiusPixels = 10.0;
        const qreal eraserRadiusSquared =
            eraserRadiusPixels * eraserRadiusPixels;
        const auto squaredDistanceAt = [&](qreal parameter) {
            QPointF localPoint;
            if (!evaluateNurbsPoint(curve, parameter, &localPoint)) {
                return std::numeric_limits<qreal>::infinity();
            }
            const QPointF curveScreen =
                viewportTransform_.workPlaneToScreen(localPoint,
                                                     size(),
                                                     workPlaneFrame);
            const qreal distance = distanceToSegment(curveScreen,
                                                     strokeStart,
                                                     strokeEnd);
            return distance * distance;
        };
        const auto goldenMinimum = [&](qreal low, qreal high) {
            constexpr qreal ratio = 0.6180339887498948482;
            qreal first = high - (high - low) * ratio;
            qreal second = low + (high - low) * ratio;
            qreal firstValue = squaredDistanceAt(first);
            qreal secondValue = squaredDistanceAt(second);
            for (int iteration = 0; iteration < 32; ++iteration) {
                if (firstValue <= secondValue) {
                    high = second;
                    second = first;
                    secondValue = firstValue;
                    first = high - (high - low) * ratio;
                    firstValue = squaredDistanceAt(first);
                } else {
                    low = first;
                    first = second;
                    firstValue = secondValue;
                    second = low + (high - low) * ratio;
                    secondValue = squaredDistanceAt(second);
                }
            }
            return (low + high) * 0.5;
        };
        const auto refineBoundary = [&](qreal outsideParameter,
                                        qreal insideParameter,
                                        bool outsideIsFirst) {
            qreal low = outsideIsFirst ? outsideParameter : insideParameter;
            qreal high = outsideIsFirst ? insideParameter : outsideParameter;
            for (int iteration = 0; iteration < 36; ++iteration) {
                const qreal middle = (low + high) * 0.5;
                const bool middleInside =
                    squaredDistanceAt(middle) <= eraserRadiusSquared;
                if (middleInside) {
                    if (outsideIsFirst) {
                        high = middle;
                    } else {
                        low = middle;
                    }
                } else {
                    if (outsideIsFirst) {
                        low = middle;
                    } else {
                        high = middle;
                    }
                }
            }
            return (low + high) * 0.5;
        };

        for (int knotIndex = curve.degree;
             knotIndex < curve.controlPoints.size();
             ++knotIndex) {
            const qreal spanStart = fullKnots[knotIndex];
            const qreal spanEnd = fullKnots[knotIndex + 1];
            if (spanEnd - spanStart <= 1.0e-12) {
                continue;
            }

            const int subdivisions = std::max(24, curve.degree * 16);
            QVector<qreal> sampleParameters;
            QVector<qreal> sampleDistances;
            sampleParameters.reserve(subdivisions + 1);
            sampleDistances.reserve(subdivisions + 1);
            for (int sample = 0; sample <= subdivisions; ++sample) {
                const qreal fraction =
                    static_cast<qreal>(sample) / subdivisions;
                const qreal parameter =
                    spanStart + (spanEnd - spanStart) * fraction;
                sampleParameters.append(parameter);
                sampleDistances.append(squaredDistanceAt(parameter));
            }

            for (int sample = 0; sample <= subdivisions; ++sample) {
                const qreal previousDistance =
                    sample > 0 ? sampleDistances[sample - 1]
                               : std::numeric_limits<qreal>::infinity();
                const qreal nextDistance =
                    sample < subdivisions
                        ? sampleDistances[sample + 1]
                        : std::numeric_limits<qreal>::infinity();
                if (sampleDistances[sample] > previousDistance ||
                    sampleDistances[sample] > nextDistance) {
                    continue;
                }

                const qreal low =
                    sampleParameters[std::max(0, sample - 1)];
                const qreal high =
                    sampleParameters[std::min(subdivisions, sample + 1)];
                qreal minimumParameter = sampleParameters[sample];
                if (high - low > 1.0e-14) {
                    minimumParameter = goldenMinimum(low, high);
                }
                if (squaredDistanceAt(minimumParameter) >
                    eraserRadiusSquared) {
                    continue;
                }

                qreal leftBoundary = spanStart;
                int previous = sample;
                while (previous >= 0 &&
                       sampleParameters[previous] >= minimumParameter) {
                    --previous;
                }
                while (previous >= 0 &&
                       sampleDistances[previous] <= eraserRadiusSquared) {
                    --previous;
                }
                if (previous >= 0) {
                    leftBoundary = refineBoundary(sampleParameters[previous],
                                                  minimumParameter,
                                                  true);
                }

                qreal rightBoundary = spanEnd;
                int next = sample;
                while (next <= subdivisions &&
                       sampleParameters[next] <= minimumParameter) {
                    ++next;
                }
                while (next <= subdivisions &&
                       sampleDistances[next] <= eraserRadiusSquared) {
                    ++next;
                }
                if (next <= subdivisions) {
                    rightBoundary = refineBoundary(sampleParameters[next],
                                                   minimumParameter,
                                                   false);
                }

                if (rightBoundary - leftBoundary > 0.0) {
                    intervals.append({leftBoundary, rightBoundary});
                }
            }
        }

        std::sort(intervals.begin(),
                  intervals.end(),
                  [](const ParameterInterval &first,
                     const ParameterInterval &second) {
                      return first.start < second.start;
                  });
        const qreal mergeTolerance =
            std::max<qreal>(1.0e-9, domainLength * 1.0e-8);
        QVector<ParameterInterval> merged;
        for (const ParameterInterval &interval : intervals) {
            if (interval.end - interval.start <= mergeTolerance) {
                continue;
            }
            if (!merged.isEmpty() &&
                interval.start <= merged.last().end + mergeTolerance) {
                merged.last().end =
                    std::max(merged.last().end, interval.end);
            } else {
                merged.append(interval);
            }
        }
        return merged;
    }

    QVector<ParameterInterval> eraserIntervalsForCurve(
        const Shape::NurbsCurve2D &curve,
        const QVector<QPointF> &stroke,
        const WorkPlaneFrame &workPlaneFrame) const
    {
        QVector<ParameterInterval> intervals;
        if (!isValidNurbsCurve(curve) || stroke.isEmpty()) {
            return intervals;
        }
        if (stroke.size() == 1) {
            intervals = eraserIntervalsForNurbsStrokeSegment(
                curve, workPlaneFrame, stroke.first(), stroke.first());
        } else {
            for (int index = 1; index < stroke.size(); ++index) {
                const QVector<ParameterInterval> segmentIntervals =
                    eraserIntervalsForNurbsStrokeSegment(
                        curve,
                        workPlaneFrame,
                        stroke[index - 1],
                        stroke[index]);
                intervals += segmentIntervals;
            }
        }

        const QVector<double> fullKnots = expandedKnotVector(curve);
        const qreal domainStart = fullKnots[curve.degree];
        const qreal domainEnd = fullKnots[curve.controlPoints.size()];
        const qreal domainLength = domainEnd - domainStart;
        const qreal mergeTolerance =
            std::max<qreal>(1.0e-9, domainLength * 1.0e-8);
        std::sort(intervals.begin(),
                  intervals.end(),
                  [](const ParameterInterval &first,
                     const ParameterInterval &second) {
                      return first.start < second.start;
                  });
        QVector<ParameterInterval> merged;
        for (const ParameterInterval &interval : intervals) {
            if (interval.end - interval.start <= mergeTolerance) {
                continue;
            }
            if (!merged.isEmpty() &&
                interval.start <= merged.last().end + mergeTolerance) {
                merged.last().end =
                    std::max(merged.last().end, interval.end);
            } else {
                merged.append(interval);
            }
        }
        return merged;
    }

    QVector<ParameterInterval> eraserIntervalsForCurve(
        const Shape::NurbsCurve2D &curve,
        const QVector<QPointF> &stroke) const
    {
        return eraserIntervalsForCurve(curve,
                                       stroke,
                                       viewportTransform_.workPlaneFrame());
    }

    QVector<ParameterInterval> boundEraseIntervals(
        const Shape::NurbsCurve2D &curve,
        const QVector<ParameterInterval> &hitIntervals,
        const QVector<qreal> &intersectionParameters) const
    {
        if (hitIntervals.isEmpty() || !isValidNurbsCurve(curve)) {
            return {};
        }

        const QVector<double> fullKnots = expandedKnotVector(curve);
        const qreal domainStart = fullKnots[curve.degree];
        const qreal domainEnd = fullKnots[curve.controlPoints.size()];
        const qreal domainLength = domainEnd - domainStart;
        const qreal tolerance = std::max<qreal>(1.0e-9, domainLength * 1.0e-8);

        QVector<qreal> boundaries{domainStart, domainEnd};
        boundaries += intersectionParameters;
        std::sort(boundaries.begin(), boundaries.end());

        QVector<qreal> uniqueBoundaries;
        for (const qreal boundary : boundaries) {
            const qreal clampedBoundary = std::clamp(boundary,
                                                     domainStart,
                                                     domainEnd);
            if (uniqueBoundaries.isEmpty() ||
                clampedBoundary > uniqueBoundaries.back() + tolerance) {
                uniqueBoundaries.append(clampedBoundary);
            } else {
                uniqueBoundaries.back() =
                    std::max(uniqueBoundaries.back(), clampedBoundary);
            }
        }

        QVector<ParameterInterval> removedIntervals;
        for (const ParameterInterval &hit : hitIntervals) {
            const qreal hitMidpoint = (hit.start + hit.end) * 0.5;
            for (int boundaryIndex = 0;
                 boundaryIndex + 1 < uniqueBoundaries.size();
                 ++boundaryIndex) {
                const qreal pieceStart = uniqueBoundaries[boundaryIndex];
                const qreal pieceEnd = uniqueBoundaries[boundaryIndex + 1];
                if (pieceEnd - pieceStart <= tolerance ||
                    hit.end <= pieceStart + tolerance ||
                    hit.start >= pieceEnd - tolerance) {
                    continue;
                }

                // The eraser has a visible radius. Near a real intersection,
                // that radius can overlap both neighboring pieces even when
                // the stroke is centered on only one of them. Use the stroke
                // interval's center to choose the directly erased piece, and
                // also retain pieces whose center is fully covered by a long
                // stroke. This prevents a small brush overlap from deleting
                // past the intersection while still allowing a long drag to
                // remove several complete pieces.
                const qreal pieceMidpoint = (pieceStart + pieceEnd) * 0.5;
                const bool hitCenterIsInPiece =
                    hitMidpoint > pieceStart + tolerance &&
                    hitMidpoint < pieceEnd - tolerance;
                const bool pieceCenterIsInHit =
                    pieceMidpoint >= hit.start - tolerance &&
                    pieceMidpoint <= hit.end + tolerance;
                if (hitCenterIsInPiece || pieceCenterIsInHit) {
                    removedIntervals.append(ParameterInterval{pieceStart, pieceEnd});
                }
            }
        }

        if (removedIntervals.isEmpty()) {
            return removedIntervals;
        }

        // A closed curve's domain ends are the same geometric point, not
        // cutting boundaries. The first and last parameter pieces therefore
        // belong to one section unless another curve intersects the seam.
        QPointF firstPoint;
        QPointF lastPoint;
        const bool closed = evaluateNurbsPoint(curve, domainStart, &firstPoint) &&
                            evaluateNurbsPoint(curve, domainEnd, &lastPoint) &&
                            std::hypot(firstPoint.x() - lastPoint.x(),
                                       firstPoint.y() - lastPoint.y()) <= 1.0e-8;
        const bool seamIsIntersection = std::any_of(
            intersectionParameters.begin(), intersectionParameters.end(),
            [&](qreal parameter) {
                return std::abs(parameter - domainStart) <= tolerance ||
                       std::abs(parameter - domainEnd) <= tolerance;
            });
        if (closed && !seamIsIntersection && uniqueBoundaries.size() > 2) {
            const bool removesFirstPiece = std::any_of(
                removedIntervals.begin(), removedIntervals.end(),
                [&](const ParameterInterval &interval) {
                    return interval.start <= domainStart + tolerance &&
                           interval.end > domainStart + tolerance;
                });
            const bool removesLastPiece = std::any_of(
                removedIntervals.begin(), removedIntervals.end(),
                [&](const ParameterInterval &interval) {
                    return interval.end >= domainEnd - tolerance &&
                           interval.start < domainEnd - tolerance;
                });
            if (removesFirstPiece || removesLastPiece) {
                removedIntervals.append({domainStart, uniqueBoundaries[1]});
                removedIntervals.append({uniqueBoundaries[uniqueBoundaries.size() - 2],
                                         domainEnd});
            }
        }

        std::sort(removedIntervals.begin(),
                  removedIntervals.end(),
                  [](const ParameterInterval &first, const ParameterInterval &second) {
                      return first.start < second.start;
                  });
        QVector<ParameterInterval> merged;
        for (const ParameterInterval &interval : removedIntervals) {
            if (!merged.isEmpty() &&
                interval.start <= merged.last().end + tolerance) {
                merged.last().end = std::max(merged.last().end, interval.end);
            } else {
                merged.append(interval);
            }
        }
        return merged;
    }

    QVector<ParameterInterval> curveIntervalsInsideScreenBox(
        const Shape::NurbsCurve2D &curve,
        const SampledNurbsCurve2D &sampled,
        const WorkPlaneFrame &workPlaneFrame,
        const QRectF &box) const
    {
        QVector<ParameterInterval> intervals;
        if (!isValidNurbsCurve(curve) || sampled.parameters.size() < 2 ||
            sampled.parameters.size() != sampled.screenPoints.size()) {
            return intervals;
        }

        const QRectF region = box.normalized();
        const auto isInside = [&](qreal parameter) {
            QPointF worldPoint;
            return evaluateNurbsPoint(curve, parameter, &worldPoint) &&
                   region.contains(viewportTransform_.workPlaneToScreen(
                       worldPoint, size(), workPlaneFrame));
        };
        const auto refineBoundary = [&](qreal first,
                                        qreal second,
                                        bool firstInside) {
            qreal low = first;
            qreal high = second;
            for (int iteration = 0; iteration < 32; ++iteration) {
                const qreal middle = (low + high) * 0.5;
                if (isInside(middle) == firstInside) {
                    low = middle;
                } else {
                    high = middle;
                }
            }
            return (low + high) * 0.5;
        };

        qreal intervalStart = sampled.parameters.first();
        bool previousInside = region.contains(sampled.screenPoints.first());
        bool insideInterval = previousInside;
        for (int sample = 1; sample < sampled.parameters.size(); ++sample) {
            const bool currentInside = region.contains(sampled.screenPoints[sample]);
            if (currentInside != previousInside) {
                const qreal boundary = refineBoundary(sampled.parameters[sample - 1],
                                                       sampled.parameters[sample],
                                                       previousInside);
                if (currentInside) {
                    intervalStart = boundary;
                    insideInterval = true;
                } else if (insideInterval) {
                    intervals.append({intervalStart, boundary});
                    insideInterval = false;
                }
            }
            previousInside = currentInside;
        }
        if (insideInterval) {
            intervals.append({intervalStart, sampled.parameters.last()});
        }

        const qreal domainLength = sampled.parameters.last() -
                                   sampled.parameters.first();
        const qreal tolerance = std::max<qreal>(1.0e-9, domainLength * 1.0e-8);
        intervals.erase(std::remove_if(intervals.begin(), intervals.end(),
                                       [tolerance](const ParameterInterval &interval) {
                                           return interval.end - interval.start <= tolerance;
                                       }),
                        intervals.end());
        return intervals;
    }

    void updateErasePreviewIntervals(bool reset,
                                     int onlyShapeIndex = -1,
                                     int onlyComponentIndex = -1)
    {
        invalidateEraseGeometryCacheForView();
        if (!eraseGeometryCachePrepared_) {
            prepareEraseGeometryCache();
            if (eraseStrokeActive_ && !eraseStrokeScreenPath_.isEmpty()) {
                eraseAlongScreenSegment(eraseStrokeScreenPath_.first(),
                                        eraseStrokeScreenPath_.first());
                for (int point = 1; point < eraseStrokeScreenPath_.size(); ++point) {
                    eraseAlongScreenSegment(eraseStrokeScreenPath_[point - 1],
                                            eraseStrokeScreenPath_[point]);
                }
            }
        }
        if (eraseStrokeScreenPath_.isEmpty()) {
            return;
        }

        const int strokePointCount = eraseStrokeScreenPath_.size();
        for (EraseCurveSampleCache &targetCurve : eraseTargetCurveCaches_) {
            if (reset || targetCurve.previewStrokePointCount > strokePointCount) {
                targetCurve.previewIntervals.clear();
                targetCurve.previewStrokePointCount = 0;
            }
            if (!eraseCandidateShapeIndices_.contains(
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
                newHitIntervals = eraserIntervalsForNurbsStrokeSegment(
                    targetCurve.curve,
                    targetCurve.workPlaneFrame,
                    eraseStrokeScreenPath_.first(),
                    eraseStrokeScreenPath_.first());
                firstStrokeSegment = 1;
            }

            for (int strokeSegment = firstStrokeSegment;
                 strokeSegment < strokePointCount;
                 ++strokeSegment) {
                const QVector<ParameterInterval> segmentIntervals =
                    eraserIntervalsForNurbsStrokeSegment(
                        targetCurve.curve,
                        targetCurve.workPlaneFrame,
                        eraseStrokeScreenPath_[strokeSegment - 1],
                        eraseStrokeScreenPath_[strokeSegment]);
                for (const ParameterInterval &interval : segmentIntervals) {
                    newHitIntervals.append(interval);
                }
            }

            if (!newHitIntervals.isEmpty()) {
                for (const ParameterInterval &interval : newHitIntervals) {
                    targetCurve.previewIntervals.append(interval);
                }
                targetCurve.previewIntervals = boundEraseIntervals(
                    targetCurve.curve,
                    targetCurve.previewIntervals,
                    targetCurve.intersectionParameters);
            }
            targetCurve.previewStrokePointCount = strokePointCount;
        }
    }

    QVector<ParameterInterval> eraseIntervalsBoundedByIntersections(
        int sourceShapeIndex,
        int sourceComponentIndex,
        const Shape::NurbsCurve2D &curve,
        const QVector<QPointF> &stroke,
        const QVector<qreal> *cachedIntersectionParameters = nullptr) const
    {
        const WorkPlaneFrame workPlaneFrame =
            sourceShapeIndex >= 0 && sourceShapeIndex < shapes_.size()
                ? shapeWorkPlaneFrame(shapes_[sourceShapeIndex])
                : viewportTransform_.workPlaneFrame();
        const QVector<ParameterInterval> hitIntervals =
            eraserIntervalsForCurve(curve, stroke, workPlaneFrame);
        if (hitIntervals.isEmpty()) {
            return {};
        }

        const QVector<qreal> intersectionParameters =
            cachedIntersectionParameters != nullptr
                ? *cachedIntersectionParameters
                : eraseIntersectionParameters(sourceShapeIndex,
                                              sourceComponentIndex,
                                              curve);
        return boundEraseIntervals(curve, hitIntervals, intersectionParameters);
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

        if (shape.geometryType == GeometryType::Point) {
            return false;
        }

        QVector<Shape::NurbsCurve2D> sourceCurves;
        if (shape.geometryType == GeometryType::PolyCurve) {
            sourceCurves = shape.components;
        } else if (isValidNurbsCurve(shape.nurbs)) {
            sourceCurves.append(shape.nurbs);
        } else if (shape.geometryType == GeometryType::Line && shape.points.size() >= 2) {
            sourceCurves.append(makeDegreeOneNurbs(shape.points));
        } else if (shape.geometryType == GeometryType::Rectangle ||
                   shape.geometryType == GeometryType::Polygon) {
            const QVector<QPointF> vertices =
                shape.geometryType == GeometryType::Rectangle
                    ? rectangleVertices(shape)
                    : polygonVerticesForShape(shape);
            const int minimumVertices = shape.geometryType == GeometryType::Rectangle
                                            ? 4
                                            : 3;
            if (vertices.size() < minimumVertices) {
                return false;
            }
            sourceCurves.reserve(vertices.size());
            for (int index = 0; index < vertices.size(); ++index) {
                sourceCurves.append(makeDegreeOneNurbs(
                    {vertices[index], vertices[(index + 1) % vertices.size()]}));
            }
        } else {
            return false;
        }

        const WorkPlaneFrame sourceFrame = shapeWorkPlaneFrame(shape);
        const bool wholeObjectOnIntersectionFreeErase =
            activeTool_ == Tool::Erase && trimBox == nullptr;
        QVector<QVector<ParameterInterval>> eraseHitIntervals;
        QVector<QVector<qreal>> eraseBoundaryParameters;
        QVector<QVector<quint64>> eraseIntersectionObjectIds;
        if (wholeObjectOnIntersectionFreeErase) {
            eraseHitIntervals.resize(sourceCurves.size());
            eraseBoundaryParameters.resize(sourceCurves.size());
            eraseIntersectionObjectIds.resize(sourceCurves.size());
            bool strokeHitsObject = false;
            bool objectHasIntersection = false;
            for (int componentIndex = 0;
                 componentIndex < sourceCurves.size();
                 ++componentIndex) {
                const Shape::NurbsCurve2D &sourceCurve =
                    sourceCurves[componentIndex];
                const EraseCurveSampleCache *cachedTargetCurve = nullptr;
                if (cachedTargets != nullptr) {
                    for (const EraseCurveSampleCache &cachedTarget : *cachedTargets) {
                        if (cachedTarget.shapeIndex == sourceShapeIndex &&
                            cachedTarget.componentIndex == componentIndex) {
                            cachedTargetCurve = &cachedTarget;
                            break;
                        }
                    }
                }

                if (cachedTargetCurve != nullptr) {
                    eraseBoundaryParameters[componentIndex] =
                        cachedTargetCurve->intersectionParameters;
                    eraseIntersectionObjectIds[componentIndex] =
                        cachedTargetCurve->intersectionObjectIds;
                } else {
                    eraseBoundaryParameters[componentIndex] =
                        eraseIntersectionParameters(sourceShapeIndex,
                                                    componentIndex,
                                                    sourceCurve,
                                                    nullptr,
                                                    &eraseIntersectionObjectIds[componentIndex]);
                }
                qreal componentDomainStart = 0.0;
                qreal componentDomainEnd = 0.0;
                const qreal componentDomainLength =
                    nurbsParameterDomain(sourceCurve,
                                         &componentDomainStart,
                                         &componentDomainEnd)
                        ? componentDomainEnd - componentDomainStart
                        : 0.0;
                const qreal boundaryTolerance = std::max<qreal>(
                    1.0e-12, componentDomainLength * 1.0e-9);
                const bool hasInteriorIntersection = std::any_of(
                    eraseBoundaryParameters[componentIndex].begin(),
                    eraseBoundaryParameters[componentIndex].end(),
                    [&](qreal parameter) {
                        return parameter > componentDomainStart + boundaryTolerance &&
                               parameter < componentDomainEnd - boundaryTolerance;
                    });
                eraseHitIntervals[componentIndex] = eraserIntervalsForCurve(
                    sourceCurve, stroke, sourceFrame);
                strokeHitsObject = strokeHitsObject ||
                                   !eraseHitIntervals[componentIndex].isEmpty();
                objectHasIntersection = objectHasIntersection ||
                                        hasInteriorIntersection ||
                                        !eraseIntersectionObjectIds[componentIndex].isEmpty();
            }

            if (strokeHitsObject && !objectHasIntersection) {
                const ObjectId objectId = sourceShapeIndex >= 0 &&
                                                  sourceShapeIndex < shapes_.size()
                                              ? shapes_.objectIdAt(sourceShapeIndex)
                                              : ObjectId::invalid();
                DebugLog::instance().write(
                    QStringLiteral("erase whole-object object=%1 shape=%2 type=%3 components=%4 reason=no-intersections")
                        .arg(objectId.value())
                        .arg(sourceShapeIndex)
                        .arg(geometryTypeName(shape.geometryType))
                        .arg(sourceCurves.size()));
                return true;
            }
        }

        QVector<Shape::NurbsCurve2D> remainingCurves;
        bool changed = false;
        for (int sourceComponentIndex = 0;
             sourceComponentIndex < sourceCurves.size();
             ++sourceComponentIndex) {
            const Shape::NurbsCurve2D &sourceCurve = sourceCurves[sourceComponentIndex];
            if (onlyComponentIndex >= 0 &&
                sourceComponentIndex != onlyComponentIndex) {
                remainingCurves.append(sourceCurve);
                continue;
            }
            const QVector<qreal> *cachedIntersectionParameters = nullptr;
            const EraseCurveSampleCache *cachedTargetCurve = nullptr;
            if (cachedTargets != nullptr) {
                for (const EraseCurveSampleCache &cachedTarget : *cachedTargets) {
                    if (cachedTarget.shapeIndex == sourceShapeIndex &&
                        cachedTarget.componentIndex == sourceComponentIndex) {
                        cachedTargetCurve = &cachedTarget;
                        cachedIntersectionParameters =
                            &cachedTarget.intersectionParameters;
                        break;
                    }
                }
            }
            QVector<ParameterInterval> removedIntervals;
            if (trimBox != nullptr) {
                SampledNurbsCurve2D fallbackSamples;
                const SampledNurbsCurve2D *samples =
                    cachedTargetCurve != nullptr ? &cachedTargetCurve->sampled
                                                 : &fallbackSamples;
                if (cachedTargetCurve == nullptr &&
                    !sampleNurbsCurveForErase(sourceCurve,
                                              sourceFrame,
                                              &fallbackSamples)) {
                    remainingCurves.append(sourceCurve);
                    continue;
                }
                const QVector<ParameterInterval> hitIntervals =
                    curveIntervalsInsideScreenBox(sourceCurve,
                                                  *samples,
                                                  sourceFrame,
                                                  *trimBox);
                QVector<qreal> fallbackIntersections;
                const QVector<qreal> *intersections = cachedIntersectionParameters;
                if (intersections == nullptr) {
                    fallbackIntersections = eraseIntersectionParameters(
                        sourceShapeIndex, sourceComponentIndex, sourceCurve);
                    intersections = &fallbackIntersections;
                }
                removedIntervals = boundEraseIntervals(sourceCurve,
                                                       hitIntervals,
                                                       *intersections);
            } else if (wholeObjectOnIntersectionFreeErase) {
                removedIntervals = boundEraseIntervals(
                    sourceCurve,
                    eraseHitIntervals[sourceComponentIndex],
                    eraseBoundaryParameters[sourceComponentIndex]);
            } else {
                removedIntervals = eraseIntervalsBoundedByIntersections(
                    sourceShapeIndex,
                    sourceComponentIndex,
                    sourceCurve,
                    stroke,
                    cachedIntersectionParameters);
            }
            if (removedIntervals.isEmpty()) {
                if (activeTool_ == Tool::Trim &&
                    (trimBox != nullptr ||
                     sourceComponentIndex == onlyComponentIndex)) {
                    const ObjectId objectId = sourceShapeIndex >= 0 &&
                                                      sourceShapeIndex < shapes_.size()
                                                  ? shapes_.objectIdAt(sourceShapeIndex)
                                                  : ObjectId::invalid();
                    DebugLog::instance().write(
                        QStringLiteral("trim section empty object=%1 shape=%2 type=%3 component=%4 mode=%5 boundaries=%6 frameValid=%7")
                            .arg(objectId.value())
                            .arg(sourceShapeIndex)
                            .arg(geometryTypeName(shape.geometryType))
                            .arg(sourceComponentIndex)
                            .arg(trimBox != nullptr ? QStringLiteral("box")
                                                    : QStringLiteral("click"))
                            .arg(cachedIntersectionParameters == nullptr
                                     ? 0
                                     : cachedIntersectionParameters->size())
                            .arg(isValidWorkPlaneFrame(sourceFrame)));
                }
                remainingCurves.append(sourceCurve);
                continue;
            }

            if (activeTool_ == Tool::Trim &&
                (trimBox != nullptr ||
                 sourceComponentIndex == onlyComponentIndex)) {
                const ObjectId objectId = sourceShapeIndex >= 0 &&
                                                  sourceShapeIndex < shapes_.size()
                                              ? shapes_.objectIdAt(sourceShapeIndex)
                                              : ObjectId::invalid();
                QStringList removedParameterText;
                for (const ParameterInterval &interval : removedIntervals) {
                    QPointF startLocal;
                    QPointF endLocal;
                    QString segmentWorldText = QStringLiteral("world=unavailable");
                    if (evaluateNurbsPoint(sourceCurve, interval.start, &startLocal) &&
                        evaluateNurbsPoint(sourceCurve, interval.end, &endLocal)) {
                        const Point3D startWorld = workPlaneFramePointToWorld(
                            startLocal, sourceFrame);
                        const Point3D endWorld = workPlaneFramePointToWorld(
                            endLocal, sourceFrame);
                        segmentWorldText =
                            QStringLiteral("world=(%1,%2,%3)->(%4,%5,%6)")
                                .arg(startWorld.x, 0, 'g', 8)
                                .arg(startWorld.y, 0, 'g', 8)
                                .arg(startWorld.z, 0, 'g', 8)
                                .arg(endWorld.x, 0, 'g', 8)
                                .arg(endWorld.y, 0, 'g', 8)
                                .arg(endWorld.z, 0, 'g', 8);
                    }
                    removedParameterText.append(
                        QStringLiteral("u%1..%2 %3")
                            .arg(interval.start, 0, 'g', 10)
                            .arg(interval.end, 0, 'g', 10)
                            .arg(segmentWorldText));
                }
                QStringList boundaryParameterText;
                if (cachedIntersectionParameters != nullptr) {
                    for (const qreal parameter : *cachedIntersectionParameters) {
                        boundaryParameterText.append(
                            QString::number(parameter, 'g', 10));
                    }
                }
                DebugLog::instance().write(
                    QStringLiteral("trim section remove object=%1 shape=%2 type=%3 component=%4 mode=%5 intervals=[%6] boundaries=[%7] frameValid=%8")
                        .arg(objectId.value())
                        .arg(sourceShapeIndex)
                        .arg(geometryTypeName(shape.geometryType))
                        .arg(sourceComponentIndex)
                        .arg(trimBox != nullptr ? QStringLiteral("box")
                                                : QStringLiteral("click"))
                        .arg(removedParameterText.join(','))
                        .arg(boundaryParameterText.join(','))
                        .arg(isValidWorkPlaneFrame(sourceFrame)));
            }

            changed = true;
            const QVector<double> fullKnots = expandedKnotVector(sourceCurve);
            const qreal domainStart = fullKnots[sourceCurve.degree];
            const qreal domainEnd = fullKnots[sourceCurve.controlPoints.size()];
            qreal keepStart = domainStart;
            const qreal tolerance =
                std::max<qreal>(1.0e-9, std::abs(domainEnd - domainStart) * 1.0e-9);
            for (const ParameterInterval &removed : removedIntervals) {
                if (removed.start - keepStart > tolerance) {
                    Shape::NurbsCurve2D kept;
                    if (!trimNurbsCurve(sourceCurve, keepStart, removed.start, &kept)) {
                        return false;
                    }
                    remainingCurves.append(kept);
                }
                keepStart = std::max(keepStart, removed.end);
            }
            if (domainEnd - keepStart > tolerance) {
                Shape::NurbsCurve2D kept;
                if (!trimNurbsCurve(sourceCurve, keepStart, domainEnd, &kept)) {
                    return false;
                }
                remainingCurves.append(kept);
            }
        }

        if (!changed) {
            return false;
        }
        if (remainingCurves.isEmpty()) {
            return true;
        }

        for (const QVector<Shape::NurbsCurve2D> &connectedCurves :
             connectedCurveGroups(remainingCurves)) {
            // Keep fragments that still meet at a closed curve's seam in one
            // selectable object, but separate disconnected trim remainders.
            Shape updated = shape;
            if (shape.geometryType == GeometryType::Line && connectedCurves.size() == 1) {
                updated.geometryType = GeometryType::Line;
                updated.nurbs = connectedCurves.first();
                updated.points = updated.nurbs.controlPoints;
                updated.subdivisionParameters.clear();
                updated.components.clear();
            } else {
                updated = polyCurveShapeForComponents(shape, connectedCurves);
            }
            replacement->append(updated);
        }
        return true;
    }

    void eraseAlongScreenSegment(const QPointF &start, const QPointF &end)
    {
        constexpr qreal eraserRadiusPixels = 10.0;
        constexpr qreal sampleSpacingPixels = 5.0;
        const qreal length = std::hypot(end.x() - start.x(), end.y() - start.y());
        const int sampleCount = std::max(
            1,
            static_cast<int>(std::ceil(length / sampleSpacingPixels)));

        int addedCandidates = 0;
        for (int sample = 0; sample <= sampleCount; ++sample) {
            const qreal fraction = static_cast<qreal>(sample) / sampleCount;
            const QPointF cursor = start + (end - start) * fraction;
            for (const ObjectId objectId : eraseTargetShapeIndices_) {
                const int shapeIndex = objectIndex(objectId);
                if (shapeIndex < 0 || shapeIndex >= shapes_.size()) {
                    continue;
                }
                if (eraseCandidateShapeIndices_.contains(objectId)) {
                    continue;
                }

                if (distanceToCachedEraseShape(cursor, shapeIndex) <= eraserRadiusPixels) {
                    eraseCandidateShapeIndices_.append(objectId);
                    ++addedCandidates;
                }
            }
        }

        if (addedCandidates > 0) {
            DebugLog::instance().write(
                QStringLiteral("erase candidates added=%1 total=%2")
                    .arg(addedCandidates)
                    .arg(eraseCandidateShapeIndices_.size()));
        }
    }

    void applyEraseCandidates(const QRectF *trimBox = nullptr,
                              int onlyComponentIndex = -1)
    {
        if (eraseCandidateShapeIndices_.isEmpty()) {
            return;
        }
        QElapsedTimer operationTimer;
        operationTimer.start();

        QVector<int> indices;
        for (const ObjectId objectId : eraseCandidateShapeIndices_) {
            const int index = objectIndex(objectId);
            if (index >= 0) {
                indices.append(index);
            }
        }
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());

        QVector<QPair<int, QVector<Shape>>> changes;
        qint64 geometryEvaluationUs = 0;
        for (auto iterator = indices.crbegin(); iterator != indices.crend(); ++iterator) {
            if (*iterator < 0 || *iterator >= shapes_.size()) {
                continue;
            }

            QVector<Shape> replacement;
            QElapsedTimer evaluationTimer;
            evaluationTimer.start();
            const bool changed = trimShapeAtEraserStroke(shapes_[*iterator],
                                                         eraseStrokeScreenPath_,
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
                changes.append(qMakePair(*iterator, replacement));
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
        recordGeometryChange();
        int removedCount = 0;
        int generatedPieceCount = 0;
        QVector<ObjectId> removedObjectIds;
        QStringList changedObjectDescriptions;
        for (const QPair<int, QVector<Shape>> &change : changes) {
            const int shapeIndex = change.first;
            if (shapeIndex < 0 || shapeIndex >= shapes_.size()) {
                continue;
            }

            const ObjectId objectId = shapes_.objectIdAt(shapeIndex);
            generatedPieceCount += change.second.size();
            changedObjectDescriptions.append(
                QStringLiteral("%1:%2:%3->%4")
                    .arg(shapeIndex)
                    .arg(objectId.value())
                    .arg(geometryTypeName(shapes_[shapeIndex].geometryType))
                    .arg(change.second.size()));
            if (change.second.isEmpty()) {
                shapes_.removeAt(shapeIndex);
                removedObjectIds.append(objectId);
                ++removedCount;
            } else {
                const SceneObject *sourceObject = document_.object(objectId);
                const SceneObject sourceObjectCopy = sourceObject != nullptr
                                                         ? *sourceObject
                                                         : SceneObject{};
                shapes_.replace(objectId, change.second.first());
                for (int pieceIndex = 1; pieceIndex < change.second.size(); ++pieceIndex) {
                    SceneObject piece = sourceObjectCopy;
                    piece.id = ObjectId::invalid();
                    piece.geometry = change.second[pieceIndex];
                    shapes_.insertObject(shapeIndex + pieceIndex, piece);
                }
            }
        }

        const QVector<ObjectId> oldSelectedShapeIndices = selectedShapeIndices_;
        const ObjectId oldSelectedShapeIndex = selectedShapeIndex_;
        selectedShapeIndices_.clear();
        for (const ObjectId objectId : oldSelectedShapeIndices) {
            if (objectIndex(objectId) >= 0 && !removedObjectIds.contains(objectId)) {
                selectedShapeIndices_.append(objectId);
            }
        }
        selectedShapeIndex_ = objectIndex(oldSelectedShapeIndex) >= 0 &&
                                      !removedObjectIds.contains(oldSelectedShapeIndex)
                                  ? oldSelectedShapeIndex
                                  : ObjectId::invalid();
        if (!selectedShapeIndex_.isValid() && !selectedShapeIndices_.isEmpty()) {
            selectedShapeIndex_ = selectedShapeIndices_.back();
        }
        draggingSelected_ = false;
        draggingShapeIndices_.clear();
        draggingControlPoint_ = false;
        controlPointIndex_ = -1;
        currentDragSnap_ = DragSnapResult{};
        dragSnapLocked_ = false;
        dragHistoryRecorded_ = false;
        if (activeTool_ == Tool::Trim) {
            QStringList removedIds;
            for (const ObjectId objectId : removedObjectIds) {
                removedIds.append(QString::number(objectId.value()));
            }
            DebugLog::instance().write(
                QStringLiteral("trim commit complete mode=%1 candidates=%2 changedObjects=[%3] generatedPieces=%4 removedObjects=[%5] geometryUs=%6 commitUs=%7 totalUs=%8")
                    .arg(trimBox != nullptr ? QStringLiteral("box")
                                            : QStringLiteral("click"))
                    .arg(indices.size())
                    .arg(changedObjectDescriptions.join(','))
                    .arg(generatedPieceCount)
                    .arg(removedIds.join(','))
                    .arg(geometryEvaluationUs)
                    .arg(commitTimer.nsecsElapsed() / 1000)
                    .arg(operationTimer.nsecsElapsed() / 1000));
        }
        DebugLog::instance().write(QStringLiteral("erase segment applied candidates=%1 changed=%2 removed=%3 shapes=%4")
                                       .arg(indices.size())
                                       .arg(changes.size())
                                       .arg(removedCount)
                                       .arg(shapes_.size()));
        update();
    }

    void cancelEraseStroke()
    {
        eraseStrokeActive_ = false;
        eraseCursorPressed_ = false;
        eraseCandidateShapeIndices_.clear();
        eraseStrokeScreenPath_.clear();
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
        scaleShapeIds_.clear();
        scaleStep_ = 0;
        scaleBaseWorld_ = QPointF();
        scaleReferenceWorld_ = QPointF();
        scaleAxisDirection_ = QPointF();
        scaleReferenceLength_ = 0.0;
        scaleUsingTypedFactor_ = false;
        scaleTypedFactor_ = 1.0;
        scaleFactorText_.clear();
        scalePreviewFactor_ = 1.0;
        scalePreviewAxis_ = QPointF();
        scalePreviewValid_ = false;
    }

    QString scalePrompt() const
    {
        if (scaleStep_ == 0) {
            return QStringLiteral("%1: click a base point, or press Enter for the selection center")
                .arg(scaleModeName(scaleMode_));
        }
        if (scaleStep_ == 1) {
            const QString numeric = scaleFactorText_.isEmpty()
                                        ? QString()
                                        : QStringLiteral("  Factor: %1").arg(scaleFactorText_);
            return QStringLiteral("%1: type a factor + Enter, or click the first reference point%2")
                .arg(scaleModeName(scaleMode_), numeric);
        }
        if (scaleUsingTypedFactor_) {
            return QStringLiteral("Scale 1D: click the scale direction  •  Factor %1")
                .arg(scaleTypedFactor_, 0, 'g', 8);
        }
        return scaleMode_ == ScaleMode::OneD
                   ? QStringLiteral("Scale 1D: click the second reference point along the first-point axis")
                   : QStringLiteral("%1: click the second reference point")
                         .arg(scaleModeName(scaleMode_));
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
        for (const ObjectId objectId : scaleShapeIds_) {
            const int shapeIndex = objectIndex(objectId);
            if (shapeIndex < 0) {
                continue;
            }
            const QRectF bounds = selectionBoundsForShape(shapes_[shapeIndex]);
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
        if (shape == nullptr) {
            return;
        }

        if (mode == ScaleMode::OneD &&
            shape->geometryType == GeometryType::Rectangle && shape->points.size() == 2) {
            shape->points = rectangleVertices(*shape);
        }

        const auto scaledPoint = [base, axisDirection, factor, mode](const QPointF &point) {
            const QPointF offset = point - base;
            if (mode != ScaleMode::OneD) {
                return base + offset * factor;
            }
            const qreal alongAxis = QPointF::dotProduct(offset, axisDirection);
            return base + offset + axisDirection * (alongAxis * (factor - 1.0));
        };

        for (QPointF &point : shape->points) {
            point = scaledPoint(point);
        }
        for (QPointF &point : shape->nurbs.controlPoints) {
            point = scaledPoint(point);
        }
        for (Shape::NurbsCurve2D &component : shape->components) {
            for (QPointF &point : component.controlPoints) {
                point = scaledPoint(point);
            }
        }
    }

    qreal scaleFactorAtPoint(const QPointF &point) const
    {
        if (scaleReferenceLength_ <= 1.0e-12) {
            return 1.0;
        }
        const QPointF offset = point - scaleBaseWorld_;
        if (scaleMode_ == ScaleMode::OneD) {
            return QPointF::dotProduct(offset, scaleAxisDirection_) /
                   scaleReferenceLength_;
        }
        return std::hypot(offset.x(), offset.y()) / scaleReferenceLength_;
    }

    void updateScalePreview(const QPointF &point)
    {
        scalePreviewValid_ = false;
        if (activeTool_ != Tool::Scale || scaleStep_ != 2) {
            return;
        }

        if (scaleUsingTypedFactor_) {
            const QPointF direction = point - scaleBaseWorld_;
            const qreal length = std::hypot(direction.x(), direction.y());
            if (length <= 1.0e-12) {
                return;
            }
            scalePreviewAxis_ = direction / length;
            scalePreviewFactor_ = scaleTypedFactor_;
        } else {
            scalePreviewAxis_ = scaleAxisDirection_;
            scalePreviewFactor_ = scaleFactorAtPoint(point);
        }

        scalePreviewValid_ = std::isfinite(scalePreviewFactor_) &&
                             (scaleMode_ != ScaleMode::OneD ||
                              std::hypot(scalePreviewAxis_.x(),
                                         scalePreviewAxis_.y()) > 1.0e-12);
    }

    void finishScale()
    {
        resetScaleInteraction();
        currentSnap_ = SnapResult{};
        setTool(Tool::Select);
        if (commandFinished_) {
            commandFinished_(Tool::Select);
        }
        update();
    }

    void commitScale(qreal factor, const QPointF &axisDirection)
    {
        if (!std::isfinite(factor)) {
            return;
        }

        const bool changed = std::abs(factor - 1.0) > 1.0e-12;
        if (changed) {
            recordGeometryChange();
            for (const ObjectId objectId : scaleShapeIds_) {
                const int shapeIndex = objectIndex(objectId);
                if (shapeIndex >= 0 && document_.isObjectEditable(objectId)) {
                    scaleShapeGeometry(&shapes_[shapeIndex],
                                       scaleBaseWorld_,
                                       axisDirection,
                                       factor,
                                       scaleMode_);
                }
            }
            updateAssociativeDimensions(document_, curveSampler_);
            notifyLayersChanged();
        }

        DebugLog::instance().write(QStringLiteral("scale committed mode=%1 factor=%2 base=%3 objects=%4 changed=%5")
                                       .arg(scaleModeName(scaleMode_))
                                       .arg(factor, 0, 'g', 10)
                                       .arg(pointText(scaleBaseWorld_))
                                       .arg(scaleShapeIds_.size())
                                       .arg(changed));
        finishScale();
    }

    bool handleScalePoint(const QPointF &worldPoint)
    {
        if (activeTool_ != Tool::Scale || scaleShapeIds_.isEmpty()) {
            return false;
        }

        if (scaleStep_ == 0) {
            scaleBaseWorld_ = worldPoint;
            scaleStep_ = 1;
            pendingPoints_ = {scaleBaseWorld_};
            publishScalePrompt();
            update();
            return true;
        }

        if (scaleStep_ == 1) {
            const QPointF reference = worldPoint - scaleBaseWorld_;
            scaleReferenceLength_ = std::hypot(reference.x(), reference.y());
            if (scaleReferenceLength_ <= 1.0e-12) {
                return false;
            }
            scaleReferenceWorld_ = worldPoint;
            scaleAxisDirection_ = reference / scaleReferenceLength_;
            scaleUsingTypedFactor_ = false;
            scaleStep_ = 2;
            updateScalePreview(worldPoint);
            publishScalePrompt();
            update();
            return true;
        }

        updateScalePreview(worldPoint);
        if (!scalePreviewValid_) {
            return false;
        }
        commitScale(scalePreviewFactor_, scalePreviewAxis_);
        return true;
    }

    void cancelScale()
    {
        if (activeTool_ != Tool::Scale && scaleShapeIds_.isEmpty()) {
            return;
        }
        resetScaleInteraction();
        pendingPoints_.clear();
        currentSnap_ = SnapResult{};
        setTool(Tool::Select);
        if (commandFinished_) {
            commandFinished_(Tool::Select);
        }
        update();
        DebugLog::instance().write(QStringLiteral("scale canceled"));
    }

    void resetRotateInteraction()
    {
        rotateShapeIndices_.clear();
        rotateStep_ = 0;
        rotateFrame_ = {};
        rotatePrimaryFrame_ = {};
        rotatePrePivotPlaneFrame_ = {};
        rotatePrePivotFloorFrame_ = {};
        rotatePrePivotFloorNormal_ = {};
        rotateBaseWorldPoint_ = {};
        rotateReferenceWorldPoint_ = {};
        rotateReferenceNormal_ = {};
        rotateBaseWorld_ = QPointF();
        rotateReferenceWorld_ = QPointF();
        rotatePreviewAngle_ = 0.0;
        rotateAccumulatedAngle_ = 0.0;
        rotateLastRawAngle_ = 0.0;
        rotateReferenceAngle_ = 0.0;
        rotateHasPreviousAngle_ = false;
        rotateAngleSnapEnabled_ = rotateToolPreferences_.angleSnapEnabled;
        rotateAngleInputActive_ = false;
        rotateAngleInputManual_ = false;
        rotateAngleInput_.clear();
        rotateAngleInputDirection_ = 1.0;
        rotateAngleInputDirectionCaptured_ = false;
        rotateLastPointerPoint_ = QPointF();
        rotatePerpendicularActive_ = false;
        rotatePrePivotPerpendicularActive_ = false;
        rotateAxisLockKey_ = 0;
        currentSnap_ = SnapResult{};
    }

    qreal rotationAngleForPoint(const QPointF &worldPoint) const
    {
        const QPointF startVector = rotateReferenceWorld_ - rotateBaseWorld_;
        const QPointF endVector = worldPoint - rotateBaseWorld_;
        if (std::hypot(startVector.x(), startVector.y()) <= 1.0e-12 ||
            std::hypot(endVector.x(), endVector.y()) <= 1.0e-12) {
            return 0.0;
        }

        constexpr qreal pi = 3.14159265358979323846;
        qreal angle = std::atan2(endVector.y(), endVector.x()) -
                      std::atan2(startVector.y(), startVector.x());
        return std::remainder(angle, 2.0 * pi);
    }

    qreal rotateSnapIncrementDegrees() const
    {
        return rotateToolPreferences_.useRadians
                   ? rotateToolPreferences_.angleSnapIncrementRadiansDegrees
                   : rotateToolPreferences_.angleSnapIncrementDegrees;
    }

    qreal snapRotateAngle(qreal angle) const
    {
        constexpr qreal pi = 3.14159265358979323846;
        const qreal increment = rotateSnapIncrementDegrees() * pi / 180.0;
        const qreal strength =
            rotateToolPreferences_.angleSnapStrengthDegrees * pi / 180.0;
        const qreal nearest = std::round(angle / increment) * increment;
        return std::abs(angle - nearest) <= strength ? nearest : angle;
    }

    void updateRotateReferencePreview(const QPointF &point)
    {
        if (rotateStep_ != 1) {
            return;
        }
        const QPointF planePoint = rotateSnappedPoint(point);
        const QPointF offset = planePoint - rotateBaseWorld_;
        const qreal radius = std::hypot(offset.x(), offset.y());
        if (radius <= 1.0e-12) {
            return;
        }

        qreal angle = std::atan2(offset.y(), offset.x());
        if (rotateAngleSnapEnabled_ && !currentSnap_.isValid()) {
            angle = snapRotateAngle(angle);
        }
        cursorWorld_ = rotateBaseWorld_ +
                       QPointF(radius * std::cos(angle),
                               radius * std::sin(angle));
    }

    void updateRotatePreview(const QPointF &point)
    {
        if (rotateStep_ != 2) {
            return;
        }

        rotateLastPointerPoint_ = rotateSnappedPoint(point);
        if (rotateAngleInputManual_) {
            updateRotateCursorForPreviewAngle();
            return;
        }

        const QPointF offset = rotateLastPointerPoint_ - rotateBaseWorld_;
        if (std::hypot(offset.x(), offset.y()) <= 1.0e-12) {
            return;
        }

        const qreal radius = std::hypot(offset.x(), offset.y());
        qreal rawAngle = std::atan2(offset.y(), offset.x());
        if (currentSnap_.isValid()) {
            cursorWorld_ = rotateLastPointerPoint_;
        } else if (rotateAngleSnapEnabled_) {
            rawAngle = snapRotateAngle(rawAngle);
            cursorWorld_ = rotateBaseWorld_ +
                           QPointF(radius * std::cos(rawAngle),
                                   radius * std::sin(rawAngle));
        }

        constexpr qreal pi = 3.14159265358979323846;
        constexpr qreal twoPi = 2.0 * pi;
        if (!rotateHasPreviousAngle_) {
            rotateLastRawAngle_ = rawAngle;
            rotateHasPreviousAngle_ = true;
        } else {
            qreal delta = rawAngle - rotateLastRawAngle_;
            if (delta > pi) {
                delta -= twoPi;
            } else if (delta < -pi) {
                delta += twoPi;
            }
            rotateAccumulatedAngle_ += delta;
            rotateLastRawAngle_ = rawAngle;
        }
        rotatePreviewAngle_ = rotateAccumulatedAngle_;
    }

    QPointF rotateSnappedPoint(const QPointF &point) const
    {
        if (rotateStep_ > 0 && currentSnap_.isValid() &&
            currentSnap_.hasWorldPoint &&
            isValidWorkPlaneFrame(rotateFrame_)) {
            return worldPointToWorkPlaneFrame(currentSnap_.worldPoint,
                                              rotateFrame_);
        }
        return point;
    }

    void updateRotateCursorForPreviewAngle()
    {
        if (rotateStep_ != 2) {
            return;
        }
        QPointF radial = rotateLastPointerPoint_ - rotateBaseWorld_;
        qreal radius = std::hypot(radial.x(), radial.y());
        if (radius <= 1.0e-12) {
            radial = rotateReferenceWorld_ - rotateBaseWorld_;
            radius = std::hypot(radial.x(), radial.y());
        }
        if (radius <= 1.0e-12) {
            return;
        }

        const qreal angle = rotateReferenceAngle_ + rotatePreviewAngle_;
        cursorWorld_ = rotateBaseWorld_ +
                       QPointF(radius * std::cos(angle),
                               radius * std::sin(angle));
        rawCursorWorld_ = cursorWorld_;
        lastWorldPosition_ = cursorWorld_;
    }

    bool setRotateTypedAngle(const QString &text)
    {
        bool validAngle = false;
        const qreal degrees = text.toDouble(&validAngle);
        if (!validAngle || !std::isfinite(degrees)) {
            return false;
        }
        if (!rotateAngleInputDirectionCaptured_) {
            rotateAngleInputDirection_ = rotateAccumulatedAngle_ < -1.0e-12
                                             ? -1.0
                                             : 1.0;
            rotateAngleInputDirectionCaptured_ = true;
        }
        const qreal angleRadians =
            degrees * 3.14159265358979323846 / 180.0;
        rotatePreviewAngle_ = rotateAngleInputDirection_ * angleRadians;
        rotateAccumulatedAngle_ = rotatePreviewAngle_;
        rotateAngleInputManual_ = true;
        updateRotateCursorForPreviewAngle();
        return true;
    }

    void rotateShapeGeometry(Shape *shape, qreal angle) const
    {
        if (shape == nullptr || !isValidWorkPlaneFrame(rotateFrame_)) {
            return;
        }

        shape->workPlaneFrame = rotateWorkPlaneFrame(
            shapeWorkPlaneFrame(*shape),
            rotateBaseWorldPoint_,
            rotateFrame_.normal,
            angle);
    }

    void rotateShapes(const QVector<ObjectId> &objectIds, qreal angle)
    {
        for (const ObjectId objectId : objectIds) {
            const int shapeIndex = objectIndex(objectId);
            if (shapeIndex >= 0 && document_.isObjectEditable(objectId)) {
                rotateShapeGeometry(&shapes_[shapeIndex], angle);
            }
        }
    }

    void commitRotate(qreal angle)
    {
        if (!std::isfinite(angle)) {
            return;
        }
        if (std::abs(angle) > 1.0e-12) {
            recordGeometryChange();
            rotateShapes(rotateShapeIndices_, angle);
            updateAssociativeDimensions(document_, curveSampler_);
            notifyLayersChanged();
        }

        DebugLog::instance().write(QStringLiteral("rotate committed shapes=%1 angleDegrees=%2 pivot=%3 reference=%4")
                                       .arg(rotateShapeIndices_.size())
                                       .arg(angle * 180.0 / 3.14159265358979323846, 0, 'f', 4)
                                       .arg(precisePoint3DText(rotateBaseWorldPoint_))
                                       .arg(pointText(rotateReferenceWorld_)));
        resetRotateInteraction();
        setTool(Tool::Select);
        if (commandFinished_) {
            commandFinished_(Tool::Select);
        }
        update();
    }

    bool handleRotatePoint(const QPointF &worldPoint)
    {
        if (activeTool_ != Tool::Rotate || rotateShapeIndices_.isEmpty()) {
            return false;
        }

        constexpr qreal minimumPointDistance = 1.0e-9;
        if (rotateStep_ == 0) {
            const WorkPlaneFrame inputFrame =
                isValidWorkPlaneFrame(viewportTransform_.workPlaneFrame())
                    ? viewportTransform_.workPlaneFrame()
                    : makeWorkPlaneFrame(WorkPlane::XY, 0.0);
            rotateBaseWorldPoint_ = currentSnap_.isValid() &&
                                            currentSnap_.hasWorldPoint
                                        ? currentSnap_.worldPoint
                                        : workPlaneFramePointToWorld(worldPoint,
                                                                     inputFrame);
            rotateFrame_ = inputFrame;
            rotateFrame_.origin = rotateBaseWorldPoint_;
            const bool usedPrePivotPerpendicular =
                rotatePrePivotPerpendicularActive_ &&
                isValidWorkPlaneFrame(rotatePrePivotFloorFrame_);
            rotatePrimaryFrame_ = usedPrePivotPerpendicular
                                      ? rotatePrePivotFloorFrame_
                                      : rotateFrame_;
            rotatePrimaryFrame_.origin = rotateBaseWorldPoint_;
            rotateReferenceNormal_ = usedPrePivotPerpendicular
                                         ? rotatePrePivotFloorNormal_
                                         : rotateFrame_.normal;
            rotatePrePivotPerpendicularActive_ = false;
            rotatePrePivotPlaneFrame_ = {};
            rotatePrePivotFloorFrame_ = {};
            rotatePrePivotFloorNormal_ = {};
            rotateAxisLockKey_ = 0;
            rotateBaseWorld_ = QPointF();
            rotateReferenceWorld_ = QPointF();
            rotateStep_ = 1;
            rotatePreviewAngle_ = 0.0;
            rotateAccumulatedAngle_ = 0.0;
            rotateHasPreviousAngle_ = false;
            rotateAngleInputManual_ = false;
            rotateAngleInputActive_ = false;
            rotateAngleInput_.clear();
            currentSnap_ = SnapResult{};
            cursorWorld_ = rotateBaseWorld_;
            viewportTransform_.setWorkPlaneFrame(rotateFrame_);
            DebugLog::instance().write(QStringLiteral("rotate pivot=%1 planeNormal=(%2,%3,%4)")
                                           .arg(precisePoint3DText(rotateBaseWorldPoint_))
                                           .arg(rotateFrame_.normal.x, 0, 'g', 10)
                                           .arg(rotateFrame_.normal.y, 0, 'g', 10)
                                           .arg(rotateFrame_.normal.z, 0, 'g', 10));
            update();
            return true;
        }

        if (rotateStep_ == 1) {
            updateRotateReferencePreview(worldPoint);
            const QPointF reference = cursorWorld_ - rotateBaseWorld_;
            const qreal radius = std::hypot(reference.x(), reference.y());
            if (radius <= minimumPointDistance) {
                DebugLog::instance().write(QStringLiteral("rotate reference ignored at center"));
                return false;
            }

            rotateReferenceWorld_ = cursorWorld_;
            rotateReferenceWorldPoint_ = workPlaneFramePointToWorld(
                rotateReferenceWorld_, rotateFrame_);
            rotateLastPointerPoint_ = rotateReferenceWorld_;
            rotateReferenceAngle_ = std::atan2(reference.y(), reference.x());
            rotateStep_ = 2;
            rotatePreviewAngle_ = 0.0;
            rotateAccumulatedAngle_ = 0.0;
            rotateLastRawAngle_ = rotateReferenceAngle_;
            rotateHasPreviousAngle_ = true;
            rotateAngleInputManual_ = false;
            rotateAngleInputActive_ = false;
            rotateAngleInput_.clear();
            DebugLog::instance().write(QStringLiteral("rotate reference=%1")
                                           .arg(pointText(rotateReferenceWorld_)));
            update();
            return true;
        }

        updateRotatePreview(worldPoint);
        if (rotateAngleInputActive_ && !rotateAngleInput_.isEmpty()) {
            if (!setRotateTypedAngle(rotateAngleInput_)) {
                return false;
            }
        }
        if (std::hypot(worldPoint.x() - rotateBaseWorld_.x(),
                       worldPoint.y() - rotateBaseWorld_.y()) <=
                minimumPointDistance &&
            std::abs(rotatePreviewAngle_) <= minimumPointDistance) {
            DebugLog::instance().write(QStringLiteral("rotate final point ignored at center"));
            return false;
        }

        commitRotate(rotatePreviewAngle_);
        return true;
    }

    void remapRotateCursorToFrame(const WorkPlaneFrame &frame,
                                  const WorkPlaneFrame &previousFrame)
    {
        const Point3D previousWorld = workPlaneFramePointToWorld(cursorWorld_,
                                                                  previousFrame);
        viewportTransform_.setWorkPlaneFrame(frame);
        QPointF remapped;
        if (!viewportTransform_.screenToWorkPlane(
                QPointF(lastMousePosition_), size(), frame, &remapped)) {
            remapped = worldPointToWorkPlaneFrame(previousWorld, frame);
        }
        cursorWorld_ = remapped;
        rawCursorWorld_ = remapped;
        lastWorldPosition_ = remapped;
        if (currentSnap_.isValid() && currentSnap_.hasWorldPoint) {
            currentSnap_.point = worldPointToWorkPlaneFrame(
                currentSnap_.worldPoint, frame);
        }
    }

    bool setRotatePrePivotAxisPlane(int key)
    {
        const WorkPlaneFrame currentFrame = viewportTransform_.workPlaneFrame();
        if (!isValidWorkPlaneFrame(currentFrame)) {
            return false;
        }
        if (rotateAxisLockKey_ == key) {
            rotateAxisLockKey_ = 0;
            rotatePrePivotPerpendicularActive_ = false;
            rotatePrePivotPlaneFrame_ = {};
            rotatePrePivotFloorFrame_ = {};
            rotatePrePivotFloorNormal_ = {};
            updateDrawingWorkPlaneFromHover(QPointF(lastMousePosition_));
            return true;
        }

        const Point3D cursorWorld = workPlaneFramePointToWorld(cursorWorld_,
                                                               currentFrame);
        Point3D normal;
        if (key == Qt::Key_X) normal = {1.0, 0.0, 0.0};
        if (key == Qt::Key_Y) normal = {0.0, 1.0, 0.0};
        if (key == Qt::Key_Z) normal = {0.0, 0.0, 1.0};
        const Point3D preferred = key == Qt::Key_X
                                      ? currentFrame.yAxis
                                      : currentFrame.xAxis;
        rotatePrePivotPlaneFrame_ = makeWorkPlaneFrameFromNormal(
            cursorWorld, normal, preferred);
        if (!isValidWorkPlaneFrame(rotatePrePivotPlaneFrame_)) {
            return false;
        }
        rotateAxisLockKey_ = key;
        rotatePrePivotPerpendicularActive_ = false;
        rotatePrePivotFloorFrame_ = {};
        rotatePrePivotFloorNormal_ = {};
        remapRotateCursorToFrame(rotatePrePivotPlaneFrame_, currentFrame);
        return true;
    }

    bool toggleRotatePerpendicularPlane()
    {
        if (rotateStep_ == 0) {
            if (rotatePrePivotPerpendicularActive_) {
                rotatePrePivotPerpendicularActive_ = false;
                rotatePrePivotPlaneFrame_ = {};
                rotatePrePivotFloorFrame_ = {};
                rotatePrePivotFloorNormal_ = {};
                rotateAxisLockKey_ = 0;
                updateDrawingWorkPlaneFromHover(QPointF(lastMousePosition_));
                return true;
            }

            const WorkPlaneFrame baseFrame = viewportTransform_.workPlaneFrame();
            if (!isValidWorkPlaneFrame(baseFrame)) {
                return true;
            }
            const Point3D viewDirection = viewportTransform_.viewDirection();
            const qreal xAlignment = std::abs(arcVectorDot(viewDirection,
                                                           baseFrame.xAxis));
            const qreal yAlignment = std::abs(arcVectorDot(viewDirection,
                                                           baseFrame.yAxis));
            const Point3D normal = xAlignment > yAlignment
                                       ? baseFrame.xAxis
                                       : baseFrame.yAxis;
            const Point3D preferred = xAlignment > yAlignment
                                          ? baseFrame.yAxis
                                          : baseFrame.xAxis;
            const Point3D cursorWorld = workPlaneFramePointToWorld(cursorWorld_,
                                                                   baseFrame);
            rotatePrePivotPlaneFrame_ = makeWorkPlaneFrameFromNormal(
                cursorWorld, normal, preferred);
            if (isValidWorkPlaneFrame(rotatePrePivotPlaneFrame_)) {
                rotatePrePivotFloorFrame_ = baseFrame;
                rotatePrePivotFloorNormal_ = baseFrame.normal;
                rotatePrePivotPerpendicularActive_ = true;
                rotateAxisLockKey_ = 0;
                remapRotateCursorToFrame(rotatePrePivotPlaneFrame_, baseFrame);
            }
            return true;
        }

        const WorkPlaneFrame oldFrame = rotateFrame_;
        Point3D targetWorld = workPlaneFramePointToWorld(cursorWorld_, oldFrame);
        WorkPlaneFrame nextFrame;
        if (rotatePerpendicularActive_) {
            nextFrame = rotatePrimaryFrame_;
            rotatePerpendicularActive_ = false;
        } else {
            const Point3D bridgePoint = rotateStep_ == 1
                                            ? targetWorld
                                            : rotateReferenceWorldPoint_;
            Point3D bridge = arcVectorSubtract(bridgePoint,
                                               rotateBaseWorldPoint_);
            bridge = arcVectorSubtract(
                bridge,
                arcVectorScale(rotateReferenceNormal_,
                               arcVectorDot(bridge,
                                            rotateReferenceNormal_)));
            const qreal bridgeLength = arcVectorLength(bridge);
            if (bridgeLength <= 1.0e-9) {
                return true;
            }
            const Point3D xAxis = arcVectorScale(bridge, 1.0 / bridgeLength);
            const Point3D yAxis = rotateReferenceNormal_;
            const Point3D normal = arcVectorCross(xAxis, yAxis);
            nextFrame = WorkPlaneFrame{rotateBaseWorldPoint_,
                                       xAxis,
                                       yAxis,
                                       normal,
                                       true};
            rotatePerpendicularActive_ = true;
        }

        if (!isValidWorkPlaneFrame(nextFrame)) {
            return true;
        }
        rotateFrame_ = nextFrame;
        viewportTransform_.setWorkPlaneFrame(rotateFrame_);
        cursorWorld_ = worldPointToWorkPlaneFrame(targetWorld, rotateFrame_);
        rawCursorWorld_ = cursorWorld_;
        lastWorldPosition_ = cursorWorld_;
        if (currentSnap_.isValid() && currentSnap_.hasWorldPoint) {
            currentSnap_.point = worldPointToWorkPlaneFrame(
                currentSnap_.worldPoint, rotateFrame_);
        }
        if (rotateStep_ == 2) {
            rotateReferenceWorld_ = worldPointToWorkPlaneFrame(
                rotateReferenceWorldPoint_, rotateFrame_);
            const QPointF reference = rotateReferenceWorld_ - rotateBaseWorld_;
            rotateReferenceAngle_ = std::atan2(reference.y(), reference.x());
            rotateAngleInputActive_ = false;
            rotateAngleInputManual_ = false;
            rotateAngleInput_.clear();
            const QPointF direction = cursorWorld_ - rotateBaseWorld_;
            if (std::hypot(direction.x(), direction.y()) > 1.0e-12) {
                rotateLastRawAngle_ = std::atan2(direction.y(), direction.x());
                rotateAccumulatedAngle_ = rotationAngleForPoint(cursorWorld_);
                rotatePreviewAngle_ = rotateAccumulatedAngle_;
                rotateHasPreviousAngle_ = true;
            } else {
                rotateHasPreviousAngle_ = false;
            }
            rotateLastPointerPoint_ = cursorWorld_;
        }
        return true;
    }

    bool handleRotateKey(QKeyEvent *event)
    {
        if (event == nullptr || activeTool_ != Tool::Rotate ||
            event->isAutoRepeat() || event->modifiers() != Qt::NoModifier) {
            return false;
        }

        if ((event->key() == Qt::Key_Return ||
             event->key() == Qt::Key_Enter ||
             event->key() == Qt::Key_Space) && rotateStep_ == 2) {
            if (rotateAngleInputActive_ && !rotateAngleInput_.isEmpty()) {
                if (!setRotateTypedAngle(rotateAngleInput_)) {
                    event->accept();
                    return true;
                }
            }
            commitRotate(rotatePreviewAngle_);
            event->accept();
            return true;
        }

        if (rotateStep_ == 2 && event->key() == Qt::Key_A) {
            rotateAngleInputActive_ = true;
            rotateAngleInputManual_ = false;
            rotateAngleInput_.clear();
            rotateAngleInputDirectionCaptured_ = false;
            update();
            event->accept();
            return true;
        }

        if (event->key() == Qt::Key_C) {
            rotateAngleSnapEnabled_ = !rotateAngleSnapEnabled_;
            if (rotateStep_ == 1) {
                updateRotateReferencePreview(cursorWorld_);
            } else if (rotateStep_ == 2) {
                if (rotateAngleSnapEnabled_ && !currentSnap_.isValid()) {
                    const QPointF offset = cursorWorld_ - rotateBaseWorld_;
                    if (std::hypot(offset.x(), offset.y()) > 1.0e-12) {
                        rotateLastRawAngle_ = std::atan2(offset.y(), offset.x());
                        rotateHasPreviousAngle_ = true;
                    }
                }
            }
            update();
            event->accept();
            return true;
        }

        if (event->key() == Qt::Key_P) {
            toggleRotatePerpendicularPlane();
            update();
            event->accept();
            return true;
        }

        if (rotateStep_ == 0 &&
            (event->key() == Qt::Key_X || event->key() == Qt::Key_Y ||
             event->key() == Qt::Key_Z)) {
            setRotatePrePivotAxisPlane(event->key());
            update();
            event->accept();
            return true;
        }

        if (rotateStep_ == 2 && rotateAngleInputActive_) {
            if (event->key() == Qt::Key_Backspace) {
                rotateAngleInput_.chop(1);
                rotateAngleInputManual_ = !rotateAngleInput_.isEmpty();
                if (rotateAngleInput_.isEmpty()) {
                    rotateAngleInputDirectionCaptured_ = false;
                    rotateAngleInputManual_ = false;
                    updateRotatePreview(rotateLastPointerPoint_);
                } else {
                    setRotateTypedAngle(rotateAngleInput_);
                }
                update();
                event->accept();
                return true;
            }

            const QString text = event->text();
            if (text.size() == 1) {
                const QChar character = text.front();
                const bool digit = character.isDigit();
                const bool decimal = character == QLatin1Char('.') &&
                                     !rotateAngleInput_.contains(QLatin1Char('.'));
                const bool sign = (character == QLatin1Char('-') ||
                                   character == QLatin1Char('+')) &&
                                  rotateAngleInput_.isEmpty();
                if (digit || decimal || sign) {
                    if (rotateAngleInput_.isEmpty()) {
                        rotateAngleInputDirectionCaptured_ = false;
                    }
                    rotateAngleInput_.append(character);
                    setRotateTypedAngle(rotateAngleInput_);
                    update();
                    event->accept();
                    return true;
                }
            }
        }

        if (rotateStep_ == 2) {
            const QString text = event->text();
            if (text.size() == 1 &&
                (text.front().isDigit() || text.front() == QLatin1Char('.') ||
                 text.front() == QLatin1Char('-') ||
                 text.front() == QLatin1Char('+'))) {
                rotateAngleInputActive_ = true;
                rotateAngleInput_ = text;
                rotateAngleInputDirectionCaptured_ = false;
                setRotateTypedAngle(rotateAngleInput_);
                update();
                event->accept();
                return true;
            }
        }
        return false;
    }

    void cancelRotate()
    {
        if (activeTool_ != Tool::Rotate && rotateShapeIndices_.isEmpty()) {
            return;
        }

        resetRotateInteraction();
        setTool(Tool::Select);
        if (commandFinished_) {
            commandFinished_(Tool::Select);
        }
        update();
        DebugLog::instance().write(QStringLiteral("rotate canceled"));
    }

    void resetMirrorInteraction()
    {
        mirrorShapeIndices_.clear();
    }

    bool handleMirrorPoint(const QPointF &worldPoint)
    {
        if (activeTool_ != Tool::Mirror || mirrorShapeIndices_.isEmpty()) {
            return false;
        }

        pendingPoints_.append(worldPoint);
        if (pendingPoints_.size() < 2) {
            update();
            return true;
        }

        const QPointF axisStart = pendingPoints_[0];
        const QPointF axisEnd = pendingPoints_[1];
        QVector<SceneObject> mirroredObjects;
        mirroredObjects.reserve(mirrorShapeIndices_.size());
        for (const ObjectId objectId : mirrorShapeIndices_) {
            const SceneObject *sourceObject = document_.object(objectId);
            if (sourceObject == nullptr || !document_.isObjectEditable(objectId)) {
                continue;
            }

            Shape mirroredShape;
            if (!mirrorShapeAcrossLine(sourceObject->geometry,
                                       axisStart,
                                       axisEnd,
                                       &mirroredShape)) {
                continue;
            }

            SceneObject mirroredObject;
            mirroredObject.layerId = sourceObject->layerId;
            mirroredObject.geometry = mirroredShape;
            mirroredObjects.append(mirroredObject);
        }

        if (mirroredObjects.isEmpty()) {
            DebugLog::instance().write(QStringLiteral("mirror rejected axis=%1->%2 objects=%3")
                                           .arg(pointText(axisStart), pointText(axisEnd))
                                           .arg(mirrorShapeIndices_.size()));
            pendingPoints_.removeLast();
            update();
            return false;
        }

        const int sourceCount = mirrorShapeIndices_.size();
        recordGeometryChange();
        QVector<ObjectId> mirroredIds;
        mirroredIds.reserve(mirroredObjects.size());
        for (const SceneObject &mirroredObject : mirroredObjects) {
            mirroredIds.append(shapes_.insertObject(shapes_.size(), mirroredObject));
        }

        selection_.setObjectIds(mirroredIds);
        selectedShapeIndex_ = selection_.primaryObjectId();
        pendingPoints_.clear();
        resetMirrorInteraction();
        setTool(Tool::Select);
        if (commandFinished_) {
            commandFinished_(Tool::Select);
        }
        notifyLayersChanged();
        update();
        DebugLog::instance().write(QStringLiteral("mirror committed sourceCount=%1 copyCount=%2 axis=%3->%4")
                                       .arg(sourceCount)
                                       .arg(mirroredIds.size())
                                       .arg(pointText(axisStart), pointText(axisEnd)));
        return true;
    }

    void cancelMirror()
    {
        if (activeTool_ != Tool::Mirror && mirrorShapeIndices_.isEmpty()) {
            return;
        }

        pendingPoints_.clear();
        resetMirrorInteraction();
        setTool(Tool::Select);
        if (commandFinished_) {
            commandFinished_(Tool::Select);
        }
        update();
        DebugLog::instance().write(QStringLiteral("mirror canceled"));
    }

    void translateControlPoint(ObjectId objectId,
                               int controlPointIndex,
                               const QPointF &delta)
    {
        const int shapeIndex = objectIndex(objectId);
        if (shapeIndex < 0 || shapeIndex >= shapes_.size() ||
            controlPointIndex < 0 ||
            (qFuzzyIsNull(delta.x()) && qFuzzyIsNull(delta.y()))) {
            return;
        }

        Shape &shape = shapes_[shapeIndex];
        if (shape.geometryType == GeometryType::PolyCurve) {
            int remaining = controlPointIndex;
            for (int componentIndex = 0; componentIndex < shape.components.size(); ++componentIndex) {
                Shape::NurbsCurve2D &component = shape.components[componentIndex];
                if (remaining < component.controlPoints.size()) {
                    const QPointF oldPoint = component.controlPoints[remaining];
                    component.controlPoints[remaining] += delta;
                    const qreal seamTolerance = joinEndpointTolerance();
                    if (remaining == 0 && componentIndex > 0) {
                        Shape::NurbsCurve2D &previous = shape.components[componentIndex - 1];
                        if (!previous.controlPoints.isEmpty() &&
                            std::hypot(previous.controlPoints.last().x() - oldPoint.x(),
                                       previous.controlPoints.last().y() - oldPoint.y()) <=
                                seamTolerance) {
                            previous.controlPoints.last() += delta;
                        }
                    }
                    if (remaining == component.controlPoints.size() - 1 &&
                        componentIndex + 1 < shape.components.size()) {
                        Shape::NurbsCurve2D &next = shape.components[componentIndex + 1];
                        if (!next.controlPoints.isEmpty() &&
                            std::hypot(next.controlPoints.first().x() - oldPoint.x(),
                                       next.controlPoints.first().y() - oldPoint.y()) <=
                                seamTolerance) {
                            next.controlPoints.first() += delta;
                        }
                    }
                    shape.points = polyCurvePoints(shape.components);
                    return;
                }
                remaining -= component.controlPoints.size();
            }
            return;
        }

        if (!shape.nurbs.controlPoints.isEmpty()) {
            if (controlPointIndex >= shape.nurbs.controlPoints.size()) {
                return;
            }

            const QPointF newControlPoint =
                shape.nurbs.controlPoints[controlPointIndex] + delta;
            if (!setClosedNurbsSeamControlPoint(&shape,
                                               controlPointIndex,
                                               newControlPoint)) {
                shape.nurbs.controlPoints[controlPointIndex] = newControlPoint;
            }

            // These curve types keep their source points in the same order as
            // their NURBS CVs. Keep both representations synchronized. Arc
            // and circle construction points intentionally remain unchanged;
            // their stored NURBS is the geometry being edited.
            if ((shape.geometryType == GeometryType::Line || shape.geometryType == GeometryType::Bezier ||
                 shape.geometryType == GeometryType::Nurbs) &&
                controlPointIndex < shape.points.size()) {
                shape.points[controlPointIndex] += delta;
            }
            if (shape.geometryType == GeometryType::Rectangle) {
                shape.points = rectangleVertices(shape);
            }
            return;
        }

        if (controlPointIndex < shape.points.size()) {
            shape.points[controlPointIndex] += delta;
        }
    }

    void translateShape(ObjectId objectId, const QPointF &delta)
    {
        const int index = objectIndex(objectId);
        if (index < 0 || index >= shapes_.size()) {
            return;
        }

        Shape &shape = shapes_[index];
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
            return;
        }
        const QPointF localDelta = worldPointToWorkPlaneFrame(displacedOrigin, frame);
        translateShapeGeometry(shape, localDelta);
        const Point3D planarEnd = workPlaneFramePointToWorld(localDelta, frame);
        frame.origin.x += displacedOrigin.x - planarEnd.x;
        frame.origin.y += displacedOrigin.y - planarEnd.y;
        frame.origin.z += displacedOrigin.z - planarEnd.z;
        shape.workPlaneFrame = frame;
    }

    void translateShapeGeometry(Shape &shape, const QPointF &delta) const
    {
        if (shape.geometryType == GeometryType::PolyCurve &&
            shape.componentWorkPlaneFrames.size() == shape.components.size()) {
            const WorkPlaneFrame frame = shapeWorkPlaneFrame(shape);
            const Point3D worldDelta{
                frame.xAxis.x * delta.x() + frame.yAxis.x * delta.y(),
                frame.xAxis.y * delta.x() + frame.yAxis.y * delta.y(),
                frame.xAxis.z * delta.x() + frame.yAxis.z * delta.y()};
            shape.workPlaneFrame.origin.x += worldDelta.x;
            shape.workPlaneFrame.origin.y += worldDelta.y;
            shape.workPlaneFrame.origin.z += worldDelta.z;
            for (WorkPlaneFrame &componentFrame :
                 shape.componentWorkPlaneFrames) {
                componentFrame.origin.x += worldDelta.x;
                componentFrame.origin.y += worldDelta.y;
                componentFrame.origin.z += worldDelta.z;
            }
            return;
        }
        for (QPointF &point : shape.points) {
            point += delta;
        }
        for (QPointF &point : shape.nurbs.controlPoints) {
            point += delta;
        }
        for (Shape::NurbsCurve2D &component : shape.components) {
            for (QPointF &point : component.controlPoints) {
                point += delta;
            }
        }
    }

    void translateShapes(const QVector<ObjectId> &objectIds, const QPointF &delta)
    {
        for (const ObjectId objectId : objectIds) {
            translateShape(objectId, delta);
        }
    }

    void applyObjectDragSnap(const QVector<ObjectId> &objectIds,
                             const DragSnapResult &snap)
    {
        if (!snap.hasWorldTranslation) {
            translateShapes(objectIds, snap.translation);
            return;
        }
        for (const ObjectId objectId : objectIds) {
            const int index = objectIndex(objectId);
            if (index < 0 || index >= shapes_.size()) continue;
            WorkPlaneFrame frame = shapeWorkPlaneFrame(shapes_[index]);
            frame.origin.x += snap.worldTranslation.x;
            frame.origin.y += snap.worldTranslation.y;
            frame.origin.z += snap.worldTranslation.z;
            shapes_[index].workPlaneFrame = frame;
            for (WorkPlaneFrame &componentFrame :
                 shapes_[index].componentWorkPlaneFrames) {
                componentFrame.origin.x += snap.worldTranslation.x;
                componentFrame.origin.y += snap.worldTranslation.y;
                componentFrame.origin.z += snap.worldTranslation.z;
            }
        }
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
        case DragAxisLock::None:
            return delta;
        }

        return delta;
    }

    void updateGrabPosition(const QVector<ObjectId> &dragIndices)
    {
        if (!grabActive_ || grabPickingBasePoint_) {
            return;
        }

        if (dragSnapLocked_) {
            const QPointF cursorScreen = worldToScreen(rawCursorWorld_);
            const QPointF snapScreen = worldToScreen(dragSnapCursorWorld_);
            const qreal cursorDistanceFromSnap =
                std::hypot(cursorScreen.x() - snapScreen.x(),
                           cursorScreen.y() - snapScreen.y());
            if (cursorDistanceFromSnap <= kDragSnapBreakawayPixels) {
                return;
            }
            dragSnapLocked_ = false;
            currentDragSnap_ = DragSnapResult{};
        }

        // Rebuild the preview from the saved document so axis changes and snap
        // changes never accumulate an additional incremental delta.
        document_.restoreSnapshot(grabStartSnapshot_);
        QPointF totalDelta;
        QPointF freeDestination;
        if (grabHasBasePoint_) {
            const QPointF destinationCursor = rawCursorWorld_ - grabCursorOffset_;
            currentSnap_ = findGrabDestinationSnap(destinationCursor);
            const QPointF destination = currentSnap_.isValid()
                                            ? currentSnap_.point
                                            : destinationCursor;
            cursorWorld_ = destination;
            lastWorldPosition_ = destination;
            freeDestination = destinationCursor;
            totalDelta = destination - grabBasePoint_;
        } else {
            currentSnap_ = SnapResult{};
            totalDelta = rawCursorWorld_ - grabStartWorld_;
        }
        QPointF delta = constrainDragDelta(totalDelta);
        if (grabHasBasePoint_ && currentSnap_.isValid() &&
            (!qFuzzyIsNull(delta.x() - totalDelta.x()) ||
             !qFuzzyIsNull(delta.y() - totalDelta.y()))) {
            // Do not show a snap marker for a destination the axis lock makes
            // impossible to reach. The cursor still projects onto the locked
            // axis, as it does for an ordinary constrained Grab.
            currentSnap_ = SnapResult{};
            cursorWorld_ = freeDestination;
            lastWorldPosition_ = freeDestination;
            totalDelta = freeDestination - grabBasePoint_;
            delta = constrainDragDelta(totalDelta);
        }
        if (!qFuzzyIsNull(delta.x()) || !qFuzzyIsNull(delta.y())) {
            translateShapes(dragIndices, delta);
            grabMoved_ = true;
        } else {
            grabMoved_ = false;
        }
        currentDragSnap_ = DragSnapResult{};
        if (grabHasBasePoint_ && dragAxisLock_ == DragAxisLock::None) {
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
                grabMoved_ = true;
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
                grabMoved_ = true;
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
        lastDragWorld_ = rawCursorWorld_;
        DebugLog::instance().write(
            QStringLiteral("grab move delta=%1 cursorWorld=%2 basePoint=%3 snap=%4 axisLock=%5")
                .arg(pointText(delta))
                .arg(pointText(rawCursorWorld_))
                .arg(grabHasBasePoint_ ? pointText(grabBasePoint_) : QStringLiteral("none"))
                .arg(snapTypeName(currentSnap_.type))
                .arg(dragAxisLockName(dragAxisLock_)));
    }

    QPointF screenToWorld(const QPointF &screen) const
    {
        return viewportTransform_.screenToWorld(screen, size());
    }

    void resetArcInputState()
    {
        arcReferenceFrame_ = {};
        arcInputFrame_ = {};
        arcAxisBaseFrame_ = {};
        arcReferenceNormal_ = {};
        arcFirstPointWorld_ = {};
        arcSecondPointWorld_ = {};
        arcResolvedChordPointWorld_ = {};
        arcReferenceFrameValid_ = false;
        arcInputFrameValid_ = false;
        arcAxisBaseFrameValid_ = false;
        arcChordWorldPointsValid_ = false;
        arcResolvedChordPointValid_ = false;
        arcAxisConstraintKey_ = 0;
        arcPlaneNormalLockKey_ = 0;
        arcVerticalOverrideAxis_ = 0;
        arcTwoPointPerpendicularNormal_ = {};
        arcTwoPointPerpendicularNormalValid_ = false;
        arcWasVertical_ = false;
        arcPreviewStartAngle_ = 0.0;
        arcAngleSnapEnabled_ = true;
        arcPerpendicularPlaneActive_ = false;
        arcPlaneLocked_ = false;
        arcLockedFrame_ = {};
        arcLockedFrameValid_ = false;
        arcTextInputMode_ = ArcTextInputMode::None;
        arcTextInput_.clear();
    }

    void captureArcReferenceForFirstPoint(const QPointF &localPoint)
    {
        arcReferenceFrame_ = viewportTransform_.workPlaneFrame();
        arcInputFrame_ = arcReferenceFrame_;
        arcReferenceFrameValid_ = isValidWorkPlaneFrame(arcReferenceFrame_);
        arcInputFrameValid_ = arcReferenceFrameValid_;
        if (!arcReferenceFrameValid_) {
            return;
        }

        arcReferenceNormal_ = arcReferenceFrame_.normal;
        arcTwoPointPerpendicularNormal_ = {};
        arcTwoPointPerpendicularNormalValid_ = false;
        arcFirstPointWorld_ = workPlaneFramePointToWorld(localPoint,
                                                          arcReferenceFrame_);
        arcChordWorldPointsValid_ = false;
        arcResolvedChordPointValid_ = false;
        arcWasVertical_ = false;
    }

    void beginArcTextInput(ArcTextInputMode mode, const QString &initialText = {})
    {
        if (pendingPoints_.isEmpty()) {
            return;
        }
        arcTextInputMode_ = mode;
        arcTextInput_ = initialText;
        update();
    }

    void applyArcTextInput()
    {
        const ArcTextInputMode inputMode = arcTextInputMode_;
        const QString input = arcTextInput_.trimmed();
        arcTextInputMode_ = ArcTextInputMode::None;
        arcTextInput_.clear();

        if (activeTool_ == Tool::Arc && arcMode_ == ArcMode::TwoPoint) {
            qreal distance = 0.0;
            if (parseDocumentLengthInput(input,
                                         document_.settings().lengthUnit,
                                         &distance)) {
                distance = std::abs(distance);
                if (inputMode == ArcTextInputMode::ChordLength &&
                    pendingPoints_.size() == 1) {
                    setTwoPointArcChordLength(distance);
                } else if (inputMode == ArcTextInputMode::Sagitta &&
                           pendingPoints_.size() >= 2) {
                    setTwoPointArcSagitta(distance);
                }
            }
            update();
            emitCoordinateUpdate();
            return;
        }

        if (inputMode == ArcTextInputMode::Radius) {
            qreal radius = 0.0;
            if (parseDocumentLengthInput(input,
                                        document_.settings().lengthUnit,
                                        &radius) &&
                !pendingPoints_.isEmpty()) {
                radius = std::max<qreal>(0.1, std::abs(radius));
                const QPointF center = pendingPoints_.first();
                QPointF direction = pendingPoints_.size() >= 2
                                        ? pendingPoints_[1] - center
                                        : cursorWorld_ - center;
                qreal directionLength = std::hypot(direction.x(), direction.y());
                if (directionLength <= 1.0e-9) {
                    direction = QPointF(1.0, 0.0);
                    directionLength = 1.0;
                }
                direction /= directionLength;
                qreal angle = std::atan2(direction.y(), direction.x());
                if (pendingPoints_.size() == 1 && arcAngleSnapEnabled_ &&
                    !currentSnap_.isValid()) {
                    angle = snapOnePointArcAngle(angle);
                }
                arcPreviewStartAngle_ = angle;
                const QPointF start = center +
                    QPointF(radius * std::cos(angle), radius * std::sin(angle));
                if (pendingPoints_.size() == 1) {
                    pendingPoints_.append(start);
                    initializeArcPreviewTracking();
                } else {
                    pendingPoints_[1] = start;
                    arcPreviewStartAngle_ = angle;
                    arcPreviewPreviousAngle_ = angle + arcPreviewSweepAngle_;
                    arcPreviewInitialized_ = true;
                    cursorWorld_ = center + QPointF(
                        radius * std::cos(arcPreviewPreviousAngle_),
                        radius * std::sin(arcPreviewPreviousAngle_));
                    lastWorldPosition_ = cursorWorld_;
                }
            }
        } else if (inputMode == ArcTextInputMode::Angle &&
                   pendingPoints_.size() >= 2) {
            QString angleInput = input;
            angleInput.remove(QStringLiteral("°"));
            bool valid = false;
            const qreal degrees = angleInput.toDouble(&valid);
            if (valid && std::isfinite(degrees)) {
                constexpr qreal pi = 3.14159265358979323846;
                // Arc input uses the drafting convention: clockwise is positive.
                // The stored workplane sweep follows the usual right-handed
                // mathematical convention, where clockwise is negative.
                arcPreviewSweepAngle_ = -degrees * pi / 180.0;
                const QPointF radiusVector = pendingPoints_[1] - pendingPoints_[0];
                arcPreviewStartAngle_ = std::atan2(radiusVector.y(),
                                                   radiusVector.x());
                arcPreviewPreviousAngle_ = arcPreviewStartAngle_ +
                                           arcPreviewSweepAngle_;
                arcPreviewInitialized_ = true;
                const qreal radius = std::hypot(radiusVector.x(),
                                                radiusVector.y());
                cursorWorld_ = pendingPoints_[0] + QPointF(
                    radius * std::cos(arcPreviewPreviousAngle_),
                    radius * std::sin(arcPreviewPreviousAngle_));
                lastWorldPosition_ = cursorWorld_;
                cursorValid_ = true;
                currentSnap_ = SnapResult{};
                arcAngleValueLocked_ = true;
            }
        }
        update();
        emitCoordinateUpdate();
    }

    bool handleOnePointArcKey(QKeyEvent *event)
    {
        if (event == nullptr || activeTool_ != Tool::Arc ||
            arcMode_ != ArcMode::OnePoint) {
            return false;
        }

        if (arcTextInputMode_ != ArcTextInputMode::None) {
            if (event->key() == Qt::Key_Escape) {
                arcTextInputMode_ = ArcTextInputMode::None;
                arcTextInput_.clear();
                pendingPoints_.clear();
                setTool(Tool::Select);
                if (commandFinished_) {
                    commandFinished_(Tool::Select);
                }
                update();
                event->accept();
                return true;
            }
            if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
                applyArcTextInput();
                event->accept();
                return true;
            }
            if (event->key() == Qt::Key_Backspace) {
                arcTextInput_.chop(1);
                update();
                event->accept();
                return true;
            }

            const QString typedText = event->text();
            bool accepted = !typedText.isEmpty();
            for (const QChar character : typedText) {
                const bool numeric = character.isDigit() ||
                                     character == QLatin1Char('.') ||
                                     character == QLatin1Char(',') ||
                                     character == QLatin1Char('-') ||
                                     character == QLatin1Char('+') ||
                                     character == QLatin1Char('/') ||
                                     character == QLatin1Char(' ') ||
                                     character == QLatin1Char('\'') ||
                                     character == QLatin1Char('"') ||
                                     character.isLetter();
                if (!numeric) {
                    accepted = false;
                    break;
                }
            }
            if (accepted) {
                arcTextInput_.append(typedText == QStringLiteral(",")
                                         ? QStringLiteral(".")
                                         : typedText);
                update();
                event->accept();
                return true;
            }
            return false;
        }

        if (event->isAutoRepeat() || event->modifiers() != Qt::NoModifier) {
            return false;
        }
        switch (event->key()) {
        case Qt::Key_C:
            arcAngleSnapEnabled_ = !arcAngleSnapEnabled_;
            update();
            event->accept();
            return true;
        case Qt::Key_L:
            arcPlaneLocked_ = !arcPlaneLocked_;
            if (arcPlaneLocked_ && pendingPoints_.isEmpty()) {
                arcLockedFrame_ = viewportTransform_.workPlaneFrame();
                arcLockedFrameValid_ = isValidWorkPlaneFrame(arcLockedFrame_);
            } else if (!arcPlaneLocked_) {
                arcLockedFrame_ = {};
                arcLockedFrameValid_ = false;
                if (pendingPoints_.isEmpty()) {
                    updateDrawingWorkPlaneFromHover(currentArcScreenPosition());
                }
            }
            update();
            event->accept();
            return true;
        case Qt::Key_P:
            toggleOnePointArcPerpendicularPlane();
            event->accept();
            return true;
        case Qt::Key_R:
            beginArcTextInput(ArcTextInputMode::Radius);
            event->accept();
            return true;
        case Qt::Key_A:
            if (pendingPoints_.size() >= 2) {
                beginArcTextInput(ArcTextInputMode::Angle);
                event->accept();
                return true;
            }
            return false;
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Space:
            if (pendingPoints_.size() >= 2) {
                finishOnePointArcAt(cursorWorld_);
                event->accept();
                return true;
            }
            return false;
        default:
            break;
        }

        if (!pendingPoints_.isEmpty()) {
            const QString typedText = event->text();
            if (typedText.size() == 1 &&
                (typedText.front().isDigit() || typedText.front() == QLatin1Char('.') ||
                 typedText.front() == QLatin1Char('-') ||
                 typedText.front() == QLatin1Char('+'))) {
                beginArcTextInput(pendingPoints_.size() >= 2
                                       ? ArcTextInputMode::Angle
                                       : ArcTextInputMode::Radius,
                                  typedText);
                event->accept();
                return true;
            }
        }
        return false;
    }

    bool handleTwoPointArcKey(QKeyEvent *event)
    {
        if (event == nullptr || activeTool_ != Tool::Arc ||
            arcMode_ != ArcMode::TwoPoint) {
            return false;
        }

        if (arcTextInputMode_ != ArcTextInputMode::None) {
            if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
                applyArcTextInput();
                event->accept();
                return true;
            }
            if (event->key() == Qt::Key_Backspace) {
                arcTextInput_.chop(1);
                update();
                event->accept();
                return true;
            }

            const QString typedText = event->text();
            bool accepted = !typedText.isEmpty();
            for (const QChar character : typedText) {
                const bool numeric = character.isDigit() ||
                                     character == QLatin1Char('.') ||
                                     character == QLatin1Char(',') ||
                                     character == QLatin1Char('-') ||
                                     character == QLatin1Char('+') ||
                                     character == QLatin1Char('/') ||
                                     character == QLatin1Char(' ') ||
                                     character == QLatin1Char('\'') ||
                                     character == QLatin1Char('"') ||
                                     character.isLetter();
                if (!numeric) {
                    accepted = false;
                    break;
                }
            }
            if (accepted) {
                arcTextInput_.append(typedText == QStringLiteral(",")
                                         ? QStringLiteral(".")
                                         : typedText);
                update();
                event->accept();
                return true;
            }
            return false;
        }

        if (event->isAutoRepeat() || event->modifiers() != Qt::NoModifier) {
            return false;
        }
        switch (event->key()) {
        case Qt::Key_D:
            if (pendingPoints_.size() == 1) {
                beginArcTextInput(ArcTextInputMode::ChordLength);
                event->accept();
                return true;
            }
            return false;
        case Qt::Key_H:
            if (pendingPoints_.size() >= 2) {
                beginArcTextInput(ArcTextInputMode::Sagitta);
                event->accept();
                return true;
            }
            return false;
        case Qt::Key_P:
            toggleTwoPointArcPerpendicularPlane();
            event->accept();
            return true;
        case Qt::Key_L:
            toggleArcPlaneLock();
            event->accept();
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Space:
            if (pendingPoints_.size() >= 2) {
                finishTwoPointArcAt(cursorWorld_);
                event->accept();
                return true;
            }
            return false;
        default:
            break;
        }

        if (!pendingPoints_.isEmpty()) {
            const QString typedText = event->text();
            if (typedText.size() == 1 &&
                (typedText.front().isDigit() ||
                 typedText.front() == QLatin1Char('.') ||
                 typedText.front() == QLatin1Char('-') ||
                 typedText.front() == QLatin1Char('+'))) {
                beginArcTextInput(pendingPoints_.size() >= 2
                                      ? ArcTextInputMode::Sagitta
                                      : ArcTextInputMode::ChordLength,
                                  typedText);
                event->accept();
                return true;
            }
        }
        return false;
    }

    bool handleThreePointArcKey(QKeyEvent *event)
    {
        if (event == nullptr || activeTool_ != Tool::Arc ||
            arcMode_ != ArcMode::ThreePoint || event->isAutoRepeat() ||
            event->modifiers() != Qt::NoModifier) {
            return false;
        }

        switch (event->key()) {
        case Qt::Key_P:
            toggleTwoPointArcPerpendicularPlane();
            event->accept();
            return true;
        case Qt::Key_L:
            toggleArcPlaneLock();
            event->accept();
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Space:
            if (pendingPoints_.size() >= 2) {
                finishThreePointArcAt(cursorWorld_);
                event->accept();
                return true;
            }
            return false;
        default:
            return false;
        }
    }

    void toggleOnePointArcPerpendicularPlane()
    {
        if (pendingPoints_.isEmpty() || !arcReferenceFrameValid_ ||
            !arcInputFrameValid_) {
            return;
        }

        const WorkPlaneFrame oldFrame = viewportTransform_.workPlaneFrame();
        const Point3D centerWorld = arcFirstPointWorld_;
        Point3D startWorld = centerWorld;
        if (pendingPoints_.size() >= 2) {
            startWorld = workPlaneFramePointToWorld(pendingPoints_[1], oldFrame);
        }

        WorkPlaneFrame nextFrame;
        if (arcPerpendicularPlaneActive_) {
            nextFrame = arcReferenceFrame_;
        } else {
            const QPointF bridgeLocal = pendingPoints_.size() >= 2
                                            ? pendingPoints_[1] - pendingPoints_[0]
                                            : cursorWorld_ - pendingPoints_[0];
            const Point3D bridgeWorld = arcVectorSubtract(
                workPlaneFramePointToWorld(pendingPoints_[0] + bridgeLocal,
                                           oldFrame),
                centerWorld);
            const qreal bridgeLength = arcVectorLength(bridgeWorld);
            if (bridgeLength <= 1.0e-9) {
                return;
            }
            const Point3D bridgeDirection =
                arcVectorScale(bridgeWorld, 1.0 / bridgeLength);
            const Point3D normal = arcVectorCross(bridgeDirection,
                                                  arcReferenceNormal_);
            const qreal normalLength = arcVectorLength(normal);
            if (normalLength <= 1.0e-9) {
                return;
            }
            const Point3D perpendicularNormal =
                arcVectorScale(normal, 1.0 / normalLength);
            const Point3D xAxis = arcVectorCross(arcReferenceNormal_,
                                                 perpendicularNormal);
            const qreal xLength = arcVectorLength(xAxis);
            if (xLength <= 1.0e-9) {
                return;
            }
            nextFrame.origin = centerWorld;
            nextFrame.xAxis = arcVectorScale(xAxis, 1.0 / xLength);
            nextFrame.yAxis = arcReferenceNormal_;
            nextFrame.normal = perpendicularNormal;
            nextFrame.valid = true;
            if (!isValidWorkPlaneFrame(nextFrame)) {
                return;
            }
        }

        if (!isValidWorkPlaneFrame(nextFrame)) {
            return;
        }
        const qreal previousSweep = arcPreviewSweepAngle_;
        viewportTransform_.setWorkPlaneFrame(nextFrame);
        arcInputFrame_ = nextFrame;
        arcInputFrameValid_ = true;
        pendingPoints_[0] = worldPointToWorkPlaneFrame(centerWorld, nextFrame);
        if (pendingPoints_.size() >= 2) {
            QPointF startLocal = worldPointToWorkPlaneFrame(startWorld, nextFrame);
            const qreal startRadius = std::hypot(startLocal.x() - pendingPoints_[0].x(),
                                                 startLocal.y() - pendingPoints_[0].y());
            if (startRadius <= 1.0e-9) {
                const QPointF oldRadius = pendingPoints_[1] - pendingPoints_[0];
                startLocal = pendingPoints_[0] + QPointF(
                    std::hypot(oldRadius.x(), oldRadius.y()), 0.0);
            }
            pendingPoints_[1] = startLocal;
        }
        arcPerpendicularPlaneActive_ = !arcPerpendicularPlaneActive_;
        arcPreviewSweepAngle_ = previousSweep;

        const QPointF screenPosition = currentArcScreenPosition();
        QPointF updatedCursor;
        if (viewportTransform_.screenToWorkPlane(screenPosition,
                                                 size(),
                                                 nextFrame,
                                                 &updatedCursor)) {
            rawCursorWorld_ = updatedCursor;
            cursorWorld_ = constrainLinePoint(updatedCursor, false, &screenPosition);
            lastWorldPosition_ = cursorWorld_;
        }

        if (pendingPoints_.size() >= 2) {
            const QPointF radiusVector = pendingPoints_[1] - pendingPoints_[0];
            arcPreviewStartAngle_ = std::atan2(radiusVector.y(), radiusVector.x());
            const QPointF cursorVector = cursorWorld_ - pendingPoints_[0];
            if (std::hypot(cursorVector.x(), cursorVector.y()) > 1.0e-9) {
                arcPreviewPreviousAngle_ = std::atan2(cursorVector.y(), cursorVector.x());
                arcPreviewInitialized_ = true;
            } else {
                arcPreviewPreviousAngle_ = arcPreviewStartAngle_ + previousSweep;
                arcPreviewInitialized_ = true;
            }
        }
        update();
        emitCoordinateUpdate();
    }

    void finishOnePointArcAt(const QPointF &cursorPoint)
    {
        if (activeTool_ != Tool::Arc || arcMode_ != ArcMode::OnePoint ||
            pendingPoints_.size() < 2 || !isValidWorkPlaneFrame(
                viewportTransform_.workPlaneFrame())) {
            return;
        }
        updateArcPreviewTracking(cursorPoint);
        constexpr qreal twoPi = 6.28318530717958647692;
        const qreal sweep = std::clamp(arcPreviewSweepAngle_,
                                       -twoPi + 1.0e-6,
                                       twoPi - 1.0e-6);
        if (std::abs(sweep) <= 1.0e-12) {
            return;
        }
        QVector<QPointF> points = pendingPoints_;
        const QPointF radiusVector = points[1] - points[0];
        const qreal radius = std::hypot(radiusVector.x(), radiusVector.y());
        if (radius <= 1.0e-9) {
            return;
        }
        const qreal startAngle = std::atan2(radiusVector.y(), radiusVector.x());
        points.append(points[0] + QPointF(radius * std::cos(startAngle + sweep),
                                          radius * std::sin(startAngle + sweep)));
        Shape completedShape;
        completedShape.geometryType = GeometryType::Arc;
        completedShape.points = points;
        completedShape.arcMode = ArcMode::OnePoint;
        completedShape.arcSweep = sweep;
        completedShape.workPlane = viewportTransform_.workPlane();
        completedShape.workPlaneOffset = viewportTransform_.workPlaneOffset();
        completedShape.workPlaneFrame = viewportTransform_.workPlaneFrame();
        completedShape.nurbs = makeArcNurbsCurve(completedShape);
        if (!isValidNurbsCurve(completedShape.nurbs)) {
            return;
        }
        recordGeometryChange();
        shapes_.append(completedShape);
        DebugLog::instance().write(
            QStringLiteral("one-point arc committed by finish action radius=%1 sweep=%2")
                .arg(radius, 0, 'f', 4)
                .arg(sweep, 0, 'f', 6));
        setTool(Tool::Select);
        if (commandFinished_) {
            commandFinished_(Tool::Select);
        }
        update();
        emitCoordinateUpdate();
    }

    void finishTwoPointArcAt(const QPointF &cursorPoint)
    {
        if (activeTool_ != Tool::Arc || arcMode_ != ArcMode::TwoPoint ||
            pendingPoints_.size() < 2 ||
            !isValidWorkPlaneFrame(viewportTransform_.workPlaneFrame())) {
            return;
        }

        QVector<QPointF> points{pendingPoints_[0], pendingPoints_[1]};
        points.append(constrainTwoPointArcThroughPoint(
            cursorPoint, currentSnap_.isValid(), false));
        Shape completedShape;
        if (!makeToolShape(Tool::Arc, points, ArcMode::TwoPoint, 0.0,
                           &completedShape)) {
            return;
        }

        recordGeometryChange();
        shapes_.append(completedShape);
        DebugLog::instance().write(
            QStringLiteral("two-point arc committed chord=%1 sagitta=%2")
                .arg(std::hypot(points[1].x() - points[0].x(),
                               points[1].y() - points[0].y()),
                     0, 'f', 4)
                .arg(std::hypot(points[2].x() -
                                    (points[0].x() + points[1].x()) * 0.5,
                                points[2].y() -
                                    (points[0].y() + points[1].y()) * 0.5),
                     0, 'f', 4));
        setTool(Tool::Select);
        if (commandFinished_) {
            commandFinished_(Tool::Select);
        }
        update();
        emitCoordinateUpdate();
    }

    void finishThreePointArcAt(const QPointF &cursorPoint)
    {
        if (activeTool_ != Tool::Arc || arcMode_ != ArcMode::ThreePoint ||
            pendingPoints_.size() < 2 ||
            !isValidWorkPlaneFrame(viewportTransform_.workPlaneFrame())) {
            return;
        }

        const QVector<QPointF> points{pendingPoints_[0], pendingPoints_[1],
                                      cursorPoint};
        Shape completedShape;
        if (!makeToolShape(Tool::Arc, points, ArcMode::ThreePoint, 0.0,
                           &completedShape)) {
            return;
        }

        recordGeometryChange();
        shapes_.append(completedShape);
        CircularArc2D arc;
        const bool arcDefined = makeCircularArcThroughPoint(
            points[0], points[1], points[2], &arc);
        DebugLog::instance().write(
            QStringLiteral("three-point arc committed radius=%1")
                .arg(arcDefined ? arc.radius : 0.0, 0, 'f', 4));
        setTool(Tool::Select);
        if (commandFinished_) {
            commandFinished_(Tool::Select);
        }
        update();
        emitCoordinateUpdate();
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

    Point3D arcVerticalPlaneNormal() const
    {
        if (!arcReferenceFrameValid_) {
            return {};
        }

        // Match the add-on's orthonormal_basis_from_normal(): the manual X/Y
        // orientations are based on the reference normal and world axes, not
        // on whichever X/Y rotation happened to be in the captured frame.
        Point3D referenceNormal = arcReferenceNormal_;
        const qreal normalLength = arcVectorLength(referenceNormal);
        if (normalLength <= 1.0e-9) {
            return {};
        }
        referenceNormal = arcVectorScale(referenceNormal, 1.0 / normalLength);
        const Point3D worldX{1.0, 0.0, 0.0};
        const Point3D worldY{0.0, 1.0, 0.0};
        const Point3D referenceAxis =
            std::abs(arcVectorDot(referenceNormal, worldX)) < 0.99
                ? worldX
                : worldY;
        Point3D basisY = arcVectorCross(referenceNormal, referenceAxis);
        const qreal basisYLength = arcVectorLength(basisY);
        if (basisYLength <= 1.0e-9) {
            return {};
        }
        basisY = arcVectorScale(basisY, 1.0 / basisYLength);
        Point3D basisX = arcVectorCross(basisY, referenceNormal);
        const qreal basisXLength = arcVectorLength(basisX);
        if (basisXLength <= 1.0e-9) {
            return {};
        }
        basisX = arcVectorScale(basisX, 1.0 / basisXLength);

        if (arcVerticalOverrideAxis_ == Qt::Key_X) {
            return basisX;
        }
        if (arcVerticalOverrideAxis_ == Qt::Key_Y) {
            return basisY;
        }

        const Point3D viewForward = viewportTransform_.viewDirection();
        return std::abs(arcVectorDot(viewForward, basisX)) >
                       std::abs(arcVectorDot(viewForward, basisY))
                   ? basisX
                   : basisY;
    }

    void restoreArcChordReferencePlaneForEndpointPick()
    {
        if (!arcReferenceFrameValid_) {
            return;
        }
        viewportTransform_.setWorkPlaneFrame(arcReferenceFrame_);
        arcInputFrame_ = arcReferenceFrame_;
        arcInputFrameValid_ = true;
        if (!pendingPoints_.isEmpty()) {
            pendingPoints_[0] = worldPointToWorkPlaneFrame(
                arcFirstPointWorld_, arcReferenceFrame_);
        }
    }

    void updateArcTwoPointWorkPlaneForView(
        const Point3D *previewEndpointWorld = nullptr)
    {
        if (activeTool_ != Tool::Arc ||
            (arcMode_ != ArcMode::TwoPoint &&
             arcMode_ != ArcMode::ThreePoint) ||
            !arcReferenceFrameValid_) {
            return;
        }

        const bool haveEndpoint = previewEndpointWorld != nullptr ||
                                  arcChordWorldPointsValid_;
        if (!haveEndpoint) {
            return;
        }
        // In the add-on, P changes the chord-picking plane immediately.
        // Keep that chosen plane fixed while the second endpoint is moving;
        // only build the final arc frame after the chord has been committed.
        if (arcMode_ == ArcMode::TwoPoint &&
            pendingPoints_.size() == 1 && !arcChordWorldPointsValid_) {
            return;
        }
        const Point3D endpoint = previewEndpointWorld != nullptr
                                     ? *previewEndpointWorld
                                     : arcSecondPointWorld_;
        const Point3D chord = arcVectorSubtract(endpoint,
                                                arcFirstPointWorld_);
        const qreal chordLength = arcVectorLength(chord);
        if (!std::isfinite(chordLength) || chordLength <= 1.0e-9) {
            return;
        }

        const Point3D chordDirection = arcVectorScale(chord, 1.0 / chordLength);
        const qreal verticalThreshold = arcWasVertical_ ? 0.98 : 0.995;
        const bool isVertical = std::abs(arcVectorDot(chordDirection,
                                                      arcReferenceNormal_)) >
                                verticalThreshold;
        arcWasVertical_ = isVertical;

        WorkPlaneFrame frame = arcReferenceFrame_;
        if (arcMode_ == ArcMode::TwoPoint) {
            Point3D planeNormal;
            if (isVertical) {
                planeNormal = arcVerticalPlaneNormal();
            } else if (arcPerpendicularPlaneActive_) {
                planeNormal = arcTwoPointPerpendicularNormalValid_
                                  ? arcTwoPointPerpendicularNormal_
                                  : arcVectorCross(chordDirection,
                                                   arcReferenceNormal_);
                const Point3D viewForward = viewportTransform_.viewDirection();
                if (arcVectorDot(planeNormal, viewForward) > 0.0) {
                    planeNormal = arcVectorScale(planeNormal, -1.0);
                }
            } else {
                planeNormal = arcReferenceNormal_;
            }

            // A snapped endpoint can sit slightly outside the selected plane.
            // Make the arc plane contain the final chord while keeping its
            // normal as close as possible to the add-on's selected normal.
            planeNormal = arcVectorSubtract(
                planeNormal,
                arcVectorScale(chordDirection,
                               arcVectorDot(planeNormal, chordDirection)));
            const qreal planeNormalLength = arcVectorLength(planeNormal);
            if (planeNormalLength > 1.0e-9) {
                planeNormal = arcVectorScale(planeNormal,
                                             1.0 / planeNormalLength);
                Point3D arcY = arcVectorCross(planeNormal, chordDirection);
                const qreal arcYLength = arcVectorLength(arcY);
                if (arcYLength > 1.0e-9) {
                    arcY = arcVectorScale(arcY, 1.0 / arcYLength);
                    frame.origin = arcVectorScale(
                        arcVectorAdd(arcFirstPointWorld_, endpoint), 0.5);
                    frame.xAxis = chordDirection;
                    frame.yAxis = arcY;
                    frame.normal = arcVectorCross(chordDirection, arcY);
                    const qreal frameNormalLength = arcVectorLength(frame.normal);
                    if (frameNormalLength > 1.0e-9) {
                        frame.normal = arcVectorScale(frame.normal,
                                                      1.0 / frameNormalLength);
                        frame.valid = true;
                    }
                }
            }
        } else if (isVertical) {
            const Point3D midpoint = arcVectorScale(
                arcVectorAdd(arcFirstPointWorld_, endpoint), 0.5);
            frame = makeWorkPlaneFrameFromNormal(midpoint,
                                                 arcVerticalPlaneNormal());
        } else if (arcPerpendicularPlaneActive_) {
            const Point3D midpoint = arcVectorScale(
                arcVectorAdd(arcFirstPointWorld_, endpoint), 0.5);
            Point3D normal = arcVectorCross(chordDirection,
                                            arcReferenceNormal_);
            const qreal normalLength = arcVectorLength(normal);
            if (normalLength > 1.0e-9) {
                normal = arcVectorScale(normal, 1.0 / normalLength);
                frame = makeWorkPlaneFrameFromNormal(midpoint, normal);
            }
        }
        if (!isValidWorkPlaneFrame(frame)) {
            return;
        }

        viewportTransform_.setWorkPlaneFrame(frame);
        arcInputFrame_ = frame;
        arcInputFrameValid_ = true;
        if (!pendingPoints_.isEmpty()) {
            pendingPoints_[0] = worldPointToWorkPlaneFrame(arcFirstPointWorld_,
                                                            frame);
        }
        if (pendingPoints_.size() >= 2 && arcChordWorldPointsValid_) {
            pendingPoints_[1] = worldPointToWorkPlaneFrame(arcSecondPointWorld_,
                                                            frame);
        }
    }

    void setTwoPointArcChordLength(qreal chordLength)
    {
        if (pendingPoints_.size() != 1 || !arcReferenceFrameValid_ ||
            !std::isfinite(chordLength) || chordLength <= 1.0e-9) {
            return;
        }

        const WorkPlaneFrame frame = viewportTransform_.workPlaneFrame();
        const Point3D cursorWorld = workPlaneFramePointToWorld(cursorWorld_, frame);
        Point3D direction = arcVectorSubtract(cursorWorld, arcFirstPointWorld_);
        if (arcAxisConstraintKey_ != 0) {
            const Point3D axis = arcAxisDirection(arcAxisConstraintKey_);
            const qreal directionLength = arcVectorLength(direction);
            const qreal sign = directionLength > 1.0e-9 &&
                                       arcVectorDot(direction, axis) < 0.0
                                   ? -1.0
                                   : 1.0;
            direction = arcVectorScale(axis, sign);
        } else {
            const qreal directionLength = arcVectorLength(direction);
            if (directionLength <= 1.0e-9) {
                direction = arcReferenceFrame_.xAxis;
            } else {
                direction = arcVectorScale(direction, 1.0 / directionLength);
            }
        }

        arcSecondPointWorld_ = arcVectorAdd(
            arcFirstPointWorld_, arcVectorScale(direction, chordLength));
        arcChordWorldPointsValid_ = true;
        updateArcTwoPointWorkPlaneForView(&arcSecondPointWorld_);
        pendingPoints_[0] = worldPointToWorkPlaneFrame(
            arcFirstPointWorld_, viewportTransform_.workPlaneFrame());
        pendingPoints_.append(worldPointToWorkPlaneFrame(
            arcSecondPointWorld_, viewportTransform_.workPlaneFrame()));
        arcAxisConstraintKey_ = 0;
        arcResolvedChordPointValid_ = false;
        cursorWorld_ = pendingPoints_[1];
        rawCursorWorld_ = cursorWorld_;
        lastWorldPosition_ = cursorWorld_;
        cursorValid_ = true;
        currentSnap_ = SnapResult{};
    }

    void setTwoPointArcSagitta(qreal sagitta)
    {
        if (pendingPoints_.size() < 2 || !std::isfinite(sagitta)) {
            return;
        }

        const QPointF chord = pendingPoints_[1] - pendingPoints_[0];
        const qreal chordLength = std::hypot(chord.x(), chord.y());
        if (chordLength <= 1.0e-9) {
            return;
        }
        const QPointF midpoint = (pendingPoints_[0] + pendingPoints_[1]) * 0.5;
        const QPointF perpendicular(-chord.y() / chordLength,
                                    chord.x() / chordLength);
        const qreal currentHeight = QPointF::dotProduct(cursorWorld_ - midpoint,
                                                        perpendicular);
        const qreal sign = currentHeight < 0.0 ? -1.0 : 1.0;
        cursorWorld_ = midpoint + perpendicular * (sign * std::abs(sagitta));
        rawCursorWorld_ = cursorWorld_;
        lastWorldPosition_ = cursorWorld_;
        cursorValid_ = true;
        currentSnap_ = SnapResult{};
    }

    void toggleArcPlaneLock()
    {
        arcPlaneLocked_ = !arcPlaneLocked_;
        if (arcPlaneLocked_ && pendingPoints_.isEmpty()) {
            arcLockedFrame_ = viewportTransform_.workPlaneFrame();
            arcLockedFrameValid_ = isValidWorkPlaneFrame(arcLockedFrame_);
        } else if (!arcPlaneLocked_) {
            arcLockedFrame_ = {};
            arcLockedFrameValid_ = false;
            if (pendingPoints_.isEmpty()) {
                updateDrawingWorkPlaneFromHover(currentArcScreenPosition());
            }
        }
        update();
    }

    void toggleTwoPointArcPerpendicularPlane()
    {
        if (arcMode_ == ArcMode::TwoPoint && pendingPoints_.size() == 1 &&
            arcReferenceFrameValid_) {
            if (arcPerpendicularPlaneActive_) {
                arcPerpendicularPlaneActive_ = false;
                arcTwoPointPerpendicularNormal_ = {};
                arcTwoPointPerpendicularNormalValid_ = false;
                arcVerticalOverrideAxis_ = 0;
                restoreArcChordReferencePlaneForEndpointPick();
                pendingPoints_[0] = worldPointToWorkPlaneFrame(
                    arcFirstPointWorld_, viewportTransform_.workPlaneFrame());
            } else {
                const Point3D endpoint = arcResolvedChordPointValid_
                                             ? arcResolvedChordPointWorld_
                                             : workPlaneFramePointToWorld(
                                                   cursorWorld_,
                                                   viewportTransform_.workPlaneFrame());
                const Point3D bridge = arcVectorSubtract(endpoint,
                                                         arcFirstPointWorld_);
                const qreal bridgeLength = arcVectorLength(bridge);
                if (bridgeLength <= 1.0e-9) {
                    return;
                }
                const Point3D bridgeDirection =
                    arcVectorScale(bridge, 1.0 / bridgeLength);
                Point3D normal = arcVectorCross(bridgeDirection,
                                                arcReferenceNormal_);
                const qreal normalLength = arcVectorLength(normal);
                if (normalLength <= 1.0e-9) {
                    return;
                }
                normal = arcVectorScale(normal, 1.0 / normalLength);
                Point3D floorNormal = arcReferenceNormal_;
                const qreal floorNormalLength = arcVectorLength(floorNormal);
                if (floorNormalLength <= 1.0e-9) {
                    return;
                }
                floorNormal = arcVectorScale(floorNormal,
                                             1.0 / floorNormalLength);
                Point3D planeX = arcVectorCross(floorNormal, normal);
                const qreal planeXLength = arcVectorLength(planeX);
                if (planeXLength <= 1.0e-9) {
                    return;
                }

                WorkPlaneFrame frame;
                frame.origin = arcFirstPointWorld_;
                frame.xAxis = arcVectorScale(planeX, 1.0 / planeXLength);
                frame.yAxis = floorNormal;
                frame.normal = normal;
                frame.valid = true;
                if (!isValidWorkPlaneFrame(frame)) {
                    return;
                }

                arcPerpendicularPlaneActive_ = true;
                arcTwoPointPerpendicularNormal_ = normal;
                arcTwoPointPerpendicularNormalValid_ = true;
                arcVerticalOverrideAxis_ = 0;
                viewportTransform_.setWorkPlaneFrame(frame);
                arcInputFrame_ = frame;
                arcInputFrameValid_ = true;
                pendingPoints_[0] = worldPointToWorkPlaneFrame(
                    arcFirstPointWorld_, frame);
            }

            refreshArcCursorOnCurrentFrame();
            update();
            emitCoordinateUpdate();
            return;
        }

        if (pendingPoints_.size() == 1 && arcReferenceFrameValid_) {
            // Match the add-on: P can choose the arc plane while the chord
            // endpoint is still moving. The next cursor update resolves the
            // provisional chord and applies this plane to its preview.
            arcPerpendicularPlaneActive_ = !arcPerpendicularPlaneActive_;
            arcVerticalOverrideAxis_ = 0;
            restoreArcChordReferencePlaneForEndpointPick();
            refreshArcCursorOnCurrentFrame();
            update();
            emitCoordinateUpdate();
            return;
        }
        if (pendingPoints_.size() < 2 || !arcChordWorldPointsValid_) {
            return;
        }

        const Point3D chord = arcVectorSubtract(arcSecondPointWorld_,
                                                arcFirstPointWorld_);
        const qreal chordLength = arcVectorLength(chord);
        if (chordLength <= 1.0e-9) {
            return;
        }
        const Point3D chordDirection = arcVectorScale(chord, 1.0 / chordLength);
        const qreal verticalThreshold = arcWasVertical_ ? 0.98 : 0.995;
        const bool isVertical = std::abs(arcVectorDot(chordDirection,
                                                      arcReferenceNormal_)) >
                                verticalThreshold;
        arcWasVertical_ = isVertical;
        if (isVertical) {
            arcVerticalOverrideAxis_ = arcVerticalOverrideAxis_ == Qt::Key_X
                                           ? Qt::Key_Y
                                           : Qt::Key_X;
        } else {
            arcPerpendicularPlaneActive_ = !arcPerpendicularPlaneActive_;
            arcVerticalOverrideAxis_ = 0;
            if (arcPerpendicularPlaneActive_) {
                arcTwoPointPerpendicularNormal_ = arcVectorCross(
                    chordDirection, arcReferenceNormal_);
                const qreal normalLength =
                    arcVectorLength(arcTwoPointPerpendicularNormal_);
                if (normalLength > 1.0e-9) {
                    arcTwoPointPerpendicularNormal_ = arcVectorScale(
                        arcTwoPointPerpendicularNormal_, 1.0 / normalLength);
                    arcTwoPointPerpendicularNormalValid_ = true;
                } else {
                    arcTwoPointPerpendicularNormal_ = {};
                    arcTwoPointPerpendicularNormalValid_ = false;
                }
            } else {
                arcTwoPointPerpendicularNormal_ = {};
                arcTwoPointPerpendicularNormalValid_ = false;
            }
        }

        updateArcTwoPointWorkPlaneForView();
        refreshArcCursorOnCurrentFrame();
        update();
        emitCoordinateUpdate();
    }

    int inferredArcChordAxis(const QPointF &screenPosition) const
    {
        if (!arcReferenceFrameValid_) {
            return 0;
        }

        QPointF anchorScreen;
        if (!viewportTransform_.worldPointToScreen(arcFirstPointWorld_,
                                                   size(),
                                                   &anchorScreen)) {
            return 0;
        }
        QPointF cursorDirection = screenPosition - anchorScreen;
        const qreal cursorLength = std::hypot(cursorDirection.x(),
                                              cursorDirection.y());
        if (!std::isfinite(cursorLength) || cursorLength < 1.0) {
            return 0;
        }
        cursorDirection /= cursorLength;

        qreal bestAlignment = -1.0;
        int bestKey = 0;
        constexpr int axisKeys[] = {Qt::Key_X, Qt::Key_Y, Qt::Key_Z};
        for (const int key : axisKeys) {
            QPointF axisScreen;
            const Point3D unitAxis = arcAxisDirection(key);
            const Point3D unitPoint = arcVectorAdd(arcFirstPointWorld_, unitAxis);
            if (!viewportTransform_.worldPointToScreen(unitPoint,
                                                       size(),
                                                       &axisScreen)) {
                continue;
            }
            QPointF projectedDirection = axisScreen - anchorScreen;
            const qreal projectedLength = std::hypot(projectedDirection.x(),
                                                     projectedDirection.y());
            if (!std::isfinite(projectedLength) || projectedLength <= 1.0e-9) {
                continue;
            }
            projectedDirection /= projectedLength;
            const qreal alignment = std::abs(QPointF::dotProduct(
                cursorDirection, projectedDirection));
            if (alignment > bestAlignment) {
                bestAlignment = alignment;
                bestKey = key;
            }
        }

        constexpr qreal sixDegreeAlignment = 0.9945218953682733;
        return bestAlignment >= sixDegreeAlignment ? bestKey : 0;
    }

    QPointF constrainArcChordEndpoint(const QPointF &rawPoint,
                                      bool altModifier,
                                      const QPointF &screenPosition)
    {
        const WorkPlaneFrame oldFrame = viewportTransform_.workPlaneFrame();
        if (!isValidWorkPlaneFrame(oldFrame) || !arcReferenceFrameValid_) {
            arcResolvedChordPointValid_ = false;
            return rawPoint;
        }

        const Point3D rawWorld = workPlaneFramePointToWorld(rawPoint, oldFrame);
        Point3D targetWorld = currentSnap_.isValid()
                                  ? (currentSnap_.hasWorldPoint
                                         ? currentSnap_.worldPoint
                                         : workPlaneFramePointToWorld(
                                               currentSnap_.point, oldFrame))
                                  : rawWorld;
        int axisKey = 0;
        if (!altModifier) {
            axisKey = arcAxisConstraintKey_;
            if (axisKey == 0 && !currentSnap_.isValid()) {
                axisKey = inferredArcChordAxis(screenPosition);
            }
        }

        if (axisKey != 0) {
            const Point3D direction = arcAxisDirection(axisKey);
            if (currentSnap_.isValid()) {
                const Point3D delta = arcVectorSubtract(targetWorld,
                                                        arcFirstPointWorld_);
                targetWorld = arcVectorAdd(
                    arcFirstPointWorld_,
                    arcVectorScale(direction, arcVectorDot(delta, direction)));
            } else {
                Point3D axisPoint;
                if (viewportTransform_.screenToWorldAxis(screenPosition,
                                                         size(),
                                                         arcFirstPointWorld_,
                                                         direction,
                                                         &axisPoint)) {
                    targetWorld = axisPoint;
                }
            }
        }

        arcResolvedChordPointWorld_ = targetWorld;
        arcResolvedChordPointValid_ = true;
        if (arcMode_ != ArcMode::TwoPoint || arcChordWorldPointsValid_) {
            updateArcTwoPointWorkPlaneForView(&targetWorld);
        }
        const WorkPlaneFrame newFrame = viewportTransform_.workPlaneFrame();
        const QPointF targetLocal = worldPointToWorkPlaneFrame(targetWorld,
                                                               newFrame);
        if (currentSnap_.isValid()) {
            currentSnap_.point = targetLocal;
        }

        if (!workPlaneFramesMatch(oldFrame, newFrame)) {
            QPointF remappedRaw;
            if (viewportTransform_.screenToWorkPlane(screenPosition,
                                                      size(),
                                                      newFrame,
                                                      &remappedRaw)) {
                rawCursorWorld_ = remappedRaw;
            } else {
                rawCursorWorld_ = worldPointToWorkPlaneFrame(rawWorld, newFrame);
            }
        }
        return targetLocal;
    }

    void refreshArcCursorOnCurrentFrame()
    {
        if (!cursorValid_) {
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

    bool handleArcAxisKey(int key)
    {
        if (activeTool_ != Tool::Arc) {
            return false;
        }

        if (arcMode_ == ArcMode::OnePoint) {
            if (!pendingPoints_.isEmpty()) {
                return true;
            }
            const WorkPlaneFrame currentFrame = viewportTransform_.workPlaneFrame();
            if (arcPlaneNormalLockKey_ == key && arcAxisBaseFrameValid_) {
                viewportTransform_.setWorkPlaneFrame(arcAxisBaseFrame_);
                toolDrawingFrame_ = {};
                toolDrawingPlaneLocked_ = false;
                arcPlaneNormalLockKey_ = 0;
                arcAxisBaseFrame_ = {};
                arcAxisBaseFrameValid_ = false;
            } else {
                if (!arcAxisBaseFrameValid_) {
                    arcAxisBaseFrame_ = currentFrame;
                    arcAxisBaseFrameValid_ = isValidWorkPlaneFrame(currentFrame);
                }
                const QPointF screenPosition = currentArcScreenPosition();
                QPointF cursorLocal;
                Point3D planeOrigin = currentFrame.origin;
                if (viewportTransform_.screenToWorkPlane(screenPosition,
                                                         size(),
                                                         currentFrame,
                                                         &cursorLocal)) {
                    planeOrigin = workPlaneFramePointToWorld(cursorLocal,
                                                             currentFrame);
                }
                const WorkPlaneFrame lockedFrame = makeWorkPlaneFrameFromNormal(
                    planeOrigin, arcAxisDirection(key));
                if (isValidWorkPlaneFrame(lockedFrame)) {
                    viewportTransform_.setWorkPlaneFrame(lockedFrame);
                    toolDrawingFrame_ = lockedFrame;
                    toolDrawingPlaneLocked_ = true;
                    arcPlaneNormalLockKey_ = key;
                }
            }
            refreshArcCursorOnCurrentFrame();
            update();
            return true;
        }

        if (pendingPoints_.isEmpty()) {
            arcAxisConstraintKey_ = arcAxisConstraintKey_ == key ? 0 : key;
            update();
            return true;
        }

        if ((arcMode_ == ArcMode::TwoPoint ||
             arcMode_ == ArcMode::ThreePoint) &&
            pendingPoints_.size() == 1) {
            arcAxisConstraintKey_ = arcAxisConstraintKey_ == key ? 0 : key;
            if (!(arcMode_ == ArcMode::TwoPoint &&
                  arcPerpendicularPlaneActive_)) {
                restoreArcChordReferencePlaneForEndpointPick();
            }
            refreshArcCursorOnCurrentFrame();
            update();
            return true;
        }

        if (pendingPoints_.size() >= 2 && arcChordWorldPointsValid_ &&
            (key == Qt::Key_X || key == Qt::Key_Y)) {
            const Point3D chord = arcVectorSubtract(arcSecondPointWorld_,
                                                    arcFirstPointWorld_);
            const qreal chordLength = arcVectorLength(chord);
            if (chordLength > 1.0e-9 &&
                std::abs(arcVectorDot(arcVectorScale(chord, 1.0 / chordLength),
                                      arcReferenceNormal_)) > 0.99) {
                arcVerticalOverrideAxis_ = key;
                updateArcTwoPointWorkPlaneForView();
                refreshArcCursorOnCurrentFrame();
            }
        }
        return true;
    }

    void updateDrawingWorkPlaneFromHover(const QPointF &screenPosition)
    {
        if (draggingSelected_ || draggingControlPoint_ || grabActive_ || duplicateActive_) {
            return;
        }
        if (activeTool_ == Tool::Arc && pendingPoints_.isEmpty() &&
            arcPlaneLocked_ &&
            arcLockedFrameValid_) {
            viewportTransform_.setWorkPlaneFrame(arcLockedFrame_);
            return;
        }
        if (activeTool_ == Tool::Rotate) {
            if (rotateStep_ > 0 && isValidWorkPlaneFrame(rotateFrame_)) {
                viewportTransform_.setWorkPlaneFrame(rotateFrame_);
                return;
            }
            if ((rotateAxisLockKey_ != 0 ||
                 rotatePrePivotPerpendicularActive_) &&
                isValidWorkPlaneFrame(rotatePrePivotPlaneFrame_)) {
                viewportTransform_.setWorkPlaneFrame(rotatePrePivotPlaneFrame_);
                return;
            }

            const int shapeIndex = curveHitTester_.hitTestShapeOnAnyWorkPlane(
                document_, screenPosition, viewportTransform_, size());
            if (shapeIndex >= 0 && shapeIndex < shapes_.size()) {
                viewportTransform_.setWorkPlaneFrame(
                    shapeWorkPlaneFrame(shapes_[shapeIndex]));
            } else {
                useAddonCompatibleDrawingPlane();
            }
            return;
        }
        if (activeTool_ != Tool::Line && toolDrawingPlaneLocked_ &&
            isValidWorkPlaneFrame(toolDrawingFrame_)) {
            viewportTransform_.setWorkPlaneFrame(toolDrawingFrame_);
            return;
        }
        const bool unlockedPointInput =
            (activeTool_ == Tool::PointByLine ||
             activeTool_ == Tool::PointByArcs) && !toolDrawingPlaneLocked_;
        if ((activeTool_ == Tool::Line && linePreviewPlaneLocked_) ||
            (activeTool_ != Tool::Line && !pendingPoints_.isEmpty() &&
             !unlockedPointInput)) {
            return;
        }
        const bool drawingShape =
            geometryTypeForTool(activeTool_) != GeometryType::Invalid;
        const bool selectingObject = activeTool_ == Tool::Select;
        if (!drawingShape && !selectingObject) {
            return;
        }
        const int shapeIndex = selectingObject
            ? curveHitTester_.hitTestShape(document_, screenPosition, viewportTransform_, size())
            : curveHitTester_.hitTestShapeOnAnyWorkPlane(
                  document_, screenPosition, viewportTransform_, size());
        if (shapeIndex >= 0 && shapeIndex < shapes_.size()) {
            const Shape &shape = shapes_[shapeIndex];
            const WorkPlaneFrame frame = shapeWorkPlaneFrame(shape);
            const Point3D viewNormal = viewportTransform_.viewDirection();
            const qreal planeFacing = std::abs(frame.normal.x*viewNormal.x +
                frame.normal.y*viewNormal.y + frame.normal.z*viewNormal.z);
            if (selectingObject && !controlPointsVisible_ && planeFacing < 0.15) {
                // An almost edge-on object plane makes tiny cursor moves
                // produce enormous world deltas. Drag through the visible
                // pick depth on a plane facing the camera instead.
                Point3D pickWorld = frame.origin;
                curveHitTester_.hitTestVisibleDepth(document_, screenPosition,
                    viewportTransform_, size(), &pickWorld);
                WorkPlaneFrame dragFrame = viewAlignedDrawingFrame();
                dragFrame.origin = pickWorld;
                viewportTransform_.setWorkPlaneFrame(dragFrame);
                return;
            }
            if (drawingShape || selectingObject) {
                QPointF planePoint;
                if (!drawingShape || viewportTransform_.screenToWorkPlane(
                        screenPosition, size(), frame, &planePoint)) {
                    viewportTransform_.setWorkPlaneFrame(frame);
                    if (qEnvironmentVariable("CLASSICAD_SNAP_TRACE") ==
                        QStringLiteral("1")) {
                        const auto point3DText = [](const Point3D &point) {
                            return QStringLiteral("(%1, %2, %3)")
                                .arg(point.x, 0, 'g', 12)
                                .arg(point.y, 0, 'g', 12)
                                .arg(point.z, 0, 'g', 12);
                        };
                        DebugLog::instance().write(
                            QStringLiteral("osnap-trace hover-frame shape=%1 geometry=%2 screen=%3 origin=%4 x=%5 y=%6 normal=%7")
                                .arg(shapeIndex)
                                .arg(geometryTypeName(shape.geometryType))
                                .arg(precisePointText(screenPosition))
                                .arg(point3DText(frame.origin))
                                .arg(point3DText(frame.xAxis))
                                .arg(point3DText(frame.yAxis))
                                .arg(point3DText(frame.normal)));
                    }
                    return;
                }
            }
            if (!drawingShape) {
                return;
            }
        }
        if (drawingShape) {
            // SurfaceDrawTool resolves a fresh plane from hovered scene
            // geometry first. Empty-space input follows Blender's view-based
            // fallback, mapped onto the supported principal workplanes.
            useAddonCompatibleDrawingPlane();
        }
    }

    void useAddonCompatibleDrawingPlane()
    {
        WorkPlane plane = WorkPlane::XY;
        if (viewportTransform_.isPerspectiveEnabled()) {
            // DrawManager.get_snap_data uses world Z as its perspective
            // fallback, regardless of the camera's viewing direction.
            plane = WorkPlane::XY;
        } else {
            switch (viewportTransform_.viewPreset()) {
            // Fixed views use the plane visible to the cursor. These map to
            // the app's principal workplanes and offsets.
            case ViewportViewPreset::Top:
            case ViewportViewPreset::Bottom:
                plane = WorkPlane::XY;
                break;
            case ViewportViewPreset::Front:
            case ViewportViewPreset::Back:
                plane = WorkPlane::XZ;
                break;
            case ViewportViewPreset::Right:
            case ViewportViewPreset::Left:
                plane = WorkPlane::YZ;
                break;
            case ViewportViewPreset::Isometric:
            case ViewportViewPreset::Perspective:
            case ViewportViewPreset::Custom:
                {
                    const Point3D normal = viewportTransform_.viewDirection();
                    const qreal xAlignment = std::abs(normal.x);
                    const qreal yAlignment = std::abs(normal.y);
                    const qreal zAlignment = std::abs(normal.z);
                    if (activeTool_ == Tool::Line &&
                        std::max({xAlignment, yAlignment, zAlignment}) <= 0.99) {
                        // The addon uses the actual camera-facing plane for
                        // oblique ortho. Storage remains local planar NURBS.
                        viewportTransform_.setWorkPlaneFrame(
                            makeWorkPlaneFrameFromNormal({0.0, 0.0, 0.0},
                                {-normal.x, -normal.y, -normal.z}));
                        return;
                    }
                    plane = xAlignment >= yAlignment && xAlignment >= zAlignment
                                ? WorkPlane::YZ
                                : (yAlignment >= zAlignment ? WorkPlane::XZ
                                                            : WorkPlane::XY);
                }
                break;
            }
        }

        const WorkPlaneFrame frame = makeWorkPlaneFrame(plane, 0.0);
        const bool frameChanged = !workPlaneFramesMatch(
            viewportTransform_.workPlaneFrame(), frame);
        const bool planeChanged = !workPlaneMatches(
            viewportTransform_.workPlane(),
            viewportTransform_.workPlaneOffset(),
            plane,
            0.0);
        if (!frameChanged && !planeChanged) {
            return;
        }
        viewportTransform_.setWorkPlane(plane, 0.0);
        notifyViewStateChanged();
    }

    WorkPlaneFrame viewAlignedDrawingFrame() const
    {
        const Point3D target = viewportTransform_.viewTarget();
        switch (viewportTransform_.viewPreset()) {
        case ViewportViewPreset::Top:
        case ViewportViewPreset::Bottom:
            return makeWorkPlaneFrame(WorkPlane::XY, target.z);
        case ViewportViewPreset::Front:
        case ViewportViewPreset::Back:
            return makeWorkPlaneFrame(WorkPlane::XZ, target.y);
        case ViewportViewPreset::Right:
        case ViewportViewPreset::Left:
            return makeWorkPlaneFrame(WorkPlane::YZ, target.x);
        case ViewportViewPreset::Isometric:
        case ViewportViewPreset::Perspective:
        case ViewportViewPreset::Custom:
            break;
        }

        const Point3D normal = viewportTransform_.viewDirection();
        const Point3D up = viewportTransform_.viewUp();
        const Point3D screenRight{up.y * normal.z - up.z * normal.y,
                                  up.z * normal.x - up.x * normal.z,
                                  up.x * normal.y - up.y * normal.x};
        return makeWorkPlaneFrameFromNormal(target, normal, screenRight);
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

    void drawLineToolPreview(QPainter &painter, bool drawCurve = true)
    {
        if (activeTool_ == Tool::Line && !linePreviewWorldPoints_.isEmpty()) {
            viewportOverlay_.drawWorldLinePreview(painter, linePreviewWorldPoints_,
                linePreviewWorldCursor_, cursorValid_, size(), drawCurve,
                linePreviewFrame_);
            if (currentSnap_.isValid()) {
                drawSnapMarker(painter, currentSnap_.type, currentSnap_.point);
            }
            return;
        }
        viewportOverlay_.drawLinePreview(painter,
                                         pendingPoints_,
                                         cursorWorld_,
                                         cursorValid_,
                                         currentSnap_,
                                         size(),
                                         drawCurve,
                                         activeTool_ == Tool::Line
                                             ? linePreviewFrame_
                                             : WorkPlaneFrame{});
    }

    void drawMirrorToolPreview(
        QPainter &painter,
        const QVector<ObjectId> &gpuHandledObjects = {})
    {
        if (pendingPoints_.isEmpty() || !cursorValid_) {
            return;
        }

        const QPointF axisStart = pendingPoints_.first();
        const QPointF axisEnd = cursorWorld_;
        for (const ObjectId objectId : mirrorShapeIndices_) {
            const int shapeIndex = objectIndex(objectId);
            if (shapeIndex < 0 || shapeIndex >= shapes_.size() ||
                !document_.isObjectVisible(objectId)) {
                continue;
            }

            Shape mirroredShape;
            if (mirrorShapeAcrossLine(shapes_[shapeIndex],
                                      axisStart,
                                      axisEnd,
                                      &mirroredShape)) {
                if (!gpuHandledObjects.contains(objectId)) {
                    drawShape(painter, mirroredShape, true, false, false);
                }
                if (!subdivisionActive_ || objectId != subdivisionShapeIndex_) {
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
        arcPreviewInitialized_ = false;
        arcPreviewPreviousAngle_ = 0.0;
        arcPreviewSweepAngle_ = 0.0;
        arcAngleValueLocked_ = false;
    }

    void initializeArcPreviewTracking()
    {
        resetArcPreviewTracking();
        if (arcMode_ != ArcMode::OnePoint || pendingPoints_.size() < 2) {
            return;
        }

        const QPointF radiusVector = pendingPoints_[1] - pendingPoints_[0];
        const qreal radius = std::hypot(radiusVector.x(), radiusVector.y());
        if (radius <= 1e-9) {
            return;
        }

        arcPreviewStartAngle_ = std::atan2(radiusVector.y(),
                                           radiusVector.x());
        arcPreviewPreviousAngle_ = arcPreviewStartAngle_;
        arcPreviewInitialized_ = true;
    }

    void updateArcPreviewTracking(const QPointF &cursorWorld)
    {
        if (arcMode_ != ArcMode::OnePoint || pendingPoints_.size() < 2 ||
            arcAngleValueLocked_) {
            return;
        }

        const QPointF cursorVector = cursorWorld - pendingPoints_[0];
        const qreal radius = std::hypot(cursorVector.x(), cursorVector.y());
        if (radius <= 1e-9) {
            return;
        }

        qreal angle = std::atan2(cursorVector.y(), cursorVector.x());
        if (arcAngleSnapEnabled_ && !currentSnap_.isValid()) {
            angle = snapOnePointArcAngle(angle);
        }
        if (!arcPreviewInitialized_) {
            arcPreviewStartAngle_ = std::atan2(
                pendingPoints_[1].y() - pendingPoints_[0].y(),
                pendingPoints_[1].x() - pendingPoints_[0].x());
            arcPreviewPreviousAngle_ = angle;
            arcPreviewInitialized_ = true;
            return;
        }

        constexpr qreal pi = 3.14159265358979323846;
        constexpr qreal twoPi = 2.0 * pi;
        qreal delta = angle - arcPreviewPreviousAngle_;
        if (delta > pi) {
            delta -= twoPi;
        } else if (delta < -pi) {
            delta += twoPi;
        }

        arcPreviewSweepAngle_ += delta;
        constexpr qreal sweepLimit = 2.0 * pi;
        if (std::abs(arcPreviewSweepAngle_) > sweepLimit &&
            std::cos(angle - arcPreviewStartAngle_) > 0.8) {
            qreal phase = std::fmod(arcPreviewSweepAngle_ + pi, twoPi);
            if (phase < 0.0) {
                phase += twoPi;
            }
            phase -= pi;
            arcPreviewSweepAngle_ =
                std::copysign(sweepLimit, arcPreviewSweepAngle_) + phase;
        }
        arcPreviewPreviousAngle_ = angle;
    }

    qreal arcCompassRotation() const
    {
        qreal rotation = 0.0;
        if (arcMode_ == ArcMode::OnePoint && !pendingPoints_.isEmpty()) {
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
        const QString radiusText = arcTextInputMode_ == ArcTextInputMode::Radius
                                       ? arcTextInput_ + QLatin1Char('|')
                                       : QStringLiteral("%1 %2")
                                             .arg(radius * unitsPerMillimeter,
                                                  0,
                                                  'f',
                                                  3)
                                             .arg(arcLengthUnitSuffix(settings.lengthUnit));
        const QString angleText = arcTextInputMode_ == ArcTextInputMode::Angle
                                      ? arcTextInput_ + QLatin1Char('|')
                                      : QStringLiteral("%1°")
                                            .arg(-arcPreviewSweepAngle_ * 180.0 / pi,
                                                 0,
                                                 'f',
                                                 1);
        const QString stageHint = arcTextInputMode_ != ArcTextInputMode::None
                                      ? QStringLiteral("Enter applies value")
                                      : pendingPoints_.isEmpty()
                                            ? QStringLiteral("Click center")
                                            : pendingPoints_.size() == 1
                                                  ? QStringLiteral("Click radius")
                                                  : QStringLiteral("Click sweep to finish");
        return {QStringLiteral("R: %1    ∠ %2").arg(radiusText, angleText),
                QStringLiteral("%1  •  Esc exits  •  C snap %2  •  R radius  •  A angle  •  P perp  •  L plane lock")
                    .arg(stageHint,
                         arcAngleSnapEnabled_ ? QStringLiteral("on")
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
        const QString chordText = arcTextInputMode_ == ArcTextInputMode::ChordLength
                                       ? arcTextInput_ + QLatin1Char('|')
                                       : QStringLiteral("%1 %2")
                                             .arg(chordLength * unitsPerMillimeter,
                                                  0, 'f', 3)
                                             .arg(arcLengthUnitSuffix(settings.lengthUnit));
        const QString heightText = arcTextInputMode_ == ArcTextInputMode::Sagitta
                                       ? arcTextInput_ + QLatin1Char('|')
                                       : QStringLiteral("%1 %2")
                                             .arg(height * unitsPerMillimeter,
                                                  0, 'f', 3)
                                             .arg(arcLengthUnitSuffix(settings.lengthUnit));

        QString stageHint;
        if (arcTextInputMode_ != ArcTextInputMode::None) {
            stageHint = QStringLiteral("Enter applies value");
        } else if (pendingPoints_.isEmpty()) {
            stageHint = QStringLiteral("Click first endpoint  •  L locks plane");
        } else if (pendingPoints_.size() == 1) {
            stageHint = QStringLiteral("Click chord end  •  D length  •  X/Y/Z axis  •  P perp  •  Alt bypass");
        } else {
            stageHint = QStringLiteral("Click arc height  •  H sagitta  •  P perpendicular plane %1  •  Alt bypass half-circle snap")
                            .arg(arcPerpendicularPlaneActive_
                                     ? QStringLiteral("ON")
                                     : QStringLiteral("OFF"));
            if (arcVerticalOverrideAxis_ != 0) {
                stageHint += QStringLiteral("  •  %1 plane")
                                 .arg(arcVerticalOverrideAxis_ == Qt::Key_X
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
                            .arg(arcPerpendicularPlaneActive_
                                     ? QStringLiteral("ON")
                                     : QStringLiteral("OFF"));
            if (arcVerticalOverrideAxis_ != 0) {
                stageHint += QStringLiteral("  •  %1 plane")
                                 .arg(arcVerticalOverrideAxis_ == Qt::Key_X
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
        viewportOverlay_.drawArcPreview(painter,
                                        pendingPoints_,
                                        arcMode_,
                                        cursorWorld_,
                                        cursorValid_,
                                        arcPreviewSweepAngle_,
                                        currentSnap_,
                                        size(),
                                        viewportTransform_.workPlaneFrame(),
                                        arcCompassRotation(),
                                        curveColor,
                                        drawCurve);
        if (arcMode_ == ArcMode::OnePoint) {
            const ArcHudDisplay display = onePointArcHudDisplay();
            drawArcHudPanel(painter, display, 570.0);
        } else if (arcMode_ == ArcMode::TwoPoint) {
            drawArcHudPanel(painter, twoPointArcHudDisplay(), 750.0);
        } else if (arcMode_ == ArcMode::ThreePoint) {
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
            controllerPreviewShapeVisible_ &&
            isValidWorkPlaneFrame(controllerPreviewShape_.workPlaneFrame) &&
            isValidWorkPlaneFrame(inputFrame) &&
            !workPlaneFramesMatch(controllerPreviewShape_.workPlaneFrame,
                                  inputFrame);
        if (drawCurve && previewUsesSeparateFrame) {
            drawShape(painter, controllerPreviewShape_, true);
        }
        if (drawCurve) {
            viewportOverlay_.drawCirclePreview(painter,
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
            drawSnapMarker(painter, currentSnap_.type, currentSnap_.point);
        }
        drawArcHudPanel(painter,
                        {circleHudDimensionsLine_, circleHudInstructionsLine_},
                        750.0);
    }

    void drawCircleTangentToolPreview(QPainter &painter,
                                      bool drawPreviewGeometry = true)
    {
        if (controllerPreviewShapeVisible_ && drawPreviewGeometry) {
            drawShape(painter, controllerPreviewShape_, true);
        }
    }

    void drawEllipseToolPreview(QPainter &painter, bool drawCurve = true)
    {
        if (drawCurve && controllerPreviewShapeVisible_) {
            drawShape(painter, controllerPreviewShape_, true);
        }
        viewportOverlay_.drawEllipsePreview(painter,
                                             pendingPoints_,
                                             cursorWorld_,
                                             cursorValid_,
                                             ellipsePreviewGuides_,
                                             currentSnap_,
                                             size());
        drawArcHudPanel(painter,
                        {ellipseHudDimensionsLine_, ellipseHudInstructionsLine_},
                        750.0);
    }

    void drawRectangleToolPreview(QPainter &painter, bool drawCurve = true)
    {
        if (drawCurve) {
            if (controllerPreviewShapeVisible_) {
                const Layer *layer = document_.layer(document_.activeLayerId());
                const QColor color = layer != nullptr && layer->color.isValid()
                    ? layer->color : QColor(QStringLiteral("#000000"));
                drawShape(painter, controllerPreviewShape_, true, false, true, color);
            }
            QVector<QPointF> markers = pendingPoints_;
            if (controllerPreviewShapeVisible_ &&
                controllerPreviewShape_.geometryType == GeometryType::Rectangle) {
                markers += rectangleVertices(controllerPreviewShape_);
            }
            painter.save();
            painter.setRenderHint(QPainter::Antialiasing, true);
            for (const ToolPreviewGuide &guide : rectanglePreviewGuides_) {
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
        if (currentSnap_.isValid()) drawSnapMarker(painter, currentSnap_.type, currentSnap_.point);
        drawArcHudPanel(painter,
                        {rectangleHudDimensionsLine_, rectangleHudInstructionsLine_},
                        750.0);
    }

    void drawPolygonToolPreview(QPainter &painter, bool drawCurve = true)
    {
        if (drawCurve && controllerPreviewShapeVisible_) {
            const Layer *activeLayer = document_.layer(document_.activeLayerId());
            const QColor previewColor =
                activeLayer != nullptr && activeLayer->color.isValid()
                    ? activeLayer->color
                    : QColor(QStringLiteral("#d28b45"));
            drawShape(painter, controllerPreviewShape_, true, false, true,
                      previewColor);
        }
        viewportOverlay_.drawPolygonPreview(painter,
                                            activeTool_,
                                            polygonSideCount_,
                                            pendingPoints_,
                                            cursorWorld_,
                                            cursorValid_,
                                            currentSnap_,
                                            size(),
                                            false);
        drawArcHudPanel(painter,
                        {polygonHudDimensionsLine_, polygonHudInstructionsLine_},
                        750.0);
    }

    void drawControlPoints(QPainter &painter,
                           const Shape &shape,
                           int shapeIndex,
                           bool drawMarkers = true)
    {
        const WorkPlaneFrame previousFrame = viewportTransform_.workPlaneFrame();
        viewportTransform_.setWorkPlaneFrame(shapeWorkPlaneFrame(shape));
        viewportOverlay_.drawControlPoints(painter,
                                           shape,
                                           size(),
                                           shapes_.objectIdAt(shapeIndex),
                                           selectedShapeIndex_,
                                           draggingControlPoint_,
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
        viewportOverlay_.drawPointPreview(painter,
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
            for (const Shape &preview : pointPreviewShapes_) {
                if (preview.geometryType == GeometryType::Point) continue;
                const bool isArc = preview.geometryType == GeometryType::Arc ||
                                   preview.geometryType == GeometryType::Circle;
                const bool pointByArcsArc = activeTool_ == Tool::PointByArcs &&
                                            isArc;
                drawShape(painter, preview, true, false, true,
                          pointByArcsArc ? arcColor : QColor(Qt::black));
            }
            for (const ToolPreviewGuide &guide : pointPreviewGuides_) {
                Shape line;
                line.geometryType = GeometryType::Line;
                line.points = {guide.line.p1(), guide.line.p2()};
                line.workPlane = WorkPlane::XY;
                line.workPlaneOffset = guide.hasWorkPlaneFrame
                    ? guide.workPlaneFrame.origin.z
                    : pointPreviewFrame_.origin.z;
                line.workPlaneFrame = guide.hasWorkPlaneFrame
                    ? guide.workPlaneFrame : pointPreviewFrame_;
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
        for (const Shape &preview : pointPreviewShapes_) {
            if (preview.geometryType != GeometryType::Point ||
                preview.points.isEmpty()) {
                continue;
            }
            const WorkPlaneFrame frame = shapeWorkPlaneFrame(preview);
            const qreal markerSize = activeTool_ == Tool::PointByArcs &&
                                             pointPreviewStage_ == 5
                                         ? 3.0 : 5.0;
            drawPointCross(workPlaneFramePointToWorld(preview.points.first(), frame),
                           frame, markerSize);
        }
        if (currentSnap_.isValid()) {
            drawSnapMarker(painter, currentSnap_.type, currentSnap_.point);
        }
    }

    void drawRotateToolPreview(QPainter &painter, bool drawGeometry = true)
    {
        viewportOverlay_.drawRotatePreview(painter,
                                           cursorWorld_,
                                           cursorValid_,
                                           rotateStep_,
                                           isValidWorkPlaneFrame(rotateFrame_)
                                               ? rotateFrame_
                                               : viewportTransform_.workPlaneFrame(),
                                           rotateBaseWorld_,
                                           rotateReferenceWorld_,
                                           rotatePreviewAngle_,
                                           rotateAngleInput_,
                                           rotateAngleInputActive_,
                                           rotateAngleSnapEnabled_,
                                           rotateSnapIncrementDegrees(),
                                           rotateToolPreferences_.useRadians,
                                           currentSnap_,
                                           size(),
                                           drawGeometry);
    }

    void drawScaleToolGuide(QPainter &painter, bool drawGuide = true)
    {
        if (!drawGuide || !cursorValid_ || scaleStep_ != 2) {
            return;
        }

        QPointF guideEnd = cursorWorld_;
        if (scaleMode_ == ScaleMode::OneD && !scaleUsingTypedFactor_) {
            const QPointF offset = cursorWorld_ - scaleBaseWorld_;
            guideEnd = scaleBaseWorld_ +
                       scaleAxisDirection_ *
                           QPointF::dotProduct(offset, scaleAxisDirection_);
        }

        painter.save();
        painter.setPen(QPen(QColor(QStringLiteral("#8aa7c7")), 1.0, Qt::DashLine));
        painter.drawLine(worldToScreen(scaleBaseWorld_), worldToScreen(guideEnd));
        painter.setPen(QPen(QColor(QStringLiteral("#e6b85c")), 1.5));
        painter.setBrush(QColor(QStringLiteral("#282828")));
        painter.drawEllipse(worldToScreen(scaleBaseWorld_), 4.0, 4.0);
        painter.restore();
    }

    void drawEraseCandidatePreview(QPainter &painter, int shapeIndex)
    {
        viewportOverlay_.drawEraseCandidatePreview(painter,
                                                   eraseTargetCurveCaches_,
                                                   shapeIndex,
                                                   !eraseStrokeScreenPath_.isEmpty());
    }

    void drawErasePreview(QPainter &painter)
    {
        viewportOverlay_.drawErasePreview(painter,
                                          activeTool_,
                                          eraseCursorScreen_,
                                          cursorValid_,
                                          eraseCursorPressed_,
                                          eraseCandidateShapeIndices_.size());
    }

    void drawShape(QPainter &painter,
                   const Shape &shape,
                   bool preview,
                   bool selected = false,
                   bool drawPreviewPoints = true,
                   const QColor &layerColor = QColor(),
                   const QString &layerLineType = QString(),
                   qreal layerLineWeightMm = 0.0)
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
                                    layerLineWeightMm);
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

    ToolInput makeToolInput(const QMouseEvent *event,
                            const QPointF &screenPosition,
                            const QPointF &rawWorldPosition,
                            const QPointF &worldPosition) const
    {
        ToolInput input;
        input.screenPosition = screenPosition;
        input.rawWorldPosition = rawWorldPosition;
        input.worldPosition = worldPosition;
        input.workPlaneFrame = viewportTransform_.workPlaneFrame();
        input.orthoEnabled = orthoEnabled_;
        input.viewportSize = size();
        input.snapResult = currentSnap_;
        input.snapType = currentSnap_.type;
        if (activeTool_ != Tool::Line && activeTool_ != Tool::PointEdgeCenter &&
            activeTool_ != Tool::PointByLine && activeTool_ != Tool::PointByArcs &&
            input.snapResult.isValid() &&
            input.snapResult.hasWorldPoint &&
            isValidWorkPlaneFrame(input.workPlaneFrame)) {
            const QPointF snapPosition = worldPointToWorkPlaneFrame(
                input.snapResult.worldPoint, input.workPlaneFrame);
            if (std::hypot(snapPosition.x() - worldPosition.x(),
                           snapPosition.y() - worldPosition.y()) > 1.0e-7) {
                // A tool constraint may project or redirect the acquired snap.
                // Keep that constrained preview point authoritative instead of
                // letting a consumer restore the unconstrained target.
                input.snapResult = SnapResult{};
            }
        }
        if (event != nullptr) {
            input.button = event->button();
            input.buttons = event->buttons();
            input.modifiers = event->modifiers();
        }
        return input;
    }

    ToolInput makeKeyToolInput(const QKeyEvent *event) const
    {
        ToolInput input;
        input.workPlaneFrame = viewportTransform_.workPlaneFrame();
        input.viewportSize = size();
        input.screenPosition = mapFromGlobal(QCursor::pos());
        if (!rect().contains(input.screenPosition.toPoint())) {
            input.screenPosition = lastMousePosition_;
        }
        if (!viewportTransform_.screenToWorkPlane(input.screenPosition,
                                                  size(),
                                                  input.workPlaneFrame,
                                                  &input.rawWorldPosition)) {
            input.rawWorldPosition = rawCursorWorld_;
        }
        input.worldPosition = input.rawWorldPosition;
        if (event != nullptr) {
            input.key = event->key();
            input.text = event->text();
            input.modifiers = event->modifiers();
        }
        return input;
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

        Shape result{geometryTypeForTool(tool),
                     points,
                     Shape::NurbsCurve2D{},
                     arcMode,
                     arcSweep,
                     {},
                     {}};
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
    ToolId repeatTool_ = ToolId::Select;
    ArcMode arcMode_ = ArcMode::OnePoint;
    bool arcPreviewInitialized_ = false;
    qreal arcPreviewPreviousAngle_ = 0.0;
    qreal arcPreviewSweepAngle_ = 0.0;
    bool arcAngleValueLocked_ = false;
    qreal arcPreviewStartAngle_ = 0.0;
    bool arcAngleSnapEnabled_ = true;
    bool arcPerpendicularPlaneActive_ = false;
    bool arcPlaneLocked_ = false;
    WorkPlaneFrame arcLockedFrame_;
    bool arcLockedFrameValid_ = false;
    ArcTextInputMode arcTextInputMode_ = ArcTextInputMode::None;
    QString arcTextInput_;
    WorkPlaneFrame arcReferenceFrame_;
    WorkPlaneFrame arcInputFrame_;
    WorkPlaneFrame arcAxisBaseFrame_;
    Point3D arcReferenceNormal_;
    Point3D arcFirstPointWorld_;
    Point3D arcSecondPointWorld_;
    Point3D arcResolvedChordPointWorld_;
    bool arcReferenceFrameValid_ = false;
    bool arcInputFrameValid_ = false;
    bool arcAxisBaseFrameValid_ = false;
    bool arcChordWorldPointsValid_ = false;
    bool arcResolvedChordPointValid_ = false;
    int arcAxisConstraintKey_ = 0;
    int arcPlaneNormalLockKey_ = 0;
    int arcVerticalOverrideAxis_ = 0;
    Point3D arcTwoPointPerpendicularNormal_;
    bool arcTwoPointPerpendicularNormalValid_ = false;
    bool arcWasVertical_ = false;
    bool controlPointsVisible_ = false;
    Document document_;
    SelectionModel selection_;
    History history_;
    ViewportTransform viewportTransform_;
    QVariantAnimation *navigationAnimation_ = nullptr;
    ViewportCameraState navigationAnimationStart_;
    ViewportCameraState navigationAnimationEnd_;
    QPointF navigationHoverPosition_{-1000.0, -1000.0};
    QPointF navigationPressPosition_;
    QPointF navigationLastPosition_;
    BlenderNavigationHit navigationPressHit_;
    BlenderNavigationAction navigationPressedAction_ = BlenderNavigationAction::None;
    bool navigationMoved_ = false;
    CurveSampler curveSampler_;
    CurveHitTester curveHitTester_;
    SnapEngine snapEngine_;
    ViewportRenderer viewportRenderer_;
    BlenderGridRenderer blenderGridRenderer_;
    ViewportGpuSurface *gpuSurface_ = nullptr;
    ViewportOverlay viewportOverlay_;
    BlenderGridAppearance gridAppearance_;
    ToolContext toolContext_;
    ToolRegistry toolRegistry_;
    InteractionTool *activeToolController_ = nullptr;
    ToolStatus toolStatus_;
    QString circleHudDimensionsLine_;
    QString circleHudInstructionsLine_;
    QString ellipseHudDimensionsLine_;
    QString ellipseHudInstructionsLine_;
    QString polygonHudDimensionsLine_;
    QString polygonHudInstructionsLine_;
    QString rectangleHudDimensionsLine_;
    QString rectangleHudInstructionsLine_;
    QVector<ToolPreviewGuide> rectanglePreviewGuides_;
    QVector<ToolPreviewGuide> ellipsePreviewGuides_;
    QVector<Shape> pointPreviewShapes_;
    QVector<ToolPreviewGuide> pointPreviewGuides_;
    QVector<Point3D> pointPreviewWorldPoints_;
    QString pointHudInstructionsLine_;
    int pointPreviewStage_ = -1;
    WorkPlaneFrame pointPreviewFrame_;
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
    Shape controllerPreviewShape_;
    WorkPlaneFrame linePreviewFrame_;
    QVector<Point3D> linePreviewWorldPoints_;
    Point3D linePreviewWorldCursor_;
    QVector<Shape> linePreviewShapes_;
    QVector<Shape> pointExtrudePreviewShapes_;
    bool linePreviewPlaneLocked_ = false;
    bool controllerPreviewShapeVisible_ = false;
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
    QVector<ObjectId> &selectedShapeIndices_;
    ObjectId &selectedShapeIndex_;
    bool draggingSelected_ = false;
    QVector<ObjectId> draggingShapeIndices_;
    bool draggingControlPoint_ = false;
    bool dragGestureStarted_ = false;
    int &controlPointIndex_;
    bool dragHistoryRecorded_ = false;
    bool dragSnapLocked_ = false;
    quint64 dragSnapTraceSequence_ = 0;
    DragAxisLock dragAxisLock_ = DragAxisLock::None;
    bool grabActive_ = false;
    bool grabMoved_ = false;
    bool grabPickingBasePoint_ = false;
    bool grabHasBasePoint_ = false;
    Document::Snapshot grabStartSnapshot_;
    QPointF grabStartWorld_{0.0, 0.0};
    QPointF grabBasePoint_{0.0, 0.0};
    QPointF grabCursorOffset_{0.0, 0.0};
    QPointF dragSnapCursorWorld_{0.0, 0.0};
    QPointF nearDragFreeSourcePoint_{0.0, 0.0};
    bool nearDragFreeSourcePointValid_ = false;
    QPointF dragStartScreen_{0.0, 0.0};
    QPointF lastDragWorld_{0.0, 0.0};
    QPointF lastControlPointWorld_{0.0, 0.0};
    bool duplicateActive_ = false;
    bool duplicatePickingBasePoint_ = false;
    bool duplicateHasBasePoint_ = false;
    QVector<SceneObject> duplicateSourceObjects_;
    QVector<Shape> duplicatePreviewShapes_;
    QPointF duplicateBasePoint_{0.0, 0.0};
    QPointF duplicateCursorOffset_{0.0, 0.0};
    QPointF duplicateDestination_{0.0, 0.0};
    bool selectionBoxActive_ = false;
    bool selectionBoxMoved_ = false;
    bool selectionBoxAdditive_ = false;
    bool trimBoxSelectionActive_ = false;
    QPointF selectionBoxStartScreen_{0.0, 0.0};
    QPointF selectionBoxCurrentScreen_{0.0, 0.0};
    bool eraseStrokeActive_ = false;
    bool eraseCursorPressed_ = false;
    QPointF eraseCursorScreen_{0.0, 0.0};
    QPointF lastEraseScreen_{0.0, 0.0};
    QVector<QPointF> eraseStrokeScreenPath_;
    QVector<ObjectId> eraseCandidateShapeIndices_;
    QVector<ObjectId> eraseTargetShapeIndices_;
    QVector<EraseCurveSampleCache> eraseSceneCurveCaches_;
    QVector<EraseCurveSampleCache> eraseTargetCurveCaches_;
    bool eraseGeometryCachePrepared_ = false;
    ViewportCameraState eraseCacheCameraState_;
    ViewportCameraPreferences eraseCacheCameraPreferences_;
    QSize eraseCacheViewportSize_;
    bool trimHoverPositionValid_ = false;
    int trimHoverComponentIndex_ = -1;
    QPointF trimHoverScreenPosition_{0.0, 0.0};
    QElapsedTimer trimHoverTimingWindow_;
    qint64 trimHoverWindowTotalUs_ = 0;
    qint64 trimHoverWindowMaxUs_ = 0;
    int trimHoverWindowEvents_ = 0;
    qreal &zoom_;
    bool panning_ = false;
    bool orbiting_ = false;
    bool panMoved_ = false;
    QPoint panStartPosition_;
    Qt::MouseButton panButton_ = Qt::MiddleButton;
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
    bool subdivisionActive_ = false;
    ObjectId subdivisionShapeIndex_ = ObjectId::invalid();
    int subdivisionSections_ = 2;
    int subdivisionWheelAccumulator_ = 0;
    qreal subdivisionPixelAccumulator_ = 0.0;
    bool joinActive_ = false;
    QVector<ObjectId> joinShapeIndices_;
    QVector<ObjectId> rotateShapeIndices_;
    QVector<ObjectId> scaleShapeIds_;
    QVector<ObjectId> mirrorShapeIndices_;
    int rotateStep_ = 0;
    WorkPlaneFrame rotateFrame_;
    WorkPlaneFrame rotatePrimaryFrame_;
    WorkPlaneFrame rotatePrePivotPlaneFrame_;
    WorkPlaneFrame rotatePrePivotFloorFrame_;
    RotateToolPreferences rotateToolPreferences_;
    Point3D rotateBaseWorldPoint_;
    Point3D rotateReferenceWorldPoint_;
    Point3D rotateReferenceNormal_;
    Point3D rotatePrePivotFloorNormal_;
    QPointF rotateBaseWorld_{0.0, 0.0};
    QPointF rotateReferenceWorld_{0.0, 0.0};
    qreal rotatePreviewAngle_ = 0.0;
    qreal rotateAccumulatedAngle_ = 0.0;
    qreal rotateLastRawAngle_ = 0.0;
    qreal rotateReferenceAngle_ = 0.0;
    bool rotateHasPreviousAngle_ = false;
    bool rotateAngleSnapEnabled_ = true;
    bool rotateAngleInputActive_ = false;
    bool rotateAngleInputManual_ = false;
    QString rotateAngleInput_;
    qreal rotateAngleInputDirection_ = 1.0;
    bool rotateAngleInputDirectionCaptured_ = false;
    QPointF rotateLastPointerPoint_;
    bool rotatePerpendicularActive_ = false;
    bool rotatePrePivotPerpendicularActive_ = false;
    int rotateAxisLockKey_ = 0;
    ScaleMode scaleMode_ = ScaleMode::TwoD;
    int scaleStep_ = 0;
    QPointF scaleBaseWorld_{0.0, 0.0};
    QPointF scaleReferenceWorld_{0.0, 0.0};
    QPointF scaleAxisDirection_{0.0, 0.0};
    qreal scaleReferenceLength_ = 0.0;
    bool scaleUsingTypedFactor_ = false;
    qreal scaleTypedFactor_ = 1.0;
    QString scaleFactorText_;
    qreal scalePreviewFactor_ = 1.0;
    QPointF scalePreviewAxis_{0.0, 0.0};
    bool scalePreviewValid_ = false;
};

ViewportWidgetApi *createViewportWidget(QWidget *parent)
{
    return new ViewportWidget(parent);
}

} // namespace classiCAD
