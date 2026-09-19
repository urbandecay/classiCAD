#include <QApplication>
#include <QAction>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QCursor>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFrame>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QInputDialog>
#include <QListWidget>
#include <QMenu>
#include <QMainWindow>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QPushButton>
#include <QProcess>
#include <QSettings>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTextStream>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVector>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <functional>

namespace {

enum class Tool {
    Select,
    Line,
    Arc,
    Bezier,
    Nurbs,
    Rectangle,
    Circle,
    // Keep Point at the end so existing saved sessions keep their tool IDs.
    Point,
};

enum class ArcMode {
    OnePoint,
    TwoPoint,
};

enum class SnapType {
    None,
    Endpoint,
    Midpoint,
    Intersection,
    Center,
    Perpendicular,
    Tangent,
};

struct SnapResult {
    SnapType type = SnapType::None;
    QPointF point;

    bool isValid() const
    {
        return type != SnapType::None;
    }
};

struct SnapCandidate {
    SnapType type = SnapType::None;
    QPointF point;
};

struct LineSegment {
    QPointF start;
    QPointF end;
};

struct DragSnapResult {
    SnapType type = SnapType::None;
    QPointF sourcePoint;
    QPointF targetPoint;
    QPointF translation;

    bool isValid() const
    {
        return type != SnapType::None;
    }
};

struct Shape {
    Tool tool;
    QVector<QPointF> points;
    struct NurbsCurve2D {
        // Mirrors the core ON_NurbsCurve fields. Rhino stores rational CVs
        // as homogeneous values; this lightweight model keeps Euclidean CVs
        // plus their weights and converts to homogeneous form at export.
        int dimension = 2;
        int degree = 1;
        int order = 2;
        bool rational = false;
        QVector<QPointF> controlPoints;
        QVector<double> weights;
        // Rhino/openNURBS knot convention: the two superfluous end knots
        // from the mathematical full vector are not stored here.
        QVector<double> knots;
    } nurbs;
    ArcMode arcMode = ArcMode::TwoPoint;
    qreal arcSweep = 0.0;
    // Interior subdivision locations are stored in the source curve's
    // parameter domain. They are markers, not new curve spans or control
    // vertices, so the original NURBS remains unchanged.
    QVector<double> subdivisionParameters;
};

Shape::NurbsCurve2D makeDegreeOneNurbs(const QVector<QPointF> &points)
{
    Shape::NurbsCurve2D curve;
    curve.dimension = 2;
    curve.degree = 1;
    curve.order = 2;
    curve.rational = false;
    curve.controlPoints = points;
    curve.weights.fill(1.0, points.size());

    if (points.size() < 2) {
        return curve;
    }

    // An open, clamped degree-1 curve represents the same connected
    // segments as a Rhino-style polyline while remaining NURBS data.
    const int pointCount = points.size();
    curve.knots.reserve(pointCount);
    for (int i = 0; i < pointCount; ++i) {
        curve.knots.append(static_cast<double>(i));
    }

    return curve;
}

Shape::NurbsCurve2D makeBezierNurbs(const QVector<QPointF> &points)
{
    Shape::NurbsCurve2D curve;
    curve.dimension = 2;
    curve.controlPoints = points;
    curve.weights.fill(1.0, points.size());

    if (points.size() < 2) {
        return curve;
    }

    curve.degree = points.size() - 1;
    curve.order = curve.degree + 1;
    curve.rational = false;

    // A single Bezier span is a clamped NURBS with degree repeated at each
    // end. Store the Rhino/openNURBS knot array without the two redundant
    // outer entries.
    curve.knots.reserve(curve.controlPoints.size() + curve.degree - 1);
    for (int index = 0; index < curve.degree; ++index) {
        curve.knots.append(0.0);
    }
    for (int index = 0; index < curve.degree; ++index) {
        curve.knots.append(1.0);
    }

    return curve;
}

Shape::NurbsCurve2D makeCircleNurbs(const QVector<QPointF> &points)
{
    Shape::NurbsCurve2D curve;
    curve.dimension = 2;
    curve.degree = 2;
    curve.order = 3;
    curve.rational = true;

    if (points.size() < 2) {
        return curve;
    }

    constexpr qreal pi = 3.14159265358979323846;
    constexpr qreal halfPi = pi / 2.0;
    const QPointF center = points[0];
    const QPointF edge = points[1];
    const qreal radius = std::hypot(edge.x() - center.x(), edge.y() - center.y());
    if (radius <= 1e-9) {
        return curve;
    }

    const qreal startAngle = std::atan2(edge.y() - center.y(), edge.x() - center.x());
    const qreal middleWeight = std::cos(pi / 4.0);
    const qreal middleRadius = radius / middleWeight;

    curve.controlPoints.reserve(9);
    curve.weights.reserve(9);
    for (int span = 0; span < 4; ++span) {
        const qreal spanStart = startAngle + halfPi * span;
        const qreal spanEnd = spanStart + halfPi;
        const qreal spanMiddle = (spanStart + spanEnd) * 0.5;
        const auto circlePoint = [center, radius](qreal angle) {
            return QPointF(center.x() + radius * std::cos(angle),
                           center.y() + radius * std::sin(angle));
        };

        if (span == 0) {
            curve.controlPoints.append(circlePoint(spanStart));
            curve.weights.append(1.0);
        }
        curve.controlPoints.append(QPointF(
            center.x() + middleRadius * std::cos(spanMiddle),
            center.y() + middleRadius * std::sin(spanMiddle)));
        curve.weights.append(middleWeight);
        curve.controlPoints.append(circlePoint(spanEnd));
        curve.weights.append(1.0);
    }

    // This is the same reduced knot array used by Rhino's documented
    // degree-2 rational NURBS circle construction.
    curve.knots = {0.0,
                   0.0,
                   halfPi,
                   halfPi,
                   pi,
                   pi,
                   3.0 * halfPi,
                   3.0 * halfPi,
                   2.0 * pi,
                   2.0 * pi};
    return curve;
}

qreal crossProduct(const QPointF &a, const QPointF &b)
{
    return a.x() * b.y() - a.y() * b.x();
}

bool segmentIntersection(const QPointF &a,
                         const QPointF &b,
                         const QPointF &c,
                         const QPointF &d,
                         QPointF *intersection)
{
    const QPointF firstDirection = b - a;
    const QPointF secondDirection = d - c;
    const qreal denominator = crossProduct(firstDirection, secondDirection);

    if (std::abs(denominator) < 1e-9) {
        return false;
    }

    const QPointF betweenStarts = c - a;
    const qreal firstParameter = crossProduct(betweenStarts, secondDirection) / denominator;
    const qreal secondParameter = crossProduct(betweenStarts, firstDirection) / denominator;
    constexpr qreal tolerance = 1e-9;

    if (firstParameter < -tolerance || firstParameter > 1.0 + tolerance ||
        secondParameter < -tolerance || secondParameter > 1.0 + tolerance) {
        return false;
    }

    if (intersection != nullptr) {
        *intersection = a + firstDirection * firstParameter;
    }
    return true;
}

QPointF eventPosition(const QMouseEvent *event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return event->position();
#else
    return event->localPos();
#endif
}

QPointF eventPosition(const QWheelEvent *event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return event->position();
#else
    return event->posF();
#endif
}

QString mouseButtonName(Qt::MouseButton button)
{
    return button == Qt::RightButton ? QStringLiteral("RMB") : QStringLiteral("MMB");
}

QString inputButtonName(Qt::MouseButton button)
{
    switch (button) {
    case Qt::LeftButton:
        return QStringLiteral("LMB");
    case Qt::MiddleButton:
        return QStringLiteral("MMB");
    case Qt::RightButton:
        return QStringLiteral("RMB");
    case Qt::BackButton:
        return QStringLiteral("Back");
    case Qt::ForwardButton:
        return QStringLiteral("Forward");
    default:
        return QStringLiteral("Button(%1)").arg(static_cast<int>(button));
    }
}

QString pointText(const QPointF &point)
{
    return QStringLiteral("(%1, %2)")
        .arg(point.x(), 0, 'f', 3)
        .arg(point.y(), 0, 'f', 3);
}

QJsonObject pointToJson(const QPointF &point)
{
    QJsonObject object;
    object.insert(QStringLiteral("x"), point.x());
    object.insert(QStringLiteral("y"), point.y());
    return object;
}

bool pointFromJson(const QJsonValue &value, QPointF *point)
{
    if (point == nullptr || !value.isObject()) {
        return false;
    }

    const QJsonObject object = value.toObject();
    const QJsonValue xValue = object.value(QStringLiteral("x"));
    const QJsonValue yValue = object.value(QStringLiteral("y"));
    if (!xValue.isDouble() || !yValue.isDouble()) {
        return false;
    }

    const qreal x = xValue.toDouble();
    const qreal y = yValue.toDouble();
    if (!std::isfinite(x) || !std::isfinite(y)) {
        return false;
    }

    *point = QPointF(x, y);
    return true;
}

QJsonArray pointsToJson(const QVector<QPointF> &points)
{
    QJsonArray array;
    for (const QPointF &point : points) {
        array.append(pointToJson(point));
    }
    return array;
}

bool pointsFromJson(const QJsonValue &value, QVector<QPointF> *points)
{
    if (points == nullptr || !value.isArray()) {
        return false;
    }

    QVector<QPointF> restoredPoints;
    const QJsonArray array = value.toArray();
    restoredPoints.reserve(array.size());
    for (const QJsonValue &pointValue : array) {
        QPointF point;
        if (!pointFromJson(pointValue, &point)) {
            return false;
        }
        restoredPoints.append(point);
    }

    *points = restoredPoints;
    return true;
}

QJsonObject nurbsToJson(const Shape::NurbsCurve2D &curve)
{
    QJsonObject object;
    object.insert(QStringLiteral("dimension"), curve.dimension);
    object.insert(QStringLiteral("degree"), curve.degree);
    object.insert(QStringLiteral("order"), curve.order);
    object.insert(QStringLiteral("rational"), curve.rational);
    object.insert(QStringLiteral("controlPoints"), pointsToJson(curve.controlPoints));

    QJsonArray weights;
    for (const double weight : curve.weights) {
        weights.append(weight);
    }
    object.insert(QStringLiteral("weights"), weights);

    QJsonArray knots;
    for (const double knot : curve.knots) {
        knots.append(knot);
    }
    object.insert(QStringLiteral("knots"), knots);
    return object;
}

bool nurbsFromJson(const QJsonValue &value, Shape::NurbsCurve2D *curve)
{
    if (curve == nullptr || !value.isObject()) {
        return false;
    }

    const QJsonObject object = value.toObject();
    QVector<QPointF> controlPoints;
    if (!pointsFromJson(object.value(QStringLiteral("controlPoints")), &controlPoints)) {
        return false;
    }

    QVector<double> weights;
    const QJsonValue weightsValue = object.value(QStringLiteral("weights"));
    if (!weightsValue.isArray()) {
        return false;
    }
    for (const QJsonValue &weightValue : weightsValue.toArray()) {
        if (!weightValue.isDouble() || !std::isfinite(weightValue.toDouble())) {
            return false;
        }
        weights.append(weightValue.toDouble());
    }

    QVector<double> knots;
    const QJsonValue knotsValue = object.value(QStringLiteral("knots"));
    if (!knotsValue.isArray()) {
        return false;
    }
    for (const QJsonValue &knotValue : knotsValue.toArray()) {
        if (!knotValue.isDouble() || !std::isfinite(knotValue.toDouble())) {
            return false;
        }
        knots.append(knotValue.toDouble());
    }

    curve->dimension = object.value(QStringLiteral("dimension")).toInt(2);
    curve->degree = object.value(QStringLiteral("degree")).toInt(1);
    curve->order = object.value(QStringLiteral("order")).toInt(2);
    curve->rational = object.value(QStringLiteral("rational")).toBool(false);
    curve->controlPoints = controlPoints;
    curve->weights = weights;
    curve->knots = knots;
    return true;
}

QJsonObject shapeToJson(const Shape &shape)
{
    QJsonObject object;
    object.insert(QStringLiteral("tool"), static_cast<int>(shape.tool));
    object.insert(QStringLiteral("points"), pointsToJson(shape.points));
    object.insert(QStringLiteral("nurbs"), nurbsToJson(shape.nurbs));
    object.insert(QStringLiteral("arcMode"), static_cast<int>(shape.arcMode));
    object.insert(QStringLiteral("arcSweep"), shape.arcSweep);

    QJsonArray subdivisionParameters;
    for (const double parameter : shape.subdivisionParameters) {
        subdivisionParameters.append(parameter);
    }
    object.insert(QStringLiteral("subdivisionParameters"), subdivisionParameters);
    return object;
}

bool shapeFromJson(const QJsonValue &value, Shape *shape)
{
    if (shape == nullptr || !value.isObject()) {
        return false;
    }

    const QJsonObject object = value.toObject();
    const int toolValue = object.value(QStringLiteral("tool")).toInt(-1);
    const int arcModeValue = object.value(QStringLiteral("arcMode")).toInt(-1);
    if (toolValue < static_cast<int>(Tool::Select) ||
        toolValue > static_cast<int>(Tool::Point) ||
        arcModeValue < static_cast<int>(ArcMode::OnePoint) ||
        arcModeValue > static_cast<int>(ArcMode::TwoPoint)) {
        return false;
    }

    QVector<QPointF> points;
    if (!pointsFromJson(object.value(QStringLiteral("points")), &points)) {
        return false;
    }

    Shape::NurbsCurve2D nurbs;
    if (!nurbsFromJson(object.value(QStringLiteral("nurbs")), &nurbs)) {
        return false;
    }

    const QJsonValue arcSweepValue = object.value(QStringLiteral("arcSweep"));
    if (!arcSweepValue.isDouble() || !std::isfinite(arcSweepValue.toDouble())) {
        return false;
    }

    QVector<double> subdivisionParameters;
    const QJsonValue subdivisionValue = object.value(QStringLiteral("subdivisionParameters"));
    if (!subdivisionValue.isUndefined()) {
        if (!subdivisionValue.isArray()) {
            return false;
        }
        for (const QJsonValue &parameterValue : subdivisionValue.toArray()) {
            if (!parameterValue.isDouble() || !std::isfinite(parameterValue.toDouble())) {
                return false;
            }
            subdivisionParameters.append(parameterValue.toDouble());
        }
    }

    shape->tool = static_cast<Tool>(toolValue);
    shape->points = points;
    shape->nurbs = nurbs;
    shape->arcMode = static_cast<ArcMode>(arcModeValue);
    shape->arcSweep = arcSweepValue.toDouble();
    shape->subdivisionParameters = subdivisionParameters;
    return true;
}

class DebugLog final {
public:
    static DebugLog &instance()
    {
        static DebugLog logger;
        return logger;
    }

    void write(const QString &message)
    {
        const QString line = QStringLiteral("[%1] %2")
                                 .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs))
                                 .arg(message);

        if (file_.isOpen()) {
            QTextStream stream(&file_);
            stream << line << Qt::endl;
            file_.flush();
        }

        qInfo().noquote() << line;
    }

    QString path() const
    {
        return file_.fileName();
    }

private:
    DebugLog()
    {
        file_.setFileName(QStringLiteral("/tmp/classiCAD.log"));
        file_.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
    }

    QFile file_;
};

QString toolName(Tool tool)
{
    switch (tool) {
    case Tool::Select:
        return QStringLiteral("Select");
    case Tool::Line:
        return QStringLiteral("Line");
    case Tool::Arc:
        return QStringLiteral("Arc");
    case Tool::Bezier:
        return QStringLiteral("Bezier");
    case Tool::Nurbs:
        return QStringLiteral("NURBS");
    case Tool::Rectangle:
        return QStringLiteral("Rectangle");
    case Tool::Circle:
        return QStringLiteral("Circle");
    case Tool::Point:
        return QStringLiteral("Point");
    }

    return QStringLiteral("Unknown");
}

QString arcModeName(ArcMode mode)
{
    switch (mode) {
    case ArcMode::OnePoint:
        return QStringLiteral("1 Point Arc");
    case ArcMode::TwoPoint:
        return QStringLiteral("2 Point Arc");
    }

    return QStringLiteral("Arc");
}

QString snapTypeName(SnapType type)
{
    switch (type) {
    case SnapType::Endpoint:
        return QStringLiteral("Endpoint");
    case SnapType::Midpoint:
        return QStringLiteral("Midpoint");
    case SnapType::Intersection:
        return QStringLiteral("Intersection");
    case SnapType::Center:
        return QStringLiteral("Center");
    case SnapType::Perpendicular:
        return QStringLiteral("Perpendicular");
    case SnapType::Tangent:
        return QStringLiteral("Tangent");
    case SnapType::None:
        return QStringLiteral("None");
    }

    return QStringLiteral("None");
}

int requiredPoints(Tool tool)
{
    switch (tool) {
    case Tool::Line:
        return 2;
    case Tool::Arc:
        return 3;
    case Tool::Bezier:
    case Tool::Nurbs:
        return 4;
    case Tool::Rectangle:
    case Tool::Circle:
        return 2;
    case Tool::Point:
        return 1;
    case Tool::Select:
        return 0;
    }

    return 0;
}

