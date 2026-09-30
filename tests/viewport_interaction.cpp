#include "ui/viewport_widget_api.h"
#include "ui/viewport/blender_grid_renderer.h"
#include "ui/viewport/viewport_gpu_surface.h"
#include "ui/viewport/viewport_depth_geometry.h"
#include "ui/viewport/line_type_style.h"

#include "core/document/document.h"
#include "core/serialization/vignola_document_file.h"

#include <QApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QDir>
#include <QEventLoop>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
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

bool waitForViewPreset(ViewportWidgetApi *viewport, ViewportViewPreset preset)
{
    QElapsedTimer timer;
    timer.start();
    while (viewport->viewPreset() != preset && timer.elapsed() < 2000) {
        QEventLoop loop;
        QTimer::singleShot(25, &loop, &QEventLoop::quit);
        loop.exec();
    }
    return viewport->viewPreset() == preset;
}

bool benchmarkNativeViewportFrames(ViewportWidgetApi *viewport,
                                   int frameCount,
                                   const QString &sceneName)
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
        << QStringLiteral("Native viewport frame benchmark: scene=%1 frames=%2 mean=%3 ms median=%4 ms p95=%5 ms rate=%6 frames/s (wheel-input to frame-swap, includes display pacing)")
               .arg(sceneName)
               .arg(frameCount)
               .arg(meanMilliseconds, 0, 'f', 2)
               .arg(medianMilliseconds, 0, 'f', 2)
               .arg(percentile95Milliseconds, 0, 'f', 2)
               .arg(meanMilliseconds > 0.0 ? 1000.0 / meanMilliseconds : 0.0,
                    0, 'f', 1);
    return true;
}

