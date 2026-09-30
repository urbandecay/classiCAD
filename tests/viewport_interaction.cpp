#include "ui/viewport_widget_api.h"
#include "ui/viewport/blender_grid_renderer.h"
#include "ui/viewport/viewport_gpu_surface.h"
#include "ui/viewport/viewport_depth_geometry.h"

#include <QApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QDir>
#include <QEventLoop>
#include <QMouseEvent>
#include <QPainter>
#include <QTemporaryDir>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
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

int pixelsNearColor(const QImage &image, const QColor &target, int tolerance = 4)
{
    int count = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            count += std::abs(pixel.red() - target.red()) <= tolerance &&
                     std::abs(pixel.green() - target.green()) <= tolerance &&
                     std::abs(pixel.blue() - target.blue()) <= tolerance;
        }
    }
    return count;
}

qreal imageMeanAbsoluteError(const QImage &first, const QImage &second)
{
    if (first.isNull() || second.isNull() || first.size() != second.size()) {
        return std::numeric_limits<qreal>::infinity();
    }
    qint64 totalDifference = 0;
    for (int y = 0; y < first.height(); ++y) {
        for (int x = 0; x < first.width(); ++x) {
            const QColor firstPixel = first.pixelColor(x, y);
            const QColor secondPixel = second.pixelColor(x, y);
            totalDifference += std::abs(firstPixel.red() - secondPixel.red());
            totalDifference += std::abs(firstPixel.green() - secondPixel.green());
            totalDifference += std::abs(firstPixel.blue() - secondPixel.blue());
        }
    }
    const qreal channelCount =
        static_cast<qreal>(first.width()) * first.height() * 3.0;
    return static_cast<qreal>(totalDifference) / channelCount;
}

int neutralGridLikePixels(const QImage &image)
{
    int count = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            const int minimum = std::min({pixel.red(), pixel.green(), pixel.blue()});
            const int maximum = std::max({pixel.red(), pixel.green(), pixel.blue()});
            const int gray = (pixel.red() + pixel.green() + pixel.blue()) / 3;
            count += maximum - minimum <= 8 && gray >= 64 && gray <= 110;
        }
    }
    return count;
}

void waitForViewportTransition()
{
    QEventLoop eventLoop;
    QTimer::singleShot(250, &eventLoop, &QEventLoop::quit);
    eventLoop.exec();
}