class ViewportWidget final : public QWidget {
public:
    explicit ViewportWidget(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setMinimumSize(480, 320);
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::CrossCursor);
        DebugLog::instance().write(QStringLiteral("viewport constructed"));
    }

    void setTool(Tool tool)
    {
        DebugLog::instance().write(QStringLiteral("setTool requested=%1 previous=%2")
                                       .arg(toolName(tool), toolName(activeTool_)));
        if (subdivisionActive_ && tool != Tool::Select) {
            cancelSubdivisionPreview();
        }
        activeTool_ = tool;
        pendingPoints_.clear();
        resetArcPreviewTracking();
        lineCommandActive_ = tool == Tool::Line;

        if (tool != Tool::Select) {
            repeatTool_ = tool;
            selectedShapeIndex_ = -1;
            draggingSelected_ = false;
            draggingControlPoint_ = false;
            controlPointIndex_ = -1;
            dragHistoryRecorded_ = false;
            currentDragSnap_ = DragSnapResult{};
            dragSnapLocked_ = false;
        }

        if (lineCommandActive_) {
            const QPoint localCursor = mapFromGlobal(QCursor::pos());
            if (rect().contains(localCursor)) {
                rawCursorWorld_ = screenToWorld(localCursor);
                cursorValid_ = true;
            }
        }

        refreshCursorConstraint();

        if (tool == Tool::Select) {
            setCursor(Qt::ArrowCursor);
        } else {
            setCursor(Qt::CrossCursor);
        }

        DebugLog::instance().write(QStringLiteral("setTool applied=%1 lineCommandActive=%2 cursorValid=%3 cursorWorld=%4")
                                       .arg(toolName(activeTool_))
                                       .arg(lineCommandActive_)
                                       .arg(cursorValid_)
                                       .arg(pointText(cursorWorld_)));
        update();
    }

    bool canUndo() const
    {
        return !undoStack_.isEmpty();
    }

    bool canRedo() const
    {
        return !redoStack_.isEmpty();
    }

    void undo()
    {
        if (undoStack_.isEmpty()) {
            DebugLog::instance().write(QStringLiteral("undo ignored empty-history"));
            return;
        }

        redoStack_.append(shapes_);
        shapes_ = undoStack_.takeLast();
        resetInteractionAfterHistory();
        notifyHistoryChanged();
        DebugLog::instance().write(QStringLiteral("undo applied shapes=%1 undoRemaining=%2 redoAvailable=%3")
                                       .arg(shapes_.size())
                                       .arg(undoStack_.size())
                                       .arg(redoStack_.size()));
    }

    void redo()
    {
        if (redoStack_.isEmpty()) {
            DebugLog::instance().write(QStringLiteral("redo ignored empty-history"));
            return;
        }

        undoStack_.append(shapes_);
        shapes_ = redoStack_.takeLast();
        resetInteractionAfterHistory();
        notifyHistoryChanged();
        DebugLog::instance().write(QStringLiteral("redo applied shapes=%1 undoAvailable=%2 redoRemaining=%3")
                                       .arg(shapes_.size())
                                       .arg(undoStack_.size())
                                       .arg(redoStack_.size()));
    }

    void repeatLastTool()
    {
        if (repeatTool_ == Tool::Select) {
            DebugLog::instance().write(QStringLiteral("repeatTool ignored no-last-tool"));
            return;
        }

        const Tool tool = repeatTool_;
        DebugLog::instance().write(QStringLiteral("repeatTool tool=%1")
                                       .arg(toolName(tool)));
        setTool(tool);
        if (toolRepeated_) {
            toolRepeated_(tool);
        }
    }

    Tool activeTool() const
    {
        return activeTool_;
    }

    void setArcMode(ArcMode mode)
    {
        arcMode_ = mode;
        pendingPoints_.clear();
        resetArcPreviewTracking();
        currentSnap_ = SnapResult{};
        DebugLog::instance().write(QStringLiteral("setArcMode mode=%1")
                                       .arg(arcModeName(arcMode_)));
        update();
    }

    ArcMode arcMode() const
    {
        return arcMode_;
    }

    void setControlPointsVisible(bool visible)
    {
        controlPointsVisible_ = visible;
        if (!visible) {
            draggingControlPoint_ = false;
            controlPointIndex_ = -1;
            dragHistoryRecorded_ = false;
        }
        DebugLog::instance().write(QStringLiteral("setControlPointsVisible=%1 selectedShape=%2")
                                       .arg(controlPointsVisible_)
                                       .arg(selectedShapeIndex_));
        update();
    }

    bool controlPointsVisible() const
    {
        return controlPointsVisible_;
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
                      bool tangent)
    {
        endpointSnapEnabled_ = endpoint;
        midpointSnapEnabled_ = midpoint;
        intersectionSnapEnabled_ = intersection;
        centerSnapEnabled_ = center;
        perpendicularSnapEnabled_ = perpendicular;
        tangentSnapEnabled_ = tangent;
        refreshCursorConstraint();
        DebugLog::instance().write(QStringLiteral("setSnapModes endpoint=%1 midpoint=%2 intersection=%3 center=%4 perpendicular=%5 tangent=%6 snap=%7")
                                       .arg(endpointSnapEnabled_)
                                       .arg(midpointSnapEnabled_)
                                       .arg(intersectionSnapEnabled_)
                                       .arg(centerSnapEnabled_)
                                       .arg(perpendicularSnapEnabled_)
                                       .arg(tangentSnapEnabled_)
                                       .arg(snapTypeName(currentSnap_.type)));
        update();
    }

    QString coordinateText() const
    {
        return QStringLiteral("X %1   Y %2   Zoom %3%")
            .arg(lastWorldPosition_.x(), 0, 'f', 2)
            .arg(lastWorldPosition_.y(), 0, 'f', 2)
            .arg(zoom_ * 100.0, 0, 'f', 0);
    }

    bool beginSubdivisionWheelMode()
    {
        if (selectedShapeIndex_ < 0 || selectedShapeIndex_ >= shapes_.size() ||
            !isSubdividableShape(shapes_[selectedShapeIndex_])) {
            DebugLog::instance().write(QStringLiteral("beginSubdivisionWheelMode ignored selectedShape=%1")
                                           .arg(selectedShapeIndex_));
            return false;
        }

        subdivisionShapeIndex_ = selectedShapeIndex_;
        subdivisionSections_ = std::clamp(
            static_cast<int>(shapes_[subdivisionShapeIndex_].subdivisionParameters.size()) + 1,
            2,
            maxSubdivisionSections);
        resetSubdivisionWheelTracking();
        subdivisionActive_ = true;
        setFocus(Qt::OtherFocusReason);
        notifySubdivisionStatus();
        update();
        DebugLog::instance().write(QStringLiteral("beginSubdivisionWheelMode shape=%1 sections=%2")
                                       .arg(subdivisionShapeIndex_)
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

        const int shapeIndex = subdivisionActive_ ? subdivisionShapeIndex_ : selectedShapeIndex_;
        if (shapeIndex < 0 || shapeIndex >= shapes_.size() ||
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
        subdivisionShapeIndex_ = -1;
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

    bool saveUpdateSession(const QString &path) const
    {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            DebugLog::instance().write(QStringLiteral("saveUpdateSession failed path=%1 error=%2")
                                           .arg(path, file.errorString()));
            return false;
        }

        QJsonObject root;
        root.insert(QStringLiteral("version"), 1);
        root.insert(QStringLiteral("zoom"), zoom_);
        root.insert(QStringLiteral("pan"), pointToJson(pan_));

        QJsonArray shapes;
        for (const Shape &shape : shapes_) {
            shapes.append(shapeToJson(shape));
        }
        root.insert(QStringLiteral("shapes"), shapes);

        const QByteArray data = QJsonDocument(root).toJson(QJsonDocument::Compact);
        if (file.write(data) != data.size()) {
            DebugLog::instance().write(QStringLiteral("saveUpdateSession write failed path=%1 error=%2")
                                           .arg(path, file.errorString()));
            return false;
        }

        DebugLog::instance().write(QStringLiteral("saveUpdateSession path=%1 shapes=%2")
                                       .arg(path)
                                       .arg(shapes_.size()));
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
        if (version != 1) {
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
            Shape shape{Tool::Select, {}, Shape::NurbsCurve2D{}, ArcMode::TwoPoint, 0.0, {}};
            if (!shapeFromJson(shapeValue, &shape)) {
                DebugLog::instance().write(QStringLiteral("restoreUpdateSession invalid shape path=%1")
                                               .arg(path));
                return false;
            }
            restoredShapes.append(shape);
        }

        shapes_ = restoredShapes;
        undoStack_.clear();
        redoStack_.clear();
        pendingPoints_.clear();
        resetArcPreviewTracking();
        selectedShapeIndex_ = -1;
        draggingSelected_ = false;
        draggingControlPoint_ = false;
        controlPointIndex_ = -1;
        dragHistoryRecorded_ = false;
        currentDragSnap_ = DragSnapResult{};
        dragSnapLocked_ = false;
        currentSnap_ = SnapResult{};
        subdivisionActive_ = false;
        subdivisionShapeIndex_ = -1;
        subdivisionSections_ = 2;
        resetSubdivisionWheelTracking();
        lineCommandActive_ = false;
        activeTool_ = Tool::Select;
        repeatTool_ = Tool::Select;
        pan_ = restoredPan;
        zoom_ = restoredZoom;
        setCursor(Qt::ArrowCursor);
        update();
        emitCoordinateUpdate();
        notifyHistoryChanged();

        DebugLog::instance().write(QStringLiteral("restoreUpdateSession path=%1 shapes=%2")
                                       .arg(path)
                                       .arg(shapes_.size()));
        return true;
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.fillRect(rect(), QColor(QStringLiteral("#282828")));

        drawGrid(painter);
        drawOrigin(painter);

        for (int index = 0; index < shapes_.size(); ++index) {
            drawShape(painter, shapes_[index], false, index == selectedShapeIndex_);
            if (!subdivisionActive_ || index != subdivisionShapeIndex_) {
                drawSubdivisionPoints(painter,
                                      shapes_[index],
                                      shapes_[index].subdivisionParameters,
                                      false);
            }
        }

        if (subdivisionActive_ && subdivisionShapeIndex_ >= 0 &&
            subdivisionShapeIndex_ < shapes_.size()) {
            const QVector<double> previewParameters = subdivisionParametersForSections(
                shapes_[subdivisionShapeIndex_], subdivisionSections_);
            drawSubdivisionPoints(painter,
                                  shapes_[subdivisionShapeIndex_],
                                  previewParameters,
                                  true);
        }

        if (controlPointsVisible_ && selectedShapeIndex_ >= 0 &&
            selectedShapeIndex_ < shapes_.size()) {
            drawControlPoints(painter, shapes_[selectedShapeIndex_]);
        }

        if (activeTool_ == Tool::Line && lineCommandActive_) {
            drawLineToolPreview(painter);
        } else if (activeTool_ == Tool::Arc) {
            drawArcToolPreview(painter);
        } else if (activeTool_ == Tool::Circle && !pendingPoints_.isEmpty()) {
            drawCircleToolPreview(painter);
        } else if (activeTool_ == Tool::Rectangle && !pendingPoints_.isEmpty()) {
            drawRectangleToolPreview(painter);
        } else if (activeTool_ == Tool::Point) {
            drawPointToolPreview(painter);
        } else if (!pendingPoints_.isEmpty()) {
            drawShape(painter,
                      Shape{activeTool_, pendingPoints_, Shape::NurbsCurve2D{}, ArcMode::TwoPoint, 0.0, {}},
                      true);
        }

        if (draggingSelected_ && currentDragSnap_.isValid()) {
            drawSnapMarker(painter,
                           currentDragSnap_.type,
                           currentDragSnap_.targetPoint);
        }

        painter.setPen(QColor(QStringLiteral("#a0a0a0")));
        painter.setFont(QFont(QStringLiteral("Sans"), 10));
        const QString activeToolLabel = activeTool_ == Tool::Arc
                                            ? arcModeName(arcMode_)
                                            : toolName(activeTool_);
        painter.drawText(18,
                         28,
                         QStringLiteral("2D VIEWPORT  •  %1").arg(activeToolLabel));

        if (subdivisionActive_) {
            painter.setPen(QColor(QStringLiteral("#f0a45a")));
            painter.drawText(18,
                             height() - 42,
                             QStringLiteral("SUBDIVIDE  •  %1 sections  •  endpoints included")
                                 .arg(subdivisionSections_));
        }

        if (activeTool_ == Tool::Line && lineCommandActive_) {
            painter.setPen(QColor(QStringLiteral("#777777")));
            painter.drawText(18,
                             height() - 18,
                             QStringLiteral("Click to place connected points  •  Right-click to finish"));
        } else if (activeTool_ != Tool::Select) {
            QString hint;
            if (activeTool_ == Tool::Arc) {
                const QString points = arcMode_ == ArcMode::OnePoint
                                            ? QStringLiteral("center, start, endpoint")
                                            : QStringLiteral("start, end, through point");
                hint = QStringLiteral("Click to place %1  •  Esc clears current tool input")
                           .arg(points);
            } else {
                hint = QStringLiteral("Click to place %1 point%2  •  Esc clears current tool input")
                           .arg(toolName(activeTool_).toLower())
                           .arg(requiredPoints(activeTool_) == 1 ? QString() : QStringLiteral("s"));
            }
            painter.setPen(QColor(QStringLiteral("#777777")));
            painter.drawText(18, height() - 18, hint);
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        const QPointF screenPosition = eventPosition(event);
        const QPointF rawWorldPosition = screenToWorld(screenPosition);
        const QPointF worldPosition = constrainLinePoint(rawWorldPosition);
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
            finishLineCommand();
            return;
        }

        if (event->button() == panButton_ ||
            (event->button() == Qt::LeftButton && event->modifiers().testFlag(Qt::AltModifier))) {
            panning_ = true;
            panMoved_ = false;
            panStartPosition_ = screenPosition.toPoint();
            lastMousePosition_ = screenPosition.toPoint();
            DebugLog::instance().write(QStringLiteral("mousePress branch=start-pan at=%1")
                                           .arg(pointText(screenPosition)));
            setCursor(Qt::ClosedHandCursor);
            return;
        }

        if (event->button() == Qt::LeftButton && activeTool_ == Tool::Select) {
            rawCursorWorld_ = rawWorldPosition;
            cursorWorld_ = rawWorldPosition;
            lastWorldPosition_ = rawWorldPosition;
            cursorValid_ = true;

            if (controlPointsVisible_ && selectedShapeIndex_ >= 0 &&
                selectedShapeIndex_ < shapes_.size()) {
                const int grabbedControlPoint =
                    hitTestControlPoint(selectedShapeIndex_, screenPosition);
                if (grabbedControlPoint >= 0) {
                    draggingControlPoint_ = true;
                    draggingSelected_ = false;
                    controlPointIndex_ = grabbedControlPoint;
                    lastControlPointWorld_ = rawWorldPosition;
                    dragHistoryRecorded_ = false;
                    currentDragSnap_ = DragSnapResult{};
                    dragSnapLocked_ = false;
                    setCursor(Qt::SizeAllCursor);
                    DebugLog::instance().write(
                        QStringLiteral("control point drag start shape=%1 index=%2 world=%3")
                            .arg(selectedShapeIndex_)
                            .arg(controlPointIndex_)
                            .arg(pointText(lastControlPointWorld_)));
                    update();
                    emitCoordinateUpdate();
                    return;
                }
            }

            selectedShapeIndex_ = hitTestShape(screenPosition);
            draggingSelected_ = selectedShapeIndex_ >= 0;
            draggingControlPoint_ = false;
            controlPointIndex_ = -1;
            dragHistoryRecorded_ = false;
            currentDragSnap_ = DragSnapResult{};
            dragSnapLocked_ = false;

            if (draggingSelected_) {
                lastDragWorld_ = rawWorldPosition;
                setCursor(Qt::SizeAllCursor);
                DebugLog::instance().write(
                    QStringLiteral("selection hit shape=%1 tool=%2 dragStart=%3")
                        .arg(selectedShapeIndex_)
                        .arg(toolName(shapes_[selectedShapeIndex_].tool))
                        .arg(pointText(lastDragWorld_)));
            } else {
                DebugLog::instance().write(QStringLiteral("selection miss at=%1")
                                               .arg(pointText(screenPosition)));
            }

            update();
            emitCoordinateUpdate();
            return;
        }

        if (event->button() != Qt::LeftButton || activeTool_ == Tool::Select) {
            DebugLog::instance().write(QStringLiteral("mousePress branch=ignored"));
            return;
        }

        rawCursorWorld_ = rawWorldPosition;
        lastWorldPosition_ = worldPosition;
        cursorWorld_ = worldPosition;
        cursorValid_ = true;

        if (activeTool_ == Tool::Line) {
            pendingPoints_.append(lastWorldPosition_);
            DebugLog::instance().write(QStringLiteral("line point planted index=%1 world=%2 total=%3")
                                           .arg(pendingPoints_.size() - 1)
                                           .arg(pointText(lastWorldPosition_))
                                           .arg(pendingPoints_.size()));
            update();
            return;
        }

        if (activeTool_ == Tool::Arc && arcMode_ == ArcMode::OnePoint &&
            pendingPoints_.size() == 2) {
            // A click can arrive without a final mouse-move event. Include
            // that last position before saving the arc's unwrapped sweep.
            updateArcPreviewTracking(lastWorldPosition_);
        }

        pendingPoints_.append(lastWorldPosition_);

        if (activeTool_ == Tool::Arc && arcMode_ == ArcMode::OnePoint &&
            pendingPoints_.size() == 2) {
            initializeArcPreviewTracking();
        }

        if (pendingPoints_.size() == requiredPoints(activeTool_)) {
            recordGeometryChange();
            const ArcMode completedArcMode = activeTool_ == Tool::Arc
                                                 ? arcMode_
                                                 : ArcMode::TwoPoint;
            Shape completedShape{activeTool_,
                                 pendingPoints_,
                                 Shape::NurbsCurve2D{},
                                 completedArcMode,
                                 0.0,
                                 {}};
            if (activeTool_ == Tool::Arc && arcMode_ == ArcMode::OnePoint) {
                completedShape.arcSweep = arcPreviewSweepAngle_;
            }
            if (activeTool_ == Tool::Arc) {
                completedShape.nurbs = makeArcNurbsCurve(completedShape);
            } else if (activeTool_ == Tool::Bezier || activeTool_ == Tool::Nurbs) {
                completedShape.nurbs = makeBezierNurbs(completedShape.points);
            } else if (activeTool_ == Tool::Circle) {
                completedShape.nurbs = makeCircleNurbs(completedShape.points);
            }
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
        rawCursorWorld_ = screenToWorld(screenPosition);
        cursorWorld_ = constrainLinePoint(rawCursorWorld_);
        lastWorldPosition_ = cursorWorld_;
        cursorValid_ = true;
        const bool pointPreviewActive = activeTool_ == Tool::Point;
        const bool circlePreviewActive = activeTool_ == Tool::Circle && !pendingPoints_.isEmpty();
        const bool rectanglePreviewActive =
            activeTool_ == Tool::Rectangle && !pendingPoints_.isEmpty();
        const bool arcPreviewActive = activeTool_ == Tool::Arc;

        if (!panning_ && activeTool_ == Tool::Arc && arcMode_ == ArcMode::OnePoint &&
            pendingPoints_.size() >= 2) {
            updateArcPreviewTracking(cursorWorld_);
        }

        if (panning_) {
            const QPoint current = screenPosition.toPoint();
            const QPoint delta = current - lastMousePosition_;
            const QPoint totalPanDelta = current - panStartPosition_;
            if (std::hypot(totalPanDelta.x(), totalPanDelta.y()) >= 3.0) {
                panMoved_ = true;
            }
            pan_ += QPointF(delta.x() / zoom_, -delta.y() / zoom_);
            lastMousePosition_ = current;
        }

        if (draggingControlPoint_ && selectedShapeIndex_ >= 0 &&
            selectedShapeIndex_ < shapes_.size() && controlPointIndex_ >= 0) {
            const QPointF delta = rawCursorWorld_ - lastControlPointWorld_;
            if (!qFuzzyIsNull(delta.x()) || !qFuzzyIsNull(delta.y())) {
                beginDragHistory();
                translateControlPoint(selectedShapeIndex_, controlPointIndex_, delta);
                lastControlPointWorld_ = rawCursorWorld_;
                DebugLog::instance().write(
                    QStringLiteral("control point drag shape=%1 index=%2 delta=%3 world=%4")
                        .arg(selectedShapeIndex_)
                        .arg(controlPointIndex_)
                        .arg(pointText(delta))
                        .arg(pointText(rawCursorWorld_)));
            }
        } else if (draggingSelected_ && selectedShapeIndex_ >= 0 &&
            selectedShapeIndex_ < shapes_.size()) {
            const QPointF delta = rawCursorWorld_ - lastDragWorld_;
            if (!qFuzzyIsNull(delta.x()) || !qFuzzyIsNull(delta.y())) {
                constexpr qreal dragSnapBreakawayPixels = 18.0;
                const qreal cursorDistanceFromSnap =
                    std::hypot(screenPosition.x() - worldToScreen(dragSnapCursorWorld_).x(),
                               screenPosition.y() - worldToScreen(dragSnapCursorWorld_).y());

                if (dragSnapLocked_ && cursorDistanceFromSnap <= dragSnapBreakawayPixels) {
                    // Keep the geometry attached while the cursor is still near the
                    // snap point. This prevents a one-pixel mouse move from
                    // repeatedly attaching and detaching the line.
                    DebugLog::instance().write(
                        QStringLiteral("selection drag snap-hold shape=%1 cursorDistance=%2 breakaway=%3")
                            .arg(selectedShapeIndex_)
                            .arg(cursorDistanceFromSnap, 0, 'f', 2)
                            .arg(dragSnapBreakawayPixels, 0, 'f', 2));
                } else {
                    if (dragSnapLocked_) {
                        // Release from the snap using the complete cursor movement
                        // since the snap was acquired, so the line leaves cleanly.
                        const QPointF detachDelta = rawCursorWorld_ - dragSnapCursorWorld_;
                        beginDragHistory();
                        translateShape(selectedShapeIndex_, detachDelta);
                        currentDragSnap_ = DragSnapResult{};
                        dragSnapLocked_ = false;
                        DebugLog::instance().write(
                            QStringLiteral("selection drag snap-breakaway shape=%1 cursorDistance=%2")
                                .arg(selectedShapeIndex_)
                                .arg(cursorDistanceFromSnap, 0, 'f', 2));
                    } else {
                        beginDragHistory();
                        translateShape(selectedShapeIndex_, delta);
                        currentDragSnap_ = findDragSnap(selectedShapeIndex_);
                        if (currentDragSnap_.isValid()) {
                            translateShape(selectedShapeIndex_, currentDragSnap_.translation);
                            dragSnapLocked_ = true;
                            dragSnapCursorWorld_ = rawCursorWorld_;
                        }
                    }
                }

                lastDragWorld_ = rawCursorWorld_;

                DebugLog::instance().write(
                    QStringLiteral("selection drag shape=%1 delta=%2 cursorWorld=%3 snap=%4 snapSource=%5 snapTarget=%6 snapTranslation=%7")
                        .arg(selectedShapeIndex_)
                        .arg(pointText(delta))
                        .arg(pointText(rawCursorWorld_))
                        .arg(snapTypeName(currentDragSnap_.type))
                        .arg(pointText(currentDragSnap_.sourcePoint))
                        .arg(pointText(currentDragSnap_.targetPoint))
                        .arg(pointText(currentDragSnap_.translation)));
            }
        }

        if (pointPreviewActive || lineCommandActive_ || arcPreviewActive || circlePreviewActive ||
            rectanglePreviewActive || panning_ || draggingSelected_ || draggingControlPoint_) {
            update();
        }

        if (pointPreviewActive || lineCommandActive_ || arcPreviewActive || circlePreviewActive ||
            rectanglePreviewActive || panning_ || draggingSelected_ || draggingControlPoint_) {
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

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        const bool repeatToolOnRelease =
            panning_ && event->button() == panButton_ && !panMoved_ &&
            activeTool_ == Tool::Select && repeatTool_ != Tool::Select;

        DebugLog::instance().write(QStringLiteral("mouseRelease button=%1 screen=%2 panningBefore=%3 panMoved=%4 draggingBefore=%5 repeat=%6")
                                       .arg(inputButtonName(event->button()))
                                       .arg(pointText(eventPosition(event)))
                                       .arg(panning_)
                                       .arg(panMoved_)
                                       .arg(draggingSelected_)
                                       .arg(repeatToolOnRelease));
        if (panning_ && (event->button() == panButton_ || event->button() == Qt::LeftButton)) {
            panning_ = false;
            setCursor(activeTool_ == Tool::Select ? Qt::ArrowCursor : Qt::CrossCursor);
            DebugLog::instance().write(QStringLiteral("mouseRelease branch=end-pan"));
        }
        panMoved_ = false;

        if (repeatToolOnRelease) {
            DebugLog::instance().write(QStringLiteral("mouseRelease branch=repeat-tool"));
            repeatLastTool();
        }

        if (draggingControlPoint_ && event->button() == Qt::LeftButton) {
            draggingControlPoint_ = false;
            controlPointIndex_ = -1;
            dragHistoryRecorded_ = false;
            setCursor(activeTool_ == Tool::Select ? Qt::ArrowCursor : Qt::CrossCursor);
            DebugLog::instance().write(QStringLiteral("mouseRelease branch=end-control-point-drag shape=%1")
                                           .arg(selectedShapeIndex_));
            update();
        } else if (draggingSelected_ && event->button() == Qt::LeftButton) {
            draggingSelected_ = false;
            dragHistoryRecorded_ = false;
            currentDragSnap_ = DragSnapResult{};
            dragSnapLocked_ = false;
            setCursor(activeTool_ == Tool::Select ? Qt::ArrowCursor : Qt::CrossCursor);
            DebugLog::instance().write(QStringLiteral("mouseRelease branch=end-selection-drag shape=%1")
                                           .arg(selectedShapeIndex_));
            update();
        }
    }

    void wheelEvent(QWheelEvent *event) override
    {
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

        const QPointF screenPosition = eventPosition(event);
        const QPointF beforeZoom = screenToWorld(screenPosition);
        const qreal oldZoom = zoom_;
        const qreal factor = event->angleDelta().y() > 0 ? 1.12 : 1.0 / 1.12;
        zoom_ = std::clamp(zoom_ * factor, 0.15, 12.0);

        const QPointF afterZoom = screenToWorld(screenPosition);
        // Adjust the view translation so the world point under the cursor
        // remains under the cursor after the zoom.
        pan_ += afterZoom - beforeZoom;

        DebugLog::instance().write(QStringLiteral("wheel screen=%1 deltaY=%2 zoom=%3->%4 worldBefore=%5 worldAfter=%6 pan=%7")
                                       .arg(pointText(screenPosition))
                                       .arg(event->angleDelta().y())
                                       .arg(oldZoom, 0, 'f', 4)
                                       .arg(zoom_, 0, 'f', 4)
                                       .arg(pointText(beforeZoom))
                                       .arg(pointText(afterZoom))
                                       .arg(pointText(pan_)));

        update();
        emitCoordinateUpdate();
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        DebugLog::instance().write(QStringLiteral("keyPress key=%1 text=%2 tool=%3 lineActive=%4 points=%5")
                                       .arg(event->key())
                                       .arg(event->text())
                                       .arg(toolName(activeTool_))
                                       .arg(lineCommandActive_)
                                       .arg(pendingPoints_.size()));
        if (subdivisionActive_ &&
            (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
            applySubdivision(subdivisionSections_);
            return;
        }

        if (subdivisionActive_ && event->key() == Qt::Key_Escape) {
            cancelSubdivisionPreview();
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
            } else {
                lineCommandActive_ = false;
            }

            update();
            return;
        }

        QWidget::keyPressEvent(event);
    }

private:
    bool isSubdividableShape(const Shape &shape) const
    {
        if (shape.tool == Tool::Line) {
            return shape.points.size() >= 2 &&
                   (isValidNurbsCurve(shape.nurbs) || !shape.points.isEmpty());
        }

        return (shape.tool == Tool::Arc ||
                shape.tool == Tool::Bezier ||
                shape.tool == Tool::Nurbs ||
                shape.tool == Tool::Circle) &&
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

        if (shape.tool == Tool::Line && shape.points.size() >= 2) {
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
        subdivisionShapeIndex_ = -1;
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

    void recordGeometryChange()
    {
        undoStack_.append(shapes_);
        redoStack_.clear();
        notifyHistoryChanged();
        DebugLog::instance().write(QStringLiteral("history record shapes=%1 undoAvailable=%2 redoCleared")
                                       .arg(shapes_.size())
                                       .arg(undoStack_.size()));
    }

    void beginDragHistory()
    {
        if (!dragHistoryRecorded_) {
            recordGeometryChange();
            dragHistoryRecorded_ = true;
        }
    }

    void resetInteractionAfterHistory()
    {
        pendingPoints_.clear();
        selectedShapeIndex_ = -1;
        draggingSelected_ = false;
        dragHistoryRecorded_ = false;
        currentSnap_ = SnapResult{};
        currentDragSnap_ = DragSnapResult{};
        dragSnapLocked_ = false;
        draggingControlPoint_ = false;
        controlPointIndex_ = -1;
        subdivisionActive_ = false;
        subdivisionShapeIndex_ = -1;
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
            shapes_.append(Shape{Tool::Line,
                                 pendingPoints_,
                                 curve,
                                 ArcMode::TwoPoint,
                                 0.0,
                                 {}});
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
        if (shape.tool != Tool::Arc || shape.points.size() < 3) {
            return false;
        }

        if (shape.arcMode == ArcMode::TwoPoint) {
            return makeCircularArcGeometry(shape.points[0],
                                           shape.points[1],
                                           shape.points[2],
                                           centerScreen,
                                           radius,
                                           startAngle,
                                           sweepAngle);
        }

        const QPointF center = worldToScreen(shape.points[0]);
        const QPointF start = worldToScreen(shape.points[1]);
        const QPointF end = worldToScreen(shape.points[2]);
        const qreal arcRadius = std::hypot(start.x() - center.x(),
                                           start.y() - center.y());
        if (arcRadius <= 1e-9) {
            return false;
        }

        constexpr qreal pi = 3.14159265358979323846;
        constexpr qreal twoPi = 2.0 * pi;
        const qreal firstAngle = std::atan2(start.y() - center.y(),
                                            start.x() - center.x());
        qreal selectedSweep = shape.arcSweep;
        if (std::abs(selectedSweep) <= 1e-9) {
            const qreal endAngle = std::atan2(end.y() - center.y(),
                                              end.x() - center.x());
            selectedSweep = endAngle - firstAngle;
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
            *radius = arcRadius;
        }
        if (startAngle != nullptr) {
            *startAngle = firstAngle;
        }
        if (sweepAngle != nullptr) {
            *sweepAngle = selectedSweep;
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

        if (shape.tool == Tool::Point) {
            candidates.append(SnapCandidate{SnapType::Endpoint, shape.points.first()});
            return candidates;
        }

        if (shape.tool == Tool::Circle) {
            candidates.append(SnapCandidate{SnapType::Center, shape.points.first()});
            return candidates;
        }

        if (shape.tool == Tool::Arc && shape.points.size() >= 3) {
            QPointF start = shape.arcMode == ArcMode::OnePoint
                                ? shape.points[1]
                                : shape.points[0];
            QPointF end = shape.arcMode == ArcMode::OnePoint
                              ? shape.points[2]
                              : shape.points[1];
            QPointF evaluatedEndpoint;
            if (arcSnapPointAtFraction(shape, 0.0, &evaluatedEndpoint)) {
                start = evaluatedEndpoint;
            }
            if (arcSnapPointAtFraction(shape, 1.0, &evaluatedEndpoint)) {
                end = evaluatedEndpoint;
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

        if (shape.tool != Tool::Line) {
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
        QVector<SnapCandidate> candidates;
        QVector<LineSegment> segments;

        for (int shapeIndex = 0; shapeIndex < shapes_.size(); ++shapeIndex) {
            if (shapeIndex == excludedShapeIndex) {
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

            if (shape.tool == Tool::Point) {
                if (endpointSnapEnabled_) {
                    candidates.append(SnapCandidate{SnapType::Endpoint, shape.points.first()});
                }
                continue;
            }

            if (shape.tool == Tool::Circle) {
                if (centerSnapEnabled_) {
                    candidates.append(SnapCandidate{SnapType::Center, shape.points.first()});
                }
                continue;
            }

            if (shape.tool == Tool::Arc && shape.points.size() >= 3) {
                QPointF start = shape.arcMode == ArcMode::OnePoint
                                    ? shape.points[1]
                                    : shape.points[0];
                QPointF end = shape.arcMode == ArcMode::OnePoint
                                  ? shape.points[2]
                                  : shape.points[1];
                QPointF evaluatedEndpoint;
                if (arcSnapPointAtFraction(shape, 0.0, &evaluatedEndpoint)) {
                    start = evaluatedEndpoint;
                }
                if (arcSnapPointAtFraction(shape, 1.0, &evaluatedEndpoint)) {
                    end = evaluatedEndpoint;
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

            if (shape.tool != Tool::Line) {
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
        QVector<SnapCandidate> candidates;
        if (!perpendicularSnapEnabled_) {
            return candidates;
        }

        constexpr qreal epsilon = 1e-9;

        for (const Shape &shape : shapes_) {
            if (shape.points.isEmpty()) {
                continue;
            }

            if (shape.tool == Tool::Circle && shape.points.size() >= 2) {
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

            if (shape.tool == Tool::Arc && shape.points.size() >= 3) {
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

            if (shape.tool != Tool::Line) {
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
        QVector<SnapCandidate> candidates;
        if (!tangentSnapEnabled_) {
            return candidates;
        }

        constexpr qreal epsilon = 1e-9;

        for (const Shape &shape : shapes_) {
            if (shape.tool == Tool::Circle && shape.points.size() >= 2) {
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

            if (shape.tool != Tool::Arc || shape.points.size() < 3) {
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
        SnapResult best;
        const bool drawingSnapActive =
            (activeTool_ == Tool::Line && lineCommandActive_) ||
            activeTool_ == Tool::Arc || activeTool_ == Tool::Circle ||
            activeTool_ == Tool::Point;
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

    DragSnapResult findDragSnap(int selectedShapeIndex) const
    {
        DragSnapResult best;
        if (!osnapEnabled_ || selectedShapeIndex < 0 ||
            selectedShapeIndex >= shapes_.size()) {
            return best;
        }

        const QVector<SnapCandidate> sourceCandidates =
            snapCandidatesForShape(shapes_[selectedShapeIndex]);
        const QVector<SnapCandidate> targetCandidates =
            snapCandidatesForScene(selectedShapeIndex);
        constexpr qreal snapRadiusPixels = 12.0;
        qreal bestDistance = snapRadiusPixels;

        for (const SnapCandidate &source : sourceCandidates) {
            const QPointF sourceScreen = worldToScreen(source.point);
            for (const SnapCandidate &target : targetCandidates) {
                const QPointF targetScreen = worldToScreen(target.point);
                const qreal distance = std::hypot(targetScreen.x() - sourceScreen.x(),
                                                   targetScreen.y() - sourceScreen.y());
                if (distance <= bestDistance) {
                    bestDistance = distance;
                    best.type = source.type;
                    best.sourcePoint = source.point;
                    best.targetPoint = target.point;
                    best.translation = target.point - source.point;
                }
            }
        }

        return best;
    }

    QPointF constrainLinePoint(const QPointF &rawPoint)
    {
        currentSnap_ = findSnapPoint(rawPoint);
        if (currentSnap_.isValid()) {
            return currentSnap_.point;
        }

        const bool drawingConstraintActive =
            (activeTool_ == Tool::Line && lineCommandActive_) || activeTool_ == Tool::Arc;
        if (!orthoEnabled_ || panning_ || !drawingConstraintActive || pendingPoints_.isEmpty()) {
            return rawPoint;
        }

        if (activeTool_ == Tool::Arc && arcMode_ == ArcMode::OnePoint &&
            pendingPoints_.size() >= 2) {
            return constrainOnePointArcEndpoint(rawPoint);
        }

        if (activeTool_ == Tool::Arc && arcMode_ == ArcMode::TwoPoint &&
            pendingPoints_.size() >= 2) {
            return constrainTwoPointArcThroughPoint(rawPoint);
        }

        const QPointF origin = pendingPoints_.back();
        const qreal deltaX = rawPoint.x() - origin.x();
        const qreal deltaY = rawPoint.y() - origin.y();

        if (std::abs(deltaX) >= std::abs(deltaY)) {
            return QPointF(rawPoint.x(), origin.y());
        }

        return QPointF(origin.x(), rawPoint.y());
    }

    QPointF constrainOnePointArcEndpoint(const QPointF &rawPoint) const
    {
        const QPointF center = worldToScreen(pendingPoints_[0]);
        const QPointF start = worldToScreen(pendingPoints_[1]);
        const qreal radius = std::hypot(start.x() - center.x(),
                                        start.y() - center.y());
        if (radius <= 1e-9) {
            return rawPoint;
        }

        constexpr qreal pi = 3.14159265358979323846;
        constexpr qreal halfPi = pi / 2.0;
        constexpr qreal twoPi = 2.0 * pi;
        const qreal startAngle = std::atan2(start.y() - center.y(),
                                            start.x() - center.x());
        const QPointF raw = worldToScreen(rawPoint);
        const qreal rawAngle = std::atan2(raw.y() - center.y(),
                                          raw.x() - center.x());

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
        const QPointF snappedScreen(center.x() + radius * std::cos(snappedAngle),
                                    center.y() + radius * std::sin(snappedAngle));
        return screenToWorld(snappedScreen);
    }

    QPointF constrainTwoPointArcThroughPoint(const QPointF &rawPoint) const
    {
        const QPointF start = worldToScreen(pendingPoints_[0]);
        const QPointF end = worldToScreen(pendingPoints_[1]);
        const QPointF chord = end - start;
        const qreal chordLength = std::hypot(chord.x(), chord.y());
        if (chordLength <= 1e-9) {
            return rawPoint;
        }

        const QPointF midpoint = (start + end) / 2.0;
        const QPointF raw = worldToScreen(rawPoint);

        // A semicircle's through point is one half-chord radius away from
        // the chord midpoint, perpendicular to the start/end chord. These
        // are snap targets, not a permanent constraint: outside the snap
        // radius the third point remains free to define any circular arc.
        const QPointF leftNormal(-chord.y() / chordLength,
                                 chord.x() / chordLength);
        const qreal halfChord = chordLength / 2.0;
        const QPointF candidates[] = {
            midpoint + leftNormal * halfChord,
            midpoint - leftNormal * halfChord};

        constexpr qreal semicircleSnapRadiusPixels = 12.0;
        qreal closestDistance = semicircleSnapRadiusPixels;
        const QPointF *closestCandidate = nullptr;
        for (const QPointF &candidate : candidates) {
            const qreal distance = std::hypot(raw.x() - candidate.x(),
                                              raw.y() - candidate.y());
            if (distance <= closestDistance) {
                closestDistance = distance;
                closestCandidate = &candidate;
            }
        }

        return closestCandidate != nullptr ? screenToWorld(*closestCandidate) : rawPoint;
    }

    void refreshCursorConstraint()
    {
        if (!cursorValid_) {
            currentSnap_ = SnapResult{};
            return;
        }

        cursorWorld_ = constrainLinePoint(rawCursorWorld_);
        lastWorldPosition_ = cursorWorld_;
    }

    qreal distanceToSegment(const QPointF &point,
                            const QPointF &start,
                            const QPointF &end) const
    {
        const QPointF direction = end - start;
        const QPointF fromStart = point - start;
        const qreal lengthSquared = direction.x() * direction.x() +
                                    direction.y() * direction.y();

        if (lengthSquared <= 1e-12) {
            return std::hypot(point.x() - start.x(), point.y() - start.y());
        }

        const qreal projection = std::clamp(
            (fromStart.x() * direction.x() + fromStart.y() * direction.y()) / lengthSquared,
            0.0,
            1.0);
        const QPointF closest = start + direction * projection;
        return std::hypot(point.x() - closest.x(), point.y() - closest.y());
    }

    qreal distanceToArc(const QPointF &screenPosition, const Shape &shape) const
    {
        QPointF center;
        qreal radius = 0.0;
        qreal startAngle = 0.0;
        qreal sweepAngle = 0.0;
        if (!makeArcSnapGeometry(shape,
                                 &center,
                                 &radius,
                                 &startAngle,
                                 &sweepAngle)) {
            return 1.0e9;
        }

        const QPointF fromCenter = screenPosition - center;
        const qreal distanceFromCenter =
            std::hypot(fromCenter.x(), fromCenter.y());
        const qreal pointAngle = std::atan2(fromCenter.y(), fromCenter.x());
        if (arcAngleIsOnSweep(startAngle, sweepAngle, pointAngle)) {
            return std::abs(distanceFromCenter - radius);
        }

        QPointF startPoint;
        QPointF endPoint;
        if (!arcSnapPointAtFraction(shape, 0.0, &startPoint) ||
            !arcSnapPointAtFraction(shape, 1.0, &endPoint)) {
            return 1.0e9;
        }

        return std::min(
            distanceToSegment(screenPosition,
                             worldToScreen(startPoint),
                             worldToScreen(startPoint)),
            distanceToSegment(screenPosition,
                             worldToScreen(endPoint),
                             worldToScreen(endPoint)));
    }

    qreal distanceToCubicCurve(const QPointF &screenPosition,
                               const Shape &shape) const
    {
        if (shape.points.size() < 4) {
            return 1.0e9;
        }

        const QPointF first = worldToScreen(shape.points[0]);
        const QPointF second = worldToScreen(shape.points[1]);
        const QPointF third = worldToScreen(shape.points[2]);
        const QPointF fourth = worldToScreen(shape.points[3]);
        constexpr int sampleCount = 64;
        qreal closestDistance = 1.0e9;
        QPointF previous = first;

        for (int sample = 1; sample <= sampleCount; ++sample) {
            const qreal t = static_cast<qreal>(sample) / sampleCount;
            const qreal inverse = 1.0 - t;
            const QPointF current =
                first * (inverse * inverse * inverse) +
                second * (3.0 * inverse * inverse * t) +
                third * (3.0 * inverse * t * t) +
                fourth * (t * t * t);
            closestDistance = std::min(closestDistance,
                                       distanceToSegment(screenPosition,
                                                        previous,
                                                        current));
            previous = current;
        }

        return closestDistance;
    }

    qreal distanceToNurbsCurve(const QPointF &screenPosition,
                               const Shape::NurbsCurve2D &curve) const
    {
        if (!isValidNurbsCurve(curve)) {
            return 1.0e9;
        }

        const QVector<double> fullKnots = expandedKnotVector(curve);
        const qreal firstParameter = fullKnots[curve.degree];
        const qreal lastParameter = fullKnots[curve.controlPoints.size()];
        int nonZeroSpans = 0;
        for (int index = curve.degree; index < curve.controlPoints.size(); ++index) {
            if (fullKnots[index + 1] > fullKnots[index]) {
                ++nonZeroSpans;
            }
        }

        const int sampleCount = std::max(64, nonZeroSpans * 32);
        QPointF previousWorld;
        if (!evaluateNurbsPoint(curve, firstParameter, &previousWorld)) {
            return 1.0e9;
        }

        qreal closestDistance = 1.0e9;
        QPointF previous = worldToScreen(previousWorld);
        for (int sample = 1; sample <= sampleCount; ++sample) {
            const qreal fraction = static_cast<qreal>(sample) / sampleCount;
            const qreal parameter = firstParameter +
                                    (lastParameter - firstParameter) * fraction;
            QPointF currentWorld;
            if (!evaluateNurbsPoint(curve, parameter, &currentWorld)) {
                continue;
            }
            const QPointF current = worldToScreen(currentWorld);
            closestDistance = std::min(closestDistance,
                                       distanceToSegment(screenPosition,
                                                        previous,
                                                        current));
            previous = current;
        }

        return closestDistance;
    }

    QVector<QPointF> controlPointsForShape(const Shape &shape) const
    {
        if (shape.tool == Tool::Point) {
            return {};
        }

        if (!shape.nurbs.controlPoints.isEmpty()) {
            return shape.nurbs.controlPoints;
        }

        return shape.points;
    }

    int hitTestControlPoint(int shapeIndex, const QPointF &screenPosition) const
    {
        if (shapeIndex < 0 || shapeIndex >= shapes_.size()) {
            return -1;
        }

        const QVector<QPointF> controlPoints = controlPointsForShape(shapes_[shapeIndex]);
        constexpr qreal hitRadiusPixels = 10.0;
        int closestIndex = -1;
        qreal closestDistance = hitRadiusPixels;
        for (int index = 0; index < controlPoints.size(); ++index) {
            const QPointF screenPoint = worldToScreen(controlPoints[index]);
            const qreal distance = std::hypot(screenPosition.x() - screenPoint.x(),
                                              screenPosition.y() - screenPoint.y());
            if (distance <= closestDistance) {
                closestDistance = distance;
                closestIndex = index;
            }
        }

        return closestIndex;
    }

    int hitTestShape(const QPointF &screenPosition) const
    {
        constexpr qreal hitRadiusPixels = 9.0;
        int closestShape = -1;
        qreal closestDistance = hitRadiusPixels;

        for (int index = 0; index < shapes_.size(); ++index) {
            const Shape &shape = shapes_[index];

            if (shape.tool == Tool::Point && !shape.points.isEmpty()) {
                const QPointF point = worldToScreen(shape.points.first());
                const qreal distance = std::hypot(screenPosition.x() - point.x(),
                                                  screenPosition.y() - point.y());
                if (distance <= closestDistance) {
                    closestDistance = distance;
                    closestShape = index;
                }
                continue;
            }

            if (shape.tool == Tool::Circle && shape.points.size() >= 2) {
                qreal distanceFromCircle = 1.0e9;
                if (isValidNurbsCurve(shape.nurbs)) {
                    distanceFromCircle = distanceToNurbsCurve(screenPosition, shape.nurbs);
                } else {
                    const QPointF center = worldToScreen(shape.points[0]);
                    const QPointF edge = worldToScreen(shape.points[1]);
                    const qreal radius = std::hypot(edge.x() - center.x(), edge.y() - center.y());
                    const qreal distanceFromCenter =
                        std::hypot(screenPosition.x() - center.x(),
                                   screenPosition.y() - center.y());
                    distanceFromCircle = std::abs(distanceFromCenter - radius);
                }

                if (distanceFromCircle <= closestDistance) {
                    closestDistance = distanceFromCircle;
                    closestShape = index;
                }
                continue;
            }

            if (shape.tool == Tool::Arc && shape.points.size() >= 3) {
                const qreal distance = isValidNurbsCurve(shape.nurbs)
                                           ? distanceToNurbsCurve(screenPosition, shape.nurbs)
                                           : distanceToArc(screenPosition, shape);
                if (distance <= closestDistance) {
                    closestDistance = distance;
                    closestShape = index;
                }
                continue;
            }

            if ((shape.tool == Tool::Bezier || shape.tool == Tool::Nurbs) &&
                shape.points.size() >= 4) {
                const qreal distance = isValidNurbsCurve(shape.nurbs)
                                           ? distanceToNurbsCurve(screenPosition, shape.nurbs)
                                           : distanceToCubicCurve(screenPosition, shape);
                if (distance <= closestDistance) {
                    closestDistance = distance;
                    closestShape = index;
                }
                continue;
            }

            if (shape.tool == Tool::Rectangle && shape.points.size() >= 2) {
                const QPointF first = worldToScreen(shape.points[0]);
                const QPointF second = worldToScreen(shape.points[1]);
                const QRectF rectangle = QRectF(first, second).normalized();
                const QPointF topLeft = rectangle.topLeft();
                const QPointF topRight = rectangle.topRight();
                const QPointF bottomLeft = rectangle.bottomLeft();
                const QPointF bottomRight = rectangle.bottomRight();
                const qreal distance = std::min({
                    distanceToSegment(screenPosition, topLeft, topRight),
                    distanceToSegment(screenPosition, topRight, bottomRight),
                    distanceToSegment(screenPosition, bottomRight, bottomLeft),
                    distanceToSegment(screenPosition, bottomLeft, topLeft)});
                if (distance <= closestDistance) {
                    closestDistance = distance;
                    closestShape = index;
                }
                continue;
            }

            if (shape.tool != Tool::Line) {
                continue;
            }

            for (int pointIndex = 0; pointIndex + 1 < shape.points.size(); ++pointIndex) {
                const qreal distance = distanceToSegment(
                    screenPosition,
                    worldToScreen(shape.points[pointIndex]),
                    worldToScreen(shape.points[pointIndex + 1]));
                if (distance <= closestDistance) {
                    closestDistance = distance;
                    closestShape = index;
                }
            }
        }

        return closestShape;
    }

    void translateControlPoint(int shapeIndex, int controlPointIndex, const QPointF &delta)
    {
        if (shapeIndex < 0 || shapeIndex >= shapes_.size() ||
            controlPointIndex < 0 ||
            (qFuzzyIsNull(delta.x()) && qFuzzyIsNull(delta.y()))) {
            return;
        }

        Shape &shape = shapes_[shapeIndex];
        if (!shape.nurbs.controlPoints.isEmpty()) {
            if (controlPointIndex >= shape.nurbs.controlPoints.size()) {
                return;
            }

            shape.nurbs.controlPoints[controlPointIndex] += delta;

            // These curve types keep their source points in the same order as
            // their NURBS CVs. Keep both representations synchronized. Arc
            // and circle construction points intentionally remain unchanged;
            // their stored NURBS is the geometry being edited.
            if ((shape.tool == Tool::Line || shape.tool == Tool::Bezier ||
                 shape.tool == Tool::Nurbs) &&
                controlPointIndex < shape.points.size()) {
                shape.points[controlPointIndex] += delta;
            }
            return;
        }

        if (controlPointIndex < shape.points.size()) {
            shape.points[controlPointIndex] += delta;
        }
    }

    void translateShape(int index, const QPointF &delta)
    {
        if (index < 0 || index >= shapes_.size()) {
            return;
        }

        Shape &shape = shapes_[index];
        for (QPointF &point : shape.points) {
            point += delta;
        }
        for (QPointF &point : shape.nurbs.controlPoints) {
            point += delta;
        }
    }

    QPointF screenToWorld(const QPointF &screen) const
    {
        return QPointF((screen.x() - width() / 2.0) / zoom_ - pan_.x(),
                       (height() / 2.0 - screen.y()) / zoom_ - pan_.y());
    }

    QPointF worldToScreen(const QPointF &world) const
    {
        return QPointF(width() / 2.0 + (world.x() + pan_.x()) * zoom_,
                       height() / 2.0 - (world.y() + pan_.y()) * zoom_);
    }

    void drawSnapMarker(QPainter &painter,
                        SnapType type,
                        const QPointF &worldPoint)
    {
        const QPointF snapScreen = worldToScreen(worldPoint);
        const QColor snapColor(QStringLiteral("#63b5e8"));
        painter.setPen(QPen(snapColor, 2.0));
        painter.setBrush(Qt::NoBrush);

        if (type == SnapType::Endpoint) {
            painter.drawEllipse(snapScreen, 7.0, 7.0);
        } else if (type == SnapType::Midpoint) {
            painter.drawRect(QRectF(snapScreen - QPointF(6.0, 6.0),
                                    snapScreen + QPointF(6.0, 6.0)));
        } else if (type == SnapType::Intersection) {
            painter.drawLine(snapScreen - QPointF(7.0, 7.0),
                             snapScreen + QPointF(7.0, 7.0));
            painter.drawLine(snapScreen - QPointF(7.0, -7.0),
                             snapScreen + QPointF(7.0, -7.0));
        } else if (type == SnapType::Center) {
            painter.drawEllipse(snapScreen, 7.0, 7.0);
            painter.drawLine(snapScreen - QPointF(9.0, 0.0),
                             snapScreen + QPointF(9.0, 0.0));
            painter.drawLine(snapScreen - QPointF(0.0, 9.0),
                             snapScreen + QPointF(0.0, 9.0));
        } else if (type == SnapType::Perpendicular) {
            const QPointF corner = snapScreen + QPointF(-2.0, 3.0);
            painter.drawLine(snapScreen + QPointF(-8.0, 3.0), corner);
            painter.drawLine(corner, snapScreen + QPointF(-2.0, -5.0));
        } else if (type == SnapType::Tangent) {
            painter.drawEllipse(snapScreen, 6.0, 6.0);
            painter.drawLine(snapScreen + QPointF(-7.0, 4.0),
                             snapScreen + QPointF(7.0, -4.0));
        }

    }

    void drawLineToolPreview(QPainter &painter)
    {
        const QColor lineColor(QStringLiteral("#e6b85c"));
        const QColor pointColor(QStringLiteral("#f0a45a"));

        painter.setPen(QPen(lineColor, 2.0));
        for (int i = 0; i + 1 < pendingPoints_.size(); ++i) {
            painter.drawLine(worldToScreen(pendingPoints_[i]),
                             worldToScreen(pendingPoints_[i + 1]));
        }

        if (!pendingPoints_.isEmpty() && cursorValid_) {
            painter.setPen(QPen(lineColor, 2.0));
            painter.drawLine(worldToScreen(pendingPoints_.back()),
                             worldToScreen(cursorWorld_));
        }

        painter.setPen(QPen(pointColor, 1.5));
        painter.setBrush(QColor(QStringLiteral("#282828")));
        for (const QPointF &point : pendingPoints_) {
            painter.drawEllipse(worldToScreen(point), 5.0, 5.0);
        }

        // This marker follows the cursor until the next left click plants it.
        if (cursorValid_) {
            painter.setPen(QPen(pointColor, 2.0));
            painter.setBrush(pointColor);
            painter.drawEllipse(worldToScreen(cursorWorld_), 4.0, 4.0);
        }

        if (currentSnap_.isValid()) {
            drawSnapMarker(painter, currentSnap_.type, currentSnap_.point);
        }
    }

    bool makeCircularArcGeometry(const QPointF &startWorld,
                                 const QPointF &endWorld,
                                 const QPointF &throughWorld,
                                 QPointF *center,
                                 qreal *radius,
                                 qreal *startAngle,
                                 qreal *sweepAngle) const
    {
        const QPointF start = worldToScreen(startWorld);
        const QPointF end = worldToScreen(endWorld);
        const QPointF through = worldToScreen(throughWorld);

        const qreal startSquared = start.x() * start.x() + start.y() * start.y();
        const qreal endSquared = end.x() * end.x() + end.y() * end.y();
        const qreal throughSquared =
            through.x() * through.x() + through.y() * through.y();
        const qreal denominator = 2.0 *
            (start.x() * (end.y() - through.y()) +
             end.x() * (through.y() - start.y()) +
             through.x() * (start.y() - end.y()));

        if (std::abs(denominator) < 1e-9) {
            return false;
        }

        const QPointF circleCenter(
            (startSquared * (end.y() - through.y()) +
             endSquared * (through.y() - start.y()) +
             throughSquared * (start.y() - end.y())) / denominator,
            (startSquared * (through.x() - end.x()) +
             endSquared * (start.x() - through.x()) +
             throughSquared * (end.x() - start.x())) / denominator);
        const qreal circleRadius = std::hypot(start.x() - circleCenter.x(),
                                              start.y() - circleCenter.y());
        if (circleRadius <= 1e-9) {
            return false;
        }

        constexpr qreal twoPi = 6.28318530717958647692;
        const auto normalizeAngle = [twoPi](qreal angle) {
            angle = std::fmod(angle, twoPi);
            if (angle < 0.0) {
                angle += twoPi;
            }
            return angle;
        };

        const qreal firstAngle = std::atan2(start.y() - circleCenter.y(),
                                            start.x() - circleCenter.x());
        const qreal secondAngle = std::atan2(end.y() - circleCenter.y(),
                                             end.x() - circleCenter.x());
        const qreal throughAngle = std::atan2(through.y() - circleCenter.y(),
                                              through.x() - circleCenter.x());
        const qreal counterClockwiseSweep = normalizeAngle(secondAngle - firstAngle);
        const qreal throughSweep = normalizeAngle(throughAngle - firstAngle);

        if (counterClockwiseSweep <= 1e-9) {
            return false;
        }

        const qreal selectedSweep = throughSweep <= counterClockwiseSweep + 1e-7
                                        ? counterClockwiseSweep
                                        : -(twoPi - counterClockwiseSweep);

        if (center != nullptr) {
            *center = circleCenter;
        }
        if (radius != nullptr) {
            *radius = circleRadius;
        }
        if (startAngle != nullptr) {
            *startAngle = firstAngle;
        }
        if (sweepAngle != nullptr) {
            *sweepAngle = selectedSweep;
        }
        return true;
    }

    void drawCircularArc(QPainter &painter,
                         const QPointF &start,
                         const QPointF &end,
                         const QPointF &through)
    {
        QPointF center;
        qreal radius = 0.0;
        qreal startAngle = 0.0;
        qreal sweepAngle = 0.0;

        if (!makeCircularArcGeometry(start,
                                     end,
                                     through,
                                     &center,
                                     &radius,
                                     &startAngle,
                                     &sweepAngle)) {
            painter.drawLine(worldToScreen(start), worldToScreen(end));
            return;
        }

        const int steps = std::clamp(
            static_cast<int>(std::ceil(std::abs(sweepAngle) * radius / 8.0)),
            12,
            256);
        QPainterPath path;
        for (int step = 0; step <= steps; ++step) {
            const qreal fraction = static_cast<qreal>(step) / steps;
            const qreal angle = startAngle + sweepAngle * fraction;
            const QPointF point(center.x() + radius * std::cos(angle),
                                 center.y() + radius * std::sin(angle));
            if (step == 0) {
                path.moveTo(point);
            } else {
                path.lineTo(point);
            }
        }
        painter.drawPath(path);
    }

    void drawCenterArcWithSweep(QPainter &painter,
                                const QPointF &centerWorld,
                                const QPointF &startWorld,
                                qreal sweepAngle)
    {
        const QPointF center = worldToScreen(centerWorld);
        const QPointF start = worldToScreen(startWorld);
        const qreal radius = std::hypot(start.x() - center.x(),
                                        start.y() - center.y());

        if (radius <= 1e-9) {
            return;
        }

        const qreal startAngle = std::atan2(start.y() - center.y(),
                                            start.x() - center.x());

        if (std::abs(sweepAngle) <= 1e-9) {
            return;
        }

        const int steps = std::clamp(
            static_cast<int>(std::ceil(std::abs(sweepAngle) * radius / 8.0)),
            12,
            256);
        QPainterPath path;
        for (int step = 0; step <= steps; ++step) {
            const qreal fraction = static_cast<qreal>(step) / steps;
            const qreal angle = startAngle + sweepAngle * fraction;
            const QPointF point(center.x() + radius * std::cos(angle),
                                 center.y() + radius * std::sin(angle));
            if (step == 0) {
                path.moveTo(point);
            } else {
                path.lineTo(point);
            }
        }
        painter.drawPath(path);
    }

    void drawCenterArc(QPainter &painter,
                       const QPointF &centerWorld,
                       const QPointF &startWorld,
                       const QPointF &endWorld)
    {
        const QPointF center = worldToScreen(centerWorld);
        const QPointF start = worldToScreen(startWorld);
        const QPointF end = worldToScreen(endWorld);
        const qreal radius = std::hypot(start.x() - center.x(),
                                        start.y() - center.y());

        if (radius <= 1e-9) {
            painter.drawLine(start, end);
            return;
        }

        constexpr qreal pi = 3.14159265358979323846;
        constexpr qreal twoPi = 2.0 * pi;
        const qreal startAngle = std::atan2(start.y() - center.y(),
                                            start.x() - center.x());
        const qreal endAngle = std::atan2(end.y() - center.y(),
                                          end.x() - center.x());
        qreal sweepAngle = endAngle - startAngle;
        if (sweepAngle > pi) {
            sweepAngle -= twoPi;
        } else if (sweepAngle < -pi) {
            sweepAngle += twoPi;
        }

        if (std::abs(sweepAngle) <= 1e-9) {
            painter.drawLine(start, end);
            return;
        }

        drawCenterArcWithSweep(painter, centerWorld, startWorld, sweepAngle);
    }

    void resetArcPreviewTracking()
    {
        arcPreviewInitialized_ = false;
        arcPreviewPreviousAngle_ = 0.0;
        arcPreviewSweepAngle_ = 0.0;
    }

    void initializeArcPreviewTracking()
    {
        resetArcPreviewTracking();
        if (arcMode_ != ArcMode::OnePoint || pendingPoints_.size() < 2) {
            return;
        }

        const QPointF center = worldToScreen(pendingPoints_[0]);
        const QPointF start = worldToScreen(pendingPoints_[1]);
        const qreal radius = std::hypot(start.x() - center.x(),
                                        start.y() - center.y());
        if (radius <= 1e-9) {
            return;
        }

        arcPreviewPreviousAngle_ = std::atan2(start.y() - center.y(),
                                              start.x() - center.x());
        arcPreviewInitialized_ = true;
    }

    void updateArcPreviewTracking(const QPointF &cursorWorld)
    {
        if (arcMode_ != ArcMode::OnePoint || pendingPoints_.size() < 2) {
            return;
        }

        const QPointF center = worldToScreen(pendingPoints_[0]);
        const QPointF cursor = worldToScreen(cursorWorld);
        const qreal radius = std::hypot(cursor.x() - center.x(),
                                        cursor.y() - center.y());
        if (radius <= 1e-9) {
            return;
        }

        const qreal angle = std::atan2(cursor.y() - center.y(),
                                       cursor.x() - center.x());
        if (!arcPreviewInitialized_) {
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
        arcPreviewPreviousAngle_ = angle;
    }

    void drawArcToolPreview(QPainter &painter)
    {
        if (pendingPoints_.isEmpty()) {
            if (currentSnap_.isValid()) {
                drawSnapMarker(painter, currentSnap_.type, currentSnap_.point);
            }
            return;
        }

        const QColor arcColor(QStringLiteral("#e6b85c"));
        const QColor pointColor(QStringLiteral("#f0a45a"));
        painter.setPen(QPen(arcColor, 2.0));
        painter.setBrush(Qt::NoBrush);

        if (arcMode_ == ArcMode::OnePoint) {
            if (pendingPoints_.size() == 1) {
                if (cursorValid_) {
                    // First click establishes the center. The radius follows
                    // the cursor until the second click plants the start.
                    painter.drawLine(worldToScreen(pendingPoints_.first()),
                                     worldToScreen(cursorWorld_));
                }
            } else if (cursorValid_) {
                // The center and start are fixed; the endpoint remains live
                // under the cursor until the third click commits the arc.
                painter.setPen(QPen(QColor(QStringLiteral("#8aa7c7")),
                                    1.0,
                                    Qt::DashLine));
                painter.drawLine(worldToScreen(pendingPoints_[0]),
                                 worldToScreen(pendingPoints_[1]));

                painter.setPen(QPen(arcColor, 2.0));
                if (arcPreviewInitialized_) {
                    drawCenterArcWithSweep(painter,
                                           pendingPoints_[0],
                                           pendingPoints_[1],
                                           arcPreviewSweepAngle_);
                }
            }
        } else {
            if (pendingPoints_.size() == 1) {
                if (cursorValid_) {
                    painter.drawLine(worldToScreen(pendingPoints_.first()),
                                     worldToScreen(cursorWorld_));
                }
            } else if (cursorValid_) {
                // The fixed two-point chord is a construction guide. It
                // disappears when the third click commits the circular arc.
                painter.setPen(QPen(QColor(QStringLiteral("#8aa7c7")),
                                    1.0,
                                    Qt::DashLine));
                painter.drawLine(worldToScreen(pendingPoints_[0]),
                                 worldToScreen(pendingPoints_[1]));

                painter.setPen(QPen(arcColor, 2.0));
                drawCircularArc(painter,
                                pendingPoints_[0],
                                pendingPoints_[1],
                                cursorWorld_);
            }
        }

        painter.setPen(QPen(pointColor, 1.5));
        painter.setBrush(QColor(QStringLiteral("#282828")));
        for (const QPointF &point : pendingPoints_) {
            painter.drawEllipse(worldToScreen(point), 5.0, 5.0);
        }

        if (cursorValid_) {
            painter.setPen(QPen(pointColor, 2.0));
            painter.setBrush(pointColor);
            painter.drawEllipse(worldToScreen(cursorWorld_), 4.0, 4.0);
        }

        if (currentSnap_.isValid()) {
            drawSnapMarker(painter, currentSnap_.type, currentSnap_.point);
        }
    }

    void drawCircleToolPreview(QPainter &painter)
    {
        if (pendingPoints_.isEmpty()) {
            return;
        }

        const QColor circleColor(QStringLiteral("#e6b85c"));
        const QColor pointColor(QStringLiteral("#f0a45a"));
        const QPointF center = worldToScreen(pendingPoints_.first());

        painter.setPen(QPen(circleColor, 2.0));
        painter.setBrush(Qt::NoBrush);

        if (cursorValid_) {
            const QPointF edge = worldToScreen(cursorWorld_);
            const qreal radius = std::hypot(edge.x() - center.x(), edge.y() - center.y());
            painter.drawEllipse(center, radius, radius);

            painter.setPen(QPen(pointColor, 1.5));
            painter.setBrush(pointColor);
            painter.drawEllipse(edge, 4.0, 4.0);
        }

        painter.setPen(QPen(pointColor, 1.5));
        painter.setBrush(QColor(QStringLiteral("#282828")));
        painter.drawEllipse(center, 5.0, 5.0);

        if (currentSnap_.isValid()) {
            drawSnapMarker(painter, currentSnap_.type, currentSnap_.point);
        }
    }

    void drawRectangleToolPreview(QPainter &painter)
    {
        if (pendingPoints_.isEmpty()) {
            return;
        }

        const QColor rectangleColor(QStringLiteral("#e6b85c"));
        const QColor pointColor(QStringLiteral("#f0a45a"));
        const QPointF firstWorld = pendingPoints_.first();
        const QPointF firstScreen = worldToScreen(firstWorld);

        painter.setPen(QPen(pointColor, 1.5));
        painter.setBrush(QColor(QStringLiteral("#282828")));
        painter.drawEllipse(firstScreen, 5.0, 5.0);

        if (!cursorValid_) {
            return;
        }

        const QPointF secondWorld = cursorWorld_;
        const QPointF secondScreen = worldToScreen(secondWorld);
        const QRectF rectangle = QRectF(firstScreen, secondScreen).normalized();

        painter.setPen(QPen(rectangleColor, 2.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(rectangle);

        painter.setPen(QPen(pointColor, 2.0));
        painter.setBrush(pointColor);
        painter.drawEllipse(secondScreen, 4.0, 4.0);

        painter.setPen(QColor(QStringLiteral("#d0d0d0")));
        painter.setFont(QFont(QStringLiteral("Sans"), 9));
        const qreal width = std::abs(secondWorld.x() - firstWorld.x());
        const qreal height = std::abs(secondWorld.y() - firstWorld.y());
        const QString dimensions = QStringLiteral("W %1  H %2")
                                       .arg(width, 0, 'f', 2)
                                       .arg(height, 0, 'f', 2);
        QPointF labelPosition = rectangle.topLeft() + QPointF(6.0, -8.0);
        if (labelPosition.y() < 14.0) {
            labelPosition.setY(rectangle.top() + 16.0);
        }
        painter.drawText(labelPosition, dimensions);

        if (currentSnap_.isValid()) {
            drawSnapMarker(painter, currentSnap_.type, currentSnap_.point);
        }
    }

    void drawControlPoints(QPainter &painter, const Shape &shape)
    {
        const QVector<QPointF> controlPoints = controlPointsForShape(shape);
        if (controlPoints.isEmpty()) {
            return;
        }

        const QColor handleColor(QStringLiteral("#77b7e6"));
        const QColor handleFill(QStringLiteral("#263b4b"));
        painter.setPen(QPen(handleColor, 1.0, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        for (int index = 0; index + 1 < controlPoints.size(); ++index) {
            painter.drawLine(worldToScreen(controlPoints[index]),
                             worldToScreen(controlPoints[index + 1]));
        }

        for (int index = 0; index < controlPoints.size(); ++index) {
            const bool active = draggingControlPoint_ &&
                                selectedShapeIndex_ >= 0 &&
                                selectedShapeIndex_ < shapes_.size() &&
                                controlPointIndex_ == index;
            painter.setPen(QPen(active ? QColor(QStringLiteral("#f0a45a")) : handleColor,
                                1.5));
            painter.setBrush(active ? QColor(QStringLiteral("#f0a45a")) : handleFill);
            const QPointF &point = controlPoints[index];
            const QPointF screenPoint = worldToScreen(point);
            painter.drawRect(QRectF(screenPoint - QPointF(4.0, 4.0),
                                    screenPoint + QPointF(4.0, 4.0)));
        }
    }

    void drawGrid(QPainter &painter)
    {
        const QPointF topLeft = screenToWorld(QPointF(0, 0));
        const QPointF bottomRight = screenToWorld(QPointF(width(), height()));
        const qreal step = 25.0;

        painter.setPen(QPen(QColor(QStringLiteral("#353535")), 1));

        const qreal firstX = std::floor(topLeft.x() / step) * step;
        const qreal firstY = std::floor(bottomRight.y() / step) * step;

        for (qreal x = firstX; x <= bottomRight.x(); x += step) {
            const int screenX = qRound(worldToScreen(QPointF(x, 0)).x());
            painter.drawLine(screenX, 0, screenX, height());
        }

        for (qreal y = firstY; y <= topLeft.y(); y += step) {
            const int screenY = qRound(worldToScreen(QPointF(0, y)).y());
            painter.drawLine(0, screenY, width(), screenY);
        }
    }

    void drawOrigin(QPainter &painter)
    {
        const QPointF origin = worldToScreen(QPointF(0, 0));
        painter.setPen(QPen(QColor(QStringLiteral("#a85b5b")), 1));
        painter.drawLine(0, qRound(origin.y()), width(), qRound(origin.y()));
        painter.setPen(QPen(QColor(QStringLiteral("#628e65")), 1));
        painter.drawLine(qRound(origin.x()), 0, qRound(origin.x()), height());
    }

    QVector<double> expandedKnotVector(const Shape::NurbsCurve2D &curve) const
    {
        QVector<double> fullKnots;
        if (curve.knots.isEmpty()) {
            return fullKnots;
        }

        fullKnots.reserve(curve.knots.size() + 2);
        fullKnots.append(curve.knots.first());
        for (const double knot : curve.knots) {
            fullKnots.append(knot);
        }
        fullKnots.append(curve.knots.last());
        return fullKnots;
    }

    bool isValidNurbsCurve(const Shape::NurbsCurve2D &curve) const
    {
        if (curve.degree < 1 ||
            curve.order != curve.degree + 1 ||
            curve.controlPoints.size() <= curve.degree ||
            curve.knots.size() != curve.controlPoints.size() + curve.order - 2 ||
            (curve.rational && curve.weights.size() != curve.controlPoints.size()) ||
            (!curve.rational && !curve.weights.isEmpty() &&
             curve.weights.size() != curve.controlPoints.size())) {
            return false;
        }

        for (int index = 0; index < curve.knots.size(); ++index) {
            if (!std::isfinite(curve.knots[index]) ||
                (index > 0 && curve.knots[index] < curve.knots[index - 1])) {
                return false;
            }
        }
        for (const double weight : curve.weights) {
            if (!std::isfinite(weight) || (curve.rational && weight <= 0.0)) {
                return false;
            }
        }

        const QVector<double> fullKnots = expandedKnotVector(curve);
        const int endKnotIndex = curve.controlPoints.size();
        return fullKnots.size() == curve.controlPoints.size() + curve.degree + 1 &&
               fullKnots[curve.degree] < fullKnots[endKnotIndex];
    }

    bool evaluateNurbsPoint(const Shape::NurbsCurve2D &curve,
                            qreal parameter,
                            QPointF *point) const
    {
        if (!isValidNurbsCurve(curve) || point == nullptr) {
            return false;
        }

        const int controlPointCount = curve.controlPoints.size();
        const QVector<double> fullKnots = expandedKnotVector(curve);
        const int endKnotIndex = controlPointCount;
        const qreal firstParameter = fullKnots[curve.degree];
        const qreal lastParameter = fullKnots[endKnotIndex];
        const qreal epsilon = 1e-12;
        if (parameter <= firstParameter + epsilon) {
            *point = curve.controlPoints.first();
            return true;
        }
        if (parameter >= lastParameter - epsilon) {
            *point = curve.controlPoints.last();
            return true;
        }

        const auto basis = [&fullKnots](const auto &self,
                                       int index,
                                       int degree,
                                       qreal parameterValue) -> qreal {
            if (degree == 0) {
                return fullKnots[index] <= parameterValue &&
                               parameterValue < fullKnots[index + 1]
                           ? 1.0
                           : 0.0;
            }

            qreal value = 0.0;
            const qreal leftDenominator = fullKnots[index + degree] - fullKnots[index];
            if (std::abs(leftDenominator) > 1e-12) {
                value += (parameterValue - fullKnots[index]) / leftDenominator *
                         self(self, index, degree - 1, parameterValue);
            }

            const qreal rightDenominator = fullKnots[index + degree + 1] -
                                           fullKnots[index + 1];
            if (std::abs(rightDenominator) > 1e-12) {
                value += (fullKnots[index + degree + 1] - parameterValue) /
                         rightDenominator *
                         self(self, index + 1, degree - 1, parameterValue);
            }
            return value;
        };

        QPointF numerator(0.0, 0.0);
        qreal denominator = 0.0;
        for (int index = 0; index < controlPointCount; ++index) {
            const qreal weightedBasis = basis(basis, index, curve.degree, parameter) *
                                         (curve.rational ? curve.weights[index] : 1.0);
            numerator += curve.controlPoints[index] * weightedBasis;
            denominator += weightedBasis;
        }

        if (std::abs(denominator) <= 1e-12) {
            return false;
        }
        *point = numerator / denominator;
        return true;
    }

    void drawNurbsCurve(QPainter &painter, const Shape::NurbsCurve2D &curve)
    {
        if (!isValidNurbsCurve(curve)) {
            return;
        }

        const int controlPointCount = curve.controlPoints.size();
        const QVector<double> fullKnots = expandedKnotVector(curve);
        const qreal firstParameter = fullKnots[curve.degree];
        const qreal lastParameter = fullKnots[controlPointCount];
        int nonZeroSpans = 0;
        for (int index = curve.degree; index < controlPointCount; ++index) {
            if (fullKnots[index + 1] > fullKnots[index]) {
                ++nonZeroSpans;
            }
        }
        const int sampleCount = std::max(32, nonZeroSpans * 24);

        QPainterPath path;
        bool hasStart = false;

        for (int sample = 0; sample <= sampleCount; ++sample) {
            const qreal fraction = static_cast<qreal>(sample) / sampleCount;
            const qreal parameter = firstParameter +
                                    (lastParameter - firstParameter) * fraction;
            QPointF point;
            if (!evaluateNurbsPoint(curve, parameter, &point)) {
                continue;
            }
            const QPointF screenPoint = worldToScreen(point);
            if (!hasStart) {
                path.moveTo(screenPoint);
                hasStart = true;
            } else {
                path.lineTo(screenPoint);
            }
        }

        if (hasStart) {
            painter.drawPath(path);
        }
    }

    void drawSubdivisionPoints(QPainter &painter,
                               const Shape &shape,
                               const QVector<double> &parameters,
                               bool preview)
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

        const QVector<double> fullKnots = expandedKnotVector(curve);
        if (fullKnots.size() > curve.controlPoints.size()) {
            const double firstParameter = fullKnots[curve.degree];
            const double lastParameter = fullKnots[curve.controlPoints.size()];
            for (const double parameter : {firstParameter, lastParameter}) {
                QPointF point;
                if (evaluateNurbsPoint(curve, parameter, &point)) {
                    painter.drawEllipse(worldToScreen(point),
                                         preview ? 5.0 : 4.0,
                                         preview ? 5.0 : 4.0);
                }
            }
        }

        for (const double parameter : parameters) {
            QPointF point;
            if (!evaluateNurbsPoint(curve, parameter, &point)) {
                continue;
            }
            painter.drawEllipse(worldToScreen(point), preview ? 5.0 : 4.0, preview ? 5.0 : 4.0);
        }
    }

    void drawPointToolPreview(QPainter &painter)
    {
        if (!cursorValid_) {
            return;
        }

        const QPointF screenPoint = worldToScreen(cursorWorld_);
        const QColor pointColor(QStringLiteral("#e6b85c"));
        painter.setPen(QPen(pointColor, 1.5));
        painter.setBrush(pointColor);
        painter.drawEllipse(screenPoint, 4.5, 4.5);

        if (currentSnap_.isValid()) {
            drawSnapMarker(painter, currentSnap_.type, currentSnap_.point);
        }
    }

    void drawShape(QPainter &painter,
                   const Shape &shape,
                   bool preview,
                   bool selected = false)
    {
        if (shape.points.isEmpty()) {
            return;
        }

        const QColor curveColor = selected ? QColor(QStringLiteral("#5da9e9"))
                                           : preview ? QColor(QStringLiteral("#e6b85c"))
                                                     : QColor(QStringLiteral("#d28b45"));
        const QColor controlColor = QColor(QStringLiteral("#8aa7c7"));
        const qreal curveWidth = selected ? 3.5 : (preview ? 1.5 : 2.0);

        painter.setPen(QPen(curveColor, curveWidth));
        // Subdivision markers use a filled brush. Always reset the geometry
        // brush so an open curve is never rendered as a filled wedge.
        painter.setBrush(Qt::NoBrush);

        if (shape.tool == Tool::Point && shape.points.size() >= 1) {
            painter.setPen(QPen(curveColor, selected ? 2.0 : 1.5));
            painter.setBrush(curveColor);
            painter.drawEllipse(worldToScreen(shape.points.first()),
                                selected ? 5.0 : 4.5,
                                selected ? 5.0 : 4.5);
        } else if (shape.tool == Tool::Line && shape.points.size() >= 2) {
            if (isValidNurbsCurve(shape.nurbs)) {
                drawNurbsCurve(painter, shape.nurbs);
            } else {
                for (int i = 0; i + 1 < shape.points.size(); ++i) {
                    painter.drawLine(worldToScreen(shape.points[i]),
                                     worldToScreen(shape.points[i + 1]));
                }
            }
        } else if (shape.tool == Tool::Rectangle && shape.points.size() >= 2) {
            const QRectF rectangle(worldToScreen(shape.points[0]), worldToScreen(shape.points[1]));
            painter.drawRect(rectangle.normalized());
        } else if (shape.tool == Tool::Circle && shape.points.size() >= 2) {
            if (isValidNurbsCurve(shape.nurbs)) {
                drawNurbsCurve(painter, shape.nurbs);
            } else {
                const QPointF center = worldToScreen(shape.points[0]);
                const QPointF edge = worldToScreen(shape.points[1]);
                const qreal radius = std::hypot(edge.x() - center.x(), edge.y() - center.y());
                painter.drawEllipse(center, radius, radius);
            }
        } else if (shape.tool == Tool::Arc && shape.points.size() >= 3) {
            if (isValidNurbsCurve(shape.nurbs)) {
                drawNurbsCurve(painter, shape.nurbs);
            } else if (shape.arcMode == ArcMode::OnePoint) {
                if (std::abs(shape.arcSweep) > 1e-9) {
                    drawCenterArcWithSweep(painter,
                                           shape.points[0],
                                           shape.points[1],
                                           shape.arcSweep);
                } else {
                    drawCenterArc(painter,
                                  shape.points[0],
                                  shape.points[1],
                                  shape.points[2]);
                }
            } else {
                drawCircularArc(painter,
                                shape.points[0],
                                shape.points[1],
                                shape.points[2]);
            }
        } else if ((shape.tool == Tool::Bezier || shape.tool == Tool::Nurbs) &&
                   shape.points.size() >= 4) {
            const QVector<QPointF> controlPoints = shape.nurbs.controlPoints.isEmpty()
                                                       ? shape.points
                                                       : shape.nurbs.controlPoints;
            painter.setPen(QPen(controlColor, 1, Qt::DashLine));
            for (int i = 0; i + 1 < controlPoints.size(); ++i) {
                painter.drawLine(worldToScreen(controlPoints[i]),
                                 worldToScreen(controlPoints[i + 1]));
            }

            painter.setPen(QPen(curveColor, curveWidth));
            if (isValidNurbsCurve(shape.nurbs)) {
                drawNurbsCurve(painter, shape.nurbs);
            } else {
                QPainterPath curve;
                curve.moveTo(worldToScreen(shape.points[0]));
                curve.cubicTo(worldToScreen(shape.points[1]),
                              worldToScreen(shape.points[2]),
                              worldToScreen(shape.points[3]));
                painter.drawPath(curve);
            }
        } else {
            painter.setPen(QPen(controlColor, 1, Qt::DashLine));
            for (int i = 0; i + 1 < shape.points.size(); ++i) {
                painter.drawLine(worldToScreen(shape.points[i]), worldToScreen(shape.points[i + 1]));
            }
        }

        if (preview) {
            painter.setBrush(controlColor);
            painter.setPen(Qt::NoPen);
            for (const QPointF &point : shape.points) {
                const QPointF screenPoint = worldToScreen(point);
                painter.drawEllipse(screenPoint, 4.0, 4.0);
            }
        }
    }

    void emitCoordinateUpdate()
    {
        if (coordinateUpdate_) {
            coordinateUpdate_(coordinateText());
        }
    }

public:
    std::function<void(const QString &)> coordinateUpdate_;
    std::function<void(Tool)> commandFinished_;
    std::function<void(Tool)> toolRepeated_;
    std::function<void()> historyChanged_;
    std::function<void(const QString &)> subdivisionStatusUpdate_;

private:
    static constexpr int maxSubdivisionSections = 10000;
    Tool activeTool_ = Tool::Select;
    Tool repeatTool_ = Tool::Select;
    ArcMode arcMode_ = ArcMode::OnePoint;
    bool arcPreviewInitialized_ = false;
    qreal arcPreviewPreviousAngle_ = 0.0;
    qreal arcPreviewSweepAngle_ = 0.0;
    bool controlPointsVisible_ = false;
    QVector<Shape> shapes_;
    QVector<QVector<Shape>> undoStack_;
    QVector<QVector<Shape>> redoStack_;
    QVector<QPointF> pendingPoints_;
    QPointF pan_{0.0, 0.0};
    QPointF lastWorldPosition_{0.0, 0.0};
    QPointF rawCursorWorld_{0.0, 0.0};
    QPoint lastMousePosition_;
    QPointF cursorWorld_{0.0, 0.0};
    SnapResult currentSnap_;
    DragSnapResult currentDragSnap_;
    int selectedShapeIndex_ = -1;
    bool draggingSelected_ = false;
    bool draggingControlPoint_ = false;
    int controlPointIndex_ = -1;
    bool dragHistoryRecorded_ = false;
    bool dragSnapLocked_ = false;
    QPointF dragSnapCursorWorld_{0.0, 0.0};
    QPointF lastDragWorld_{0.0, 0.0};
    QPointF lastControlPointWorld_{0.0, 0.0};
    qreal zoom_ = 1.0;
    bool panning_ = false;
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
    bool subdivisionActive_ = false;
    int subdivisionShapeIndex_ = -1;
    int subdivisionSections_ = 2;
    int subdivisionWheelAccumulator_ = 0;
    qreal subdivisionPixelAccumulator_ = 0.0;
};

class PreferencesDialog final : public QDialog {
public:
    explicit PreferencesDialog(Qt::MouseButton panButton, QWidget *parent = nullptr)
        : QDialog(parent)
    {
        setWindowTitle(QStringLiteral("Preferences"));
        resize(760, 520);
        setModal(true);

        auto *rootLayout = new QVBoxLayout(this);
        rootLayout->setContentsMargins(10, 10, 10, 10);

        auto *contentLayout = new QHBoxLayout;
        contentLayout->setSpacing(10);
        rootLayout->addLayout(contentLayout, 1);

        categoryList_ = new QListWidget;
        categoryList_->setObjectName(QStringLiteral("preferencesCategories"));
        categoryList_->setFixedWidth(165);

        const QStringList categoryNames{
            QStringLiteral("Interface"),
            QStringLiteral("Viewport"),
            QStringLiteral("Lights"),
            QStringLiteral("Editing"),
            QStringLiteral("Animation"),
            QStringLiteral("Get Extensions"),
            QStringLiteral("Add-ons"),
            QStringLiteral("Themes"),
            QStringLiteral("Asset Libraries"),
            QStringLiteral("Input"),
            QStringLiteral("Navigation"),
            QStringLiteral("Keymap"),
            QStringLiteral("System"),
            QStringLiteral("Save & Load"),
            QStringLiteral("File Paths"),
        };
        categoryList_->addItems(categoryNames);
        contentLayout->addWidget(categoryList_);

        pages_ = new QStackedWidget;
        for (const QString &category : categoryNames) {
            if (category == QStringLiteral("Keymap")) {
                pages_->addWidget(createKeymapPage(panButton));
            } else {
                pages_->addWidget(createPlaceholderPage(category));
            }
        }
        contentLayout->addWidget(pages_, 1);

        auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
        rootLayout->addWidget(buttonBox);

        connect(categoryList_, &QListWidget::currentRowChanged,
                pages_, &QStackedWidget::setCurrentIndex);
        categoryList_->setCurrentRow(categoryNames.indexOf(QStringLiteral("Keymap")));
    }

    Qt::MouseButton panButton() const
    {
        return panButtonCombo_->currentIndex() == 1 ? Qt::RightButton : Qt::MiddleButton;
    }

private:
    QWidget *createPlaceholderPage(const QString &category)
    {
        auto *page = new QWidget;
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(18, 12, 18, 12);

        auto *title = new QLabel(category);
        title->setObjectName(QStringLiteral("preferencesTitle"));
        layout->addWidget(title);

        auto *description = new QLabel(QStringLiteral("The %1 preferences will be added here.")
                                           .arg(category));
        description->setObjectName(QStringLiteral("preferencesHint"));
        description->setWordWrap(true);
        layout->addWidget(description);
        layout->addStretch(1);
        return page;
    }

    QWidget *createKeymapPage(Qt::MouseButton panButton)
    {
        auto *page = new QWidget;
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(18, 12, 18, 12);
        layout->setSpacing(12);

        auto *title = new QLabel(QStringLiteral("Keymap"));
        title->setObjectName(QStringLiteral("preferencesTitle"));
        layout->addWidget(title);

        auto *navigationBox = new QGroupBox(QStringLiteral("Viewport Navigation"));
        auto *navigationLayout = new QFormLayout(navigationBox);
        panButtonCombo_ = new QComboBox;
        panButtonCombo_->addItem(QStringLiteral("Middle Mouse Button"));
        panButtonCombo_->addItem(QStringLiteral("Right Mouse Button"));
        panButtonCombo_->setCurrentIndex(panButton == Qt::RightButton ? 1 : 0);
        navigationLayout->addRow(QStringLiteral("Pan with"), panButtonCombo_);
        layout->addWidget(navigationBox);

        auto *hint = new QLabel(QStringLiteral(
            "Choose which mouse button pans the 2D viewport. The mouse wheel continues to zoom, "
            "and Alt + Left Mouse Button remains available as an alternate pan shortcut."));
        hint->setObjectName(QStringLiteral("preferencesHint"));
        hint->setWordWrap(true);
        layout->addWidget(hint);
        layout->addStretch(1);
        return page;
    }

    QListWidget *categoryList_ = nullptr;
    QStackedWidget *pages_ = nullptr;
    QComboBox *panButtonCombo_ = nullptr;
};

class MainWindow final : public QMainWindow {
public:
    MainWindow()
    {
        setWindowTitle(QStringLiteral("classiCAD — 2D NURBS Modeler"));
        resize(1440, 900);
        setMinimumSize(980, 620);

        createMenus();
        createWorkspaceBar();
        createMainLayout();
        loadPreferences();
        applyTheme();
    }

    bool restoreUpdateSession(const QString &path)
    {
        if (viewport_ == nullptr || !viewport_->restoreUpdateSession(path)) {
            statusBar()->showMessage(QStringLiteral("Update session could not be restored"), 8000);
            return false;
        }

        QFile::remove(path);
        statusBar()->showMessage(QStringLiteral("Update complete — scene restored"), 5000);
        return true;
    }

private:
    void updateApplication()
    {
        if (updateProcess_ != nullptr) {
            statusBar()->showMessage(QStringLiteral("Update already in progress"), 3000);
            return;
        }

        const QString executablePath = QCoreApplication::applicationFilePath();
        const QString buildDirectory = QCoreApplication::applicationDirPath();
        const QString sessionPath = QDir(QDir::tempPath()).filePath(
            QStringLiteral("classiCAD-update-%1-%2.json")
                .arg(QCoreApplication::applicationPid())
                .arg(QDateTime::currentMSecsSinceEpoch()));

        if (viewport_ == nullptr || !viewport_->saveUpdateSession(sessionPath)) {
            statusBar()->showMessage(QStringLiteral("Update cancelled — could not save the current scene"),
                                     8000);
            return;
        }

        updateAction_->setEnabled(false);
        statusBar()->showMessage(QStringLiteral("Updating classiCAD — rebuilding…"));

        auto *process = new QProcess(this);
        updateProcess_ = process;
        process->setWorkingDirectory(buildDirectory);

        const auto failUpdate = [this, process, sessionPath](const QString &message) {
            if (updateProcess_ != process) {
                return;
            }

            updateProcess_ = nullptr;
            updateAction_->setEnabled(true);
            QFile::remove(sessionPath);
            statusBar()->showMessage(message, 8000);
            process->deleteLater();
        };

        connect(process,
                &QProcess::errorOccurred,
                this,
                [process, failUpdate](QProcess::ProcessError error) {
                    if (error == QProcess::FailedToStart) {
                        failUpdate(QStringLiteral("Update failed — could not start cmake: %1")
                                       .arg(process->errorString()));
                    }
                });

        connect(process,
                qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                this,
                [this,
                 process,
                 sessionPath,
                 executablePath,
                 buildDirectory,
                 failUpdate](int exitCode, QProcess::ExitStatus exitStatus) {
                    if (updateProcess_ != process) {
                        return;
                    }

                    const QString buildOutput =
                        QString::fromLocal8Bit(process->readAllStandardOutput() +
                                                process->readAllStandardError())
                            .trimmed();
                    if (!buildOutput.isEmpty()) {
                        DebugLog::instance().write(QStringLiteral("update build output: %1")
                                                       .arg(buildOutput));
                    }

                    if (exitStatus != QProcess::NormalExit || exitCode != 0) {
                        failUpdate(QStringLiteral("Update failed — build exited with code %1")
                                       .arg(exitCode));
                        return;
                    }

                    const QStringList arguments{
                        QStringLiteral("--update-session"),
                        sessionPath};
                    if (!QProcess::startDetached(executablePath,
                                                 arguments,
                                                 buildDirectory)) {
                        failUpdate(QStringLiteral("Update failed — could not restart classiCAD"));
                        return;
                    }

                    updateProcess_ = nullptr;
                    process->deleteLater();
                    statusBar()->showMessage(QStringLiteral("Update complete — restarting classiCAD"));
                    QTimer::singleShot(0, []() {
                        QCoreApplication::quit();
                    });
                });

        process->start(QStringLiteral("cmake"),
                       QStringList{QStringLiteral("--build"), buildDirectory});
    }

    void startSubdivisionWheelMode()
    {
        if (viewport_ == nullptr || !viewport_->beginSubdivisionWheelMode()) {
            statusBar()->showMessage(QStringLiteral("Select a line or curve first"), 4000);
            return;
        }

        statusBar()->showMessage(viewport_->subdivisionStatusText());
    }

    void subdivideWithNumberOfPoints()
    {
        if (viewport_ == nullptr || !viewport_->beginSubdivisionWheelMode()) {
            statusBar()->showMessage(QStringLiteral("Select a line or curve first"), 4000);
            return;
        }

        const QString title = QStringLiteral("Subdivide Curve");
        const QString label = QStringLiteral("Number of sections:");
        bool accepted = false;
        const int sections = QInputDialog::getInt(this,
                                                  title,
                                                  label,
                                                  2,
                                                  2,
                                                  10000,
                                                  1,
                                                  &accepted);
        if (!accepted) {
            viewport_->cancelSubdivisionWheelMode();
            return;
        }

        if (!viewport_->applySubdivision(sections)) {
            statusBar()->showMessage(QStringLiteral("Could not subdivide the selected curve"), 5000);
            return;
        }

        statusBar()->showMessage(QStringLiteral("Subdivided into %1 sections (endpoints marked)")
                                     .arg(sections),
                                 5000);
    }

    void createMenus()
    {
        QMenu *fileMenu = menuBar()->addMenu(QStringLiteral("File"));
        fileMenu->addAction(QStringLiteral("New Document"));
        fileMenu->addAction(QStringLiteral("Open…"));
        fileMenu->addSeparator();
        fileMenu->addAction(QStringLiteral("Save"));
        fileMenu->addAction(QStringLiteral("Save As…"));
        fileMenu->addSeparator();
        fileMenu->addAction(QStringLiteral("Quit"), this, &QWidget::close);

        QMenu *editMenu = menuBar()->addMenu(QStringLiteral("Edit"));
        undoAction_ = editMenu->addAction(QStringLiteral("Undo"));
        undoAction_->setShortcut(QKeySequence::Undo);
        undoAction_->setEnabled(false);
        redoAction_ = editMenu->addAction(QStringLiteral("Redo"));
        redoAction_->setShortcut(QKeySequence::Redo);
        redoAction_->setEnabled(false);
        connect(undoAction_, &QAction::triggered, this, [this]() {
            viewport_->undo();
            statusBar()->showMessage(QStringLiteral("Undo"));
        });
        connect(redoAction_, &QAction::triggered, this, [this]() {
            viewport_->redo();
            statusBar()->showMessage(QStringLiteral("Redo"));
        });

        editMenu->addSeparator();
        subdivideAction_ = editMenu->addAction(QStringLiteral("Subdivide Selected (Wheel)"));
        subdivideAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+R")));
        subdivideAction_->setShortcutContext(Qt::WindowShortcut);
        connect(subdivideAction_, &QAction::triggered, this, [this]() {
            startSubdivisionWheelMode();
        });

        editMenu->addSeparator();
        QAction *preferencesAction = editMenu->addAction(QStringLiteral("Preferences…"));
        connect(preferencesAction, &QAction::triggered, this, [this]() {
            openPreferences();
        });

        QMenu *viewMenu = menuBar()->addMenu(QStringLiteral("View"));
        viewMenu->addAction(QStringLiteral("Frame All"));
        viewMenu->addAction(QStringLiteral("Toggle Grid"));

        menuBar()->addMenu(QStringLiteral("Help"));
    }

    void updateHistoryActions()
    {
        if (undoAction_ != nullptr && viewport_ != nullptr) {
            undoAction_->setEnabled(viewport_->canUndo());
        }
        if (redoAction_ != nullptr && viewport_ != nullptr) {
            redoAction_->setEnabled(viewport_->canRedo());
        }
    }

    void createWorkspaceBar()
    {
        auto *bar = new QToolBar(QStringLiteral("Workspace"), this);
        bar->setObjectName(QStringLiteral("workspaceBar"));
        bar->setMovable(false);
        bar->setFloatable(false);
        bar->setToolButtonStyle(Qt::ToolButtonTextOnly);

        QLabel *brand = new QLabel(QStringLiteral("classiCAD"));
        brand->setObjectName(QStringLiteral("brand"));
        bar->addWidget(brand);
        bar->addSeparator();

        for (const QString &name : {QStringLiteral("Modeling"),
                                     QStringLiteral("Sketching"),
                                     QStringLiteral("Layout")}) {
            QToolButton *workspaceButton = new QToolButton;
            workspaceButton->setText(name);
            workspaceButton->setCheckable(true);
            workspaceButton->setAutoExclusive(true);
            workspaceButton->setObjectName(QStringLiteral("workspaceButton"));
            bar->addWidget(workspaceButton);
            if (name == QStringLiteral("Modeling")) {
                workspaceButton->setChecked(true);
            }
        }

        bar->addSeparator();
        QLabel *mode = new QLabel(QStringLiteral("2D NURBS"));
        mode->setObjectName(QStringLiteral("modeLabel"));
        bar->addWidget(mode);

        bar->addSeparator();
        updateAction_ = new QAction(QStringLiteral("Update"), this);
        updateAction_->setToolTip(QStringLiteral("Rebuild and restart classiCAD, preserving the current scene"));
        connect(updateAction_, &QAction::triggered, this, [this]() {
            updateApplication();
        });
        bar->addAction(updateAction_);

        addToolBar(Qt::TopToolBarArea, bar);
    }

    void createMainLayout()
    {
        auto *root = new QWidget;
        auto *rootLayout = new QHBoxLayout(root);
        rootLayout->setContentsMargins(0, 0, 0, 0);
        rootLayout->setSpacing(0);

        rootLayout->addWidget(createToolShelf());

        viewport_ = new ViewportWidget;
        viewport_->commandFinished_ = [this](Tool tool) {
            if (tool == Tool::Select && selectToolButton_ != nullptr) {
                selectToolButton_->setChecked(true);
                statusBar()->showMessage(QStringLiteral("Select mode"));
            }
        };
        viewport_->toolRepeated_ = [this](Tool tool) {
            for (QToolButton *button : toolButtons_) {
                if (button->toolTip() == toolName(tool)) {
                    button->setChecked(true);
                    break;
                }
            }
            statusBar()->showMessage(QStringLiteral("Repeated tool: %1").arg(toolName(tool)));
        };
        rootLayout->addWidget(viewport_, 1);

        rootLayout->addWidget(createRightPanel());
        setCentralWidget(root);
        createOsnapLane();

        coordinateLabel_ = new QLabel(QStringLiteral("X 0.00   Y 0.00   Zoom 100%"));
        statusBar()->addWidget(coordinateLabel_);

        orthoAction_ = new QAction(QStringLiteral("Ortho"), this);
        orthoAction_->setCheckable(true);
        orthoAction_->setShortcut(QKeySequence(Qt::Key_F8));
        orthoAction_->setShortcutContext(Qt::WindowShortcut);
        addAction(orthoAction_);

        auto *orthoButton = new QToolButton;
        orthoButton->setObjectName(QStringLiteral("statusToggle"));
        orthoButton->setDefaultAction(orthoAction_);
        statusBar()->addPermanentWidget(orthoButton);

        osnapAction_ = new QAction(QStringLiteral("OSnap"), this);
        osnapAction_->setCheckable(true);
        osnapAction_->setShortcut(QKeySequence(Qt::Key_F3));
        osnapAction_->setShortcutContext(Qt::WindowShortcut);
        addAction(osnapAction_);

        auto *osnapButton = new QToolButton;
        osnapButton->setObjectName(QStringLiteral("statusToggle"));
        osnapButton->setDefaultAction(osnapAction_);
        statusBar()->addPermanentWidget(osnapButton);
        statusBar()->addPermanentWidget(new QLabel(QStringLiteral("Ready")));

        connect(orthoAction_, &QAction::toggled, this, [this](bool enabled) {
            viewport_->setOrthoEnabled(enabled);
            QSettings settings;
            settings.setValue(QStringLiteral("modeling/orthoEnabled"), enabled);
            settings.sync();
            statusBar()->showMessage(enabled ? QStringLiteral("Ortho: On")
                                             : QStringLiteral("Ortho: Off"));
        });

        connect(osnapAction_, &QAction::toggled, this, [this](bool enabled) {
            osnapLane_->setVisible(enabled);
            viewport_->setOsnapEnabled(enabled);
            QSettings settings;
            settings.setValue(QStringLiteral("osnap/enabled"), enabled);
            settings.sync();
            statusBar()->showMessage(enabled ? QStringLiteral("OSnap: On")
                                             : QStringLiteral("OSnap: Off"));
        });

        viewport_->coordinateUpdate_ = [this](const QString &text) {
            coordinateLabel_->setText(text);
        };
        viewport_->historyChanged_ = [this]() {
            updateHistoryActions();
        };
        viewport_->subdivisionStatusUpdate_ = [this](const QString &message) {
            if (message.isEmpty()) {
                statusBar()->clearMessage();
            } else {
                statusBar()->showMessage(message);
            }
        };
        updateHistoryActions();
    }

    void createOsnapLane()
    {
        osnapLane_ = new QToolBar(QStringLiteral("Object Snaps"), this);
        osnapLane_->setObjectName(QStringLiteral("osnapLane"));
        osnapLane_->setMovable(false);
        osnapLane_->setFloatable(false);
        osnapLane_->setToolButtonStyle(Qt::ToolButtonTextOnly);

        QLabel *label = new QLabel(QStringLiteral("OSNAP"));
        label->setObjectName(QStringLiteral("osnapLaneLabel"));
        osnapLane_->addWidget(label);
        osnapLane_->addSeparator();

        endpointSnapCheckBox_ = new QCheckBox(QStringLiteral("Endpoint"));
        midpointSnapCheckBox_ = new QCheckBox(QStringLiteral("Midpoint"));
        intersectionSnapCheckBox_ = new QCheckBox(QStringLiteral("Intersection"));
        centerSnapCheckBox_ = new QCheckBox(QStringLiteral("Center"));
        perpendicularSnapCheckBox_ = new QCheckBox(QStringLiteral("Perpendicular"));
        tangentSnapCheckBox_ = new QCheckBox(QStringLiteral("Tangent"));

        for (QCheckBox *checkBox : {endpointSnapCheckBox_,
                                    midpointSnapCheckBox_,
                                    intersectionSnapCheckBox_,
                                    centerSnapCheckBox_,
                                    perpendicularSnapCheckBox_,
                                    tangentSnapCheckBox_}) {
            checkBox->setObjectName(QStringLiteral("osnapCheckBox"));
            checkBox->setChecked(true);
            osnapLane_->addWidget(checkBox);
        }
        perpendicularSnapCheckBox_->setChecked(false);
        tangentSnapCheckBox_->setChecked(false);

        const auto syncSnapModes = [this]() {
            viewport_->setSnapModes(endpointSnapCheckBox_->isChecked(),
                                    midpointSnapCheckBox_->isChecked(),
                                    intersectionSnapCheckBox_->isChecked(),
                                    centerSnapCheckBox_->isChecked(),
                                    perpendicularSnapCheckBox_->isChecked(),
                                    tangentSnapCheckBox_->isChecked());
            QSettings settings;
            settings.setValue(QStringLiteral("osnap/endpoint"), endpointSnapCheckBox_->isChecked());
            settings.setValue(QStringLiteral("osnap/midpoint"), midpointSnapCheckBox_->isChecked());
            settings.setValue(QStringLiteral("osnap/intersection"),
                              intersectionSnapCheckBox_->isChecked());
            settings.setValue(QStringLiteral("osnap/center"), centerSnapCheckBox_->isChecked());
            settings.setValue(QStringLiteral("osnap/perpendicular"),
                              perpendicularSnapCheckBox_->isChecked());
            settings.setValue(QStringLiteral("osnap/tangent"), tangentSnapCheckBox_->isChecked());
            settings.sync();
        };

        connect(endpointSnapCheckBox_, &QCheckBox::toggled, this, syncSnapModes);
        connect(midpointSnapCheckBox_, &QCheckBox::toggled, this, syncSnapModes);
        connect(intersectionSnapCheckBox_, &QCheckBox::toggled, this, syncSnapModes);
        connect(centerSnapCheckBox_, &QCheckBox::toggled, this, syncSnapModes);
        connect(perpendicularSnapCheckBox_, &QCheckBox::toggled, this, syncSnapModes);
        connect(tangentSnapCheckBox_, &QCheckBox::toggled, this, syncSnapModes);

        addToolBar(Qt::BottomToolBarArea, osnapLane_);
        osnapLane_->setVisible(false);
    }

    QWidget *createToolShelf()
    {
        auto *shelf = new QFrame;
        shelf->setObjectName(QStringLiteral("toolShelf"));
        shelf->setFixedWidth(82);

        auto *layout = new QVBoxLayout(shelf);
        layout->setContentsMargins(7, 10, 7, 10);
        layout->setSpacing(6);

        QLabel *label = new QLabel(QStringLiteral("TOOLS"));
        label->setObjectName(QStringLiteral("shelfLabel"));
        label->setAlignment(Qt::AlignCenter);
        layout->addWidget(label);

        auto *group = new QButtonGroup(shelf);
        group->setExclusive(true);

        selectToolButton_ = addToolButton(layout, group, QStringLiteral("↖\nSelect"), Tool::Select, true);
        addToolButton(layout, group, QStringLiteral("•\nPoint"), Tool::Point);
        addToolButton(layout, group, QStringLiteral("╱\nLine"), Tool::Line);
        arcToolButton_ = addToolButton(layout, group, QStringLiteral("⌒\nArc"), Tool::Arc);
        createArcToolMenu(arcToolButton_);
        addToolButton(layout, group, QStringLiteral("∿\nBezier"), Tool::Bezier);
        addToolButton(layout, group, QStringLiteral("N\nNURBS"), Tool::Nurbs);
        addToolButton(layout, group, QStringLiteral("□\nRect"), Tool::Rectangle);
        addToolButton(layout, group, QStringLiteral("○\nCircle"), Tool::Circle);

        layout->addSpacing(8);
        controlPointsButton_ = new QToolButton;
        controlPointsButton_->setObjectName(QStringLiteral("toolButton"));
        controlPointsButton_->setText(QStringLiteral("CP\nPoints"));
        controlPointsButton_->setToolTip(QStringLiteral("Control Points"));
        controlPointsButton_->setCheckable(true);
        controlPointsButton_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        layout->addWidget(controlPointsButton_);
        connect(controlPointsButton_, &QToolButton::toggled, this, [this](bool visible) {
            viewport_->setControlPointsVisible(visible);
            QSettings settings;
            settings.setValue(QStringLiteral("view/controlPoints"), visible);
            settings.sync();
            statusBar()->showMessage(visible ? QStringLiteral("Control points: On")
                                             : QStringLiteral("Control points: Off"));
        });

        subdivideButton_ = new QToolButton;
        subdivideButton_->setObjectName(QStringLiteral("toolButton"));
        subdivideButton_->setText(QStringLiteral("Subdiv\nPoints"));
        subdivideButton_->setToolTip(QStringLiteral("Add evenly spaced subdivision points to the selected line or curve"));
        subdivideButton_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        layout->addWidget(subdivideButton_);
        connect(subdivideButton_, &QToolButton::clicked, this, [this]() {
            subdivideWithNumberOfPoints();
        });

        layout->addStretch(1);

        toolHelp_ = new QLabel;
        toolHelp_->setObjectName(QStringLiteral("toolHelp"));
        toolHelp_->setAlignment(Qt::AlignCenter);
        layout->addWidget(toolHelp_);
        updateToolHelp();

        return shelf;
    }

    QToolButton *addToolButton(QVBoxLayout *layout,
                               QButtonGroup *group,
                               const QString &text,
                               Tool tool,
                               bool checked = false)
    {
        auto *button = new QToolButton;
        button->setObjectName(QStringLiteral("toolButton"));
        button->setText(text);
        button->setCheckable(true);
        button->setChecked(checked);
        button->setToolTip(toolName(tool));
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        group->addButton(button);
        layout->addWidget(button);
        toolButtons_.append(button);

        connect(button, &QToolButton::clicked, this, [this, tool]() {
            if (tool == Tool::Arc) {
                // A normal click always returns to the default arc command.
                viewport_->setArcMode(ArcMode::OnePoint);
            }
            viewport_->setTool(tool);
            statusBar()->showMessage(QStringLiteral("Active tool: %1").arg(toolName(tool)));
        });

        return button;
    }

    void activateArcMode(ArcMode mode)
    {
        viewport_->setArcMode(mode);
        viewport_->setTool(Tool::Arc);
        if (arcToolButton_ != nullptr) {
            arcToolButton_->setChecked(true);
        }
        statusBar()->showMessage(QStringLiteral("Active tool: %1").arg(arcModeName(mode)));
    }

    void createArcToolMenu(QToolButton *button)
    {
        if (button == nullptr) {
            return;
        }

        auto *menu = new QMenu(button);
        QAction *onePointAction = menu->addAction(QStringLiteral("1 Point Arc"));
        QAction *twoPointAction = menu->addAction(QStringLiteral("2 Point Arc"));
        button->setMenu(menu);
        // A quick click runs the default 1 Point Arc. Holding the button
        // opens this menu, matching the tool-flyout behavior requested here.
        button->setPopupMode(QToolButton::DelayedPopup);

        connect(onePointAction, &QAction::triggered, this, [this]() {
            activateArcMode(ArcMode::OnePoint);
        });
        connect(twoPointAction, &QAction::triggered, this, [this]() {
            activateArcMode(ArcMode::TwoPoint);
        });
    }

    void loadPreferences()
    {
        QSettings settings;
        const QString savedPanButton = settings.value(QStringLiteral("keymap/panButton"),
                                                      QStringLiteral("middle"))
                                           .toString();
        applyPanButton(savedPanButton == QStringLiteral("right") ? Qt::RightButton
                                                                   : Qt::MiddleButton,
                       false);

        if (orthoAction_ != nullptr) {
            orthoAction_->setChecked(settings.value(QStringLiteral("modeling/orthoEnabled"), false)
                                         .toBool());
        }

        if (endpointSnapCheckBox_ != nullptr) {
            endpointSnapCheckBox_->setChecked(settings.value(QStringLiteral("osnap/endpoint"), true)
                                                  .toBool());
            midpointSnapCheckBox_->setChecked(settings.value(QStringLiteral("osnap/midpoint"), true)
                                                  .toBool());
            intersectionSnapCheckBox_->setChecked(
                settings.value(QStringLiteral("osnap/intersection"), true).toBool());
            centerSnapCheckBox_->setChecked(settings.value(QStringLiteral("osnap/center"), true)
                                                .toBool());
            perpendicularSnapCheckBox_->setChecked(
                settings.value(QStringLiteral("osnap/perpendicular"), false).toBool());
            tangentSnapCheckBox_->setChecked(
                settings.value(QStringLiteral("osnap/tangent"), false).toBool());
        }

        if (osnapAction_ != nullptr) {
            osnapAction_->setChecked(settings.value(QStringLiteral("osnap/enabled"), false)
                                         .toBool());
        }

        if (controlPointsButton_ != nullptr) {
            controlPointsButton_->setChecked(
                settings.value(QStringLiteral("view/controlPoints"), false).toBool());
        }
    }

    void openPreferences()
    {
        PreferencesDialog dialog(viewport_->panButton(), this);
        if (dialog.exec() == QDialog::Accepted) {
            applyPanButton(dialog.panButton(), true);
        }
    }

    void applyPanButton(Qt::MouseButton button, bool save)
    {
        viewport_->setPanButton(button);
        updateToolHelp();

        if (save) {
            QSettings settings;
            settings.setValue(QStringLiteral("keymap/panButton"),
                              button == Qt::RightButton ? QStringLiteral("right")
                                                        : QStringLiteral("middle"));
            settings.sync();
            statusBar()->showMessage(QStringLiteral("Viewport pan: %1").arg(mouseButtonName(button)));
        }
    }

    void updateToolHelp()
    {
        if (toolHelp_ != nullptr && viewport_ != nullptr) {
            toolHelp_->setText(QStringLiteral("LMB\nDraw\n\n%1\nPan\n\nWheel\nZoom")
                                   .arg(mouseButtonName(viewport_->panButton())));
        }
    }

    QWidget *createRightPanel()
    {
        auto *panel = new QFrame;
        panel->setObjectName(QStringLiteral("rightPanel"));
        panel->setFixedWidth(286);

        auto *layout = new QVBoxLayout(panel);
        layout->setContentsMargins(8, 8, 8, 8);
        layout->setSpacing(8);

        auto *outlinerBox = new QGroupBox(QStringLiteral("Outliner"));
        auto *outlinerLayout = new QVBoxLayout(outlinerBox);
        auto *items = new QListWidget;
        items->addItems({QStringLiteral("Document"),
                         QStringLiteral("  Sketch"),
                         QStringLiteral("    Grid"),
                         QStringLiteral("    Curves")});
        outlinerLayout->addWidget(items);
        layout->addWidget(outlinerBox, 1);

        auto *propertiesBox = new QGroupBox(QStringLiteral("Properties"));
        auto *propertiesLayout = new QFormLayout(propertiesBox);
        propertiesLayout->addRow(QStringLiteral("Active tool"), new QLabel(QStringLiteral("Select")));
        propertiesLayout->addRow(QStringLiteral("Selection"), new QLabel(QStringLiteral("None")));
        propertiesLayout->addRow(QStringLiteral("Degree"), new QLabel(QStringLiteral("3")));
        propertiesLayout->addRow(QStringLiteral("Units"), new QLabel(QStringLiteral("Millimeters")));
        layout->addWidget(propertiesBox);

        auto *notes = new QLabel(QStringLiteral("NURBS geometry and command history will appear here."));
        notes->setObjectName(QStringLiteral("panelHint"));
        notes->setWordWrap(true);
        layout->addWidget(notes);

        return panel;
    }

    void applyTheme()
    {
        setStyleSheet(QStringLiteral(R"(
            QMainWindow, QWidget {
                background: #282828;
                color: #d6d6d6;
                font-family: "Sans";
                font-size: 11px;
            }
            QMenuBar {
                background: #202020;
                border-bottom: 1px solid #121212;
                padding: 2px 4px;
            }
            QMenuBar::item {
                padding: 5px 9px;
                background: transparent;
            }
            QMenuBar::item:selected, QMenu::item:selected {
                background: #4a4a4a;
            }
            QMenu {
                background: #303030;
                border: 1px solid #151515;
            }
            QMenu::item {
                padding: 5px 28px 5px 12px;
            }
            QToolBar#workspaceBar {
                background: #242424;
                border: 0;
                border-bottom: 1px solid #171717;
                spacing: 5px;
                padding: 4px 8px;
            }
            QLabel#brand {
                color: #f0a45a;
                font-size: 15px;
                font-weight: bold;
                padding-right: 8px;
            }
            QLabel#modeLabel {
                color: #999999;
                padding-left: 12px;
            }
            QToolButton#workspaceButton {
                border: 1px solid transparent;
                border-radius: 3px;
                padding: 5px 10px;
            }
            QToolButton#workspaceButton:checked,
            QToolButton#workspaceButton:hover {
                background: #454545;
                border-color: #5d5d5d;
            }
            QToolBar#osnapLane {
                background: #232323;
                border-top: 1px solid #151515;
                border-bottom: 1px solid #151515;
                spacing: 4px;
                padding: 3px 8px;
            }
            QLabel#osnapLaneLabel {
                color: #777777;
                font-size: 9px;
                font-weight: bold;
                padding-right: 6px;
            }
            QCheckBox#osnapCheckBox {
                color: #c7c7c7;
                spacing: 5px;
                padding: 3px 7px;
                border-radius: 3px;
            }
            QCheckBox#osnapCheckBox:hover {
                background: #414141;
            }
            QCheckBox#osnapCheckBox::indicator {
                width: 13px;
                height: 13px;
                background: #303030;
                border: 1px solid #686868;
                border-radius: 2px;
            }
            QCheckBox#osnapCheckBox::indicator:checked {
                background: #537da0;
                border-color: #82c7ec;
            }
            QFrame#toolShelf, QFrame#rightPanel {
                background: #232323;
                border: 0;
            }
            QFrame#toolShelf {
                border-right: 1px solid #151515;
            }
            QFrame#rightPanel {
                border-left: 1px solid #151515;
            }
            QLabel#shelfLabel {
                color: #777777;
                font-size: 9px;
                font-weight: bold;
                padding-bottom: 4px;
            }
            QToolButton#toolButton {
                background: #303030;
                border: 1px solid #3b3b3b;
                border-radius: 3px;
                color: #c7c7c7;
                min-height: 43px;
                padding: 3px 1px;
            }
            QToolButton#toolButton:hover {
                background: #414141;
                border-color: #686868;
            }
            QToolButton#toolButton:checked {
                background: #9b5b2e;
                border-color: #e39a54;
                color: #ffffff;
            }
            QLabel#toolHelp, QLabel#panelHint {
                color: #777777;
            }
            QGroupBox {
                border: 1px solid #3d3d3d;
                border-radius: 3px;
                margin-top: 8px;
                padding: 8px 6px 6px 6px;
                font-weight: bold;
                color: #bbbbbb;
            }
            QGroupBox::title {
                subcontrol-origin: margin;
                left: 8px;
                padding: 0 4px;
            }
            QListWidget {
                background: #2b2b2b;
                border: 0;
                padding: 3px;
            }
            QListWidget::item {
                padding: 4px;
            }
            QListWidget::item:selected {
                background: #5a3824;
            }
            QListWidget#preferencesCategories {
                background: #3a3a3a;
                border: 0;
                padding: 2px;
            }
            QListWidget#preferencesCategories::item {
                padding: 7px 6px;
                border-bottom: 1px solid #454545;
            }
            QListWidget#preferencesCategories::item:selected {
                background: #537db5;
                color: #ffffff;
            }
            QLabel#preferencesTitle {
                color: #eeeeee;
                font-size: 16px;
                font-weight: bold;
                padding-bottom: 6px;
            }
            QLabel#preferencesHint {
                color: #999999;
            }
            QDialog QGroupBox {
                background: #303030;
            }
            QToolButton#statusToggle {
                background: transparent;
                border: 1px solid transparent;
                border-radius: 3px;
                color: #999999;
                padding: 2px 9px;
                margin: 1px 3px;
            }
            QToolButton#statusToggle:hover {
                background: #3d3d3d;
            }
            QToolButton#statusToggle:checked {
                background: #9b5b2e;
                border-color: #e39a54;
                color: #ffffff;
            }
            QStatusBar {
                background: #202020;
                color: #999999;
                border-top: 1px solid #151515;
            }
        )"));
    }

    ViewportWidget *viewport_ = nullptr;
    QLabel *coordinateLabel_ = nullptr;
    QLabel *toolHelp_ = nullptr;
    QToolButton *selectToolButton_ = nullptr;
    QToolButton *arcToolButton_ = nullptr;
    QToolButton *controlPointsButton_ = nullptr;
    QToolButton *subdivideButton_ = nullptr;
    QVector<QToolButton *> toolButtons_;
    QAction *undoAction_ = nullptr;
    QAction *redoAction_ = nullptr;
    QAction *subdivideAction_ = nullptr;
    QAction *updateAction_ = nullptr;
    QAction *orthoAction_ = nullptr;
    QAction *osnapAction_ = nullptr;
    QCheckBox *endpointSnapCheckBox_ = nullptr;
    QCheckBox *midpointSnapCheckBox_ = nullptr;
    QCheckBox *intersectionSnapCheckBox_ = nullptr;
    QCheckBox *centerSnapCheckBox_ = nullptr;
    QCheckBox *perpendicularSnapCheckBox_ = nullptr;
    QCheckBox *tangentSnapCheckBox_ = nullptr;
    QToolBar *osnapLane_ = nullptr;
    QProcess *updateProcess_ = nullptr;
};

} // namespace

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("classiCAD"));
    application.setOrganizationName(QStringLiteral("classiCAD"));

    QString updateSessionPath;
    const QStringList arguments = application.arguments();
    for (int index = 1; index + 1 < arguments.size(); ++index) {
        if (arguments[index] == QStringLiteral("--update-session")) {
            updateSessionPath = arguments[index + 1];
            break;
        }
    }

    DebugLog::instance().write(QStringLiteral("application start logFile=%1")
                                   .arg(DebugLog::instance().path()));

    MainWindow window;
    if (!updateSessionPath.isEmpty()) {
        window.restoreUpdateSession(updateSessionPath);
    }
    window.show();

    return application.exec();
}
