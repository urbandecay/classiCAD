#include <QApplication>
#include <QAction>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QCursor>
#include <QDateTime>
#include <QDebug>
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
#include <QListWidget>
#include <QMenu>
#include <QMainWindow>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPushButton>
#include <QSettings>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTextStream>
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
};

enum class SnapType {
    None,
    Endpoint,
    Midpoint,
    Intersection,
    Center,
    Perpendicular,
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
        int degree = 1;
        QVector<QPointF> controlPoints;
        QVector<double> weights;
        QVector<double> knots;
    } nurbs;
};

Shape::NurbsCurve2D makeDegreeOneNurbs(const QVector<QPointF> &points)
{
    Shape::NurbsCurve2D curve;
    curve.controlPoints = points;
    curve.weights.fill(1.0, points.size());

    if (points.size() < 2) {
        return curve;
    }

    // An open, clamped degree-1 curve represents the same connected
    // segments as a Rhino-style polyline while remaining NURBS data.
    const int pointCount = points.size();
    curve.knots.reserve(pointCount + 2);
    for (int i = 0; i < pointCount + 2; ++i) {
        if (i < 2) {
            curve.knots.append(0.0);
        } else if (i >= pointCount) {
            curve.knots.append(pointCount - 1.0);
        } else {
            curve.knots.append(i - 1.0);
        }
    }

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
    }

    return QStringLiteral("Unknown");
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
        activeTool_ = tool;
        pendingPoints_.clear();
        lineCommandActive_ = tool == Tool::Line;

        if (tool != Tool::Select) {
            selectedShapeIndex_ = -1;
            draggingSelected_ = false;
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

    Tool activeTool() const
    {
        return activeTool_;
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
                      bool perpendicular)
    {
        endpointSnapEnabled_ = endpoint;
        midpointSnapEnabled_ = midpoint;
        intersectionSnapEnabled_ = intersection;
        centerSnapEnabled_ = center;
        perpendicularSnapEnabled_ = perpendicular;
        refreshCursorConstraint();
        DebugLog::instance().write(QStringLiteral("setSnapModes endpoint=%1 midpoint=%2 intersection=%3 center=%4 perpendicular=%5 snap=%6")
                                       .arg(endpointSnapEnabled_)
                                       .arg(midpointSnapEnabled_)
                                       .arg(intersectionSnapEnabled_)
                                       .arg(centerSnapEnabled_)
                                       .arg(perpendicularSnapEnabled_)
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
        }

        if (activeTool_ == Tool::Line && lineCommandActive_) {
            drawLineToolPreview(painter);
        } else if (activeTool_ == Tool::Circle && !pendingPoints_.isEmpty()) {
            drawCircleToolPreview(painter);
        } else if (!pendingPoints_.isEmpty()) {
            drawShape(painter, Shape{activeTool_, pendingPoints_, Shape::NurbsCurve2D{}}, true);
        }

        if (draggingSelected_ && currentDragSnap_.isValid()) {
            drawSnapMarker(painter,
                           currentDragSnap_.type,
                           currentDragSnap_.targetPoint);
        }

        painter.setPen(QColor(QStringLiteral("#a0a0a0")));
        painter.setFont(QFont(QStringLiteral("Sans"), 10));
        painter.drawText(18, 28, QStringLiteral("2D VIEWPORT  •  %1").arg(toolName(activeTool_)));

        if (activeTool_ == Tool::Line && lineCommandActive_) {
            painter.setPen(QColor(QStringLiteral("#777777")));
            painter.drawText(18,
                             height() - 18,
                             QStringLiteral("Click to place connected points  •  Right-click to finish"));
        } else if (activeTool_ != Tool::Select) {
            const QString hint = QStringLiteral("Click to place %1 point%2  •  Esc clears current tool input")
                                     .arg(toolName(activeTool_).toLower())
                                     .arg(requiredPoints(activeTool_) == 1 ? QString() : QStringLiteral("s"));
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
            QStringLiteral("mousePress button=%1 screen=%2 worldRaw=%3 worldUsed=%4 tool=%5 lineActive=%6 ortho=%7 panButton=%8 modifiers=0x%9")
                .arg(inputButtonName(event->button()))
                .arg(pointText(screenPosition))
                .arg(pointText(rawWorldPosition))
                .arg(pointText(worldPosition))
                .arg(toolName(activeTool_))
                .arg(lineCommandActive_)
                .arg(orthoEnabled_)
                .arg(inputButtonName(panButton_))
                .arg(static_cast<int>(event->modifiers()), 0, 16));

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

            selectedShapeIndex_ = hitTestShape(screenPosition);
            draggingSelected_ = selectedShapeIndex_ >= 0;
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

        pendingPoints_.append(lastWorldPosition_);

        if (pendingPoints_.size() == requiredPoints(activeTool_)) {
            shapes_.append(Shape{activeTool_, pendingPoints_, Shape::NurbsCurve2D{}});
            DebugLog::instance().write(QStringLiteral("placeholder shape committed tool=%1 points=%2")
                                           .arg(toolName(activeTool_))
                                           .arg(pendingPoints_.size()));
            pendingPoints_.clear();

            if (activeTool_ == Tool::Circle) {
                setTool(Tool::Select);
                if (commandFinished_) {
                    commandFinished_(Tool::Select);
                }
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
        const bool circlePreviewActive = activeTool_ == Tool::Circle && !pendingPoints_.isEmpty();

        if (panning_) {
            const QPoint current = screenPosition.toPoint();
            const QPoint delta = current - lastMousePosition_;
            pan_ += QPointF(delta.x() / zoom_, -delta.y() / zoom_);
            lastMousePosition_ = current;
        }

        if (draggingSelected_ && selectedShapeIndex_ >= 0 &&
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
                        translateShape(selectedShapeIndex_, detachDelta);
                        currentDragSnap_ = DragSnapResult{};
                        dragSnapLocked_ = false;
                        DebugLog::instance().write(
                            QStringLiteral("selection drag snap-breakaway shape=%1 cursorDistance=%2")
                                .arg(selectedShapeIndex_)
                                .arg(cursorDistanceFromSnap, 0, 'f', 2));
                    } else {
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

        if (lineCommandActive_ || circlePreviewActive || panning_ || draggingSelected_) {
            update();
        }

        if (lineCommandActive_ || circlePreviewActive || panning_ || draggingSelected_) {
            DebugLog::instance().write(
                QStringLiteral("mouseMove screen=%1 worldRaw=%2 worldUsed=%3 lineActive=%4 points=%5 panning=%6 dragging=%7 ortho=%8 pan=%9 zoom=%10 buttons=0x%11")
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
                    .arg(static_cast<int>(event->buttons()), 0, 16));
        }

        emitCoordinateUpdate();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        DebugLog::instance().write(QStringLiteral("mouseRelease button=%1 screen=%2 panningBefore=%3 draggingBefore=%4")
                                       .arg(inputButtonName(event->button()))
                                       .arg(pointText(eventPosition(event)))
                                       .arg(panning_)
                                       .arg(draggingSelected_));
        if (panning_ && (event->button() == panButton_ || event->button() == Qt::LeftButton)) {
            panning_ = false;
            setCursor(activeTool_ == Tool::Select ? Qt::ArrowCursor : Qt::CrossCursor);
            DebugLog::instance().write(QStringLiteral("mouseRelease branch=end-pan"));
        }

        if (draggingSelected_ && event->button() == Qt::LeftButton) {
            draggingSelected_ = false;
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
        if (event->key() == Qt::Key_Escape) {
            pendingPoints_.clear();
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
    void finishLineCommand()
    {
        DebugLog::instance().write(QStringLiteral("finishLineCommand begin points=%1")
                                       .arg(pendingPoints_.size()));
        if (pendingPoints_.size() >= 2) {
            const Shape::NurbsCurve2D curve = makeDegreeOneNurbs(pendingPoints_);
            shapes_.append(Shape{Tool::Line, pendingPoints_, curve});
            DebugLog::instance().write(
                QStringLiteral("finishLineCommand committed controlPoints=%1 degree=%2 weights=%3 knots=%4 shapes=%5")
                    .arg(curve.controlPoints.size())
                    .arg(curve.degree)
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

    QVector<SnapCandidate> snapCandidatesForShape(const Shape &shape) const
    {
        QVector<SnapCandidate> candidates;
        if (shape.points.isEmpty()) {
            return candidates;
        }

        if (shape.tool == Tool::Circle) {
            candidates.append(SnapCandidate{SnapType::Center, shape.points.first()});
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

            if (shape.tool == Tool::Circle) {
                if (centerSnapEnabled_) {
                    candidates.append(SnapCandidate{SnapType::Center, shape.points.first()});
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

    SnapResult findSnapPoint(const QPointF &rawPoint) const
    {
        SnapResult best;
        if (!osnapEnabled_ || activeTool_ != Tool::Line || !lineCommandActive_) {
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

        if (!orthoEnabled_ || panning_ || activeTool_ != Tool::Line ||
            !lineCommandActive_ || pendingPoints_.isEmpty()) {
            return rawPoint;
        }

        const QPointF origin = pendingPoints_.back();
        const qreal deltaX = rawPoint.x() - origin.x();
        const qreal deltaY = rawPoint.y() - origin.y();

        if (std::abs(deltaX) >= std::abs(deltaY)) {
            return QPointF(rawPoint.x(), origin.y());
        }

        return QPointF(origin.x(), rawPoint.y());
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

    int hitTestShape(const QPointF &screenPosition) const
    {
        constexpr qreal hitRadiusPixels = 9.0;
        int closestShape = -1;
        qreal closestDistance = hitRadiusPixels;

        for (int index = 0; index < shapes_.size(); ++index) {
            const Shape &shape = shapes_[index];

            if (shape.tool == Tool::Circle && shape.points.size() >= 2) {
                const QPointF center = worldToScreen(shape.points[0]);
                const QPointF edge = worldToScreen(shape.points[1]);
                const qreal radius = std::hypot(edge.x() - center.x(), edge.y() - center.y());
                const qreal distanceFromCenter =
                    std::hypot(screenPosition.x() - center.x(),
                               screenPosition.y() - center.y());
                const qreal distanceFromCircle = std::abs(distanceFromCenter - radius);

                if (distanceFromCircle <= closestDistance) {
                    closestDistance = distanceFromCircle;
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

        if (shape.tool == Tool::Line && shape.points.size() >= 2) {
            for (int i = 0; i + 1 < shape.points.size(); ++i) {
                painter.drawLine(worldToScreen(shape.points[i]),
                                 worldToScreen(shape.points[i + 1]));
            }
        } else if (shape.tool == Tool::Rectangle && shape.points.size() >= 2) {
            const QRectF rectangle(worldToScreen(shape.points[0]), worldToScreen(shape.points[1]));
            painter.drawRect(rectangle.normalized());
        } else if (shape.tool == Tool::Circle && shape.points.size() >= 2) {
            const QPointF center = worldToScreen(shape.points[0]);
            const QPointF edge = worldToScreen(shape.points[1]);
            const qreal radius = std::hypot(edge.x() - center.x(), edge.y() - center.y());
            painter.drawEllipse(center, radius, radius);
        } else if (shape.tool == Tool::Arc && shape.points.size() >= 3) {
            QPainterPath path;
            path.moveTo(worldToScreen(shape.points[0]));
            path.quadTo(worldToScreen(shape.points[1]), worldToScreen(shape.points[2]));
            painter.drawPath(path);
        } else if ((shape.tool == Tool::Bezier || shape.tool == Tool::Nurbs) &&
                   shape.points.size() >= 4) {
            painter.setPen(QPen(controlColor, 1, Qt::DashLine));
            for (int i = 0; i + 1 < shape.points.size(); ++i) {
                painter.drawLine(worldToScreen(shape.points[i]), worldToScreen(shape.points[i + 1]));
            }

            QPainterPath curve;
            curve.moveTo(worldToScreen(shape.points[0]));
            curve.cubicTo(worldToScreen(shape.points[1]),
                          worldToScreen(shape.points[2]),
                          worldToScreen(shape.points[3]));
            painter.setPen(QPen(curveColor, curveWidth));
            painter.drawPath(curve);
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

private:
    Tool activeTool_ = Tool::Select;
    QVector<Shape> shapes_;
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
    bool dragSnapLocked_ = false;
    QPointF dragSnapCursorWorld_{0.0, 0.0};
    QPointF lastDragWorld_{0.0, 0.0};
    qreal zoom_ = 1.0;
    bool panning_ = false;
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

private:
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
        editMenu->addAction(QStringLiteral("Undo"));
        editMenu->addAction(QStringLiteral("Redo"));
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

        for (QCheckBox *checkBox : {endpointSnapCheckBox_,
                                    midpointSnapCheckBox_,
                                    intersectionSnapCheckBox_,
                                    centerSnapCheckBox_,
                                    perpendicularSnapCheckBox_}) {
            checkBox->setObjectName(QStringLiteral("osnapCheckBox"));
            checkBox->setChecked(true);
            osnapLane_->addWidget(checkBox);
        }
        perpendicularSnapCheckBox_->setChecked(false);

        const auto syncSnapModes = [this]() {
            viewport_->setSnapModes(endpointSnapCheckBox_->isChecked(),
                                    midpointSnapCheckBox_->isChecked(),
                                    intersectionSnapCheckBox_->isChecked(),
                                    centerSnapCheckBox_->isChecked(),
                                    perpendicularSnapCheckBox_->isChecked());
            QSettings settings;
            settings.setValue(QStringLiteral("osnap/endpoint"), endpointSnapCheckBox_->isChecked());
            settings.setValue(QStringLiteral("osnap/midpoint"), midpointSnapCheckBox_->isChecked());
            settings.setValue(QStringLiteral("osnap/intersection"),
                              intersectionSnapCheckBox_->isChecked());
            settings.setValue(QStringLiteral("osnap/center"), centerSnapCheckBox_->isChecked());
            settings.setValue(QStringLiteral("osnap/perpendicular"),
                              perpendicularSnapCheckBox_->isChecked());
            settings.sync();
        };

        connect(endpointSnapCheckBox_, &QCheckBox::toggled, this, syncSnapModes);
        connect(midpointSnapCheckBox_, &QCheckBox::toggled, this, syncSnapModes);
        connect(intersectionSnapCheckBox_, &QCheckBox::toggled, this, syncSnapModes);
        connect(centerSnapCheckBox_, &QCheckBox::toggled, this, syncSnapModes);
        connect(perpendicularSnapCheckBox_, &QCheckBox::toggled, this, syncSnapModes);

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
        addToolButton(layout, group, QStringLiteral("╱\nLine"), Tool::Line);
        addToolButton(layout, group, QStringLiteral("⌒\nArc"), Tool::Arc);
        addToolButton(layout, group, QStringLiteral("∿\nBezier"), Tool::Bezier);
        addToolButton(layout, group, QStringLiteral("N\nNURBS"), Tool::Nurbs);
        addToolButton(layout, group, QStringLiteral("□\nRect"), Tool::Rectangle);
        addToolButton(layout, group, QStringLiteral("○\nCircle"), Tool::Circle);

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

        connect(button, &QToolButton::clicked, this, [this, tool]() {
            viewport_->setTool(tool);
            statusBar()->showMessage(QStringLiteral("Active tool: %1").arg(toolName(tool)));
        });

        return button;
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
        }

        if (osnapAction_ != nullptr) {
            osnapAction_->setChecked(settings.value(QStringLiteral("osnap/enabled"), false)
                                         .toBool());
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
    QAction *orthoAction_ = nullptr;
    QAction *osnapAction_ = nullptr;
    QCheckBox *endpointSnapCheckBox_ = nullptr;
    QCheckBox *midpointSnapCheckBox_ = nullptr;
    QCheckBox *intersectionSnapCheckBox_ = nullptr;
    QCheckBox *centerSnapCheckBox_ = nullptr;
    QCheckBox *perpendicularSnapCheckBox_ = nullptr;
    QToolBar *osnapLane_ = nullptr;
};

} // namespace

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("classiCAD"));
    application.setOrganizationName(QStringLiteral("classiCAD"));

    DebugLog::instance().write(QStringLiteral("application start logFile=%1")
                                   .arg(DebugLog::instance().path()));

    MainWindow window;
    window.show();

    return application.exec();
}
