#include "ui/viewport_widget_api.h"
#include "ui/viewport/blender_grid_renderer.h"
#include "ui/viewport/viewport_gpu_surface.h"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <memory>

using namespace classiCAD;

namespace {

bool check(bool condition, const char *message)
{
    if (!condition) {
        qCritical("%s", message);
    }
    return condition;
}

void sendMouse(ViewportWidgetApi *viewport,
               QEvent::Type type,
               const QPointF &position,
               Qt::MouseButton button,
               Qt::MouseButtons buttons,
               Qt::KeyboardModifiers modifiers)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QMouseEvent event(type, position, position, button, buttons, modifiers);
#else
    QMouseEvent event(type, position, button, buttons, modifiers);
#endif
    QApplication::sendEvent(viewport, &event);
}

void sendWheel(ViewportWidgetApi *viewport,
               const QPointF &position,
               int angleDeltaY)
{
    QWheelEvent event(position,
                      viewport->mapToGlobal(position.toPoint()),
                      QPoint(),
                      QPoint(0, angleDeltaY),
                      Qt::NoButton,
                      Qt::NoModifier,
                      Qt::ScrollUpdate,
                      false);
    QApplication::sendEvent(viewport, &event);
}

QImage captureViewport(ViewportWidgetApi *viewport)
{
    if (auto *surface = viewport->findChild<ViewportGpuSurface *>()) {
        return surface->grabFramebuffer();
    }
    return viewport->grab().toImage();
}

