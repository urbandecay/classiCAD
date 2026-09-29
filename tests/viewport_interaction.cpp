#include "ui/viewport_widget_api.h"
#include "ui/viewport/blender_grid_renderer.h"
#include "ui/viewport/viewport_gpu_surface.h"

#include <QApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

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

QImage captureViewport(ViewportWidgetApi *viewport)
{
    if (auto *surface = viewport->findChild<ViewportGpuSurface *>()) {
        return surface->grabFramebuffer();
    }
    return viewport->grab().toImage();
}

} // namespace

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    bool passed = true;

    std::unique_ptr<ViewportWidgetApi> viewport(createViewportWidget());
    viewport->resize(640, 480);
    viewport->show();
    application.processEvents();

    passed &= check(viewport->viewportAntiAliasingSamples() == 8,
                    "viewport anti-aliasing must default to the saved Blender 8x setting");
    viewport->setViewportAntiAliasingSamples(4);
    passed &= check(viewport->viewportAntiAliasingSamples() == 4,
                    "viewport anti-aliasing preference must update the OpenGL renderer");
    viewport->setViewportAntiAliasingSamples(8);

    const QPointF center(320.0, 240.0);
    const QImage beforeWheel = captureViewport(viewport.get());
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
        const QImage orthographicGrid = gridRenderer.render(
            orthographicTransform,
            QSize(640, 480),
            1.0,
            {},
            1.0,
            BlenderGridAppearance{});
        ViewportTransform perspectiveTransform;
        perspectiveTransform.setViewPreset(ViewportViewPreset::Isometric);
        perspectiveTransform.setPerspectiveEnabled(true);
        const QImage perspectiveGrid = gridRenderer.render(
            perspectiveTransform,
            QSize(640, 480),
            1.0,
            {},
            1.0,
            BlenderGridAppearance{});
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
