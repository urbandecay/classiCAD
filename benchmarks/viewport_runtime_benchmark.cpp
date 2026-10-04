/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "core/document/document.h"
#include "core/geometry/curve_construction.h"
#include "core/geometry/nurbs_surface_factory.h"
#include "core/geometry/shape_mapping.h"
#include "core/history/document_transaction.h"
#include "core/history/history.h"
#include "services/hit_testing/curve_hit_tester.h"
#include "services/sampling/curve_sampler.h"
#include "services/sampling/surface_tessellation_cache.h"
#include "services/snapping/snap_engine.h"
#include "services/viewport/viewport_transform.h"
#include "ui/viewport/viewport_depth_geometry.h"
#include "ui/viewport/viewport_geometry_cache.h"
#include "ui/viewport/viewport_render_frame.h"
#include "ui/viewport/viewport_renderer.h"
#include "ui/viewport/viewport_scene_renderer.h"

#include <QElapsedTimer>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QImage>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions_3_3_Core>
#include <QOffscreenSurface>
#include <QSurfaceFormat>

#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
#include <sys/resource.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

using namespace classiCAD;

namespace {

struct TimingSummary {
    double medianMilliseconds = 0.0;
    double p95Milliseconds = 0.0;
};

TimingSummary summarize(std::vector<double> samples)
{
    if (samples.empty()) {
        return {};
    }
    std::sort(samples.begin(), samples.end());
    const std::size_t p95Index = std::min(
        samples.size() - 1,
        static_cast<std::size_t>(std::ceil(samples.size() * 0.95)) - 1);
    return {samples[samples.size() / 2], samples[p95Index]};
}

template<typename Operation>
TimingSummary measure(int warmupCount, int sampleCount, Operation operation)
{
    for (int index = 0; index < warmupCount; ++index) {
        operation();
    }

    std::vector<double> samples;
    samples.reserve(sampleCount);
    for (int index = 0; index < sampleCount; ++index) {
        QElapsedTimer timer;
        timer.start();
        operation();
        samples.push_back(timer.nsecsElapsed() / 1.0e6);
    }
    return summarize(std::move(samples));
}

void printTiming(const char *name, const TimingSummary &timing)
{
    std::cout << name << " median=" << timing.medianMilliseconds
              << " ms p95=" << timing.p95Milliseconds << " ms";
}

Document makeScene(int curveCount)
{
    Document document;
    const WorkPlaneFrame frame = makeWorkPlaneFrame(WorkPlane::XY);
    for (int index = 0; index < curveCount; ++index) {
        const qreal y = (static_cast<qreal>(index) - curveCount / 2.0) * 0.2;
        Shape shape;
        shape.geometryType = GeometryType::Bezier;
        shape.workPlaneFrame = frame;
        shape.points = {QPointF(-80.0, y),
                        QPointF(-40.0, y + 3.0),
                        QPointF(40.0, y - 3.0),
                        QPointF(80.0, y)};
        shape.nurbs = makeBezierNurbs(shape.points);
        document.append(shape);
    }
    return document;
}

Document makeSurfaceScene(int surfaceCount)
{
    Document document;
    const WorkPlaneFrame frame = makeWorkPlaneFrame(WorkPlane::XY);
    for (int index = 0; index < surfaceCount; ++index) {
        const QPointF center((index % 16) * 5.0,
                             (index / 16) * 5.0);
        const NurbsCurve2D circle = makeCircleNurbs(
            {center, center + QPointF(1.0, 0.0)});
        NurbsSurface3D surface;
        if (!makeNurbsExtrusionSurface(circle, frame, {0.0, 0.0, 2.0},
                                       &surface)) {
            continue;
        }
        Shape shape;
        shape.geometryType = GeometryType::NurbsSurface;
        shape.nurbsSurface = std::move(surface);
        document.append(shape);
    }
    return document;
}

ViewportTransform makeCamera()
{
    ViewportTransform camera;
    camera.setViewPreset(ViewportViewPreset::Top);
    camera.zoom() = 12.0;
    return camera;
}

ViewportRenderFrame makePreparedFrame(
    const Document &document,
    const ViewportTransform &camera,
    const QSize &viewportSize,
    ViewportGeometryCache *geometryCache,
    const SurfaceTessellationCache *surfaceCache)
{
    ViewportRenderFrame frame = buildViewportRenderFrame(
        document, camera, viewportSize, ViewportRenderFrameInput{});
    geometryCache->prepareFrame(&frame, surfaceCache);
    return frame;
}

QVector<ViewportSceneStroke> makeStrokes(ViewportRenderFrame &frame)
{
    QVector<ViewportSceneStroke> strokes;
    strokes.reserve(frame.objects.size());
    for (const ViewportRenderObject &object : frame.objects) {
        ViewportSceneStroke stroke;
        if (makeViewportSceneStrokes(object, true, &stroke)) {
            strokes.append(std::move(stroke));
        }
    }
    return strokes;
}

void benchmarkServices(Document &document,
                       ViewportTransform &camera,
                       const QSize &viewportSize,
                       int curveCount)
{
    constexpr int warmupCount = 3;
    constexpr int sampleCount = 21;
    CurveSampler sampler;
    CurveHitTester hitTester;
    SnapEngine snapEngine;
    SnapSettings settings;
    settings.enabled = true;
    settings.endpoint = true;
    settings.midpoint = false;
    settings.intersection = false;
    settings.center = false;
    settings.perpendicular = false;
    settings.tangent = false;
    settings.near = true;
    settings.controlPoint = false;
    snapEngine.setSettings(settings);

    const QPointF screenCenter(viewportSize.width() * 0.5,
                               viewportSize.height() * 0.5);
    quint64 checksum = 0;
    const TimingSummary candidateTiming = measure(
        warmupCount, sampleCount, [&]() {
            checksum += static_cast<quint64>(
                snapEngine.snapCandidatesForScene(document, {}, camera,
                                                  viewportSize).size());
        });
    const TimingSummary snapTiming = measure(
        warmupCount, sampleCount, [&]() {
            const SnapResult result = snapEngine.findSnapPoint(
                document, QPointF(0.0, 0.0), true, {}, camera, viewportSize,
                {}, false, false);
            checksum += static_cast<quint64>(result.type) +
                        static_cast<quint64>(result.hasWorldPoint);
        });
    const TimingSummary hitTiming = measure(
        warmupCount, sampleCount, [&]() {
            checksum += static_cast<quint64>(
                hitTester.hitTestShape(document, screenCenter, camera,
                                       viewportSize, true) + 1);
        });
    const TimingSummary sampleTiming = measure(
        warmupCount, sampleCount, [&]() {
            checksum += static_cast<quint64>(
                sampler.sampleDocument(document, camera, viewportSize).size());
        });

    std::cout << "curves=" << curveCount << ' ';
    printTiming("snap-candidate-generation", candidateTiming);
    std::cout << ' ';
    printTiming("snap-resolution", snapTiming);
    std::cout << ' ';
    printTiming("scene-hit-test", hitTiming);
    std::cout << ' ';
    printTiming("screen-sample-generation", sampleTiming);
    std::cout << " checksum=" << checksum << '\n';
}

void benchmarkCpuRedraw(const Document &document,
                        ViewportTransform camera,
                        const QSize &viewportSize,
                        int curveCount)
{
    CurveHitTester hitTester;
    SurfaceTessellationCache surfaceCache;
    ViewportGeometryCache geometryCache;
    ViewportRenderFrame frame = makePreparedFrame(document,
                                                   camera,
                                                   viewportSize,
                                                   &geometryCache,
                                                   &surfaceCache);
    ViewportRenderer renderer(camera, hitTester, &surfaceCache);
    QImage image(viewportSize, QImage::Format_ARGB32_Premultiplied);
    quint64 checksum = 0;

    const auto redrawCurves = [&](int iteration) {
        camera.pan() = QPointF((iteration % 5) * 0.25,
                               (iteration % 3) * -0.2);
        image.fill(QColor(QStringLiteral("#282828")));
        QPainter painter(&image);
        const WorkPlaneFrame previousFrame = camera.workPlaneFrame();
        for (const ViewportRenderObject &object : frame.objects) {
            camera.setWorkPlaneFrame(shapeWorkPlaneFrame(object.shape));
            renderer.drawShape(painter,
                               object.shape,
                               viewportSize,
                               false,
                               false,
                               true,
                               QColor(QStringLiteral("#d28b45")),
                               QStringLiteral("Continuous"),
                               0.0,
                               object.objectId,
                               object.geometryRevision,
                               object.preparedDepthGeometry.data());
        }
        camera.setWorkPlaneFrame(previousFrame);
        painter.end();
        checksum += image.pixel(viewportSize.width() / 2,
                                viewportSize.height() / 2);
    };

    const auto redrawGrid = [&](int iteration) {
        camera.pan() = QPointF((iteration % 5) * 0.25,
                               (iteration % 3) * -0.2);
        image.fill(QColor(QStringLiteral("#282828")));
        QPainter painter(&image);
        renderer.drawGrid(painter, viewportSize);
        renderer.drawOrigin(painter, viewportSize);
        painter.end();
        checksum += image.pixel(viewportSize.width() / 2,
                                viewportSize.height() / 2);
    };

    const auto redraw = [&](int iteration) {
        camera.pan() = QPointF((iteration % 5) * 0.25,
                               (iteration % 3) * -0.2);
        image.fill(QColor(QStringLiteral("#282828")));
        QPainter painter(&image);
        renderer.drawGrid(painter, viewportSize);
        renderer.drawOrigin(painter, viewportSize);
        const WorkPlaneFrame previousFrame = camera.workPlaneFrame();
        for (const ViewportRenderObject &object : frame.objects) {
            camera.setWorkPlaneFrame(shapeWorkPlaneFrame(object.shape));
            renderer.drawShape(painter,
                               object.shape,
                               viewportSize,
                               false,
                               false,
                               true,
                               QColor(QStringLiteral("#d28b45")),
                               QStringLiteral("Continuous"),
                               0.0,
                               object.objectId,
                               object.geometryRevision,
                               object.preparedDepthGeometry.data());
        }
        camera.setWorkPlaneFrame(previousFrame);
        painter.end();
        checksum += image.pixel(viewportSize.width() / 2,
                                viewportSize.height() / 2);
    };

    const TimingSummary curveTiming = measure(3, 21, [&]() {
        static int iteration = 0;
        redrawCurves(iteration++);
    });
    const TimingSummary gridTiming = measure(3, 21, [&]() {
        static int iteration = 0;
        redrawGrid(iteration++);
    });
    const TimingSummary timing = measure(3, 21, [&]() {
        static int iteration = 0;
        redraw(iteration++);
    });
    std::cout << "curves=" << curveCount << ' ';
    printTiming("cpu-fallback-curve-scene-redraw", curveTiming);
    std::cout << ' ';
    printTiming("cpu-grid-origin-redraw", gridTiming);
    std::cout << ' ';
    printTiming("cpu-fallback-scene-redraw", timing);
    std::cout << " (grid+origin+curves; prepared double-precision world samples)"
              << " checksum=" << checksum << '\n';
}

bool benchmarkNativeGlRedraw(const Document &document,
                             ViewportTransform camera,
                             const QSize &viewportSize,
                             int objectCount,
                             const char *objectLabel = "curves");

void benchmarkSurfaceWorkload(int surfaceCount,
                              const QSize &viewportSize)
{
    Document document = makeSurfaceScene(surfaceCount);
    ViewportTransform camera = makeCamera();
    SurfaceTessellationCache surfaceCache;
    ViewportGeometryCache geometryCache;
    ViewportRenderFrame frame = makePreparedFrame(document,
                                                   camera,
                                                   viewportSize,
                                                   &geometryCache,
                                                   &surfaceCache);
    CurveHitTester hitTester(&surfaceCache);
    ViewportRenderer renderer(camera, hitTester, &surfaceCache);
    QImage image(viewportSize, QImage::Format_ARGB32_Premultiplied);
    quint64 checksum = 0;

    const TimingSummary redrawTiming = measure(2, 7, [&]() {
        image.fill(QColor(QStringLiteral("#282828")));
        QPainter painter(&image);
        for (const ViewportRenderObject &object : frame.objects) {
            camera.setWorkPlaneFrame(shapeWorkPlaneFrame(object.shape));
            renderer.drawShape(painter,
                               object.shape,
                               viewportSize,
                               false,
                               false,
                               true,
                               QColor(QStringLiteral("#ffffff")),
                               QStringLiteral("Continuous"),
                               0.0,
                               object.objectId,
                               object.geometryRevision,
                               object.preparedDepthGeometry.data());
        }
        painter.end();
        checksum += image.pixel(viewportSize.width() / 2,
                                viewportSize.height() / 2);
    });

    const QPointF cursor(viewportSize.width() * 0.5,
                         viewportSize.height() * 0.5);
    int hitShape = -1;
    const TimingSummary hitTiming = measure(1, 5, [&]() {
        hitShape = hitTester.hitTestShape(document,
                                          cursor,
                                          camera,
                                          viewportSize,
                                          true);
        checksum += static_cast<quint64>(hitShape + 1);
    });
    ViewportTransform isometricCamera = makeCamera();
    isometricCamera.setViewPreset(ViewportViewPreset::Isometric);
    isometricCamera.zoom() = 12.0;
    const int isometricHitShape = hitTester.hitTestShape(document,
                                                         cursor,
                                                         isometricCamera,
                                                         viewportSize,
                                                         true);

    std::cout << "surfaces=" << document.size() << ' ';
    printTiming("cpu-surface-scene-redraw", redrawTiming);
    std::cout << ' ';
    printTiming("surface-selection-hit-test", hitTiming);
    std::cout << " (48x48 trim mesh and 9 isocurves per direction)"
              << " top-cursor-hit-index=" << hitShape
              << " isometric-cursor-hit-index=" << isometricHitShape
              << " checksum=" << checksum << '\n';
    benchmarkNativeGlRedraw(document,
                            camera,
                            viewportSize,
                            document.size(),
                            "surfaces");
}

bool benchmarkNativeGlRedraw(const Document &document,
                             ViewportTransform camera,
                             const QSize &viewportSize,
                             int objectCount,
                             const char *objectLabel)
{
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setSamples(0);

    QOpenGLContext context;
    context.setFormat(format);
    if (!context.create()) {
        std::cout << objectLabel << '=' << objectCount
                  << " native-gl=unavailable (context creation failed)\n";
        return false;
    }

    QOffscreenSurface surface;
    surface.setFormat(context.format());
    surface.create();
    if (!surface.isValid() || !context.makeCurrent(&surface)) {
        std::cout << objectLabel << '=' << objectCount
                  << " native-gl=unavailable (offscreen surface unavailable)\n";
        return false;
    }

    QOpenGLFunctions_3_3_Core functions;
    if (!functions.initializeOpenGLFunctions()) {
        std::cout << objectLabel << '=' << objectCount
                  << " native-gl=unavailable (OpenGL 3.3 functions unavailable)\n";
        context.doneCurrent();
        return false;
    }

    QOpenGLFramebufferObjectFormat framebufferFormat;
    framebufferFormat.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
    QOpenGLFramebufferObject framebuffer(viewportSize, framebufferFormat);
    if (!framebuffer.isValid()) {
        std::cout << objectLabel << '=' << objectCount
                  << " native-gl=unavailable (framebuffer creation failed)\n";
        context.doneCurrent();
        return false;
    }

    SurfaceTessellationCache surfaceCache;
    ViewportGeometryCache geometryCache;
    ViewportRenderFrame frame = makePreparedFrame(document,
                                                   camera,
                                                   viewportSize,
                                                   &geometryCache,
                                                   &surfaceCache);
    const QVector<ViewportSceneStroke> strokes = makeStrokes(frame);
    quint64 checksum = 0;
    bool allDrawsSucceeded = true;
    {
        ViewportSceneRenderer renderer;
        const auto draw = [&](int iteration) {
            camera.pan() = QPointF((iteration % 5) * 0.25,
                                  (iteration % 3) * -0.2);
            framebuffer.bind();
            functions.glViewport(0, 0, viewportSize.width(), viewportSize.height());
            functions.glClearColor(0.16f, 0.16f, 0.16f, 1.0f);
            functions.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            const bool succeeded = renderer.draw(strokes,
                                                 camera,
                                                 viewportSize,
                                                 1.0);
            functions.glFinish();
            framebuffer.release();
            allDrawsSucceeded = allDrawsSucceeded && succeeded;
            checksum += static_cast<quint64>(succeeded);
        };

        draw(0); // Compile shaders and populate the renderer's static buffers.
        const TimingSummary timing = measure(3, 21, [&]() {
            static int iteration = 1;
            draw(iteration++);
        });
        std::cout << objectLabel << '=' << objectCount << ' ';
        printTiming("native-gl-cached-scene-redraw", timing);
        std::cout << " (prepared world vertices; no grid/Qt overlays)"
                  << " checksum=" << checksum << '\n';
    }
    context.doneCurrent();
    return allDrawsSucceeded;
}

void benchmarkInvalidation(Document &document,
                           const ViewportTransform &camera,
                           const QSize &viewportSize,
                           int curveCount)
{
    SurfaceTessellationCache surfaceCache;
    ViewportGeometryCache geometryCache;
    ViewportRenderFrame frame = makePreparedFrame(document,
                                                   camera,
                                                   viewportSize,
                                                   &geometryCache,
                                                   &surfaceCache);
    if (frame.objects.isEmpty()) {
        return;
    }
    const QVector<QSharedPointer<const ViewportDepthGeometry>> before = [&]() {
        QVector<QSharedPointer<const ViewportDepthGeometry>> values;
        values.reserve(frame.objects.size());
        for (const ViewportRenderObject &object : frame.objects) {
            values.append(object.preparedDepthGeometry);
        }
        return values;
    }();

    int changedGeometryCount = 0;
    int iteration = 0;
    const TimingSummary timing = measure(2, 15, [&]() {
        const ObjectId editedId = document.objectIdAt(0);
        document.mutateGeometry(editedId, [&](Shape &shape) {
            if (shape.nurbs.controlPoints.isEmpty()) {
                return false;
            }
            shape.nurbs.controlPoints[0].rx() += 0.001;
            return true;
        });
        frame = buildViewportRenderFrame(
            document, camera, viewportSize, ViewportRenderFrameInput{});
        geometryCache.prepareFrame(&frame, &surfaceCache);
        int changed = 0;
        for (int index = 0; index < frame.objects.size(); ++index) {
            if (frame.objects[index].preparedDepthGeometry.data() !=
                before[index].data()) {
                ++changed;
            }
        }
        changedGeometryCount = changed;
        ++iteration;
    });

    std::cout << "curves=" << curveCount << ' ';
    printTiming("single-object-frame+cache-refresh", timing);
    std::cout << " changed-world-geometry-per-edit=" << changedGeometryCount
              << " edits=" << iteration << '\n';
}

long peakResidentKilobytes()
{
#if defined(Q_OS_LINUX)
    rusage usage{};
    return getrusage(RUSAGE_SELF, &usage) == 0 ? usage.ru_maxrss : -1;
#elif defined(Q_OS_MACOS)
    rusage usage{};
    return getrusage(RUSAGE_SELF, &usage) == 0
               ? static_cast<long>(usage.ru_maxrss / 1024)
               : -1;
#else
    return -1;
#endif
}

long residentKilobytes()
{
#if defined(Q_OS_LINUX)
    std::ifstream statusFile("/proc/self/status");
    std::string line;
    while (std::getline(statusFile, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            long value = -1;
            std::string unit;
            std::istringstream stream(line.substr(6));
            stream >> value >> unit;
            return value;
        }
    }
#endif
    return -1;
}

int benchmarkHistory(int objectCount)
{
    if (objectCount <= 0) {
        std::cerr << "history object count must be positive\n";
        return 2;
    }

    Document document = makeScene(objectCount);
    History history(document);
    const long baselineRss = residentKilobytes();
    const long baselinePeakRss = peakResidentKilobytes();
    std::vector<double> commitSamples;
    constexpr int editCount = 5;
    for (int index = 0; index < editCount; ++index) {
        const ObjectId editedId = document.objectIdAt(index % objectCount);
        QElapsedTimer timer;
        timer.start();
        DocumentTransaction transaction(document, history);
        Shape *shape = transaction.editGeometry(editedId);
        if (shape == nullptr || shape->nurbs.controlPoints.isEmpty()) {
            return 3;
        }
        shape->nurbs.controlPoints[0].rx() += 0.01;
        if (!transaction.commit()) {
            return 4;
        }
        commitSamples.push_back(timer.nsecsElapsed() / 1.0e6);
    }
    const long afterHistoryRss = residentKilobytes();
    const long afterHistoryPeakRss = peakResidentKilobytes();

    std::vector<double> undoSamples;
    for (int index = 0; index < editCount; ++index) {
        QElapsedTimer timer;
        timer.start();
        if (!history.undo()) {
            return 5;
        }
        undoSamples.push_back(timer.nsecsElapsed() / 1.0e6);
    }
    std::vector<double> redoSamples;
    for (int index = 0; index < editCount; ++index) {
        QElapsedTimer timer;
        timer.start();
        if (!history.redo()) {
            return 6;
        }
        redoSamples.push_back(timer.nsecsElapsed() / 1.0e6);
    }
    const long afterUndoRedoRss = residentKilobytes();
    const long afterUndoRedoPeakRss = peakResidentKilobytes();

    std::cout << "history objects=" << objectCount
              << " committed-edits=" << editCount << ' ';
    printTiming("transaction+snapshot-commit", summarize(commitSamples));
    std::cout << ' ';
    printTiming("undo", summarize(undoSamples));
    std::cout << ' ';
    printTiming("redo", summarize(redoSamples));
    std::cout << " rss-baseline=" << baselineRss << " KB"
              << " rss-after-commits=" << afterHistoryRss << " KB"
              << " rss-after-undo-redo=" << afterUndoRedoRss << " KB"
              << " rss-delta="
              << (baselineRss >= 0 && afterUndoRedoRss >= baselineRss
                      ? afterUndoRedoRss - baselineRss
                      : -1)
              << " KB"
              << " peak-rss-baseline=" << baselinePeakRss << " KB"
              << " peak-rss-after-commits=" << afterHistoryPeakRss << " KB"
              << " peak-rss-after-undo-redo=" << afterUndoRedoPeakRss << " KB"
              << " retained-undo-snapshots=" << history.undoCount() << '\n';
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc == 3 && std::string(argv[1]) == "--history-only") {
        QCoreApplication application(argc, argv);
        return benchmarkHistory(std::atoi(argv[2]));
    }
    QGuiApplication application(argc, argv);

    constexpr QSize viewportSize{1280, 720};
    for (const int curveCount : {64, 256, 1024}) {
        Document document = makeScene(curveCount);
        ViewportTransform camera = makeCamera();
        benchmarkServices(document, camera, viewportSize, curveCount);
        benchmarkCpuRedraw(document, camera, viewportSize, curveCount);
        benchmarkNativeGlRedraw(document, camera, viewportSize, curveCount);
        benchmarkInvalidation(document, camera, viewportSize, curveCount);
    }
    for (const int surfaceCount : {16, 64, 128, 160}) {
        benchmarkSurfaceWorkload(surfaceCount, viewportSize);
    }
    return 0;
}
