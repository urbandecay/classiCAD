#include <QApplication>
#include <QButtonGroup>
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

        if (lineCommandActive_) {
            const QPoint localCursor = mapFromGlobal(QCursor::pos());
            if (rect().contains(localCursor)) {
                cursorWorld_ = screenToWorld(localCursor);
                cursorValid_ = true;
            }
        }

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

        for (const Shape &shape : shapes_) {
            drawShape(painter, shape, false);
        }

        if (activeTool_ == Tool::Line && lineCommandActive_) {
            drawLineToolPreview(painter);
        } else if (!pendingPoints_.isEmpty()) {
            drawShape(painter, Shape{activeTool_, pendingPoints_, Shape::NurbsCurve2D{}}, true);
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
        const QPointF worldPosition = screenToWorld(screenPosition);
        DebugLog::instance().write(
            QStringLiteral("mousePress button=%1 screen=%2 world=%3 tool=%4 lineActive=%5 panButton=%6 modifiers=0x%7")
                .arg(inputButtonName(event->button()))
                .arg(pointText(screenPosition))
                .arg(pointText(worldPosition))
                .arg(toolName(activeTool_))
                .arg(lineCommandActive_)
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

        if (event->button() != Qt::LeftButton || activeTool_ == Tool::Select) {
            DebugLog::instance().write(QStringLiteral("mousePress branch=ignored"));
            return;
        }

        lastWorldPosition_ = worldPosition;
        cursorWorld_ = lastWorldPosition_;
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
        }

        update();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const QPointF screenPosition = eventPosition(event);
        lastWorldPosition_ = screenToWorld(screenPosition);
        cursorWorld_ = lastWorldPosition_;
        cursorValid_ = true;

        if (panning_) {
            const QPoint current = screenPosition.toPoint();
            const QPoint delta = current - lastMousePosition_;
            pan_ += QPointF(delta.x() / zoom_, -delta.y() / zoom_);
            lastMousePosition_ = current;
        }

        if (lineCommandActive_ || panning_) {
            update();
        }

        if (lineCommandActive_ || panning_) {
            DebugLog::instance().write(
                QStringLiteral("mouseMove screen=%1 world=%2 lineActive=%3 points=%4 panning=%5 pan=%6 zoom=%7 buttons=0x%8")
                    .arg(pointText(screenPosition))
                    .arg(pointText(lastWorldPosition_))
                    .arg(lineCommandActive_)
                    .arg(pendingPoints_.size())
                    .arg(panning_)
                    .arg(pointText(pan_))
                    .arg(zoom_, 0, 'f', 4)
                    .arg(static_cast<int>(event->buttons()), 0, 16));
        }

        emitCoordinateUpdate();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        DebugLog::instance().write(QStringLiteral("mouseRelease button=%1 screen=%2 panningBefore=%3")
                                       .arg(inputButtonName(event->button()))
                                       .arg(pointText(eventPosition(event)))
                                       .arg(panning_));
        if (panning_ && (event->button() == panButton_ || event->button() == Qt::LeftButton)) {
            panning_ = false;
            setCursor(activeTool_ == Tool::Select ? Qt::ArrowCursor : Qt::CrossCursor);
            DebugLog::instance().write(QStringLiteral("mouseRelease branch=end-pan"));
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
            lineCommandActive_ = false;
            DebugLog::instance().write(QStringLiteral("keyPress branch=cancel-input"));
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
        lineCommandActive_ = false;
        setCursor(Qt::ArrowCursor);
        DebugLog::instance().write(QStringLiteral("finishLineCommand end lineActive=%1 points=%2")
                                       .arg(lineCommandActive_)
                                       .arg(pendingPoints_.size()));
        update();
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

    void drawShape(QPainter &painter, const Shape &shape, bool preview)
    {
        if (shape.points.isEmpty()) {
            return;
        }

        const QColor curveColor = preview ? QColor(QStringLiteral("#e6b85c"))
                                         : QColor(QStringLiteral("#d28b45"));
        const QColor controlColor = QColor(QStringLiteral("#8aa7c7"));

        painter.setPen(QPen(curveColor, preview ? 1.5 : 2.0));

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
            painter.setPen(QPen(curveColor, preview ? 1.5 : 2.0));
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

private:
    Tool activeTool_ = Tool::Select;
    QVector<Shape> shapes_;
    QVector<QPointF> pendingPoints_;
    QPointF pan_{0.0, 0.0};
    QPointF lastWorldPosition_{0.0, 0.0};
    QPoint lastMousePosition_;
    QPointF cursorWorld_{0.0, 0.0};
    qreal zoom_ = 1.0;
    bool panning_ = false;
    Qt::MouseButton panButton_ = Qt::MiddleButton;
    bool lineCommandActive_ = false;
    bool cursorValid_ = false;
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
        rootLayout->addWidget(viewport_, 1);

        rootLayout->addWidget(createRightPanel());
        setCentralWidget(root);

        coordinateLabel_ = new QLabel(QStringLiteral("X 0.00   Y 0.00   Zoom 100%"));
        statusBar()->addWidget(coordinateLabel_);
        statusBar()->addPermanentWidget(new QLabel(QStringLiteral("Ready")));

        viewport_->coordinateUpdate_ = [this](const QString &text) {
            coordinateLabel_->setText(text);
        };
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

        addToolButton(layout, group, QStringLiteral("↖\nSelect"), Tool::Select, true);
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

    void addToolButton(QVBoxLayout *layout,
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
