#pragma once

#include "core/geometry/arc_mode.h"
#include "core/tool_id.h"

#include <QPainter>
#include <QRectF>
#include <QSize>
#include <QString>

namespace classiCAD {

struct ViewportHudState {
    ToolId activeTool = ToolId::Select;
    ArcMode arcMode = ArcMode::TwoPoint;
    bool subdivisionActive = false;
    int subdivisionSections = 0;
    bool joinActive = false;
    int joinCount = 0;
    bool lineCommandActive = false;
    QString lineCommandStatus;
    QString pointToolInstructions;
    bool pointExtrudeWeldEnabled = true;
    int rotateStep = 0;
    bool rotateAngleSnapEnabled = false;
    bool rotateAngleInputActive = false;
    qreal rotateAngleSnapIncrementDegrees = 15.0;
    bool rotateAngleInputInRadians = false;
    bool grabActive = false;
    bool grabPickingBasePoint = false;
    bool grabHasBasePoint = false;
    bool duplicateActive = false;
    bool duplicatePickingBasePoint = false;
    bool duplicateHasBasePoint = false;
};

class ViewportHudRenderer final {
public:
    void draw(QPainter &painter,
              const QSize &viewportSize,
              const ViewportHudState &state) const;

    static QString rotateSnapIncrementLabel(qreal incrementDegrees,
                                            bool useRadians);
    static QRectF pointExtrudeWeldToggleRect(const QSize &viewportSize);
};

} // namespace classiCAD
