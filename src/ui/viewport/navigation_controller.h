#pragma once

#include "services/viewport/viewport_transform.h"
#include "navigation_gizmo.h"

#include <QMouseEvent>
#include <QElapsedTimer>
#include <QPoint>
#include <QPointF>
#include <QSize>

#include <functional>
#include <memory>

class QVariantAnimation;
class QWidget;

namespace classiCAD {

struct NavigationControllerCallbacks {
    std::function<void(const QPointF &)> beginOrbitAt;
    std::function<Qt::CursorShape()> idleCursor;
    std::function<void()> requestUpdate;
    std::function<void()> coordinatesChanged;
    std::function<void()> viewStateChanged;
};

struct WheelZoomResult {
    bool changed = false;
    qreal oldZoom = 1.0;
    QPointF worldBefore;
    QPointF worldAfter;
};

// Owns camera navigation input state and preset animation. The viewport keeps
// scene-specific orbit-pivot picking behind the beginOrbitAt callback.
class NavigationController final {
public:
    NavigationController(QWidget &host,
                         ViewportTransform &transform,
                         const ViewportNavigationGizmo &gizmo,
                         NavigationControllerCallbacks callbacks,
                         Qt::MouseButton panButton = Qt::MiddleButton);
    ~NavigationController();

    NavigationController(const NavigationController &) = delete;
    NavigationController &operator=(const NavigationController &) = delete;

    void stopAnimation();
    void animateCameraChange(const std::function<void()> &setTarget);
    void setPanButton(Qt::MouseButton button);
    Qt::MouseButton panButton() const;

    bool handleGizmoPress(const QPointF &screenPosition,
                          const QSize &viewportSize);
    bool handleGizmoMove(const QPointF &screenPosition,
                         Qt::MouseButtons buttons,
                         const QSize &viewportSize);
    bool handleGizmoRelease(Qt::MouseButton button,
                            const QPointF &screenPosition,
                            const QSize &viewportSize);
    bool handlePanPress(Qt::MouseButton button,
                        Qt::KeyboardModifiers modifiers,
                        const QPointF &screenPosition,
                        const QSize &viewportSize);
    bool handlePanMove(const QPointF &screenPosition,
                       const QSize &viewportSize);
    bool finishPointerRelease(Qt::MouseButton button);
    bool updateGizmoHover(const QPointF &screenPosition,
                          const QSize &viewportSize,
                          bool updateIdleCursor);
    void pointerLeave();

    WheelZoomResult zoomFromWheel(const QPointF &screenPosition,
                                  int angleDeltaY,
                                  int pixelDeltaY,
                                  const QSize &viewportSize);

    QPointF hoverPosition() const;
    bool hasPressedGizmo() const;
    bool isPanning() const;
    bool panMoved() const;

private:
    void requestUpdate() const;
    void notifyCoordinatesChanged() const;
    void notifyViewStateChanged() const;
    Qt::CursorShape idleCursor() const;
    void beginOrbit(const QPointF &screenPosition, const QSize &viewportSize);
    void notifyCameraModeChanged(ViewportViewPreset previousPreset,
                                 bool previousPerspective);
    void updateCursorForAction(BlenderNavigationAction action);
    QString tooltipForHit(const BlenderNavigationHit &hit) const;

    QWidget &host_;
    ViewportTransform &transform_;
    const ViewportNavigationGizmo &gizmo_;
    NavigationControllerCallbacks callbacks_;
    std::unique_ptr<QVariantAnimation> animation_;
    ViewportCameraState animationStart_;
    ViewportCameraState animationEnd_;
    QPointF hoverPosition_{-1000.0, -1000.0};
    QPointF pressPosition_;
    QPointF lastPosition_;
    QElapsedTimer panPressTimer_;
    BlenderNavigationHit pressHit_;
    BlenderNavigationAction pressedAction_ = BlenderNavigationAction::None;
    Qt::MouseButton panButton_ = Qt::MiddleButton;
    QPoint panStartPosition_;
    bool navigationMoved_ = false;
    bool panning_ = false;
    bool orbiting_ = false;
    bool panMoved_ = false;
    bool delayedRightPan_ = false;
    bool panMotionApplied_ = false;
};

} // namespace classiCAD