QString openGlRendererName(ViewportGpuSurface *surface)
{
    if (surface == nullptr || !surface->isValid()) {
        return {};
    }
    surface->makeCurrent();
    QString rendererName;
    QOpenGLContext *context = surface->context();
    if (context != nullptr) {
        QOpenGLFunctions *functions = context->functions();
        if (functions != nullptr) {
            functions->initializeOpenGLFunctions();
            const GLubyte *renderer = functions->glGetString(GL_RENDERER);
            if (renderer != nullptr) {
                rendererName = QString::fromLatin1(
                    reinterpret_cast<const char *>(renderer));
            }
        }
    }
    surface->doneCurrent();
    return rendererName;
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
    // Keep the extreme-distance grid clip regressions independent of the
    // matched Blender capture settings above.
    const ViewportCameraPreferences wideClipGridPreferences{
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
            std::abs(appliedCameraPreferences.clipEnd - 1000.0) < 1.0e-9 &&
            viewport->viewportAntiAliasingSamples() == 8,
        "Blender comparison must use a 50 mm lens, 0.01/1000 clipping, and 8x AA");
    passed &= check(
        std::abs(viewport->documentSettings().gridSpacing - 1.0) < 1.0e-9,
        "Blender comparison must use the same 1-unit base grid spacing");
    viewport->setGridAppearance(blenderGridAppearance);

    const QPointF center(320.0, 240.0);
    // A side-view click over an edge-on XY curve must use the visible YZ
    // plane, rather than discarding input on an unpickable inherited plane.
    {
        QTemporaryDir directory;
        Document source;
        Shape edge;
        edge.geometryType = GeometryType::Line;
        edge.points = {QPointF(0.0, -100.0), QPointF(0.0, 100.0)};
        edge.nurbs = makeDegreeOneNurbs(edge.points);
        source.append(edge);
        const QString path = directory.filePath(QStringLiteral("side-line.vignola"));
        QString error;
        std::unique_ptr<ViewportWidgetApi> probe(createViewportWidget());
        probe->resize(interactionViewportSize);
        probe->show();
        passed &= check(saveVignolaDocument(path, source, &error) &&
                            probe->loadVignolaDocument(path, &error),
                        "side-view line fixture must load");
        probe->setViewPreset(ViewportViewPreset::Right);
        application.processEvents();
        passed &= check(waitForViewPreset(probe.get(), ViewportViewPreset::Right),
                        "side-view camera transition must finish before drawing");
        probe->setTool(ToolId::Line);
        sendMouse(probe.get(), QEvent::MouseButtonPress, center,
                  Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        sendMouse(probe.get(), QEvent::MouseMove, center + QPointF(0.0, -80.0),
                  Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        sendMouse(probe.get(), QEvent::MouseButtonPress, center + QPointF(0.0, -80.0),
                  Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        sendMouse(probe.get(), QEvent::MouseButtonPress, center + QPointF(0.0, -80.0),
                  Qt::RightButton, Qt::RightButton, Qt::NoModifier);
        Document result;
        passed &= check(probe->saveVignolaDocument(path, &error) &&
                            loadVignolaDocument(path, &result, &error) &&
                            result.objects().size() == 2,
                        "side-view line must commit above an edge-on existing curve");
        if (result.objects().size() == 2) {
            const Shape &line = result.objects().back().geometry;
            const WorkPlaneFrame frame = shapeWorkPlaneFrame(line);
            const Point3D a = workPlaneFramePointToWorld(line.nurbs.controlPoints.first(), frame);
            const Point3D b = workPlaneFramePointToWorld(line.nurbs.controlPoints.last(), frame);
            passed &= check(validateNurbsCurve(line.nurbs) &&
                                std::abs(a.x - b.x) < 1.0e-8 &&
                                std::abs(a.y - b.y) < 1.0e-8 &&
                                std::abs(a.z - b.z) > 1.0,
                            "vertical side-view line must store a world Z segment");
        }
        // Exercise real Qt input and save/reload world CVs. Z starts from the
        // perspective XY floor here, so projecting input onto XY would fail.
        ViewportTransform projection;
        projection.setViewPreset(ViewportViewPreset::Perspective);
        const Point3D ends[] = {{10.0, 0.0, 0.0}, {0.0, 10.0, 0.0}, {0.0, 0.0, 10.0}};
        const int keys[] = {Qt::Key_X, Qt::Key_Y, Qt::Key_Z};
        for (int index = 0; index < 3; ++index) {
            probe->createNewDocument();
            probe->setOsnapEnabled(false);
            probe->setViewPreset(ViewportViewPreset::Perspective);
            passed &= check(waitForViewPreset(probe.get(), ViewportViewPreset::Perspective),
                            "perspective camera transition must finish before axis placement");
            probe->setTool(ToolId::Line);
            sendMouse(probe.get(), QEvent::MouseButtonPress, center,
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QKeyEvent key(QEvent::KeyPress, keys[index], Qt::NoModifier);
            QApplication::sendEvent(probe.get(), &key);
            QPointF endScreen;
            projection.worldPointToScreen(ends[index], interactionViewportSize, &endScreen);
            sendMouse(probe.get(), QEvent::MouseMove, endScreen,
                      Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            application.processEvents();
            saveGridCapture(QStringLiteral("line-world-axis-%1-preview").arg(index),
                            captureViewport(probe.get()));
            sendMouse(probe.get(), QEvent::MouseButtonPress, endScreen,
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            sendMouse(probe.get(), QEvent::MouseButtonPress, endScreen,
                      Qt::RightButton, Qt::RightButton, Qt::NoModifier);
            Document axisResult;
            const bool saved = probe->saveVignolaDocument(path, &error) &&
                loadVignolaDocument(path, &axisResult, &error);
            bool axisMatches = saved && axisResult.objects().size() == 1;
            if (axisMatches) {
                const Shape &line = axisResult.objects().first().geometry;
                const Point3D end = workPlaneFramePointToWorld(
                    line.nurbs.controlPoints.last(), shapeWorkPlaneFrame(line));
                axisMatches = validateNurbsCurve(line.nurbs) &&
                    std::abs(end.x - ends[index].x) < 1.0e-6 &&
                    std::abs(end.y - ends[index].y) < 1.0e-6 &&
                    std::abs(end.z - ends[index].z) < 1.0e-6;
            }
            passed &= check(axisMatches,
                "X/Y/Z keys must create the requested world axis in perspective, including outside XY");
        }
        probe->createNewDocument();
        probe->setOsnapEnabled(false);
        probe->setViewPreset(ViewportViewPreset::Perspective);
        passed &= check(waitForViewPreset(probe.get(), ViewportViewPreset::Perspective),
                        "chain camera transition must finish before drawing");
        probe->setTool(ToolId::Line);
        sendMouse(probe.get(), QEvent::MouseButtonPress, center,
                  Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        const Point3D chain[] = {{10.0, 0.0, 0.0}, {10.0, 10.0, 0.0}, {10.0, 10.0, 10.0}};
        QPointF endScreen;
        for (int index = 0; index < 3; ++index) {
            QKeyEvent key(QEvent::KeyPress, keys[index], Qt::NoModifier);
            QApplication::sendEvent(probe.get(), &key);
            projection.worldPointToScreen(chain[index], interactionViewportSize, &endScreen);
            sendMouse(probe.get(), QEvent::MouseMove, endScreen,
                      Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            sendMouse(probe.get(), QEvent::MouseButtonPress, endScreen,
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        }
        sendMouse(probe.get(), QEvent::MouseButtonPress, endScreen,
                  Qt::RightButton, Qt::RightButton, Qt::NoModifier);
        Document chainResult;
        passed &= check(probe->saveVignolaDocument(path, &error) &&
                            loadVignolaDocument(path, &chainResult, &error) &&
                            chainResult.objects().size() == 2,
                        "an XYZ path must preserve its planar NURBS runs");
        probe->executeCommand(ViewportCommand::Undo);
        passed &= check(probe->saveVignolaDocument(path, &error) &&
                            loadVignolaDocument(path, &chainResult, &error) &&
                            chainResult.objects().isEmpty(),
                        "one Undo must remove the whole XYZ drawing command");
        Document snapSource;
        Shape elevatedPoint;
        elevatedPoint.geometryType = GeometryType::Point;
        elevatedPoint.workPlaneOffset = 10.0;
        elevatedPoint.points = {QPointF(5.0, 6.0)};
        snapSource.append(elevatedPoint);
        passed &= check(saveVignolaDocument(path, snapSource, &error) &&
                            probe->loadVignolaDocument(path, &error),
                        "spatial OSnap fixture must load");
        probe->setOsnapEnabled(true);
        probe->setViewPreset(ViewportViewPreset::Perspective);
        passed &= check(waitForViewPreset(probe.get(), ViewportViewPreset::Perspective),
                        "snap camera transition must finish before drawing");
        probe->setTool(ToolId::Line);
        sendMouse(probe.get(), QEvent::MouseButtonPress, center,
                  Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        projection.worldPointToScreen({5.0, 6.0, 10.0}, interactionViewportSize, &endScreen);
        sendMouse(probe.get(), QEvent::MouseMove, endScreen,
                  Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        sendMouse(probe.get(), QEvent::MouseButtonPress, endScreen,
                  Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        sendMouse(probe.get(), QEvent::MouseButtonPress, endScreen,
                  Qt::RightButton, Qt::RightButton, Qt::NoModifier);
        Document snapResult;
        bool snapped = probe->saveVignolaDocument(path, &error) &&
            loadVignolaDocument(path, &snapResult, &error) && snapResult.size() == 2;
        if (snapped) {
            const Shape &line = snapResult.objects().last().geometry;
            const Point3D end = workPlaneFramePointToWorld(line.nurbs.controlPoints.last(),
                                                          shapeWorkPlaneFrame(line));
            snapped = std::abs(end.x - 5.0) < 1.0e-6 &&
                std::abs(end.y - 6.0) < 1.0e-6 && std::abs(end.z - 10.0) < 1.0e-6;
        }
        passed &= check(snapped,
                        "Line OSnap must keep the actual XYZ depth of an endpoint on another plane");
        probe->createNewDocument();
        probe->setOsnapEnabled(false);
        probe->setViewPreset(ViewportViewPreset::Isometric);
        passed &= check(waitForViewPreset(probe.get(), ViewportViewPreset::Isometric),
                        "oblique camera transition must finish before drawing");
        probe->setTool(ToolId::Line);
        sendMouse(probe.get(), QEvent::MouseButtonPress, center,
                  Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        const QPointF obliqueEnd = center + QPointF(80.0, 10.0);
        sendMouse(probe.get(), QEvent::MouseButtonPress, obliqueEnd,
                  Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        sendMouse(probe.get(), QEvent::MouseButtonPress, obliqueEnd,
                  Qt::RightButton, Qt::RightButton, Qt::NoModifier);
        Document obliqueResult;
        bool oblique = probe->saveVignolaDocument(path, &error) &&
            loadVignolaDocument(path, &obliqueResult, &error) && obliqueResult.size() == 1;
        if (oblique) {
            const WorkPlaneFrame frame = shapeWorkPlaneFrame(obliqueResult[0]);
            oblique = std::abs(frame.normal.x) > 0.1 && std::abs(frame.normal.y) > 0.1 &&
                std::abs(frame.normal.z) > 0.1 && validateNurbsCurve(obliqueResult[0].nurbs);
        }
        passed &= check(oblique,
                        "oblique orthographic Line input must use the actual camera plane, as the addon does");
    }
    if (QApplication::platformName() == QStringLiteral("xcb") &&
        !qEnvironmentVariable("CLASSICAD_VIEWPORT_CAPTURE_DIR").isEmpty()) {
        viewport->resize(blenderReferenceViewportSize);
        application.processEvents();
        waitForViewportTransition();

        // Blender's orthographic and perspective projections share the same
        // scale at the view target. The native view transform uses that shared
        // scale, so capture the default top view without a calibration zoom.
        waitForViewportTransition();
        const QImage topOrthographicCapture = captureViewport(viewport.get());
        saveGridCapture(QStringLiteral("viewport-native-top-ortho-scale-matched"),
                        topOrthographicCapture);
        passed &= check(
            topOrthographicCapture.size() == blenderReferenceViewportSize &&
                neutralGridLikePixels(topOrthographicCapture) > 200,
            "native top-view capture must use Blender's reference dimensions and show the grid");
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

        // Blender's baseline view distance of 60 maps to classiCAD zoom 1.0.
        // Use reciprocal 2x changes to compare Blender distances 30 and 120.
        viewport->setViewPreset(ViewportViewPreset::Top);
        waitForViewportTransition();
        sendWheel(viewport.get(), comparisonCenter, 456);
        application.processEvents();
        waitForViewportTransition();
        saveGridCapture(QStringLiteral("viewport-native-top-close-zoom"),
                        captureViewport(viewport.get()));
        sendWheel(viewport.get(), comparisonCenter, -912);
        application.processEvents();
        waitForViewportTransition();
        saveGridCapture(QStringLiteral("viewport-native-top-far-zoom"),
                        captureViewport(viewport.get()));
        sendWheel(viewport.get(), comparisonCenter, 456);
        application.processEvents();
        waitForViewportTransition();

        const std::array<std::pair<const char *, ViewportViewPreset>, 7> orthoViewMatrix{{
            {"top", ViewportViewPreset::Top},
            {"bottom", ViewportViewPreset::Bottom},
            {"front", ViewportViewPreset::Front},
            {"back", ViewportViewPreset::Back},
            {"right", ViewportViewPreset::Right},
            {"left", ViewportViewPreset::Left},
            {"isometric-ortho", ViewportViewPreset::Isometric},
        }};
        bool allNativeViewsCaptured = true;
        for (const auto &[name, preset] : orthoViewMatrix) {
            viewport->setViewPreset(preset);
            waitForViewportTransition();
            const QImage viewCapture = captureViewport(viewport.get());
            allNativeViewsCaptured &=
                !viewCapture.isNull() &&
                viewCapture.size() == blenderReferenceViewportSize;
            saveGridCapture(QStringLiteral("viewport-native-matched-") +
                                QString::fromLatin1(name),
                            viewCapture);
        }
        passed &= check(allNativeViewsCaptured,
                        "native matched-scale matrix must cover all six axis views and isometric orthographic at Blender's reference dimensions");

        // The close/far round trip restores the shared baseline zoom of 1.0.
        viewport->setViewPreset(ViewportViewPreset::Perspective);
        waitForViewportTransition();
        const QImage matchedPerspectiveCapture = captureViewport(viewport.get());
        allNativeViewsCaptured &=
            !matchedPerspectiveCapture.isNull() &&
            matchedPerspectiveCapture.size() == blenderReferenceViewportSize;
        saveGridCapture(QStringLiteral("viewport-native-matched-isometric-perspective"),
                        matchedPerspectiveCapture);
        passed &= check(allNativeViewsCaptured,
                        "native matched-scale matrix must include the perspective view at Blender's reference dimensions");

        viewport->setViewPreset(ViewportViewPreset::Top);
        waitForViewportTransition();
        viewport->resize(interactionViewportSize);
        application.processEvents();
        waitForViewportTransition();
    }

    if (QApplication::platformName() == QStringLiteral("xcb") &&
        !qEnvironmentVariable("CLASSICAD_VIEWPORT_CAPTURE_DIR").isEmpty()) {
        const QSize stage6ViewportSize(1311, 846);
        viewport->resize(stage6ViewportSize);
        viewport->setPanButton(Qt::RightButton);
        ViewportNavigationPreferences stage6Navigation =
            viewport->navigationPreferences();
        stage6Navigation.autoPerspective = true;
        stage6Navigation.zoomToMouse = true;
        stage6Navigation.orbitAroundActive = true;
        stage6Navigation.useMouseDepthNavigate = true;
        stage6Navigation.turntableSensitivityRadiansPerPixel =
            0.006981316953897476;
        stage6Navigation.orbitMethod = ViewportOrbitMethod::Turntable;
        stage6Navigation.trackballSensitivity = 1.0;
        stage6Navigation.zoomMethod = ViewportZoomMethod::Dolly;
        stage6Navigation.zoomAxis = ViewportZoomAxis::Vertical;
        stage6Navigation.invertMouseZoom = false;
        stage6Navigation.invertZoomWheel = false;
        viewport->setNavigationPreferences(stage6Navigation);
        application.processEvents();

        const QPointF stage6Center(stage6ViewportSize.width() * 0.5,
                                   stage6ViewportSize.height() * 0.5);
        const auto saveStage6Frame = [&](const QString &name) {
            const QImage image = captureViewport(viewport.get());
            saveGridCapture(QStringLiteral("stage6-") + name, image);
            return image;
        };
        const auto prepareStage6View = [&](ViewportViewPreset preset,
                                           const QString &name) {
            viewport->setViewPreset(preset);
            application.processEvents();
            waitForViewportTransition();
            const QImage image = saveStage6Frame(name + QStringLiteral("-before"));
            passed &= check(!image.isNull() && image.size() == stage6ViewportSize,
                            "Stage 6 navigation captures must use the Blender comparison viewport dimensions");
            return image;
        };
        const auto sendStage6Orbit = [&](const QPointF &start,
                                         const QPointF &delta) {
            const QPointF end = start + delta;
            sendMouse(viewport.get(), QEvent::MouseButtonPress, start,
                      Qt::RightButton, Qt::RightButton, Qt::ShiftModifier);
            sendMouse(viewport.get(), QEvent::MouseMove, end,
                      Qt::NoButton, Qt::RightButton, Qt::ShiftModifier);
            sendMouse(viewport.get(), QEvent::MouseButtonRelease, end,
                      Qt::RightButton, Qt::NoButton, Qt::ShiftModifier);
            application.processEvents();
            waitForViewportTransition();
        };
        const auto compareStage6Orbit = [&](ViewportViewPreset preset,
                                            const QString &name,
                                            const QPointF &start,
                                            const QPointF &delta) {
            const QImage before = prepareStage6View(preset, name);
            sendStage6Orbit(start, delta);
            const QImage after = saveStage6Frame(name + QStringLiteral("-after"));
            passed &= check(!before.isNull() && !after.isNull() && before != after &&
                                viewport->viewPreset() == ViewportViewPreset::Custom,
                            "Stage 6 turntable and trackball drags must change the actual viewport camera");
        };

        stage6Navigation.orbitMethod = ViewportOrbitMethod::Turntable;
        viewport->setNavigationPreferences(stage6Navigation);
        compareStage6Orbit(ViewportViewPreset::Top,
                           QStringLiteral("top-horizontal-50"),
                           stage6Center,
                           QPointF(50.0, 0.0));
        compareStage6Orbit(ViewportViewPreset::Top,
                           QStringLiteral("top-vertical-50"),
                           stage6Center,
                           QPointF(0.0, 50.0));
        compareStage6Orbit(ViewportViewPreset::Top,
                           QStringLiteral("top-horizontal-150"),
                           stage6Center,
                           QPointF(150.0, 0.0));
        compareStage6Orbit(ViewportViewPreset::Front,
                           QStringLiteral("front-horizontal-75"),
                           stage6Center,
                           QPointF(75.0, 0.0));
        compareStage6Orbit(ViewportViewPreset::Isometric,
                           QStringLiteral("iso-diagonal-minus-80-plus-40"),
                           stage6Center,
                           QPointF(-80.0, 40.0));

        stage6Navigation.orbitMethod = ViewportOrbitMethod::Trackball;
        viewport->setNavigationPreferences(stage6Navigation);
        const QPointF trackballStart = stage6Center + QPointF(-90.0, -40.0);
        compareStage6Orbit(ViewportViewPreset::Isometric,
                           QStringLiteral("trackball-offcenter-180-80"),
                           trackballStart,
                           QPointF(180.0, 80.0));

        stage6Navigation.orbitMethod = ViewportOrbitMethod::Turntable;
        viewport->setNavigationPreferences(stage6Navigation);
        viewport->setViewPreset(ViewportViewPreset::Top);
        application.processEvents();
        waitForViewportTransition();
        const QPointF stage6WheelPosition(981.0, 328.0);
        // Match the line spacing at this larger reference viewport before
        // comparing how one wheel notch changes scale and cursor anchoring.
        sendWheel(viewport.get(), stage6Center, 285);
        application.processEvents();
        waitForViewportTransition();
        const QImage topWheelBefore =
            saveStage6Frame(QStringLiteral("top-wheel-cursor-before"));
        sendWheel(viewport.get(), stage6WheelPosition, 120);
        application.processEvents();
        waitForViewportTransition();
        const QImage topWheelAfter =
            saveStage6Frame(QStringLiteral("top-wheel-cursor-after"));
        passed &= check(!topWheelBefore.isNull() && !topWheelAfter.isNull() &&
                            topWheelBefore != topWheelAfter,
                        "Stage 6 off-center wheel zoom must visibly change the top view");

        // Undo the cursor zoom at the same location, then return the classiCAD
        // zoom to 1.0 so the perspective comparison starts at Blender's
        // 60-unit distance with an unshifted target.
        sendWheel(viewport.get(), stage6WheelPosition, -120);
        sendWheel(viewport.get(), stage6Center, -285);
        application.processEvents();
        waitForViewportTransition();
        viewport->setViewPreset(ViewportViewPreset::Perspective);
        application.processEvents();
        waitForViewportTransition();
        const QImage perspectiveWheelBefore =
            saveStage6Frame(QStringLiteral("perspective-wheel-cursor-before"));
        sendWheel(viewport.get(), stage6WheelPosition, 120);
        application.processEvents();
        waitForViewportTransition();
        const QImage perspectiveWheelAfter =
            saveStage6Frame(QStringLiteral("perspective-wheel-cursor-after"));
        passed &= check(!perspectiveWheelBefore.isNull() &&
                            !perspectiveWheelAfter.isNull() &&
                            perspectiveWheelBefore != perspectiveWheelAfter,
                        "Stage 6 off-center wheel zoom must visibly change the perspective view");

        viewport->setViewPreset(ViewportViewPreset::Top);
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
        const QString glRendererName = openGlRendererName(nativeSurface);
        qInfo().noquote() << "Native viewport GL_RENDERER:" << glRendererName;
        passed &= check(!glRendererName.trimmed().isEmpty(),
                        "native viewport hardware check must report the active OpenGL renderer");
        if (qEnvironmentVariableIsSet("CLASSICAD_REQUIRE_HARDWARE_GL")) {
            const QString normalizedRenderer = glRendererName.toLower();
            const bool softwareRenderer =
                normalizedRenderer.contains(QStringLiteral("llvmpipe")) ||
                normalizedRenderer.contains(QStringLiteral("softpipe")) ||
                normalizedRenderer.contains(QStringLiteral("swrast")) ||
                normalizedRenderer.contains(QStringLiteral("swiftshader")) ||
                normalizedRenderer.contains(QStringLiteral("software rasterizer"));
            passed &= check(!softwareRenderer,
                            "hardware-only viewport run must not use a software OpenGL renderer");
        }

        const QSize gpuPatternSize(640, 480);
        const QStringList gpuLayerLineTypes = standardLayerLineTypes();
        QVector<Shape> gpuPatternShapes;
        QVector<ViewportSceneStroke> gpuPatternStrokes;
        QVector<qreal> gpuPatternWorldY;
        gpuPatternShapes.reserve(gpuLayerLineTypes.size());
        gpuPatternStrokes.reserve(gpuLayerLineTypes.size());
        gpuPatternWorldY.reserve(gpuLayerLineTypes.size());
        ViewportTransform gpuPatternTransform;
        gpuPatternTransform.setCameraPreferences(blenderCameraPreferences);
        for (int index = 0; index < gpuLayerLineTypes.size(); ++index) {
            const qreal rowY = 78.0 + index * 13.0;
            Shape line;
            line.geometryType = GeometryType::Line;
            line.workPlane = WorkPlane::XY;
            line.points = {
                gpuPatternTransform.screenToWorld(QPointF(60.0, rowY),
                                                  gpuPatternSize),
                gpuPatternTransform.screenToWorld(QPointF(580.0, rowY),
                                                  gpuPatternSize),
            };
            gpuPatternWorldY.append(line.points[0].y());
            gpuPatternShapes.append(std::move(line));

            const LayerGpuLinePattern layerPattern =
                layerGpuLinePattern(gpuLayerLineTypes[index]);
            ViewportSceneStroke stroke;
            stroke.shape = &gpuPatternShapes.back();
            stroke.color = QColor(245, 222, 112);
            stroke.width = 2.0f;
            stroke.linePatternScale = static_cast<float>(layerPattern.scale);
            switch (layerPattern.kind) {
            case LayerGpuLinePatternKind::Dashed:
                stroke.lineStyle = ViewportSceneLineStyle::Dashed;
                break;
            case LayerGpuLinePatternKind::Dotted:
                stroke.lineStyle = ViewportSceneLineStyle::Dotted;
                break;
            case LayerGpuLinePatternKind::Pattern:
                stroke.lineStyle = ViewportSceneLineStyle::Pattern;
                stroke.linePatternSegmentCount = static_cast<int>(
                    layerPattern.segments.size());
                for (int segmentIndex = 0;
                     segmentIndex < stroke.linePatternSegmentCount;
                     ++segmentIndex) {
                    stroke.linePatternSegmentsWidthUnits[
                        static_cast<std::size_t>(segmentIndex)] =
                        static_cast<float>(layerPattern.segments[segmentIndex]);
                }
                break;
            case LayerGpuLinePatternKind::Solid:
            case LayerGpuLinePatternKind::Unsupported:
                stroke.lineStyle = ViewportSceneLineStyle::Solid;
                break;
            }
            gpuPatternStrokes.append(stroke);
        }

        Shape gpuPicture;
        gpuPicture.geometryType = GeometryType::Picture;
        gpuPicture.workPlane = WorkPlane::XY;
        gpuPicture.points = {
            gpuPatternTransform.screenToWorld(QPointF(18.0, 18.0),
                                              gpuPatternSize),
            gpuPatternTransform.screenToWorld(QPointF(54.0, 18.0),
                                              gpuPatternSize),
            gpuPatternTransform.screenToWorld(QPointF(54.0, 54.0),
                                              gpuPatternSize),
            gpuPatternTransform.screenToWorld(QPointF(18.0, 54.0),
                                              gpuPatternSize),
        };
        gpuPicture.pictureImage = pictureImage;
        ViewportGpuSurface gpuSceneProbe;
        gpuSceneProbe.resize(gpuPatternSize);
        bool gpuPictureDrawSucceeded = false;
        bool gpuPatternDrawSucceeded = false;
        gpuSceneProbe.setDrawCallback(
            [&gpuPicture,
             &gpuPictureDrawSucceeded,
             &gpuPatternDrawSucceeded,
             &gpuPatternStrokes,
             &gpuPatternTransform,
             &gpuPatternSize](QPainter &painter,
                             BlenderGridRenderer &,
                             ViewportSceneRenderer &sceneRenderer,
                             ViewportSceneRenderer &,
                             ViewportControlPointRenderer &) {
                painter.fillRect(QRect(QPoint(0, 0), gpuPatternSize),
                                 QColor(34, 34, 34));
                painter.beginNativePainting();
                gpuPictureDrawSucceeded = sceneRenderer.drawPicture(
                    gpuPicture, gpuPatternTransform, gpuPatternSize, 1.0, 1.0f);
                gpuPatternDrawSucceeded = sceneRenderer.draw(
                    gpuPatternStrokes, gpuPatternTransform, gpuPatternSize, 1.0);
                painter.endNativePainting();
            });
        gpuSceneProbe.show();
        application.processEvents();
        const QImage gpuSceneProbeImage = gpuSceneProbe.grabFramebuffer();
        saveGridCapture(QStringLiteral("stage7-gpu-picture-and-layer-styles"),
                        gpuSceneProbeImage);
        const QColor gpuPictureRed = gpuSceneProbeImage.pixelColor(28, 28);
        const QColor gpuPictureBlue = gpuSceneProbeImage.pixelColor(28, 44);
        const QColor gpuPictureYellow = gpuSceneProbeImage.pixelColor(44, 44);
        const QColor gpuPictureTransparent = gpuSceneProbeImage.pixelColor(44, 28);
        const int solidStylePixelCount = [&]() {
            const QPointF screenPoint = gpuPatternTransform.workPlaneToScreen(
                QPointF(0.0, gpuPatternWorldY.first()),
                gpuPatternSize,
                WorkPlane::XY);
            int count = 0;
            const int row = qRound(screenPoint.y());
            for (int x = 60; x < 580; ++x) {
                const QColor pixel = gpuSceneProbeImage.pixelColor(x, row);
                count += pixel.red() > 180 && pixel.green() > 150 &&
                         pixel.blue() < 170;
            }
            return count;
        }();
        bool allGpuPatternsVisible = solidStylePixelCount > 400;
        bool patternedStylesHaveGaps = true;
        for (int index = 1; index < gpuPatternWorldY.size(); ++index) {
            const QPointF screenPoint = gpuPatternTransform.workPlaneToScreen(
                QPointF(0.0, gpuPatternWorldY[index]),
                gpuPatternSize,
                WorkPlane::XY);
            const int row = qRound(screenPoint.y());
            int count = 0;
            for (int x = 60; x < 580; ++x) {
                const QColor pixel = gpuSceneProbeImage.pixelColor(x, row);
                count += pixel.red() > 180 && pixel.green() > 150 &&
                         pixel.blue() < 170;
            }
            allGpuPatternsVisible &= count > 10;
            patternedStylesHaveGaps &= count < solidStylePixelCount - 10;
        }
        const QColor gpuPictureBackground(34, 34, 34);
        const bool transparentPictureKeepsBackground =
            std::abs(gpuPictureTransparent.red() - gpuPictureBackground.red()) <= 8 &&
            std::abs(gpuPictureTransparent.green() - gpuPictureBackground.green()) <= 8 &&
            std::abs(gpuPictureTransparent.blue() - gpuPictureBackground.blue()) <= 8;
        passed &= check(gpuSceneProbe.isValid() && gpuPictureDrawSucceeded &&
                            gpuPatternDrawSucceeded &&
                            gpuPictureRed.red() > gpuPictureRed.green() + 60 &&
                            gpuPictureBlue.blue() > gpuPictureBlue.red() + 60 &&
                            gpuPictureYellow.red() > 150 &&
                            gpuPictureYellow.green() > 150 &&
                            transparentPictureKeepsBackground,
                        "native picture shader must draw committed RGBA pixels and preserve transparent areas");
        passed &= check(allGpuPatternsVisible && patternedStylesHaveGaps,
                        "native stroke shader must visibly draw every built-in CAD layer line style with the expected gaps");
        gpuSceneProbe.hide();

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
            transform.setCameraPreferences(wideClipGridPreferences);
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
            transform.setCameraPreferences(wideClipGridPreferences);
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
        passed &= check(benchmarkNativeViewportFrames(
                            viewport.get(), 90, QStringLiteral("small scene")),
                        "native viewport small-scene benchmark must receive each requested frame swap");

        if (qEnvironmentVariableIsSet("CLASSICAD_RUN_LARGE_GPU_CHECKS")) {
            constexpr int largeSceneStrokeCount = 1500;
            Document largeGpuDocument;
            const LayerId largeSceneLayer = largeGpuDocument.activeLayerId();
            largeGpuDocument.setLayerLineType(largeSceneLayer,
                                              QStringLiteral("Continuous"));
            largeGpuDocument.setLayerColor(largeSceneLayer,
                                           QColor(245, 222, 112));
            for (int index = 0; index < largeSceneStrokeCount; ++index) {
                const qreal y = -240.0 +
                                480.0 * index /
                                    static_cast<qreal>(largeSceneStrokeCount - 1);
                Shape line;
                line.geometryType = GeometryType::Line;
                line.workPlane = WorkPlane::XY;
                line.points = {QPointF(-600.0, y), QPointF(600.0, y)};
                largeGpuDocument.append(line);
            }

            QTemporaryDir largeSceneDirectory;
            const QString largeScenePath = largeSceneDirectory.filePath(
                QStringLiteral("stage7-large-scene.vignola"));
            QString largeSceneError;
            const bool largeSceneSaved = largeSceneDirectory.isValid() &&
                saveVignolaDocument(largeScenePath,
                                    largeGpuDocument,
                                    &largeSceneError);
            passed &= check(largeSceneSaved,
                            "large GPU performance scene must save to a temporary native document");
            if (largeSceneSaved) {
                passed &= check(viewport->loadVignolaDocument(
                                    largeScenePath, &largeSceneError),
                                "large GPU performance scene must load into the native viewport");
                viewport->resize(QSize(1280, 720));
                viewport->setViewPreset(ViewportViewPreset::Top);
                application.processEvents();
                waitForViewportTransition();
                saveGridCapture(QStringLiteral("stage7-large-scene-1500-strokes"),
                                captureViewport(viewport.get()));
                qInfo().noquote()
                    << QStringLiteral("Large-scene hardware check: renderer=%1 strokes=%2 viewport=%3x%4")
                           .arg(glRendererName)
                           .arg(largeSceneStrokeCount)
                           .arg(viewport->width())
                           .arg(viewport->height());
                passed &= check(benchmarkNativeViewportFrames(
                                    viewport.get(), 60,
                                    QStringLiteral("1500 strokes at 1280x720")),
                                "large-scene viewport benchmark must receive each requested frame swap");
            }
        }
    }

    return passed ? 0 : 1;
}
