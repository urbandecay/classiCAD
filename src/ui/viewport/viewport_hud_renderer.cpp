#include "viewport_hud_renderer.h"

#include <QFont>
#include <QFontMetrics>

#include <algorithm>

namespace classiCAD {

QString ViewportHudRenderer::rotateSnapIncrementLabel(qreal incrementDegrees,
                                                       bool useRadians)
{
    if (useRadians) {
        const int denominator = qRound(180.0 / incrementDegrees);
        return QStringLiteral("π/%1").arg(denominator);
    }
    return QStringLiteral("%1°").arg(incrementDegrees, 0, 'g', 4);
}

QRectF ViewportHudRenderer::pointExtrudeWeldToggleRect(
    const QSize &viewportSize)
{
    return QRectF(14.0,
                  viewportSize.height() - 36.0,
                  88.0,
                  24.0);
}

void ViewportHudRenderer::draw(QPainter &painter,
                               const QSize &viewportSize,
                               const ViewportHudState &state) const
{
    const ToolId activeTool = state.activeTool;
    const ArcMode arcMode = state.arcMode;
    const bool subdivisionActive = state.subdivisionActive;
    const int subdivisionSections = state.subdivisionSections;
    const bool joinActive = state.joinActive;
    const int joinCount = state.joinCount;
    const bool lineCommandActive = state.lineCommandActive;
    const QString &lineCommandStatus = state.lineCommandStatus;
    const QString &pointToolInstructions = state.pointToolInstructions;
    const int rotateStep = state.rotateStep;
    const bool rotateAngleSnapEnabled = state.rotateAngleSnapEnabled;
    const bool rotateAngleInputActive = state.rotateAngleInputActive;
    const qreal rotateAngleSnapIncrementDegrees =
        state.rotateAngleSnapIncrementDegrees;
    const bool rotateAngleInputInRadians = state.rotateAngleInputInRadians;
    const bool grabActive = state.grabActive;
    const bool grabPickingBasePoint = state.grabPickingBasePoint;
    const bool grabHasBasePoint = state.grabHasBasePoint;
    const bool duplicateActive = state.duplicateActive;
    const bool duplicatePickingBasePoint = state.duplicatePickingBasePoint;
    const bool duplicateHasBasePoint = state.duplicateHasBasePoint;

    painter.setPen(QColor(QStringLiteral("#a0a0a0")));
    painter.setFont(QFont(QStringLiteral("Sans"), 10));
    const QString activeToolLabel = activeTool == Tool::Arc
                                        ? arcModeName(arcMode)
                                        : toolName(activeTool);
    painter.drawText(18,
                     28,
                     QStringLiteral("2D VIEWPORT  •  %1").arg(activeToolLabel));

    if (subdivisionActive) {
        painter.setPen(QColor(QStringLiteral("#f0a45a")));
        painter.drawText(18,
                         viewportSize.height() - 42,
                         QStringLiteral("SUBDIVIDE  •  %1 sections  •  endpoints included")
                             .arg(subdivisionSections));
    }

    if (joinActive) {
        painter.setPen(QColor(QStringLiteral("#f0a45a")));
        painter.drawText(18,
                         viewportSize.height() - 42,
                         QStringLiteral("JOIN  •  %1 curves selected  •  Click connected curves to join  •  Esc to cancel")
                             .arg(joinCount));
    }

    if (activeTool == Tool::PointExtrude) {
        painter.setPen(QColor(QStringLiteral("#777777")));
        const qreal baseline = viewportSize.height() - 18.0;
        const QRectF checkBox(19.0, baseline - 12.0, 12.0, 12.0);
        painter.setPen(QPen(state.pointExtrudeWeldEnabled
                                ? QColor(QStringLiteral("#5597df"))
                                : QColor(QStringLiteral("#888888")),
                            1.0));
        painter.setBrush(state.pointExtrudeWeldEnabled
                             ? QColor(QStringLiteral("#315f91"))
                             : QColor(QStringLiteral("#292929")));
        painter.drawRoundedRect(checkBox, 2.0, 2.0);
        if (state.pointExtrudeWeldEnabled) {
            painter.setPen(QPen(QColor(QStringLiteral("#ffffff")), 1.7));
            painter.drawLine(QPointF(22.0, baseline - 6.0),
                             QPointF(24.5, baseline - 3.5));
            painter.drawLine(QPointF(24.5, baseline - 3.5),
                             QPointF(29.0, baseline - 9.0));
        }
        painter.setPen(QColor(QStringLiteral("#b0b0b0")));
        painter.drawText(37.0,
                         baseline,
                         QStringLiteral("Weld (W)"));
        painter.setPen(QColor(QStringLiteral("#777777")));
        painter.drawText(108.0,
                         baseline,
                         QStringLiteral("Click endpoint  •  X/Y/Z locks axis  •  Enter confirms  •  Esc/RMB cancels"));
    } else if (activeTool == Tool::Erase) {
        painter.setPen(QColor(QStringLiteral("#777777")));
        painter.drawText(18,
                         viewportSize.height() - 18,
                         QStringLiteral("Drag over a curve segment  •  Release to erase to intersection/end  •  Esc/RMB exits"));
    } else if (activeTool == Tool::Trim) {
        painter.setPen(QColor(QStringLiteral("#777777")));
        painter.drawText(18,
                         viewportSize.height() - 18,
                         QStringLiteral("Click a selected curve segment to trim to intersection/end  •  Esc/RMB exits"));
    } else if (activeTool == Tool::Rotate) {
        painter.setPen(QColor(QStringLiteral("#777777")));
        const QString rotateHint = rotateStep == 0
                                       ? QStringLiteral("Click pivot  •  X/Y/Z plane  •  P perpendicular  •  Esc/RMB cancel")
                                     : rotateStep == 1
                                             ? QStringLiteral("Click reference  •  C %1 snap %2  •  P perpendicular  •  Esc/RMB cancel")
                                                   .arg(rotateSnapIncrementLabel(
                                                       rotateAngleSnapIncrementDegrees,
                                                       rotateAngleInputInRadians))
                                                   .arg(rotateAngleSnapEnabled
                                                            ? QStringLiteral("on")
                                                            : QStringLiteral("off"))
                                             : QStringLiteral("Click/Enter confirm%3  •  A angle (deg)  •  C %1 snap %2  •  P perpendicular  •  Esc/RMB cancel")
                                                   .arg(rotateSnapIncrementLabel(
                                                       rotateAngleSnapIncrementDegrees,
                                                       rotateAngleInputInRadians))
                                                   .arg(rotateAngleSnapEnabled
                                                            ? QStringLiteral("on")
                                                            : QStringLiteral("off"))
                                                   .arg(rotateAngleInputActive
                                                            ? QStringLiteral(" (typing)")
                                                            : QString());
        painter.drawText(18, viewportSize.height() - 18, rotateHint);
    } else if (activeTool == Tool::Mirror) {
        painter.setPen(QColor(QStringLiteral("#777777")));
        painter.drawText(18,
                         viewportSize.height() - 18,
                         QStringLiteral("Click the first and second points of the mirror axis  •  Esc/RMB cancels"));
    } else if (activeTool == Tool::PointByLine ||
               activeTool == Tool::PointByArcs ||
               activeTool == Tool::PointCenter ||
               activeTool == Tool::PointEdgeCenter) {
        painter.save();
        const QRectF panel(12.0,
                           std::max<qreal>(12.0, viewportSize.height() - 58.0),
                           std::max<qreal>(1.0,
                                           std::min<qreal>(750.0,
                                                           viewportSize.width() - 24.0)),
                           46.0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(20, 20, 20, 170));
        painter.drawRoundedRect(panel, 4.0, 4.0);
        painter.setFont(QFont(QStringLiteral("Sans"), 9));
        const QFontMetrics metrics(painter.font());
        const int textWidth = std::max(1, static_cast<int>(panel.width() - 16.0));
        painter.setPen(QColor(225, 225, 225));
        const QString title = lineCommandStatus.isEmpty()
                                  ? toolName(activeTool)
                                  : lineCommandStatus;
        painter.drawText(QPointF(20.0, panel.top() + 17.0),
                         metrics.elidedText(title, Qt::ElideRight, textWidth));
        painter.setPen(QColor(170, 170, 170));
        QString instructions = pointToolInstructions;
        if (instructions.isEmpty() && activeTool == Tool::PointByLine) {
            instructions = QStringLiteral("Click points • Shift locks direction • X/Y/Z axis • type length + Enter • L plane lock • Enter/Space/RMB finishes • Esc cancels");
        } else if (instructions.isEmpty() && activeTool == Tool::PointByArcs) {
            instructions = QStringLiteral("Click center, radius, sweep for each arc • P perpendicular • C angle snap • R radius • A angle • Esc cancels");
        } else if (instructions.isEmpty() && activeTool == Tool::PointCenter) {
            instructions = QStringLiteral("Click to place the fitted center • Enter confirms • Esc cancels");
        } else if (instructions.isEmpty()) {
            instructions = QStringLiteral("Hover an enabled midpoint snap • Click places • Esc cancels");
        }
        painter.drawText(QPointF(20.0, panel.top() + 35.0),
                         metrics.elidedText(instructions, Qt::ElideRight,
                                            textWidth));
        painter.restore();
    } else if (activeTool == Tool::Line && lineCommandActive) {
        painter.save();
        const QRectF panel(12.0,
                           std::max<qreal>(12.0, viewportSize.height() - 58.0),
                           std::max<qreal>(1.0,
                                           std::min<qreal>(750.0,
                                                           viewportSize.width() - 24.0)),
                           46.0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(20, 20, 20, 170));
        painter.drawRoundedRect(panel, 4.0, 4.0);
        painter.setFont(QFont(QStringLiteral("Sans"), 9));
        const QFontMetrics metrics(painter.font());
        painter.setPen(QColor(225, 225, 225));
        painter.drawText(QPointF(20.0, panel.top() + 17.0),
                         lineCommandStatus.isEmpty()
                             ? QStringLiteral("Line")
                             : lineCommandStatus);
        painter.setPen(QColor(170, 170, 170));
        painter.drawText(
            QPointF(20.0, panel.top() + 35.0),
            metrics.elidedText(
                QStringLiteral("Click point  •  Length + Enter  •  X/Y/Z  •  Shift direction  •  N normal  •  L plane  •  Space/RMB finish  •  Esc exits"),
                Qt::ElideRight,
                std::max(1, static_cast<int>(panel.width() - 96.0))));
        painter.restore();
    } else if (activeTool == Tool::Picture) {
        painter.setPen(QColor(QStringLiteral("#777777")));
        painter.drawText(18,
                         viewportSize.height() - 18,
                         QStringLiteral("Click the first corner, then the opposite corner  •  Aspect ratio is preserved  •  Esc cancels"));
    } else if (activeTool == Tool::TangentFromCurve) {
        painter.setPen(QColor(QStringLiteral("#777777")));
        painter.drawText(18,
                         viewportSize.height() - 18,
                         QStringLiteral("Click a curve, then click the line endpoint  •  Esc cancels"));
    } else if (activeTool == Tool::PerpendicularFromCurve) {
        painter.setPen(QColor(QStringLiteral("#777777")));
        painter.drawText(
            18,
            viewportSize.height() - 18,
            QStringLiteral("Click a curve, then click the perpendicular line endpoint  •  Esc cancels"));
    } else if (duplicateActive) {
        painter.setPen(QColor(QStringLiteral("#f0a45a")));
        const QString duplicateHint = duplicatePickingBasePoint
                                          ? QStringLiteral("DUPLICATE  •  Click a base point on the selection  •  Esc/RMB cancels")
                                          : duplicateHasBasePoint
                                                ? QStringLiteral("DUPLICATE  •  Move preview to destination  •  Click to place  •  Esc cancels")
                                                : QStringLiteral("DUPLICATE  •  Click a base point on the selection  •  Esc/RMB cancels");
        painter.drawText(18, viewportSize.height() - 18, duplicateHint);
    } else if (activeTool != Tool::Select && activeTool != Tool::Arc &&
               !isRectangleTool(activeTool) &&
               !isPolygonTool(activeTool) &&
               !isCircleConstructionTool(activeTool) &&
               !isEllipseTool(activeTool)) {
        QString hint;
        if (isRectangleTool(activeTool)) {
            QString inputDescription;
            switch (rectangleModeForTool(activeTool)) {
            case RectangleMode::CornerCorner:
                inputDescription = QStringLiteral("first corner, opposite corner");
                break;
            case RectangleMode::CenterCorner:
                inputDescription = QStringLiteral("center, corner");
                break;
            case RectangleMode::ThreePoint:
                inputDescription = QStringLiteral("first edge point, second edge point, width point");
                break;
            }
            hint = QStringLiteral("Click to place %1  •  Esc clears current tool input")
                       .arg(inputDescription);
        } else if (activeTool == Tool::Arc) {
            const QString points = arcMode == ArcMode::OnePoint
                                        ? QStringLiteral("center, start, endpoint")
                                        : QStringLiteral("start, end, through point");
            hint = QStringLiteral("Click to place %1  •  Esc clears current tool input")
                       .arg(points);
        } else {
            hint = QStringLiteral("Click to place %1 point%2  •  Esc clears current tool input")
                       .arg(toolName(activeTool).toLower())
                       .arg(requiredPoints(activeTool) == 1 ? QString()
                                                            : QStringLiteral("s"));
        }
        painter.setPen(QColor(QStringLiteral("#777777")));
        painter.drawText(18, viewportSize.height() - 18, hint);
    } else if (grabActive) {
        painter.setPen(QColor(QStringLiteral("#f0a45a")));
        const QString grabHint = grabPickingBasePoint
                                     ? QStringLiteral("Click an OSnap point on the selection for the move base  •  Esc/RMB cancels")
                                     : grabHasBasePoint
                                           ? QStringLiteral("Move base point to destination  •  X/Y: constrain  •  Click: confirm  •  Esc: cancel")
                                           : QStringLiteral("Move selection  •  X/Y: constrain  •  B: choose base point  •  Click: confirm  •  Esc: cancel");
        painter.drawText(18, viewportSize.height() - 18, grabHint);
    }
}

} // namespace classiCAD