bool benchmarkNativeViewportFrames(ViewportWidgetApi *viewport, int frameCount)
{
    auto *surface = viewport == nullptr
                        ? nullptr
                        : viewport->findChild<ViewportGpuSurface *>();
    if (surface == nullptr || !surface->isValid() || frameCount < 2) {
        return false;
    }

    QVector<qint64> frameLatenciesNanoseconds;
    frameLatenciesNanoseconds.reserve(frameCount);
    QElapsedTimer latencyTimer;
    QEventLoop frameLoop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &frameLoop,
                     &QEventLoop::quit);

    bool waitingForFrame = false;
    bool frameReceived = false;
    QObject::connect(surface, &QOpenGLWidget::frameSwapped, &frameLoop,
                     [&]() {
                         if (!waitingForFrame) {
                             return;
                         }
                         waitingForFrame = false;
                         frameReceived = true;
                         frameLatenciesNanoseconds.append(
                             latencyTimer.nsecsElapsed());
                         frameLoop.quit();
                     });

    const QPointF center(viewport->width() * 0.5,
                         viewport->height() * 0.5);
    for (int frame = 0; frame < frameCount; ++frame) {
        frameReceived = false;
        waitingForFrame = true;
        latencyTimer.start();
        sendWheel(viewport, center, frame % 2 == 0 ? 120 : -120);
        surface->update();
        timeout.start(1000);
        frameLoop.exec();
        timeout.stop();
        waitingForFrame = false;
        if (!frameReceived) {
            qWarning() << "Viewport benchmark timed out waiting for frame"
                       << frame;
            return false;
        }
    }

    std::sort(frameLatenciesNanoseconds.begin(),
              frameLatenciesNanoseconds.end());
    qint64 totalNanoseconds = 0;
    for (const qint64 latency : frameLatenciesNanoseconds) {
        totalNanoseconds += latency;
    }
    const qreal meanMilliseconds =
        static_cast<qreal>(totalNanoseconds) / frameCount / 1.0e6;
    const qreal medianMilliseconds =
        static_cast<qreal>(frameLatenciesNanoseconds[frameCount / 2]) / 1.0e6;
    const int percentile95Index =
        std::clamp(static_cast<int>(std::ceil(frameCount * 0.95)) - 1,
                   0,
                   frameCount - 1);
    const qreal percentile95Milliseconds =
        static_cast<qreal>(frameLatenciesNanoseconds[percentile95Index]) / 1.0e6;
    qInfo().noquote()
        << QStringLiteral("Native viewport frame benchmark: frames=%1 mean=%2 ms median=%3 ms p95=%4 ms rate=%5 frames/s (wheel-input to frame-swap, includes display pacing)")
               .arg(frameCount)
               .arg(meanMilliseconds, 0, 'f', 2)
               .arg(medianMilliseconds, 0, 'f', 2)
               .arg(percentile95Milliseconds, 0, 'f', 2)
               .arg(meanMilliseconds > 0.0 ? 1000.0 / meanMilliseconds : 0.0,
                    0, 'f', 1);
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    bool passed = true;

    const QSize interactionViewportSize(640, 480);
    const QSize blenderReferenceViewportSize(591, 511);
    const ViewportCameraPreferences blenderCameraPreferences{
        50.0, 0.01, 10000.0};
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
            std::abs(appliedCameraPreferences.clipEnd - 10000.0) < 1.0e-9 &&
            viewport->viewportAntiAliasingSamples() == 8,
        "Blender comparison must use a 50 mm lens, 0.01/10000 clipping, and 8x AA");
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
    if (QApplication::platformName() == QStringLiteral("xcb")) {
        viewport->resize(blenderReferenceViewportSize);
        application.processEvents();
        waitForViewportTransition();
        viewport->setViewPreset(ViewportViewPreset::Top);
        waitForViewportTransition();

        const QPointF comparisonCenter(
            blenderReferenceViewportSize.width() * 0.5,
            blenderReferenceViewportSize.height() * 0.5);
        const QImage zoomRoundTripStart = captureViewport(viewport.get());
        for (int step = 0; step < 20; ++step) {
            sendWheel(viewport.get(), comparisonCenter, -120);
        }
        application.processEvents();
        waitForViewportTransition();
        const QImage twentyStepsOut = captureViewport(viewport.get());
        for (int step = 0; step < 20; ++step) {
            sendWheel(viewport.get(), comparisonCenter, 120);
        }
        application.processEvents();
        waitForViewportTransition();
        const QImage zoomRoundTripEnd = captureViewport(viewport.get());
        saveGridCapture(QStringLiteral("viewport-native-zoom-20-out"),
                        twentyStepsOut);
        saveGridCapture(QStringLiteral("viewport-native-zoom-20-return"),
                        zoomRoundTripEnd);
        const int gridPixelsAfterZoomOut =
            neutralGridLikePixels(twentyStepsOut);
        const qreal zoomRoundTripImageError = imageMeanAbsoluteError(
            zoomRoundTripStart, zoomRoundTripEnd);
        qInfo().noquote()
            << QStringLiteral("Native 20-step zoom check: gridPixels=%1 returnImageMAE=%2")
                   .arg(gridPixelsAfterZoomOut)
                   .arg(zoomRoundTripImageError, 0, 'f', 4);
        passed &= check(zoomRoundTripStart.size() == blenderReferenceViewportSize &&
                            gridPixelsAfterZoomOut > 200 &&
                            zoomRoundTripImageError < 2.0,
                        "native grid must remain visible after twenty zoom-out steps and return to the same centered view after twenty steps in");

        viewport->setViewPreset(ViewportViewPreset::Perspective);
        waitForViewportTransition();
        const QImage perspectiveZoomRoundTripStart =
            captureViewport(viewport.get());
        for (int step = 0; step < 20; ++step) {
            sendWheel(viewport.get(), comparisonCenter, -120);
        }
        application.processEvents();
        waitForViewportTransition();
        const QImage perspectiveTwentyStepsOut =
            captureViewport(viewport.get());
        for (int step = 0; step < 20; ++step) {
            sendWheel(viewport.get(), comparisonCenter, 120);
        }
        application.processEvents();
        waitForViewportTransition();
        const QImage perspectiveZoomRoundTripEnd =
            captureViewport(viewport.get());
        saveGridCapture(QStringLiteral("viewport-native-perspective-zoom-20-out"),
                        perspectiveTwentyStepsOut);
        saveGridCapture(QStringLiteral("viewport-native-perspective-zoom-20-return"),
                        perspectiveZoomRoundTripEnd);
        const int perspectiveGridPixelsAfterZoomOut =
            neutralGridLikePixels(perspectiveTwentyStepsOut);
        const qreal perspectiveZoomRoundTripImageError =
            imageMeanAbsoluteError(perspectiveZoomRoundTripStart,
                                  perspectiveZoomRoundTripEnd);
        qInfo().noquote()
            << QStringLiteral("Native 20-step perspective zoom check: gridPixels=%1 returnImageMAE=%2")
                   .arg(perspectiveGridPixelsAfterZoomOut)
                   .arg(perspectiveZoomRoundTripImageError, 0, 'f', 4);
        passed &= check(
            perspectiveZoomRoundTripStart.size() == blenderReferenceViewportSize &&
                perspectiveGridPixelsAfterZoomOut > 200 &&
                perspectiveZoomRoundTripImageError < 2.0,
            "native perspective grid must remain visible after twenty zoom-out steps and return to the same centered view after twenty steps in");

        const std::array<std::pair<const char *, ViewportViewPreset>, 8> viewMatrix{{
            {"top", ViewportViewPreset::Top},
            {"bottom", ViewportViewPreset::Bottom},
            {"front", ViewportViewPreset::Front},
            {"back", ViewportViewPreset::Back},
            {"right", ViewportViewPreset::Right},
            {"left", ViewportViewPreset::Left},
            {"isometric-ortho", ViewportViewPreset::Isometric},
            {"isometric-perspective", ViewportViewPreset::Perspective},
        }};
        bool allNativeViewsCaptured = true;
        for (const auto &[name, preset] : viewMatrix) {
            viewport->setViewPreset(preset);
            waitForViewportTransition();
            const QImage viewCapture = captureViewport(viewport.get());
            allNativeViewsCaptured &=
                !viewCapture.isNull() &&
                viewCapture.size() == blenderReferenceViewportSize;
            saveGridCapture(QStringLiteral("viewport-native-") +
                                QString::fromLatin1(name),
                            viewCapture);
        }
        passed &= check(allNativeViewsCaptured,
                        "native viewport capture matrix must cover all six axis views, isometric orthographic, and perspective at Blender's reference dimensions");

        viewport->setViewPreset(ViewportViewPreset::Top);
        waitForViewportTransition();
        viewport->resize(interactionViewportSize);
        application.processEvents();
        waitForViewportTransition();
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
    const QPointF rectangleStart(170.0, 290.0);
    const QPointF rectangleEnd(330.0, 360.0);
    sendMouse(viewport.get(), QEvent::MouseButtonPress, rectangleStart,
              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    sendMouse(viewport.get(), QEvent::MouseButtonRelease, rectangleStart,
              Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    sendMouse(viewport.get(), QEvent::MouseMove, rectangleEnd,
              Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    application.processEvents();
    const QImage rectanglePreview = captureViewport(viewport.get());
    const auto previewGoldPixels = [](const QImage &image) {
        int count = 0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const QColor color = image.pixelColor(x, y);
                count += color.red() > 175 && color.green() > 115 &&
                         color.green() < 215 && color.blue() < 140;
            }
        }
        return count;
    };
    if (QApplication::platformName() == QStringLiteral("xcb")) {
        passed &= check(previewGoldPixels(rectanglePreview) > 20,
                        "in-progress rectangle preview must be rendered into the native OpenGL viewport");
    }
    sendMouse(viewport.get(), QEvent::MouseButtonPress, rectangleEnd,
              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    sendMouse(viewport.get(), QEvent::MouseButtonRelease, rectangleEnd,
              Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
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

    QTemporaryDir pictureDirectory;
    QImage pictureImage(64, 64, QImage::Format_ARGB32);
    pictureImage.fill(Qt::transparent);
    {
        QPainter picturePainter(&pictureImage);
        picturePainter.fillRect(QRect(0, 0, 32, 32), Qt::red);
        picturePainter.fillRect(QRect(0, 32, 32, 32), Qt::blue);
        picturePainter.fillRect(QRect(32, 32, 32, 32), Qt::yellow);
    }
    Shape depthPicture;
    depthPicture.geometryType = GeometryType::Picture;
    depthPicture.points = {QPointF(0.0, 1.0), QPointF(1.0, 1.0),
                           QPointF(1.0, 0.0), QPointF(0.0, 0.0)};
    depthPicture.pictureImage = pictureImage;
    const ViewportDepthGeometry pictureDepth =
        buildViewportDepthGeometry(depthPicture);
    bool pictureDepthSkipsTransparentTopRight = true;
    for (const QVector3D &vertex : pictureDepth.surfaceVertices) {
        pictureDepthSkipsTransparentTopRight &=
            !(vertex.x() > 0.5f && vertex.y() > 0.5f);
    }
    passed &= check(!pictureDepth.surfaceVertices.isEmpty() &&
                        pictureDepthSkipsTransparentTopRight,
                    "picture depth mesh must omit transparent pixels instead of treating the full frame as opaque");
    const QString picturePath = pictureDirectory.filePath(
        QStringLiteral("viewport-preview-quadrants.png"));
    passed &= check(pictureDirectory.isValid() && pictureImage.save(picturePath),
                    "test picture preview image must be created");
    viewport->setViewPreset(ViewportViewPreset::Top);
    viewport->setWorkPlane(WorkPlane::XY);
    viewport->setOsnapEnabled(false);
    waitForViewportTransition();
    const QImage beforePicture = captureViewport(viewport.get());
    QString pictureError;
    passed &= check(viewport->beginPicturePlacement(picturePath, &pictureError),
                    "picture placement must start for the OpenGL preview test");
    const QPointF pictureFirstCorner(150.0, 140.0);
    const QPointF pictureCursorCorner(390.0, 380.0);
    sendMouse(viewport.get(), QEvent::MouseButtonPress, pictureFirstCorner,
              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    sendMouse(viewport.get(), QEvent::MouseButtonRelease, pictureFirstCorner,
              Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    sendMouse(viewport.get(), QEvent::MouseMove, pictureCursorCorner,
              Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    application.processEvents();
    const QImage picturePreview = captureViewport(viewport.get());
    struct ColorRegionStats {
        int count = 0;
        qint64 xSum = 0;
        qint64 ySum = 0;
        QPointF center() const
        {
            return count == 0 ? QPointF()
                              : QPointF(static_cast<qreal>(xSum) / count,
                                        static_cast<qreal>(ySum) / count);
        }
    };
    ColorRegionStats redRegion;
    ColorRegionStats greenRegion;
    ColorRegionStats blueRegion;
    ColorRegionStats yellowRegion;
    for (int y = 0; y < picturePreview.height(); ++y) {
        for (int x = 0; x < picturePreview.width(); ++x) {
            const QColor color = picturePreview.pixelColor(x, y);
            ColorRegionStats *region = nullptr;
            if (color.red() > color.green() + 65 &&
                color.red() > color.blue() + 45) {
                region = &redRegion;
            } else if (color.green() > color.red() + 45 &&
                       color.green() > color.blue() + 25) {
                region = &greenRegion;
            } else if (color.blue() > color.red() + 55 &&
                       color.blue() > color.green() + 30) {
                region = &blueRegion;
            } else if (color.red() > color.blue() + 55 &&
                       color.green() > color.blue() + 55 &&
                       color.red() > 100 && color.green() > 100) {
                region = &yellowRegion;
            }
            if (region != nullptr) {
                ++region->count;
                region->xSum += x;
                region->ySum += y;
            }
        }
    }
    const QPointF redCenter = redRegion.center();
    const QPointF blueCenter = blueRegion.center();
    const QPointF yellowCenter = yellowRegion.center();
    passed &= check(redRegion.count > 100 && blueRegion.count > 100 &&
                        yellowRegion.count > 100 &&
                        blueCenter.x() < yellowCenter.x() &&
                        redCenter.y() < blueCenter.y() &&
                        redCenter.x() < yellowCenter.x(),
                    "picture placement preview must render its image with the correct texture orientation");
    sendMouse(viewport.get(), QEvent::MouseButtonPress, pictureCursorCorner,
              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    sendMouse(viewport.get(), QEvent::MouseButtonRelease, pictureCursorCorner,
              Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    application.processEvents();
    const QImage committedPicture = captureViewport(viewport.get());
    const QPoint redSample = redCenter.toPoint();
    const QPoint blueSample = blueCenter.toPoint();
    const QPoint yellowSample = yellowCenter.toPoint();
    const QPoint transparentSample =
        (redCenter + yellowCenter - blueCenter).toPoint();
    const QColor transparentBefore = beforePicture.pixelColor(transparentSample);
    const QColor transparentAfter = committedPicture.pixelColor(transparentSample);
    const QColor committedRed = committedPicture.pixelColor(redSample);
    const QColor committedBlue = committedPicture.pixelColor(blueSample);
    const QColor committedYellow = committedPicture.pixelColor(yellowSample);
    const bool transparentAreaPreserved =
        std::abs(transparentBefore.red() - transparentAfter.red()) <= 12 &&
        std::abs(transparentBefore.green() - transparentAfter.green()) <= 12 &&
        std::abs(transparentBefore.blue() - transparentAfter.blue()) <= 12;
    passed &= check(committedRed.red() > committedRed.green() + 50 &&
                        committedBlue.blue() > committedBlue.red() + 50 &&
                        committedYellow.red() > 150 && committedYellow.green() > 150 &&
                        transparentAreaPreserved,
                    "committed picture rendering must preserve image colors and leave transparent pixels unobscuring the grid");

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

    viewport->setControlPointsVisible(true);
    sendMouse(viewport.get(), QEvent::MouseButtonPress, QPointF(350.0, 280.0),
              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    sendMouse(viewport.get(), QEvent::MouseButtonRelease, QPointF(350.0, 280.0),
              Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    application.processEvents();
    waitForViewportTransition();
    const QImage controlPointFrame = captureViewport(viewport.get());
    passed &= check(pixelsNearColor(controlPointFrame,
                                    QColor(QStringLiteral("#263b4b"))) >= 24,
                    "selected curve control-point handles must render in the native viewport and CPU fallback");
    viewport->setControlPointsVisible(false);

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

        const auto redAxisPixels = [](const QImage &image) {
            int count = 0;
            for (int y = 0; y < image.height(); ++y) {
                for (int x = 0; x < image.width(); ++x) {
                    const QColor pixel = image.pixelColor(x, y);
                    count += pixel.red() > pixel.green() + 25 &&
                             pixel.red() > pixel.blue() + 20 &&
                             pixel.red() > 40;
                }
            }
            return count;
        };
        const auto greenAxisPixels = [](const QImage &image) {
            int count = 0;
            for (int y = 0; y < image.height(); ++y) {
                for (int x = 0; x < image.width(); ++x) {
                    const QColor pixel = image.pixelColor(x, y);
                    count += pixel.green() > pixel.red() + 25 &&
                             pixel.green() > pixel.blue() + 20 &&
                             pixel.green() > 40;
                }
            }
            return count;
        };
        const auto renderCameraRelativeAxis = [&](qreal yawRadians,
                                                  const Point3D &orbitPivot) {
            ViewportTransform transform;
            transform.setCameraPreferences(blenderCameraPreferences);
            ViewportCameraState camera = transform.cameraState();
            camera.yawRadians = yawRadians;
            camera.pitchRadians = 0.12;
            camera.orbitPivot = orbitPivot;
            camera.perspective = true;
            camera.preset = ViewportViewPreset::Custom;
            camera.hasOrientation = false;
            transform.setCameraState(camera);
            return gridRenderer.render(transform,
                                       blenderReferenceViewportSize,
                                       1.0,
                                       {},
                                       1.0,
                                       blenderGridAppearance);
        };
        constexpr qreal halfPi = 1.57079632679489661923;
        const QImage nearEdgeOnRedAxisBefore =
            renderCameraRelativeAxis(halfPi - 0.003,
                                    {8000.0, 0.0, 0.0});
        const QImage nearEdgeOnRedAxisAfter =
            renderCameraRelativeAxis(halfPi + 0.003,
                                    {8000.0, 0.0, 0.0});
        saveGridCapture(QStringLiteral("axis-camera-relative-before"),
                        nearEdgeOnRedAxisBefore);
        saveGridCapture(QStringLiteral("axis-camera-relative-after"),
                        nearEdgeOnRedAxisAfter);
        passed &= check(redAxisPixels(nearEdgeOnRedAxisBefore) > 2 &&
                            redAxisPixels(nearEdgeOnRedAxisAfter) > 2,
                        "native GPU X axis must stay visible through small perspective camera movements far from world origin");

        const QImage redAxisEdgeOn =
            renderCameraRelativeAxis(halfPi, {});
        const QImage greenAxisEdgeOn =
            renderCameraRelativeAxis(0.0, {});
        saveGridCapture(QStringLiteral("axis-red-edge-on"), redAxisEdgeOn);
        saveGridCapture(QStringLiteral("axis-green-edge-on"), greenAxisEdgeOn);
        qInfo() << "Native edge-on axis pixels:"
                << redAxisPixels(redAxisEdgeOn)
                << greenAxisPixels(redAxisEdgeOn)
                << redAxisPixels(greenAxisEdgeOn)
                << greenAxisPixels(greenAxisEdgeOn);
        passed &= check(redAxisPixels(redAxisEdgeOn) > 2 &&
                            greenAxisPixels(redAxisEdgeOn) > 2 &&
                            redAxisPixels(greenAxisEdgeOn) > 2 &&
                            greenAxisPixels(greenAxisEdgeOn) > 2,
                        "native GPU red and green axes must remain visible when either axis points nearly into the perspective camera");

        for (int yawStep = 0; yawStep < 8; ++yawStep) {
            ViewportTransform transform;
            transform.setCameraPreferences(blenderCameraPreferences);
            ViewportCameraState camera = transform.cameraState();
            camera.yawRadians = yawStep * halfPi / 2.0;
            camera.pitchRadians = 0.55;
            camera.orbitPivot = {4.0, 4.0, 0.0};
            camera.zoom = 0.05;
            camera.perspective = true;
            camera.preset = ViewportViewPreset::Custom;
            camera.hasOrientation = false;
            transform.setCameraState(camera);
            const QImage image = gridRenderer.render(transform,
                                                     blenderReferenceViewportSize,
                                                     1.0,
                                                     {},
                                                     1.0,
                                                     blenderGridAppearance);
            passed &= check(redAxisPixels(image) > 50 &&
                                greenAxisPixels(image) > 50,
                            "native GPU axes must survive orbit at 5% perspective zoom");
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
                              ViewportSceneRenderer &,
                              ViewportSceneRenderer &,
                              ViewportControlPointRenderer &) {
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
        passed &= check(benchmarkNativeViewportFrames(viewport.get(), 90),
                        "native viewport frame benchmark must receive each requested frame swap");
    }

    return passed ? 0 : 1;
}