int medianNeutralGray(const QImage &image, const QPoint &center, int radius)
{
    QVector<int> samples;
    for (int y = std::max(0, center.y() - radius);
         y <= std::min(image.height() - 1, center.y() + radius);
         ++y) {
        for (int x = std::max(0, center.x() - radius);
             x <= std::min(image.width() - 1, center.x() + radius);
             ++x) {
            const QColor color = image.pixelColor(x, y);
            const int minimum = std::min({color.red(), color.green(), color.blue()});
            const int maximum = std::max({color.red(), color.green(), color.blue()});
            if (maximum - minimum <= 8) {
                samples.append((color.red() + color.green() + color.blue()) / 3);
            }
        }
    }
    if (samples.isEmpty()) {
        return -1;
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

void saveGridCapture(const QString &name, const QImage &image)
{
    const QString captureRoot = qEnvironmentVariable(
        "CLASSICAD_VIEWPORT_CAPTURE_DIR");
    if (captureRoot.isEmpty()) {
        return;
    }

    QDir captureDirectory(captureRoot);
    if (!captureDirectory.exists() && !captureDirectory.mkpath(QStringLiteral("."))) {
        qWarning() << "Could not create viewport capture directory:" << captureRoot;
        return;
    }
    if (image.isNull() ||
        !image.save(captureDirectory.filePath(name + QStringLiteral(".png")))) {
        qWarning() << "Could not save viewport grid capture:" << name;
    }
}

void waitForViewportTransition()
{
    QEventLoop eventLoop;
    QTimer::singleShot(250, &eventLoop, &QEventLoop::quit);
    eventLoop.exec();
}

} // namespace

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    bool passed = true;

    const QSize interactionViewportSize(640, 480);
    const QSize blenderReferenceViewportSize(591, 511);
    const ViewportCameraPreferences blenderCameraPreferences{
        50.0, 0.01, 1000.0};
    const BlenderGridAppearance blenderGridAppearance{};
    passed &= check(
        blenderGridAppearance.gridColor == QColor::fromRgb(84, 84, 84, 128) &&
            blenderGridAppearance.emphasisColor ==
                QColor::fromRgb(84, 84, 84, 255) &&
            blenderGridAppearance.axisYColor == QColor::fromRgb(109, 176, 23, 235),
        "grid and Y-axis defaults must match Blender's saved theme appearance");

    std::unique_ptr<ViewportWidgetApi> viewport(createViewportWidget());
    viewport->resize(interactionViewportSize);
    viewport->show();
    application.processEvents();

    passed &= check(viewport->viewportAntiAliasingSamples() == 8,
                    "viewport anti-aliasing must default to the saved Blender 8x setting");
    viewport->setViewportAntiAliasingSamples(4);
    passed &= check(viewport->viewportAntiAliasingSamples() == 4,
                    "viewport anti-aliasing preference must update the OpenGL renderer");
    viewport->setViewportAntiAliasingSamples(8);
    passed &= check(viewport->setCameraPreferences(blenderCameraPreferences),
                    "Blender comparison camera preferences must be accepted by the viewport");
    const ViewportCameraPreferences appliedCameraPreferences =
        viewport->cameraPreferences();
    passed &= check(
        std::abs(appliedCameraPreferences.focalLengthMillimeters - 50.0) < 1.0e-9 &&
            std::abs(appliedCameraPreferences.clipStart - 0.01) < 1.0e-9 &&
            std::abs(appliedCameraPreferences.clipEnd - 1000.0) < 1.0e-9 &&
            viewport->viewportAntiAliasingSamples() == 8,
        "Blender comparison must use a 50 mm lens, 0.01/1000 clipping, and 8x AA");
    passed &= check(
        std::abs(viewport->documentSettings().gridSpacing - 1.0) < 1.0e-9,
        "Blender comparison must use the same 1-unit base grid spacing");
    viewport->setGridAppearance(blenderGridAppearance);

    const QPointF center(320.0, 240.0);
    if (QApplication::platformName() == QStringLiteral("xcb") &&
        !qEnvironmentVariable("CLASSICAD_VIEWPORT_CAPTURE_DIR").isEmpty()) {
        viewport->resize(blenderReferenceViewportSize);
        application.processEvents();
        waitForViewportTransition();

        // Blender's top-view RegionView3D distance is not the same quantity as
        // classiCAD's direct orthographic pixels-per-unit zoom. The current
        // reference captures show about 68 px between Blender's major lines
        // versus 100 px in classiCAD at zoom 1.0. This fractional smooth-wheel
        // event produces a test-only ~0.681 zoom, matching that framing; the
        // inverse event restores the original state before perspective capture.
        const QPointF comparisonCenter(blenderReferenceViewportSize.width() * 0.5,
                                       blenderReferenceViewportSize.height() * 0.5);
        sendWheel(viewport.get(), comparisonCenter, -253);
        application.processEvents();
        const QImage topOrthographicCapture = captureViewport(viewport.get());
        saveGridCapture(QStringLiteral("viewport-native-top-ortho-scale-matched"),
                        topOrthographicCapture);
        passed &= check(topOrthographicCapture.size() == blenderReferenceViewportSize,
                        "native top-view capture must use Blender's reference dimensions");
        sendWheel(viewport.get(), comparisonCenter, 253);
        application.processEvents();
        viewport->setViewPreset(ViewportViewPreset::Perspective);
        waitForViewportTransition();
        saveGridCapture(QStringLiteral("viewport-native-iso-perspective-scale-matched"),
                        captureViewport(viewport.get()));
        viewport->setViewPreset(ViewportViewPreset::Top);
        waitForViewportTransition();
        viewport->resize(interactionViewportSize);
        application.processEvents();
        waitForViewportTransition();
    } else {
        saveGridCapture(QStringLiteral("viewport-native-start"),
                        captureViewport(viewport.get()));
    }
    const QImage beforeWheel = captureViewport(viewport.get());
    saveGridCapture(QStringLiteral("viewport-native-final-top-background"),
                    beforeWheel);
    const int edgeBackground = medianNeutralGray(
        beforeWheel,
        QPoint(12, 12),
        8);
    const int centerBackground = medianNeutralGray(
        beforeWheel,
        QPoint(beforeWheel.width() / 2, beforeWheel.height() / 2),
        3);
    passed &= check(edgeBackground >= 45 &&
                        edgeBackground <= 53 &&
                        centerBackground >= edgeBackground + 8,
                    "viewport background must use Blender's soft radial theme gradient");
    const QPoint globalCenter = viewport->mapToGlobal(center.toPoint());
    QWheelEvent wheel(center,
                      globalCenter,
                      QPoint(),
                      QPoint(0, 120),
                      Qt::NoButton,
                      Qt::NoModifier,
                      Qt::ScrollUpdate,
                      false);
    QApplication::sendEvent(viewport.get(), &wheel);
    application.processEvents();
    const QImage afterWheel = captureViewport(viewport.get());
    passed &= check(!beforeWheel.isNull() && afterWheel != beforeWheel &&
                        viewport->viewPreset() == ViewportViewPreset::Top,
                    "viewport wheel input must zoom the grid without changing its view preset");

    const QImage beforePixelWheel = captureViewport(viewport.get());
    QWheelEvent pixelWheelUp(center,
                             globalCenter,
                             QPoint(0, 40),
                             QPoint(),
                             Qt::NoButton,
                             Qt::NoModifier,
                             Qt::ScrollUpdate,
                             false);
    QApplication::sendEvent(viewport.get(), &pixelWheelUp);
    application.processEvents();
    const QImage afterPixelWheelUp = captureViewport(viewport.get());
    QWheelEvent pixelWheelDown(center,
                               globalCenter,
                               QPoint(0, -40),
                               QPoint(),
                               Qt::NoButton,
                               Qt::NoModifier,
                               Qt::ScrollUpdate,
                               false);
    QApplication::sendEvent(viewport.get(), &pixelWheelDown);
    application.processEvents();
    const QImage afterPixelWheelRoundTrip = captureViewport(viewport.get());
    passed &= check(afterPixelWheelUp != beforePixelWheel &&
                        afterPixelWheelRoundTrip == beforePixelWheel,
                    "pixel-only smooth-scroll events must zoom in and out symmetrically instead of always zooming out");

    viewport->setPanButton(Qt::RightButton);
    const QImage beforePan = captureViewport(viewport.get());
    sendMouse(viewport.get(),
              QEvent::MouseButtonPress,
              center,
              Qt::RightButton,
              Qt::RightButton,
              Qt::NoModifier);
    sendMouse(viewport.get(),
              QEvent::MouseMove,
              center + QPointF(42.0, 24.0),
              Qt::NoButton,
              Qt::RightButton,
              Qt::NoModifier);
    sendMouse(viewport.get(),
              QEvent::MouseButtonRelease,
              center + QPointF(42.0, 24.0),
              Qt::RightButton,
              Qt::NoButton,
              Qt::NoModifier);
    application.processEvents();
    const QImage afterPan = captureViewport(viewport.get());
    passed &= check(afterPan != beforePan &&
                        viewport->viewPreset() == ViewportViewPreset::Top,
                    "configured pan-button drag must pan the grid without orbiting the camera");

    const QImage beforeMiddlePan = captureViewport(viewport.get());
    sendMouse(viewport.get(),
              QEvent::MouseButtonPress,
              center,
              Qt::MiddleButton,
              Qt::MiddleButton,
              Qt::ShiftModifier);
    sendMouse(viewport.get(),
              QEvent::MouseMove,
              center + QPointF(28.0, 16.0),
              Qt::NoButton,
              Qt::MiddleButton,
              Qt::ShiftModifier);
    sendMouse(viewport.get(),
              QEvent::MouseButtonRelease,
              center + QPointF(28.0, 16.0),
              Qt::MiddleButton,
              Qt::NoButton,
              Qt::ShiftModifier);
    application.processEvents();
    const QImage afterMiddlePan = captureViewport(viewport.get());
    passed &= check(afterMiddlePan != beforeMiddlePan &&
                        viewport->viewPreset() == ViewportViewPreset::Top,
                    "Shift plus middle-button drag must pan instead of orbiting the camera");

    sendMouse(viewport.get(),
              QEvent::MouseButtonPress,
              center,
              Qt::RightButton,
              Qt::RightButton,
              Qt::ShiftModifier);
    sendMouse(viewport.get(),
              QEvent::MouseMove,
              center + QPointF(36.0, 18.0),
              Qt::NoButton,
              Qt::RightButton,
              Qt::ShiftModifier);
    sendMouse(viewport.get(),
              QEvent::MouseButtonRelease,
              center + QPointF(36.0, 18.0),
              Qt::RightButton,
              Qt::NoButton,
              Qt::ShiftModifier);
    application.processEvents();
    passed &= check(viewport->viewPreset() == ViewportViewPreset::Custom,
                    "Shift plus the configured pan button must orbit the actual viewport camera");

    const QImage beforeRectangle = captureViewport(viewport.get());
    viewport->setTool(ToolId::Rectangle);
    for (const QPointF point : {QPointF(170.0, 290.0),
                                QPointF(330.0, 360.0)}) {
        sendMouse(viewport.get(), QEvent::MouseButtonPress, point,
                  Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        sendMouse(viewport.get(), QEvent::MouseButtonRelease, point,
                  Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    }
    viewport->setTool(ToolId::Select);
    application.processEvents();
    const QImage afterRectangle = captureViewport(viewport.get());
    const auto orangePixels = [](const QImage &image) {
        int count = 0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const QColor color = image.pixelColor(x, y);
                count += color.red() > 140 && color.green() > 65 &&
                         color.green() < color.red() &&
                         color.blue() < color.green();
            }
        }
        return count;
    };
    passed &= check(afterRectangle != beforeRectangle &&
                        orangePixels(afterRectangle) > orangePixels(beforeRectangle) + 25,
                    "committed rectangle stroke must appear after viewport rendering");

    const QImage beforeBezier = captureViewport(viewport.get());
    viewport->setTool(ToolId::Bezier);
    for (const QPointF point : {QPointF(350.0, 280.0),
                                QPointF(380.0, 340.0),
                                QPointF(430.0, 260.0),
                                QPointF(480.0, 320.0)}) {
        sendMouse(viewport.get(), QEvent::MouseButtonPress, point,
                  Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        sendMouse(viewport.get(), QEvent::MouseButtonRelease, point,
                  Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    }
    viewport->setTool(ToolId::Select);
    application.processEvents();
    const QImage afterBezier = captureViewport(viewport.get());
    const auto guidePixels = [](const QImage &image) {
        int count = 0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const QColor color = image.pixelColor(x, y);
                count += std::abs(color.red() - 138) < 20 &&
                         std::abs(color.green() - 167) < 20 &&
                         std::abs(color.blue() - 199) < 20;
            }
        }
        return count;
    };
    passed &= check(afterBezier != beforeBezier &&
                        guidePixels(afterBezier) > guidePixels(beforeBezier) + 5,
                    "Bezier control guides must be visible in the viewport frame");

    const QImage beforePoint = captureViewport(viewport.get());
    viewport->setTool(ToolId::Point);
    const QPointF pointPosition(520.0, 350.0);
    sendMouse(viewport.get(), QEvent::MouseButtonPress, pointPosition,
              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    sendMouse(viewport.get(), QEvent::MouseButtonRelease, pointPosition,
              Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    viewport->setTool(ToolId::Select);
    application.processEvents();
    const QImage afterPoint = captureViewport(viewport.get());
    passed &= check(orangePixels(afterPoint) > orangePixels(beforePoint) + 10,
                    "committed point must appear in the viewport frame");

    if (QApplication::platformName() == QStringLiteral("xcb")) {
        auto *nativeSurface = viewport->findChild<ViewportGpuSurface *>();
        passed &= check(nativeSurface != nullptr && nativeSurface->isValid(),
                        "desktop viewport must use a valid native OpenGL surface");
        BlenderGridRenderer gridRenderer;
        ViewportTransform orthographicTransform;
        orthographicTransform.setCameraPreferences(blenderCameraPreferences);
        const QImage orthographicGrid = gridRenderer.render(
            orthographicTransform,
            blenderReferenceViewportSize,
            1.0,
            {},
            1.0,
            blenderGridAppearance);
        ViewportTransform perspectiveTransform;
        perspectiveTransform.setViewPreset(ViewportViewPreset::Isometric);
        perspectiveTransform.setPerspectiveEnabled(true);
        perspectiveTransform.setCameraPreferences(blenderCameraPreferences);
        const QImage perspectiveGrid = gridRenderer.render(
            perspectiveTransform,
            blenderReferenceViewportSize,
            1.0,
            {},
            1.0,
            blenderGridAppearance);
        saveGridCapture(QStringLiteral("top-ortho"), orthographicGrid);
        saveGridCapture(QStringLiteral("iso-perspective"), perspectiveGrid);

        const auto captureView = [&gridRenderer,
                                  &blenderCameraPreferences,
                                  &blenderReferenceViewportSize,
                                  &blenderGridAppearance](
                                     const QString &name,
                                     ViewportTransform transform) {
            transform.setCameraPreferences(blenderCameraPreferences);
            saveGridCapture(
                name,
                gridRenderer.render(transform,
                                    blenderReferenceViewportSize,
                                    1.0,
                                    {},
                                    1.0,
                                    blenderGridAppearance));
        };
        for (const auto &[name, preset] : {
                 std::pair{QStringLiteral("front-ortho"), ViewportViewPreset::Front},
                 std::pair{QStringLiteral("right-ortho"), ViewportViewPreset::Right},
                 std::pair{QStringLiteral("bottom-ortho"), ViewportViewPreset::Bottom},
                 std::pair{QStringLiteral("back-ortho"), ViewportViewPreset::Back},
                 std::pair{QStringLiteral("left-ortho"), ViewportViewPreset::Left},
                 std::pair{QStringLiteral("iso-ortho"), ViewportViewPreset::Isometric}}) {
            ViewportTransform transform;
            transform.setViewPreset(preset);
            captureView(name, transform);
        }
        ViewportTransform zoomInTransform;
        ViewportCameraState zoomInState = zoomInTransform.cameraState();
        zoomInState.zoom = 2.0;
        zoomInState.gridViewDistance = 30.0;
        zoomInTransform.setCameraState(zoomInState);
        captureView(QStringLiteral("top-zoom-in"), zoomInTransform);
        ViewportTransform zoomOutTransform;
        ViewportCameraState zoomOutState = zoomOutTransform.cameraState();
        zoomOutState.zoom = 0.5;
        zoomOutState.gridViewDistance = 120.0;
        zoomOutTransform.setCameraState(zoomOutState);
        captureView(QStringLiteral("top-zoom-out"), zoomOutTransform);
        ViewportTransform panTransform;
        panTransform.pan() = QPointF(30.0, -18.0);
        captureView(QStringLiteral("top-pan"), panTransform);

        const auto coveredPixels = [](const QImage &image) {
            int count = 0;
            for (int y = 0; y < image.height(); ++y) {
                for (int x = 0; x < image.width(); ++x) {
                    count += image.pixelColor(x, y).alpha() > 0;
                }
            }
            return count;
        };
        passed &= check(!orthographicGrid.isNull() && !perspectiveGrid.isNull() &&
                            coveredPixels(orthographicGrid) > 100 &&
                            coveredPixels(perspectiveGrid) > 100 &&
                            orthographicGrid != perspectiveGrid,
                        "required OpenGL run must render distinct covered grid pixels in orthographic and perspective views");

        ViewportGpuSurface depthProbe;
        depthProbe.resize(200, 150);
        ViewportTransform depthTransform;
        depthTransform.zoom() = 100.0;
        Shape backPoint;
        backPoint.geometryType = GeometryType::Point;
        backPoint.points = {QPointF(0.0, 0.0)};
        Shape frontPoint = backPoint;
        frontPoint.workPlaneOffset = 10.0;
        const QVector<Shape> depthShapes{backPoint, frontPoint};
        depthProbe.setDrawCallback(
            [&depthTransform](QPainter &painter,
                              BlenderGridRenderer &gridRenderer,
                              ViewportSceneRenderer &) {
                painter.fillRect(QRect(QPoint(0, 0), painter.viewport().size()),
                                 QColor(QStringLiteral("#282828")));
                painter.beginNativePainting();
                gridRenderer.renderToCurrentFramebuffer(
                    depthTransform, QSize(200, 150), 1.0, {}, 1.0,
                    BlenderGridAppearance{});
                painter.endNativePainting();
            });
        depthProbe.show();
        application.processEvents();
        Point3D pickedPoint;
        const bool depthPickSucceeded = depthProbe.pickScenePoint(
            QPointF(100.0, 75.0), depthTransform, QSize(200, 150),
            depthShapes, &pickedPoint);
        passed &= check(depthProbe.isValid() && depthPickSucceeded &&
                            std::abs(pickedPoint.x) < 0.1 &&
                            std::abs(pickedPoint.y) < 0.1 &&
                            std::abs(pickedPoint.z - 10.0) < 0.1,
                        "GPU depth picking must unproject the frontmost overlapping scene point");
        depthProbe.hide();
    }

    return passed ? 0 : 1;
}
