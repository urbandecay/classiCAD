#include "navigation_controller.h"

#include <QToolTip>
#include <QVariantAnimation>
#include <QWidget>
#include <QLineF>

#include <algorithm>
#include <cmath>

namespace classiCAD {
namespace {

constexpr qint64 rightPanActivationDelayMs = 110;
constexpr qreal panDragThresholdPixels = 3.0;

qreal directionDot(const Point3D &first, const Point3D &second)
{
    return first.x * second.x + first.y * second.y + first.z * second.z;
}

} // namespace

NavigationController::NavigationController(
    QWidget &host,
    ViewportTransform &transform,
    const ViewportNavigationGizmo &gizmo,
    NavigationControllerCallbacks callbacks,
    Qt::MouseButton panButton)
    : host_(host),
      transform_(transform),
      gizmo_(gizmo),
      callbacks_(std::move(callbacks)),
      animation_(std::make_unique<QVariantAnimation>()),
      panButton_(panButton)
{
    animation_->setDuration(200);
    animation_->setStartValue(0.0);
    animation_->setEndValue(1.0);
    animation_->setEasingCurve(QEasingCurve::Linear);
    QObject::connect(animation_.get(), &QVariantAnimation::valueChanged,
                     animation_.get(), [this](const QVariant &value) {
        const qreal time = value.toReal();
        // Blender's smooth view uses 3*t*t - 2*t*t*t.
        const qreal progress = time * time * (3.0 - 2.0 * time);
        ViewportCameraState state;
        const qreal startDistance = 1.0 / animationStart_.zoom;
        const qreal endDistance = 1.0 / animationEnd_.zoom;
        state.zoom = 1.0 / (startDistance * (1.0 - progress) +
                            endDistance * progress);
        state.gridViewDistance =
            animationStart_.gridViewDistance * (1.0 - progress) +
            animationEnd_.gridViewDistance * progress;
        state.pan = animationStart_.pan * (1.0 - progress) +
                    animationEnd_.pan * progress;
        state.orbitPivot = {
            animationStart_.orbitPivot.x * (1.0 - progress) +
                animationEnd_.orbitPivot.x * progress,
            animationStart_.orbitPivot.y * (1.0 - progress) +
                animationEnd_.orbitPivot.y * progress,
            animationStart_.orbitPivot.z * (1.0 - progress) +
                animationEnd_.orbitPivot.z * progress};
        state.orientation = ViewportOrientation::slerp(
            animationStart_.orientation, animationEnd_.orientation, progress);
        state.hasOrientation = true;
        state.perspective = animationStart_.perspective;
        state.preset = progress >= 1.0 ? animationEnd_.preset
                                       : ViewportViewPreset::Custom;
        transform_.setCameraState(state);
        requestUpdate();
        notifyCoordinatesChanged();
    });
    QObject::connect(animation_.get(), &QVariantAnimation::finished,
                     animation_.get(), [this]() {
        transform_.setCameraState(animationEnd_);
        notifyViewStateChanged();
        requestUpdate();
        notifyCoordinatesChanged();
    });
}

NavigationController::~NavigationController() = default;

void NavigationController::stopAnimation()
{
    if (animation_) {
        animation_->stop();
    }
}

void NavigationController::animateCameraChange(
    const std::function<void()> &setTarget)
{
    stopAnimation();
    const ViewportCameraState original = transform_.cameraState();
    setTarget();
    const ViewportCameraState target = transform_.cameraState();
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
    if (!poseChanged || !animation_) {
        transform_.setCameraState(target);
        notifyViewStateChanged();
        requestUpdate();
        notifyCoordinatesChanged();
        return;
    }
    animationStart_ = start;
    animationEnd_ = target;
    transform_.setCameraState(start);
    animation_->start();
}

void NavigationController::setPanButton(Qt::MouseButton button)
{
    panButton_ = button;
}

Qt::MouseButton NavigationController::panButton() const
{
    return panButton_;
}

bool NavigationController::handleGizmoPress(const QPointF &screenPosition,
                                             const QSize &viewportSize)
{
    stopAnimation();
    const BlenderNavigationHit hit =
        gizmo_.hitAt(screenPosition, viewportSize);
    if (hit.action == BlenderNavigationAction::None) {
        return false;
    }

    pressedAction_ = hit.action;
    pressHit_ = hit;
    pressPosition_ = screenPosition;
    lastPosition_ = screenPosition;
    navigationMoved_ = false;
    if (hit.action == BlenderNavigationAction::Orbit ||
        hit.action == BlenderNavigationAction::Axis) {
        beginOrbit(screenPosition, viewportSize);
    }
    updateCursorForAction(hit.action);
    requestUpdate();
    return true;
}

bool NavigationController::handleGizmoMove(const QPointF &screenPosition,
                                            Qt::MouseButtons buttons,
                                            const QSize &viewportSize)
{
    hoverPosition_ = screenPosition;
    if (pressedAction_ == BlenderNavigationAction::None ||
        !buttons.testFlag(Qt::LeftButton)) {
        return false;
    }

    const QPointF delta = screenPosition - lastPosition_;
    if (QLineF(pressPosition_, screenPosition).length() >= 3.0) {
        navigationMoved_ = true;
    }
    if (navigationMoved_) {
        host_.setCursor(Qt::ClosedHandCursor);
        const ViewportViewPreset previousPreset = transform_.viewPreset();
        const bool previousPerspective = transform_.isPerspectiveEnabled();
        switch (pressedAction_) {
        case BlenderNavigationAction::Orbit:
        case BlenderNavigationAction::Axis:
            if (transform_.navigationPreferences().orbitMethod ==
                ViewportOrbitMethod::Trackball) {
                transform_.orbitToPosition(screenPosition, viewportSize);
            } else {
                transform_.orbitByPixels(delta);
            }
            break;
        case BlenderNavigationAction::Zoom: {
            const ViewportNavigationPreferences preferences =
                transform_.navigationPreferences();
            qreal zoomDelta = preferences.zoomAxis == ViewportZoomAxis::Vertical
                                  ? delta.y()
                                  : delta.x();
            if (preferences.invertMouseZoom) {
                zoomDelta = -zoomDelta;
            }
            transform_.zoomAt(QPointF(viewportSize.width() * 0.5,
                                      viewportSize.height() * 0.5),
                              std::exp(-zoomDelta * 0.012),
                              viewportSize);
            break;
        }
        case BlenderNavigationAction::Pan:
            transform_.panByPixels(delta, viewportSize);
            break;
        case BlenderNavigationAction::Camera:
        case BlenderNavigationAction::Projection:
        case BlenderNavigationAction::None:
            break;
        }
        notifyCameraModeChanged(previousPreset, previousPerspective);
        requestUpdate();
        notifyCoordinatesChanged();
    }
    lastPosition_ = screenPosition;
    requestUpdate();
    return true;
}

bool NavigationController::handleGizmoRelease(Qt::MouseButton button,
                                               const QPointF &screenPosition,
                                               const QSize &viewportSize)
{
    if (button != Qt::LeftButton ||
        pressedAction_ == BlenderNavigationAction::None) {
        return false;
    }

    const BlenderNavigationAction action = pressedAction_;
    const BlenderNavigationHit hit = pressHit_;
    transform_.endOrbitGesture();
    if (!navigationMoved_) {
        switch (action) {
        case BlenderNavigationAction::Axis: {
            Point3D targetDirection = hit.direction;
            if (directionDot(transform_.viewDirection(), targetDirection) > 0.999) {
                targetDirection.x = -targetDirection.x;
                targetDirection.y = -targetDirection.y;
                targetDirection.z = -targetDirection.z;
            }
            animateCameraChange([this, targetDirection]() {
                transform_.setViewDirection(targetDirection);
            });
            break;
        }
        case BlenderNavigationAction::Projection:
            transform_.setPerspectiveEnabled(!transform_.isPerspectiveEnabled());
            notifyViewStateChanged();
            requestUpdate();
            notifyCoordinatesChanged();
            break;
        case BlenderNavigationAction::Camera:
            QToolTip::showText(
                host_.mapToGlobal(screenPosition.toPoint()),
                QStringLiteral("No active scene camera is available."),
                &host_);
            break;
        case BlenderNavigationAction::Orbit:
        case BlenderNavigationAction::Zoom:
        case BlenderNavigationAction::Pan:
        case BlenderNavigationAction::None:
            break;
        }
    }

    pressedAction_ = BlenderNavigationAction::None;
    pressHit_ = {};
    navigationMoved_ = false;
    if (gizmo_.hitAt(screenPosition, viewportSize).action ==
        BlenderNavigationAction::None) {
        host_.setCursor(idleCursor());
        host_.setToolTip(QString());
    }
    requestUpdate();
    return true;
}

bool NavigationController::handlePanPress(Qt::MouseButton button,
                                           Qt::KeyboardModifiers modifiers,
                                           const QPointF &screenPosition,
                                           const QSize &viewportSize)
{
    const bool middleMouseNavigation = button == Qt::MiddleButton;
    const bool configuredPanButton = button == panButton_;
    const bool altLeftNavigation = button == Qt::LeftButton &&
                                   modifiers.testFlag(Qt::AltModifier);
    if (!middleMouseNavigation && !configuredPanButton && !altLeftNavigation) {
        return false;
    }

    panning_ = true;
    delayedRightPan_ = configuredPanButton && button == Qt::RightButton;
    panMotionApplied_ = false;
    if (delayedRightPan_) {
        panPressTimer_.start();
    } else {
        panPressTimer_.invalidate();
    }
    if (middleMouseNavigation) {
        // Blender-style navigation: MMB orbits; Shift+MMB pans.
        orbiting_ = !modifiers.testFlag(Qt::ShiftModifier);
    } else {
        orbiting_ = configuredPanButton &&
                    modifiers.testFlag(Qt::ShiftModifier);
    }
    if (orbiting_) {
        beginOrbit(screenPosition, viewportSize);
    }
    panMoved_ = false;
    panStartPosition_ = screenPosition.toPoint();
    lastPosition_ = QPointF(screenPosition.toPoint());
    // A right press on the Select tool remains a click while the short
    // right-pan activation window is pending. Keep its normal cursor until
    // pointer motion actually activates the pan.
    host_.setCursor(delayedRightPan_ ? idleCursor() : Qt::ClosedHandCursor);
    return true;
}

bool NavigationController::handlePanMove(const QPointF &screenPosition,
                                          const QSize &viewportSize)
{
    if (!panning_) {
        return false;
    }

    const QPoint current = screenPosition.toPoint();
    const QPoint delta = current - lastPosition_.toPoint();
    const QPoint totalDelta = current - panStartPosition_;
    if (std::hypot(totalDelta.x(), totalDelta.y()) >= panDragThresholdPixels) {
        panMoved_ = true;
    }
    if (delayedRightPan_) {
        const bool activationDelayPending = panPressTimer_.isValid() &&
            panPressTimer_.elapsed() < rightPanActivationDelayMs;
        if (activationDelayPending) {
            // Ignore motion during the short click-recognition window so a
            // click cannot nudge the view; start dragging from the latest
            // pointer position once the delay has elapsed.
            lastPosition_ = current;
            panMotionApplied_ = true;
            return true;
        }
        if (!panMoved_) {
            return true;
        }
        host_.setCursor(Qt::ClosedHandCursor);
        const QPointF appliedDelta = panMotionApplied_
                                         ? QPointF(delta)
                                         : QPointF(current - panStartPosition_);
        panMotionApplied_ = true;
        if (orbiting_) {
            const ViewportViewPreset previousPreset = transform_.viewPreset();
            const bool previousPerspective = transform_.isPerspectiveEnabled();
            if (transform_.navigationPreferences().orbitMethod ==
                ViewportOrbitMethod::Trackball) {
                transform_.orbitToPosition(screenPosition, viewportSize);
            } else {
                transform_.orbitByPixels(appliedDelta);
            }
            notifyCameraModeChanged(previousPreset, previousPerspective);
        } else {
            transform_.panByPixels(appliedDelta, viewportSize);
        }
        lastPosition_ = current;
        requestUpdate();
        notifyCoordinatesChanged();
        return true;
    }
    if (orbiting_) {
        const ViewportViewPreset previousPreset = transform_.viewPreset();
        const bool previousPerspective = transform_.isPerspectiveEnabled();
        if (transform_.navigationPreferences().orbitMethod ==
            ViewportOrbitMethod::Trackball) {
            transform_.orbitToPosition(screenPosition, viewportSize);
        } else {
            transform_.orbitByPixels(QPointF(delta));
        }
        notifyCameraModeChanged(previousPreset, previousPerspective);
    } else {
        transform_.panByPixels(QPointF(delta), viewportSize);
    }
    lastPosition_ = current;
    requestUpdate();
    notifyCoordinatesChanged();
    return true;
}

bool NavigationController::finishPointerRelease(Qt::MouseButton button)
{
    bool ended = false;
    if (panning_ && (button == panButton_ || button == Qt::MiddleButton ||
                     button == Qt::LeftButton)) {
        if (orbiting_) {
            transform_.endOrbitGesture();
        }
        panning_ = false;
        orbiting_ = false;
        delayedRightPan_ = false;
        panMotionApplied_ = false;
        panPressTimer_.invalidate();
        host_.setCursor(idleCursor());
        ended = true;
    }
    panMoved_ = false;
    return ended;
}

bool NavigationController::updateGizmoHover(const QPointF &screenPosition,
                                             const QSize &viewportSize,
                                             bool updateIdleCursor)
{
    hoverPosition_ = screenPosition;
    const BlenderNavigationHit hit =
        gizmo_.hitAt(screenPosition, viewportSize);
    if (hit.action != BlenderNavigationAction::None) {
        host_.setToolTip(tooltipForHit(hit));
        updateCursorForAction(hit.action);
        requestUpdate();
        return true;
    }

    host_.setToolTip(QString());
    if (updateIdleCursor) {
        host_.setCursor(idleCursor());
    }
    return false;
}

void NavigationController::pointerLeave()
{
    hoverPosition_ = QPointF(-1000.0, -1000.0);
    if (pressedAction_ == BlenderNavigationAction::None) {
        host_.setToolTip(QString());
    }
    requestUpdate();
}

WheelZoomResult NavigationController::zoomFromWheel(
    const QPointF &screenPosition,
    int angleDeltaY,
    int pixelDeltaY,
    const QSize &viewportSize)
{
    WheelZoomResult result;
    result.oldZoom = transform_.zoom();
    result.worldBefore = transform_.screenToWorld(screenPosition, viewportSize);
    qreal wheelSteps = viewportWheelStepsFromDeltas(angleDeltaY, pixelDeltaY);
    if (wheelSteps == 0.0) {
        result.worldAfter = result.worldBefore;
        return result;
    }
    if (transform_.navigationPreferences().invertZoomWheel) {
        wheelSteps = -wheelSteps;
    }
    wheelSteps = std::clamp(wheelSteps, -24.0, 24.0);
    // Blender's view_zoom_apply_step uses a 1.2 distance ratio per notch.
    transform_.zoomAt(screenPosition,
                      std::exp(std::log(1.2) * wheelSteps),
                      viewportSize);
    result.worldAfter = transform_.screenToWorld(screenPosition, viewportSize);
    result.changed = true;
    requestUpdate();
    notifyCoordinatesChanged();
    return result;
}

QPointF NavigationController::hoverPosition() const
{
    return hoverPosition_;
}

bool NavigationController::hasPressedGizmo() const
{
    return pressedAction_ != BlenderNavigationAction::None;
}

bool NavigationController::isPanning() const
{
    return panning_;
}

bool NavigationController::panMoved() const
{
    return panMoved_;
}

void NavigationController::requestUpdate() const
{
    if (callbacks_.requestUpdate) {
        callbacks_.requestUpdate();
    }
}

void NavigationController::notifyCoordinatesChanged() const
{
    if (callbacks_.coordinatesChanged) {
        callbacks_.coordinatesChanged();
    }
}

void NavigationController::notifyViewStateChanged() const
{
    if (callbacks_.viewStateChanged) {
        callbacks_.viewStateChanged();
    }
}

Qt::CursorShape NavigationController::idleCursor() const
{
    return callbacks_.idleCursor ? callbacks_.idleCursor() : Qt::ArrowCursor;
}

void NavigationController::beginOrbit(const QPointF &screenPosition,
                                      const QSize &viewportSize)
{
    if (callbacks_.beginOrbitAt) {
        callbacks_.beginOrbitAt(screenPosition);
    }
    if (transform_.navigationPreferences().orbitMethod ==
        ViewportOrbitMethod::Trackball) {
        transform_.beginOrbitGesture(screenPosition, viewportSize);
    }
}

void NavigationController::notifyCameraModeChanged(
    ViewportViewPreset previousPreset,
    bool previousPerspective)
{
    if (previousPreset != transform_.viewPreset() ||
        previousPerspective != transform_.isPerspectiveEnabled()) {
        notifyViewStateChanged();
    }
}

void NavigationController::updateCursorForAction(BlenderNavigationAction action)
{
    host_.setCursor(action == BlenderNavigationAction::Camera ||
                             action == BlenderNavigationAction::Projection
                         ? Qt::PointingHandCursor
                         : Qt::OpenHandCursor);
}

QString NavigationController::tooltipForHit(const BlenderNavigationHit &hit) const
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
        return transform_.isPerspectiveEnabled()
                   ? QStringLiteral("Switch to orthographic projection")
                   : QStringLiteral("Switch to perspective projection");
    case BlenderNavigationAction::None:
        break;
    }
    return {};
}

} // namespace classiCAD
